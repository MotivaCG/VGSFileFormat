// SPDX-License-Identifier: LicenseRef-VGS-Encoder-Proprietary
// Encoding-0 fitter extracted from encoder/tools/vgsplyseq.cpp,
// TemporalPlyEncoderV4. Input discovery and all-take storage stay in that CLI;
// this adapter takes one bounded chunk and uses the RD2 residual-rotation profile.
#include "plyfit.h"
#include "vgscodec.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>
#if defined(__AVX2__) || defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#endif
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif
namespace plyfit {
namespace {
using vgs::Bytes;using vgs::Error;
thread_local const Progress *progress=nullptr;
void checkpoint(const std::string &message="Fitting PLY trajectories") {if (progress && *progress) (*progress)(message);}
template<class Body> void parallelFor(size_t count,const Body &body) {
    checkpoint();
#pragma omp parallel for schedule(dynamic,1) num_threads(8) if(count>8)
    for (long long i=0;i<static_cast<long long>(count);++i) body(size_t(i));
}
uint16_t toHalf(float value) {
  uint32_t x;
  std::memcpy(&x, &value, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  x &= 0x7FFFFFFFu;
  if (x > 0x7F800000u || x >= 0x477FF000u)
    throw Error("value outside half precision");
  if (x >= 0x38800000u) {
    x += 0x0FFFu + ((x >> 13) & 1u); // round to nearest even
    return uint16_t(sign | ((x - 0x38000000u) >> 13));
  }
  return uint16_t(sign | uint32_t(std::nearbyint(std::fabs(value) * 16777216.0f)));
}

float fromHalf(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000u) << 16, exponent = (h >> 10) & 0x1Fu,
                 mantissa = h & 0x3FFu;
  float v;
  if (!exponent) {
    v = std::ldexp(float(mantissa), -24);
    return sign ? -v : v;
  }
  const uint32_t bits = sign | ((exponent == 31 ? 255 : exponent + 112) << 23) | (mantissa << 13);
  std::memcpy(&v, &bits, 4);
  return v;
}

float roundHalf(float v) { return fromHalf(toHalf(v)); }

// Smallest-three quaternion, wxyz in, as the reader unpacks it.
uint32_t packQuaternion(const float *q) {
  int largest = 0;
  float norm = 0;
  for (int i = 0; i < 4; ++i) {
    norm += q[i] * q[i];
    if (std::abs(q[i]) > std::abs(q[largest]))
      largest = i;
  }
  const float scale = (q[largest] < 0 ? -1.0f : 1.0f) / std::sqrt(std::max(norm, 1e-30f));
  uint32_t word = uint32_t(largest) << 30;
  for (int i = 0, j = 0; i < 4; ++i)
    if (i != largest) {
      const int bits = j == 2 ? 511 : 1023, shift = j == 0 ? 20 : j == 1 ? 10 : 1;
      const int v = int(std::lround((q[i] * scale * 1.41421356237f + 1) * .5f * float(bits)));
      word |= uint32_t(std::clamp(v, 0, bits)) << shift;
      ++j;
    }
  return word;
}

// The reader's unpacking, returned wxyz.
void unpackQuaternion(uint32_t word, float *wxyz) {
  static const float kMax[3] = {1023.0f, 1023.0f, 511.0f};
  static const int kShift[3] = {20, 10, 1};
  static const uint32_t kMask[3] = {1023u, 1023u, 511u};
  float kept[3];
  for (int j = 0; j < 3; ++j)
    kept[j] = (2.0f * float((word >> kShift[j]) & kMask[j]) / kMax[j] - 1.0f) *
              0.70710678118654752f;
  const float omitted =
      std::sqrt(std::max(0.0f, 1.0f - kept[0] * kept[0] - kept[1] * kept[1] - kept[2] * kept[2]));
  const int largest = int(word >> 30);
  const float sign = 1.0f - 2.0f * float(word & 1u);
  for (int c = 0, j = 0; c < 4; ++c)
    wxyz[c] = ((c == largest) ? omitted : kept[j++]) * sign;
}

// Hamilton product, wxyz.
void multiply(const float *a, const float *b, float *out) {
  out[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
  out[1] = a[0] * b[1] + b[0] * a[1] + (a[2] * b[3] - a[3] * b[2]);
  out[2] = a[0] * b[2] + b[0] * a[2] + (a[3] * b[1] - a[1] * b[3]);
  out[3] = a[0] * b[3] + b[0] * a[3] + (a[1] * b[2] - a[2] * b[1]);
}

void normalize4(float *q) {
  const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n < 1e-20f) {
    q[0] = 1;
    q[1] = q[2] = q[3] = 0;
    return;
  }
  for (int c = 0; c < 4; ++c)
    q[c] /= n;
}

float angleDegrees(const float *a, const float *b) {
  const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  return 2.0f * std::acos(std::min(1.0f, d)) * 57.2957795f;
}

// 21 bits per axis over one scalar range, as unpackPosition reads it.
struct PositionGrid {
  double lo = 0, hi = 1;
  uint64_t pack(const float *xyz) const {
    const double step = (hi - lo) / 2097151.0;
    uint64_t word = 0;
    for (int c = 0; c < 3; ++c) {
      const double q = std::clamp(std::round((double(xyz[c]) - lo) / step), 0.0, 2097151.0);
      word |= uint64_t(q) << (c == 0 ? 43 : c == 1 ? 22 : 1);
    }
    return word;
  }
  void unpack(uint64_t word, float *xyz) const {
    const double step = (hi - lo) / 2097151.0;
    xyz[0] = float(lo + double((word >> 43) & 0x1FFFFFull) * step);
    xyz[1] = float(lo + double((word >> 22) & 0x1FFFFFull) * step);
    xyz[2] = float(lo + double((word >> 1) & 0x1FFFFFull) * step);
  }
};

PositionGrid gridFor(double lo, double hi) {
  if (!(hi > lo))
    hi = lo + 1e-6;
  return {lo, hi};
}

// Atoms packed for the search kernel in panels of 16: [panel][dimension][16].
struct Panels {
  size_t count = 0, dims = 0;
  std::vector<float> data;
};

Panels pack(const float *atoms, size_t count, size_t dims) {
  Panels p;
  p.count = count;
  p.dims = dims;
  p.data.assign((count + 15) / 16 * dims * 16, 0.0f);
  for (size_t j = 0; j < count; ++j)
    for (size_t d = 0; d < dims; ++d)
      p.data[(j / 16) * dims * 16 + d * 16 + j % 16] = atoms[j * dims + d];
  return p;
}

constexpr size_t kQueries = 6; // per kernel call

// out[q][j] = dot(query q, atom j) for six queries and one panel.
#if defined(_MSC_VER) && defined(_M_X64)
// Keep AVX2/FMA inside an isolated function. The rest of the application retains
// its baseline CPU target, including machines whose OS does not save AVX state.
bool avx2Available() {
    int r[4];__cpuid(r,0);if (r[0]<7) return false;
    __cpuid(r,1);constexpr int flags=(1<<27)|(1<<28)|(1<<12);
    if ((r[2]&flags)!=flags || (_xgetbv(0)&6)!=6) return false;
    __cpuidex(r,7,0);return (r[1]&(1<<5))!=0;
}
__declspec(noinline) void dotAvx2(const float *q,size_t dims,const float *panel,float *out) {
  __m256 acc[kQueries][2];
  for (auto &a : acc)
    a[0] = a[1] = _mm256_setzero_ps();
  for (size_t d = 0; d < dims; ++d) {
    const __m256 p0 = _mm256_loadu_ps(panel + d * 16), p1 = _mm256_loadu_ps(panel + d * 16 + 8);
    for (size_t k = 0; k < kQueries; ++k) {
      const __m256 b = _mm256_broadcast_ss(q + k * dims + d);
      acc[k][0] = _mm256_fmadd_ps(b, p0, acc[k][0]);
      acc[k][1] = _mm256_fmadd_ps(b, p1, acc[k][1]);
    }
  }
  for (size_t k = 0; k < kQueries; ++k) {
    _mm256_storeu_ps(out + k * 16, acc[k][0]);
    _mm256_storeu_ps(out + k * 16 + 8, acc[k][1]);
  }
  _mm256_zeroupper();
}
#endif
void dot6x16(const float *q, size_t dims, const float *panel, float *out) {
#if defined(_MSC_VER) && defined(_M_X64)
  static const bool avx2=avx2Available();if (avx2) {dotAvx2(q,dims,panel,out);return;}
#endif
#if defined(__AVX2__)
  __m256 acc[kQueries][2];
  for (auto &a : acc)
    a[0] = a[1] = _mm256_setzero_ps();
  for (size_t d = 0; d < dims; ++d) {
    const __m256 p0 = _mm256_loadu_ps(panel + d * 16), p1 = _mm256_loadu_ps(panel + d * 16 + 8);
    for (size_t k = 0; k < kQueries; ++k) {
      const __m256 b = _mm256_broadcast_ss(q + k * dims + d);
      acc[k][0] = _mm256_fmadd_ps(b, p0, acc[k][0]);
      acc[k][1] = _mm256_fmadd_ps(b, p1, acc[k][1]);
    }
  }
  for (size_t k = 0; k < kQueries; ++k) {
    _mm256_storeu_ps(out + k * 16, acc[k][0]);
    _mm256_storeu_ps(out + k * 16 + 8, acc[k][1]);
  }
#elif defined(_M_X64) || defined(__SSE2__)
  __m128 acc[kQueries][4];
  for (auto &a:acc) for (auto &v:a) v=_mm_setzero_ps();
  for (size_t d=0;d<dims;++d) {
    const __m128 p[]={_mm_loadu_ps(panel+d*16),_mm_loadu_ps(panel+d*16+4),_mm_loadu_ps(panel+d*16+8),_mm_loadu_ps(panel+d*16+12)};
    for (size_t k=0;k<kQueries;++k) {const auto v=_mm_set1_ps(q[k*dims+d]);for (int b=0;b<4;++b) acc[k][b]=_mm_add_ps(acc[k][b],_mm_mul_ps(v,p[b]));}
  }
  for (size_t k=0;k<kQueries;++k) for (int b=0;b<4;++b) _mm_storeu_ps(out+k*16+b*4,acc[k][b]);
#else
  for (size_t k = 0; k < kQueries; ++k)
    for (size_t j = 0; j < 16; ++j) {
      float s = 0;
      for (size_t d = 0; d < dims; ++d)
        s += q[k * dims + d] * panel[d * 16 + j];
      out[k * 16 + j] = s;
    }
#endif
}

// For each query, the atom with the smallest dot(qa, a), or with `b` the largest
// dot(qa, a)^2 / dot(qb, b). Queries are row-major, qa with a.dims and qb with b.dims.
void search(const float *qa, const float *qb, size_t n, const Panels &a, const Panels *b,
            int32_t *best, float *bestScore) {
  const size_t da = a.dims, db = b ? b->dims : 0, panels = (a.count + 15) / 16;
  const size_t tile = std::max<size_t>(1, (256 * 1024) / ((da + db) * 64));
  constexpr size_t group = kQueries * 16;
  parallelFor((n + group - 1) / group, [&](size_t g) {
    const size_t first = g * group, count = std::min(group, n - first);
    const size_t blocks = (count + kQueries - 1) / kQueries;
    std::vector<float> la(blocks * kQueries * da, 0.0f), lb(blocks * kQueries * db, 0.0f);
    std::memcpy(la.data(), qa + first * da, count * da * 4);
    if (b)
      std::memcpy(lb.data(), qb + first * db, count * db * 4);
    float score[group], outA[kQueries * 16], outB[kQueries * 16];
    int32_t index[group];
    std::fill(score, score + group, b ? -1.0f : std::numeric_limits<float>::infinity());
    std::fill(index, index + group, 0);
    for (size_t t0 = 0; t0 < panels; t0 += tile) {
      const size_t t1 = std::min(panels, t0 + tile);
      for (size_t blk = 0; blk < blocks; ++blk)
        for (size_t p = t0; p < t1; ++p) {
          dot6x16(la.data() + blk * kQueries * da, da, a.data.data() + p * da * 16, outA);
          if (b)
            dot6x16(lb.data() + blk * kQueries * db, db, b->data.data() + p * db * 16, outB);
          const size_t valid = std::min<size_t>(16, a.count - p * 16);
          for (size_t k = 0; k < kQueries; ++k) {
            const size_t row = blk * kQueries + k;
            if (row >= count)
              break;
            for (size_t j = 0; j < valid; ++j) {
              float s = outA[k * 16 + j];
              if (b) {
                const float den = outB[k * 16 + j];
                s = den > 1e-12f ? s * s / den : 0.0f;
                if (s > score[row]) {
                  score[row] = s;
                  index[row] = int32_t(p * 16 + j);
                }
              } else if (s < score[row]) {
                score[row] = s;
                index[row] = int32_t(p * 16 + j);
              }
            }
          }
        }
    }
    for (size_t r = 0; r < count; ++r) {
      best[first + r] = index[r];
      if (bestScore)
        bestScore[first + r] = score[r];
    }
  });
}

// ---------------------------------------------------------------- weighted k-means

// Curves are rows x samples x channels with a weight per row and sample, applying to all
// of its channels. Distances only count weighted samples.
struct Curves {
  const float *x = nullptr, *w = nullptr;
  size_t samples = 0, channels = 0;
  size_t dims() const { return samples * channels; }
};

// Nearest centre for each listed row, and its weighted squared error.
void assign(const Curves &c, const uint32_t *rows, size_t count, const std::vector<float> &centres,
            size_t entries, int32_t *codes, float *errors) {
  const size_t S = c.samples, C = c.channels, D = S * C + S;
  std::vector<float> atoms(entries * D);
  for (size_t j = 0; j < entries; ++j)
    for (size_t s = 0; s < S; ++s) {
      float norm = 0;
      for (size_t k = 0; k < C; ++k) {
        const float v = centres[(j * S + s) * C + k];
        atoms[j * D + s * C + k] = v;
        norm += v * v;
      }
      atoms[j * D + S * C + s] = norm;
    }
  const Panels panels = pack(atoms.data(), entries, D);
  constexpr size_t batch = 131072;
  std::vector<float> queries, scores;
  for (size_t at = 0; at < count; at += batch) {
    const size_t m = std::min(batch, count - at);
    queries.assign(m * D, 0.0f);
    scores.resize(m);
    std::vector<float> constant(m, 0.0f);
    parallelFor(m, [&](size_t i) {
      const size_t r = rows ? rows[at + i] : at + i;
      float *q = &queries[i * D];
      for (size_t s = 0; s < S; ++s) {
        const float w = c.w[r * S + s];
        for (size_t k = 0; k < C; ++k) {
          const float v = c.x[(r * S + s) * C + k];
          q[s * C + k] = -2.0f * w * v;
          constant[i] += w * v * v;
        }
        q[S * C + s] = w;
      }
    });
    search(queries.data(), nullptr, m, panels, nullptr, codes + at, scores.data());
    if (errors)
      for (size_t i = 0; i < m; ++i)
        errors[at + i] = std::max(0.0f, constant[i] + scores[i]);
  }
}

std::vector<uint32_t> sample(const std::vector<uint32_t> &from, size_t count, std::mt19937_64 &random) {
  std::vector<uint32_t> out = from;
  if (out.size() > count) {
    for (size_t i = 0; i < count; ++i)
      std::swap(out[i], out[i + size_t(random() % (out.size() - i))]);
    out.resize(count);
  }
  return out;
}

// `entries` centres trained on `members`; with `zeroFirst` centre 0 stays zero, the code
// that adds nothing. Entries beyond the member count stay zero.
std::vector<float> kmeans(const Curves &c, const std::vector<uint32_t> &members, size_t entries,
                          int iterations, bool zeroFirst, std::mt19937_64 &random) {
  const size_t D = c.dims(), S = c.samples, C = c.channels;
  std::vector<float> centres(entries * D, 0.0f);
  const size_t first = zeroFirst ? 1 : 0;
  const size_t trained = std::min(entries, first + members.size());
  {
    const auto seeds = sample(members, trained - first, random);
    for (size_t j = first; j < trained; ++j)
      std::memcpy(&centres[j * D], c.x + size_t(seeds[j - first]) * D, D * 4);
  }
  if (trained <= first)
    return centres;
  std::vector<int32_t> codes(members.size());
  std::vector<float> errors(members.size());
  for (int it = 0; it < iterations; ++it) {
    assign(c, members.data(), members.size(), centres, trained, codes.data(), errors.data());
    std::vector<size_t> start(trained + 1, 0);
    for (int32_t code : codes)
      ++start[size_t(code) + 1];
    for (size_t j = 0; j < trained; ++j)
      start[j + 1] += start[j];
    std::vector<uint32_t> order(members.size());
    {
      std::vector<size_t> at(start.begin(), start.end() - 1);
      for (size_t i = 0; i < members.size(); ++i)
        order[at[size_t(codes[i])]++] = uint32_t(i);
    }
    std::vector<uint8_t> empty(trained, 0);
    parallelFor(trained - first, [&](size_t jj) {
      const size_t j = jj + first;
      if (start[j] == start[j + 1]) {
        empty[j] = 1;
        return;
      }
      std::vector<double> sum(D, 0.0), weight(S, 0.0);
      for (size_t q = start[j]; q < start[j + 1]; ++q) {
        const size_t r = members[order[q]];
        for (size_t s = 0; s < S; ++s) {
          const double w = c.w[r * S + s];
          if (!w)
            continue;
          weight[s] += w;
          for (size_t k = 0; k < C; ++k)
            sum[s * C + k] += w * c.x[(r * S + s) * C + k];
        }
      }
      for (size_t s = 0; s < S; ++s)
        if (weight[s] > 0)
          for (size_t k = 0; k < C; ++k)
            centres[(j * S + s) * C + k] = float(sum[s * C + k] / weight[s]);
    });
    // Empty centres restart on the worst-fitted members.
    std::vector<uint32_t> worst(members.size());
    std::iota(worst.begin(), worst.end(), 0u);
    size_t next = 0;
    bool sorted = false;
    for (size_t j = first; j < trained; ++j) {
      if (!empty[j])
        continue;
      if (!sorted) {
        std::sort(worst.begin(), worst.end(), [&](uint32_t a, uint32_t b) { return errors[a] > errors[b]; });
        sorted = true;
      }
      if (next < worst.size())
        std::memcpy(&centres[j * D], c.x + size_t(members[worst[next++]]) * D, D * 4);
    }
  }
  return centres;
}

// Residual VQ: `stages` codebooks of `entries`, each trained on what the ones before it
// left, applied to every row in turn. `residual` is updated in place.
struct Staged {
  std::vector<float> book;     // stages * entries x dims
  std::vector<uint16_t> codes; // rows x stages
};

Staged residualVq(std::vector<float> &residual, const float *weight, size_t rows, size_t samples,
                  size_t channels, size_t stages, size_t entries, size_t trainRows, int iterations,
                  std::mt19937_64 &random) {
  Curves c{residual.data(), weight, samples, channels};
  const size_t D = c.dims();
  Staged out;
  out.book.assign(stages * entries * D, 0.0f);
  out.codes.assign(rows * stages, 0);
  std::vector<uint32_t> all(rows);
  std::iota(all.begin(), all.end(), 0u);
  std::vector<int32_t> codes(rows);
  for (size_t k = 0; k < stages; ++k) {
    auto book = kmeans(c, sample(all, trainRows, random), entries, iterations, true, random);
    for (float &v : book)
      v = roundHalf(v);
    assign(c, nullptr, rows, book, entries, codes.data(), nullptr);
    parallelFor(rows, [&](size_t i) {
      out.codes[i * stages + k] = uint16_t(codes[i]);
      const float *e = &book[size_t(codes[i]) * D];
      for (size_t d = 0; d < D; ++d)
        residual[i * D + d] -= e[d];
    });
    std::copy(book.begin(), book.end(), out.book.begin() + std::ptrdiff_t(k * entries * D));
  }
  return out;
}

// 1D Lloyd quantiser: `levels` sorted values minimising squared error over `values`.
std::vector<float> lloyd(std::vector<float> values, size_t levels) {
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  std::vector<double> prefix(n + 1, 0.0);
  for (size_t i = 0; i < n; ++i)
    prefix[i + 1] = prefix[i] + values[i];
  std::vector<float> centre(levels);
  for (size_t j = 0; j < levels; ++j)
    centre[j] = n ? values[std::min(n - 1, (2 * j + 1) * n / (2 * levels))] : 0.0f;
  for (int it = 0; it < 30 && n; ++it) {
    std::sort(centre.begin(), centre.end());
    size_t lo = 0;
    for (size_t j = 0; j < levels; ++j) {
      const float edge = j + 1 < levels ? 0.5f * (centre[j] + centre[j + 1]) : std::numeric_limits<float>::infinity();
      const size_t hi = size_t(std::upper_bound(values.begin() + std::ptrdiff_t(lo), values.end(), edge) - values.begin());
      if (hi > lo)
        centre[j] = float((prefix[hi] - prefix[lo]) / double(hi - lo));
      lo = hi;
    }
  }
  std::sort(centre.begin(), centre.end());
  return centre;
}

uint8_t nearest(const std::vector<float> &sorted, float v) {
  const auto it = std::lower_bound(sorted.begin(), sorted.end(), v);
  size_t j = size_t(it - sorted.begin());
  if (j == sorted.size() || (j > 0 && v - sorted[j - 1] < sorted[j] - v))
    --j;
  return uint8_t(j);
}

// ---------------------------------------------------------------- positions

// p(s) = base + sum of up to four weighted atoms, each a 3D curve over the samples.
struct PositionFit {
  std::vector<float> atoms; // decoded, entries x samples x 3
  size_t entries = 0;
  PositionGrid trajectory, base;
  std::vector<std::array<float, 3>> bases; // before packing
  std::vector<uint64_t> baseWords;
  std::vector<std::array<uint32_t, 4>> terms; // sorted by atom index
  std::vector<uint8_t> rank;
};

// Least-squares base and weights for the chosen atoms; weights rounded to half, then the
// base refitted. Writes the residual and returns its weighted RMS.
float refit(const float *p, const float *w, size_t S, size_t k, const float *const *atoms,
            float *alpha, double *base, float *residual) {
  const size_t n = 3 + k;
  double m[7][7] = {}, rhs[7] = {};
  double W = 0;
  for (size_t s = 0; s < S; ++s) {
    const double ws = w[s];
    if (!ws)
      continue;
    W += ws;
    for (size_t c = 0; c < 3; ++c) {
      m[c][c] += ws;
      rhs[c] += ws * p[s * 3 + c];
    }
    for (size_t l = 0; l < k; ++l) {
      const float *a = atoms[l] + s * 3;
      for (size_t c = 0; c < 3; ++c) {
        m[c][3 + l] += ws * a[c];
        rhs[3 + l] += ws * double(p[s * 3 + c]) * a[c];
      }
      for (size_t q = 0; q <= l; ++q) {
        const float *b = atoms[q] + s * 3;
        m[3 + q][3 + l] += ws * (double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]);
      }
    }
  }
  for (size_t r = 0; r < n; ++r) // upper triangle mirrored
    for (size_t c = 0; c < r; ++c)
      m[r][c] = m[c][r];
  for (size_t l = 0; l < k; ++l)
    m[3 + l][3 + l] += 1e-9 * (1.0 + m[3 + l][3 + l]);
  // Gaussian elimination with partial pivoting.
  for (size_t col = 0; col < n; ++col) {
    size_t pivot = col;
    for (size_t r = col + 1; r < n; ++r)
      if (std::abs(m[r][col]) > std::abs(m[pivot][col]))
        pivot = r;
    std::swap(m[col], m[pivot]);
    std::swap(rhs[col], rhs[pivot]);
    if (std::abs(m[col][col]) < 1e-30)
      continue;
    for (size_t r = col + 1; r < n; ++r) {
      const double f = m[r][col] / m[col][col];
      for (size_t c = col; c < n; ++c)
        m[r][c] -= f * m[col][c];
      rhs[r] -= f * rhs[col];
    }
  }
  double x[7] = {};
  for (size_t r = n; r-- > 0;) {
    double v = rhs[r];
    for (size_t c = r + 1; c < n; ++c)
      v -= m[r][c] * x[c];
    x[r] = std::abs(m[r][r]) < 1e-30 ? 0.0 : v / m[r][r];
  }
  for (size_t l = 0; l < k; ++l)
    alpha[l] = roundHalf(float(std::clamp(x[3 + l], -60000.0, 60000.0)));
  double sum[3] = {};
  for (size_t s = 0; s < S; ++s) {
    for (size_t c = 0; c < 3; ++c) {
      float v = p[s * 3 + c];
      for (size_t l = 0; l < k; ++l)
        v -= alpha[l] * atoms[l][s * 3 + c];
      residual[s * 3 + c] = v;
      sum[c] += double(w[s]) * v;
    }
  }
  for (size_t c = 0; c < 3; ++c)
    base[c] = sum[c] / W;
  double error = 0;
  for (size_t s = 0; s < S; ++s)
    for (size_t c = 0; c < 3; ++c) {
      residual[s * 3 + c] = float(residual[s * 3 + c] - base[c]);
      error += double(w[s]) * residual[s * 3 + c] * residual[s * 3 + c];
    }
  return float(std::sqrt(error / W));
}

// Matching pursuit state for a set of records.
struct Pursuit {
  size_t S = 0;
  std::vector<float> residual;                   // rows x S x 3
  std::vector<std::array<const float *, 4>> chosen;
  std::vector<std::array<int32_t, 4>> index;
  std::vector<std::array<float, 4>> alpha;
  std::vector<std::array<double, 3>> base;
  std::vector<uint8_t> terms;
  std::vector<float> rms;
};

Pursuit startPursuit(const float *position, const float *weight, const std::vector<uint32_t> &rows, size_t S) {
  Pursuit st;
  st.S = S;
  const size_t n = rows.size();
  st.residual.resize(n * S * 3);
  st.chosen.assign(n, {});
  st.index.assign(n, {0, 0, 0, 0});
  st.alpha.assign(n, {0, 0, 0, 0});
  st.base.assign(n, {0, 0, 0});
  st.terms.assign(n, 0);
  st.rms.assign(n, 0);
  parallelFor(n, [&](size_t i) {
    const size_t r = rows[i];
    st.rms[i] = refit(position + r * S * 3, weight + r * S, S, 0, nullptr, nullptr, st.base[i].data(),
                      &st.residual[i * S * 3]);
  });
  return st;
}

// Panels for the pursuit score: the atoms, and their per-sample squared norms.
struct AtomPanels {
  Panels curves, norms;
};

AtomPanels atomPanels(const float *atoms, size_t entries, size_t S) {
  std::vector<float> norms(entries * S);
  for (size_t j = 0; j < entries; ++j)
    for (size_t s = 0; s < S; ++s) {
      const float *a = atoms + (j * S + s) * 3;
      norms[j * S + s] = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    }
  return {pack(atoms, entries, S * 3), pack(norms.data(), entries, S)};
}

// One step for the records in `active` (indices into the pursuit): the atom explaining
// most of each residual joins, then base and weights are refitted.
void pursue(Pursuit &st, const float *position, const float *weight, const std::vector<uint32_t> &rows,
            const std::vector<uint32_t> &active, const float *atoms, const AtomPanels &panels,
            int32_t indexOffset) {
  const size_t S = st.S, m = active.size();
  constexpr size_t batch = 131072;
  std::vector<float> qa, qb;
  std::vector<int32_t> best;
  for (size_t at = 0; at < m; at += batch) {
    const size_t count = std::min(batch, m - at);
    qa.resize(count * S * 3);
    qb.resize(count * S);
    best.resize(count);
    parallelFor(count, [&](size_t i) {
      const size_t a = active[at + i], r = rows[a];
      for (size_t s = 0; s < S; ++s) {
        const float w = weight[r * S + s];
        qb[i * S + s] = w;
        for (size_t c = 0; c < 3; ++c)
          qa[(i * S + s) * 3 + c] = w * st.residual[(a * S + s) * 3 + c];
      }
    });
    search(qa.data(), qb.data(), count, panels.curves, &panels.norms, best.data(), nullptr);
    parallelFor(count, [&](size_t i) {
      const size_t a = active[at + i], r = rows[a], k = st.terms[a];
      const int32_t id = best[i] + indexOffset;
      for (size_t l = 0; l < k; ++l)
        if (st.index[a][l] == id)
          return; // nothing new explains it
      st.chosen[a][k] = atoms + size_t(best[i]) * S * 3;
      st.index[a][k] = id;
      st.terms[a] = uint8_t(k + 1);
      st.rms[a] = refit(position + r * S * 3, weight + r * S, S, k + 1, st.chosen[a].data(),
                        st.alpha[a].data(), st.base[a].data(), &st.residual[a * S * 3]);
    });
  }
}

// Gain-shape atoms: unit-norm curves clustered by correlation, sign included.
std::vector<float> trainAtoms(const std::vector<float> &shapes, const float *weight,
                              const std::vector<uint32_t> &rows, size_t S, size_t entries,
                              int iterations, std::mt19937_64 &random) {
  const size_t n = rows.size(), D = S * 3;
  entries = std::min(entries, n);
  std::vector<float> atoms(entries * D);
  {
    std::vector<uint32_t> all(n);
    std::iota(all.begin(), all.end(), 0u);
    const auto seeds = sample(all, entries, random);
    for (size_t j = 0; j < entries; ++j)
      std::memcpy(&atoms[j * D], &shapes[size_t(seeds[j]) * D], D * 4);
  }
  std::vector<float> qa(n * D), qb(n * S);
  parallelFor(n, [&](size_t i) {
    for (size_t s = 0; s < S; ++s) {
      const float w = weight[size_t(rows[i]) * S + s];
      qb[i * S + s] = w;
      for (size_t c = 0; c < 3; ++c)
        qa[(i * S + s) * 3 + c] = w * shapes[(i * S + s) * 3 + c];
    }
  });
  std::vector<int32_t> best(n);
  std::vector<float> score(n);
  for (int it = 0; it < iterations; ++it) {
    const AtomPanels panels = atomPanels(atoms.data(), entries, S);
    search(qa.data(), qb.data(), n, panels.curves, &panels.norms, best.data(), score.data());
    std::vector<std::vector<uint32_t>> members(entries);
    for (size_t i = 0; i < n; ++i)
      members[size_t(best[i])].push_back(uint32_t(i));
    std::vector<uint8_t> empty(entries, 0);
    parallelFor(entries, [&](size_t j) {
      if (members[j].empty()) {
        empty[j] = 1;
        return;
      }
      std::vector<double> sum(D, 0.0), count(S, 0.0);
      const float *a = &atoms[j * D];
      for (uint32_t i : members[j]) {
        double dot = 0;
        for (size_t d = 0; d < D; ++d)
          dot += double(qa[i * D + d]) * a[d];
        const double sign = dot < 0 ? -1.0 : 1.0;
        for (size_t s = 0; s < S; ++s) {
          count[s] += qb[i * S + s];
          for (size_t c = 0; c < 3; ++c)
            sum[s * 3 + c] += sign * qa[(i * S + s) * 3 + c];
        }
      }
      std::vector<float> atom(D, 0.0f);
      int lastSeen = -1;
      for (size_t s = 0; s < S; ++s)
        if (count[s] > 0) {
          for (size_t c = 0; c < 3; ++c)
            atom[s * 3 + c] = float(sum[s * 3 + c] / count[s]);
          if (lastSeen < 0)
            for (size_t e = 0; e < s; ++e)
              std::copy(&atom[s * 3], &atom[s * 3] + 3, &atom[e * 3]);
          lastSeen = int(s);
        } else if (lastSeen >= 0)
          std::copy(&atom[size_t(lastSeen) * 3], &atom[size_t(lastSeen) * 3] + 3, &atom[s * 3]);
      // Zero mean where the members were observed, unit norm over the whole chunk.
      double total = 0, mean[3] = {};
      for (size_t s = 0; s < S; ++s) {
        total += count[s];
        for (size_t c = 0; c < 3; ++c)
          mean[c] += count[s] * atom[s * 3 + c];
      }
      double norm = 0;
      for (size_t s = 0; s < S; ++s)
        for (size_t c = 0; c < 3; ++c) {
          atom[s * 3 + c] = float(atom[s * 3 + c] - mean[c] / total);
          norm += double(atom[s * 3 + c]) * atom[s * 3 + c];
        }
      if (norm < 1e-20) {
        empty[j] = 1;
        return;
      }
      for (size_t d = 0; d < D; ++d)
        atoms[j * D + d] = float(atom[d] / std::sqrt(norm));
    });
    // Empty atoms restart on the worst-explained shapes.
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    bool sorted = false;
    size_t next = 0;
    for (size_t j = 0; j < entries; ++j) {
      if (!empty[j])
        continue;
      if (!sorted) {
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return score[a] < score[b]; });
        sorted = true;
      }
      std::memcpy(&atoms[j * D], &shapes[size_t(order[next++ % n]) * D], D * 4);
    }
  }
  return atoms;
}

