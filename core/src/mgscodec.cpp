#include "mgscodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <memory>

namespace mgs {
namespace {

const uint32_t kMagic = 0x3153474D;  // "MGS1"
const uint32_t kVersion = 4;
const uint32_t kRansL = 1u << 23;
const uint64_t kMaxTableEntries = 1u << 24;

// ================================================================ byte io

class Writer
{
public:
    Bytes buf;

    void u8(uint8_t v) { buf.push_back(v); }
    void u32(uint32_t v)
    {
        for (int k = 0; k < 4; ++k)
            buf.push_back(uint8_t(v >> (8 * k)));
    }
    void u64(uint64_t v)
    {
        u32(uint32_t(v));
        u32(uint32_t(v >> 32));
    }
    void varint(uint64_t v)
    {
        while (v >= 128) {
            buf.push_back(uint8_t((v & 127) | 128));
            v >>= 7;
        }
        buf.push_back(uint8_t(v));
    }
    void bytes(const uint8_t* p, size_t n) { buf.insert(buf.end(), p, p + n); }
    void bytes(const Bytes& b) { buf.insert(buf.end(), b.begin(), b.end()); }
    size_t size() const { return buf.size(); }
};

class Reader
{
public:
    Reader(const uint8_t* data, size_t size) : p(data), end(data + size) {}

    void need(uint64_t n) const
    {
        if (uint64_t(end - p) < n)
            throw Error("truncated .mgs data");
    }
    uint8_t u8()
    {
        need(1);
        return *p++;
    }
    uint32_t u32()
    {
        need(4);
        uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        p += 4;
        return v;
    }
    uint64_t u64()
    {
        const uint64_t lo = u32();
        return lo | uint64_t(u32()) << 32;
    }
    uint64_t varint()
    {
        uint64_t v = 0;
        for (int shift = 0;; shift += 7) {
            if (shift > 63)
                throw Error("malformed varint");
            const uint8_t b = u8();
            v |= uint64_t(b & 127) << shift;
            if (!(b & 128))
                return v;
        }
    }
    const uint8_t* bytes(uint64_t n)
    {
        need(n);
        const uint8_t* q = p;
        p += n;
        return q;
    }
    size_t remaining() const { return size_t(end - p); }

