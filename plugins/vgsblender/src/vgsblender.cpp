// vgsblender - a capture player for hosts that pull frames. vgsblender.h says what and why.

#include "vgsblender.h"

#include "vgsdecoder/vgsdecoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
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

/** The constant that turns a spherical harmonic DC term into a colour, and back. */
constexpr float C0 = 0.28209479177387814f;

/**
 * How close two times must be to name the same frame. The host computes a frame's time
 * the same way when it schedules it and when it asks for it, so they normally match
 * exactly; this only absorbs rounding, and is far below any frame interval.
 */
constexpr double SameTime = 1e-6;

/**
 * Blender stores log(scale) for drawing and treats an exact zero as log(0), packing -1e6
 * into the range every other splat is quantised against. Nothing drawn is this small.
 */
constexpr float SmallestScale = 1e-6f;

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
struct Slot {
  enum class State { Empty, Filling, Ready };
  State state = State::Empty;
  double seconds = 0;
  /** The player's generation it was decoded for; an older one is as good as empty. */
  uint64_t generation = 0;
  /** Handed to the host and not to be touched until it lets go. */
  bool pinned = false;
  uint64_t count = 0;
  int shCoefficients = 0;
  std::vector<float> positions, rotations, scales, radiance, sh;
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

/** The least density accepted: below it a capture is a scattering of dots. */
constexpr float SmallestDensity = 0.01f;

/**
 * Whether a record stays at a given density. A hash of the index rather than every n-th
 * record, because records are stored in an order that means something - by term count,
 * by group - and taking every n-th would thin some parts of the capture more than others.
 */
bool keeps(size_t record, uint64_t threshold) {
  uint32_t x = uint32_t(record) * 0x9E3779B1u;
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  x *= 0xC2B2AE35u;
  x ^= x >> 16;
  return x < threshold;
}

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
  std::string failure;
  bool quit = false;
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
 * A slot that may be overwritten: empty, stale, or holding a frame nobody has asked for.
 * Never the one the host is holding, and never one being filled.
 */
int pickVictim(const vgsb_player &player) {
  const size_t considered = slotted(player);
  for (size_t i = 0; i < player.slots.size(); ++i) {
    const Slot &slot = player.slots[i];
    if (slot.pinned || slot.state == Slot::State::Filling)
      continue;
    if (slot.state == Slot::State::Empty || slot.generation != player.generation)
      return int(i);
    bool needed = false;
    for (size_t w = 0; w < considered && !needed; ++w)
      needed = sameTime(slot.seconds, player.wanted[w]);
    if (!needed)
      return int(i);
  }
  return -1;
}

/**
 * Decodes the instant at `seconds` into `slot`, in the layout vgsblender.h promises.
 * Runs on a lane without the lock: the slot is marked Filling, so nothing else reads
 * or writes it meanwhile.
 */
void decodeInto(vgsdec::Capture &capture, Slot &slot, double seconds, bool includeSh,
                float density) {
  const vgsdec::Frame &frame = capture.setTime(seconds, includeSh);
  const size_t total = size_t(frame.splatCount);

  // Thinning is one more test in the loop that already drops dead records, and everything
  // after it - the copy into the host, its packing, the upload, the sort, the drawing -
  // then has that much less to do.
  const bool thinned = density < 1.0f;
  const uint64_t threshold = uint64_t(double(density) * 4294967296.0);
  const float exponent = thinned ? 1.0f / density : 1.0f;
  const auto wanted = [&](size_t i) {
    return (!frame.active || frame.active[i]) && (!thinned || keeps(i, threshold));
  };

  size_t live = 0;
  for (size_t i = 0; i < total; ++i)
    live += wanted(i) ? 1 : 0;

  const int coefficients =
      (includeSh && frame.sphericalHarmonics) ? frame.shCoefficients : 0;
  slot.positions.resize(live * 3);
  slot.rotations.resize(live * 4);
  slot.scales.resize(live * 3);
  slot.radiance.resize(live * 4);
  slot.sh.resize(live * 3 * size_t(coefficients));

  size_t j = 0;
  for (size_t i = 0; i < total; ++i) {
    if (!wanted(i))
      continue;

    for (int c = 0; c < 3; ++c)
      slot.positions[j * 3 + c] = frame.positions[i * 3 + c];

    // xyzw in, wxyz out, normalised the way Blender's own importer does.
    const float *q = frame.rotations + i * 4;
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    float *r = &slot.rotations[j * 4];
    if (length > 0) {
      r[0] = q[3] / length;
      r[1] = q[0] / length;
      r[2] = q[1] / length;
      r[3] = q[2] / length;
    } else {
      r[0] = 1;
      r[1] = r[2] = r[3] = 0;
    }

    for (int c = 0; c < 3; ++c)
      slot.scales[j * 3 + c] = std::max(frame.scales[i * 3 + c], SmallestScale);

    for (int c = 0; c < 3; ++c)
      slot.radiance[j * 4 + c] = (frame.colors[i * 3 + c] - 0.5f) / C0;
    // What a thinned-out neighbour would have covered, the ones that stay cover instead.
    const float opacity = frame.opacities[i];
    slot.radiance[j * 4 + 3] =
        thinned ? 1.0f - std::pow(std::max(0.0f, 1.0f - opacity), exponent) : opacity;

    // Per splat per coefficient in, one plane per coefficient out.
    for (int k = 0; k < coefficients; ++k)
      for (int c = 0; c < 3; ++c)
        slot.sh[(size_t(k) * live + j) * 3 + size_t(c)] =
            frame.sphericalHarmonics[(i * size_t(coefficients) + size_t(k)) * 3 + size_t(c)];
    ++j;
  }

  slot.count = live;
  slot.shCoefficients = coefficients;
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
  while (!player.quit) {
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

void unpin(vgsb_player &player) {
  for (Slot &slot : player.slots)
    slot.pinned = false;
}

} // namespace

extern "C" {

int vgsb_api_version(void) { return VGSB_API_VERSION; }

const char *vgsb_last_error(void) { return lastError.c_str(); }

vgsb_player *vgsb_open(const char *path, int includeSh, int slots, int threads) {
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
    for (Lane &lane : player->lanes) {
      lane.capture = std::make_unique<vgsdec::Capture>(
          vgsdec::Capture::openMemory(player->file.data(), player->file.size()));
      lane.capture->setThreadCount(threads > 0 ? unsigned(threads) : std::max(1u, cores / 2));
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
    for (size_t lane = 0; lane < player->laneCount; ++lane)
      player->lanes[lane].thread = std::thread(run, std::ref(*player), lane);
    return player.release();
  } catch (const std::exception &e) {
    lastError = e.what();
    return nullptr;
  }
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

int vgsb_acquire(vgsb_player *player, double seconds, double timeoutMs, vgsb_frame *out) {
  if (!player || !out) {
    lastError = "no player";
    return VGSB_ERROR;
  }
  std::unique_lock<std::mutex> lock(player->mutex);
  unpin(*player);

  // Whatever the schedule said, the frame being waited for comes first.
  const double t = clampTime(*player, seconds);
  if (player->wanted.empty() || !sameTime(player->wanted.front(), t)) {
    player->wanted.erase(std::remove_if(player->wanted.begin(), player->wanted.end(),
                                        [t](double w) { return sameTime(w, t); }),
                         player->wanted.end());
    player->wanted.insert(player->wanted.begin(), t);
  }
  player->workChanged.notify_all();

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
  out->sh = slot.sh.empty() ? nullptr : slot.sh.data();
  return VGSB_OK;
}

void vgsb_release(vgsb_player *player) {
  if (!player)
    return;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    unpin(*player);
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
  }
  player->workChanged.notify_all();
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