// The quantised position dictionary alone, trained on these records.
PositionFit trainPositionAtoms(const Tracks &t, size_t entries, float tolerance, std::mt19937_64 &random) {
  const size_t S = t.samples, n = t.count, D = S * 3;
  PositionFit fit;
  std::vector<uint32_t> all(n);
  std::iota(all.begin(), all.end(), 0u);

  // Dictionary: stage by stage, each trained on the normalised residuals the atoms before
  // it leave on a subset of the records that still miss the tolerance.
  std::vector<std::vector<float>> stageAtoms;
  {
    std::vector<uint32_t> moving;
    {
      Pursuit probe = startPursuit(t.position.data(), t.weight.data(), all, S);
      for (size_t i = 0; i < n; ++i)
        if (probe.rms[i] > tolerance)
          moving.push_back(uint32_t(i));
    }
    const std::vector<uint32_t> rows = sample(moving, std::min<size_t>(262144, std::max<size_t>(65536, 24 * entries)), random);
    Pursuit st = startPursuit(t.position.data(), t.weight.data(), rows, S);
    const size_t budget[4] = {entries / 2, entries / 4, entries / 8, entries - entries / 2 - entries / 4 - entries / 8};
    int32_t offset = 0;
    for (size_t stage = 0; stage < 4; ++stage) {
      std::vector<uint32_t> open, openRows;
      for (size_t i = 0; i < rows.size(); ++i)
        if (st.rms[i] > tolerance && st.terms[i] == stage) {
          open.push_back(uint32_t(i));
          openRows.push_back(rows[i]);
        }
      if (open.empty() || !budget[stage])
        break;
      // Residuals scaled to unit weighted norm.
      std::vector<float> shapes(open.size() * D);
      parallelFor(open.size(), [&](size_t i) {
        const size_t a = open[i];
        float weight = 0;
        for (size_t s = 0; s < S; ++s)
          weight += t.weight[size_t(openRows[i]) * S + s];
        const float scale = 1.0f / std::max(st.rms[a] * std::sqrt(weight), 1e-12f);
        for (size_t d = 0; d < D; ++d)
          shapes[i * D + d] = st.residual[a * D + d] * scale;
      });
      stageAtoms.push_back(trainAtoms(shapes, t.weight.data(), openRows, S, budget[stage], 6, random));
      const auto &atoms = stageAtoms.back();
      pursue(st, t.position.data(), t.weight.data(), rows, open, atoms.data(),
             atomPanels(atoms.data(), atoms.size() / D, S), offset);
      offset += int32_t(atoms.size() / D);
    }
  }

  // Quantise the union, then fit every record against what the reader will decode.
  std::vector<float> atoms;
  for (const auto &a : stageAtoms)
    atoms.insert(atoms.end(), a.begin(), a.end());
  if (atoms.empty())
    atoms.assign(D, 0.0f); // nothing moves: one atom, used with weight zero
  fit.entries = atoms.size() / D;
  const auto range = std::minmax_element(atoms.begin(), atoms.end());
  fit.trajectory = gridFor(*range.first, *range.second);
  fit.atoms.resize(atoms.size());
  for (size_t s = 0; s < atoms.size() / 3; ++s)
    fit.trajectory.unpack(fit.trajectory.pack(&atoms[s * 3]), &fit.atoms[s * 3]);
  return fit;
}

