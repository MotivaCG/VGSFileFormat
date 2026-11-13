// The VGSStatic Unreal plugin's import, without Unreal: VGSStaticPly.h reads a Gaussian splat
// PLY and builds what the asset keeps (records in cluster order, normal words, clusters);
// VGSStaticNormals.h works out normals for a PLY that has none (a rig's cameras, or each
// splat's shortest axis as PGSR, corrected by its neighbours); VGSStaticProxy.h makes the
// shadow proxy mesh of where the splats are opaque.
//
//   vgs_unreal_static                 synthetic PLYs, binary and ASCII: every value read and
//                                     packed as the renderer reads it, header comments,
//                                     clusters, normal words; shortest-axis normals on a sphere;
//                                     the shadow proxy of a sphere of splats and a floater
//   vgs_unreal_static some.ply        also reads a real one, builds it, works out normals
//                                     both ways (against the file's, if it has them) and its
//                                     shadow proxy
//   vgs_unreal_static some.ply x.obj  ... and writes that proxy to x.obj
#include "VGSStaticNormals.h"
#include "VGSStaticNormals.inl"
#include "VGSStaticPly.h"
#include "VGSStaticProxy.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
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

// What a proxy mesh is: closed (every edge in an even number of triangles), its signed
// volume (positive when the triangles face outwards), its vertices' distance from a point.
struct MeshCheck {
  bool closed = true;
  double volume = 0.0;
};

MeshCheck checkMesh(const vgsstatic::ProxyMesh &m) {
  MeshCheck r;
  std::map<std::pair<uint32_t, uint32_t>, int> edges;
  const auto &P = m.positions;
  for (size_t t = 0; t < m.indices.size(); t += 3) {
    const uint32_t i[3] = {m.indices[t], m.indices[t + 1], m.indices[t + 2]};
    for (int k = 0; k < 3; ++k) ++edges[std::minmax(i[k], i[(k + 1) % 3])];
    const float *a = &P[size_t(i[0]) * 3], *b = &P[size_t(i[1]) * 3], *c = &P[size_t(i[2]) * 3];
    r.volume += (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) - double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) +
                 double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) / 6.0;
  }
  for (const auto &e : edges)
    if (e.second % 2) r.closed = false;
  return r;
}

void writeObj(const vgsstatic::ProxyMesh &m, const char *path) {
  std::ofstream out(path);
  for (size_t i = 0; i < m.positions.size(); i += 3) out << "v " << m.positions[i] << ' ' << m.positions[i + 1] << ' ' << m.positions[i + 2] << '\n';
  for (size_t t = 0; t < m.indices.size(); t += 3) out << "f " << m.indices[t] + 1 << ' ' << m.indices[t + 1] + 1 << ' ' << m.indices[t + 2] + 1 << '\n';
}

