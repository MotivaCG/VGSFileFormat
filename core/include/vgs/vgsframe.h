#pragma once
#include "vgscodec.h"
#include <map>
#include <string>
namespace vgs {
// Owning frame arrays. Rotation is xyzw; scale and opacity are activated.
// colorDc is RGB (0.5 + C0 * SH0); shRest is [splat][shCoefficients][RGB], with
// three, eight or fifteen coefficients for SH degree 1, 2 or 3.
// Inactive records are retained to keep stable indices within a chunk.
struct Frame {
  double seconds = 0;
  int chunkIndex = 0, sampleIndex = 0;
  float sampleAlpha = 0;
  uint64_t count = 0;
  std::vector<float> position, rotation, scale, opacity, colorDc, shRest;
  // Coefficients per splat in shRest; 0 when the frame was evaluated without SH.
  int shCoefficients = 0;
  std::vector<uint8_t> active;
};
// Owns assembled attributes from one decoded chunk; no Qt or renderer
// dependency. Construct once per chunk, then evaluate repeatedly. Throws
// vgs::Error on invalid data.
class FrameDecoder {
public:
  // What the decoded chunk handed in holds, and so what can be asked of it.
  //
  // `Frame` is the whole of a frame: every base attribute, and the SH layers if they
  // were decoded. `Positions` is the position attributes alone (see usedByPositions),
  // which is all evaluatePositions reads and about a third of the base layer's decoding;
  // a chunk built that way can only answer evaluatePositions, and evaluate throws.
  enum class Contents { Frame, Positions };

  // The attributes evaluatePositions reads. A caller that decodes pages itself and only
  // wants positions keeps these and skips the rest of the base layer.
  static bool usedByPositions(uint32_t attribute);

  // secondsPerTick comes from the header (timeNumerator / timeDenominator); the
  // default is the 30 Hz timebase every MINT import uses.
  explicit FrameDecoder(const DecodedChunk &, double secondsPerTick = 1.0 / 30.0,
                        Contents = Contents::Frame);
  Contents contents() const { return held; }
  Frame evaluate(double normalizedTime, bool includeSh = true) const;
  // The same, into a frame the caller keeps. A frame of a quarter of a million splats is
  // tens of megabytes of arrays, and returning one by value allocates and zero-fills all
  // of them every call, only to overwrite them immediately. Handing the same frame back
  // reuses the buffers: resizing to a size a vector already has does nothing.
  void evaluateInto(double normalizedTime, bool includeSh, Frame *) const;
  // Positions alone, as [splat][xyz]. A renderer that evaluates everything else on the
  // GPU still needs these on the CPU when it sorts splats by depth there, and reading
  // them back from the GPU costs tens of milliseconds on a phone.
  void evaluatePositions(double normalizedTime, std::vector<float> *) const;

private:
  struct Block {
    uint64_t splats = 0, intervals = 0, shStaticEntries = 0,
             shTemporalEntries = 0, sh0Entries = 0, opacityEntries = 0,
             rotationEntries = 0, positionEntries = 0;
    bool positionPerSample = false, rotationPerSample = false;
    double positionMin = 0, positionMax = 0, trajectoryMin = 0,
           trajectoryMax = 0;
    std::map<std::string, Bytes> arrays;
    uint64_t samples() const { return intervals + 1; }
    const char *array(const char *name) const;
  };
  struct SharedBasis {
    std::vector<float> sh0, opacity, position, rotation;
  };
  std::vector<Block> blocks;
  double secondsPerTick = 1.0 / 30.0;
  Contents held = Contents::Frame;
  void buildBasis(const Block &, uint64_t, float, bool, bool,
                  SharedBasis *) const;
  void decodeGroupColor(const Block &, const Block &, const SharedBasis &,
                        std::vector<float> *) const;
};
} // namespace vgs
