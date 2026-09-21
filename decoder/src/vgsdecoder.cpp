#include "vgsdecoder/vgsdecoder.h"

#include "vgscodec.h"
#include "vgsframe.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <chrono>
#include <exception>
#include <fstream>
#include <mutex>
#include <functional>
#include <thread>

namespace vgsdec {

const char *const InvalidCapture = vgs::InvalidCapture;

namespace {

// A capture opened from memory borrows the caller's bytes; one opened from a file or a
// source goes back to it for anything past the structural region. Both end up answering
// the same question: give me these bytes.
struct Span {
  const uint8_t *data = nullptr;
  size_t size = 0;
};

std::vector<uint8_t> fetch(Source &source, uint64_t offset, uint64_t size) {
  if (size > (uint64_t(1) << 32))
    throw Error("VGS read too large");
  const size_t count = static_cast<size_t>(size);
  std::vector<uint8_t> out(count, uint8_t(0));
  if (count && !source.read(offset, count, out.data()))
    throw Error("VGS read failed");
  return out;
}

/** A Source over a file on disk, so openFile is openStream with the plumbing supplied. */
class FileSource : public Source {
public:
  explicit FileSource(const std::string &path)
      : file(path, std::ios::binary) {
    if (!file)
      throw Error("cannot open " + path);
    file.seekg(0, std::ios::end);
    length = uint64_t(file.tellg());
  }

  bool read(uint64_t offset, size_t size, uint8_t *into) override {
    if (offset > length || size > length - offset)
      return false;
    file.clear();
    file.seekg(std::streamoff(offset), std::ios::beg);
    file.read(reinterpret_cast<char *>(into), std::streamsize(size));
    return file.gcount() == std::streamsize(size);
  }

  uint64_t size() const override { return length; }

private:
  mutable std::ifstream file;
  uint64_t length = 0;
};

Metadata convert(const vgs::Metadata &m) {
  Metadata out;
  out.id = m.id;
  out.title = m.title;
  out.author = m.author;
  out.projectName = m.projectName;
  out.takeName = m.takeName;
  out.captureStudio = m.captureStudio;
  out.copyright = m.copyright;
  out.softwareName = m.softwareName;
  out.softwareVersion = m.softwareVersion;
  out.tags = m.tags;
  return out;
}

// What playback keeps by default, which is not the same question on every target.
//
// One chunk either side, everywhere. That is three chunks - around 66 MB on a capture
// whose chunks are a second long - and it buys a second of stepping backwards for nothing,
// which is already more than the player this replaces had: that one kept a single decoded
// chunk and was smooth, because what makes playback smooth is having the bytes downloaded,
// not having them decoded twice over.
//
// Keeping more only pays when something decodes ahead of playback rather than on demand.
// Nothing does yet; when a player starts decoding into idle frames, raise `ahead` and let
// maxBytes be what actually limits it.
//
// The native build has no ceiling, because whoever embeds it decides its own budget. The
// WebAssembly build has one, because it does not get to decide what machine it runs on;
// it only binds on captures whose chunks are much larger than usual, which is when it
// should.
//
// All three are build options as well (VGS_CACHE_BEHIND, VGS_CACHE_AHEAD,
// VGS_CACHE_MAX_BYTES), and any of it can be overridden at runtime with setCachePolicy.
#ifndef VGS_CACHE_BEHIND
#define VGS_CACHE_BEHIND 1
#endif
#ifndef VGS_CACHE_AHEAD
#define VGS_CACHE_AHEAD 1
#endif
#ifndef VGS_CACHE_MAX_BYTES
#ifdef __EMSCRIPTEN__
#define VGS_CACHE_MAX_BYTES (192ull * 1024 * 1024)
#else
#define VGS_CACHE_MAX_BYTES 0ull
#endif
#endif

CachePolicy defaultCachePolicy() {
  CachePolicy policy;
  policy.behind = VGS_CACHE_BEHIND;
  policy.ahead = VGS_CACHE_AHEAD;
  policy.maxBytes = VGS_CACHE_MAX_BYTES;
  return policy;
}

// Reading only the base layer is most of the saving when a caller does not want colour
// detail, so the two cases are the two masks the container defines.
constexpr uint32_t AllLayers = 7;
constexpr uint32_t BaseLayer = 1;

} // namespace

struct Capture::State {
  vgs::Header header;
  Metadata metadata;
  Signature signature;
  std::vector<ChunkInfo> chunks;
  std::array<uint8_t, 16> uuid{};

