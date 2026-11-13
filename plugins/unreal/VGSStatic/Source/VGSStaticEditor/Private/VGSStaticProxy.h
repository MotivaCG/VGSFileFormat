#pragma once

/*
 * A static splat character's shadow proxy: a closed triangle mesh of where its splats are
 * opaque. Unreal casts the character's shadows from it (from every light, as from any mesh),
 * and can collide with it, while the splats are what is seen.
 *
 * Steps 1 and 2 are PlayCanvas splat-transform's collision mesh (MIT licence; its
 * src/lib/gpu/gpu-voxelization.ts and src/lib/voxel/block-cleanup.ts), ported to the CPU on a
 * dense grid. The rest is this plugin's, for one closed, smooth mesh of a character rather
 * than the walls of a room:
 *   1. density: each voxel adds up its splats' opacity * exp(-d^2 / 2), d the Mahalanobis
 *      distance from a splat's centre to the voxel's point nearest that centre; the voxel is
 *      solid where 1 - exp(-sum) reaches the opacity cutoff (light through the splats, as
 *      Beer-Lambert). Unlike splat-transform, "nearest" is in the splat's own metric for an
 *      elongated splat: the nearest point in metres can lie off a thin splat that crosses
 *      the voxel, which then misses it;
 *   2. clean: a solid voxel with no solid face neighbour goes, an empty one with six is filled;
 *   3. floaters: pieces far smaller than the largest go;
 *   4. inside: what the outside cannot reach through empty voxels is solid, so the splats' shell
 *      becomes one surface instead of an outer and an inner one;
 *   5. surface: surface nets on the occupancy, the vertices placed where the occupancy blurred
 *      over 3x3x3 voxels crosses one half (no stair steps; a part one voxel thin, which the
 *      blur would erase, keeps its surface), then Taubin smoothing, which does not shrink it;
 *   6. inset: moved inwards along the vertex normals, so that the lit splats are not inside
 *      their own shadow caster (they would shadow themselves in blotches); no more than 45 %
 *      of the solid's thickness behind each vertex, so that thin parts do not turn inside out.
 *
 * Why not a TSDF from rendered depth (gs2mesh): that renders hundreds of views, twice, and
 * fuses each into the volume, for a millimetre surface to texture; a shadow caster needs one
 * look at each splat. A few hundred thousand splats take well under a second here.
 *
 * Plain C++, as VGSStaticPly.h: tests/unreal_static.cpp checks it without the engine.
 */

#include "VGSStaticPly.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace vgsstatic {

struct ProxySettings {
  float voxel = 0.01f;          // in the file's units: 1 cm for a PLY in metres
  float opacity = 0.3f;         // a voxel is solid from this opacity
  float inset = 0.04f;          // file units the surface is moved inwards (at most 45 % of the thickness)
  float floaterShare = 0.005f;  // pieces with fewer solid voxels than this share of the largest go
  int smoothing = 4;            // Taubin passes
  size_t maxVoxels = 24u << 20; // past this many the voxel grows (and the result says so)
  unsigned threads = 0;         // 0: every hardware thread
};

struct ProxyMesh {
  std::vector<float> positions;  // 3 per vertex, in the file's axes and units
  std::vector<uint32_t> indices; // 3 per triangle, counter-clockwise seen from outside (right handed)
  float voxel = 0.0f;            // the voxel used (settings.voxel, or larger past maxVoxels)
  int dims[3] = {0, 0, 0};
  size_t solid = 0;        // solid voxels after step 2, before the inside is filled
  size_t floaterPieces = 0, floaterVoxels = 0;
  size_t insideFilled = 0; // voxels step 4 made solid
};

