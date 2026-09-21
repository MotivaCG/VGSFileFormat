#ifndef VGSDECODER_H
#define VGSDECODER_H

// Reading VFGS captures (.vgs and .pgs) from a native program.
//
// Opening a capture authenticates it. If the object exists, its structure, metadata and
// chunk table came from the authoring pipeline unaltered; anything else throws
// Error(InvalidCapture), whose message is exactly "invalid 4dgs capture".
//
//     vgsdec::Capture capture = vgsdec::Capture::openFile("boxing.vgs");
//     printf("%s by %s, %.2f s\n", capture.metadata().title.c_str(),
//            capture.metadata().author.c_str(), capture.duration());
//
//     for (double t = 0; t < capture.duration(); t += 1.0 / 30) {
//       const vgsdec::Frame &frame = capture.setTime(t);
//       draw(frame.positions, frame.rotations, frame.scales, frame.colors,
//            frame.splatCount);
//     }
//
// This library reads. It cannot write a capture and holds no private key: the container
// writer and the signing key are compiled into the encoder library and into nothing else,
// so this one is safe to ship to whoever needs to play a file.
//
// Three ways in: a path, a block of memory, or a Source the caller implements for a
// socket or a CDN. A capture is authenticated from its first kilobytes, so a streaming
// reader knows whether a file is genuine before fetching any of the payload.
//
// ---- threads, and how to stay in real time ------------------------------------------
//
// A Capture belongs to one thread. It holds the decoded chunks and the frame it last
// produced, and none of that is guarded, so two threads calling setTime on one Capture is
// a data race. Two Captures over the same file on two threads is fine.
//
// That matters because of where the time goes. Evaluating a frame of a quarter of a
// million splats takes around 16 ms, about half a frame at 30 fps, and decoding a chunk
// takes around 200 ms once a second. A renderer that calls setTime and waits has spent
// half its budget before drawing anything, and stalls outright at every chunk boundary.
//
// So do what a player does: run the Capture on its own thread, and let the renderer draw
// the last frame that was ready rather than wait for the next one. The display then runs
// at its own rate whatever the decoder is doing, which is what makes playback look
// smooth - not the decoder being fast enough to fit inside a frame.
//
//     decode thread:  frame = capture.setTime(t);  publish(copy of frame);
//                     capture.prepare(capture.chunkAt(t + 1.0), 8);
//     render thread:  draw(latest published frame);
//
// The frame arrays belong to the Capture and are replaced by its next setTime, so what
// crosses between threads is a copy, or a buffer the renderer owns.
//
// prepare() is the other half: it decodes the next chunk a few milliseconds at a time, so
// the 200 ms never lands on one frame. Without it, one frame per second is late however
// the threads are arranged.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vgsdec {

/**
 * What a reader reports for a capture it cannot vouch for: exactly "invalid 4dgs capture".
 *
 * Every authenticity failure says this and only this - an altered header, altered
 * metadata, an altered table or chunk directory, a broken, missing or unknown signature,
 * a file cut short. Which check failed is of no use to a caller and of some use to
 * whoever is trying to get past them, so the message does not say.
 *
 * Two failures deliberately say something else, because they are not about authenticity
 * but about what the file is:
 *
 *   - bytes that are not a VFGS file at all say so, which is what a loader picking
 *     between formats needs to hear;
 *   - a VFGS file of a version this build does not read says that, so an application can
 *     tell someone to update rather than tell them their capture is broken.
 *
 * Neither reveals anything: both are decided from the first eight bytes, which whoever
 * supplied the file already knows.
 */
extern const char *const InvalidCapture;

