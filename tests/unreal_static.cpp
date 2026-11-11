// The VGSStatic Unreal plugin's import, without Unreal: VGSStaticPly.h reads a Gaussian splat
// PLY and builds what the asset keeps (records in cluster order, normal words, clusters);
// VGSStaticNormals.h works out normals for a PLY that has none (a rig's cameras, or each
// splat's shortest axis as PGSR, corrected by its neighbours).
//
//   vgs_unreal_static                 synthetic PLYs, binary and ASCII: every value read and
//                                     packed as the renderer reads it, header comments,
//                                     clusters, normal words; shortest-axis normals on a sphere
//   vgs_unreal_static some.ply        also reads a real one, builds it, and works out normals
//                                     both ways (against the file's, if it has them)
#include "VGSStaticNormals.h"
#include "VGSStaticNormals.inl"
#include "VGSStaticPly.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define EXPECT(cond, ...)                                                                                              \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      ++failures;                                                                                                      \
      std::printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #cond);                                                       \
      std::printf(__VA_ARGS__);                                                                                        \
      std::printf("\n");                                                                                               \
    }                                                                                                                  \
  } while (0)

// One splat as a training run writes it: raw (log scale, logit opacity, unnormalised wxyz).
struct Raw {
  float p[3], n[3], dc[3], rest[45], opacity, scale[3], rot[4];
};

std::vector<Raw> randomSplats(size_t count, int restPerChannel, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<Raw> out(count);
  for (auto &r : out) {
    for (int c = 0; c < 3; ++c) {
      r.p[c] = u(rng) * 0.8f;
      r.n[c] = u(rng);
      r.dc[c] = u(rng) * 2.0f;
      r.scale[c] = -5.0f + u(rng) * 2.0f;
    }
    for (int k = 0; k < 3 * restPerChannel; ++k) r.rest[k] = u(rng) * 0.3f;
    r.opacity = u(rng) * 4.0f;
    for (float &q : r.rot) q = u(rng) * 2.0f;
  }
  return out;
}

// Property order as SMNForge and the 3DGS trainer write it; `doubles` writes positions as doubles.
std::string plyHeader(const char *format, size_t count, int restPerChannel, bool normals, bool doubles) {
  std::ostringstream h;
  h << "ply\nformat " << format << " 1.0\ncomment synthetic\ncomment capture_rig  SMNV1\ncomment normals SMNForge\nelement vertex " << count << "\n";
  const char *pt = doubles ? "double" : "float";
  h << "property " << pt << " x\nproperty " << pt << " y\nproperty " << pt << " z\n";
  if (normals) h << "property float nx\nproperty float ny\nproperty float nz\n";
  for (int c = 0; c < 3; ++c) h << "property float f_dc_" << c << "\n";
  for (int k = 0; k < 3 * restPerChannel; ++k) h << "property float f_rest_" << k << "\n";
  h << "property float opacity\n";
  for (int c = 0; c < 3; ++c) h << "property float scale_" << c << "\n";
  for (int c = 0; c < 4; ++c) h << "property float rot_" << c << "\n";
  h << "end_header\n";
  return h.str();
}

std::vector<uint8_t> writePly(const std::vector<Raw> &splats, int restPerChannel, bool normals, bool ascii, bool doubles) {
  std::string text = plyHeader(ascii ? "ascii" : "binary_little_endian", splats.size(), restPerChannel, normals, doubles);
  std::vector<uint8_t> out(text.begin(), text.end());
  auto putF = [&](float v) {
    if (ascii) {
      char b[32];
      std::snprintf(b, sizeof(b), "%.9g ", v);
      out.insert(out.end(), b, b + std::strlen(b));
    } else {
      const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
      out.insert(out.end(), p, p + 4);
    }
  };
  auto putD = [&](double v) {
    if (ascii) {
      char b[40];
      std::snprintf(b, sizeof(b), "%.17g ", v);
      out.insert(out.end(), b, b + std::strlen(b));
    } else {
      const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
      out.insert(out.end(), p, p + 8);
    }
  };
  for (const Raw &r : splats) {
    for (int c = 0; c < 3; ++c) doubles ? putD(r.p[c]) : putF(r.p[c]);
    if (normals) for (int c = 0; c < 3; ++c) putF(r.n[c]);
    for (int c = 0; c < 3; ++c) putF(r.dc[c]);
    for (int k = 0; k < 3 * restPerChannel; ++k) putF(r.rest[k]);
    putF(r.opacity);
    for (int c = 0; c < 3; ++c) putF(r.scale[c]);
    for (int c = 0; c < 4; ++c) putF(r.rot[c]);
    if (ascii) out.push_back('\n');
  }
  return out;
}

