#pragma once

// A line-for-line C++ twin of plugins/unreal/VGS/Shaders/Private/VGSPacked.usf, the compute
// shader that evaluates a packed chunk (vgsb_acquire_packed) into the float splat records
// Epic's Gaussian Splat renderer reads. The shader cannot run without Unreal; this can, so
// tests/unreal_packed.cpp checks it against the frames vgsbridge evaluates on the CPU. A
// change to one is a change to the other.
//
// The shader itself is a port of the VGS editor's packed viewport (editor/viewport.cpp,
// packedRecord), which reads the same arrays.

#include "vgsbridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vgspacked {

struct Float3 { float x = 0, y = 0, z = 0; };
struct Float4 { float x = 0, y = 0, z = 0, w = 0; };

/** One record as the shader writes it, before the layout's bytes. */
struct Record {
  bool alive = false;
  Float3 position, scale;
  Float4 rotation; // xyzw, unit
  Float3 dc;       // the SH DC term, not the colour
  float opacity = 0;
  Float3 sh[15];
};

/** What the shader gets besides the chunk's buffers: the instant, and what the component adds. */
struct Instant {
  uint32_t frame = 0;
  float alpha = 0, normalized = 0;
  const float *basis = nullptr;
  // Intensity (VGSSplatLayout.h, Gain) and density (vgsbridge's framepack.h, keeps).
  float gain = 1, dcOffset = 0;
  uint32_t densityThreshold = 0xFFFFFFFFu; // a record stays when its hash is below this
  bool thinned = false;
  float densityExponent = 1;
};