// Every record fitted against the dictionary in `fit`, as the reader will decode it.
// With `reuse`, record i keeps the fit of the previous record reuse[i] when that is not
// negative; the dictionary must be the one that fit was made with.
PositionFit fitPositions(const Tracks &t, float tolerance, PositionFit fit,
                         const std::vector<int64_t> &reuse = {}) {
  const size_t S = t.samples, n = t.count;
  std::vector<uint32_t> rows;
  for (size_t i = 0; i < n; ++i)
    if (reuse.empty() || reuse[i] < 0)
      rows.push_back(uint32_t(i));
  const size_t m = rows.size();
  Pursuit st = startPursuit(t.position.data(), t.weight.data(), rows, S);
  const AtomPanels panels = atomPanels(fit.atoms.data(), fit.entries, S);
  for (size_t step = 0; step < 4; ++step) {
    std::vector<uint32_t> active;
    for (size_t i = 0; i < m; ++i)
      if (st.terms[i] == step && (st.rms[i] > tolerance || step == 0))
        active.push_back(uint32_t(i));
    if (active.empty())
      break;
    if (step == 0) {
      // Records already inside the tolerance still need one term: atom 0, weight 0.
      std::vector<uint32_t> needed;
      for (uint32_t i : active)
        if (st.rms[i] > tolerance)
          needed.push_back(i);
        else {
          st.terms[i] = 1;
          st.index[i][0] = 0;
        }
      active.swap(needed);
    }
    pursue(st, t.position.data(), t.weight.data(), rows, active, fit.atoms.data(), panels, 0);
  }

  // Packed terms, reused ones copied over.
  const PositionFit previous = reuse.empty() ? PositionFit() : fit;
  fit.bases.resize(n);
  fit.terms.resize(n);
  fit.rank.resize(n);
  for (size_t i = 0; i < reuse.size(); ++i)
    if (reuse[i] >= 0) {
      fit.bases[i] = previous.bases[size_t(reuse[i])];
      fit.terms[i] = previous.terms[size_t(reuse[i])];
      fit.rank[i] = previous.rank[size_t(reuse[i])];
    }
  for (size_t q = 0; q < m; ++q) {
    const size_t i = rows[q];
    fit.bases[i] = {float(st.base[q][0]), float(st.base[q][1]), float(st.base[q][2])};
    const size_t k = std::max<size_t>(1, st.terms[q]);
    std::array<std::pair<int32_t, float>, 4> pairs{};
    for (size_t l = 0; l < k; ++l)
      pairs[l] = {st.index[q][l], st.terms[q] ? st.alpha[q][l] : 0.0f};
    std::sort(pairs.begin(), pairs.begin() + std::ptrdiff_t(k));
    for (size_t l = 0; l < k; ++l)
      fit.terms[i][l] = uint32_t(pairs[l].first) | (uint32_t(toHalf(pairs[l].second)) << 16);
    fit.rank[i] = uint8_t(k);
  }

  // Bases on one grid for the whole chunk.
  float lo = std::numeric_limits<float>::max(), hi = -lo;
  for (const auto &b : fit.bases)
    for (float v : b) {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  fit.base = gridFor(lo, hi);
  fit.baseWords.resize(n);
  for (size_t i = 0; i < n; ++i)
    fit.baseWords[i] = fit.base.pack(fit.bases[i].data());
  return fit;
}

struct RotationFit {
  std::vector<float> lut; // 256 steps, sorted
  size_t entries = 0;
  std::vector<uint16_t> initial; // entries x 4 half, wxyz
  std::vector<uint8_t> deltas;   // entries x intervals x 4
  std::vector<uint32_t> baseWords;
  std::vector<std::array<uint16_t, 5>> terms;
  std::vector<uint8_t> rank;
};

// Steps from 1e-4 to 2 in geometric progression, both signs, and zero.
std::vector<float> stepTable() {
  std::vector<float> lut;
  const double ratio = std::pow(2.0 / 1e-4, 1.0 / 126.0);
  for (int k = 126; k >= 0; --k)
    lut.push_back(-float(1e-4 * std::pow(ratio, k)));
  lut.push_back(0.0f);
  for (int k = 0; k <= 127; ++k)
    lut.push_back(float(1e-4 * std::pow(ratio, k)));
  return lut;
}

// Replaces each curve with what the reader integrates from its stored steps.
void quantiseRotations(std::vector<float> &book, size_t entries, size_t S, const std::vector<float> &lut,
                       uint16_t *initial, uint8_t *deltas) {
  parallelFor(entries, [&](size_t j) {
    float *curve = &book[j * S * 4];
    float acc[4];
    for (size_t c = 0; c < 4; ++c) {
      const uint16_t h = toHalf(std::clamp(curve[c], -2.0f, 2.0f));
      if (initial)
        initial[j * 4 + c] = h;
      acc[c] = fromHalf(h);
      curve[c] = acc[c];
    }
    for (size_t s = 1; s < S; ++s)
      for (size_t c = 0; c < 4; ++c) {
        const uint8_t index = nearest(lut, curve[s * 4 + c] - acc[c]);
        if (deltas)
          deltas[(j * (S - 1) + s - 1) * 4 + c] = index;
        acc[c] += lut[index];
        curve[s * 4 + c] = roundHalf(acc[c]);
      }
  });
}

RotationFit fitRotations(const Tracks &t, size_t entries, float toleranceDegrees, std::mt19937_64 &random) {
  const size_t S = t.samples, n = t.count, D = S * 4, stages = 5;
  RotationFit fit;
  fit.lut = stepTable();
  const size_t perStage = entries / stages;
  fit.entries = perStage * stages;
  fit.baseWords.resize(n);
  fit.terms.assign(n, {});
  fit.rank.assign(n, 0);

  // Base: the weighted mean rotation. Target: the rotation relative to it, q = r * base.
  std::vector<float> base(n * 4), target(n * D), residual(n * D);
  parallelFor(n, [&](size_t i) {
    float mean[4] = {};
    for (size_t s = 0; s < S; ++s)
      for (size_t c = 0; c < 4; ++c)
        mean[c] += t.weight[i * S + s] * t.rotation[(i * S + s) * 4 + c];
    normalize4(mean);
    fit.baseWords[i] = packQuaternion(mean);
    float b[4];
    unpackQuaternion(fit.baseWords[i], b);
    std::copy(b, b + 4, &base[i * 4]);
    const float inverse[4] = {b[0], -b[1], -b[2], -b[3]};
    for (size_t s = 0; s < S; ++s) {
      float *r = &target[(i * S + s) * 4];
      multiply(&t.rotation[(i * S + s) * 4], inverse, r);
      if (r[0] < 0)
        for (size_t c = 0; c < 4; ++c)
          r[c] = -r[c];
    }
  });
  residual = target;
  fit.initial.assign(fit.entries * 4, 0);
  fit.deltas.assign(fit.entries * (S - 1) * 4, nearest(fit.lut, 0.0f));
  const auto error = [&](size_t i) {
    float worst = 0;
    for (size_t s = 0; s < S; ++s) {
      if (!t.weight[i * S + s])
        continue;
      float sum[4], q[4];
      for (size_t c = 0; c < 4; ++c)
        sum[c] = target[(i * S + s) * 4 + c] - residual[(i * S + s) * 4 + c];
      if (sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2] + sum[3] * sum[3] < 1e-12f)
        std::copy(&base[i * 4], &base[i * 4] + 4, q);
      else {
        normalize4(sum);
        multiply(sum, &base[i * 4], q);
        normalize4(q);
      }
      worst = std::max(worst, angleDegrees(q, &t.rotation[(i * S + s) * 4]));
    }
    return worst;
  };

  Curves curves{residual.data(), t.weight.data(), S, 4};
  std::vector<uint32_t> open(n);
  std::iota(open.begin(), open.end(), 0u);
  for (size_t k = 0; k < stages && !open.empty(); ++k) {
    auto book = kmeans(curves, sample(open, std::min<size_t>(262144, 48 * perStage), random), perStage, 6, k > 0, random);
    quantiseRotations(book, perStage, S, fit.lut, &fit.initial[k * perStage * 4],
                      &fit.deltas[k * perStage * (S - 1) * 4]);
    std::vector<int32_t> codes(open.size());
    assign(curves, open.data(), open.size(), book, perStage, codes.data(), nullptr);
    std::vector<uint8_t> done(open.size(), 0);
    parallelFor(open.size(), [&](size_t q) {
      const size_t i = open[q];
      const float *e = &book[size_t(codes[q]) * D];
      for (size_t d = 0; d < D; ++d)
        residual[i * D + d] -= e[d];
      fit.terms[i][k] = uint16_t(k * perStage + size_t(codes[q]));
      fit.rank[i] = uint8_t(k + 1);
      done[q] = k + 1 == stages || error(i) < toleranceDegrees;
    });
    std::vector<uint32_t> next;
    for (size_t q = 0; q < open.size(); ++q)
      if (!done[q])
        next.push_back(open[q]);
    open.swap(next);
  }
  return fit;
}

