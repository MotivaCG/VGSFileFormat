// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#include "plyexport.h"
#include "nativeexport.h"
#include "previewmemory.h"
#include "dependencies/plyseq/plyfit.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
namespace {
using Bytes=vgs::Bytes;
void report(const ExportProgress &progress,const QString &text) {if (progress && !progress(0,text)) throw std::runtime_error("Export canceled.");}
void holdEdges(std::vector<float> &values,size_t channels,int samples,const std::vector<PlySequence::Record> &records,int first) {
    for (size_t i=0;i<records.size();++i) {const int begin=records[i].first-first,end=records[i].last-first;
        for (int s=0;s<samples;++s) if (s<begin || s>end) std::copy_n(values.data()+(i*size_t(samples)+size_t(std::clamp(s,begin,end)))*channels,channels,values.data()+(i*size_t(samples)+size_t(s))*channels);}
}
float median(std::vector<float> &values) {const size_t mid=values.size()/2;std::nth_element(values.begin(),values.begin()+mid,values.end());return values.size()%2 ? values[mid] : .5f*(values[mid]+*std::max_element(values.begin(),values.begin()+mid));}
}
vgs::Header plySequenceHeader(const PlySequence &source) {
    vgs::Header h;h.timeNumerator=1000000;h.timeDenominator=uint32_t(std::llround(source.fps()*1000000));h.frameCount=h.durationTicks=source.frames();h.shDegree=source.degree();
    for (const auto &c:source.chunks()) {vgs::ChunkEntry e;e.startTick=c.first;e.intervals=c.frames;h.chunks.push_back(e);}return h;
}
vgs::DecodedChunk packPlySequenceChunk(PlySequence &source,size_t index,int degree,const ExportProgress &progress,std::vector<uint32_t> *sourceRecords,PlyFitCheck *check) {
    const auto &chunk=source.chunks().at(qsizetype(index));const auto records=source.records(index);const size_t n=records.size();const int T=chunk.frames,S=T+1,kept=std::min(degree,chunk.degree),K=kept==3 ? 15 : kept==2 ? 8 : kept==1 ? 3 : 0;
    if (sourceRecords) sourceRecords->clear();
    if (!n) {
        vgs::Frame sentinel;sentinel.count=1;sentinel.active={0};sentinel.position={0,0,0};sentinel.rotation={0,0,0,1};sentinel.scale={0,0,0};sentinel.opacity={0};sentinel.colorDc={.5f,.5f,.5f};
        auto out=packExportFrame(sentinel,degree);for (auto &g:out.groups) g.intervals=T;
        for (auto &p:out.pages) {const auto id=p.descriptor.attribute;if (id==vgs::PositionSamples || id==vgs::RotationSamples || id==vgs::Sh0Trajectories || id==vgs::OpacityTrajectories || id==vgs::ShTemporalBook) {const size_t stride=p.bytes.size()/2;Bytes expanded(size_t(S)*stride);for (int s=0;s<S;++s) std::copy_n(p.bytes.data(),stride,expanded.data()+size_t(s)*stride);p.bytes=std::move(expanded);p.descriptor.spec={};p.descriptor.spec.rows=p.descriptor.totalRows=p.bytes.size();}}
        return assembleNativeChunk(std::move(out));
    }
    // As indexing planned it: free memory less 2 GiB left to the rest of the computer.
    const quint64 work=n*quint64(S)*(48+quint64(K*12))*2+64*previewmemory::MiB;const auto memory=previewmemory::systemSnapshot();
    if (!memory.valid || memory.physicalAvailable<work+2*previewmemory::GiB) throw std::runtime_error("Not enough free memory to prepare this PLY chunk safely.");
    plyfit::Tracks tracks;tracks.count=n;tracks.samples=S;
    tracks.position.resize(n*size_t(S)*3);tracks.rotation.resize(n*size_t(S)*4);tracks.color.resize(n*size_t(S)*3);tracks.opacity.resize(n*size_t(S));tracks.weight.resize(n*size_t(S));tracks.scale.resize(n*3);tracks.rest.resize(n*size_t(K)*3);
    std::vector<float> higher(n*size_t(S)*size_t(K)*3);
    std::vector<plyfit::Record> lives;for (const auto &r:records) lives.push_back({uint32_t(r.first-chunk.first),uint32_t(r.last-chunk.first)});
    // The closing sample, the next chunk's first frame, holds the records that run into it.
    const bool closing=std::any_of(records.begin(),records.end(),[&](const PlySequence::Record &r) {return r.last==chunk.first+T;});
    for (int s=0;s<(closing ? S : T);++s) {
        report(progress,QStringLiteral("Reading PLY chunk %1: frame %2 / %3").arg(index+1).arg(s+1).arg(closing ? S : T));const auto input=source.sourceFrame(chunk.first+s,K>0);
        for (size_t i=0;i<n;++i) {const auto &r=records[i];const int frame=chunk.first+s;if (frame<r.first || frame>r.last) continue;const size_t row=r.row,at=i*size_t(S)+size_t(s);
            std::copy_n(input.position.data()+row*3,3,tracks.position.data()+at*3);tracks.opacity[at]=input.opacity[row];
            tracks.rotation[at*4]=input.rotation[row*4+3];for (int c=0;c<3;++c) tracks.rotation[at*4+c+1]=input.rotation[row*4+c];
            for (int c=0;c<3;++c) tracks.color[at*3+c]=float((input.colorDc[row*3+c]-.5)/.28209479177387814);
            for (int k=0;k<K*3;++k) higher[(i*size_t(S)+size_t(s))*size_t(K)*3+size_t(k)]=input.shRest[row*size_t(input.shCoefficients)*3+size_t(k)];
            if (frame==r.first) for (int c=0;c<3;++c) tracks.scale[i*3+c]=std::log(std::max(input.scale[row*3+c],1e-30f));
        }
    }
    holdEdges(tracks.position,3,S,records,chunk.first);holdEdges(tracks.rotation,4,S,records,chunk.first);holdEdges(tracks.color,3,S,records,chunk.first);holdEdges(tracks.opacity,1,S,records,chunk.first);
    for (size_t i=0;i<n;++i) for (int s=1;s<S;++s) {
        auto *q=tracks.rotation.data()+(i*size_t(S)+size_t(s))*4;const auto *previous=q-4;float dot=0;
        for (int c=0;c<4;++c) dot+=q[c]*previous[c];if (dot<0) for (int c=0;c<4;++c) q[c]=-q[c];
    }
    std::vector<float> values;
    for (size_t i=0;i<n;++i) {const auto &r=lives[i];for (int s=int(r.first);s<=std::min(int(r.last)+1,S-1);++s) tracks.weight[i*size_t(S)+size_t(s)]=1;
        values.resize(r.last-r.first+1);for (int k=0;k<K*3;++k) {for (size_t s=r.first;s<=r.last;++s) values[s-r.first]=higher[(i*size_t(S)+s)*size_t(K)*3+size_t(k)];tracks.rest[i*size_t(K)*3+size_t(k)]=median(values);}}
    std::vector<float>().swap(higher);
    const auto exact=check ? tracks.position : std::vector<float>();
    auto result=plyfit::fit(std::move(tracks),lives,size_t(K),[&](const std::string &stage) {report(progress,QString::fromStdString(stage));});
    auto out=assembleNativeChunk(std::move(result.chunk));
    if (check) {
        report(progress,QStringLiteral("Checking PLY chunk %1").arg(index+1));const vgs::FrameDecoder decoder(out,1/source.fps());std::vector<uint8_t> seen(n);
        for (int s=0;s<T;++s) {
            // The sample itself: nudged past float rounding so the decoder lands on it.
            double at=double(s)/T;while (std::floor(float(at)*float(T))<float(s)) at=std::nextafter(at,1.);
            const auto frame=decoder.evaluate(at,false);std::fill(seen.begin(),seen.end(),uint8_t(0));
            for (size_t j=0;j<frame.count;++j) {if (!frame.active[j]) continue;const auto i=result.sourceRecords[j];
                if (s<int(lives[i].first) || s>int(lives[i].last)) {++check->stray;continue;}
                float d=0;for (int c=0;c<3;++c) d+=std::pow(frame.position[j*3+c]-exact[(i*size_t(S)+size_t(s))*3+size_t(c)],2.f);
                seen[i]=1;++check->shown;check->squares+=d;check->worst=std::max(check->worst,std::sqrt(d));}
            for (size_t i=0;i<n;++i) check->missing+=s>=int(lives[i].first) && s<=int(lives[i].last) && !seen[i];
        }
    }
    if (sourceRecords) *sourceRecords=std::move(result.sourceRecords);
    if (degree!=kept) {setNativeShDegree(out,kept,degree);out=assembleNativeChunk(std::move(out));}return out;
}
QString PlyFitCheck::note() const {
    return QStringLiteral("PLY fit, before modifiers: %1 splat-frames shown, %2 visible PLY splats not shown, %3 shown where their splat is not; position RMS %4 mm, max %5 mm.")
        .arg(shown).arg(missing).arg(stray).arg(shown ? 1000*std::sqrt(squares/double(shown)) : 0.,0,'f',3).arg(1000*double(worst),0,'f',2);
}
