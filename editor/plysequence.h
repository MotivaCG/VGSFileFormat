// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#pragma once
#include "plysequencespec.h"
#include "vgsframe.h"
#include <functional>
#include <memory>

class PlySequence {
public:
    using Progress=std::function<void(int,int,const QString &)>;
    struct Record {quint32 row=0,generation=0;int first=0,last=0;};
    struct Chunk {int first=0,frames=0;size_t records=0;int degree=0;};
    explicit PlySequence(const QString &first,const PlySequenceSpec &saved={},double fps=30,const Progress &progress={});
    ~PlySequence();
    const PlySequenceSpec &spec() const;
    const QVector<Chunk> &chunks() const;
    int frames() const;
    int degree() const;
    double fps() const;
    void setFps(double);
    const double *bounds() const;
    size_t chunkAt(double seconds) const;
    // Stable chunk-local record IDs; a new generation never inherits an Erase pick.
    vgs::Frame frame(double seconds,bool sh=true);
    // Source rows before remapping to generation records, for the export packer.
    vgs::Frame sourceFrame(int index,bool sh=true) const;
    std::vector<Record> records(size_t chunk) const;
private:
    struct State;
    std::unique_ptr<State> d;
};
