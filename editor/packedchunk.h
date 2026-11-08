// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace vgsdec { struct ChunkData; }

// A chunk the decoder prepared in Packed mode, turned into the texel arrays the viewport's
// packed splat shader reads (Viewport, packedRecord). The decoder's buffers are one per
// attribute page in the format's layout; these are one per texture, everything per splat
// concatenated over the groups and packed four words at a time, so the whole capture is
// one draw. The same layout as the web viewer's vgspack.mjs, with what WebGL forced on it
// (16-bit halves, 2D textures) turned into what OpenGL 3.3 texture buffers take.
//
// Built once per chunk on the worker thread and uploaded once per chunk; per frame the
// GPU reads it with the instant's sample, fraction and the shared tables' basis.
struct PackedChunk {
    static constexpr int MaxGroups = 8;
    size_t chunkIndex = 0;
    uint64_t samples = 0, intervals = 0, splats = 0;
    // SH planes of three coefficients kept, and the coefficients of whole bands: 1/3, 3/8, 5/15.
    int planes = 0, coefficients = 0;
    bool hasSh = false;
    uint64_t entriesStatic = 0, entriesTemporal = 0;
    uint64_t positionEntries = 0, sh0Entries = 0, opacityEntries = 0, rotationEntries = 0;
    uint64_t sh0Stride = 0, opacityStride = 0;
    double trajectoryMin = 0, trajectoryMax = 0;
    // Per group: end index, flags, position min, max; and the first per-sample texel of
    // positions and rotations for groups that store them.
    int groupCount = 0;
    float groupA[MaxGroups * 4] = {}, groupB[MaxGroups * 4] = {};

    // Per splat, four words each:
    //   A: position base (2), rotation base, scale indices + birth sample
    //   B: colour and opacity residual words (2 each); top nibbles: the opacity factor
    //   C: first position term, first rotation term, term counts, colour base + death sample
    std::vector<uint32_t> splatA, splatB, splatC;
    std::vector<uint32_t> shStatic, shTemporal;   // plane-major, one word per plane and splat
    std::vector<uint32_t> positionTerms;          // pairs: dictionary entry, float weight bits
    std::vector<uint32_t> rotationTerms;          // dictionary entries
    std::vector<uint32_t> positionSamples;        // pairs: the packed 64-bit position
    std::vector<uint32_t> rotationSamples;
    std::vector<float> books;                     // RGBA per SH codebook entry: static, then temporal
    std::vector<float> luts;                      // 256 pairs: scale, base colour
    std::vector<uint8_t> lifetimes;               // birth and death sample per splat

    // The shared tables, kept to evaluate the basis at each instant.
    std::vector<uint8_t> positionTrajectories, sh0Trajectories, opacityTrajectories, rotationInitial, rotationDeltas;
    float rotationLut[256] = {};

    uint64_t bytes() const;
};
using PackedChunkPtr = std::shared_ptr<const PackedChunk>;

// Where an instant falls in its chunk, as the CPU evaluation reads it: the interval, the
// fraction inside it, and the normalised chunk time per-sample positions use.
struct PackedInstant {
    uint32_t frame = 0;
    float alpha = 0, normalized = 0;
};
PackedInstant packedInstant(const PackedChunk &, uint32_t sampleA, float alpha);

// The four shared tables at one instant, as RGBA texels one after the other: positions,
// colour, opacity and rotations. Where each starts.
struct PackedBasisLayout {
    int position = 0, sh0 = 0, opacity = 0, rotation = 0, total = 0;
};
PackedBasisLayout basisLayout(const PackedChunk &);
// Exactly as the CPU evaluator builds them, half rounding included.
void evaluateBasis(const PackedChunk &, const PackedInstant &, std::vector<float> *texels);

// Throws std::runtime_error for a chunk this layout cannot hold (too many groups, a missing
// attribute); the caller then plays the capture the floats way.
std::shared_ptr<PackedChunk> packChunk(const vgsdec::ChunkData &, unsigned shDegree);

// What a packed frame carries besides its chunk.
struct PackedFrame {
    PackedChunkPtr chunk;
    PackedInstant instant;
    std::vector<float> basis;
    // The decoder's positions at the instant, moved, for sorting and picking; and which
    // splats are alive at it.
    std::vector<float> positions;
    std::vector<uint8_t> alive;
    // The capture's motion at the instant, column-major; the identity without one.
    float motion[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    // What the editor's modifiers made of the frame, when it plays packed (the viewer's frames
    // leave these empty). Per splat, the floats frame's modifierVisibility: 0 for a splat it
    // does not draw at all, else 1 + 254 x the visibility.
    std::vector<uint8_t> visibility;
    // A despill's base colours, per splat RGBA as half floats (alpha unused), replacing the
    // chunk's; and its linear transform of every SH coefficient's RGB, row-major.
    std::vector<uint16_t> colours;
    float shMatrix[9] = {1,0,0, 0,1,0, 0,0,1};
};
