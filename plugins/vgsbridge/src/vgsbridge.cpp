// vgsbridge - a capture player for hosts that pull frames. vgsbridge.h says what and why.

#include "vgsbridge.h"
#include "framepack.h"
#include "packedchunk.h"

#include "vgsdecoder/vgsdecoder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

thread_local std::string lastError;

/**
 * How close two times must be to name the same frame. The host computes a frame's time
 * the same way when it schedules it and when it asks for it, so they normally match
 * exactly; this only absorbs rounding, and is far below any frame interval.
 */
constexpr double SameTime = 1e-6;

/**
 * The capture, mapped whole. A chunk is tens of megabytes and the decoder reads each one
 * entire, so handing it mapped bytes saves a copy of every chunk; and opening the path
 * here rather than through the decoder is what lets a UTF-8 path with accents work on
 * Windows.
 */
class MappedFile {
public:
  MappedFile() = default;
  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;
  ~MappedFile() { close(); }

  bool open(const char *path, std::string &error) {
#ifdef _WIN32
    const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (wideLength <= 0) {
      error = "the path is not valid UTF-8";
      return false;
    }
    std::wstring wide(size_t(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, &wide[0], wideLength);
    file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      error = std::string("cannot open ") + path;
      return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
      error = std::string("cannot read ") + path;
      return false;
    }
    mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping)
      bytes = static_cast<const uint8_t *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!bytes) {
      error = std::string("cannot map ") + path;
      return false;
    }
    length = size_t(size.QuadPart);
#else
    descriptor = ::open(path, O_RDONLY);
    if (descriptor < 0) {
      error = std::string("cannot open ") + path;
      return false;
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || status.st_size <= 0) {
      error = std::string("cannot read ") + path;
      return false;
    }
    void *view = mmap(nullptr, size_t(status.st_size), PROT_READ, MAP_PRIVATE, descriptor, 0);
    if (view == MAP_FAILED) {
      error = std::string("cannot map ") + path;
      return false;
    }
    bytes = static_cast<const uint8_t *>(view);
    length = size_t(status.st_size);
#endif
    return true;
  }

  const uint8_t *data() const { return bytes; }
  size_t size() const { return length; }

private:
  void close() {
#ifdef _WIN32
    if (bytes)
      UnmapViewOfFile(bytes);
    if (mapping)
      CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    mapping = nullptr;
#else
    if (bytes)
      munmap(const_cast<uint8_t *>(bytes), length);
    if (descriptor >= 0)
      ::close(descriptor);
    descriptor = -1;
#endif
    bytes = nullptr;
    length = 0;
  }

#ifdef _WIN32
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
#else
  int descriptor = -1;
#endif
  const uint8_t *bytes = nullptr;
  size_t length = 0;
};

/** One decoded frame, laid out as vgsb_frame describes. */
struct Slot : vgsbdetail::FrameBuffers {
  enum class State { Empty, Filling, Ready };
  State state = State::Empty;
  double seconds = 0;
  /** The player's generation it was decoded for; an older one is as good as empty. */
  uint64_t generation = 0;
  /** Handed to the host and not to be touched until it lets go. */
  bool pinned = false;
  /** When it became Ready, counting up: of the frames nobody wants, the oldest goes first. */
  uint64_t readyOrder = 0;
};

int coefficientsForDegree(uint32_t degree) {
  switch (degree) {
  case 1:
    return 3;
  case 2:
    return 8;
  case 3:
    return 15;
  default:
    return 0;
  }
}

bool sameTime(double a, double b) { return std::fabs(a - b) < SameTime; }

/** The host's parallel-for, from vgsb_set_parallel_for; players take it when they open. */
std::mutex hostGuard;
vgsb_parallel_for hostRun = nullptr;
void *hostData = nullptr;

/**
 * The decoder's parallel-for, run through the host's. The body may throw, and an
 * exception must not unwind through the host's scheduler, so each task catches its own
 * and the first one is rethrown here, on the lane, where the decoder expects it.
 */