inline float asFloat(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

inline Float3 unpackPosition(uint32_t wx, uint32_t wy, float lo, float hi) {
  const uint32_t z = (wx >> 1) & 0x1FFFFFu, y = (wx >> 22) | ((wy & 0x7FFu) << 10), x = wy >> 11;
  const float scale = (hi - lo) / 2097151.0f;
  return {float(x) * scale + lo, float(y) * scale + lo, float(z) * scale + lo};
}

// Smallest three, stored wxyz, returned xyzw; the low bit negates the whole quaternion.
inline Float4 unpackQuat(uint32_t w) {
  const uint32_t largest = w >> 30;
  const float k = 0.70710678118654752f;
  const float sx = (float((w >> 20) & 1023u) / 1023.0f * 2.0f - 1.0f) * k;
  const float sy = (float((w >> 10) & 1023u) / 1023.0f * 2.0f - 1.0f) * k;
  const float sz = (float((w >> 1) & 511u) / 511.0f * 2.0f - 1.0f) * k;
  const float o = std::sqrt(std::max(0.0f, 1.0f - (sx * sx + sy * sy + sz * sz)));
  Float4 q; // wxyz in x, y, z, w
  if (largest == 0u) q = {o, sx, sy, sz};
  else if (largest == 1u) q = {sx, o, sy, sz};
  else if (largest == 2u) q = {sx, sy, o, sz};
  else q = {sx, sy, sz, o};
  const float sign = (w & 1u) != 0u ? -1.0f : 1.0f;
  return {q.y * sign, q.z * sign, q.w * sign, q.x * sign};
}

// One of five 12-bit residual codes in a 64-bit word; codes 3 and 4 straddle the halves.
inline uint32_t residualCode(uint32_t wx, uint32_t wy, int k) {
  return k < 2 ? (wx >> uint32_t(12 * k)) & 4095u
       : k == 2 ? ((wx >> 24) | ((wy & 15u) << 8)) & 4095u
                : (wy >> uint32_t(12 * k - 32)) & 4095u;
}

inline const float *basisAt(const vgsb_packed_chunk &c, const Instant &at, int table, uint32_t i) {
  return at.basis + size_t(uint32_t(c.basis_offset[table]) + i) * 4;
}

// vgsbridge's framepack.h, keeps.
inline uint32_t densityHash(uint32_t record) {
  uint32_t x = record * 0x9E3779B1u;
  x ^= x >> 16;
  x *= 0x85EBCA6Bu;
  x ^= x >> 13;
  x *= 0xC2B2AE35u;
  x ^= x >> 16;
  return x;
}

inline Record evaluate(const vgsb_packed_chunk &c, const Instant &at, uint32_t index) {
  Record r;
  const uint32_t *A = c.splat_a + size_t(index) * 4, *B = c.splat_b + size_t(index) * 4,
                 *C = c.splat_c + size_t(index) * 4;
  const int frame = int(at.frame);
  if (frame < int(A[3] >> 24) || frame + 1 > int(C[3] >> 24))
    return r; // not alive at this instant
  if (at.thinned && densityHash(index) >= at.densityThreshold)
    return r;
  r.alive = true;

  int group = c.group_count - 1;
  for (int g = 0; g < c.group_count; ++g)
    if (float(index) < c.group_a[g * 4]) { group = g; break; }
  const uint32_t local = index - (group == 0 ? 0u : uint32_t(c.group_a[(group - 1) * 4]));
  const uint32_t flags = uint32_t(c.group_a[group * 4 + 1]);
  const float lo = c.group_a[group * 4 + 2], hi = c.group_a[group * 4 + 3];

  Float3 p;
  if (flags & 1u) {
    const uint32_t base = uint32_t(c.group_b[group * 4]) + local * uint32_t(c.samples);
    const uint32_t *w0 = c.position_samples + size_t(base + at.frame) * 2, *w1 = w0 + 2;
    const Float3 p0 = unpackPosition(w0[0], w0[1], lo, hi), p1 = unpackPosition(w1[0], w1[1], lo, hi);
    // Normalised chunk time, as the runtime does.
    p = {p0.x + (p1.x - p0.x) * at.normalized, p0.y + (p1.y - p0.y) * at.normalized,
         p0.z + (p1.z - p0.z) * at.normalized};
  } else {
    p = unpackPosition(A[0], A[1], lo, hi);
    const uint32_t terms = C[2] & 255u;
    for (uint32_t k = 0; k < 4 && k < terms; ++k) {
      const uint32_t *term = c.position_terms + size_t(C[0] + k) * 2;
      const float *b = basisAt(c, at, 0, term[0]);
      const float weight = asFloat(term[1]);
      p.x += b[0] * weight;
      p.y += b[1] * weight;
      p.z += b[2] * weight;
    }
  }

  Float4 q;
  if (flags & 2u) {
    const uint32_t base = uint32_t(c.group_b[group * 4 + 1]) + local * uint32_t(c.samples);
    const Float4 q0 = unpackQuat(c.rotation_samples[base + at.frame]);
    const Float4 q1 = unpackQuat(c.rotation_samples[base + at.frame + 1]);
    const float s = (q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w) < 0.0f ? -1.0f : 1.0f;
    q = {q0.x + (q1.x * s - q0.x) * at.alpha, q0.y + (q1.y * s - q0.y) * at.alpha,
         q0.z + (q1.z * s - q0.z) * at.alpha, q0.w + (q1.w * s - q0.w) * at.alpha};
  } else {
    const Float4 base = unpackQuat(A[2]);
    Float4 residual;
    const uint32_t terms = (C[2] >> 8) & 255u;
    for (uint32_t k = 0; k < 5 && k < terms; ++k) {
      const float *b = basisAt(c, at, 3, c.rotation_terms[C[1] + k]);
      residual.x += b[0];
      residual.y += b[1];
      residual.z += b[2];
      residual.w += b[3];
    }
    const float n2 = residual.x * residual.x + residual.y * residual.y + residual.z * residual.z +
                     residual.w * residual.w;
    if (n2 < 1e-12f) {
      q = base;
    } else {
      const float inv = 1.0f / std::sqrt(n2);
      residual = {residual.x * inv, residual.y * inv, residual.z * inv, residual.w * inv};
      // residual * base, xyzw
      const Float3 cr = {residual.y * base.z - residual.z * base.y, residual.z * base.x - residual.x * base.z,
                         residual.x * base.y - residual.y * base.x};
      q = {residual.w * base.x + base.w * residual.x + cr.x, residual.w * base.y + base.w * residual.y + cr.y,
           residual.w * base.z + base.w * residual.z + cr.z,
           residual.w * base.w - (residual.x * base.x + residual.y * base.y + residual.z * base.z)};
    }
  }
  const float length = std::max(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w), 1e-20f);
  r.rotation = {q.x / length, q.y / length, q.z / length, q.w / length};

  Float3 sh0 = {c.luts[(C[3] & 255u) * 2 + 1], c.luts[((C[3] >> 8) & 255u) * 2 + 1],
                c.luts[((C[3] >> 16) & 255u) * 2 + 1]};
  float opacity = 0;
  for (int k = 0; k < 5; ++k) {
    const float *b = basisAt(c, at, 1, uint32_t(k) * uint32_t(c.sh0_stride) + residualCode(B[0], B[1], k));
    sh0.x += b[0];
    sh0.y += b[1];
    sh0.z += b[2];
    opacity += basisAt(c, at, 2, uint32_t(k) * uint32_t(c.opacity_stride) + residualCode(B[2], B[3], k))[0];
  }
  // The optional per-splat opacity factor in the words' top nibbles: 2^(-code/16).
  const float factor = std::exp2(-float((B[3] >> 28) | ((B[1] >> 28) << 4)) / 16.0f);
  opacity = std::min(std::max(opacity, 0.0f), 1.0f) * factor;
  if (at.thinned)
    opacity = 1.0f - std::pow(std::max(0.0f, 1.0f - opacity), at.densityExponent);
  r.opacity = opacity;

  const float SmallestScale = 1e-6f;
  r.scale = {std::max(c.luts[(A[3] & 255u) * 2], SmallestScale),
             std::max(c.luts[((A[3] >> 8) & 255u) * 2], SmallestScale),
             std::max(c.luts[((A[3] >> 16) & 255u) * 2], SmallestScale)};
  r.position = p;
  r.dc = {sh0.x * at.gain + at.dcOffset, sh0.y * at.gain + at.dcOffset, sh0.z * at.gain + at.dcOffset};

  // One word per plane carries three coefficients' codebook indices.
  const uint32_t splats = uint32_t(c.splats);
  const uint32_t booksTemporal = uint32_t(c.sh_planes) * 3u * uint32_t(c.entries_static);
  for (int co = 0; co < c.sh_coefficients; ++co) {
    const uint32_t word = uint32_t(co / 3) * splats + index;
    const uint32_t shift = uint32_t(20 - 10 * (co % 3));
    const uint32_t si = (c.sh_static[word] >> shift) & 1023u, ti = (c.sh_temporal[word] >> shift) & 1023u;
    const float *s = c.books + size_t(uint32_t(co) * uint32_t(c.entries_static) + si) * 4;
    const float *t = c.books + size_t(booksTemporal + at.frame * uint32_t(c.sh_planes) * 3u * uint32_t(c.entries_temporal) +
                                      uint32_t(co) * uint32_t(c.entries_temporal) + ti) * 4;
    r.sh[co] = {(s[0] + t[0]) * at.gain, (s[1] + t[1]) * at.gain, (s[2] + t[2]) * at.gain};
  }
  return r;
}

} // namespace vgspacked
