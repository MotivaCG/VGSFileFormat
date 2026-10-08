#pragma once
#include "vgscodec.h"
#include <functional>
#include <map>
#include <string>
namespace vgs {
// How a capture moves as a whole: world = translation + scale * rotate(rotation, local).
// Rotation is a unit quaternion, xyzw; scale is uniform and positive. A capture without
// motion samples behaves as if every sample were the identity.
//
// Stored per chunk as the shared attribute MotionSamples, one sample at every sample
// point of the chunk (intervals + 1), each eight little-endian f64 in the order
// tx ty tz qx qy qz qw s. Between two samples translation and scale are interpolated
// linearly and rotation by spherical linear interpolation along the shorter arc, with the
// same fraction every other attribute uses.
//
// FrameDecoder applies it: positions, rotations and scales come out moved, and the
// higher-order spherical harmonics come out rotated, so a reader of Frame needs nothing
// further. A renderer that evaluates the stored attributes itself applies motion(t) as a
// model matrix and evaluates the harmonics with the view direction rotated by the inverse
// rotation.
struct Motion {
  double translation[3] = {0, 0, 0};
  double rotation[4] = {0, 0, 0, 1};
  double scale = 1;
  bool isIdentity() const;
  // Row-major 3x4: [R*s | t].
  void matrix(double out[12]) const;
};
constexpr uint64_t MotionSampleBytes = 64;
// The one numerical descriptor motion samples are stored with: 8-byte words split into
// four 16-bit fields, which entropy codes losslessly. `totalRows` is samples * 8.
Spec motionSamplesSpec(uint64_t samples);
bool isMotionSamplesSpec(const Spec &, uint64_t totalRows, uint64_t samples);
// Whether a capture declares motion; if it does, every chunk carries its samples.
bool declaresMotion(const Header &);
Bytes packMotionSamples(const std::vector<Motion> &);
// Validates (finite, unit rotation within 1e-6, scale in (0, 1e6]) and renormalises the
// rotation; throws vgs::Error otherwise.
std::vector<Motion> unpackMotionSamples(const uint8_t *, size_t, uint64_t samples);
Motion interpolateMotion(const Motion &a, const Motion &b, double fraction);
// The spherical harmonic rotation matrices for one rotation, in this format's coefficient
// order (bands 1, 2 and 3: 3x3, 5x5, 7x7, row-major): rotated = M * stored.
void shRotationMatrices(const double rotation[4], double band1[9], double band2[25],
                        double band3[49]);

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

  // secondsPerTick comes from the header (timeNumerator / timeDenominator), which the
  // encoder takes from the source; the default is only a fallback, 30 Hz.
  explicit FrameDecoder(const DecodedChunk &, double secondsPerTick = 1.0 / 30.0,
                        Contents = Contents::Frame);
  Contents contents() const { return held; }
  Frame evaluate(double normalizedTime, bool includeSh = true) const;
  // The same, into a frame the caller keeps. A frame of a quarter of a million splats is
  // tens of megabytes of arrays, and returning one by value allocates and zero-fills all
  // of them every call, only to overwrite them immediately. Handing the same frame back
  // reuses the buffers: resizing to a size a vector already has does nothing.
  //
  // `pieces` above 1 splits the splats into that many ranges and hands them to
  // `parallel`, which runs body(0) .. body(count - 1) however it likes and returns when
  // all are done. Every splat is computed from shared tables and from nothing another
  // splat writes, so the frame is the same bit for bit however it is split. With 1, or
  // without `parallel`, all of it runs on the calling thread.
  using Parallel =
      std::function<void(size_t count, const std::function<void(size_t)> &body)>;
  void evaluateInto(double normalizedTime, bool includeSh, Frame *, size_t pieces = 1,
                    const Parallel &parallel = {}) const;
  // Positions alone, as [splat][xyz]. A renderer that evaluates everything else on the
  // GPU still needs these on the CPU when it sorts splats by depth there, and reading
  // them back from the GPU costs tens of milliseconds on a phone.
  void evaluatePositions(double normalizedTime, std::vector<float> *) const;
  // The chunk's motion at a normalised time, and whether it has any. Every evaluation
  // above has already applied it.
  bool hasMotion() const { return !motionSamples.empty(); }
  Motion motion(double normalizedTime) const;

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
  std::vector<Motion> motionSamples;
  double secondsPerTick = 1.0 / 30.0;
  Contents held = Contents::Frame;
  void buildBasis(const Block &, uint64_t, float, bool, bool,
                  SharedBasis *) const;
};
} // namespace vgs