vgsdec::ParallelFor throughHost(vgsb_parallel_for run, void *host) {
  return [run, host](size_t count, const std::function<void(size_t)> &body) {
    struct Job {
      const std::function<void(size_t)> *body;
      std::mutex guard;
      std::exception_ptr failure;
    } job{&body, {}, {}};
    run(host, count,
        [](void *context, size_t index) {
          Job &job = *static_cast<Job *>(context);
          try {
            (*job.body)(index);
          } catch (...) {
            std::lock_guard<std::mutex> lock(job.guard);
            if (!job.failure)
              job.failure = std::current_exception();
          }
        },
        &job);
    if (job.failure)
      std::rethrow_exception(job.failure);
  };
}

/** The least density accepted: below it a capture is a scattering of dots. */
constexpr float SmallestDensity = 0.01f;

/**
 * Two lanes, one for the even chunks and one for the odd ones, each a Capture of its own
 * on a thread of its own.
 *
 * A Capture belongs to one thread, and one thread cannot evaluate the frames of the chunk
 * being played and decompress the next chunk at the same time: interleaved, the two add
 * up, and at 30 fps they do not fit. Split by parity, the lane playing chunk k evaluates
 * its frames while the other lane decompresses chunk k + 1 in full, on another core.
 * Forwards or backwards makes no difference, since the neighbour is the other lane either
 * way.
 *
 * Each lane keeps only the chunk it is on, so two chunks are held decoded, three for the
 * moment a lane moves on to its next one.
 */
constexpr size_t LaneCount = 2;

struct Lane {
  std::unique_ptr<vgsdec::Capture> capture;
  std::thread thread;
};

/**
 * A packed chunk as the host sees it. The arrays are the VGS editor's PackedChunk
 * (editor/packedchunk.cpp), the layout its viewport and the web viewer read; this only
 * points the C struct at them and counts who holds it.
 */
struct ChunkHolder : vgsb_packed_chunk {
  std::shared_ptr<const PackedChunk> data;
  mutable std::atomic<int> references{1};
};

std::atomic<uint64_t> nextSerial{1};

ChunkHolder *makeHolder(std::shared_ptr<const PackedChunk> data) {
  auto *h = new ChunkHolder();
  const PackedChunk &c = *data;
  h->serial = nextSerial++;
  h->chunk_index = c.chunkIndex;
  h->splats = c.splats;
  h->samples = c.samples;
  h->intervals = c.intervals;
  h->sh_planes = c.planes;
  h->sh_coefficients = c.coefficients;
  h->group_count = c.groupCount;
  const PackedBasisLayout layout = basisLayout(c);
  h->basis_texels = std::max(1, layout.total);
  h->basis_offset[0] = layout.position;
  h->basis_offset[1] = layout.sh0;
  h->basis_offset[2] = layout.opacity;
  h->basis_offset[3] = layout.rotation;
  h->entries_static = c.entriesStatic;
  h->entries_temporal = c.entriesTemporal;
  h->sh0_stride = c.sh0Stride;
  h->opacity_stride = c.opacityStride;
  static_assert(sizeof(h->group_a) == sizeof(c.groupA) && sizeof(h->group_b) == sizeof(c.groupB),
                "group tables differ");
  std::copy(std::begin(c.groupA), std::end(c.groupA), h->group_a);
  std::copy(std::begin(c.groupB), std::end(c.groupB), h->group_b);
  h->splat_a = c.splatA.data();
  h->splat_b = c.splatB.data();
  h->splat_c = c.splatC.data();
  h->sh_static = c.shStatic.data();
  h->sh_temporal = c.shTemporal.data();
  h->sh_words = c.shStatic.size();
  h->position_terms = c.positionTerms.data();
  h->position_term_words = c.positionTerms.size();
  h->rotation_terms = c.rotationTerms.data();
  h->rotation_term_words = c.rotationTerms.size();
  h->position_samples = c.positionSamples.data();
  h->position_sample_words = c.positionSamples.size();
  h->rotation_samples = c.rotationSamples.data();
  h->rotation_sample_words = c.rotationSamples.size();
  h->books = c.books.data();
  h->book_floats = c.books.size();
  h->luts = c.luts.data();
  h->lut_floats = c.luts.size();
  h->normals = c.normals.empty() ? nullptr : c.normals.data();
  h->normal_words = c.normals.size();
  h->bytes = c.bytes();
  h->data = std::move(data);
  return h;
}