// A hollow sphere of flat splats (radius 0.3, as a character's skin is a shell) and two stray
// splats: the proxy is one closed surface around the sphere, facing out, its inside
// filled, the stray left out; with an inset it is that much smaller.
void checkProxy() {
  std::mt19937 rng(9);
  std::normal_distribution<float> g;
  vgsstatic::Splats s;
  const float R = 0.3f;
  for (int i = 0; i < 30000; ++i) {
    Eigen::Vector3f d(g(rng), g(rng), g(rng));
    d.normalize();
    s.positions.insert(s.positions.end(), {R * d.x(), R * d.y(), R * d.z()});
    const Eigen::Quaternionf q = Eigen::Quaternionf::FromTwoVectors(Eigen::Vector3f::UnitZ(), d);
    s.rotations.insert(s.rotations.end(), {q.w(), q.x(), q.y(), q.z()});
    s.scales.insert(s.scales.end(), {0.01f, 0.01f, 0.001f});
    s.radiance.insert(s.radiance.end(), {0.0f, 0.0f, 0.0f, 0.9f});
  }
  // strays, opaque and big enough to be solid on their own: one far off, outside the grid,
  // and one 7 cm from the sphere, a floater inside it
  for (float x : {0.6f, 0.37f}) {
    s.positions.insert(s.positions.end(), {x, 0.0f, 0.0f});
    s.rotations.insert(s.rotations.end(), {1.0f, 0.0f, 0.0f, 0.0f});
    s.scales.insert(s.scales.end(), {0.01f, 0.01f, 0.01f});
    s.radiance.insert(s.radiance.end(), {0.0f, 0.0f, 0.0f, 1.0f});
  }
  s.count = s.positions.size() / 3;

  for (float inset : {0.0f, 0.02f}) {
    vgsstatic::ProxySettings settings;
    settings.inset = inset;
    vgsstatic::ProxyMesh m;
    const auto t = std::chrono::steady_clock::now();
    const bool built = vgsstatic::buildProxy(s, settings, m);
    EXPECT(built, "a proxy for the sphere");
    if (!built) continue;
    const MeshCheck c = checkMesh(m);
    double mean = 0.0, far = 0.0, lo = 1e9, hi = 0.0;
    for (size_t i = 0; i < m.positions.size(); i += 3) {
      const double r = std::sqrt(double(m.positions[i]) * m.positions[i] + double(m.positions[i + 1]) * m.positions[i + 1] +
                                 double(m.positions[i + 2]) * m.positions[i + 2]);
      mean += r;
      lo = std::min(lo, r);
      hi = std::max(hi, r);
      far = std::max(far, double(m.positions[i]));
    }
    mean /= double(m.positions.size() / 3);
    const double expected = R - inset;
    std::printf("proxy of a sphere, inset %.2f: %zu vertices, %zu triangles, voxel %.3f, grid %dx%dx%d, %zu solid, %zu filled inside, %zu floaters; "
                "radius mean %.4f (%.4f to %.4f), volume %.4f (sphere %.4f); %.2f s\n",
                inset, m.positions.size() / 3, m.indices.size() / 3, m.voxel, m.dims[0], m.dims[1], m.dims[2], m.solid, m.insideFilled,
                m.floaterPieces, mean, lo, hi, c.volume, 4.18879 * expected * expected * expected, seconds(t));
    EXPECT(c.closed, "the proxy is closed");
    EXPECT(c.volume > 0.0, "its triangles face outwards (volume %.4f)", c.volume);
    // The splats are 1 mm thick and the voxels take each splat's nearest point: the surface
    // lies within about a voxel outside the sphere's.
    EXPECT(std::fabs(mean - expected) < 0.012, "mean radius %.4f, about %.4f", mean, expected);
    EXPECT(lo > expected - 0.02 && hi < expected + 0.025, "radius %.4f to %.4f", lo, hi);
    EXPECT(std::fabs(c.volume / (4.18879 * expected * expected * expected) - 1.0) < 0.2, "the inside is filled: volume %.4f", c.volume);
    EXPECT(far < 0.34 && m.floaterPieces == 1, "the stray splats are left out (x up to %.3f, %zu floaters)", far, m.floaterPieces);
  }
}

