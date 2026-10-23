// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#pragma once
#include "plysequence.h"
#include "exportcapture.h"
// The fitted chunks decoded at every source frame against the PLY rows they come from.
struct PlyFitCheck {
    quint64 shown=0,missing=0,stray=0;double squares=0;float worst=0;
    QString note() const;
};
vgs::Header plySequenceHeader(const PlySequence &);
// A chunk of `intervals` frames with nothing in it: one dead sentinel, as an empty frame has.
vgs::DecodedChunk emptyTemporalChunk(int intervals,int degree);
vgs::DecodedChunk packPlySequenceChunk(PlySequence &,size_t chunk,int degree,const ExportProgress &progress={},std::vector<uint32_t> *sourceRecords=nullptr,PlyFitCheck *check=nullptr);