  // Exactly one of whole and source is used. structure holds the authenticated prefix
  // when streaming; owned keeps a source this library made for itself.
  Span whole;
  Source *source = nullptr;
  std::unique_ptr<Source> owned;
  std::vector<uint8_t> structure;

  // What playback holds on to between calls: decoded chunks and the evaluator over each,
  // so playing forward costs a chunk decode per chunk rather than per frame, and stepping
  // back into one already decoded costs nothing.
  struct Cached {
    size_t index = 0;
    uint32_t mask = 0;
    uint64_t bytes = 0;
    // Floats mode keeps the evaluator and drops the pages it was built from; Packed mode
    // keeps the pages and never builds an evaluator. Keeping both would double what a
    // chunk costs to hold, for a caller that asked for one of them.
    std::unique_ptr<vgs::FrameDecoder> evaluator;
    vgs::DecodedChunk packed;
    std::vector<Buffer> buffers;
    std::vector<GroupData> groups;
    ChunkData data;
  };
  std::vector<Cached> cache;
  CachePolicy policy = defaultCachePolicy();
  Output output = Output::Floats;
  unsigned threads = 1;
  ParallelFor parallelFor;

  /**
   * Runs `count` pieces of work and returns when all are done: through the host's task
   * system if it gave us one, on threads of our own if not, and on this one for a single
   * item so that a caller who never asked for threads never gets any.
   *
   * An exception from a worker cannot cross a thread boundary on its own, so the first
   * one is kept and rethrown here, where the caller can catch it.
   */
  void runParallel(size_t count, const std::function<void(size_t)> &body) {
    if (count == 0)
      return;
    if (count == 1 || (threads <= 1 && !parallelFor)) {
      for (size_t i = 0; i < count; ++i)
        body(i);
      return;
    }
    if (parallelFor) {
      parallelFor(count, body);
      return;
    }

    std::exception_ptr failure;
    std::mutex guard;
    const auto guarded = [&](size_t i) {
      try {
        body(i);
      } catch (...) {
        std::lock_guard<std::mutex> lock(guard);
        if (!failure)
          failure = std::current_exception();
      }
    };

    // One thread per item, minus the one this call is already on. A batch is a handful
    // of pages and tens of milliseconds of work, so making them costs a fraction of a
    // percent; a pool would save that and cost a great deal more to get right.
    std::vector<std::thread> workers;
    workers.reserve(count - 1);
    for (size_t i = 1; i < count; ++i)
      workers.emplace_back(guarded, i);
    guarded(0);
    for (std::thread &worker : workers)
      worker.join();

    if (failure)
      std::rethrow_exception(failure);
  }

  /**
   * Describes a decoded chunk without copying any of it: the buffers point into the pages
   * the chunk already holds. Uploading is the caller's business, and it can do it straight
   * from these.
   */
  void describePacked(Cached &entry) {
    entry.groups.clear();
    entry.buffers.clear();
    entry.groups.reserve(entry.packed.groups.size());
    entry.buffers.reserve(entry.packed.pages.size());

    uint64_t samples = 0, total = 0;
    for (const vgs::Group &group : entry.packed.groups) {
      GroupData out;
      out.type = group.type;
      out.flags = group.flags;
      out.splats = group.splats;
      out.intervals = group.intervals;
      for (size_t k = 0; k < 6; ++k)
        out.counts[k] = group.counts[k];
      out.positionMin = group.positionMin;
      out.positionMax = group.positionMax;
      out.trajectoryMin = group.trajectoryMin;
      out.trajectoryMax = group.trajectoryMax;
      samples = std::max(samples, group.intervals + 1);
      entry.groups.push_back(out);
    }

    for (const vgs::DecodedPage &page : entry.packed.pages) {
      Buffer buffer;
      buffer.attribute = page.descriptor.attribute;
      buffer.group = page.descriptor.group;
      buffer.layer = page.descriptor.layer;
      buffer.firstRow = page.descriptor.firstRow;
      buffer.rows = page.descriptor.spec.rows;
      buffer.totalRows = page.descriptor.totalRows;
      buffer.kind = page.descriptor.spec.kind;
      buffer.width = page.descriptor.spec.width;
      buffer.data = page.bytes.data();
      buffer.size = page.bytes.size();
      total += page.bytes.size();
      entry.buffers.push_back(buffer);
    }

    entry.data.chunkIndex = entry.index;
    entry.data.sampleCount = samples;
    entry.data.groups = entry.groups.empty() ? nullptr : entry.groups.data();
    entry.data.groupCount = entry.groups.size();
    entry.data.buffers = entry.buffers.empty() ? nullptr : entry.buffers.data();
    entry.data.bufferCount = entry.buffers.size();
    entry.data.totalBytes = total;
  }