/** Anything that goes wrong: a malformed file, a failed read, a failed signature. */
class Error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/** Where the bytes come from. Implement this to stream from a file, a socket, a CDN. */
class Source {
public:
  virtual ~Source() = default;
  /**
   * Fills `into` with `size` bytes starting at `offset`. Returns false when the range is
   * not available; a short read is a failure, not a partial success.
   */
  virtual bool read(uint64_t offset, size_t size, uint8_t *into) = 0;
  /** Total size in bytes, or 0 when the source does not know it up front. */
  virtual uint64_t size() const = 0;
  /**
   * Optional. A pointer to `size` bytes at `offset` that stays valid until the next call
   * on this source, or null when the source cannot hand one over.
   *
   * A source that already holds the bytes - a mapped file, a downloaded buffer - should
   * implement this. A chunk is tens of megabytes and the decoder reads each one whole, so
   * answering here rather than through read() is one fewer copy of all of it per chunk.
   */
  virtual const uint8_t *map(uint64_t offset, size_t size) { return nullptr; }
};

/** What the capture says about itself. All strings are UTF-8. */
struct Metadata {
  /** The catalogue identifier, written by hand rather than derived from the capture. */
  std::string id;
  std::string title, author, projectName, takeName, captureStudio, copyright,
      softwareName, softwareVersion;
  std::vector<std::string> tags;
};

/** One chunk of the timeline, located without decoding anything. */
struct ChunkInfo {
  uint64_t startTick = 0, intervals = 0, splats = 0, offset = 0, size = 0;
  /** A box holding every live position in the chunk: minXYZ then maxXYZ. */
  float bounds[6] = {};
  /** The same span in seconds. */
  double startSeconds = 0, endSeconds = 0;
};

/**
 * How many decoded chunks to keep while playing.
 *
 * Decoding a chunk is the expensive part; keeping a few decoded means a player can step
 * back a second, or run a little ahead, without paying for it again. Keeping too many is
 * how a phone runs out of memory, so there is a budget.
 *
 * `behind` and `ahead` describe the shape of the window, but the budget is their sum plus
 * the chunk being played. Nothing is evicted while the total held fits, even if one side
 * is over its own share - near the end of a timeline there is nothing ahead to keep, and
 * throwing away what is behind to honour a limit that cannot be used anyway would only
 * mean decoding it again on the way back.
 *
 * A capture starts with one chunk either side, which is three chunks held and about a
 * second of stepping backwards for free. Keeping more only pays when something decodes
 * ahead of playback rather than on demand, and what makes playback smooth is having the
 * bytes downloaded, not having them decoded twice over.
 *
 * The WebAssembly build adds a 192 MB ceiling, because a browser does not get to decide
 * what machine it runs on; a native build has none, because whoever embeds it does. Both
 * are build options - VGS_CACHE_BEHIND, VGS_CACHE_AHEAD, VGS_CACHE_MAX_BYTES - and either
 * way setCachePolicy overrides them at runtime.
 */
struct CachePolicy {
  size_t behind = 1;
  size_t ahead = 1;
  /**
   * A ceiling on the decoded chunks held, in bytes. 0 means no ceiling, and the chunk
   * being played is never evicted whatever this says, so a single chunk larger than the
   * ceiling still plays.
   */
  uint64_t maxBytes = 0;
};

/** The signature a capture carries, once it has been checked. */
struct Signature {
  uint32_t algorithm = 0; // 1: Ed25519
  uint32_t keyId = 0;
  uint64_t signedBytes = 0;
};

/**
 * One instant of the capture: plain arrays, ready to upload or to walk.
 *
 * The pointers belong to the Capture and are replaced by the next setTime(); copy what
 * you need to keep. Every array has splatCount entries of the stated width, indexed the
 * same way, so element i of each describes the same splat.
 *
 * Records that are not alive at this instant are kept rather than removed, so indices
 * stay stable across a chunk; `active` says which ones count.
 */
struct Frame {
  double seconds = 0;
  /**
   * Records in this frame's arrays. A record that is not alive at this instant is kept
   * rather than removed, so that an index means the same splat for the whole chunk and a
   * renderer can hold its buffers still; `active` says which ones count.
   */
  uint64_t splatCount = 0;
  /** How many of them are alive: what to size a draw call with. */
  uint64_t activeCount = 0;