    const uint8_t* p;
    const uint8_t* end;
};

// Saturating arithmetic for sizes read from a file: an overflow must fail a bounds
// check rather than wrap into one that passes.
uint64_t mul(uint64_t a, uint64_t b)
{
    if (a && b > UINT64_MAX / a)
        return UINT64_MAX;
    return a * b;
}
uint64_t mul(uint64_t a, uint64_t b, uint64_t c) { return mul(mul(a, b), c); }
uint64_t mul(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { return mul(mul(a, b, c), d); }

inline int bitlen(uint32_t v)
{
    int n = 0;
    while (v) {
        ++n;
        v >>= 1;
    }
    return n;
}
inline uint32_t zig(int32_t d) { return (uint32_t(d) << 1) ^ uint32_t(d >> 31); }
inline int32_t unzig(uint32_t z) { return int32_t(z >> 1) ^ -int32_t(z & 1); }

// Unaligned 16/32-bit access through memcpy: the compiler turns each one into a single
// instruction, where byte-at-a-time stores measured as a quarter of the decode time.
// Little-endian only, which requireLittleEndian() checks.
void requireLittleEndian()
{
    const uint16_t one = 1;
    uint8_t first;
    std::memcpy(&first, &one, 1);
    if (!first)
        throw Error(".mgs needs a little-endian platform");
}

inline uint16_t loadU16(const uint8_t* p)
{
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
inline uint32_t loadU32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline void storeU16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline void storeU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void orU32(uint8_t* p, uint32_t v)
{
    uint32_t old;
    std::memcpy(&old, p, 4);
    old |= v;
    std::memcpy(p, &old, 4);
}

using Col = std::vector<int32_t>;

// Buffers reused across the items of a chunk. Decoding allocated a few megabytes per
// column, which measured as a third of the decode time; these grow once and stay.
// Slots must differ wherever two buffers are alive at the same time.
enum Slot { SBuf, SAlt, SStart, SFirst, SGaps, SAbs, SRel, SValues, SlotCount };
enum ByteSlot { BCtx, BMask, BAbsCtx, BRelCtx, BLargest, BSign, BCv, ByteSlotCount };

template <class T, int Count>
std::vector<T>& pool(int slot, size_t n)
{
    static thread_local std::vector<std::vector<T>> buffers(Count);
    std::vector<T>& v = buffers[size_t(slot)];
    if (v.size() < n)
        v.assign(n, T());
    return v;
}
int32_t* ints(int slot, size_t n) { return pool<int32_t, SlotCount>(slot, n).data(); }
uint8_t* bytes(int slot, size_t n) { return pool<uint8_t, ByteSlotCount>(slot, n).data(); }

// ================================================================ rANS

// Scale counts to frequencies summing to M, every present symbol at least 1.
void normalize(const uint32_t* counts, uint32_t A, uint32_t* f, uint32_t M)
{
    double total = 0;
    for (uint32_t s = 0; s < A; ++s)
        total += counts[s];
    if (total == 0)
        return;
    int64_t sum = 0;
    int64_t best = -1;
    for (uint32_t s = 0; s < A; ++s) {
        if (!counts[s])
            continue;
        const double x = double(counts[s]) * M / total;
        double r = std::floor(x);
        if (x - r >= 0.5)
            r += 1;
        f[s] = std::max<uint32_t>(1, uint32_t(r));
        sum += f[s];
        if (best < 0 || counts[s] > counts[best])
            best = s;
    }
    int64_t diff = int64_t(M) - sum;
    if (int64_t(f[best]) + diff >= 1) {
        f[best] = uint32_t(int64_t(f[best]) + diff);
        return;
    }
    while (diff < 0)
        for (uint32_t s = 0; s < A && diff < 0; ++s)
            if (f[s] > 1) {
                --f[s];
                ++diff;
            }
}

void writeTable(Writer& w, const uint32_t* f, uint32_t A)
{
    uint32_t k = 0;
    for (uint32_t s = 0; s < A; ++s)
        k += f[s] != 0;
    if (!k) {
        w.u8(0);
        return;
    }
    if (uint64_t(k) * 2 > A) {
        w.u8(2);
        for (uint32_t s = 0; s < A; ++s)
            w.varint(f[s]);
        return;
    }
    w.u8(1);
    w.varint(k);
    int64_t last = -1;
    for (uint32_t s = 0; s < A; ++s) {
        if (!f[s])
            continue;
        w.varint(uint64_t(int64_t(s) - last - 1));
        w.varint(f[s] - 1);
        last = s;
    }
}

// ctx: null, or one context per symbol.
template <class Ctx>
void encodeSymbols(Writer& w, const int32_t* sym, size_t N, const Ctx* ctx)
{
    int32_t max = 0;
    for (size_t i = 0; i < N; ++i) {
        if (sym[i] < 0 || sym[i] > 65535)
            throw Error("symbol out of range");
        max = std::max(max, sym[i]);
    }
    const uint32_t A = uint32_t(max) + 1;
    uint32_t nctx = 1;
    if (ctx)
        for (size_t i = 0; i < N; ++i)
            nctx = std::max<uint32_t>(nctx, uint32_t(ctx[i]) + 1);
    if (uint64_t(nctx) * A > kMaxTableEntries)
        throw Error("context tables too large");
    const int bits = std::min(16, std::max(12, bitlen(A - 1) + 3));
    const uint32_t M = 1u << bits;

    std::vector<uint32_t> counts(size_t(nctx) * A, 0), freq(size_t(nctx) * A, 0), cum(size_t(nctx) * A, 0);
    if (ctx)
        for (size_t i = 0; i < N; ++i)
            ++counts[size_t(ctx[i]) * A + sym[i]];
    else
        for (size_t i = 0; i < N; ++i)
            ++counts[sym[i]];

    w.varint(A);
    w.u8(uint8_t(bits));
    w.varint(nctx);
    for (uint32_t c = 0; c < nctx; ++c) {
        const uint32_t* f = &freq[size_t(c) * A];
        normalize(&counts[size_t(c) * A], A, &freq[size_t(c) * A], M);
        writeTable(w, f, A);
        uint32_t acc = 0;
        for (uint32_t s = 0; s < A; ++s) {
            cum[size_t(c) * A + s] = acc;
            acc += f[s];
        }
    }

    // Encoded backwards, then reversed so the decoder reads forwards.
    // Interleaved states: symbol i belongs to state i % states, so the decoder has
    // several independent chains in flight and the CPU can overlap their work. Measured
    // at 1.6x on real symbol streams. Short sequences keep one state: four would pay
    // four flushes and four lengths for nothing.
    const int states = N >= 4096 ? 4 : 1;
    w.u8(uint8_t(states));
    std::vector<std::vector<uint8_t>> rev(states);
    std::vector<uint32_t> x(states, kRansL);
    const uint32_t xmaxBase = (kRansL >> bits) << 8;
    for (size_t i = N; i-- > 0;) {
        const int s = int(i % states);
        const size_t k = (ctx ? size_t(ctx[i]) * A : 0) + size_t(sym[i]);
        const uint32_t fr = freq[k];
        const uint64_t xmax = uint64_t(xmaxBase) * fr;
        while (x[s] >= xmax) {
            rev[s].push_back(uint8_t(x[s] & 255));
            x[s] >>= 8;
        }
        x[s] = (x[s] / fr) * M + (x[s] % fr) + cum[k];
    }
    for (int s = 0; s < states; ++s) {
        for (int k = 3; k >= 0; --k)
            rev[s].push_back(uint8_t(x[s] >> (8 * k)));
        std::reverse(rev[s].begin(), rev[s].end());
        w.varint(rev[s].size());
    }
    for (int s = 0; s < states; ++s)
        w.bytes(rev[s]);
}

struct Tables
{
    uint32_t A = 0, nctx = 0, M = 0, mask = 0;
    int bits = 0;
    std::vector<uint32_t> freq, cum;
    std::vector<uint16_t> slots;

    void read(Reader& r)
    {
        const uint64_t a = r.varint();
        bits = r.u8();
        const uint64_t n = r.varint();
        if (a < 1 || a > 65536 || bits < 12 || bits > 16 || n < 1 || mul(a, n) > kMaxTableEntries ||
            mul(n, uint64_t(1) << bits) > (uint64_t(1) << 26))
            throw Error("malformed symbol table");
        A = uint32_t(a);
        nctx = uint32_t(n);
        M = 1u << bits;
        mask = M - 1;
        freq.assign(size_t(nctx) * A, 0);
        cum.assign(size_t(nctx) * A, 0);
        slots.assign(size_t(nctx) * M, 0);
        for (uint32_t c = 0; c < nctx; ++c) {
            uint32_t* f = &freq[size_t(c) * A];
            const uint8_t mode = r.u8();
            if (mode == 2) {
                for (uint32_t s = 0; s < A; ++s) {
                    const uint64_t v = r.varint();
                    if (v > M)
                        throw Error("malformed frequency");
                    f[s] = uint32_t(v);
                }
            } else if (mode == 1) {
                const uint64_t k = r.varint();
                if (k > A)
                    throw Error("malformed frequency table");
                int64_t s = -1;
                for (uint64_t j = 0; j < k; ++j) {
                    s += int64_t(r.varint()) + 1;
                    const uint64_t v = r.varint() + 1;
                    if (s >= int64_t(A) || v > M)
                        throw Error("malformed frequency table");
                    f[s] = uint32_t(v);
                }
            } else if (mode != 0) {
                throw Error("malformed frequency table");
            }
            uint64_t acc = 0;
            for (uint32_t s = 0; s < A; ++s) {
                cum[size_t(c) * A + s] = uint32_t(acc);
                if (f[s]) {
                    if (acc + f[s] > M)
                        throw Error("frequencies exceed their scale");
                    std::fill_n(&slots[size_t(c) * M + acc], f[s], uint16_t(s));
                }
                acc += f[s];
            }
            if (acc != 0 && acc != M)
                throw Error("frequencies do not sum to their scale");
        }
    }
};

// Symbol decoding. Out is the output element type; ctxOf(i, out) gives the context.
template <class Out, class CtxOf>
void decodeLoop(Reader& r, Out* out, size_t N, const Tables& t, CtxOf ctxOf)
{
    const int states = r.u8();
    if (states < 1 || states > 8)
        throw Error("malformed symbol stream");
    const uint8_t* p[8];
    const uint8_t* end[8];
    uint32_t x[8];
    uint64_t len[8];
    for (int s = 0; s < states; ++s) {
        len[s] = r.varint();
        if (len[s] < 4)
            throw Error("truncated symbol stream");
    }
    for (int s = 0; s < states; ++s) {
        p[s] = r.bytes(len[s]);
        end[s] = p[s] + len[s];
        x[s] = loadU32(p[s]);
        p[s] += 4;
    }
    const uint32_t A = t.A, M = t.M, mask = t.mask;
    const int bits = t.bits;
    const uint32_t* freq = t.freq.data();
    const uint32_t* cum = t.cum.data();
    const uint16_t* slots = t.slots.data();
    // Symbol i is decoded by state i % states, in order, so a context may still be the
    // symbol before it.
    for (size_t i = 0; i < N;) {
        for (int s = 0; s < states && i < N; ++s, ++i) {
            const uint32_t c = ctxOf(i, out);
            const uint32_t slot = x[s] & mask;
            const uint32_t sym = slots[size_t(c) * M + slot];
            const size_t k = size_t(c) * A + sym;
            x[s] = freq[k] * (x[s] >> bits) + slot - cum[k];
            // a valid stream is consumed exactly, so running out means corrupt data
            while (x[s] < kRansL) {
                if (p[s] == end[s])
                    throw Error("corrupt symbol stream");
                x[s] = (x[s] << 8) | *p[s]++;
            }
            out[i] = Out(sym);
        }
    }
}

template <class Out>
void checkOutput(const Tables& t)
{
    if constexpr (sizeof(Out) == 1) {
        if (t.A > 256)
            throw Error("symbol alphabet too large for its column");
    }
}

template <class Out>
void decodeSymbols(Reader& r, Out* out, size_t N)
{
    Tables t;
    t.read(r);
    checkOutput<Out>(t);
    decodeLoop(r, out, N, t, [](size_t, const Out*) { return uint32_t(0); });
}

template <class Out, class Ctx>
void decodeSymbols(Reader& r, Out* out, size_t N, const Ctx* ctx)
{
    Tables t;
    t.read(r);
    checkOutput<Out>(t);
    for (size_t i = 0; i < N; ++i)
        if (uint32_t(ctx[i]) >= t.nctx)
            throw Error("context out of range");
    decodeLoop(r, out, N, t, [ctx](size_t i, const Out*) { return uint32_t(ctx[i]); });
}

// Context = the previous symbol in the same run of P; `start` (or A when negative) at
// the start of each run.
template <class Out>
void decodeSymbolsRun(Reader& r, Out* out, size_t N, uint64_t P, int64_t start)
{
    Tables t;
    t.read(r);
    checkOutput<Out>(t);
    const uint32_t first = start < 0 ? t.A : uint32_t(start);
    if (first >= t.nctx || t.A > t.nctx || P == 0)
        throw Error("context out of range");
    decodeLoop(r, out, N, t, [P, first](size_t i, const Out* o) {
        return i % P == 0 ? first : uint32_t(o[i - 1]);
    });
}

// Values below 2^31: the top bits are entropy coded, the low `lo` bits stored raw.
void packBits(Writer& w, const int32_t* v, size_t N, int lo)
{
    const uint32_t mask = (1u << lo) - 1;
    uint32_t acc = 0;
    int nb = 0;
    for (size_t i = 0; i < N; ++i) {
        acc |= (uint32_t(v[i]) & mask) << nb;
        nb += lo;
        while (nb >= 8) {
            w.u8(uint8_t(acc & 255));
            acc >>= 8;
            nb -= 8;
        }
    }
    if (nb)
        w.u8(uint8_t(acc & 255));
    w.u8(0);
    w.u8(0);
    w.u8(0);
}

template <class Ctx>
void encodeWide(Writer& w, const int32_t* v, size_t N, const Ctx* ctx)
{
    int32_t max = 0;
    for (size_t i = 0; i < N; ++i) {
        if (v[i] < 0)
            throw Error("negative value");
        max = std::max(max, v[i]);
    }
    const int bl = bitlen(uint32_t(max));
    std::vector<int> options;
    for (int drop : {16, 12, 8}) {
        const int lo = std::max(0, bl - drop);
        if (std::find(options.begin(), options.end(), lo) == options.end())
            options.push_back(lo);
    }
    Writer best;
    bool have = false;
    Col hi;
    for (int lo : options) {
        if (lo > 16)
            continue;
        Writer t;
        t.u8(uint8_t(lo));
        if (lo) {
            hi.resize(N);
            for (size_t i = 0; i < N; ++i)
                hi[i] = v[i] >> lo;
            encodeSymbols(t, hi.data(), N, ctx);
            packBits(t, v, N, lo);
        } else {
            encodeSymbols(t, v, N, ctx);
        }
        if (!have || t.size() < best.size()) {
            best = std::move(t);
            have = true;
        }
    }
    if (!have)
        throw Error("value too wide");
    w.bytes(best.buf);
}
void encodeWide(Writer& w, const Col& v) { encodeWide(w, v.data(), v.size(), static_cast<const uint8_t*>(nullptr)); }

void unpackBits(Reader& r, int32_t* out, size_t N, int lo)
{
    const uint64_t bytes = (uint64_t(N) * lo + 7) / 8 + 3;
    const uint8_t* b = r.bytes(bytes);
    const uint32_t mask = (1u << lo) - 1;
    uint64_t bit = 0;
    for (size_t i = 0; i < N; ++i, bit += lo) {
        const uint8_t* q = b + (bit >> 3);
        const uint32_t word = uint32_t(q[0]) | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16;
        out[i] = int32_t((uint32_t(out[i]) << lo) | ((word >> (bit & 7)) & mask));
    }
}

int readLo(Reader& r)
{
    const int lo = r.u8();
    if (lo > 16)
        throw Error("malformed wide column");
    return lo;
}

void decodeWide(Reader& r, int32_t* out, size_t N)
{
    const int lo = readLo(r);
    decodeSymbols(r, out, N);
    if (lo)
        unpackBits(r, out, N, lo);
}

template <class Ctx>
void decodeWide(Reader& r, int32_t* out, size_t N, const Ctx* ctx)
{
    const int lo = readLo(r);
    decodeSymbols(r, out, N, ctx);
    if (lo)
        unpackBits(r, out, N, lo);
}

// ================================================================ f16 as ordered integers

// Bijection from half-float bit patterns to ranks by value (NaN last, ties by pattern),
// so close values get close integers and deltas stay small.
struct F16Order
{
    uint16_t rank[65536];
    uint16_t back[65536];

    F16Order()
    {
        std::vector<double> key(65536);
        for (uint32_t p = 0; p < 65536; ++p) {
            const int e = (p >> 10) & 31, m = p & 1023;
            const double sign = (p >> 15) ? -1.0 : 1.0;
            if (e == 31)
                key[p] = m ? NAN : sign * INFINITY;
            else
                key[p] = sign * (e ? (1.0 + m / 1024.0) * std::ldexp(1.0, e - 15) : (m / 1024.0) * std::ldexp(1.0, -14));
        }
        std::vector<uint32_t> order(65536);
        for (uint32_t p = 0; p < 65536; ++p)
            order[p] = p;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            const bool na = std::isnan(key[a]), nb = std::isnan(key[b]);
            if (na != nb)
                return nb;
            if (!na && key[a] != key[b])
                return key[a] < key[b];
            return a < b;
        });
        for (uint32_t r = 0; r < 65536; ++r) {
            rank[order[r]] = uint16_t(r);
            back[r] = uint16_t(order[r]);
        }
    }
};

const F16Order& f16()
{
    static const F16Order* order = new F16Order;
    return *order;
}

// ================================================================ .mint metadata

struct ArrayRef
{
    uint64_t start = 0, size = 0;
};

enum SharedArray { ScaleLut, ShStaticCodebooks, ShTemporalCodebooks, Sh0Trajectories, Sh0BaseLut,
                   OpacityTrajectories, RotationInitial, RotationDeltaLut, RotationDeltaIndices,
                   PositionTrajectories, MeshExtentLut, SharedArrayCount };
const uint32_t kSharedFields[SharedArrayCount][2] = {
    {8, 16}, {112, 120}, {128, 136}, {168, 176}, {184, 192}, {208, 216},
    {232, 240}, {248, 256}, {264, 272}, {304, 312}, {320, 328}};

enum SplatArray { ScaleIndices, PositionSamples, ShStaticIndices, ShTemporalIndices, Lifetimes,
                  RotationSamples, Sh0RqIndices, Sh0BaseIndices, OpacityRqIndices, RotationBase,
                  RotationRqIndices, RotationRankBoundaries, PositionBase, PositionRqCoefficients,
                  PositionRankBoundaries, SplatArrayCount };
const uint32_t kSplatFields[SplatArrayCount][2] = {
    {56, 64}, {136, 144}, {200, 208}, {216, 224}, {232, 240}, {248, 256}, {264, 272}, {280, 288},
    {296, 304}, {312, 320}, {328, 336}, {344, 352}, {360, 368}, {376, 384}, {392, 400}};

struct Block
{
    uint64_t type = 0;
    std::vector<ArrayRef> arrays;
    uint64_t intervals = 0, splats = 0;
    uint64_t shStatic = 0, shTemporal = 0, sh0 = 0, opacity = 0, rotation = 0, position = 0;
};

struct MintChunk
{
    std::vector<Block> blocks;
    uint64_t start = 0, end = 0;
};

struct Meta
{
    uint64_t payloadStart = 0;
    std::vector<MintChunk> chunks;
};

// Parses the container from its metadata region. Offsets are resolved to file offsets.
Meta parseMint(const uint8_t* data, size_t size)
{
    auto u64 = [&](uint64_t off) {
        if (off > size || size - off < 8)
            throw Error("the .mint metadata is truncated");
        return uint64_t(loadU32(data + off)) | uint64_t(loadU32(data + off + 4)) << 32;
    };
    if (u64(0) != 6)
        throw Error("not a format-6 .mint");
    Meta meta;
    meta.payloadStart = u64(16);
    uint64_t cursor = 24;
    bool haveIndex = false;
    uint64_t indexOffset = 0;
    const uint64_t records = u64(8);
    if (records > size)
        throw Error("malformed .mint records");
    for (uint64_t k = 0; k < records; ++k) {
        const uint64_t kind = u64(cursor);
        if (kind == 1) {
            indexOffset = u64(cursor + 16);
            haveIndex = true;
            cursor += 24;
        } else if (kind == 2) {
            cursor += 32;
        } else {
            throw Error("unknown .mint record type");
        }
    }
    if (!haveIndex)
        throw Error("the .mint has no index");
    const uint64_t count = u64(indexOffset);
    if (count > size)
        throw Error("malformed .mint index");
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t entry = indexOffset + 56 + i * 24;
        MintChunk chunk;
        cursor = u64(entry + 16);
        const uint64_t blocks = u64(entry + 8);
        if (blocks == 0 || blocks > size)
            throw Error("malformed .mint chunk");
        chunk.start = UINT64_MAX;
        for (uint64_t b = 0; b < blocks; ++b) {
            Block block;
            block.type = u64(cursor);
            const uint64_t h = cursor + 32;
            const uint64_t start = meta.payloadStart + u64(cursor + 8);
            const uint64_t bsize = u64(cursor + 16);
            if (start < meta.payloadStart || start + bsize < start)
                throw Error("malformed .mint block");
            chunk.start = std::min(chunk.start, start);
            chunk.end = std::max(chunk.end, start + bsize);
            auto readArrays = [&](const uint32_t (*fields)[2], int n) {
                block.arrays.resize(n);
                for (int a = 0; a < n; ++a) {
                    const uint64_t off = u64(h + fields[a][0]);
                    block.arrays[a].size = u64(h + fields[a][1]);
                    block.arrays[a].start = start + off < start ? UINT64_MAX : start + off;
                }
            };
            if (block.type == 3) {
                readArrays(kSharedFields, SharedArrayCount);
                block.shStatic = u64(h + 144);
                block.shTemporal = u64(h + 152);
                block.sh0 = u64(h + 160);
                block.opacity = u64(h + 200);
                block.rotation = u64(h + 224);
                block.position = u64(h + 296);
            } else if (block.type == 1) {
                readArrays(kSplatFields, SplatArrayCount);
                block.intervals = u64(h + 8);
                block.splats = u64(h + 16);
            }
            chunk.blocks.push_back(std::move(block));
            cursor = h + u64(cursor + 24);
        }
        meta.chunks.push_back(std::move(chunk));
    }
    return meta;
}

// ================================================================ items

enum Kind { KF16, KU8, KPacked, KPlanes, KRotTerms, KPosTerms };
enum Family { FPlain, FTime, FEntry, FChannels, FRuns, FLifetimes, FQuatBase, FQuatTime, FTerms, FPosTerms };
const int kModelCount[] = {1, 2, 2, 2, 2, 2, 2, 2, 2, 4};

const std::vector<int> kPos64 = {1, 21, 21, 21};
const std::vector<int> kRq64 = {12, 12, 12, 12, 12, 4};
const std::vector<int> kQuat32 = {1, 9, 10, 10, 2};
const std::vector<int> kSh32 = {10, 10, 10, 2};

struct Layout
{
    std::vector<uint8_t> ranks;
    std::vector<uint32_t> offsets;
    uint64_t used = 0;
};

struct Item
{
    const char* name = "";
    Kind kind = KF16;
    Family family = FPlain;
    uint64_t off = 0, bytes = 0;
    uint64_t rows = 0;          // per column
    std::vector<int> fields;    // bit widths (packed), or one zero per column
    int width = 0;              // packed word size in bytes
    uint64_t S = 0, T = 0, E = 0, n = 0;
    std::shared_ptr<Layout> layout;