void dropReference(const vgsb_packed_chunk *chunk) {
  const auto *holder = static_cast<const ChunkHolder *>(chunk);
  if (holder->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
    delete holder;
}

/** Distinct chunks a packed player holds at most: the schedule's first few. */
constexpr size_t HeldChunks = 3;

} // namespace

struct vgsb_player {
  // Declared before the lanes so that it is destroyed after them: their captures borrow
  // the mapped bytes.
  MappedFile file;
  Lane lanes[LaneCount];
  size_t laneCount = 0;

  vgsb_info info{};
  std::string title, author, project, take, studio, copyright, id, uuid;
  size_t chunkCount = 0;

  // Everything below is shared between the host's thread and the lanes, under `mutex`.
  // The captures are not: each is touched by its own lane's thread only, once open.
  std::mutex mutex;
  std::condition_variable workChanged, slotChanged;
  std::vector<Slot> slots;
  std::vector<double> wanted;
  bool includeSh = true;
  float density = 1.0f;
  uint64_t generation = 1;
  uint64_t readyCount = 0;
  // The cache each lane should keep, applied by the lane itself on its own thread: a
  // Capture belongs to one thread, so the host cannot set it directly.
  size_t cacheChunks = 0;
  uint64_t cacheBytes = 0;
  uint64_t cacheGeneration = 0;
  std::string failure;
  bool quit = false;

  // Packed playback, vgsb_open_packed: the chunks packed so far, each holding one
  // reference; the one the host acquired last, holding another; and the host's own
  // capture, for instantAt, and basis, which only the host's thread touches.
  bool packed = false;
  unsigned shDegree = 0;
  std::map<size_t, ChunkHolder *> chunks;
  ChunkHolder *acquired = nullptr;
  std::unique_ptr<vgsdec::Capture> clock;
  std::vector<float> basis;
};