float rel(float a, float b) { return std::fabs(a - b) / std::max(std::fabs(b), 1e-6f); }

void checkSynthetic(int restPerChannel, bool normals, bool ascii, bool doubles) {
  const size_t count = 1000;
  std::vector<Raw> raw = randomSplats(count, restPerChannel, 7u + unsigned(restPerChannel));
  raw[17].scale[1] = std::nanf(""); // dropped
  const std::vector<uint8_t> file = writePly(raw, restPerChannel, normals, ascii, doubles);
  vgsstatic::Splats s;
  const std::string error = vgsstatic::readPly(file.data(), file.size(), s);
  const char *what = ascii ? "ascii" : "binary";
  EXPECT(error.empty(), "%s: %s", what, error.c_str());
  if (!error.empty()) return;
  EXPECT(s.count == count - 1 && s.dropped == 1, "%s: %zu splats, %zu dropped", what, s.count, s.dropped);
  EXPECT(s.shCoefficients == restPerChannel, "%s: %d coefficients", what, s.shCoefficients);
  EXPECT(normals == !s.normals.empty(), "%s: normals", what);
  vgsstatic::NormalRig rig = vgsstatic::NormalRig::The4DScanner;
  EXPECT(s.comment("normals") == "SMNForge" && s.comment("capture_rig") == "SMNV1" && s.comment("synthetic").empty() &&
             s.comment("capture").empty() && vgsstatic::rigFromComment(s, rig) && rig == vgsstatic::NormalRig::SmnV1,
         "%s: header comments", what);

  // Every value, activated as the renderer wants it.
  const float tol = ascii ? 1e-6f : 0.0f;
  float worst = 0.0f;
  for (size_t i = 0, j = 0; j < count; ++j) {
    if (j == 17) continue;
    const Raw &r = raw[j];
    for (int c = 0; c < 3; ++c) {
      worst = std::max(worst, rel(s.positions[i * 3 + c], r.p[c]));
      worst = std::max(worst, rel(s.scales[i * 3 + c], std::exp(r.scale[c])) * 0.5f);
      worst = std::max(worst, rel(s.radiance[i * 4 + c], r.dc[c]));
    }
    worst = std::max(worst, rel(s.radiance[i * 4 + 3], 1.0f / (1.0f + std::exp(-r.opacity))));
    const float ql = std::sqrt(r.rot[0] * r.rot[0] + r.rot[1] * r.rot[1] + r.rot[2] * r.rot[2] + r.rot[3] * r.rot[3]);
    for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(s.rotations[i * 4 + c] - r.rot[c] / ql));
    for (int k = 0; k < restPerChannel; ++k)
      for (int c = 0; c < 3; ++c) // f_rest: all of red, then green, then blue
        worst = std::max(worst, rel(s.sh[(size_t(k) * s.count + i) * 3 + c], r.rest[c * restPerChannel + k]));
    if (normals) {
      const float nl = std::sqrt(r.n[0] * r.n[0] + r.n[1] * r.n[1] + r.n[2] * r.n[2]);
      for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(s.normals[i * 3 + c] - r.n[c] / nl));
    }
    ++i;
  }
  EXPECT(worst <= std::max(tol * 4, 2e-7f), "%s SH %d: worst error %g", what, restPerChannel, worst);

  // Built: the records hold what was read, in Morton order; the clusters cover them.
  vgsstatic::Built b;
  vgsstatic::build(s, s.normals.empty() ? nullptr : s.normals.data(), b, 64);
  EXPECT(b.records.size() == size_t(b.count) * b.stride && b.stride == vgsunreal::floatStride(restPerChannel), "%s: records", what);
  size_t mismatches = 0;
  for (uint32_t i = 0; i < b.count; ++i) {
    const uint8_t *rec = b.records.data() + size_t(i) * b.stride;
    const size_t j = b.order[i];
    float v[14];
    std::memcpy(v, rec, 56);
    const float *q = &s.rotations[j * 4];
    const float expect[14] = {s.positions[j * 3], s.positions[j * 3 + 1], s.positions[j * 3 + 2], s.scales[j * 3], s.scales[j * 3 + 1],
                              s.scales[j * 3 + 2], q[1], q[2], q[3], q[0], s.radiance[j * 4], s.radiance[j * 4 + 1], s.radiance[j * 4 + 2],
                              s.radiance[j * 4 + 3]};
    mismatches += std::memcmp(v, expect, sizeof(v)) != 0;
    for (int k = 0; k < restPerChannel; ++k)
      mismatches += std::memcmp(rec + 56 + k * 12, &s.sh[(size_t(k) * s.count + j) * 3], 12) != 0;
  }
  EXPECT(mismatches == 0, "%s: %zu records differ from the splats read", what, mismatches);

  uint32_t covered = 0;
  size_t outside = 0;
  for (const auto &c : b.clusters) {
    EXPECT(c.splatOffset == covered && c.level == 0 && c.parentIndex == ~0u, "cluster layout");
    for (uint32_t i = c.splatOffset; i < c.splatOffset + c.splatCount; ++i) {
      const float *p = reinterpret_cast<const float *>(b.records.data() + size_t(i) * b.stride);
      for (int a = 0; a < 3; ++a) outside += p[a] < c.boundsMin[a] || p[a] > c.boundsMax[a];
    }
    covered += c.splatCount;
  }
  EXPECT(covered == b.count && outside == 0, "%s: clusters cover %u of %u, %zu centres outside", what, covered, b.count, outside);

  if (normals) {
    float worstAngle = 0.0f;
    for (uint32_t i = 0; i < b.count; ++i) {
      float n[3];
      vgsstatic::unpackNormal(b.normals[i], n);
      const float *e = &s.normals[size_t(b.order[i]) * 3];
      worstAngle = std::max(worstAngle, std::acos(std::min(1.0f, n[0] * e[0] + n[1] * e[1] + n[2] * e[2])));
    }
    EXPECT(worstAngle < 1e-3f, "%s: normal words off by %g rad", what, worstAngle);
  }
}