  const float *positions = nullptr;  // 3 per splat, xyz
  const float *rotations = nullptr;  // 4 per splat, xyzw
  const float *scales = nullptr;     // 3 per splat, activated
  const float *opacities = nullptr;  // 1 per splat, activated
  const float *colors = nullptr;     // 3 per splat, RGB from the SH DC term

  /** 3 per coefficient per splat, or null when the frame was decoded without them. */
  const float *sphericalHarmonics = nullptr;
  /** Coefficients per splat in sphericalHarmonics: 3, 8 or 15 for degree 1, 2 or 3. */
  int shCoefficients = 0;

  /** 1 per splat: non-zero where the record is alive at this instant. */
  const uint8_t *active = nullptr;

  /** Which chunk this instant came from. */
  size_t chunkIndex = 0;
};

/**
 * How much of the work this library does, which is the biggest decision a caller makes.
 *
 * `Floats` decompresses a chunk and evaluates it: setTime gives plain arrays of
 * positions, rotations, scales, opacities and colours, ready to use. It costs about 16 ms
 * a frame for a quarter of a million splats, which is half a frame at 30 fps and more
 * than a whole one on a headset at 90.
 *
 * `Packed` decompresses a chunk and stops. It hands over the result as buffers to upload,
 * and per frame says only where the current time falls between two of the chunk's
 * samples. Decompressing costs the same either way; what `Packed` leaves out is the
 * evaluation.
 *
 * Be clear about what that means: the work does not disappear, it moves to you. Nothing
 * in this library touches a GPU - there is no shader here and no graphics API. `Packed`
 * is for a renderer that will interpolate and activate in its own vertex shader, on data
 * the GPU needs anyway, and the buffers and the sample pair are shaped for exactly that.
 * A caller that takes `Packed` and then evaluates on the CPU has gained nothing and
 * rewritten what `Floats` already does.
 *
 * So: `Floats` to compute - an exporter, a physics body, a collision mesh. `Packed` to
 * draw, if and only if your shader does the rest. When it does, the per-frame cost on
 * this side stops depending on the splat count or on how many captures are playing.
 *
 * The price of `Packed` is that your shader has to know how the attributes are encoded,
 * which FORMAT.md describes. `Floats` keeps all of that inside the library.
 */
enum class Output {
  Floats,
  Packed
};

/**
 * One array of a prepared chunk, as stored, ready to upload.
 *
 * `kind` and `width` say how to read an element; FORMAT.md, under the numerical
 * descriptor, defines each kind. An attribute larger than one page arrives as several
 * buffers with the same `attribute` and ascending `firstRow`.
 */
struct Buffer {
  uint32_t attribute = 0;
  uint32_t group = 0;
  /** 0 base, 1 static spherical harmonics, 2 temporal. */
  uint32_t layer = 0;
  uint64_t firstRow = 0, rows = 0, totalRows = 0;
  uint32_t kind = 0, width = 0;
  const uint8_t *data = nullptr;
  uint64_t size = 0;
};

/** A group of splats within a chunk, and the ranges its values are reconstructed against. */
struct GroupData {
  uint32_t type = 0, flags = 0;
  uint64_t splats = 0, intervals = 0;
  double positionMin = 0, positionMax = 0, trajectoryMin = 0, trajectoryMax = 0;
};

/** Everything a shader needs about one prepared chunk. Valid until the chunk is evicted. */
struct ChunkData {
  size_t chunkIndex = 0;
  /** Samples in this chunk; a frame falls between two of them. */
  uint64_t sampleCount = 0;
  const GroupData *groups = nullptr;
  size_t groupCount = 0;
  const Buffer *buffers = nullptr;
  size_t bufferCount = 0;
  /** What uploading all of it costs. */
  uint64_t totalBytes = 0;
};