namespace {

double clampTime(const vgsb_player &player, double seconds) {
  if (!(seconds > 0))
    return 0;
  return std::min(seconds, player.info.duration);
}

/** A slot holding, or filling, the frame at `seconds` for the current generation. */
int findSlot(const vgsb_player &player, double seconds, bool readyOnly) {
  for (size_t i = 0; i < player.slots.size(); ++i) {
    const Slot &slot = player.slots[i];
    if (slot.state == Slot::State::Empty || slot.generation != player.generation)
      continue;
    if (readyOnly && slot.state != Slot::State::Ready)
      continue;
    if (sameTime(slot.seconds, seconds))
      return int(i);
  }
  return -1;
}

/** How many of the wanted times are decoded ahead into slots; the rest are only prepared. */
size_t slotted(const vgsb_player &player) {
  return std::min(player.wanted.size(), player.slots.size());
}

/**
 * A slot that may be overwritten: empty or stale first, then the frame nobody has asked
 * for that was decoded longest ago. Never the one the host is holding, and never one
 * being filled.
 *
 * Oldest first because the newest unwanted frame is usually one that came out late: the
 * playhead passed it while it was decoding. vgsb_acquire_between can still show it, and
 * that is what keeps a capture the machine cannot keep up with playing at a lower rate
 * instead of throwing away every frame it decodes.
 */
int pickVictim(const vgsb_player &player) {
  const size_t considered = slotted(player);
  int unwanted = -1;
  for (size_t i = 0; i < player.slots.size(); ++i) {
    const Slot &slot = player.slots[i];
    if (slot.pinned || slot.state == Slot::State::Filling)
      continue;
    if (slot.state == Slot::State::Empty || slot.generation != player.generation)
      return int(i);
    bool needed = false;
    for (size_t w = 0; w < considered && !needed; ++w)
      needed = sameTime(slot.seconds, player.wanted[w]);
    if (!needed && (unwanted < 0 || slot.readyOrder < player.slots[size_t(unwanted)].readyOrder))
      unwanted = int(i);
  }
  return unwanted;
}

/**
 * The ready frame nearest `seconds` that lies past `after` on the way to it: `after`
 * itself excluded, `seconds` included, in either direction. -1 if there is none.
 */
int findBetween(const vgsb_player &player, double seconds, double after) {
  const double low = std::min(seconds, after), high = std::max(seconds, after);
  int best = -1;
  double bestDistance = 0;
  for (size_t i = 0; i < player.slots.size(); ++i) {
    const Slot &slot = player.slots[i];
    if (slot.state != Slot::State::Ready || slot.generation != player.generation)
      continue;
    if (slot.seconds < low - SameTime || slot.seconds > high + SameTime || sameTime(slot.seconds, after))
      continue;
    const double distance = std::fabs(slot.seconds - seconds);
    if (best < 0 || distance < bestDistance) {
      best = int(i);
      bestDistance = distance;
    }
  }
  return best;
}

/**
 * Decodes the instant at `seconds` into `slot`, in the layout vgsbridge.h promises.
 * Runs on a lane without the lock: the slot is marked Filling, so nothing else reads
 * or writes it meanwhile.
 */
void decodeInto(vgsdec::Capture &capture, Slot &slot, double seconds, bool includeSh,
                float density) {
  vgsbdetail::packFrame(capture.setTime(seconds, includeSh), slot, includeSh, density);
}

/** Which lane plays the chunk holding `seconds`, asked of that lane's own capture. */
size_t laneFor(const vgsb_player &player, vgsdec::Capture &capture, double seconds) {
  return player.laneCount > 1 ? capture.chunkAt(seconds) % player.laneCount : 0;
}

/**
 * The chunk a lane should decompress while it has no frame to fill: the first of its
 * chunks the schedule reaches, if that one is not decoded yet. Returns chunkCount when
 * there is nothing to do.
 *
 * This is what keeps a chunk boundary smooth. The slots reach a few frames ahead, and
 * decoding a chunk takes longer than those frames last; the schedule reaches further, so
 * the other lane has the next chunk decoded before any slot needs it.
 */
size_t chunkToPrepare(const vgsb_player &player, size_t lane, vgsdec::Capture &capture,
                      vgsdec::Detail detail) {
  for (double seconds : player.wanted) {
    const size_t chunk = capture.chunkAt(seconds);
    if (chunk >= player.chunkCount || chunk % player.laneCount != lane)
      continue;
    return capture.isChunkCached(chunk, detail) ? player.chunkCount : chunk;
  }
  return player.chunkCount;
}

/**
 * A lane: fills slots with the wanted frames of its own chunks, in schedule order;
 * prepares its next chunk when it has none to fill; sleeps otherwise.
 */
void run(vgsb_player &player, size_t lane) {
  vgsdec::Capture &capture = *player.lanes[lane].capture;
  std::unique_lock<std::mutex> lock(player.mutex);
  uint64_t cacheApplied = 0;
  while (!player.quit) {
    if (cacheApplied != player.cacheGeneration) {
      vgsdec::CachePolicy policy;
      policy.behind = player.cacheChunks;
      policy.ahead = player.cacheChunks;
      policy.maxBytes = player.cacheBytes;
      cacheApplied = player.cacheGeneration;
      lock.unlock();
      capture.setCachePolicy(policy);
      lock.lock();
      continue;
    }

    if (!player.failure.empty()) {
      player.workChanged.wait(lock);
      continue;
    }

    // The soonest wanted frame of this lane that no slot holds or is filling.
    const size_t considered = slotted(player);
    bool missing = false;
    double target = 0;
    for (size_t w = 0; w < considered && !missing; ++w) {
      const double seconds = player.wanted[w];
      if (laneFor(player, capture, seconds) == lane && findSlot(player, seconds, false) < 0) {
        missing = true;
        target = seconds;
      }
    }

    if (missing) {
      const int victim = pickVictim(player);
      if (victim >= 0) {
        Slot &slot = player.slots[size_t(victim)];
        slot.state = Slot::State::Filling;
        slot.seconds = target;
        slot.generation = player.generation;
        const bool includeSh = player.includeSh;
        const float density = player.density;
        const uint64_t generation = player.generation;

        lock.unlock();
        std::string error;
        try {
          decodeInto(capture, slot, target, includeSh, density);
        } catch (const std::exception &e) {
          error = e.what();
        }
        lock.lock();

        if (!error.empty()) {
          player.failure = error;
          slot.state = Slot::State::Empty;
        } else {
          slot.state = generation == player.generation ? Slot::State::Ready : Slot::State::Empty;
          slot.readyOrder = ++player.readyCount;
        }
        player.slotChanged.notify_all();
        continue;
      }
    }

    const vgsdec::Detail detail = player.includeSh ? vgsdec::Detail::Full : vgsdec::Detail::Base;
    const size_t chunk = chunkToPrepare(player, lane, capture, detail);
    if (chunk < player.chunkCount) {
      lock.unlock();
      std::string error;
      try {
        // A few milliseconds at a time, so that a new schedule, or a close, is noticed
        // within that long.
        capture.prepare(chunk, 8.0, detail);
      } catch (const std::exception &e) {
        error = e.what();
      }
      lock.lock();
      if (!error.empty()) {
        player.failure = error;
        player.slotChanged.notify_all();
      }
      continue;
    }

    player.workChanged.wait(lock);
  }
}

/** The distinct chunks the schedule reaches first, at most HeldChunks of them. */
std::vector<size_t> chunksWanted(const vgsb_player &player, vgsdec::Capture &capture) {
  std::vector<size_t> out;
  for (double seconds : player.wanted) {
    const size_t chunk = capture.chunkAt(seconds);
    if (chunk >= player.chunkCount || std::find(out.begin(), out.end(), chunk) != out.end())
      continue;
    out.push_back(chunk);
    if (out.size() == HeldChunks)
      break;
  }
  return out;
}

void dropChunks(vgsb_player &player) {
  for (auto &entry : player.chunks)
    dropReference(entry.second);
  player.chunks.clear();
}

/**
 * A packed lane: decompresses the first of its chunks the schedule reaches that is not
 * packed yet, a few milliseconds at a time, and packs it; then lets go of the chunks the
 * schedule has left behind. The same split by parity as run(), so one lane packs the
 * next chunk while the host plays the current one.
 */
void runPacked(vgsb_player &player, size_t lane) {
  vgsdec::Capture &capture = *player.lanes[lane].capture;
  std::unique_lock<std::mutex> lock(player.mutex);
  while (!player.quit) {
    if (!player.failure.empty()) {
      player.workChanged.wait(lock);
      continue;
    }
    size_t target = player.chunkCount;
    for (size_t chunk : chunksWanted(player, capture))
      if (chunk % player.laneCount == lane && !player.chunks.count(chunk)) {
        target = chunk;
        break;
      }
    if (target == player.chunkCount) {
      player.workChanged.wait(lock);
      continue;
    }

    const bool includeSh = player.includeSh;
    const uint64_t generation = player.generation;
    lock.unlock();
    std::string error;
    std::shared_ptr<const PackedChunk> packedChunk;
    try {
      // A few milliseconds at a time, so that a new schedule, or a close, is noticed.
      if (capture.prepare(target, 8.0, includeSh ? vgsdec::Detail::Full : vgsdec::Detail::Base))
        packedChunk = packChunk(capture.chunkData(target), includeSh ? player.shDegree : 0);
    } catch (const std::exception &e) {
      error = e.what();
    }
    lock.lock();
    if (!error.empty()) {
      player.failure = error;
      player.slotChanged.notify_all();
      continue;
    }
    if (!packedChunk || generation != player.generation)
      continue;

    const std::vector<size_t> still = chunksWanted(player, capture);
    for (auto it = player.chunks.begin(); it != player.chunks.end();) {
      if (std::find(still.begin(), still.end(), it->first) == still.end()) {
        dropReference(it->second);
        it = player.chunks.erase(it);
      } else {
        ++it;
      }
    }
    player.chunks[target] = makeHolder(std::move(packedChunk));
    player.slotChanged.notify_all();
  }
}

void releaseAcquired(vgsb_player &player) {
  if (player.acquired)
    dropReference(player.acquired);
  player.acquired = nullptr;
}

/** Whatever the schedule said, the time being waited for comes first. */
void wantFirst(vgsb_player &player, double t) {
  if (player.wanted.empty() || !sameTime(player.wanted.front(), t)) {
    player.wanted.erase(std::remove_if(player.wanted.begin(), player.wanted.end(),
                                       [t](double w) { return sameTime(w, t); }),
                        player.wanted.end());
    player.wanted.insert(player.wanted.begin(), t);
  }
  player.workChanged.notify_all();
}

void unpin(vgsb_player &player) {
  for (Slot &slot : player.slots)
    slot.pinned = false;
}

} // namespace