    // A plane is one 32-bit word per row holding three SH coefficients. Captures
    // written with a lower SH degree carry fewer of them; `width` is the count.
    size_t planes() const { return kind == KPlanes ? size_t(width) : 1; }
    size_t columns() const { return kind == KPlanes ? planes() * fields.size() : fields.size(); }
};

// Reads a u32 at a chunk offset; for the decoder it comes from the table's raw spans.
using BoundaryReader = std::function<uint32_t(uint64_t off)>;

std::shared_ptr<Layout> rankLayout(const BoundaryReader& read, uint64_t off, int R, uint64_t n)
{
    auto layout = std::make_shared<Layout>();
    layout->ranks.resize(size_t(n));
    layout->offsets.resize(size_t(n));
    uint64_t id = 0, used = 0, prev = 0;
    for (int k = 0; k < R; ++k) {
        const uint64_t end = read(off + 4 * uint64_t(k));
        if (end < prev || end > n)
            return nullptr;
        for (; id < end; ++id) {
            layout->ranks[size_t(id)] = uint8_t(k + 1);
            layout->offsets[size_t(id)] = uint32_t(used);
            used += k + 1;
            if (used > UINT32_MAX)
                return nullptr;
        }
        prev = end;
    }
    if (id != n)
        return nullptr;
    layout->used = used;
    return layout;
}

// The raw items (rank boundaries), which the planner needs before anything else.
std::vector<Item> planRaw(const MintChunk& chunk)
{
    std::vector<Item> items;
    const uint64_t size = chunk.end - chunk.start;
    for (const Block& g : chunk.blocks) {
        if (g.type != 1)
            continue;
        for (int a : {RotationRankBoundaries, PositionRankBoundaries}) {
            const ArrayRef& ref = g.arrays[a];
            if (!ref.size || ref.start < chunk.start || ref.start - chunk.start > size ||
                size - (ref.start - chunk.start) < ref.size)
                continue;
            Item it;
            it.off = ref.start - chunk.start;
            it.bytes = ref.size;
            items.push_back(it);
        }
    }
    return items;
}

// Every coded item of a chunk, in coding order. Offsets are relative to the chunk.
std::vector<Item> planItems(const MintChunk& chunk, const BoundaryReader& read)
{
    std::vector<Item> items;
    const uint64_t size = chunk.end - chunk.start;
    const Block* shared = nullptr;
    std::vector<const Block*> groups;
    for (const Block& b : chunk.blocks) {
        if (b.type == 3 && !shared)
            shared = &b;
        if (b.type == 1)
            groups.push_back(&b);
    }
    const uint64_t T = groups.empty() ? 0 : groups[0]->intervals;
    const uint64_t S = T + 1;

    auto add = [&](const char* name, const ArrayRef& ref, uint64_t bytes, Item it) {
        it.name = name;
        if (!ref.size || !bytes || ref.size < bytes || ref.start < chunk.start)
            return;
        const uint64_t off = ref.start - chunk.start;
        if (off > size || size - off < bytes)
            return;
        it.off = off;
        it.bytes = bytes;
        items.push_back(std::move(it));
    };
    auto item = [](Kind kind, Family family, uint64_t rows, std::vector<int> fields) {
        Item it;
        it.kind = kind;
        it.family = family;
        it.rows = rows;
        it.fields = std::move(fields);
        return it;
    };
    auto zeros = [](int n) { return std::vector<int>(n, 0); };

    if (shared && !groups.empty() && T > 0 && T < 65536) {
        const Block& s = *shared;
        add("shared.sh_static_codebooks", s.arrays[ShStaticCodebooks], mul(15 * 3 * 2, s.shStatic),
            item(KF16, FPlain, mul(15, s.shStatic), zeros(3)));
        {
            Item it = item(KF16, FEntry, mul(S, 15, s.shTemporal), zeros(3));
            it.E = mul(15, s.shTemporal);
            add("shared.sh_temporal_codebooks", s.arrays[ShTemporalCodebooks], mul(S, 15 * 3 * 2, s.shTemporal), it);
        }
        {
            Item it = item(KF16, FTime, mul(s.sh0, S), zeros(3));
            it.S = S;
            add("shared.sh0_trajectories", s.arrays[Sh0Trajectories], mul(s.sh0, S, 3 * 2), it);
        }
        {
            Item it = item(KF16, FTime, mul(s.opacity, S), zeros(1));
            it.S = S;
            add("shared.opacity_trajectories", s.arrays[OpacityTrajectories], mul(s.opacity, S, 2), it);
        }
        add("shared.rotation_initial", s.arrays[RotationInitial], mul(s.rotation, 4 * 2), item(KF16, FPlain, s.rotation, zeros(4)));
        {
            Item it = item(KU8, FRuns, mul(s.rotation, T), zeros(4));
            it.T = T;
            add("shared.rotation_delta_indices", s.arrays[RotationDeltaIndices], mul(s.rotation, T, 4), it);
        }
        {
            Item it = item(KPacked, FTime, mul(s.position, S), kPos64);
            it.width = 8;
            it.S = S;
            add("shared.position_trajectories", s.arrays[PositionTrajectories], mul(s.position, S, 8), it);
        }
    }
    for (const Block* gp : groups) {
        const Block& g = *gp;
        const uint64_t n = g.splats;
        if (n == 0 || g.intervals != T)
            continue;
        add("lifetimes", g.arrays[Lifetimes], mul(n, 2), item(KU8, FLifetimes, n, zeros(2)));
        add("scale_indices", g.arrays[ScaleIndices], mul(n, 3), item(KU8, FChannels, n, zeros(3)));
        add("sh0_base_indices", g.arrays[Sh0BaseIndices], mul(n, 3), item(KU8, FChannels, n, zeros(3)));
        for (int a : {OpacityRqIndices, Sh0RqIndices}) {
            Item it = item(KPacked, FPlain, n, kRq64);
            it.width = 8;
            add(a == OpacityRqIndices ? "opacity_rq_indices" : "sh0_rq_indices", g.arrays[a], mul(n, 8), it);
        }
        for (int a : {ShStaticIndices, ShTemporalIndices}) {
            Item it = item(KPlanes, FPlain, n, kSh32);
            it.width = 5;   // a .mint always carries all five planes
            add(a == ShStaticIndices ? "sh_static_indices" : "sh_temporal_indices", g.arrays[a], mul(5, n, 4), it);
        }
        {
            Item it = item(KPacked, FQuatBase, n, kQuat32);
            it.width = 4;
            add("rotation_base", g.arrays[RotationBase], mul(n, 4), it);
        }
        {
            Item it = item(KPacked, FQuatTime, mul(n, S), kQuat32);
            it.width = 4;
            it.S = S;
            add("rotation_samples", g.arrays[RotationSamples], mul(n, S, 4), it);
        }
        {
            Item it = item(KPacked, FPlain, n, kPos64);
            it.width = 8;
            add("position_base", g.arrays[PositionBase], mul(n, 8), it);
        }
        {
            Item it = item(KPacked, FTime, mul(n, S), kPos64);
            it.width = 8;
            it.S = S;
            add("position_samples", g.arrays[PositionSamples], mul(n, S, 8), it);
        }
        struct TermsSpec { int boundaries, terms, R, width; Kind kind; Family family; int columns; };
        for (const TermsSpec& t : {TermsSpec{RotationRankBoundaries, RotationRqIndices, 5, 2, KRotTerms, FTerms, 1},
                                   TermsSpec{PositionRankBoundaries, PositionRqCoefficients, 4, 4, KPosTerms, FPosTerms, 2}}) {
            const ArrayRef& b = g.arrays[t.boundaries];
            const ArrayRef& terms = g.arrays[t.terms];
            if (!b.size || b.size < uint64_t(4 * t.R) || !terms.size || b.start < chunk.start)
                continue;
            // every splat has at least one term, so this bounds n before allocating by it
            if (mul(n, t.width) > terms.size || b.start - chunk.start > size || size - (b.start - chunk.start) < b.size)
                continue;
            auto layout = rankLayout(read, b.start - chunk.start, t.R, n);
            if (!layout)
                continue;
            Item it = item(t.kind, t.family, 0, zeros(t.columns));
            it.n = n;
            it.layout = layout;
            add(t.kind == KRotTerms ? "rotation_rq_indices" : "position_rq_coefficients", terms,
                mul(layout->used, t.width), it);
        }
    }
    return items;
}

std::vector<Span> uncovered(std::vector<Span> spans, uint64_t size)
{
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.offset < b.offset; });
    std::vector<Span> out;
    uint64_t at = 0;
    for (const Span& s : spans) {
        if (s.offset > at)
            out.push_back({at, s.offset - at});
        at = std::max(at, s.offset + s.size);
    }
    if (at < size)
        out.push_back({at, size - at});
    return out;
}