namespace proxydetail {

inline void parallelFor(size_t n, unsigned threads, const std::function<void(size_t)> &f) {
  size_t t = threads ? threads : std::max(1u, std::thread::hardware_concurrency());
  t = std::min(t, n);
  if (t <= 1) {
    for (size_t i = 0; i < n; ++i) f(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  for (size_t k = 0; k < t; ++k)
    pool.emplace_back([&] {
      for (size_t i; (i = next.fetch_add(1)) < n;) f(i);
    });
  for (std::thread &th : pool) th.join();
}

struct Grid {
  int n[3] = {0, 0, 0};
  size_t at(int x, int y, int z) const { return (size_t(z) * size_t(n[1]) + size_t(y)) * size_t(n[0]) + size_t(x); }
  size_t size() const { return size_t(n[0]) * size_t(n[1]) * size_t(n[2]); }
};

// Occupancy flags (one byte a voxel).
constexpr uint8_t Solid = 1, Seen = 2, Outside = 4;

/* 6-connected flood from `seeds` through voxels where (flags & mask) == want, marking them
 * with `mark`. Returns how many it marked. */
inline size_t flood(std::vector<uint8_t> &v, const Grid &g, std::vector<size_t> &stack, uint8_t mask, uint8_t want, uint8_t mark) {
  size_t marked = 0;
  const size_t sx = 1, sy = size_t(g.n[0]), sz = size_t(g.n[0]) * size_t(g.n[1]);
  while (!stack.empty()) {
    const size_t i = stack.back();
    stack.pop_back();
    ++marked;
    const int x = int(i % sy), y = int((i / sy) % size_t(g.n[1])), z = int(i / sz);
    auto visit = [&](size_t j) {
      if ((v[j] & mask) == want) {
        v[j] |= mark;
        stack.push_back(j);
      }
    };
    if (x > 0) visit(i - sx);
    if (x + 1 < g.n[0]) visit(i + sx);
    if (y > 0) visit(i - sy);
    if (y + 1 < g.n[1]) visit(i + sy);
    if (z > 0) visit(i - sz);
    if (z + 1 < g.n[2]) visit(i + sz);
  }
  return marked;
}

} // namespace proxydetail

/* Builds the proxy. Returns false (and an empty mesh) when nothing in the splats is solid. */
inline bool buildProxy(const Splats &s, const ProxySettings &settings, ProxyMesh &out) {
  using namespace proxydetail;
  out = ProxyMesh();
  const float cutoff = std::min(std::max(settings.opacity, 0.01f), 0.99f);
  const float sigmaCut = -std::log(1.0f - cutoff); // the density that makes a voxel solid
  const float eps = 1.0f / 255.0f;                  // contributions below this are left out

  // ---- each splat: centre, world -> its scaled axes, opacity, reach
  struct G {
    float c[3];
    float m[9]; // rows: the splat's axes divided by its scales (world offset -> scaled local)
    float A[6]; // m^T m (xx, yy, zz, xy, xz, yz): d^2 = offset^T A offset
    float e[3]; // half extents of the box beyond which it adds less than eps
    float opacity, reach2;
    bool elongated; // far from round: its nearest point in a voxel is not the nearest in metres
  };
  std::vector<G> gs;
  gs.reserve(s.count);
  for (size_t i = 0; i < s.count; ++i) {
    const float a = s.radiance[i * 4 + 3];
    if (!(a > eps)) continue;
    const float *q = &s.rotations[i * 4];
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    // Columns of R are the splat's axes in the file's frame.
    const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                        2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                        2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
    G g;
    float sc[3];
    for (int k = 0; k < 3; ++k) sc[k] = std::max(s.scales[i * 3 + k], 1e-7f);
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) g.m[r * 3 + c] = R[c * 3 + r] / sc[r];
    const auto mtm = [&](int i, int j) { return g.m[i] * g.m[j] + g.m[3 + i] * g.m[3 + j] + g.m[6 + i] * g.m[6 + j]; };
    g.A[0] = mtm(0, 0), g.A[1] = mtm(1, 1), g.A[2] = mtm(2, 2), g.A[3] = mtm(0, 1), g.A[4] = mtm(0, 2), g.A[5] = mtm(1, 2);
    g.elongated = std::max(sc[0], std::max(sc[1], sc[2])) > 1.5f * std::min(sc[0], std::min(sc[1], sc[2]));
    g.opacity = a;
    g.reach2 = 2.0f * std::log(a / eps);
    const float reach = std::sqrt(g.reach2);
    for (int k = 0; k < 3; ++k) {
      g.c[k] = s.positions[i * 3 + k];
      const float r0 = R[k * 3] * sc[0], r1 = R[k * 3 + 1] * sc[1], r2 = R[k * 3 + 2] * sc[2];
      g.e[k] = reach * std::sqrt(r0 * r0 + r1 * r1 + r2 * r2);
    }
    gs.push_back(g);
  }
  if (gs.empty()) return false;

  // ---- the grid: around the splats that can make a voxel solid, give or take a few strays
  // (a splat far from the rest would stretch it; such floaters are dropped in step 3 anyway)
  float lo[3], hi[3];
  {
    // Each splat's own reach where it adds an eighth of what makes a voxel solid, not only
    // its centre: a long splat reaches well past it.
    std::vector<float> lows[3], highs[3];
    for (const G &g : gs)
      if (g.opacity * 8.0f > sigmaCut) {
        const float share = std::sqrt(2.0f * std::log(g.opacity * 8.0f / sigmaCut) / g.reach2);
        for (int k = 0; k < 3; ++k) {
          lows[k].push_back(g.c[k] - share * g.e[k]);
          highs[k].push_back(g.c[k] + share * g.e[k]);
        }
      }
    if (lows[0].empty()) return false;
    for (int k = 0; k < 3; ++k) {
      const size_t n = lows[k].size(), cut = n / 1000; // 0.1 % at each end
      std::nth_element(lows[k].begin(), lows[k].begin() + cut, lows[k].end());
      const float qlo = lows[k][cut];
      std::nth_element(highs[k].begin(), highs[k].begin() + (n - 1 - cut), highs[k].end());
      const float qhi = highs[k][n - 1 - cut];
      const float margin = 0.1f * (qhi - qlo) + 4.0f * settings.voxel;
      lo[k] = qlo - margin;
      hi[k] = qhi + margin;
    }
  }
  float voxel = std::max(settings.voxel, 1e-6f);
  const int pad = 3; // empty voxels all around: the surface is closed
  Grid grid;
  for (int attempt = 0; attempt < 8; ++attempt) {
    double count = 1.0;
    for (int k = 0; k < 3; ++k) {
      grid.n[k] = int(std::ceil((hi[k] - lo[k]) / voxel)) + 2 * pad;
      count *= double(grid.n[k]);
    }
    if (count <= double(settings.maxVoxels)) break;
    voxel *= float(std::cbrt(count / double(settings.maxVoxels))) * 1.01f;
  }
  float origin[3];
  for (int k = 0; k < 3; ++k) origin[k] = lo[k] - float(pad) * voxel;
  out.voxel = voxel;
  for (int k = 0; k < 3; ++k) out.dims[k] = grid.n[k];

  // ---- 1. density, in slabs of z so that threads never share a voxel
  const int slabDepth = 8;
  const int slabs = (grid.n[2] + slabDepth - 1) / slabDepth;
  std::vector<std::vector<uint32_t>> bySlab(static_cast<size_t>(slabs));
  for (size_t i = 0; i < gs.size(); ++i) {
    const G &g = gs[i];
    const int z0 = std::max(0, int(std::floor((g.c[2] - g.e[2] - origin[2]) / voxel)));
    const int z1 = std::min(grid.n[2] - 1, int(std::floor((g.c[2] + g.e[2] - origin[2]) / voxel)));
    for (int sl = z0 / slabDepth; z0 <= z1 && sl <= z1 / slabDepth; ++sl) bySlab[size_t(sl)].push_back(uint32_t(i));
  }
  std::vector<float> density(grid.size(), 0.0f);
  parallelFor(size_t(slabs), settings.threads, [&](size_t sl) {
    const int zs = int(sl) * slabDepth, ze = std::min(grid.n[2], zs + slabDepth) - 1;
    for (uint32_t gi : bySlab[sl]) {
      const G &g = gs[gi];
      int r0[3], r1[3];
      for (int k = 0; k < 3; ++k) {
        r0[k] = std::max(0, int(std::floor((g.c[k] - g.e[k] - origin[k]) / voxel)));
        r1[k] = std::min(grid.n[k] - 1, int(std::floor((g.c[k] + g.e[k] - origin[k]) / voxel)));
      }
      r0[2] = std::max(r0[2], zs);
      r1[2] = std::min(r1[2], ze);
      for (int z = r0[2]; z <= r1[2]; ++z) {
        const float vz = origin[2] + float(z) * voxel;
        const float dz = std::min(std::max(g.c[2], vz), vz + voxel) - g.c[2];
        for (int y = r0[1]; y <= r1[1]; ++y) {
          const float vy = origin[1] + float(y) * voxel;
          const float dy = std::min(std::max(g.c[1], vy), vy + voxel) - g.c[1];
          float *row = &density[grid.at(0, y, z)];
          for (int x = r0[0]; x <= r1[0]; ++x) {
            const float vx = origin[0] + float(x) * voxel;
            float d[3] = {std::min(std::max(g.c[0], vx), vx + voxel) - g.c[0], dy, dz};
            if (g.elongated) {
              // The voxel's point of highest density is its nearest to the centre in the
              // splat's own metric, not in metres: for a thin splat across the voxel they
              // differ, and the nearest in metres can miss it. Coordinate descent on the
              // convex quadratic d^T A d within the voxel (each step exact, then clamped).
              const float lo3[3] = {vx - g.c[0], origin[1] + float(y) * voxel - g.c[1], vz - g.c[2]};
              for (int sweep = 0; sweep < 3; ++sweep) {
                d[0] = std::min(std::max(-(g.A[3] * d[1] + g.A[4] * d[2]) / g.A[0], lo3[0]), lo3[0] + voxel);
                d[1] = std::min(std::max(-(g.A[3] * d[0] + g.A[5] * d[2]) / g.A[1], lo3[1]), lo3[1] + voxel);
                d[2] = std::min(std::max(-(g.A[4] * d[0] + g.A[5] * d[1]) / g.A[2], lo3[2]), lo3[2] + voxel);
              }
            }
            const float d2 = g.A[0] * d[0] * d[0] + g.A[1] * d[1] * d[1] + g.A[2] * d[2] * d[2] +
                             2.0f * (g.A[3] * d[0] * d[1] + g.A[4] * d[0] * d[2] + g.A[5] * d[1] * d[2]);
            if (d2 < g.reach2) row[x] += g.opacity * std::exp(-0.5f * d2);
          }
        }
      }
    }
  });
  bySlab.clear();
  gs.clear();
  gs.shrink_to_fit();

  std::vector<uint8_t> v(grid.size(), 0);
  for (int z = pad - 1; z <= grid.n[2] - pad; ++z)
    for (int y = pad - 1; y <= grid.n[1] - pad; ++y)
      for (int x = pad - 1; x <= grid.n[0] - pad; ++x) {
        const size_t i = grid.at(x, y, z);
        v[i] = density[i] >= sigmaCut ? Solid : 0;
      }

  // ---- 2. clean, against a snapshot as splat-transform does
  {
    const std::vector<uint8_t> before = v;
    const size_t sy = size_t(grid.n[0]), sz = sy * size_t(grid.n[1]);
    for (int z = 1; z + 1 < grid.n[2]; ++z)
      for (int y = 1; y + 1 < grid.n[1]; ++y)
        for (int x = 1; x + 1 < grid.n[0]; ++x) {
          const size_t i = grid.at(x, y, z);
          const int around = before[i - 1] + before[i + 1] + before[i - sy] + before[i + sy] + before[i - sz] + before[i + sz];
          if (before[i] && around == 0) v[i] = 0;
          else if (!before[i] && around == 6) v[i] = Solid;
        }
  }
  for (uint8_t f : v) out.solid += f;

  // ---- 3. floaters: pieces (6-connected) far smaller than the largest
  {
    struct Piece {
      size_t seed, size;
    };
    std::vector<Piece> pieces;
    std::vector<size_t> stack;
    size_t largest = 0;
    for (size_t i = 0; i < v.size(); ++i)
      if (v[i] == Solid) {
        v[i] |= Seen;
        stack.push_back(i);
        const size_t n = flood(v, grid, stack, Solid | Seen, Solid, Seen);
        pieces.push_back({i, n});
        largest = std::max(largest, n);
      }
    const double keep = double(largest) * double(settings.floaterShare);
    for (const Piece &p : pieces)
      if (double(p.size) < keep) {
        // Cleared by flooding again over what is still marked, unmarking as it goes.
        v[p.seed] = 0;
        stack.push_back(p.seed);
        const size_t sy = size_t(grid.n[0]), sz = sy * size_t(grid.n[1]);
        while (!stack.empty()) {
          const size_t i = stack.back();
          stack.pop_back();
          const int x = int(i % sy), y = int((i / sy) % size_t(grid.n[1])), z = int(i / sz);
          auto visit = [&](size_t j) {
            if (v[j] == (Solid | Seen)) {
              v[j] = 0;
              stack.push_back(j);
            }
          };
          if (x > 0) visit(i - 1);
          if (x + 1 < grid.n[0]) visit(i + 1);
          if (y > 0) visit(i - sy);
          if (y + 1 < grid.n[1]) visit(i + sy);
          if (z > 0) visit(i - sz);
          if (z + 1 < grid.n[2]) visit(i + sz);
        }
        ++out.floaterPieces;
        out.floaterVoxels += p.size;
      }
    if (largest == 0) return false;
  }

  // ---- 4. inside: flood the outside from the grid's border, the rest is solid
  {
    std::vector<size_t> stack;
    const size_t corner = grid.at(0, 0, 0); // in the empty padding
    v[corner] |= Outside;
    stack.push_back(corner);
    flood(v, grid, stack, Solid | Outside, 0, Outside);
    for (uint8_t &f : v) {
      if (!(f & (Solid | Outside))) {
        f |= Solid;
        ++out.insideFilled;
      }
      f &= Solid;
    }
  }

  // ---- 5. surface nets on the blurred occupancy ([1 2 1] / 4 along each axis)
  std::vector<float> &field = density; // reused
  {
    // v stays: it says inside or out; the blurred field only places the surface between.
    for (size_t i = 0; i < v.size(); ++i) field[i] = float(v[i]);
    std::vector<float> tmp(field.size(), 0.0f);
    const size_t stride[3] = {1, size_t(grid.n[0]), size_t(grid.n[0]) * size_t(grid.n[1])};
    for (int axis = 0; axis < 3; ++axis) {
      std::vector<float> &from = axis == 1 ? tmp : field;
      std::vector<float> &to = axis == 1 ? field : tmp;
      const size_t st = stride[axis];
      parallelFor(size_t(grid.n[2]), settings.threads, [&](size_t z) {
        for (int y = 0; y < grid.n[1]; ++y)
          for (int x = 0; x < grid.n[0]; ++x) {
            const int c[3] = {x, y, int(z)};
            const size_t i = grid.at(x, y, int(z));
            const float a = c[axis] > 0 ? from[i - st] : 0.0f;
            const float b = c[axis] + 1 < grid.n[axis] ? from[i + st] : 0.0f;
            to[i] = 0.25f * a + 0.5f * from[i] + 0.25f * b;
          }
      });
    }
    field.swap(tmp); // the third pass wrote into tmp
  }
  const float iso = 0.5f;
  const int cx = grid.n[0] - 1, cy = grid.n[1] - 1, cz = grid.n[2] - 1; // cells between voxel centres
  std::vector<int32_t> cellVertex(size_t(cx) * size_t(cy) * size_t(cz), -1);
  auto cellAt = [&](int x, int y, int z) { return (size_t(z) * size_t(cy) + size_t(y)) * size_t(cx) + size_t(x); };
  std::vector<float> &P = out.positions;
  static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
  static const int edge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (int z = 0; z < cz; ++z)
    for (int y = 0; y < cy; ++y)
      for (int x = 0; x < cx; ++x) {
        float f[8];
        bool solid[8];
        int inside = 0;
        for (int k = 0; k < 8; ++k) {
          const size_t i = grid.at(x + corner[k][0], y + corner[k][1], z + corner[k][2]);
          f[k] = field[i];
          solid[k] = v[i] != 0;
          inside += solid[k];
        }
        if (inside == 0 || inside == 8) continue;
        float sum[3] = {0, 0, 0};
        int crossings = 0;
        for (const auto &e : edge) {
          if (solid[e[0]] == solid[e[1]]) continue;
          // Where the blurred field crosses one half between them; a part one voxel thin,
          // which the blur leaves below one half, is kept, its surface halfway.
          const float a = f[e[0]], b = f[e[1]];
          const float t = (a > iso) != (b > iso) ? std::min(std::max((iso - a) / (b - a), 0.05f), 0.95f) : 0.5f;
          for (int k = 0; k < 3; ++k) sum[k] += float(corner[e[0]][k]) + t * float(corner[e[1]][k] - corner[e[0]][k]);
          ++crossings;
        }
        cellVertex[cellAt(x, y, z)] = int32_t(P.size() / 3);
        const int c[3] = {x, y, z};
        for (int k = 0; k < 3; ++k) P.push_back(origin[k] + (float(c[k]) + 0.5f + sum[k] / float(crossings)) * voxel);
      }
  // A quad across each edge between voxel centres where the inside ends, joining the four
  // cells around that edge, turned to face the outside.
  std::vector<uint32_t> &T = out.indices;
  auto quad = [&](size_t a, size_t b, size_t c, size_t d, bool flip) {
    int32_t q[4] = {cellVertex[a], cellVertex[b], cellVertex[c], cellVertex[d]};
    if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0) return;
    if (flip) std::swap(q[1], q[3]);
    auto dist2 = [&](int32_t i, int32_t j) {
      float r = 0;
      for (int k = 0; k < 3; ++k) r += (P[size_t(i) * 3 + k] - P[size_t(j) * 3 + k]) * (P[size_t(i) * 3 + k] - P[size_t(j) * 3 + k]);
      return r;
    };
    if (dist2(q[0], q[2]) <= dist2(q[1], q[3])) {
      T.insert(T.end(), {uint32_t(q[0]), uint32_t(q[1]), uint32_t(q[2]), uint32_t(q[0]), uint32_t(q[2]), uint32_t(q[3])});
    } else {
      T.insert(T.end(), {uint32_t(q[0]), uint32_t(q[1]), uint32_t(q[3]), uint32_t(q[1]), uint32_t(q[2]), uint32_t(q[3])});
    }
  };
  for (int z = 1; z < cz; ++z)
    for (int y = 1; y < cy; ++y)
      for (int x = 1; x < cx; ++x) {
        const bool in = v[grid.at(x, y, z)] != 0;
        // +x edge: cells (x, y-1..y, z-1..z), counter-clockwise seen from +x (y, then z)
        if (in != (v[grid.at(x + 1, y, z)] != 0))
          quad(cellAt(x, y - 1, z - 1), cellAt(x, y, z - 1), cellAt(x, y, z), cellAt(x, y - 1, z), !in);
        // +y edge: cells (x-1..x, y, z-1..z), counter-clockwise seen from +y (z, then x)
        if (in != (v[grid.at(x, y + 1, z)] != 0))
          quad(cellAt(x - 1, y, z - 1), cellAt(x - 1, y, z), cellAt(x, y, z), cellAt(x, y, z - 1), !in);
        // +z edge: cells (x-1..x, y-1..y, z), counter-clockwise seen from +z (x, then y)
        if (in != (v[grid.at(x, y, z + 1)] != 0))
          quad(cellAt(x - 1, y - 1, z), cellAt(x, y - 1, z), cellAt(x, y, z), cellAt(x - 1, y, z), !in);
      }
  std::vector<int32_t>().swap(cellVertex);
  if (T.empty()) {
    out.positions.clear();
    return false;
  }