  /**
   * A chunk being decoded across several calls. The bytes are copied in on the first step
   * rather than read again on each one: a streaming caller is free to move its window
   * elsewhere while this is in progress, and in a browser the range it primed is the only
   * one there is.
   */
  struct Pending {
    bool active = false;
    size_t index = 0;
    uint32_t mask = 0;
    std::vector<uint8_t> bytes;
    vgs::ChunkDirectory directory;
    vgs::DecodedChunk decoded;
    size_t nextPage = 0;
    size_t totalPages = 0;
  };
  Pending pending;
  vgs::FrameDecoder *evaluator = nullptr; // the entry setTime last used
  vgs::Frame decoded;
  Frame view;
  std::vector<float> positions; // positionsAt only
  double requestedTime = 0, normalizedTime = 0;

  /** The cached entry for a chunk that already holds at least the layers asked for. */
  Cached *find(size_t index, uint32_t mask) {
    for (Cached &entry : cache)
      if (entry.index == index && (entry.mask & mask) == mask)
        return &entry;
    return nullptr;
  }

  uint64_t cachedBytes() const {
    uint64_t total = 0;
    for (const Cached &entry : cache)
      total += entry.bytes;
    return total;
  }

  /**
   * Makes room around `current`.
   *
   * The two limits are the shape of the window, but an allowance the timeline cannot
   * supply is not wasted: near the end there are no chunks ahead to keep, so that share
   * is lent to the side that does have something, and the same the other way round at the
   * start. What is lent is the allowance the timeline cannot fill, not the one a player
   * has not got round to decoding yet - otherwise a window asking for ten ahead would
   * quietly keep ten extra behind for the whole of a capture.
   *
   * Then the byte ceiling, if there is one, takes whatever is furthest away until it fits.
   * The chunk being played is never evicted, whatever either rule says.
   */
  void evict(size_t current) {
    const size_t count = chunks.size();
    const size_t availableAhead = count > current + 1 ? count - current - 1 : 0;
    const size_t availableBehind = current;
    const size_t lentToBehind =
        policy.ahead > availableAhead ? policy.ahead - availableAhead : 0;
    const size_t lentToAhead =
        policy.behind > availableBehind ? policy.behind - availableBehind : 0;
    const size_t allowedBehind = policy.behind + lentToBehind;
    const size_t allowedAhead = policy.ahead + lentToAhead;

    for (size_t i = cache.size(); i-- > 0;) {
      const size_t index = cache[i].index;
      const bool tooFar = index < current ? current - index > allowedBehind
                                          : index - current > allowedAhead;
      if (tooFar && index != current)
        cache.erase(cache.begin() + static_cast<std::ptrdiff_t>(i));
    }

    while (policy.maxBytes && cache.size() > 1 && cachedBytes() > policy.maxBytes) {
      size_t victim = cache.size();
      long long worst = -1;
      for (size_t i = 0; i < cache.size(); ++i) {
        if (cache[i].index == current)
          continue;
        const bool behind = cache[i].index < current;
        const long long distance =
            behind ? static_cast<long long>(current - cache[i].index)
                   : static_cast<long long>(cache[i].index - current);
        // Two chunks equally far apart: the one behind loses, so the half a player is
        // heading into survives a tie.
        const long long score = distance * 2 + (behind ? 1 : 0);
        if (score > worst) {
          worst = score;
          victim = i;
        }
      }
      if (victim >= cache.size())
        break;
      cache.erase(cache.begin() + static_cast<std::ptrdiff_t>(victim));
    }
    evaluator = nullptr; // the vector moved; setTime re-finds its entry
  }