uint64_t itemCount(const Item& it) { return it.layout ? it.layout->used : it.rows; }

// ================================================================ columns

std::vector<Col> readColumns(const uint8_t* chunk, const Item& it)
{
    const uint8_t* p = chunk + it.off;
    const size_t rows = size_t(itemCount(it));
    const size_t C = it.columns();
    std::vector<Col> cols(C, Col(rows));
    const F16Order& order = f16();
    switch (it.kind) {
    case KF16:
        for (size_t i = 0, q = 0; i < rows; ++i)
            for (size_t k = 0; k < C; ++k, q += 2)
                cols[k][i] = order.rank[loadU16(p + q)];
        break;
    case KU8:
        for (size_t i = 0, q = 0; i < rows; ++i)
            for (size_t k = 0; k < C; ++k)
                cols[k][i] = p[q++];
        break;
    case KPacked:
    case KPlanes: {
        const size_t planes = it.planes();
        const size_t perWord = it.fields.size();
        const int width = it.kind == KPlanes ? 4 : it.width;
        for (size_t plane = 0; plane < planes; ++plane)
            for (size_t i = 0; i < rows; ++i) {
                const uint8_t* w = p + (plane * rows + i) * width;
                const uint64_t word = width == 8 ? uint64_t(loadU32(w)) | uint64_t(loadU32(w + 4)) << 32 : loadU32(w);
                int s = 0;
                for (size_t k = 0; k < perWord; ++k) {
                    cols[plane * perWord + k][i] = int32_t((word >> s) & ((uint64_t(1) << it.fields[k]) - 1));
                    s += it.fields[k];
                }
            }
        break;
    }
    case KRotTerms:
        for (size_t i = 0; i < rows; ++i)
            cols[0][i] = loadU16(p + 2 * i);
        break;
    case KPosTerms:
        for (size_t i = 0; i < rows; ++i) {
            const uint32_t v = loadU32(p + 4 * i);
            cols[0][i] = int32_t(v & 65535);
            cols[1][i] = order.rank[v >> 16];
        }
        break;
    }
    return cols;
}