extern "C" {

int vgsb_api_version(void) { return VGSB_API_VERSION; }

const char *vgsb_last_error(void) { return lastError.c_str(); }

} // extern "C"

namespace {

vgsb_player *openPlayer(const char *path, int includeSh, int slots, int threads, bool packed) {
  if (!path) {
    lastError = "no path";
    return nullptr;
  }
  try {
    auto player = std::make_unique<vgsb_player>();
    std::string error;
    if (!player->file.open(path, error)) {
      lastError = error;
      return nullptr;
    }
    // Every lane authenticates the file for itself; it reads a kilobyte and checks one
    // signature, which is nothing next to decoding a chunk.
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    vgsb_parallel_for parallelFor;
    void *host;
    {
      std::lock_guard<std::mutex> lock(hostGuard);
      parallelFor = hostRun;
      host = hostData;
    }
    for (Lane &lane : player->lanes) {
      lane.capture = std::make_unique<vgsdec::Capture>(
          vgsdec::Capture::openMemory(player->file.data(), player->file.size()));
      lane.capture->setThreadCount(threads > 0 ? unsigned(threads) : std::max(1u, cores / 2));
      if (parallelFor)
        lane.capture->setParallelFor(throughHost(parallelFor, host));
      if (packed)
        lane.capture->setOutput(vgsdec::Output::Packed);
      // The chunk the lane is on and nothing else: the other lane holds the neighbour.
      vgsdec::CachePolicy policy;
      policy.behind = 0;
      policy.ahead = 0;
      lane.capture->setCachePolicy(policy);
    }
    vgsdec::Capture &capture = *player->lanes[0].capture;
    player->laneCount = std::max<size_t>(1, std::min(LaneCount, capture.chunkCount()));

    const vgsdec::Metadata &metadata = capture.metadata();
    player->title = metadata.title;
    player->author = metadata.author;
    player->project = metadata.projectName;
    player->take = metadata.takeName;
    player->studio = metadata.captureStudio;
    player->copyright = metadata.copyright;
    player->id = metadata.id;
    player->uuid = capture.uuidText();
    player->chunkCount = capture.chunkCount();

    vgsb_info &info = player->info;
    info.duration = capture.duration();
    info.frame_rate = capture.frameRate();
    info.start_seconds = capture.startSeconds();
    for (int i = 0; i < 6; ++i)
      info.bounds[i] = capture.bounds()[i];
    info.frame_count = capture.frameCount();
    info.max_splats = capture.maxSplatsPerFrame();
    info.sh_degree = capture.shDegree();
    info.sh_coefficients = coefficientsForDegree(info.sh_degree);
    info.playback_mode = int32_t(capture.playbackMode());
    info.title = player->title.c_str();
    info.author = player->author.c_str();
    info.project = player->project.c_str();
    info.take = player->take.c_str();
    info.studio = player->studio.c_str();
    info.copyright = player->copyright.c_str();
    info.id = player->id.c_str();
    info.uuid = player->uuid.c_str();

    player->slots.resize(size_t(std::clamp(slots, 2, 64)));
    player->includeSh = includeSh != 0;
    player->packed = packed;
    player->shDegree = capture.shDegree();
    if (packed)
      player->clock = std::make_unique<vgsdec::Capture>(
          vgsdec::Capture::openMemory(player->file.data(), player->file.size()));
    for (size_t lane = 0; lane < player->laneCount; ++lane)
      player->lanes[lane].thread = std::thread(packed ? runPacked : run, std::ref(*player), lane);
    return player.release();
  } catch (const std::exception &e) {
    lastError = e.what();
    return nullptr;
  }
}

} // namespace