/** Where a time falls between a chunk's samples: everything a frame needs in Packed mode. */
struct Instant {
  size_t chunkIndex = 0;
  uint32_t sampleA = 0, sampleB = 0;
  /** 0 at sampleA, 1 at sampleB. */
  float alpha = 0;
};

/**
 * Runs `count` pieces of work, however the host likes, and returns when all are done.
 *
 * A library that creates its own threads fights whatever the application already has:
 * play eight captures with four threads each and a machine with eight cores has
 * thirty-two threads competing with the renderer. Hand one of these in and the decoder
 * never creates a thread at all - the work goes to the task system that is already
 * there, and every capture shares it.
 *
 * `body` must be safe to call from several threads at once, which it is: it is given a
 * different index each time and they touch different data.
 */
using ParallelFor = std::function<void(size_t count, const std::function<void(size_t)> &body)>;

/** An open, authenticated capture. */
class Capture {
public:
  ~Capture();
  Capture(Capture &&) noexcept;
  Capture &operator=(Capture &&) noexcept;

  /** Opens a capture from a file, read as it is needed rather than all at once. */
  static Capture openFile(const std::string &path);

  /**
   * Opens a capture already in memory. The bytes are borrowed and must outlive the
   * returned object; nothing is copied.
   */
  static Capture openMemory(const uint8_t *data, size_t size);

  /**
   * Opens a capture from a source, reading only what it takes to authenticate it. The
   * source is borrowed and must outlive the capture.
   */
  static Capture openStream(Source &);

  /**
   * True when the first bytes of a file look like a capture this build can read, without
   * opening it or throwing. Useful for choosing a loader; it says nothing about whether
   * the file is genuine.
   */
  static bool looksLikeCapture(const uint8_t *data, size_t size);

  /** How many bytes from the start authenticating a file will need. */
  static uint64_t structuralSize(const uint8_t *data, size_t size);

  /** The format version this build reads. */
  static uint32_t formatVersion();

  // ---- what the capture says about itself --------------------------------------

  const Metadata &metadata() const;
  /** The capture's identifier, 16 raw bytes. */
  const uint8_t *uuid() const;
  /** The identifier as 8-4-4-4-12 hexadecimal. */
  std::string uuidText() const;
  /** When the capture was written: milliseconds since the Unix epoch, UTC. */
  uint64_t createdMillis() const;
  const Signature &signature() const;
  uint32_t version() const;
  /** True when the pages are stored without entropy coding, which is what .pgs means. */
  bool isPlain() const;

  // ---- the timeline --------------------------------------------------------------

  /** The last time that can be asked for, in seconds. Time runs from 0 to here. */
  double duration() const;
  uint64_t frameCount() const;
  /** Frames per second of the capture's own timebase. */
  double frameRate() const;
  /** Where this capture starts on an external timeline, in seconds. */
  double startSeconds() const;
  uint32_t shDegree() const;
  uint64_t maxSplatsPerFrame() const;
  uint64_t fileSize() const;
  /** The capture's own box over every frame: minXYZ then maxXYZ. */
  const double *bounds() const;

  size_t chunkCount() const;
  const ChunkInfo &chunk(size_t index) const;
  /** The chunk covering a time in seconds, or chunkCount() when there is none. */
  size_t chunkAt(double seconds) const;