  // ---- Taubin smoothing over each vertex's neighbours
  const size_t nv = P.size() / 3;
  std::vector<uint32_t> start(nv + 1, 0), nbr;
  {
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    pairs.reserve(T.size() * 2);
    for (size_t t = 0; t < T.size(); t += 3)
      for (int k = 0; k < 3; ++k) {
        const uint32_t a = T[t + size_t(k)], b = T[t + size_t((k + 1) % 3)];
        pairs.push_back({a, b});
        pairs.push_back({b, a});
      }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    nbr.reserve(pairs.size());
    for (const auto &p : pairs) {
      ++start[p.first + 1];
      nbr.push_back(p.second);
    }
    for (size_t i = 0; i < nv; ++i) start[i + 1] += start[i];
  }
  {
    std::vector<float> next(P.size());
    for (int pass = 0; pass < 2 * std::max(settings.smoothing, 0); ++pass) {
      const float w = (pass % 2 == 0) ? 0.5f : -0.53f;
      for (size_t i = 0; i < nv; ++i) {
        float avg[3] = {0, 0, 0};
        const uint32_t n = start[i + 1] - start[i];
        for (uint32_t j = start[i]; j < start[i + 1]; ++j)
          for (int k = 0; k < 3; ++k) avg[k] += P[size_t(nbr[j]) * 3 + k];
        for (int k = 0; k < 3; ++k) next[i * 3 + k] = n ? P[i * 3 + k] + w * (avg[k] / float(n) - P[i * 3 + k]) : P[i * 3 + k];
      }
      P.swap(next);
    }
  }