extern "C" {

vgsb_player *vgsb_open(const char *path, int includeSh, int slots, int threads) {
  return openPlayer(path, includeSh, slots, threads, false);
}

vgsb_player *vgsb_open_packed(const char *path, int includeSh, int threads) {
  return openPlayer(path, includeSh, 2, threads, true);
}

int vgsb_acquire_packed(vgsb_player *player, double seconds, double timeoutMs,
                        vgsb_packed_frame *out) {
  if (!player || !out) {
    lastError = "no player";
    return VGSB_ERROR;
  }
  if (!player->packed) {
    lastError = "the player was opened with vgsb_open, not vgsb_open_packed";
    return VGSB_ERROR;
  }
  const double t = clampTime(*player, seconds);
  vgsdec::Instant instant;
  try {
    instant = player->clock->instantAt(t);
  } catch (const std::exception &e) {
    lastError = e.what();
    return VGSB_ERROR;
  }

  ChunkHolder *holder = nullptr;
  {
    std::unique_lock<std::mutex> lock(player->mutex);
    releaseAcquired(*player);
    wantFirst(*player, t);
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(std::max(0.0, timeoutMs)));
    for (;;) {
      if (!player->failure.empty()) {
        lastError = player->failure;
        return VGSB_ERROR;
      }
      const auto found = player->chunks.find(instant.chunkIndex);
      if (found != player->chunks.end()) {
        holder = found->second;
        break;
      }
      if (timeoutMs < 0)
        player->slotChanged.wait(lock);
      else if (player->slotChanged.wait_until(lock, deadline) == std::cv_status::timeout &&
               !player->chunks.count(instant.chunkIndex))
        return VGSB_TIMEOUT;
    }
    holder->references.fetch_add(1, std::memory_order_relaxed);
    player->acquired = holder;
  }

  // The basis outside the lock: the lanes never touch it, and the chunk is held.
  try {
    const PackedInstant at = packedInstant(*holder->data, instant.sampleA, instant.alpha);
    evaluateBasis(*holder->data, at, &player->basis);
    out->seconds = t;
    out->chunk = holder;
    out->frame = at.frame;
    out->alpha = at.alpha;
    out->normalized = at.normalized;
    out->reserved = 0;
    out->basis = player->basis.data();
  } catch (const std::exception &e) {
    lastError = e.what();
    return VGSB_ERROR;
  }
  return VGSB_OK;
}