// Writes decoded columns into an item's own bytes (which start zeroed), one column at
// a time, so a model never needs all of an item's columns alive at once.
struct Sink
{
    uint8_t* out;
    const Item& it;
    // The first column written into a word assigns it and later ones accumulate, so a
    // chunk is never zero-filled first: that measured as 56 MB of pointless writes.
    mutable uint8_t written[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    template <class T>
    void operator()(size_t k, const T* col) const
    {
        const size_t rows = size_t(itemCount(it));
        const F16Order& order = f16();
        switch (it.kind) {
        case KF16: {
            const size_t C = it.columns();
            for (size_t i = 0; i < rows; ++i)
                storeU16(out + 2 * (i * C + k), order.back[uint32_t(col[i]) & 65535]);
            break;
        }
        case KU8: {
            const size_t C = it.columns();
            for (size_t i = 0; i < rows; ++i)
                out[i * C + k] = uint8_t(col[i]);
            break;
        }
        case KPacked: {
            int s = 0;
            for (size_t j = 0; j < k; ++j)
                s += it.fields[j];
            const bool first = !written[0];
            written[0] = 1;
            if (it.width == 4) {
                if (first)
                    for (size_t i = 0; i < rows; ++i)
                        storeU32(out + 4 * i, uint32_t(col[i]) << s);
                else
                    for (size_t i = 0; i < rows; ++i)
                        orU32(out + 4 * i, uint32_t(col[i]) << s);
            } else if (first) {
                for (size_t i = 0; i < rows; ++i) {
                    const uint64_t v = uint64_t(uint32_t(col[i])) << s;
                    storeU32(out + 8 * i, uint32_t(v));
                    storeU32(out + 8 * i + 4, uint32_t(v >> 32));
                }
            } else {
                for (size_t i = 0; i < rows; ++i) {
                    const uint64_t v = uint64_t(uint32_t(col[i])) << s;
                    orU32(out + 8 * i, uint32_t(v));
                    orU32(out + 8 * i + 4, uint32_t(v >> 32));
                }
            }
            break;
        }
        case KPlanes: {
            const size_t plane = k / 4;
            int s = 0;
            for (size_t j = 0; j < k % 4; ++j)
                s += kSh32[j];
            const bool first = !written[plane];
            written[plane] = 1;
            if (first)
                for (size_t i = 0; i < rows; ++i)
                    storeU32(out + 4 * (plane * rows + i), uint32_t(col[i]) << s);
            else
                for (size_t i = 0; i < rows; ++i)
                    orU32(out + 4 * (plane * rows + i), uint32_t(col[i]) << s);
            break;
        }
        case KRotTerms:
            for (size_t i = 0; i < rows; ++i)
                storeU16(out + 2 * i, uint16_t(col[i]));
            break;
        case KPosTerms:
            for (size_t i = 0; i < rows; ++i)
                storeU16(out + 4 * i + 2 * (k ? 1 : 0), k ? order.back[uint32_t(col[i]) & 65535] : uint16_t(col[i]));
            break;
        }
    }
};

// ================================================================ models

std::vector<uint8_t> sampleContext(size_t rows, uint64_t S, uint8_t clamp)
{
    std::vector<uint8_t> c(rows);
    for (size_t i = 0; i < rows; ++i)
        c[i] = uint8_t(std::min<uint64_t>(i % S, clamp));
    return c;
}

std::vector<uint8_t> entryContext(size_t rows, uint64_t E)
{
    std::vector<uint8_t> c(rows, 0);
    for (size_t i = size_t(std::min<uint64_t>(E, rows)); i < rows; ++i)
        c[i] = 1;
    return c;
}

struct Quat
{
    std::vector<uint8_t> cl, cs, cv;
};
template <class A, class B>
Quat quatContexts(const A* largest, const B* sign, size_t n, uint64_t S)
{
    Quat q;
    q.cl.resize(n);
    q.cs.resize(n);
    q.cv.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const bool first = i % S == 0;
        q.cl[i] = first ? 4 : uint8_t(largest[i - 1]);
        q.cs[i] = first ? 2 : uint8_t(sign[i - 1]);
        q.cv[i] = first ? 0 : largest[i] == largest[i - 1] ? (sign[i] == sign[i - 1] ? 2 : 3) : 1;
    }
    return q;
}

struct Terms
{
    Col first, gaps;
    std::vector<uint8_t> gctx;
};
bool termsFirstGaps(const Col& values, const Layout& layout, uint64_t n, Terms* out)
{
    out->first.resize(size_t(n));
    out->gaps.clear();
    out->gctx.clear();
    out->gaps.reserve(size_t(layout.used - n));
    out->gctx.reserve(size_t(layout.used - n));
    for (size_t i = 0; i < n; ++i) {
        const uint32_t o = layout.offsets[i];
        out->first[i] = values[o];
        for (int k = 1; k < layout.ranks[i]; ++k) {
            const int32_t d = values[o + k] - values[o + k - 1];
            if (d < 0)
                return false;
            out->gaps.push_back(d);
            out->gctx.push_back(layout.ranks[i]);
        }
    }
    return true;
}

std::vector<uint8_t> gapContexts(const Layout& layout, uint64_t n)
{
    std::vector<uint8_t> c;
    c.reserve(size_t(layout.used - n));
    for (size_t i = 0; i < n; ++i)
        for (int k = 1; k < layout.ranks[i]; ++k)
            c.push_back(layout.ranks[i]);
    return c;
}

std::vector<uint8_t> slotContexts(const Layout& layout, uint64_t n)
{
    std::vector<uint8_t> c(static_cast<size_t>(layout.used));
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < layout.ranks[i]; ++k)
            c[layout.offsets[i] + k] = uint8_t(k);
    return c;
}

const uint8_t* none = nullptr;

// A temporal column mixes absolute values (the first sample of a run) with small deltas.
// Coded as one stream, the wide absolutes set the raw low-bit count for every value, so a
// delta of 1 paid for bits it never uses. `absolute` (computable by both sides) splits them
// into two streams; ctx, when given, applies to both.
void encodeSplit(Writer& w, const Col& d, const std::vector<uint8_t>& absolute, const uint8_t* ctx)
{
    Col abs, rel;
    std::vector<uint8_t> absCtx, relCtx;
    for (size_t i = 0; i < d.size(); ++i) {
        (absolute[i] ? abs : rel).push_back(d[i]);
        if (ctx)
            (absolute[i] ? absCtx : relCtx).push_back(ctx[i]);
    }
    encodeWide(w, abs.data(), abs.size(), ctx ? absCtx.data() : none);
    encodeWide(w, rel.data(), rel.size(), ctx ? relCtx.data() : none);
}

void decodeSplit(Reader& r, int32_t* out, size_t n, const uint8_t* absolute, const uint8_t* ctx)
{
    size_t count = 0;
    for (size_t i = 0; i < n; ++i)
        count += absolute[i] != 0;
    int32_t* abs = ints(SAbs, count);
    int32_t* rel = ints(SRel, n - count);
    if (ctx) {
        uint8_t* absCtx = bytes(BAbsCtx, count);
        uint8_t* relCtx = bytes(BRelCtx, n - count);
        size_t a = 0, b = 0;
        for (size_t i = 0; i < n; ++i)
            (absolute[i] ? absCtx[a++] : relCtx[b++]) = ctx[i];
        decodeWide(r, abs, count, absCtx);
        decodeWide(r, rel, n - count, relCtx);
    } else {
        decodeWide(r, abs, count);
        decodeWide(r, rel, n - count);
    }
    size_t a = 0, b = 0;
    for (size_t i = 0; i < n; ++i)
        out[i] = absolute[i] ? abs[a++] : rel[b++];
}

std::vector<uint8_t> runStarts(size_t rows, uint64_t S)
{
    std::vector<uint8_t> m(rows);
    for (size_t i = 0; i < rows; ++i)
        m[i] = i % S == 0;
    return m;
}

// Same mask in a reused buffer, for the decoder.
uint8_t* runStartMask(size_t rows, uint64_t S)
{
    uint8_t* m = bytes(BMask, rows);
    for (size_t i = 0; i < rows; ++i)
        m[i] = i % S == 0;
    return m;
}

void encodeTime(Writer& w, const Col& v, uint64_t S)
{
    Col d(v.size());
    for (size_t i = 0; i < v.size(); ++i)
        d[i] = i % S ? int32_t(zig(v[i] - v[i - 1])) : v[i];
    const auto ctx = sampleContext(v.size(), S, 2);
    encodeSplit(w, d, runStarts(v.size(), S), ctx.data());
}

void encodeEntry(Writer& w, const Col& v, uint64_t E)
{
    Col d(v.size());
    for (size_t i = 0; i < v.size(); ++i)
        d[i] = i >= E ? int32_t(zig(v[i] - v[i - E])) : v[i];
    std::vector<uint8_t> absolute(v.size(), 0);
    for (size_t i = 0; i < std::min<size_t>(size_t(E), v.size()); ++i)
        absolute[i] = 1;
    encodeSplit(w, d, absolute, none);
}

std::vector<uint8_t> quatAbsolute(const std::vector<uint8_t>& cv)
{
    std::vector<uint8_t> m(cv.size());
    for (size_t i = 0; i < cv.size(); ++i)
        m[i] = cv[i] < 2;
    return m;
}

void encodeTermsGaps(Writer& w, const Terms& t, const Layout& layout)
{
    encodeWide(w, t.first.data(), t.first.size(), layout.ranks.data());
    encodeWide(w, t.gaps.data(), t.gaps.size(), t.gctx.data());
}

