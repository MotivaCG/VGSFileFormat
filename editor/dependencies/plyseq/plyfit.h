// SPDX-License-Identifier: LicenseRef-VGS-Encoder-Proprietary
#pragma once
#include "vgsframe.h"
#include <functional>
#include <string>

namespace plyfit {
// One chunk, already read progressively and split into stable identity records.
// Position/DC/rotation/opacity are [record][sample][channel], quaternion wxyz.
// Scale is constant log scale; higher SH is the record's median, RGB per coefficient.
struct Tracks {
    size_t count=0,samples=0;
    std::vector<float> position,rotation,color,opacity,weight,scale,rest;
};
struct Record {uint32_t first=0,last=0;};
struct Result {vgs::DecodedChunk chunk;std::vector<uint32_t> sourceRecords;};
using Progress=std::function<void(const std::string &)>;
// The encoding-0 fitting algorithm from vgsplyseq (TemporalPlyEncoderV4).
// Stored records are grouped by fitted rank; sourceRecords maps back to preview IDs.
Result fit(Tracks,const std::vector<Record> &,size_t coefficients,const Progress & = {});
}