void vgsb_retain_chunk(const vgsb_packed_chunk *chunk) {
  if (chunk)
    static_cast<const ChunkHolder *>(chunk)->references.fetch_add(1, std::memory_order_relaxed);
}

void vgsb_release_chunk(const vgsb_packed_chunk *chunk) {
  if (chunk)
    dropReference(chunk);
}

void vgsb_close(vgsb_player *player) {
  if (!player)
    return;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    player->quit = true;
  }
  player->workChanged.notify_all();
  for (Lane &lane : player->lanes)
    if (lane.thread.joinable())
      lane.thread.join();
  dropChunks(*player);
  releaseAcquired(*player);
  delete player;
}

int vgsb_get_info(const vgsb_player *player, vgsb_info *out) {
  if (!player || !out) {
    lastError = "no player";
    return VGSB_ERROR;
  }
  *out = player->info;
  return VGSB_OK;
}

void vgsb_schedule(vgsb_player *player, const double *seconds, int count) {
  if (!player)
    return;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    player->wanted.clear();
    for (int i = 0; i < count && seconds; ++i)
      player->wanted.push_back(clampTime(*player, seconds[i]));
  }
  player->workChanged.notify_all();
}

} // extern "C"

namespace {

int acquire(vgsb_player *player, double seconds, double timeoutMs, const double *after,
            vgsb_frame *out) {
  if (!player || !out) {
    lastError = "no player";
    return VGSB_ERROR;
  }
  if (player->packed) {
    lastError = "the player was opened with vgsb_open_packed: use vgsb_acquire_packed";
    return VGSB_ERROR;
  }
  std::unique_lock<std::mutex> lock(player->mutex);
  unpin(*player);
  const double t = clampTime(*player, seconds);
  wantFirst(*player, t);

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double, std::milli>(std::max(0.0, timeoutMs)));
  int index = -1;
  for (;;) {
    if (!player->failure.empty()) {
      lastError = player->failure;
      return VGSB_ERROR;
    }
    index = findSlot(*player, t, true);
    if (index >= 0)
      break;
    if (timeoutMs < 0) {
      player->slotChanged.wait(lock);
    } else if (player->slotChanged.wait_until(lock, deadline) == std::cv_status::timeout) {
      index = findSlot(*player, t, true);
      if (index < 0 && after)
        index = findBetween(*player, t, clampTime(*player, *after));
      if (index < 0)
        return VGSB_TIMEOUT;
      break;
    }
  }