// Encodes model `model` of the item's family. Throws when the model does not apply.
void encodeModel(Writer& w, const Item& it, int model, const std::vector<Col>& cols)
{
    if (model == 0) {
        for (const Col& c : cols)
            encodeWide(w, c);
        return;
    }
    switch (it.family) {
    case FTime:
        for (const Col& c : cols)
            encodeTime(w, c, it.S);
        return;
    case FEntry:
        for (const Col& c : cols)
            encodeEntry(w, c, it.E);
        return;
    case FChannels:
        encodeWide(w, cols[0]);
        for (size_t k = 1; k < cols.size(); ++k)
            encodeSymbols(w, cols[k].data(), cols[k].size(), cols[k - 1].data());
        return;
    case FRuns:
        for (const Col& v : cols) {
            int32_t max = 0;
            for (int32_t x : v)
                max = std::max(max, x);
            Col ctx(v.size());
            for (size_t i = 0; i < v.size(); ++i)
                ctx[i] = i % it.T ? v[i - 1] : max + 1;
            encodeSymbols(w, v.data(), v.size(), ctx.data());
        }
        return;
    case FLifetimes: {
        const Col& start = cols[0];
        Col dur(start.size());
        for (size_t i = 0; i < start.size(); ++i)
            dur[i] = int32_t(zig(cols[1][i] - start[i]));
        encodeWide(w, start);
        encodeSymbols(w, dur.data(), dur.size(), start.data());
        return;
    }
    case FQuatBase: {
        const Col& largest = cols[4];
        encodeWide(w, largest);
        encodeWide(w, cols[0]);
        for (int k = 1; k <= 3; ++k)
            encodeWide(w, cols[k].data(), cols[k].size(), largest.data());
        return;
    }
    case FQuatTime: {
        const Col& largest = cols[4];
        const Col& sign = cols[0];
        const size_t n = largest.size();
        const Quat q = quatContexts(largest.data(), sign.data(), n, it.S);
        encodeSymbols(w, largest.data(), n, q.cl.data());
        encodeSymbols(w, sign.data(), n, q.cs.data());
        const auto absolute = quatAbsolute(q.cv);
        for (int k = 1; k <= 3; ++k) {
            const Col& v = cols[k];
            Col d(n);
            for (size_t i = 0; i < n; ++i)
                d[i] = q.cv[i] >= 2 ? int32_t(zig(v[i] - v[i - 1])) : v[i];
            encodeSplit(w, d, absolute, q.cv.data());
        }
        return;
    }
    case FTerms: {
        Terms t;
        if (!termsFirstGaps(cols[0], *it.layout, it.n, &t))
            throw Error("terms are not sorted");
        encodeTermsGaps(w, t, *it.layout);
        return;
    }
    case FPosTerms: {
        // model = 2 * weights + indices, each 0 (plain) or 1 (gaps / slot context)
        if (model & 1) {
            Terms t;
            if (!termsFirstGaps(cols[0], *it.layout, it.n, &t))
                throw Error("terms are not sorted");
            encodeTermsGaps(w, t, *it.layout);
        } else {
            encodeWide(w, cols[0]);
        }
        if (model & 2) {
            const auto ctx = slotContexts(*it.layout, it.n);
            encodeWide(w, cols[1].data(), cols[1].size(), ctx.data());
        } else {
            encodeWide(w, cols[1]);
        }
        return;
    }
    default:
        throw Error("model does not apply");
    }
}

void decodeTermsGaps(Reader& r, const Item& it, int32_t* values)
{
    const Layout& layout = *it.layout;
    int32_t* first = ints(SFirst, static_cast<size_t>(it.n));
    decodeWide(r, first, size_t(it.n), layout.ranks.data());
    const auto gctx = gapContexts(layout, it.n);
    int32_t* gaps = ints(SGaps, gctx.size());
    decodeWide(r, gaps, gctx.size(), gctx.data());
    size_t q = 0;
    for (size_t i = 0; i < it.n; ++i) {
        const uint32_t o = layout.offsets[i];
        values[o] = first[i];
        for (int k = 1; k < layout.ranks[i]; ++k)
            values[o + k] = values[o + k - 1] + gaps[q++];
    }
}

void decodeModel(Reader& r, const Item& it, int model, const Sink& emit)
{
    const size_t rows = size_t(itemCount(it));
    const size_t C = it.columns();
    if (model < 0 || model >= kModelCount[it.family])
        throw Error("unknown model");
    if (model == 0) {
        int32_t* buf = ints(SBuf, rows);
        for (size_t k = 0; k < C; ++k) {
            decodeWide(r, buf, rows);
            emit(k, buf);
        }
        return;
    }
    switch (it.family) {
    case FTime: {
        int32_t* buf = ints(SBuf, rows);
        uint8_t* ctx = bytes(BCtx, rows);
        for (size_t i = 0; i < rows; ++i)
            ctx[i] = uint8_t(std::min<uint64_t>(i % it.S, 2));
        const uint8_t* absolute = runStartMask(rows, it.S);
        for (size_t k = 0; k < C; ++k) {
            decodeSplit(r, buf, rows, absolute, ctx);
            for (size_t i = 0; i < rows; ++i)
                if (i % it.S)
                    buf[i] = buf[i - 1] + unzig(uint32_t(buf[i]));
            emit(k, buf);
        }
        return;
    }
    case FEntry: {
        int32_t* buf = ints(SBuf, rows);
        uint8_t* first = bytes(BMask, rows);          // the first E values are absolute
        for (size_t i = 0; i < rows; ++i)
            first[i] = i < it.E;
        for (size_t k = 0; k < C; ++k) {
            decodeSplit(r, buf, rows, first, none);
            for (size_t i = size_t(std::min<uint64_t>(it.E, rows)); i < rows; ++i)
                buf[i] = buf[i - it.E] + unzig(uint32_t(buf[i]));
            emit(k, buf);
        }
        return;
    }
    case FChannels: {
        int32_t* prev = ints(SBuf, rows);
        int32_t* cur = ints(SAlt, rows);
        decodeWide(r, prev, rows);
        emit(0, prev);
        for (size_t k = 1; k < C; ++k) {
            decodeSymbols(r, cur, rows, prev);
            emit(k, cur);
            std::swap(prev, cur);
        }
        return;
    }
    case FRuns: {
        uint8_t* buf = bytes(BLargest, rows);
        for (size_t k = 0; k < C; ++k) {
            decodeSymbolsRun(r, buf, rows, it.T, -1);
            emit(k, buf);
        }
        return;
    }
    case FLifetimes: {
        int32_t* start = ints(SStart, rows);
        int32_t* end = ints(SBuf, rows);
        decodeWide(r, start, rows);
        decodeSymbols(r, end, rows, start);
        for (size_t i = 0; i < rows; ++i)
            end[i] = start[i] + unzig(uint32_t(end[i]));
        emit(0, start);
        emit(1, end);
        return;
    }
    case FQuatBase: {
        int32_t* largest = ints(SValues, rows);
        int32_t* buf = ints(SBuf, rows);
        decodeWide(r, largest, rows);
        for (size_t i = 0; i < rows; ++i)
            if (largest[i] > 3)
                throw Error("malformed quaternion");
        emit(4, largest);
        decodeWide(r, buf, rows);
        emit(0, buf);
        for (int k = 1; k <= 3; ++k) {
            decodeWide(r, buf, rows, largest);
            emit(k, buf);
        }
        return;
    }
    case FQuatTime: {
        uint8_t* largest = bytes(BLargest, rows);
        uint8_t* sign = bytes(BSign, rows);
        decodeSymbolsRun(r, largest, rows, it.S, 4);
        decodeSymbolsRun(r, sign, rows, it.S, 2);
        uint8_t* cv = bytes(BCv, rows);
        uint8_t* absolute = bytes(BMask, rows);
        for (size_t i = 0; i < rows; ++i) {
            const bool start = i % it.S == 0;
            cv[i] = start ? 0 : largest[i] == largest[i - 1] ? (sign[i] == sign[i - 1] ? 2 : 3) : 1;
            absolute[i] = cv[i] < 2;
        }
        emit(4, largest);
        emit(0, sign);
        int32_t* buf = ints(SBuf, rows);
        for (int k = 1; k <= 3; ++k) {
            decodeSplit(r, buf, rows, absolute, cv);
            for (size_t i = 0; i < rows; ++i)
                if (cv[i] >= 2)
                    buf[i] = buf[i - 1] + unzig(uint32_t(buf[i]));
            emit(k, buf);
        }
        return;
    }
    case FTerms: {
        int32_t* values = ints(SValues, rows);
        decodeTermsGaps(r, it, values);
        emit(0, values);
        return;
    }
    case FPosTerms: {
        int32_t* buf = ints(SBuf, rows);
        if (model & 1)
            decodeTermsGaps(r, it, buf);
        else
            decodeWide(r, buf, rows);
        emit(0, buf);
        if (model & 2) {
            const auto ctx = slotContexts(*it.layout, it.n);
            decodeWide(r, buf, rows, ctx.data());
        } else {
            decodeWide(r, buf, rows);
        }
        emit(1, buf);
        return;
    }
    default:
        throw Error("unknown model");
    }
}

// ================================================================ chunk encoding