// A thin splat lying diagonally across the voxels, off their corners: its voxels must be found
// by the point of highest density in each, not the point nearest its centre in metres.
// And a plate 1 cm thick with a 2 cm inset: the inset stops short of turning it inside out.
void checkProxyThin() {
  {
    vgsstatic::Splats s;
    const float c = 0.7071068f, sn = 0.7071068f; // 45 degrees about z: (cos 22.5, 0, 0, sin 22.5)
    (void)c; (void)sn;
    s.positions = {0.0031f, -0.0023f, 0.0047f};
    s.rotations = {0.9238795f, 0.0f, 0.0f, 0.3826834f};
    s.scales = {0.1f, 0.0015f, 0.0015f};
    s.radiance = {0.0f, 0.0f, 0.0f, 1.0f};
    s.count = 1;
    vgsstatic::ProxySettings settings;
    settings.inset = 0.0f;
    vgsstatic::ProxyMesh m;
    const bool built = vgsstatic::buildProxy(s, settings, m);
    EXPECT(built, "a proxy for a thin diagonal splat");
    double lo = 1e9, hi = -1e9;
    for (size_t i = 0; i < m.positions.size(); i += 3) {
      const double along = 0.7071068 * (m.positions[i] + m.positions[i + 1]);
      lo = std::min(lo, along);
      hi = std::max(hi, along);
    }
    // Solid where exp(-t^2 / 2 sigma^2) >= 0.357 (opacity 0.3): |t| <= 0.143 m along it.
    std::printf("thin diagonal splat: %zu solid voxels, the proxy spans %.3f to %.3f along it (about -0.143 to 0.143), %zu pieces dropped\n", m.solid, lo, hi, m.floaterPieces);
    EXPECT(built && lo < -0.12 && hi > 0.12 && m.floaterPieces == 0, "the thin splat is found along its length (%.3f to %.3f)", lo, hi);
  }
  {
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-0.2f, 0.2f), t(-0.003f, 0.003f);
    vgsstatic::Splats s;
    for (int i = 0; i < 20000; ++i) {
      s.positions.insert(s.positions.end(), {u(rng), u(rng), t(rng)});
      s.rotations.insert(s.rotations.end(), {1.0f, 0.0f, 0.0f, 0.0f});
      s.scales.insert(s.scales.end(), {0.008f, 0.008f, 0.001f});
      s.radiance.insert(s.radiance.end(), {0.0f, 0.0f, 0.0f, 0.9f});
    }
    s.count = s.positions.size() / 3;
    vgsstatic::ProxySettings settings;
    settings.inset = 0.02f;
    vgsstatic::ProxyMesh m;
    const bool built = vgsstatic::buildProxy(s, settings, m);
    EXPECT(built, "a proxy for a thin plate");
    if (!built) return;
    const MeshCheck c = checkMesh(m);
    double zlo = 1e9, zhi = -1e9;
    for (size_t i = 2; i < m.positions.size(); i += 3) {
      zlo = std::min(zlo, double(m.positions[i]));
      zhi = std::max(zhi, double(m.positions[i]));
    }
    std::printf("thin plate, inset 0.02: %zu triangles, %s, volume %.5f, z from %.4f to %.4f\n", m.indices.size() / 3, c.closed ? "closed" : "NOT closed",
                c.volume, zlo, zhi);
    EXPECT(c.closed && c.volume > 0.0, "the plate is not turned inside out by the inset (volume %.5f)", c.volume);
  }
}

// A real PLY's shadow proxy, with the import's settings (a PLY in metres): how long, how big.
void checkFileProxy(const char *path, const char *obj) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  vgsstatic::Splats s;
  if (!vgsstatic::readPly(file.data(), file.size(), s).empty()) return;
  vgsstatic::ProxyMesh m;
  const auto t = std::chrono::steady_clock::now();
  const bool built = vgsstatic::buildProxy(s, vgsstatic::ProxySettings(), m);
  EXPECT(built, "%s: a proxy", path);
  if (!built) return;
  const MeshCheck c = checkMesh(m);
  std::printf("  shadow proxy: %zu vertices, %zu triangles, voxel %.4f, grid %dx%dx%d, %zu solid voxels, %zu filled inside, %zu floaters (%zu voxels), "
              "%s, volume %.4f; %.2f s\n",
              m.positions.size() / 3, m.indices.size() / 3, m.voxel, m.dims[0], m.dims[1], m.dims[2], m.solid, m.insideFilled, m.floaterPieces,
              m.floaterVoxels, c.closed ? "closed" : "NOT closed", c.volume, seconds(t));
  EXPECT(c.closed && c.volume > 0.0, "%s: the proxy is closed and faces outwards", path);
  if (obj) writeObj(m, obj);
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
  checkProxy();
  checkProxyThin();
  for (int i = 1; i < argc; ++i)
    if (std::strstr(argv[i], ".obj") == nullptr) {
      checkFile(argv[i]);
      checkFileProxy(argv[i], i + 1 < argc && std::strstr(argv[i + 1], ".obj") ? argv[i + 1] : nullptr);
    }
  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