  /**
   * Puts a decoded chunk in the cache, replacing any thinner copy of the same one, and
   * turns it into whatever the current mode delivers: an evaluator, or a description of
   * the buffers to upload.
   */
  void store(size_t index, uint32_t mask, vgs::DecodedChunk decoded) {
    for (size_t i = 0; i < cache.size(); ++i)
      if (cache[i].index == index) {
        cache.erase(cache.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }

    uint64_t held = 0;
    for (const auto &page : decoded.pages)
      held += page.bytes.size();

    Cached entry;
    entry.index = index;
    entry.mask = mask;
    entry.bytes = held;
    if (output == Output::Floats) {
      entry.evaluator.reset(new vgs::FrameDecoder(decoded, secondsPerTick()));
    } else {
      entry.packed = std::move(decoded);
    }
    cache.push_back(std::move(entry));
    // Described after the move, so the buffers point at the pages where they now live.
    if (output == Output::Packed)
      describePacked(cache.back());
    evict(index);
  }

  double secondsPerTick() const {
    return header.timeDenominator
               ? double(header.timeNumerator) / header.timeDenominator
               : 1.0 / 30.0;
  }

  /**
   * A pointer to a range, copying only when it has to. A capture in memory and a source
   * that implements map() both answer without a copy; anything else is read into
   * `scratch`, which the caller keeps alive for as long as it uses the pointer.
   *
   * This is the difference between one copy of a chunk and two on the way to decoding it,
   * and a chunk is tens of megabytes.
   */
  const uint8_t *bytesAt(uint64_t offset, uint64_t size,
                         std::vector<uint8_t> &scratch) const {
    if (offset > header.fileSize || size > header.fileSize - offset)
      throw Error("VGS range outside file");
    if (whole.data) {
      if (offset + size > whole.size)
        throw Error("VGS range outside file");
      return whole.data + offset;
    }
    if (size > (uint64_t(1) << 32))
      throw Error("VGS read too large");
    if (const uint8_t *mapped = source->map(offset, size_t(size)))
      return mapped;
    scratch.resize(size_t(size));
    if (size && !source->read(offset, size_t(size), scratch.data()))
      throw Error("VGS read failed");
    return scratch.data();
  }

  void describe() {
    metadata = convert(header.metadata);
    uuid = header.metadata.uuid;
    signature = {header.signature.algorithm, header.signature.keyId, header.signedSize};
    const double perTick = secondsPerTick();
    chunks.reserve(header.chunks.size());
    for (const auto &c : header.chunks) {
      ChunkInfo info;
      info.startTick = c.startTick;
      info.intervals = c.intervals;
      info.splats = c.splats;
      info.offset = c.offset;
      info.size = c.size;
      std::copy(c.bounds.begin(), c.bounds.end(), info.bounds);
      info.startSeconds = double(c.startTick) * perTick;
      info.endSeconds = double(c.startTick + c.intervals) * perTick;
      chunks.push_back(info);
    }
  }

  // Points the public Frame at the arrays the evaluator just filled. An array that came
  // back empty is reported as null rather than as a pointer to nothing.
  void publish(size_t chunkIndex, double seconds) {
    auto first = [](const std::vector<float> &v) {
      return v.empty() ? nullptr : v.data();
    };
    view.seconds = seconds;
    view.splatCount = decoded.count;
    view.positions = first(decoded.position);
    view.rotations = first(decoded.rotation);
    view.scales = first(decoded.scale);
    view.opacities = first(decoded.opacity);
    view.colors = first(decoded.colorDc);
    view.sphericalHarmonics = first(decoded.shRest);
    view.shCoefficients = decoded.shCoefficients;
    view.active = decoded.active.empty() ? nullptr : decoded.active.data();
    view.activeCount = decoded.count;
    if (!decoded.active.empty()) {
      uint64_t alive = 0;
      for (uint8_t flag : decoded.active)
        alive += flag ? 1 : 0;
      view.activeCount = alive;
    }
    view.chunkIndex = chunkIndex;
  }
};

Capture::Capture() : state(new State) {}
Capture::~Capture() = default;
Capture::Capture(Capture &&) noexcept = default;
Capture &Capture::operator=(Capture &&) noexcept = default;

uint32_t Capture::formatVersion() { return vgs::Version; }

bool Capture::looksLikeCapture(const uint8_t *data, size_t size) {
  if (!data || size < 8)
    return false;
  uint32_t magic = 0, version = 0;
  std::memcpy(&magic, data, 4);
  std::memcpy(&version, data + 4, 4);
  return magic == vgs::Magic && version == vgs::Version;
}

uint64_t Capture::structuralSize(const uint8_t *data, size_t size) {
  try {
    return vgs::structuralSize(data, size);
  } catch (const std::exception &e) {
    throw Error(e.what());
  }
}

Capture Capture::openMemory(const uint8_t *data, size_t size) {
  Capture capture;
  try {
    capture.state->header = vgs::readHeader(data, size);
  } catch (const std::exception &e) {
    throw Error(e.what());
  }
  capture.state->whole = {data, size};
  capture.state->describe();
  return capture;
}

Capture Capture::openStream(Source &source) {
  Capture capture;
  try {
    // The fixed header says how large the structural region is; that region is then read
    // once and authenticated. Nothing else is touched, however large the capture is.
    std::vector<uint8_t> prefix = fetch(source, 0, vgs::FixedHeaderSize);
    const uint64_t structural = vgs::structuralSize(prefix.data(), prefix.size());
    capture.state->structure = fetch(source, 0, structural);
    capture.state->header =
        vgs::readHeader(capture.state->structure.data(), capture.state->structure.size());
  } catch (const Error &) {
    throw;
  } catch (const std::exception &e) {
    throw Error(e.what());
  }
  capture.state->source = &source;
  capture.state->describe();
  return capture;
}

Capture Capture::openFile(const std::string &path) {
  std::unique_ptr<Source> file(new FileSource(path));
  Capture capture = openStream(*file);
  // The source outlives the capture because the capture now owns it.
  capture.state->owned = std::move(file);
  return capture;
}

const Metadata &Capture::metadata() const { return state->metadata; }
const uint8_t *Capture::uuid() const { return state->uuid.data(); }

std::string Capture::uuidText() const {
  static const char *digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < state->uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out.push_back('-');
    out.push_back(digits[state->uuid[i] >> 4]);
    out.push_back(digits[state->uuid[i] & 15]);
  }
  return out;
}

const Signature &Capture::signature() const { return state->signature; }
uint64_t Capture::createdMillis() const { return state->header.createdMillis; }
uint32_t Capture::version() const { return vgs::Version; }

bool Capture::isPlain() const {
  for (const auto &policy : state->header.policies)
    if (policy.codec != vgs::Raw)
      return false;
  return true;
}

uint64_t Capture::fileSize() const { return state->header.fileSize; }
uint64_t Capture::frameCount() const { return state->header.frameCount; }

double Capture::duration() const {
  return double(state->header.durationTicks) * state->secondsPerTick();
}

double Capture::frameRate() const {
  const auto &h = state->header;
  return h.timeNumerator ? double(h.timeDenominator) / h.timeNumerator : 0.0;
}

double Capture::startSeconds() const {
  return double(state->header.startTick) * state->secondsPerTick();
}

uint32_t Capture::shDegree() const { return state->header.shDegree; }
uint64_t Capture::maxSplatsPerFrame() const { return state->header.maxSplatsPerFrame; }
const double *Capture::bounds() const { return state->header.bounds.data(); }
size_t Capture::chunkCount() const { return state->chunks.size(); }

const ChunkInfo &Capture::chunk(size_t index) const {
  if (index >= state->chunks.size())
    throw Error("VGS chunk index out of range");
  return state->chunks[index];
}

size_t Capture::chunkAt(double seconds) const {
  const auto &chunks = state->chunks;
  if (chunks.empty() || !(seconds >= 0))
    return chunks.size();
  const double tick = seconds / state->secondsPerTick();
  for (size_t i = 0; i < chunks.size(); ++i)
    if (tick < double(chunks[i].startTick + chunks[i].intervals))
      return i;
  // The very end of the timeline is the last chunk's closing sample, not a gap.
  if (tick <= double(state->header.durationTicks))
    return chunks.size() - 1;
  return chunks.size();
}

/**
 * Everything setTime does except the evaluation: choose the chunk, decode it if it is
 * not held, point the evaluator at it and work out where in it this instant falls.
 *
 * It is separate because positions alone are a real request - it is what a renderer
 * that evaluates on the GPU still needs on the CPU, once per frame, to sort by depth -
 * and routing that through setTime evaluated every other attribute first and then threw
 * the answer away.
 */
size_t Capture::selectChunk(double seconds, bool includeSphericalHarmonics) {
  State &s = *state;
  if (s.chunks.empty())
    throw Error("VGS capture has no chunks");
  if (!std::isfinite(seconds))
    throw Error("VGS time is not a number");

  // Clamping rather than wrapping: a player that loops knows its own length, and a
  // reader that does not should not have a seek past the end silently mean something
  // else.
  const double clamped = std::min(std::max(seconds, 0.0), duration());
  s.requestedTime = clamped;

  const size_t index = chunkAt(clamped);
  if (index >= s.chunks.size())
    throw Error("VGS no chunk at time");

  const uint32_t mask = includeSphericalHarmonics ? AllLayers : BaseLayer;
  // The mask is what this call needs, not what the cache must hold exactly: a chunk
  // already decoded with more layers than asked for is used as it is.
  if (!s.find(index, mask)) {
    const ChunkInfo &info = s.chunks[index];
    std::vector<uint8_t> scratch;
    const uint8_t *bytes = s.bytesAt(info.offset, info.size, scratch);

    // A chunk held with too few layers is replaced rather than kept alongside: two
    // entries for one chunk would spend the budget on the same frames twice.
    for (size_t i = 0; i < s.cache.size(); ++i)
      if (s.cache[i].index == index) {
        s.cache.erase(s.cache.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }

    try {
      // decodeChunk checks the chunk's directory against the digest in the signed table,
      // so this is where the signature reaches the frames a player is about to draw.
      s.store(index, mask,
              vgs::decodeChunk(s.header, index, bytes, size_t(info.size), mask));
    } catch (const std::exception &e) {
      s.evaluator = nullptr;
      throw Error(e.what());
    }
  }

  State::Cached *entry = s.find(index, mask);
  if (!entry)
    throw Error("VGS chunk cache is too small for one chunk");
  if (!entry->evaluator)
    throw Error("setTime needs Output::Floats; in Packed mode use instantAt and chunkData");
  s.evaluator = entry->evaluator.get();

  const ChunkInfo &info = s.chunks[index];
  const double tick = clamped / s.secondsPerTick();
  const double normalized =
      info.intervals ? (tick - double(info.startTick)) / double(info.intervals) : 0.0;
  // Just below 1, not 1: the evaluator's domain is [0, 1), because a normalised time of
  // exactly 1 is the first instant of the next chunk rather than the last of this one.
  // Clamping to 1.0 threw at the end of the timeline, where a player that runs to the end
  // arrives every time.
  constexpr double justUnderOne = 1.0 - 1e-9;
  s.normalizedTime = std::min(std::max(normalized, 0.0), justUnderOne);
  return index;
}

const Frame &Capture::setTime(double seconds, bool includeSphericalHarmonics) {
  State &s = *state;
  const size_t index = selectChunk(seconds, includeSphericalHarmonics);
  try {
    s.evaluator->evaluateInto(s.normalizedTime, includeSphericalHarmonics, &s.decoded);
  } catch (const std::exception &e) {
    throw Error(e.what());
  }
  s.publish(index, s.requestedTime);
  return s.view;
}

bool Capture::prepare(size_t chunkIndex, double budgetMilliseconds,
                      bool includeSphericalHarmonics) {
  State &s = *state;
  if (chunkIndex >= s.chunks.size())
    throw Error("VGS chunk index out of range");

  const uint32_t mask = includeSphericalHarmonics ? AllLayers : BaseLayer;
  if (s.find(chunkIndex, mask))
    return true;

  // A different chunk, or the same one with more layers than was being built: start over
  // rather than finish work nobody is waiting for.
  if (s.pending.active && (s.pending.index != chunkIndex || s.pending.mask != mask))
    s.pending = State::Pending();

  try {
    if (!s.pending.active) {
      const ChunkInfo &info = s.chunks[chunkIndex];
      std::vector<uint8_t> scratch;
      const uint8_t *bytes = s.bytesAt(info.offset, info.size, scratch);
      s.pending.bytes.assign(bytes, bytes + info.size);
      // readChunkDirectory checks the directory against the digest in the signed table,
      // so an altered chunk is refused here, before any of it has been decoded.
      s.pending.directory = vgs::readChunkDirectory(s.header, chunkIndex,
                                                    s.pending.bytes.data(),
                                                    s.pending.bytes.size());
      s.pending.decoded.groups = s.pending.directory.groups;
      s.pending.index = chunkIndex;
      s.pending.mask = mask;
      s.pending.nextPage = 0;
      s.pending.totalPages = s.pending.directory.pages.size();
      s.pending.active = true;
    }

    const auto started = std::chrono::steady_clock::now();
    const size_t width = s.threads > 1 ? size_t(s.threads) : 1;
    std::vector<const vgs::Page *> batch;
    std::vector<vgs::Bytes> decoded;

    while (s.pending.nextPage < s.pending.totalPages) {
      // A batch of pages this mask wants. Pages are independent of each other, which is
      // what makes decompressing them together possible at all.
      batch.clear();
      while (s.pending.nextPage < s.pending.totalPages && batch.size() < width) {
        const vgs::Page &page = s.pending.directory.pages[s.pending.nextPage++];
        if (mask & (1u << page.layer))
          batch.push_back(&page);
      }
      if (batch.empty())
        continue;

      decoded.assign(batch.size(), vgs::Bytes());
      s.runParallel(batch.size(), [&](size_t i) {
        const vgs::Page &page = *batch[i];
        decoded[i] = vgs::decodePage(s.header, page, s.pending.bytes.data() + page.offset,
                                     size_t(page.size));
      });
      for (size_t i = 0; i < batch.size(); ++i)
        s.pending.decoded.pages.push_back({*batch[i], std::move(decoded[i])});

      // Checked after a batch rather than before, so a budget of zero still advances and
      // a caller cannot spin without progress.
      const double spent =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
              .count();
      if (spent >= budgetMilliseconds)
        return false;
    }

    s.store(chunkIndex, mask, std::move(s.pending.decoded));
    s.pending = State::Pending();
    return true;
  } catch (const std::exception &e) {
    s.pending = State::Pending();
    throw Error(e.what());
  }
}

void Capture::setThreadCount(unsigned count) { state->threads = count ? count : 1; }
unsigned Capture::threadCount() const { return state->threads; }

void Capture::setParallelFor(ParallelFor parallelFor) {
  state->parallelFor = std::move(parallelFor);
}

void Capture::setOutput(Output output) {
  if (state->output == output)
    return;
  state->output = output;
  // What is cached was built for the other mode, so it is of no use now. Dropping it is
  // cheaper than carrying both, and switching mid-playback is not something a player
  // does more than once.
  releaseCache();
}

Output Capture::output() const { return state->output; }

const ChunkData &Capture::chunkData(size_t chunkIndex) const {
  if (state->output != Output::Packed)
    throw Error("chunkData needs Output::Packed");
  for (const auto &entry : state->cache)
    if (entry.index == chunkIndex)
      return entry.data;
  throw Error("that chunk is not prepared; call prepare() until it returns true");
}

Instant Capture::instantAt(double seconds) const {
  Instant at;
  if (state->chunks.empty())
    throw Error("VGS capture has no chunks");

  const double clamped = std::min(std::max(seconds, 0.0), duration());
  const size_t index = chunkAt(clamped);
  if (index >= state->chunks.size())
    throw Error("VGS no chunk at time");
  const ChunkInfo &info = state->chunks[index];

  // Ticks are samples: a chunk of n intervals has n + 1 of them, and a frame sits between
  // two. The pair is clamped to the chunk rather than allowed to reach into the next one,
  // whose data a shader will not have uploaded.
  const double tick = clamped / state->secondsPerTick() - double(info.startTick);
  const double bounded = std::min(std::max(tick, 0.0), double(info.intervals));
  const double floor = std::floor(bounded);

  at.chunkIndex = index;
  at.sampleA = uint32_t(floor);
  at.sampleB = uint32_t(std::min(floor + 1, double(info.intervals)));
  at.alpha = float(bounded - floor);
  return at;
}

double Capture::preparedFraction() const {
  const State::Pending &p = state->pending;
  if (!p.active || !p.totalPages)
    return 0;
  return double(p.nextPage) / double(p.totalPages);
}

const float *Capture::positionsAt(double seconds, uint64_t *splatCount) {
  State &s = *state;
  // Shares the chunk cache with setTime, and asks for the base layer only: positions do
  // not need the colour detail, and decoding it would be most of the work. It stops
  // short of evaluating the frame, which is the point - a sorter wants three floats per
  // splat, not eleven.
  selectChunk(seconds, false);
  s.evaluator->evaluatePositions(s.normalizedTime, &s.positions);
  if (splatCount)
    *splatCount = s.positions.size() / 3;
  return s.positions.empty() ? nullptr : s.positions.data();
}

double Capture::time() const { return state->requestedTime; }
const Frame &Capture::frame() const { return state->view; }

void Capture::setCachePolicy(const CachePolicy &policy) { state->policy = policy; }
const CachePolicy &Capture::cachePolicy() const { return state->policy; }

bool Capture::isChunkCached(size_t index) const {
  for (const auto &entry : state->cache)
    if (entry.index == index)
      return true;
  return false;
}

size_t Capture::cachedChunkCount() const { return state->cache.size(); }
uint64_t Capture::cachedBytes() const { return state->cachedBytes(); }

void Capture::releaseCache() {
  state->cache.clear();
  state->pending = State::Pending();
  state->evaluator = nullptr;
  state->decoded = vgs::Frame();
  state->view = Frame();
  state->positions.clear();
  state->positions.shrink_to_fit();
}

bool Capture::hasEntry(uint32_t type) const {
  for (const auto &e : state->header.extras)
    if (e.type == type)
      return true;
  return false;
}

uint32_t Capture::extraFormat(uint32_t type) const {
  for (const auto &e : state->header.extras)
    if (e.type == type)
      return e.format;
  return 0;
}

uint64_t Capture::extraOffset(uint32_t type) const {
  for (const auto &e : state->header.extras)
    if (e.type == type)
      return e.offset;
  return 0;
}

uint64_t Capture::extraSize(uint32_t type) const {
  for (const auto &e : state->header.extras)
    if (e.type == type)
      return e.size;
  return 0;
}

std::vector<uint8_t> Capture::extra(uint32_t type) const {
  for (const auto &e : state->header.extras) {
    if (e.type != type)
      continue;
    std::vector<uint8_t> scratch;
    const uint8_t *bytes = state->bytesAt(e.offset, e.size, scratch);
    // The table entry is signed, so these bytes are covered by it once they match.
    if (vgs::digest(bytes, size_t(e.size)) != e.digest)
      throw Error(InvalidCapture);
    return std::vector<uint8_t>(bytes, bytes + e.size);
  }
  return {};
}

namespace {

// The type numbers the container uses for its extras. They are an implementation detail
// now that each payload has its own getter, so they live here rather than in the header.
constexpr uint32_t MetadataJsonExtra = 1;
constexpr uint32_t ThumbnailExtra = 2;
constexpr uint32_t AudioExtra = 3;
constexpr uint32_t MetadataJson2Extra = 4;

} // namespace

bool Capture::hasAudio() const { return hasEntry(AudioExtra); }
bool Capture::hasThumbnail() const { return hasEntry(ThumbnailExtra); }
bool Capture::hasMetadataJson() const { return hasEntry(MetadataJsonExtra); }
bool Capture::hasMetadataJson2() const { return hasEntry(MetadataJson2Extra); }

Capture::AudioFormat Capture::audioFormat() const {
  const uint32_t format = extraFormat(AudioExtra);
  return format >= 1 && format <= 4 ? AudioFormat(format) : AudioFormat::None;
}

Capture::ImageFormat Capture::thumbnailFormat() const {
  const uint32_t format = extraFormat(ThumbnailExtra);
  return format >= 1 && format <= 3 ? ImageFormat(format) : ImageFormat::None;
}

std::vector<uint8_t> Capture::audio() const { return extra(AudioExtra); }
std::vector<uint8_t> Capture::thumbnail() const { return extra(ThumbnailExtra); }

std::string Capture::metadataJson() const {
  const std::vector<uint8_t> bytes = extra(MetadataJsonExtra);
  return std::string(bytes.begin(), bytes.end());
}

Capture::Location Capture::locate(Payload payload) const {
  const uint32_t type = payload == Payload::Audio          ? AudioExtra
                        : payload == Payload::Thumbnail    ? ThumbnailExtra
                        : payload == Payload::MetadataJson ? MetadataJsonExtra
                                                           : MetadataJson2Extra;
  return {extraOffset(type), extraSize(type)};
}

std::string Capture::metadataJson2() const {
  const std::vector<uint8_t> bytes = extra(MetadataJson2Extra);
  return std::string(bytes.begin(), bytes.end());
}

} // namespace vgsdec
