// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "packedchunk.h"
#include "vgsdecoder/vgsdecoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

namespace {
// The format's attribute numbers (vgscodec.h, Attribute).
enum : uint32_t {
    ScaleLut = 1, ShStaticBook, ShTemporalBook, Sh0Trajectories, Sh0Lut, OpacityTrajectories,
    RotationInitial, RotationDeltaLut, RotationDeltas, PositionTrajectories,
    ScaleIndices = 32, PositionSamples, ShStaticIndices, ShTemporalIndices, Lifetimes, RotationSamples,
    Sh0Terms, Sh0Base, OpacityTerms, RotationBase, RotationTerms, RotationRanks, PositionBase,
    PositionTerms, PositionRanks, OpacityScales
};
// The numerical descriptor's kind for SH index planes: one block of rows per plane.
constexpr uint32_t ShIndexPlanesKind = 3;

using Bytes = std::vector<uint8_t>;
uint16_t u16(const uint8_t *p) { uint16_t v; std::memcpy(&v,p,2); return v; }
uint32_t u32(const uint8_t *p) { uint32_t v; std::memcpy(&v,p,4); return v; }
uint64_t u64(const uint8_t *p) { uint64_t v; std::memcpy(&v,p,8); return v; }
float f32(const uint8_t *p) { float v; std::memcpy(&v,p,4); return v; }
uint32_t bits(float f) { uint32_t v; std::memcpy(&v,&f,4); return v; }

// vgsframe.cpp's conversions, so the basis rounds exactly as the CPU evaluation does.
float halfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
    uint32_t out;
    if (exponent == 0) {
        if (mantissa == 0) out = sign;
        else { int e = -1; uint32_t m = mantissa; do { ++e; m <<= 1; } while ((m & 0x400u) == 0);
               out = sign | (uint32_t(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13); }
    } else if (exponent == 0x1F) out = sign | 0x7F800000u | (mantissa << 13);
    else out = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f; std::memcpy(&f,&out,4); return f;
}
uint16_t floatToHalf(float f) {
    const uint32_t value = bits(f), sign = (value >> 16) & 0x8000u;
    int32_t exponent = int32_t((value >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = value & 0x7FFFFFu;
    if (exponent >= 0x1F) return uint16_t(sign | 0x7C00u);
    if (exponent <= 0) {
        if (exponent < -10) return uint16_t(sign);
        mantissa |= 0x800000u; const uint32_t shift = uint32_t(14 - exponent);
        return uint16_t(sign | ((mantissa + (1u << (shift - 1))) >> shift));
    }
    const uint32_t rounded = (mantissa + 0x1000u) >> 13;
    if (rounded & 0x400u) return uint16_t(sign | (uint32_t(exponent + 1) << 10));
    return uint16_t(sign | (uint32_t(exponent) << 10) | rounded);
}
float roundHalf(float v) { return halfToFloat(floatToHalf(v)); }
float readHalf(const uint8_t *p) { return halfToFloat(u16(p)); }
void unpackPosition(uint64_t word, double lo, double hi, float *xyz) {
    const double scale = (hi - lo) / 2097151.0;
    xyz[0] = float(lo + double((word >> 43) & 0x1FFFFFull) * scale);
    xyz[1] = float(lo + double((word >> 22) & 0x1FFFFFull) * scale);
    xyz[2] = float(lo + double((word >> 1) & 0x1FFFFFull) * scale);
}

struct Group {
    uint32_t flags = 0;
    uint64_t splats = 0;
    double positionMin = 0, positionMax = 0;
    std::map<uint32_t, Bytes> arrays;
    const Bytes &array(uint32_t attribute) const {
        const auto it = arrays.find(attribute);
        if (it == arrays.end()) throw std::runtime_error("packed chunk: missing attribute " + std::to_string(attribute));
        return it->second;
    }
    const Bytes *optional(uint32_t attribute) const { const auto it = arrays.find(attribute); return it == arrays.end() ? nullptr : &it->second; }
};
void need(const Bytes &bytes, uint64_t size) { if (bytes.size() < size) throw std::runtime_error("packed chunk: short attribute"); }
}

uint64_t PackedChunk::bytes() const {
    uint64_t total = 0;
    for (const auto *v : {&splatA,&splatB,&splatC,&shStatic,&shTemporal,&positionTerms,&rotationTerms,&positionSamples,&rotationSamples}) total += v->size()*4;
    return total + (books.size() + luts.size())*4 + lifetimes.size();
}

PackedInstant packedInstant(const PackedChunk &chunk, uint32_t sampleA, float alpha) {
    PackedInstant at;
    const double intervals = double(std::max<uint64_t>(1, chunk.intervals));
    const double t = std::clamp(double(sampleA) + double(alpha), 0.0, intervals);
    at.frame = uint32_t(std::min(std::floor(t), intervals - 1));
    at.alpha = float(t - at.frame);
    at.normalized = float(t / intervals);
    return at;
}

PackedBasisLayout basisLayout(const PackedChunk &chunk) {
    PackedBasisLayout layout;
    layout.position = 0;
    layout.sh0 = int(chunk.positionEntries);
    layout.opacity = layout.sh0 + int(chunk.sh0Entries);
    layout.rotation = layout.opacity + int(chunk.opacityEntries);
    layout.total = layout.rotation + int(chunk.rotationEntries);
    return layout;
}

// FrameDecoder::buildBasis, writing RGBA texels.
void evaluateBasis(const PackedChunk &c, const PackedInstant &at, std::vector<float> *texels) {
    const auto layout = basisLayout(c);
    texels->assign(size_t(std::max(1, layout.total)) * 4, 0.f);
    float *out = texels->data();
    const uint64_t samples = c.samples, intervals = c.intervals, frame = at.frame;
    const float alpha = at.alpha;
    for (uint64_t i = 0; i < c.positionEntries; ++i) {
        const uint8_t *row = c.positionTrajectories.data() + i*samples*8;
        float a[3], b[3];
        unpackPosition(u64(row + frame*8), c.trajectoryMin, c.trajectoryMax, a);
        unpackPosition(u64(row + (frame+1)*8), c.trajectoryMin, c.trajectoryMax, b);
        for (int k = 0; k < 3; ++k) out[(layout.position + i)*4 + k] = a[k] + (b[k]-a[k])*alpha;
    }
    for (uint64_t i = 0; i < c.sh0Entries; ++i) {
        const uint8_t *row = c.sh0Trajectories.data() + i*samples*6;
        for (int k = 0; k < 3; ++k) {
            const float a = readHalf(row + frame*6 + k*2), b = readHalf(row + (frame+1)*6 + k*2);
            out[(layout.sh0 + i)*4 + k] = a + (b-a)*alpha;
        }
    }
    for (uint64_t i = 0; i < c.opacityEntries; ++i) {
        const uint8_t *row = c.opacityTrajectories.data() + i*samples*2;
        const float a = readHalf(row + frame*2), b = readHalf(row + (frame+1)*2);
        out[(layout.opacity + i)*4] = a + (b-a)*alpha;
    }
    // The accumulator stays float32; only the stored samples are rounded back to half.
    for (uint64_t i = 0; i < c.rotationEntries; ++i) {
        float acc[4];
        for (int k = 0; k < 4; ++k) acc[k] = readHalf(c.rotationInitial.data() + (i*4 + k)*2);
        const uint8_t *row = c.rotationDeltas.data() + i*intervals*4;
        for (uint64_t step = 0; step < frame; ++step) for (int k = 0; k < 4; ++k) acc[k] += c.rotationLut[row[step*4 + k]];
        float q0[4], q1[4];
        for (int k = 0; k < 4; ++k) { q0[k] = roundHalf(acc[k]); q1[k] = roundHalf(acc[k] + c.rotationLut[row[frame*4 + k]]); }
        static const int order[4] = {1, 2, 3, 0}; // stored wxyz, used xyzw
        for (int k = 0; k < 4; ++k) { const int s = order[k]; out[(layout.rotation + i)*4 + k] = roundHalf(q0[s] + (q1[s]-q0[s])*alpha); }
    }
}

std::shared_ptr<PackedChunk> packChunk(const vgsdec::ChunkData &data, unsigned shDegree) {
    if (data.groupCount < 2) throw std::runtime_error("packed chunk: no splat groups");
    if (data.groupCount - 1 > size_t(PackedChunk::MaxGroups)) throw std::runtime_error("packed chunk: too many groups");
    std::vector<Group> groups(data.groupCount);
    for (size_t g = 0; g < data.groupCount; ++g) {
        groups[g].flags = data.groups[g].flags; groups[g].splats = data.groups[g].splats;
        groups[g].positionMin = data.groups[g].positionMin; groups[g].positionMax = data.groups[g].positionMax;
    }
    // Pages into per-attribute arrays: a page holds some rows of one attribute of one group.
    for (size_t b = 0; b < data.bufferCount; ++b) {
        const auto &page = data.buffers[b];
        if (page.group >= groups.size() || !page.rows || !page.data) continue;
        if (page.firstRow + page.rows > page.totalRows) throw std::runtime_error("packed chunk: page past its attribute");
        auto &dest = groups[page.group].arrays[page.attribute];
        const uint64_t stride = page.size / page.rows;
        if (dest.empty()) dest.assign(size_t(page.totalRows*stride), 0);
        if (page.kind == ShIndexPlanesKind) {
            const uint64_t planes = page.width ? page.width : 5;
            if (page.size < planes*page.rows*4) throw std::runtime_error("packed chunk: short SH index page");
            for (uint64_t plane = 0; plane < planes; ++plane)
                std::memcpy(dest.data() + (plane*page.totalRows + page.firstRow)*4, page.data + plane*page.rows*4, size_t(page.rows*4));
        } else std::memcpy(dest.data() + page.firstRow*stride, page.data, size_t(page.rows*stride));
    }

    auto out = std::make_shared<PackedChunk>();
    PackedChunk &c = *out;
    const auto &shared = data.groups[0];
    const Group &tables = groups[0];
    c.chunkIndex = data.chunkIndex;
    c.intervals = shared.intervals; c.samples = shared.intervals + 1;
    if (!c.intervals) throw std::runtime_error("packed chunk: no intervals");
    c.entriesStatic = shared.counts[0]; c.entriesTemporal = shared.counts[1];
    c.sh0Entries = shared.counts[2]; c.opacityEntries = shared.counts[3];
    c.rotationEntries = shared.counts[4]; c.positionEntries = shared.counts[5];
    c.sh0Stride = c.sh0Entries/5; c.opacityStride = c.opacityEntries/5;
    c.trajectoryMin = shared.trajectoryMin; c.trajectoryMax = shared.trajectoryMax;
    c.groupCount = int(groups.size() - 1);
    for (size_t g = 1; g < groups.size(); ++g) c.splats += groups[g].splats;
    const uint64_t n = c.splats, samples = c.samples;
    c.planes = shDegree >= 3 ? 5 : shDegree == 2 ? 3 : int(shDegree);
    c.coefficients = shDegree >= 3 ? 15 : shDegree == 2 ? 8 : int(shDegree)*3;
    c.hasSh = c.planes > 0;
    for (size_t g = 1; g < groups.size(); ++g) c.hasSh = c.hasSh && (!groups[g].splats || groups[g].arrays.count(ShStaticIndices));
    if (!c.hasSh) c.planes = c.coefficients = 0;

    c.splatA.assign(n*4, 0); c.splatB.assign(n*4, 0); c.splatC.assign(n*4, 0);
    c.lifetimes.assign(n*2, 0);
    if (c.hasSh) { c.shStatic.assign(n*c.planes, 0); c.shTemporal.assign(n*c.planes, 0); }

    // Splats are sorted by term count and the boundaries are cumulative, so a splat's terms
    // follow by arithmetic (vgsframe.cpp, RankLookup); C keeps where they start and how many.
    auto ranks = [&](const Bytes &boundaries, int count, uint64_t groupSplats, uint64_t base, uint64_t at, int slot, int shift) {
        need(boundaries, uint64_t(count)*4);
        uint64_t offset = base, id = 0;
        for (int k = 0; k < count; ++k) {
            const uint64_t end = std::min<uint64_t>(u32(boundaries.data() + k*4), groupSplats);
            for (; id < end; ++id) { c.splatC[(at+id)*4 + slot] = uint32_t(offset); c.splatC[(at+id)*4 + 2] |= uint32_t(k+1) << shift; offset += uint64_t(k+1); }
        }
        if (id != groupSplats) throw std::runtime_error("packed chunk: incomplete rank boundaries");
        return offset - base;
    };

    uint64_t at = 0, positionTerms = 0, rotationTerms = 0;
    for (size_t g = 1; g < groups.size(); ++g) {
        const Group &group = groups[g];
        const uint64_t count = group.splats; const uint32_t flags = group.flags; const int gi = int(g - 1);
        c.groupA[gi*4] = float(at + count); c.groupA[gi*4+1] = float(flags);
        c.groupA[gi*4+2] = float(group.positionMin); c.groupA[gi*4+3] = float(group.positionMax);
        if (!count) continue;
        const Bytes &scale = group.array(ScaleIndices), &sh0Base = group.array(Sh0Base), &life = group.array(Lifetimes);
        const Bytes &sh0Rq = group.array(Sh0Terms), &opacityRq = group.array(OpacityTerms);
        need(scale, count*3); need(sh0Base, count*3); need(life, count*2); need(sh0Rq, count*8); need(opacityRq, count*8);
        const Bytes *opacityScales = group.optional(OpacityScales);
        if (opacityScales && opacityScales->size() < count) opacityScales = nullptr;
        const Bytes *positionBase = (flags & 1) ? nullptr : &group.array(PositionBase);
        const Bytes *rotationBase = (flags & 2) ? nullptr : &group.array(RotationBase);
        if (positionBase) need(*positionBase, count*8);
        if (rotationBase) need(*rotationBase, count*4);
        for (uint64_t i = 0; i < count; ++i) {
            uint32_t *A = &c.splatA[(at+i)*4], *B = &c.splatB[(at+i)*4], *C = &c.splatC[(at+i)*4];
            if (positionBase) { A[0] = u32(positionBase->data() + i*8); A[1] = u32(positionBase->data() + i*8 + 4); }
            if (rotationBase) A[2] = u32(rotationBase->data() + i*4);
            A[3] = scale[i*3] | (uint32_t(scale[i*3+1]) << 8) | (uint32_t(scale[i*3+2]) << 16) | (uint32_t(life[i*2]) << 24);
            // The opacity factor's eight bits ride in the top nibbles the residual codes never use.
            const uint32_t code = opacityScales ? (*opacityScales)[i] : 0;
            B[0] = u32(sh0Rq.data() + i*8); B[1] = (u32(sh0Rq.data() + i*8 + 4) & 0x0FFFFFFFu) | ((code >> 4) << 28);
            B[2] = u32(opacityRq.data() + i*8); B[3] = (u32(opacityRq.data() + i*8 + 4) & 0x0FFFFFFFu) | ((code & 15u) << 28);
            C[3] = sh0Base[i*3] | (uint32_t(sh0Base[i*3+1]) << 8) | (uint32_t(sh0Base[i*3+2]) << 16) | (uint32_t(life[i*2+1]) << 24);
            c.lifetimes[(at+i)*2] = life[i*2]; c.lifetimes[(at+i)*2+1] = life[i*2+1];
        }
        if (flags & 1) {
            const Bytes &s = group.array(PositionSamples); need(s, count*samples*8);
            c.groupB[gi*4] = float(c.positionSamples.size()/2);
            const auto *w = reinterpret_cast<const uint32_t *>(s.data());
            c.positionSamples.insert(c.positionSamples.end(), w, w + count*samples*2);
        } else {
            const uint64_t terms = ranks(group.array(PositionRanks), 4, count, positionTerms, at, 0, 0);
            const Bytes &t = group.array(PositionTerms); need(t, terms*4);
            for (uint64_t k = 0; k < terms; ++k) {
                const uint32_t packed = u32(t.data() + k*4);
                c.positionTerms.push_back(packed & 0xFFFFu); c.positionTerms.push_back(bits(halfToFloat(uint16_t(packed >> 16))));
            }
            positionTerms += terms;
        }
        if (flags & 2) {
            const Bytes &s = group.array(RotationSamples); need(s, count*samples*4);
            c.groupB[gi*4+1] = float(c.rotationSamples.size());
            const auto *w = reinterpret_cast<const uint32_t *>(s.data());
            c.rotationSamples.insert(c.rotationSamples.end(), w, w + count*samples);
        } else {
            const uint64_t terms = ranks(group.array(RotationRanks), 5, count, rotationTerms, at, 1, 8);
            const Bytes &t = group.array(RotationTerms); need(t, terms*2);
            for (uint64_t k = 0; k < terms; ++k) c.rotationTerms.push_back(u16(t.data() + k*2));
            rotationTerms += terms;
        }
        if (c.hasSh) {
            // Plane-major on both sides: plane p of a group lands inside plane p of the chunk.
            const Bytes &s = group.array(ShStaticIndices), &t = group.array(ShTemporalIndices);
            need(s, count*c.planes*4); need(t, count*c.planes*4);
            for (int plane = 0; plane < c.planes; ++plane) {
                std::memcpy(&c.shStatic[plane*n + at], s.data() + plane*count*4, size_t(count*4));
                std::memcpy(&c.shTemporal[plane*n + at], t.data() + plane*count*4, size_t(count*4));
            }
        }
        at += count;
    }

    if (c.hasSh) {
        // RGBA per entry, so one fetch reads a coefficient's three channels.
        const uint64_t staticEntries = uint64_t(c.planes)*3*c.entriesStatic, temporalEntries = samples*uint64_t(c.planes)*3*c.entriesTemporal;
        const Bytes &s = tables.array(ShStaticBook), &t = tables.array(ShTemporalBook);
        need(s, staticEntries*6); need(t, temporalEntries*6);
        c.books.resize(size_t(staticEntries + temporalEntries)*4);
        float *b = c.books.data();
        for (uint64_t i = 0; i < staticEntries; ++i, b += 4) for (int k = 0; k < 3; ++k) b[k] = readHalf(s.data() + (i*3 + k)*2);
        for (uint64_t i = 0; i < temporalEntries; ++i, b += 4) for (int k = 0; k < 3; ++k) b[k] = readHalf(t.data() + (i*3 + k)*2);
    }
    {
        const Bytes &scale = tables.array(ScaleLut), &sh0 = tables.array(Sh0Lut);
        need(scale, 1024); need(sh0, 512);
        c.luts.resize(512);
        for (int i = 0; i < 256; ++i) { c.luts[i*2] = f32(scale.data() + i*4); c.luts[i*2+1] = readHalf(sh0.data() + i*2); }
    }
    // A table with no entries may be left out: chunks whose groups all store per-sample
    // positions or rotations carry no dictionary for them.
    auto table = [&](uint32_t attribute, uint64_t entries, uint64_t bytes) {
        if (!entries) return Bytes();
        const Bytes &b = tables.array(attribute); need(b, bytes); return b;
    };
    c.positionTrajectories = table(PositionTrajectories, c.positionEntries, c.positionEntries*samples*8);
    c.sh0Trajectories = table(Sh0Trajectories, c.sh0Entries, c.sh0Entries*samples*6);
    c.opacityTrajectories = table(OpacityTrajectories, c.opacityEntries, c.opacityEntries*samples*2);
    c.rotationInitial = table(RotationInitial, c.rotationEntries, c.rotationEntries*8);
    c.rotationDeltas = table(RotationDeltas, c.rotationEntries, c.rotationEntries*c.intervals*4);
    if (c.rotationEntries) {
        const Bytes &lut = tables.array(RotationDeltaLut); need(lut, 1024);
        for (int i = 0; i < 256; ++i) c.rotationLut[i] = f32(lut.data() + i*4);
    }
    // A texture buffer cannot be empty; one zero word keeps every sampler bound.
    for (auto *v : {&c.positionTerms,&c.rotationTerms,&c.positionSamples,&c.rotationSamples,&c.shStatic,&c.shTemporal}) if (v->empty()) v->assign(2, 0);
    if (c.books.empty()) c.books.assign(4, 0.f);
    return out;
}