Bytes encodeChunk(const uint8_t* mint, const MintChunk& chunk)
{
    const uint8_t* bytes = mint + chunk.start;
    const uint64_t size = chunk.end - chunk.start;
    BoundaryReader read = [&](uint64_t off) {
        if (off > size || size - off < 4)
            throw Error("rank boundaries outside the chunk");
        return loadU32(bytes + off);
    };

    const std::vector<Item> raw = planRaw(chunk);
    const std::vector<Item> items = planItems(chunk, read);

    std::vector<Bytes> payloads;
    std::vector<Span> covered;
    for (const Item& it : items) {
        const std::vector<Col> cols = readColumns(bytes, it);
        Writer best;
        int bestModel = -1;
        for (int model = 0; model < kModelCount[it.family]; ++model) {
            Writer t;
            t.u8(uint8_t(model));
            try {
                encodeModel(t, it, model, cols);
            } catch (const Error&) {
                if (model == 0)
                    throw;
                continue;
            }
            if (bestModel < 0 || t.size() < best.size()) {
                best = std::move(t);
                bestModel = model;
            }
        }
        payloads.push_back(std::move(best.buf));
        covered.push_back({it.off, it.bytes});
    }
    for (const Item& it : raw)
        covered.push_back({it.off, it.bytes});

    const std::vector<Span> gaps = uncovered(covered, size);
    Col residue;
    for (const Span& g : gaps)
        for (uint64_t i = 0; i < g.size; ++i)
            residue.push_back(bytes[g.offset + i]);
    Writer residueStream;
    if (!residue.empty())
        encodeSymbols(residueStream, residue.data(), residue.size(), none);

    Writer w;
    w.varint(raw.size());
    for (const Item& it : raw) {
        w.varint(it.off);
        w.varint(it.bytes);
    }
    for (const Item& it : raw)
        w.bytes(bytes + it.off, size_t(it.bytes));
    w.varint(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        w.varint(items[i].off);
        w.varint(items[i].bytes);
        w.varint(payloads[i].size());
    }
    w.varint(gaps.size());
    for (const Span& g : gaps) {
        w.varint(g.offset);
        w.varint(g.size);
    }
    w.varint(residueStream.size());
    for (const Bytes& p : payloads)
        w.bytes(p);
    w.bytes(residueStream.buf);
    return std::move(w.buf);
}

} // namespace

// ================================================================ container

bool readHeader(const uint8_t* data, size_t size, Header* out, size_t* needed)
{
    try {
        Reader r(data, size);
        if (r.u32() != kMagic)
            throw Error("not a .mgs file");
        if (r.u32() != kVersion)
            throw Error("unsupported .mgs version");
        Header h;
        h.mintSize = r.u64();
        h.metadataSize = r.u64();
        h.metadataStart = uint64_t(r.p - data);
        r.bytes(h.metadataSize);
        h.tailSize = r.u64();
        h.tailStart = uint64_t(r.p - data);
        r.bytes(h.tailSize);
        const uint32_t count = r.u32();
        r.need(mul(count, 24));
        uint64_t at = uint64_t(r.p - data) + uint64_t(count) * 24;
        h.headerSize = at;
        for (uint32_t i = 0; i < count; ++i) {
            ChunkEntry c;
            c.mintStart = r.u64();
            c.mintSize = r.u64();
            c.mgsSize = r.u64();
            c.mgsStart = at;
            if (c.mintStart > h.mintSize || h.mintSize - c.mintStart < c.mintSize || at + c.mgsSize < at)
                throw Error("malformed .mgs chunk table");
            at += c.mgsSize;
            h.chunks.push_back(c);
        }
        *out = std::move(h);
        return true;
    } catch (const Error& e) {
        if (std::string(e.what()) != "truncated .mgs data")
            throw;
        if (needed)
            *needed = size * 2 + 64;
        return false;
    }
}

Bytes encodeFile(const uint8_t* mint, size_t size, const std::function<bool(int, int)>& progress)
{
    requireLittleEndian();
    const Meta meta = parseMint(mint, size);
    const int count = int(meta.chunks.size());
    for (int i = 0; i < count; ++i) {
        const MintChunk& c = meta.chunks[i];
        if (c.end > size || (i > 0 && c.start != meta.chunks[i - 1].end))
            throw Error("the .mint chunks are not stored contiguously");
    }
    const uint64_t head = count ? meta.chunks.front().start : size;
    const uint64_t tail = count ? meta.chunks.back().end : size;

    std::vector<Bytes> payloads;
    for (int i = 0; i < count; ++i) {
        payloads.push_back(encodeChunk(mint, meta.chunks[i]));
        if (progress && !progress(i + 1, count))
            throw Error("cancelled");
    }

    Writer w;
    w.u32(kMagic);
    w.u32(kVersion);
    w.u64(size);
    w.u64(head);
    w.bytes(mint, size_t(head));
    w.u64(size - tail);
    w.bytes(mint + tail, size_t(size - tail));
    w.u32(uint32_t(count));
    for (int i = 0; i < count; ++i) {
        w.u64(meta.chunks[i].start);
        w.u64(meta.chunks[i].end - meta.chunks[i].start);
        w.u64(payloads[i].size());
    }
    for (const Bytes& p : payloads)
        w.bytes(p);
    return std::move(w.buf);
}

ChunkTable readChunkTable(const uint8_t* payload, size_t size)
{
    Reader r(payload, size);
    ChunkTable t;
    // every span is at least two varint bytes, which bounds a count before it is trusted
    const uint64_t raws = r.varint();
    if (raws > r.remaining() / 2)
        throw Error("malformed chunk table");
    uint64_t rawTotal = 0;
    for (uint64_t i = 0; i < raws; ++i) {
        Span s;
        s.offset = r.varint();
        s.size = r.varint();
        rawTotal += s.size;
        t.rawSpans.push_back(s);
    }
    t.rawBytes = r.bytes(rawTotal);
    t.head = payload;
    t.headSize = uint64_t(r.p - payload);
    const uint64_t items = r.varint();
    if (items > r.remaining() / 3)
        throw Error("malformed chunk table");
    uint64_t total = 0;
    for (uint64_t i = 0; i < items; ++i) {
        ChunkTable::Item it;
        it.out.offset = r.varint();
        it.out.size = r.varint();
        it.payloadSize = r.varint();
        total += it.payloadSize;
        t.items.push_back(it);
    }
    const uint64_t spans = r.varint();
    if (spans > r.remaining() / 2)
        throw Error("malformed chunk table");
    for (uint64_t i = 0; i < spans; ++i) {
        Span s;
        s.offset = r.varint();
        s.size = r.varint();
        t.residueSpans.push_back(s);
    }
    t.residueSize = r.varint();
    for (ChunkTable::Item& it : t.items)
        it.payload = r.bytes(it.payloadSize);
    t.residue = r.bytes(t.residueSize);
    return t;
}

struct ItemDecoder::State
{
    Meta meta;
    size_t chunk = SIZE_MAX;
    std::vector<Item> items;
};

ItemDecoder::ItemDecoder(const uint8_t* metadata, size_t size) : m_state(new State)
{
    try {
        requireLittleEndian();
        m_state->meta = parseMint(metadata, size);
    } catch (...) {
        delete m_state;
        throw;
    }
}

ItemDecoder::~ItemDecoder() { delete m_state; }

size_t ItemDecoder::chunkCount() const { return m_state->meta.chunks.size(); }

void ItemDecoder::plan(size_t index, const uint8_t* head, size_t headSize)
{
    if (index >= m_state->meta.chunks.size())
        throw Error("chunk index out of range");
    const MintChunk& chunk = m_state->meta.chunks[index];
    Reader r(head, headSize);
    std::vector<Span> spans(size_t(std::min<uint64_t>(r.varint(), headSize)));
    uint64_t total = 0;
    for (Span& s : spans) {
        s.offset = r.varint();
        s.size = r.varint();
        total += s.size;
    }
    const uint8_t* raw = r.bytes(total);
    BoundaryReader read = [&](uint64_t off) {
        uint64_t at = 0;
        for (const Span& s : spans) {
            if (off >= s.offset && off - s.offset <= s.size && s.size - (off - s.offset) >= 4)
                return loadU32(raw + at + (off - s.offset));
            at += s.size;
        }
        throw Error("rank boundaries missing from the chunk table");
    };
    m_state->items = planItems(chunk, read);
    m_state->chunk = index;
}

size_t ItemDecoder::itemCount() const { return m_state->items.size(); }

uint64_t ItemDecoder::itemSize(size_t item) const
{
    if (item >= m_state->items.size())
        throw Error("item index out of range");
    return m_state->items[item].bytes;
}

void ItemDecoder::decodeItem(size_t item, const uint8_t* payload, size_t size, uint8_t* out) const
{
    if (item >= m_state->items.size())
        throw Error("item index out of range");
    const Item& it = m_state->items[item];
    Reader r(payload, size);
    const int model = r.u8();
    decodeModel(r, it, model, Sink{out, it});
}

void decodeResidue(const uint8_t* payload, size_t size, uint8_t* out, size_t count)
{
    if (!count)
        return;
    Reader r(payload, size);
    decodeSymbols(r, out, count);
}