  Slot &slot = player->slots[size_t(index)];
  slot.pinned = true;
  out->seconds = slot.seconds;
  out->count = slot.count;
  out->sh_coefficients = slot.shCoefficients;
  out->reserved = 0;
  out->positions = slot.positions.data();
  out->rotations = slot.rotations.data();
  out->scales = slot.scales.data();
  out->radiance = slot.radiance.data();
  out->sh = slot.count && slot.shCoefficients ? slot.sh.data() : nullptr;
  out->normals = slot.count && slot.hasNormals ? slot.normals.data() : nullptr;
  return VGSB_OK;
}

} // namespace

extern "C" {

int vgsb_acquire(vgsb_player *player, double seconds, double timeoutMs, vgsb_frame *out) {
  return acquire(player, seconds, timeoutMs, nullptr, out);
}

int vgsb_acquire_between(vgsb_player *player, double seconds, double after, double timeoutMs,
                         vgsb_frame *out) {
  return acquire(player, seconds, timeoutMs, &after, out);
}

void vgsb_release(vgsb_player *player) {
  if (!player)
    return;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    unpin(*player);
    releaseAcquired(*player);
  }
  player->workChanged.notify_all();
}

void vgsb_set_include_sh(vgsb_player *player, int includeSh) {
  if (!player)
    return;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    if ((includeSh != 0) == player->includeSh)
      return;
    player->includeSh = includeSh != 0;
    ++player->generation;
    dropChunks(*player);
  }
  player->workChanged.notify_all();
}

void vgsb_set_cache(vgsb_player *player, int chunks, uint64_t maxBytes) {
  if (!player)
    return;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    const size_t wanted = size_t(std::max(chunks, 0));
    if (wanted == player->cacheChunks && maxBytes == player->cacheBytes)
      return;
    player->cacheChunks = wanted;
    player->cacheBytes = maxBytes;
    ++player->cacheGeneration;
  }
  player->workChanged.notify_all();
}

void vgsb_set_parallel_for(vgsb_parallel_for run, void *host) {
  std::lock_guard<std::mutex> lock(hostGuard);
  hostRun = run;
  hostData = run ? host : nullptr;
}

void vgsb_set_density(vgsb_player *player, float density) {
  if (!player)
    return;
  if (!(density >= SmallestDensity))
    density = SmallestDensity;
  density = std::min(density, 1.0f);
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    if (density == player->density)
      return;
    player->density = density;
    ++player->generation;
  }
  player->workChanged.notify_all();
}

} // extern "C"