  // ---- using more than one core ---------------------------------------------------
  //
  // Decompressing a chunk is the one part of this that parallelises well: its pages are
  // independent, and decoding them at once divides the cost by about the number of cores
  // given to it. Evaluation stays on the calling thread.
  //
  // It is off by default. A decoder that quietly spawns threads is a decoder that
  // interferes with whatever the host is doing, and the host is better placed to decide.
  //
  // Note what this does not solve. Several captures playing at once are already parallel
  // without any of this: give each one its own Capture on its own thread and they scale
  // with the cores you have, because they share nothing. Spreading one capture over more
  // cores makes that one finish sooner; it does not make the machine do less work.
  //
  // Which leads to the rule worth following: keep captures x threads-per-capture at about
  // the number of cores.
  //
  //   more captures than cores    leave this alone. You are already using the machine,
  //                               and a second level of parallelism underneath the first
  //                               adds scheduling and risk for nothing.
  //   fewer captures than cores   turn it on. One hero capture on eight cores is the case
  //                               this exists for.
  //
  // Doing both at once nests one parallel-for inside another. A task system built for
  // that - most engine task graphs and work-stealing schedulers do - copes, running the
  // inner work on the calling thread rather than waiting for a free one. A hand-written
  // pool with a blocking join can deadlock outright: every worker holding an outer task,
  // waiting on inner tasks that no thread is left to run. If you parallelise per
  // capture, set this to 1.

  /**
   * How many pages may be decompressed at once. 1, the default, keeps everything on the
   * calling thread.
   *
   * With no parallel-for installed the decoder runs them on threads of its own, made for
   * the batch and joined at the end of it. With one installed, this is only how many
   * pieces of work it is handed at a time.
   */
  void setThreadCount(unsigned count);
  unsigned threadCount() const;

  /**
   * Where that work runs. Install this and the decoder creates no threads; leave it out
   * and it makes its own. Either way setThreadCount decides how many pages at a time.
   */
  void setParallelFor(ParallelFor);

  // ---- how frames come out ------------------------------------------------------

  /**
   * Chooses between evaluating on the CPU and handing the data to a shader. Changing it
   * drops whatever is decoded, because the two modes keep different things.
   */
  void setOutput(Output);
  Output output() const;

  /**
   * The prepared chunk's data, to upload. Only in Packed mode, and only after prepare()
   * has finished with that chunk; throws otherwise. The pointers belong to the capture
   * and stay valid until the chunk is evicted, which isChunkCached() reports.
   */
  const ChunkData &chunkData(size_t chunkIndex) const;

  /**
   * Where `seconds` falls between the samples of its chunk. Costs nothing: no decoding,
   * no allocation, no lookup beyond the chunk table. This is the whole of a frame's CPU
   * work in Packed mode.
   */
  Instant instantAt(double seconds) const;

  // ---- playback --------------------------------------------------------------------

  /**
   * Decodes the instant at `seconds` and returns it. Times outside [0, duration()] are
   * clamped. Pass includeSphericalHarmonics false to skip the colour detail layers,
   * which is most of the work when a viewer does not need them.
   *
   * Consecutive times inside one chunk reuse the decoded chunk, so playing forward costs
   * one chunk decode per chunk rather than one per frame. How many decoded chunks are
   * kept either side is setCachePolicy().
   */
  const Frame &setTime(double seconds, bool includeSphericalHarmonics = true);

  /**
   * Positions alone for the instant at `seconds`, as 3 floats per splat, decoding no more
   * than it takes to produce them. A renderer that evaluates everything else on the GPU
   * still needs these on the CPU when it sorts splats by depth there, and reading them
   * back from the GPU costs tens of milliseconds a frame on a phone.
   *
   * The returned pointer belongs to the capture and is replaced by the next call. Its
   * length is 3 * splatCount(), which is frame().splatCount after this returns.
   */
  const float *positionsAt(double seconds, uint64_t *splatCount = nullptr);

