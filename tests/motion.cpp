// The arithmetic behind motion samples, which every reader runs: spherical harmonic
// rotation, interpolation between samples, and the stored form. No capture is needed.
#include "vgsframe.h"
#include <cmath>
#include <cstdio>
#include <random>

namespace {
int failures = 0;
void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}
// The same basis FrameDecoder rotates against, written out again so the test does not
// trust the code under test for it.
void basis(double x, double y, double z, double *b) {
  b[0] = -0.4886025119029199 * y;
  b[1] = 0.4886025119029199 * z;
  b[2] = -0.4886025119029199 * x;
  b[3] = 1.0925484305920792 * x * y;
  b[4] = -1.0925484305920792 * y * z;
  b[5] = 0.31539156525252005 * (2 * z * z - x * x - y * y);
  b[6] = -1.0925484305920792 * x * z;
  b[7] = 0.5462742152960396 * (x * x - y * y);
  b[8] = -0.5900435899266435 * y * (3 * x * x - y * y);
  b[9] = 2.890611442640554 * x * y * z;
  b[10] = -0.4570457994644658 * y * (4 * z * z - x * x - y * y);
  b[11] = 0.3731763325901154 * z * (2 * z * z - 3 * x * x - 3 * y * y);
  b[12] = -0.4570457994644658 * x * (4 * z * z - x * x - y * y);
  b[13] = 1.445305721320277 * z * (x * x - y * y);
  b[14] = -0.5900435899266435 * x * (x * x - 3 * y * y);
}
void normalise(double *v, int n) {
  double s = 0;
  for (int i = 0; i < n; ++i)
    s += v[i] * v[i];
  s = std::sqrt(s);
  for (int i = 0; i < n; ++i)
    v[i] /= s;
}
} // namespace