Bytes decodeChunk(const uint8_t* metadata, size_t metadataSize, size_t index, const uint8_t* payload, size_t size)
{
    ItemDecoder decoder(metadata, metadataSize);
    Meta meta = parseMint(metadata, metadataSize);
    const MintChunk& chunk = meta.chunks.at(index);
    const uint64_t chunkSize = chunk.end - chunk.start;
    const ChunkTable table = readChunkTable(payload, size);
    decoder.plan(index, table.head, size_t(table.headSize));
    if (table.items.size() != decoder.itemCount())
        throw Error("chunk table does not match its plan");

    Bytes bytes(size_t(chunkSize), 0);
    auto fits = [&](const Span& s) { return s.offset <= chunkSize && chunkSize - s.offset >= s.size; };
    uint64_t at = 0;
    for (const Span& s : table.rawSpans) {
        if (!fits(s))
            throw Error("raw span outside the chunk");
        std::memcpy(bytes.data() + s.offset, table.rawBytes + at, size_t(s.size));
        at += s.size;
    }
    for (size_t i = 0; i < table.items.size(); ++i) {
        const ChunkTable::Item& it = table.items[i];
        if (!fits(it.out) || it.out.size != decoder.itemSize(i))
            throw Error("item span does not match its plan");
        decoder.decodeItem(i, it.payload, size_t(it.payloadSize), bytes.data() + it.out.offset);
    }
    uint64_t residueTotal = 0;
    for (const Span& s : table.residueSpans) {
        if (!fits(s))
            throw Error("residue span outside the chunk");
        residueTotal += s.size;
    }
    std::vector<uint8_t> residue(static_cast<size_t>(residueTotal));
    decodeResidue(table.residue, size_t(table.residueSize), residue.data(), residue.size());
    at = 0;
    for (const Span& s : table.residueSpans) {
        std::memcpy(bytes.data() + s.offset, residue.data() + at, size_t(s.size));
        at += s.size;
    }
    return bytes;
}

std::vector<ColumnDump> dumpColumns(const uint8_t* mint, size_t size, size_t chunk)
{
    const Meta meta = parseMint(mint, size);
    const MintChunk& c = meta.chunks.at(chunk);
    if (c.end > size)
        throw Error("chunk outside the file");
    const uint8_t* bytes = mint + c.start;
    const uint64_t chunkSize = c.end - c.start;
    BoundaryReader read = [&](uint64_t off) {
        if (off > chunkSize || chunkSize - off < 4)
            throw Error("rank boundaries outside the chunk");
        return loadU32(bytes + off);
    };
    std::vector<ColumnDump> out;
    for (const Item& it : planItems(c, read)) {
        const std::vector<Col> cols = readColumns(bytes, it);
        for (size_t k = 0; k < cols.size(); ++k)
            out.push_back({it.name, int(k), cols[k]});
    }
    return out;
}

Bytes decodeFile(const uint8_t* mgs, size_t size)
{
    Header h;
    if (!readHeader(mgs, size, &h, nullptr))
        throw Error("truncated .mgs file");
    Bytes out(size_t(h.mintSize), 0);
    if (h.metadataSize > h.mintSize || h.tailSize > h.mintSize - h.metadataSize)
        throw Error("malformed .mgs header");
    std::memcpy(out.data(), mgs + h.metadataStart, size_t(h.metadataSize));
    std::memcpy(out.data() + (h.mintSize - h.tailSize), mgs + h.tailStart, size_t(h.tailSize));
    for (size_t i = 0; i < h.chunks.size(); ++i) {
        const ChunkEntry& c = h.chunks[i];
        if (c.mgsStart > size || size - c.mgsStart < c.mgsSize)
            throw Error("truncated .mgs chunk");
        const Bytes chunk = decodeChunk(mgs + h.metadataStart, size_t(h.metadataSize), i, mgs + c.mgsStart, size_t(c.mgsSize));
        if (chunk.size() != c.mintSize)
            throw Error("chunk size does not match the table");
        std::memcpy(out.data() + c.mintStart, chunk.data(), chunk.size());
    }
    return out;
}


// Shared numerical codec entry points. Descriptor validation happens before any
// allocation or model invocation, since VGS descriptors arrive over the network.
namespace {
Item attributeItem(const AttributeSpec& s) {
    requireLittleEndian();
    if (s.kind > KPosTerms || s.family > FPosTerms || s.fields.empty() || s.fields.size() > 4 + 2)
        throw Error("invalid attribute descriptor");
    Item it;
    it.kind = Kind(s.kind); it.family = Family(s.family); it.width = int(s.width);
    it.rows = s.rows; it.S = s.samples; it.T = s.intervals; it.E = s.entries; it.fields = s.fields;
    const auto C = s.fields.size();
    if (s.kind == KPacked || s.kind == KPlanes) {
        int bits = 0;
        for (int f : s.fields) { if (f < 1 || f > 21) throw Error("invalid bit field"); bits += f; }
        if (s.kind == KPlanes) {
            // One, three or five planes of three coefficients each: SH degree 1, 2
            // (with one slot to spare) or 3. Zero means five, which is what the
            // first version of the format wrote before a degree could be chosen.
            const int planes = s.width ? s.width : 5;
            if (s.fields != kSh32 || (planes != 1 && planes != 3 && planes != 5))
                throw Error("invalid SH planes");
            it.width = planes;
            it.bytes = mul(s.rows, uint64_t(planes) * 4);
        } else {
            if ((s.width != 4 && s.width != 8) || bits != int(s.width * 8)) throw Error("invalid packed width");
            it.bytes = mul(s.rows, s.width);
        }
    } else if (s.kind == KF16 || s.kind == KU8) {
        for (int f : s.fields) if (f) throw Error("invalid scalar field");
        it.bytes = mul(s.rows, C, s.kind == KF16 ? 2 : 1);
    } else {
        if ((s.kind == KRotTerms && C != 1) || (s.kind == KPosTerms && C != 2) || s.ranks.empty())
            throw Error("invalid term layout");
        it.layout = std::make_shared<Layout>(); it.n = s.ranks.size();
        it.layout->ranks = s.ranks; it.layout->offsets.reserve(s.ranks.size());
        for (auto rank : s.ranks) {
            if (rank < 1 || rank > (s.kind == KRotTerms ? 5 : 4) || it.layout->used > UINT32_MAX - rank)
                throw Error("invalid term rank");
            it.layout->offsets.push_back(uint32_t(it.layout->used)); it.layout->used += rank;
        }
        if (s.rows != it.layout->used) throw Error("term count mismatch");
        it.bytes = mul(s.rows, s.kind == KRotTerms ? 2 : 4);
    }
    if (!it.bytes || it.bytes > (uint64_t(1) << 30)) throw Error("attribute exceeds 1 GiB limit");
    switch (it.family) {
    case FPlain: break;
    case FTime: if (!it.S || it.S > 65536 || it.rows % it.S) throw Error("invalid temporal shape"); break;
    case FEntry: if (!it.E || it.rows % it.E) throw Error("invalid entry shape"); break;
    case FChannels: if (it.kind != KU8) throw Error("invalid channel shape"); break;
    case FRuns: if (it.kind != KU8 || !it.T || it.rows % it.T) throw Error("invalid run shape"); break;
    case FLifetimes: if (it.kind != KU8 || C != 2) throw Error("invalid lifetimes"); break;
    case FQuatBase: case FQuatTime:
        if (it.kind != KPacked || it.fields != kQuat32 || it.width != 4) throw Error("invalid quaternion shape");
        if (it.family == FQuatTime && (!it.S || it.S > 65536 || it.rows % it.S)) throw Error("invalid quaternion samples");
        break;
    case FTerms: if (it.kind != KRotTerms) throw Error("invalid rotation terms"); break;
    case FPosTerms: if (it.kind != KPosTerms) throw Error("invalid position terms"); break;
    }
    if (it.layout && it.family != FTerms && it.family != FPosTerms && it.family != FPlain)
        throw Error("invalid term family");
    return it;
}
}
uint64_t attributeSize(const AttributeSpec& spec) { return attributeItem(spec).bytes; }
int attributeModelCount(const AttributeSpec& spec) { return kModelCount[attributeItem(spec).family]; }
Bytes encodeAttribute(const AttributeSpec& spec, int model, const uint8_t* data, size_t size) {
    const Item it = attributeItem(spec);
    if (size != it.bytes || model < 0 || model >= kModelCount[it.family]) throw Error("invalid attribute input");
    Writer w; encodeModel(w, it, model, readColumns(data, it)); return std::move(w.buf);
}
Bytes decodeAttribute(const AttributeSpec& spec, int model, const uint8_t* data, size_t size) {
    const Item it = attributeItem(spec);
    if (model < 0 || model >= kModelCount[it.family]) throw Error("invalid attribute model");
    Bytes out(size_t(it.bytes)); Reader r(data, size);
    decodeModel(r, it, model, Sink{out.data(), it});
    if (r.remaining()) throw Error("trailing attribute payload");
    return out;
}
std::vector<SourceAttribute> sourceAttributes(const uint8_t* data, size_t size, size_t index) {
    const Meta meta = parseMint(data, size);
    if (index >= meta.chunks.size()) throw Error("chunk index out of range");
    const auto& c = meta.chunks[index];
    if (c.end > size || c.end < c.start) throw Error("source chunk outside file");
    const auto items = planItems(c, [&](uint64_t off) {
        if (off > c.end-c.start || c.end-c.start-off < 4) throw Error("invalid rank offset");
        return loadU32(data+c.start+off);
    });
    std::vector<SourceAttribute> result;
    for (const auto& it : items) {
        SourceAttribute a; a.name = it.name; a.offset = c.start + it.off; a.size = it.bytes;
        auto& s = a.spec;
        s.kind = it.kind; s.family = it.family; s.width = it.width; s.rows = itemCount(it);
        s.samples = it.S; s.intervals = it.T; s.entries = it.E; s.fields = it.fields;
        if (it.layout) s.ranks = it.layout->ranks;
        attributeSize(s); result.push_back(std::move(a));
    }
    return result;
}

} // namespace mgs