  /**
   * Decodes a chunk a little at a time, so that nothing blocks for long.
   *
   * This is what a player calls to stay smooth. Decoding a chunk takes around 150 ms and a
   * frame at 30 fps has 33, so a player that decodes when it arrives at a chunk drops
   * frames at every boundary however fast the decoder is and however early the bytes were
   * downloaded. Spread over the second of playback before it, the same work is invisible.
   *
   *     // once a frame, after drawing, with whatever time is left over
   *     capture.prepare(capture.chunkAt(now + 1.0), 4.0);
   *
   * Returns true when the chunk is fully decoded and setTime on it will not block; false
   * when there is more to do, so call it again next frame. Asking for a different chunk
   * abandons whatever was in progress, and asking for one already decoded returns true
   * without doing anything.
   *
   * `budgetMilliseconds` is advisory: one page is always decoded, so a budget of zero
   * still makes progress rather than spinning.
   *
   * Choose it so the work finishes before playback arrives: a chunk takes roughly 200 ms
   * to decode and covers about a second, so at 30 fps a budget under 7 ms does not get
   * there in time and the boundary is a dropped frame anyway. 8 ms leaves margin and
   * still fits inside a 33 ms frame alongside evaluating one, which costs about 16.
   *
   * @param chunkIndex from chunkAt()
   * @param budgetMilliseconds how long this call may spend
   * @param includeSphericalHarmonics must match what setTime will ask for, or the work is
   *   done again
   */
  bool prepare(size_t chunkIndex, double budgetMilliseconds,
               bool includeSphericalHarmonics = true);

  /** How far the chunk being prepared has got, 0 to 1. */
  double preparedFraction() const;

  /** The time last asked for. */
  double time() const;
  /** The instant last decoded. Empty until the first setTime(). */
  const Frame &frame() const;

  // ---- what playback keeps in memory ------------------------------------------------

  /**
   * How many decoded chunks to keep either side of the one being played. Applied on the
   * next setTime; it never evicts on its own, so tightening it while paused takes effect
   * when playback resumes.
   */
  void setCachePolicy(const CachePolicy &);
  const CachePolicy &cachePolicy() const;

  /** Whether a chunk is decoded right now, so a player knows what it would cost. */
  bool isChunkCached(size_t index) const;
  size_t cachedChunkCount() const;
  /** Roughly how much the decoded chunks are holding. */
  uint64_t cachedBytes() const;

  /** Drops every decoded chunk. For a player that has seeked away and wants the memory. */
  void releaseCache();

  // ---- payloads carried alongside -------------------------------------------------
  //
  // A capture can carry a sound track, a thumbnail and two independent blocks of JSON.
  // Each has its own getter; each fetches the bytes and checks them against the signed
  // table before handing them over, so what comes back is as genuine as the frames.

  /** What a sound track is. The container stores it as delivered and never transcodes. */
  enum class AudioFormat { None = 0, Mp3 = 1, Aac = 2, Opus = 3, Wav = 4 };
  enum class ImageFormat { None = 0, Png = 1, Jpeg = 2, Webp = 3 };

  bool hasAudio() const;
  AudioFormat audioFormat() const;
  /** The sound track as it was delivered, or empty when the capture carries none. */
  std::vector<uint8_t> audio() const;

  bool hasThumbnail() const;
  ImageFormat thumbnailFormat() const;
  std::vector<uint8_t> thumbnail() const;

  bool hasMetadataJson() const;
  /** The free-form JSON block as UTF-8, or empty when there is none. */
  std::string metadataJson() const;

  bool hasMetadataJson2() const;
  /**
   * The second block. There are two because one belongs to whoever produced the capture
   * and the other to whoever uses it, and neither should have to parse the other's.
   */
  std::string metadataJson2() const;

  /**
   * Where a payload sits in the file. Only needed by a reader that fetches ranges itself
   * instead of letting the getters above read through its Source - a browser, mostly,
   * which cannot answer a synchronous read. Everything else can ignore this.
   */
  enum class Payload { Audio, Thumbnail, MetadataJson, MetadataJson2 };
  struct Location {
    uint64_t offset = 0, size = 0;
  };
  Location locate(Payload) const;

private:
  Capture();
  bool hasEntry(uint32_t type) const;
  uint32_t extraFormat(uint32_t type) const;
  uint64_t extraOffset(uint32_t type) const;
  uint64_t extraSize(uint32_t type) const;
  std::vector<uint8_t> extra(uint32_t type) const;
  struct State;
  std::unique_ptr<State> state;
};

} // namespace vgsdec
#endif