// ---------------------------------------------------------------- one chunk

vgs::Spec spec(uint32_t kind, uint32_t family, uint64_t rows, std::vector<int> fields, uint32_t width = 0,
               uint64_t samples = 0, uint64_t intervals = 0, uint64_t entries = 0) {
  vgs::Spec s;
  s.kind = kind;
  s.family = family;
  s.rows = rows;
  s.fields = std::move(fields);
  s.width = width;
  s.samples = samples;
  s.intervals = intervals;
  s.entries = entries;
  return s;
}

const std::vector<int> kPos64 = {1, 21, 21, 21}, kRq64 = {12, 12, 12, 12, 12, 4},
                       kQuat32 = {1, 9, 10, 10, 2}, kSh32 = {10, 10, 10, 2};

template <class T> Bytes bytes(const std::vector<T> &v) {
  Bytes b(v.size() * sizeof(T));
  if (!b.empty())
    std::memcpy(b.data(), v.data(), b.size());
  return b;
}

void add(vgs::DecodedChunk &chunk, uint32_t attribute, uint32_t group, Bytes data, vgs::Spec s) {
  vgs::Page p;
  p.attribute = attribute;
  p.group = group;
  p.totalRows = s.rows;
  p.decodedSize = data.size();
  p.spec = std::move(s);
  if (mgs::attributeSize(p.spec) != data.size())
    throw Error(std::string("array shape mismatch: ") + vgs::attributeName(attribute));
  chunk.pages.push_back({std::move(p), std::move(data)});
}