int main() {
  std::mt19937 random(12345);
  std::normal_distribution<double> gauss;

  // Rotating the coefficients must equal evaluating the original at the rotated-back
  // direction: f'(d) = f(R^T d), for every band and any direction.
  double worst = 0;
  for (int trial = 0; trial < 200; ++trial) {
    vgs::Motion m;
    for (double &v : m.rotation)
      v = gauss(random);
    normalise(m.rotation, 4);
    double matrix[12];
    m.matrix(matrix);
    double band[3][49];
    vgs::shRotationMatrices(m.rotation, band[0], band[1], band[2]);
    double c[15];
    for (double &v : c)
      v = gauss(random);
    double rotated[15] = {};
    const int first[3] = {0, 3, 8}, size[3] = {3, 5, 7};
    for (int b = 0; b < 3; ++b)
      for (int row = 0; row < size[b]; ++row)
        for (int k = 0; k < size[b]; ++k)
          rotated[first[b] + row] += band[b][row * size[b] + k] * c[first[b] + k];
    for (int sample = 0; sample < 20; ++sample) {
      double d[3] = {gauss(random), gauss(random), gauss(random)};
      normalise(d, 3);
      double back[3];
      for (int k = 0; k < 3; ++k)
        back[k] = matrix[k] * d[0] + matrix[4 + k] * d[1] + matrix[8 + k] * d[2];
      double world[15], local[15];
      basis(d[0], d[1], d[2], world);
      basis(back[0], back[1], back[2], local);
      for (int b = 0; b < 3; ++b) {
        double expected = 0, got = 0;
        for (int k = 0; k < size[b]; ++k) {
          expected += c[first[b] + k] * local[first[b] + k];
          got += rotated[first[b] + k] * world[first[b] + k];
        }
        worst = std::max(worst, std::abs(expected - got));
      }
    }
  }
  check(worst < 1e-9, "SH rotation matches evaluation at the rotated direction");

  // The identity leaves every band alone.
  {
    const double identity[4] = {0, 0, 0, 1};
    double band[3][49];
    vgs::shRotationMatrices(identity, band[0], band[1], band[2]);
    double error = 0;
    for (int b = 0, n = 3; b < 3; ++b, n += 2)
      for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
          error = std::max(error, std::abs(band[b][i * n + j] - (i == j ? 1.0 : 0.0)));
    check(error < 1e-12, "identity SH rotation");
  }

  // Interpolation: exact at both ends, half the angle in the middle, the shorter arc.
  {
    vgs::Motion a, b;
    a.translation[0] = 1;
    a.scale = 1;
    b.translation[0] = 3;
    b.scale = 2;
    const double half = 3.14159265358979323846 / 4; // 90 degrees about z
    b.rotation[2] = std::sin(half);
    b.rotation[3] = std::cos(half);
    const auto start = vgs::interpolateMotion(a, b, 0), end = vgs::interpolateMotion(a, b, 1);
    const auto middle = vgs::interpolateMotion(a, b, 0.5);
    check(std::abs(start.translation[0] - 1) < 1e-15 && std::abs(end.translation[0] - 3) < 1e-15,
          "interpolation reaches both translations");
    check(std::abs(end.rotation[2] - b.rotation[2]) < 1e-12 && std::abs(end.scale - 2) < 1e-15,
          "interpolation reaches the second rotation and scale");
    check(std::abs(middle.translation[0] - 2) < 1e-15 && std::abs(middle.scale - 1.5) < 1e-15,
          "translation and scale are linear");
    check(std::abs(2 * std::atan2(middle.rotation[2], middle.rotation[3]) - half) < 1e-12,
          "rotation is spherical: half the angle at the midpoint");
    vgs::Motion flipped = b;
    for (double &v : flipped.rotation)
      v = -v; // the same rotation, the other sign
    const auto shorter = vgs::interpolateMotion(a, flipped, 0.5);
    check(std::abs(std::abs(2 * std::atan2(shorter.rotation[2], shorter.rotation[3])) - half) < 1e-12,
          "interpolation takes the shorter arc");
  }

  // Stored form: exact round trip, and anything a reader cannot use is refused.
  {
    std::vector<vgs::Motion> samples(3);
    samples[1].translation[1] = -12.25;
    samples[1].rotation[0] = std::sin(0.3);
    samples[1].rotation[3] = std::cos(0.3);
    samples[2].scale = 0.5;
    const auto bytes = vgs::packMotionSamples(samples);
    check(bytes.size() == 3 * vgs::MotionSampleBytes, "eight f64 per sample");
    const auto back = vgs::unpackMotionSamples(bytes.data(), bytes.size(), 3);
    bool same = true;
    for (size_t i = 0; i < 3; ++i)
      for (int k = 0; k < 3; ++k)
        same = same && back[i].translation[k] == samples[i].translation[k];
    check(same && back[2].scale == 0.5 && std::abs(back[1].rotation[0] - std::sin(0.3)) < 1e-15,
          "motion samples round trip");
    auto refused = [&](std::vector<vgs::Motion> bad) {
      const auto b = vgs::packMotionSamples(bad);
      try {
        vgs::unpackMotionSamples(b.data(), b.size(), bad.size());
      } catch (const vgs::Error &) {
        return true;
      }
      return false;
    };
    auto badScale = samples;
    badScale[0].scale = 0;
    auto badRotation = samples;
    badRotation[0].rotation[3] = 2;
    auto badTranslation = samples;
    badTranslation[0].translation[2] = std::nan("");
    check(refused(badScale) && refused(badRotation) && refused(badTranslation),
          "invalid samples are refused");
    check(vgs::isMotionSamplesSpec(vgs::motionSamplesSpec(31), 31 * 8, 31) &&
              !vgs::isMotionSamplesSpec(vgs::motionSamplesSpec(31), 30 * 8, 31),
          "the descriptor is fixed by the sample count");
  }

  // matrix(): R s | t, with R the quaternion's rotation.
  {
    vgs::Motion m;
    m.rotation[2] = std::sin(3.14159265358979323846 / 4);
    m.rotation[3] = std::cos(3.14159265358979323846 / 4); // +90 degrees about z
    m.scale = 2;
    m.translation[0] = 5;
    double r[12];
    m.matrix(r);
    // (1, 0, 0) -> (0, 2, 0) + (5, 0, 0)
    check(std::abs(r[0] * 1 + r[3] - 5) < 1e-12 && std::abs(r[4] * 1 - 2) < 1e-12,
          "matrix rotates, scales and translates");
  }

  if (failures) {
    std::printf("%d motion check(s) failed\n", failures);
    return 1;
  }
  std::printf("motion: all checks passed (worst SH error %.3g)\n", worst);
  return 0;
}