void checkRejected() {
  const auto read = [](const std::string &text) {
    vgsstatic::Splats s;
    return vgsstatic::readPly(reinterpret_cast<const uint8_t *>(text.data()), text.size(), s);
  };
  EXPECT(!read("hello").empty(), "not a PLY");
  EXPECT(read("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\n"
              "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n0 0 0 1 2 3\n")
             .find("point cloud") != std::string::npos,
         "a coloured point cloud is named as such");
  EXPECT(read("ply\nformat binary_big_endian 1.0\nelement vertex 1\nproperty float x\nend_header\n").find("big") != std::string::npos,
         "big endian is refused by name");
  // Zero normals are no normals.
  std::vector<Raw> raw = randomSplats(10, 0, 3);
  for (auto &r : raw) r.n[0] = r.n[1] = r.n[2] = 0.0f;
  const auto file = writePly(raw, 0, true, false, false);
  vgsstatic::Splats s;
  EXPECT(vgsstatic::readPly(file.data(), file.size(), s).empty() && s.normals.empty(), "nx ny nz all 0 means none");
}

void checkNormalWords() {
  std::mt19937 rng(11);
  std::normal_distribution<float> g;
  float worst = 0.0f;
  for (int i = 0; i < 100000; ++i) {
    float n[3] = {g(rng), g(rng), g(rng)};
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (float &c : n) c /= l;
    const uint32_t w = vgsstatic::packNormal(n);
    EXPECT(w != 0u && (w & 0xFFFFu) != 0u && (w >> 16) != 0u, "a normal never packs to 0");
    float m[3];
    vgsstatic::unpackNormal(w, m);
    worst = std::max(worst, std::acos(std::min(1.0f, n[0] * m[0] + n[1] * m[1] + n[2] * m[2])));
  }
  EXPECT(worst < 1e-3f, "normal words round trip within %g rad (16 bits a component)", worst);
  std::printf("normal words: worst round trip %.2g rad over 100000 directions\n", worst);
}

double seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

void checkFile(const char *path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto t = std::chrono::steady_clock::now();
  vgsstatic::Splats s;
  const std::string error = vgsstatic::readPly(file.data(), file.size(), s);
  EXPECT(error.empty(), "%s: %s", path, error.c_str());
  if (!error.empty()) return;
  std::printf("%s: %zu splats (%zu dropped), SH %d coefficients, %zu with normals; read in %.2f s\n", path, s.count, s.dropped,
              s.shCoefficients, s.splatsWithNormals, seconds(t));
  t = std::chrono::steady_clock::now();
  vgsstatic::Built b;
  vgsstatic::build(s, s.normals.empty() ? nullptr : s.normals.data(), b);
  std::printf("  built %zu clusters, %.1f MB of records in %.2f s\n", b.clusters.size(), b.records.size() / 1048576.0, seconds(t));

  // Against the file's normals: signed for the rig's (outward), |cos| for shortest axes, whose
  // side the renderer decides on screen.
  const auto compare = [&](const std::vector<float> &normals, bool sided) {
    double agree = 0.0, within30 = 0.0;
    size_t compared = 0;
    for (size_t i = 0; i < s.count && !s.normals.empty(); ++i) {
      const float *a = &s.normals[i * 3], *e = &normals[i * 3];
      if ((a[0] == 0 && a[1] == 0 && a[2] == 0) || (e[0] == 0 && e[1] == 0 && e[2] == 0)) continue;
      const float c = sided ? a[0] * e[0] + a[1] * e[1] + a[2] * e[2] : std::fabs(a[0] * e[0] + a[1] * e[1] + a[2] * e[2]);
      agree += c;
      within30 += c >= 0.8660254f;
      ++compared;
    }
    if (compared)
      std::printf("; to the file's: mean %s %.3f, %.1f%% within 30 deg", sided ? "cos" : "|cos|", agree / double(compared), 100.0 * within30 / double(compared));
    std::printf("\n");
  };
  for (auto rig : {vgsstatic::NormalRig::SmnV1, vgsstatic::NormalRig::The4DScanner}) {
    t = std::chrono::steady_clock::now();
    std::vector<float> normals;
    const auto r = vgsstatic::rigNormals(s, rig, normals);
    std::printf("  normals, %-12s rig: cameras see %5.1f%% of %zu opaque splats; %.2f s", rig == vgsstatic::NormalRig::SmnV1 ? "SMNV1" : "THE4DSCANNER",
                100.0 * r.seenShare(), r.opaque, seconds(t));
    compare(normals, true);
  }
  {
    t = std::chrono::steady_clock::now();
    std::vector<float> normals;
    const auto r = vgsstatic::surroundingNormals(s, normals);
    std::printf("  normals, surrounding cameras: they see %5.1f%% of %zu opaque splats; %.2f s", 100.0 * r.seenShare(), r.opaque, seconds(t));
    compare(normals, true);
    std::printf("  ... the same, either side");
    compare(normals, false);
  }
  struct Variant { const char *name; int passes, smoothing, neighbours; float radius; };
  for (const Variant v : {Variant{"PGSR alone", 0, 0, 16, 0.01f}, Variant{"consensus", 2, 0, 16, 0.01f}, Variant{"+ smoothing", 2, 2, 16, 0.01f},
                          Variant{"+ smoothing, 32 / 2 %", 2, 2, 32, 0.02f}, Variant{"+ smoothing x4", 2, 4, 16, 0.01f}}) {
    t = std::chrono::steady_clock::now();
    std::vector<float> normals;
    vgsstatic::AxisNormalsSettings settings;
    settings.passes = v.passes;
    settings.smoothingPasses = v.smoothing;
    settings.neighbours = v.neighbours;
    settings.radiusShare = v.radius;
    const auto r = vgsstatic::axisNormals(s, normals, settings);
    std::printf("  normals, shortest axes, %-22s: %.0f%% flat, %.0f%% took the neighbours'; %.2f s", v.name,
                100.0 * double(r.flat) / double(r.splats), 100.0 * double(r.replaced) / double(r.splats), seconds(t));
    compare(normals, false);
  }
}