  // ---- 6. inset along the area-weighted vertex normals
  if (settings.inset > 0.0f) {
    std::vector<float> N(P.size(), 0.0f);
    for (size_t t = 0; t < T.size(); t += 3) {
      const float *a = &P[size_t(T[t]) * 3], *b = &P[size_t(T[t + 1]) * 3], *c = &P[size_t(T[t + 2]) * 3];
      const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      const float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
      for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j) N[size_t(T[t + size_t(k)]) * 3 + size_t(j)] += n[j];
    }
    for (size_t i = 0; i < nv; ++i) {
      const float l = std::sqrt(N[i * 3] * N[i * 3] + N[i * 3 + 1] * N[i * 3 + 1] + N[i * 3 + 2] * N[i * 3 + 2]);
      if (!(l > 0.0f)) continue;
      const float n[3] = {N[i * 3] / l, N[i * 3 + 1] / l, N[i * 3 + 2] / l};
      // No more than 45 % of how thick the solid is behind the vertex, measured along the
      // inward normal through the occupancy: a full inset would turn a part thinner than
      // twice the inset (a finger, a shoe's edge) inside out, its two sides crossing.
      float thickness = 0.0f;
      bool entered = false;
      const float step = 0.25f * voxel, limit = 2.5f * settings.inset + 2.0f * voxel;
      for (float t = 0.0f; t <= limit; t += step) {
        int c[3];
        bool inGrid = true;
        for (int k = 0; k < 3; ++k) {
          c[k] = int(std::floor((P[i * 3 + k] - t * n[k] - origin[k]) / voxel));
          inGrid = inGrid && c[k] >= 0 && c[k] < grid.n[k];
        }
        const bool solid = inGrid && v[grid.at(c[0], c[1], c[2])] != 0;
        if (solid) entered = true;
        else if (entered) break;
        thickness = t;
      }
      const float inset = std::min(settings.inset, 0.45f * thickness);
      for (int k = 0; k < 3; ++k) P[i * 3 + k] -= inset * n[k];
    }
  }
  return true;
}

} // namespace vgsstatic