// Uninterpreted LUTs and rank tables are byte arrays.
vgs::Spec byteSpec(size_t size) { return spec(1, 0, size, {0}); }

uint64_t stageWord(const uint16_t *codes) {
  uint64_t word = 0;
  for (int k = 0; k < 5; ++k)
    word |= uint64_t(codes[k]) << (12 * k);
  return word;
}

struct Settings {size_t positionEntries=16384,sh0Entries=5120,opacityEntries=20480,shEntries=1024,rotationEntries=10240;float positionTolerance=.0005f,rotationTolerance=2.f;bool residualRotation=true;};
Result encode(Tracks t,const std::vector<Record> &records,size_t K) {
    Settings set;const size_t I=t.samples-1,S=t.samples;
    set.positionEntries=std::min(set.positionEntries,std::max<size_t>(8,t.count));
    set.sh0Entries=5*std::min<size_t>(1024,t.count);
    set.opacityEntries=5*std::min<size_t>(4096,t.count);
    set.rotationEntries=5*std::min<size_t>(2048,t.count);
    std::mt19937_64 random(20260928);
    auto position=fitPositions(t,set.positionTolerance,trainPositionAtoms(t,set.positionEntries,set.positionTolerance,random));
    checkpoint("Positions fitted; fitting colour");
  const size_t n = t.count;
  const size_t trainScale = std::min<size_t>(262144, n);

  // Scale: one 256-entry table for every axis, fitted in log space.
  const std::vector<float> scaleLevels = lloyd(t.scale, 256);
  std::vector<float> scaleLut(256);
  for (size_t j = 0; j < 256; ++j)
    scaleLut[j] = std::exp(scaleLevels[j]);
  std::vector<uint8_t> scaleIndex(n * 3);
  for (size_t i = 0; i < n * 3; ++i)
    scaleIndex[i] = nearest(scaleLevels, t.scale[i]);

  // Colour: a per-channel base from a 256-entry table, then five staged residual curves.
  std::vector<float> meanColor(n * 3, 0.0f);
  parallelFor(n, [&](size_t i) {
    float w = 0;
    for (size_t s = 0; s < S; ++s) {
      w += t.weight[i * S + s];
      for (size_t c = 0; c < 3; ++c)
        meanColor[i * 3 + c] += t.weight[i * S + s] * t.color[(i * S + s) * 3 + c];
    }
    for (size_t c = 0; c < 3; ++c)
      meanColor[i * 3 + c] /= w;
  });
  std::vector<float> colorLevels = lloyd(meanColor, 256);
  for (float &v : colorLevels)
    v = roundHalf(v);
  std::sort(colorLevels.begin(), colorLevels.end());
  std::vector<uint8_t> colorBase(n * 3);
  std::vector<float> colorResidual = t.color;
  parallelFor(n, [&](size_t i) {
    for (size_t c = 0; c < 3; ++c) {
      colorBase[i * 3 + c] = nearest(colorLevels, meanColor[i * 3 + c]);
      for (size_t s = 0; s < S; ++s)
        colorResidual[(i * S + s) * 3 + c] -= colorLevels[colorBase[i * 3 + c]];
    }
  });
  const size_t sh0Stage = set.sh0Entries / 5;
  const Staged sh0 = residualVq(colorResidual, t.weight.data(), n, S, 3, 5, sh0Stage,
                                std::min(trainScale, 48 * sh0Stage), 8, random);

  // Opacity: five staged curves summed and clamped, with no base.
  std::vector<float> opacityResidual = t.opacity;
  const size_t opacityStage = set.opacityEntries / 5;
  const Staged opacity = residualVq(opacityResidual, t.weight.data(), n, S, 1, 5, opacityStage,
                                    std::min(trainScale, 48 * opacityStage), 8, random);

  // Higher-order SH: one static codebook per coefficient, no temporal change.
  const size_t planes = K == 15 ? 5 : K == 8 ? 3 : K == 3 ? 1 : 0;
  std::vector<float> shBook;
  std::vector<int32_t> shCode(n * K);
  const size_t shEntries = K ? std::min(set.shEntries, n) : 0;
  if (K) {
    shBook.assign(planes * 3 * shEntries * 3, 0.0f);
    std::vector<float> points(n * 3), ones(n, 1.0f);
    std::vector<uint32_t> all(n);
    std::iota(all.begin(), all.end(), 0u);
    std::vector<int32_t> codes(n);
    for (size_t k = 0; k < K; ++k) {
      for (size_t i = 0; i < n; ++i)
        std::copy(&t.rest[(i * K + k) * 3], &t.rest[(i * K + k) * 3] + 3, &points[i * 3]);
      const Curves c{points.data(), ones.data(), 1, 3};
      auto book = kmeans(c, sample(all, trainScale, random), shEntries, 10, false, random);
      for (float &v : book)
        v = roundHalf(v);
      assign(c, nullptr, n, book, shEntries, codes.data(), nullptr);
      for (size_t i = 0; i < n; ++i)
        shCode[i * K + k] = codes[i];
      std::copy(book.begin(), book.end(), shBook.begin() + std::ptrdiff_t(k * shEntries * 3));
    }
  }

  checkpoint("Fitting residual rotations");
  RotationFit rotation=fitRotations(t,set.rotationEntries,set.rotationTolerance,random);

  // Stored order: one splat group per position rank, rotation ranks ascending inside it.
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0u);
  const auto rotationRank = [&](uint32_t i) { return rotation.rank[i]; };
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    if (position.rank[a] != position.rank[b])
      return position.rank[a] < position.rank[b];
    return rotationRank(a) < rotationRank(b);
  });

  Result out;
  vgs::DecodedChunk &c = out.chunk;
  vgs::Group dictionary;
  dictionary.type = 0;
  dictionary.intervals = I;
  dictionary.counts = {shEntries * (K ? 1 : 0), K ? 1u : 0u, sh0Stage * 5, opacityStage * 5,
                       rotation.entries, position.entries};
  dictionary.trajectoryMin = position.trajectory.lo;
  dictionary.trajectoryMax = position.trajectory.hi;
  c.groups.push_back(dictionary);

  // Shared tables.
  add(c, vgs::ScaleLut, 0, bytes(scaleLut), byteSpec(1024));
  {
    std::vector<uint16_t> lut(256);
    for (size_t j = 0; j < 256; ++j)
      lut[j] = toHalf(colorLevels[j]);
    add(c, vgs::Sh0Lut, 0, bytes(lut), byteSpec(512));
  }
  const auto curves = [&](const Staged &staged, size_t channels, size_t entries) {
    std::vector<uint16_t> halves(entries * S * channels);
    for (size_t i = 0; i < halves.size(); ++i)
      halves[i] = toHalf(staged.book[i]);
    return halves;
  };
  add(c, vgs::Sh0Trajectories, 0, bytes(curves(sh0, 3, sh0Stage * 5)),
      spec(0, 1, sh0Stage * 5 * S, {0, 0, 0}, 0, S));
  add(c, vgs::OpacityTrajectories, 0, bytes(curves(opacity, 1, opacityStage * 5)),
      spec(0, 1, opacityStage * 5 * S, {0}, 0, S));
  {
    std::vector<uint64_t> words(position.entries * S);
    for (size_t s = 0; s < words.size(); ++s)
      words[s] = position.trajectory.pack(&position.atoms[s * 3]);
    add(c, vgs::PositionTrajectories, 0, bytes(words), spec(2, 1, words.size(), kPos64, 8, S));
  }
  if (set.residualRotation) {
    add(c, vgs::RotationInitial, 0, bytes(rotation.initial), spec(0, 0, rotation.entries, {0, 0, 0, 0}));
    add(c, vgs::RotationDeltaLut, 0, bytes(rotation.lut), byteSpec(1024));
    add(c, vgs::RotationDeltas, 0, bytes(rotation.deltas), spec(1, 4, rotation.entries * I, {0, 0, 0, 0}, 0, 0, I));
  }
  if (K) {
    std::vector<uint16_t> book(shBook.size());
    for (size_t i = 0; i < book.size(); ++i)
      book[i] = toHalf(shBook[i]);
    add(c, vgs::ShStaticBook, 0, bytes(book), spec(0, 0, planes * 3 * shEntries, {0, 0, 0}));
    add(c, vgs::ShTemporalBook, 0, bytes(std::vector<uint16_t>(S * planes * 3 * 3, 0)),
        spec(0, 2, S * planes * 3, {0, 0, 0}, 0, 0, 0, planes * 3));
  }

  // Splat groups.
  std::vector<size_t> activePerInterval(I, 0);
  for (size_t begin = 0; begin < n;) {
    const uint8_t prank = position.rank[order[begin]];
    size_t end = begin;
    while (end < n && position.rank[order[end]] == prank)
      ++end;
    const size_t m = end - begin;
    const uint32_t gi = uint32_t(c.groups.size());
    vgs::Group g;
    g.type = 1;
    g.flags = 0;
    g.splats = m;
    g.intervals = I;
    g.positionMin = position.base.lo;
    g.positionMax = position.base.hi;
    c.groups.push_back(g);

    std::vector<uint8_t> scales(m * 3), bases(m * 3), lifetimes(m * 2);
    std::vector<uint64_t> sh0Words(m), opacityWords(m), positionBase(m);
    std::vector<uint32_t> positionTerms, shStatic(planes * m, 0);
    std::vector<uint8_t> positionRanks(m, prank), rotationRanks;
    for (size_t q = 0; q < m; ++q) {
      const uint32_t i = order[begin + q];
      const Record &rec = records[i];
      out.sourceRecords.push_back(i);
      for (size_t k = 0; k < 3; ++k) {
        scales[q * 3 + k] = scaleIndex[i * 3 + k];
        bases[q * 3 + k] = colorBase[i * 3 + k];
      }
      lifetimes[q * 2] = uint8_t(rec.first);
      lifetimes[q * 2 + 1] = uint8_t(std::min<size_t>(rec.last, I - 1) + 1);
      for (size_t f = lifetimes[q * 2]; f < lifetimes[q * 2 + 1]; ++f)
        ++activePerInterval[f];
      sh0Words[q] = stageWord(&sh0.codes[i * 5]);
      opacityWords[q] = stageWord(&opacity.codes[i * 5]);
      positionBase[q] = position.baseWords[i];
      for (size_t l = 0; l < prank; ++l)
        positionTerms.push_back(position.terms[i][l]);
      for (size_t k = 0; k < K; ++k)
        shStatic[(k / 3) * m + q] |= uint32_t(shCode[i * K + k]) << (k % 3 == 0 ? 20 : k % 3 == 1 ? 10 : 0);
    }
    add(c, vgs::ScaleIndices, gi, bytes(scales), spec(1, 3, m, {0, 0, 0}));
    add(c, vgs::Sh0Base, gi, bytes(bases), spec(1, 3, m, {0, 0, 0}));
    add(c, vgs::Lifetimes, gi, bytes(lifetimes), spec(1, 5, m, {0, 0}));
    add(c, vgs::Sh0Terms, gi, bytes(sh0Words), spec(2, 0, m, kRq64, 8));
    add(c, vgs::OpacityTerms, gi, bytes(opacityWords), spec(2, 0, m, kRq64, 8));
    add(c, vgs::PositionBase, gi, bytes(positionBase), spec(2, 0, m, kPos64, 8));
    {
      vgs::Spec s = spec(5, 9, positionTerms.size(), {0, 0});
      s.ranks = positionRanks;
      add(c, vgs::PositionTerms, gi, bytes(positionTerms), s);
      std::vector<uint32_t> boundaries(4);
      for (size_t j = 0; j < 4; ++j)
        boundaries[j] = uint32_t(j + 1 >= prank ? m : 0);
      add(c, vgs::PositionRanks, gi, bytes(boundaries), byteSpec(16));
    }
    if (set.residualRotation) {
      std::vector<uint32_t> baseWords(m);
      std::vector<uint16_t> terms;
      std::vector<uint32_t> boundaries(5, 0);
      for (size_t q = 0; q < m; ++q) {
        const uint32_t i = order[begin + q];
        baseWords[q] = rotation.baseWords[i];
        rotationRanks.push_back(rotation.rank[i]);
        for (size_t l = 0; l < rotation.rank[i]; ++l)
          terms.push_back(rotation.terms[i][l]);
        for (size_t j = rotation.rank[i] - 1; j < 5; ++j)
          ++boundaries[j];
      }
      add(c, vgs::RotationBase, gi, bytes(baseWords), spec(2, 6, m, kQuat32, 4));
      vgs::Spec s = spec(4, 8, terms.size(), {0});
      s.ranks = rotationRanks;
      add(c, vgs::RotationTerms, gi, bytes(terms), s);
      add(c, vgs::RotationRanks, gi, bytes(boundaries), byteSpec(20));
    } else {
      std::vector<uint32_t> words(m * S);
      for (size_t q = 0; q < m; ++q) {
        const uint32_t i = order[begin + q];
        for (size_t s = 0; s < S; ++s)
          words[q * S + s] = packQuaternion(&t.rotation[(i * S + s) * 4]);
      }
      add(c, vgs::RotationSamples, gi, bytes(words), spec(2, 7, m * S, kQuat32, 4, S));
    }
    if (K) {
      add(c, vgs::ShStaticIndices, gi, bytes(shStatic), spec(3, 0, m, kSh32, uint32_t(planes)));
      add(c, vgs::ShTemporalIndices, gi, bytes(std::vector<uint32_t>(planes * m, 0)),
          spec(3, 0, m, kSh32, uint32_t(planes)));
    }
    begin = end;
  }
  return out;
}

}
Result fit(Tracks tracks,const std::vector<Record> &records,size_t coefficients,const Progress &report) {
    if (!tracks.count || records.size()!=tracks.count || tracks.samples<2 || tracks.samples>256)
        throw Error("Invalid PLY fitting tracks");
    struct Scope {const Progress *old=progress;~Scope(){progress=old;}} scope;
    progress=&report;
    return encode(std::move(tracks),records,coefficients);
}
}