// Discs tangent to a sphere, some round and some turned across it: the shortest axes,
// corrected by their neighbours, should come out radial (either side).
void checkAxisNormals() {
  std::mt19937 rng(5);
  std::normal_distribution<float> g;
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  vgsstatic::Splats s;
  const size_t count = 20000;
  s.count = count;
  std::vector<int> kind(count); // 0 a good disc, 1 round, 2 a disc turned 90 degrees
  for (size_t i = 0; i < count; ++i) {
    Eigen::Vector3f d(g(rng), g(rng), g(rng));
    d.normalize();
    s.positions.insert(s.positions.end(), {d.x(), d.y(), d.z()});
    Eigen::Quaternionf q = Eigen::Quaternionf::FromTwoVectors(Eigen::Vector3f::UnitZ(), d); // the disc's z along the radius
    const float r = u(rng);
    kind[i] = r < 0.15f ? 1 : r < 0.3f ? 2 : 0;
    if (kind[i] == 2) q = q * Eigen::Quaternionf(Eigen::AngleAxisf(1.5707963f, Eigen::Vector3f::UnitX()));
    s.rotations.insert(s.rotations.end(), {q.w(), q.x(), q.y(), q.z()});
    s.scales.insert(s.scales.end(), {0.02f, 0.02f, kind[i] == 1 ? 0.019f : 0.002f});
    s.radiance.insert(s.radiance.end(), {0.0f, 0.0f, 0.0f, 0.9f});
  }
  for (int passes : {0, 2}) {
    std::vector<float> normals;
    vgsstatic::AxisNormalsSettings settings;
    settings.passes = passes;
    settings.radiusShare = 0.05f;
    const auto r = vgsstatic::axisNormals(s, normals, settings);
    double off[3] = {0, 0, 0}, total[3] = {0, 0, 0};
    for (size_t i = 0; i < count; ++i) {
      const float c = std::fabs(normals[i * 3] * s.positions[i * 3] + normals[i * 3 + 1] * s.positions[i * 3 + 1] + normals[i * 3 + 2] * s.positions[i * 3 + 2]);
      total[kind[i]] += 1;
      off[kind[i]] += c < 0.9396926f; // more than 20 degrees from the radius
    }
    std::printf("shortest axes on a sphere, %d passes: %zu replaced; more than 20 deg off: discs %.1f%%, round %.1f%%, turned %.1f%%\n", passes, r.replaced,
                100 * off[0] / total[0], 100 * off[1] / total[1], 100 * off[2] / total[2]);
    if (passes)
      EXPECT(off[0] / total[0] < 0.01 && off[1] / total[1] < 0.05 && off[2] / total[2] < 0.05, "corrected shortest axes follow the sphere");
  }
}

} // namespace

int main(int argc, char **argv) {
  for (int rest : {0, 3, 8, 15}) {
    checkSynthetic(rest, true, false, false);
    checkSynthetic(rest, rest == 8, false, true);
    checkSynthetic(rest, false, true, rest == 3);
  }
  checkRejected();
  checkNormalWords();
  checkAxisNormals();
  for (int i = 1; i < argc; ++i) checkFile(argv[i]);
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
