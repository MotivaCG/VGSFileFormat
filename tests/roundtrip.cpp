// Encode a capture, read it back, and check that what comes out is what went in.
//
// The encoder already verifies its own output against the source during the encode, which
// catches a codec that loses data. This checks the other half: that the container says
// what it was told to say, that the timeline adds up, and that the decoder's view of a
// capture matches the encoder's.

#include "vgsdecoder/vgsdecoder.h"
#include "vgsencoder/vgsencoder.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0, checks = 0;

void check(bool ok, const char *what) {
  ++checks;
  std::printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
  if (!ok)
    ++failures;
}

template <typename T> void equals(const T &got, const T &want, const char *what) {
  ++checks;
  if (got == want) {
    std::printf("ok    %s\n", what);
  } else {
    std::printf("FAIL  %s\n", what);
    ++failures;
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: roundtrip <input.mint> <output.vgs>\n");
    return 2;
  }

  const std::string id = "round-trip-id";
  const std::string title = "Round trip";
  const std::string author = "SMN|The4DSCanner";

  vgsenc::Encoder encoder;
  check(encoder.setInputFile(argv[1]), "the source opens");
  encoder.setCoding(vgsenc::Coding::Compressed);
  encoder.setSphericalHarmonicDegree(2);
  encoder.setId(id);
  encoder.setTitle(title);
  encoder.setAuthor(author);
  encoder.setCaptureStudio("Valladolid");
  encoder.addTag("test");
  encoder.addTag("round-trip");
  encoder.setMetadataJson("{\"kind\":\"test\"}");

  if (!encoder.write(argv[2])) {
    std::fprintf(stderr, "encode failed: %s\n", encoder.lastError().c_str());
    return 1;
  }
  const std::string uuid = encoder.uuidText();
  std::printf("wrote %llu bytes, identifier %s\n\n",
              static_cast<unsigned long long>(encoder.outputSize()), uuid.c_str());

  try {
    vgsdec::Capture capture = vgsdec::Capture::openFile(argv[2]);

    equals(capture.metadata().id, id, "the identifier survives");
    equals(capture.metadata().title, title, "the title survives");
    equals(capture.metadata().author, author, "the author survives");
    equals(capture.metadata().tags.size(), size_t(2), "both tags survive");
    equals(capture.uuidText(), uuid, "the decoder derives the identifier the encoder did");
    check(capture.createdMillis() > 0, "the capture records when it was written");
    equals(capture.signature().keyId, uint32_t(1), "it is signed with the authoring key");
    check(!capture.isPlain(), "a .vgs reports itself as compressed");

    check(capture.duration() > 0, "the timeline has a duration");
    check(capture.chunkCount() > 0, "the timeline has chunks");
    check(capture.frameCount() > 0, "the timeline has frames");

    // The chunks must tile the timeline without a gap or an overlap: a player walking
    // forward has to land somewhere for every time it asks about.
    bool contiguous = true;
    for (size_t i = 1; i < capture.chunkCount(); ++i)
      if (capture.chunk(i).startTick != capture.chunk(i - 1).startTick +
                                            capture.chunk(i - 1).intervals)
        contiguous = false;
    check(contiguous, "the chunks tile the timeline");

    // Every time in the capture resolves to a chunk, including both ends.
    bool everyTimeResolves = true;
    for (int i = 0; i <= 20; ++i) {
      const double t = capture.duration() * i / 20.0;
      if (capture.chunkAt(t) >= capture.chunkCount())
        everyTimeResolves = false;
    }
    check(everyTimeResolves, "every time in the capture resolves to a chunk");

    // Decoding: shapes have to agree with each other and with the header.
    const vgsdec::Frame &frame = capture.setTime(capture.duration() / 2);
    check(frame.splatCount > 0, "the middle of the capture decodes to splats");
    check(frame.activeCount <= frame.splatCount,
          "the live splats are a subset of the records");
    check(frame.activeCount <= capture.maxSplatsPerFrame(),
          "no frame holds more live splats than the header promised");
    check(frame.positions && frame.rotations && frame.scales && frame.opacities &&
              frame.colors,
          "every base attribute comes back");
    equals(frame.shCoefficients, 8, "spherical harmonic degree 2 means eight coefficients");
    check(frame.sphericalHarmonics != nullptr, "the harmonics come back too");

    // Positions must sit inside the box the header declares, or a renderer culling
    // against it would drop splats that are really there.
    const double *bounds = capture.bounds();
    bool inside = true;
    for (uint64_t i = 0; i < frame.splatCount; ++i) {
      if (frame.active && !frame.active[i])
        continue;
      for (int axis = 0; axis < 3; ++axis) {
        const double v = frame.positions[i * 3 + size_t(axis)];
        if (v < bounds[axis] - 1e-3 || v > bounds[axis + 3] + 1e-3)
          inside = false;
      }
    }
    check(inside, "every live splat sits inside the capture's declared box");

    // Asking for the same instant twice must give the same answer.
    const uint64_t first = frame.splatCount;
    const float x = frame.positions[0];
    const vgsdec::Frame &again = capture.setTime(capture.duration() / 2);
    check(again.splatCount == first && again.positions[0] == x,
          "the same time decodes to the same frame");

    // Positions alone must agree with the full evaluation, and share its decoded chunk.
    uint64_t count = 0;
    const float *positions = capture.positionsAt(capture.duration() / 2, &count);
    check(count == first && positions && positions[0] == x,
          "positions alone agree with the full frame");

    // And they must not need a setTime to have happened first. positionsAt exists for a
    // renderer that evaluates everything else on the GPU and only sorts on the CPU: it
    // asks for nothing but positions, at an instant nothing else was asked about.
    const double elsewhere = capture.duration() / 4;
    uint64_t aloneCount = 0;
    const float *alone = capture.positionsAt(elsewhere, &aloneCount);
    std::vector<float> keep(alone, alone + aloneCount * 3);
    const vgsdec::Frame &whole = capture.setTime(elsewhere);
    bool samePositions = aloneCount == whole.splatCount;
    for (uint64_t i = 0; samePositions && i < aloneCount * 3; ++i)
      if (keep[size_t(i)] != whole.positions[i])
        samePositions = false;
    check(samePositions, "positions alone match the full frame at an untouched instant");

    // Detail::Positions: a chunk decoded for positions alone must answer positionsAt
    // exactly as a fully decoded one does, hold less, and not pass for more than it is.
    // Checked on every chunk and at both ends of each, on a capture of its own so no
    // entry left behind by the checks above can answer instead.
    {
      vgsdec::Capture lean = vgsdec::Capture::openFile(argv[2]);
      vgsdec::Capture reference = vgsdec::Capture::openFile(argv[2]);
      bool identical = true, smaller = true, notMore = true, upgrades = true;
      for (size_t c = 0; c < lean.chunkCount(); ++c) {
        const vgsdec::ChunkInfo &info = lean.chunk(c);
        lean.releaseCache();
        while (!lean.prepare(c, 1000, vgsdec::Detail::Positions)) {
        }
        if (!lean.isChunkCached(c, vgsdec::Detail::Positions) ||
            lean.isChunkCached(c, vgsdec::Detail::Base))
          notMore = false;
        const uint64_t leanBytes = lean.cachedBytes();
        for (double t : {info.startSeconds, (info.startSeconds + info.endSeconds) / 2,
                         info.endSeconds - 1e-6}) {
          uint64_t n = 0, m = 0;
          const float *p = lean.positionsAt(t, &n);
          std::vector<float> got(p, p + n * 3);
          const float *q = reference.positionsAt(t, &m);
          const vgsdec::Frame &full = reference.setTime(t);
          if (n != m || n != full.splatCount ||
              std::memcmp(got.data(), q, size_t(n) * 12) != 0 ||
              std::memcmp(got.data(), full.positions, size_t(n) * 12) != 0)
            identical = false;
        }
        // positionsAt must not have widened what was decoded.
        if (lean.isChunkCached(c, vgsdec::Detail::Base))
          notMore = false;
        // setTime on it decodes the rest and then holds the whole chunk.
        if (lean.setTime(info.startSeconds).splatCount == 0 ||
            !lean.isChunkCached(c, vgsdec::Detail::Full))
          upgrades = false;
        if (!(lean.cachedBytes() > leanBytes))
          smaller = false;
      }
      check(identical, "positions from a positions-only chunk are bit-identical to a full decode");
      check(notMore, "a positions-only chunk does not report itself as more");
      check(smaller, "a positions-only chunk holds less than a full one");
      check(upgrades, "setTime on a positions-only chunk decodes the rest");

      // The earlier spelling of prepare still means what it did.
      lean.releaseCache();
      while (!lean.prepare(0, 1000, false)) {
      }
      check(lean.isChunkCached(0, vgsdec::Detail::Base) &&
                !lean.isChunkCached(0, vgsdec::Detail::Full),
            "prepare(..., false) is still the base layer");

      // pages(): the directory, undecoded. The position pages are the base layer's
      // position_ attributes, and nothing else is.
      const std::vector<vgsdec::PageInfo> pages = lean.pages(0);
      bool consistent = !pages.empty(), anyPosition = false;
      uint64_t stored = 0;
      for (const vgsdec::PageInfo &page : pages) {
        stored += page.storedSize;
        const bool named = page.attribute.rfind("position_", 0) == 0;
        if (page.usedByPositions != (named && page.layer == 0) || page.rows == 0 ||
            page.attribute.empty())
          consistent = false;
        anyPosition |= page.usedByPositions;
      }
      check(consistent && anyPosition, "pages() lists the directory and marks the position pages");
      check(stored <= lean.chunk(0).size, "the pages fit inside their chunk");
    }

    // Times outside the capture are clamped rather than refused or wrapped.
    check(capture.setTime(-5).seconds == 0.0, "a negative time clamps to the start");
    check(capture.setTime(capture.duration() + 5).seconds == capture.duration(),
          "a time past the end clamps to the end");

    // Packed mode: the same chunk, handed over rather than evaluated. What matters is
    // that the description agrees with itself, since a shader will index by it.
    capture.setOutput(vgsdec::Output::Packed);
    const size_t middle = capture.chunkAt(capture.duration() / 2);
    while (!capture.prepare(middle, 1000, false)) {
    }
    const vgsdec::ChunkData &data = capture.chunkData(middle);
    check(data.bufferCount > 0 && data.groupCount > 0, "a packed chunk has buffers and groups");
    check(data.sampleCount > 1, "a packed chunk spans more than one sample");

    uint64_t declared = 0;
    bool described = true;
    for (size_t i = 0; i < data.bufferCount; ++i) {
      const vgsdec::Buffer &buffer = data.buffers[i];
      declared += buffer.size;
      if (!buffer.data || buffer.size == 0 || buffer.rows == 0 ||
          buffer.firstRow + buffer.rows > buffer.totalRows || buffer.layer > 2)
        described = false;
    }
    check(described, "every buffer points somewhere and sits inside its attribute");
    check(declared == data.totalBytes, "the buffers add up to what the chunk claims");

    const vgsdec::Instant at = capture.instantAt(capture.duration() / 2);
    check(at.chunkIndex == middle, "the instant lands in the chunk that was prepared");
    check(at.sampleB < data.sampleCount && at.sampleA <= at.sampleB,
          "the sample pair is inside the chunk and in order");
    check(at.alpha >= 0.0f && at.alpha <= 1.0f, "the blend factor is between the two");

    // The ends of the timeline are where an off-by-one would show.
    const vgsdec::Instant atStart = capture.instantAt(0);
    const vgsdec::Instant atEnd = capture.instantAt(capture.duration());
    check(atStart.sampleA == 0 && atStart.alpha == 0.0f,
          "time zero is the first sample exactly");
    check(atEnd.chunkIndex == capture.chunkCount() - 1,
          "the end of the timeline lands in the last chunk");

    // And the mode is a switch, not a one-way door.
    capture.setOutput(vgsdec::Output::Floats);
    check(capture.setTime(capture.duration() / 2).splatCount > 0,
          "switching back to floats decodes again");

    equals(capture.metadataJson(), std::string("{\"kind\":\"test\"}"),
           "the free-form metadata survives and passes its digest");
    check(!capture.hasAudio() && !capture.hasThumbnail(),
          "payloads that were never written are reported absent");

    std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "\nreading it back failed: %s\n", error.what());
    return 1;
  }
}
