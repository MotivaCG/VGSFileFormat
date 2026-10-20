// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "captureworker.h"
#include "isolation.h"
#include "pruning.h"
#include "pointvisibility.h"
#include <Eigen/Dense>
#include <QJsonArray>
#include <QJsonDocument>
#ifndef VGS_VIEWER // the viewer never despills, and must not link the export (it carries the encoder)
#include "exportcapture.h"
#endif
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QScopedValueRollback>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>
#include <mutex>
#include <QDateTime>

struct PreviewExportStore {
    std::mutex mutex;
    QString asset;
    qint64 fileSize=0,modified=0;
    QByteArray key;
    std::map<int,std::weak_ptr<const ExportVisibilityMask>> frames;
    std::atomic<size_t> hits{0};
};
namespace {
std::mutex exportStoresMutex;
std::vector<std::weak_ptr<PreviewExportStore>> exportStores;
}

class CaptureWorker::FileSource : public vgsdec::Source {
public:
    QFile file;
    explicit FileSource(const QString &path) : file(path) {
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
    }
    bool read(uint64_t offset, size_t count, uint8_t *out) override {
        if (offset > uint64_t(file.size()) || count > uint64_t(file.size()) - offset) return false;
        return file.seek(qint64(offset)) && file.read(reinterpret_cast<char *>(out), qint64(count)) == qint64(count);
    }
    uint64_t size() const override { return uint64_t(file.size()); }
};
CaptureWorker::CaptureWorker(QObject *parent,previewmemory::Probe memoryProbe) : QObject(parent),memory_(std::move(memoryProbe)) {
    exportStore_=std::make_shared<PreviewExportStore>();
    {std::lock_guard<std::mutex> lock(exportStoresMutex);exportStores.push_back(exportStore_);}
    memoryTimer_=new QTimer(this);memoryTimer_->setInterval(500);
    connect(memoryTimer_,&QTimer::timeout,this,&CaptureWorker::relieveMemoryPressure);
}
CaptureWorker::~CaptureWorker() = default;
size_t CaptureWorker::exportVisibilityHits() const {return exportStore_->hits.load();}
void CaptureWorker::forgetExportVisibility(int frame) {
    std::lock_guard<std::mutex> lock(exportStore_->mutex);
    if (frame<0) {exportStore_->frames.clear();exportStore_->key.clear();}
    else exportStore_->frames.erase(frame);
}
std::function<bool(int,const std::vector<uint8_t> &,std::vector<uint8_t> &)> CaptureWorker::exportVisibilityLookup(const Project &project) {
    const CompiledModifiers modifiers(project);
    if (modifiers.isolations.isEmpty() || (modifiers.despill && !modifiers.despillPreview && modifiers.greensAfterColour())) return {};
    Project identity=project;identity.updatePreviewCacheKeys();const auto key=identity.previewVisibilityKeys[0];
    const QFileInfo file(project.asset);const auto asset=file.canonicalFilePath();const auto size=file.size(),modified=file.lastModified().toMSecsSinceEpoch();
    std::weak_ptr<PreviewExportStore> selected;
    {std::lock_guard<std::mutex> registry(exportStoresMutex);
     for (auto it=exportStores.begin();it!=exportStores.end();) {
        const auto store=it->lock();if (!store) {it=exportStores.erase(it);continue;}++it;
        std::lock_guard<std::mutex> lock(store->mutex);
        if (store->asset==asset && store->fileSize==size && store->modified==modified && store->key==key) {selected=store;break;}
     }}
    if (selected.expired()) return {};
    return [selected,key](int frame,const std::vector<uint8_t> &active,std::vector<uint8_t> &out) {
        const auto store=selected.lock();if (!store) return false;
        std::shared_ptr<const ExportVisibilityMask> mask;
        {std::lock_guard<std::mutex> lock(store->mutex);if (store->key!=key) return false;
         const auto found=store->frames.find(frame);if (found==store->frames.end()) return false;mask=found->second.lock();}
        if (!mask || mask->records!=active.size() || mask->points!=size_t(std::count_if(active.begin(),active.end(),[](uint8_t a){return a!=0;}))) return false;
        out.assign(active.size(),0);size_t row=0;bool visible=true;
        for (uint32_t run:mask->runs) {for (uint32_t k=0;k<run;++k) {while (row<active.size() && !active[row]) ++row;if (row==active.size()) return false;out[row++]=visible;}visible=!visible;}
        ++store->hits;return true;
    };
}
namespace {
size_t frameBufferBytes(const RenderFrame &f) {
    return sizeof(RenderFrame)+f.records.capacity()*sizeof(Splat)+f.active.capacity()+f.points.capacity()*sizeof(PointVertex)+f.sh.capacity()*sizeof(float);
}
}
size_t CaptureWorker::reusableFrameBytes() const {
    size_t bytes=(morphScratch_.position.capacity()+morphScratch_.rotation.capacity()+morphScratch_.scale.capacity()+morphScratch_.colour.capacity()+morphScratch_.opacity.capacity()+morphScratch_.sh.capacity())*sizeof(float);for (const auto &f:framePool_) if (f) bytes+=frameBufferBytes(*f);return bytes;}
void CaptureWorker::trimFramePool() {
    if (!cachingEnabled_ || !memory_.canRetain(0)) {framePool_={};morphScratch_={};return;}
    size_t bytes=reusableFrameBytes();
    // This is only the spare SH block left by a swap. Prefer releasing it to
    // evicting either in-flight frame's reusable storage during the crossfade.
    if (bytes>FrameBudget && morphScratch_.sh.capacity()) {
        bytes-=morphScratch_.sh.capacity()*sizeof(float);std::vector<float>().swap(morphScratch_.sh);
    }
    for (auto &slot:framePool_) if (slot && bytes>FrameBudget) {bytes-=frameBufferBytes(*slot);slot.reset();}
    if (bytes>FrameBudget) morphScratch_={};
}
void CaptureWorker::clear() {
    memoryTimer_->stop();clearColours();
#ifndef VGS_VIEWER
    ply_.reset();
#endif
    vgs_.reset(); source_.reset(); mint_.reset(); info_ = {}; generation_ = 0; pruneScores_.clear(); morph_.reset();morphScratch_={}; morphKey_.clear(); clearVisibility();framePool_={};
    dependencyRevision_=0;dependencyVisibilityKeys_={};dependencyMorphKey_.clear();dependencyColourKey_.clear();
}
void CaptureWorker::setCachingEnabled(bool enabled) {
    if (cachingEnabled_==enabled) return;cachingEnabled_=enabled;
    clearVisibility();clearColours();framePool_={};morphScratch_={};pruneScores_.clear();
    if (morph_) morph_->setCaching(enabled,PreviewBudget-VisibilityBudget-FrameBudget);
    if (vgs_) {vgs_->setCachePolicy(enabled ? vgsdec::CachePolicy{1,1,512ull*1024*1024} : vgsdec::CachePolicy{0,0,1});if (!enabled) vgs_->releaseCache();}
    emit cacheReduced(generation_);
}
void CaptureWorker::preparePreview(Project &project) {
    setCachingEnabled(project.previewCacheEnabled);processingRevision_=project.previewRevision;
    if (project.previewMorphKey.isEmpty()) {
        if (!project.previewRevision || dependencyRevision_!=project.previewRevision || dependencyMorphKey_.isEmpty()) {
            project.updatePreviewCacheKeys();dependencyRevision_=project.previewRevision;
            dependencyVisibilityKeys_=project.previewVisibilityKeys;dependencyMorphKey_=project.previewMorphKey;dependencyColourKey_=project.previewColourKey;
        } else {project.previewVisibilityKeys=dependencyVisibilityKeys_;project.previewMorphKey=dependencyMorphKey_;project.previewColourKey=dependencyColourKey_;}
    }
    for (int context=0;context<3;++context) if (visibilityKeys_[context]!=project.previewVisibilityKeys[context]) {
        clearVisibility(context);visibilityKeys_[context]=project.previewVisibilityKeys[context];
    }
    // An invalidated morph is fitted lazily, but its old optional shapes need
    // not occupy RAM until the playhead reaches the morph again.
    if (morph_ && !morphKey_.startsWith(project.previewMorphKey+":")) morph_->trimCache(0);
}
void CaptureWorker::clearColours() {colours_.clear();colourOrder_.clear();colourBytes_=0;colourKey_.clear();}
void CaptureWorker::setColourCacheBudget(size_t bytes) {
    colourBudget_=std::min(bytes,ColourBudget);
    while (colourBytes_>colourBudget_ && !colourOrder_.empty()) evictColour();
}
size_t CaptureWorker::evictColour() {
    if (colourOrder_.empty()) return 0;
    const auto it=colours_.find(colourOrder_.front());const size_t bytes=it->second.bytes();
    colourBytes_-=bytes;colours_.erase(it);colourOrder_.pop_front();return bytes;
}
void CaptureWorker::relieveMemoryPressure() {
    const auto allowed=memory_.inspect();if (!allowed.reclaim) return;
    cacheLimited_=true;emit cacheReduced(generation_);
    uint64_t remaining=allowed.reclaim;
    auto released=[&](uint64_t bytes) {remaining=bytes>=remaining ? 0 : remaining-bytes;};
    while (remaining && !colourOrder_.empty()) released(evictColour());
    // Retain small visibility masks and costly shapes when old colour alone is
    // enough. Published frames and aliased shapes stay alive for their consumers.
    if (remaining && morph_) {const auto bytes=morph_->cachedBytes();morph_->trimCache(bytes>remaining ? size_t(bytes-remaining) : 0);released(bytes-morph_->cachedBytes());}
    while (remaining && !visibilityOrder_.empty()) {
        const auto it=visibility_.find(visibilityOrder_.front());visibilityOrder_.pop_front();
        if (it!=visibility_.end()) {const auto bytes=it->second.bytes();visibilityBytes_-=bytes;--visibilityStageFrames_[it->first.second];visibilityStageBytes_[it->first.second]-=bytes;if (!it->first.second) forgetExportVisibility(it->first.first);visibility_.erase(it);released(bytes);}
    }
    if (remaining) {const auto bytes=reusableFrameBytes();framePool_={};morphScratch_={};released(bytes);}
    if (remaining) {pruneScores_.clear();if (vgs_) vgs_->releaseCache();}
}
void CaptureWorker::applyDespill(RenderFrame &out,const CompiledModifiers &modifiers,bool keep) {
#ifndef VGS_VIEWER
    CaptureSettings settings;settings.despillStrength=modifiers.despillStrength;settings.greenGain=modifiers.greenGain;
    settings.viewChromaScale=modifiers.viewChroma;settings.recoverSkin=modifiers.recoverSkin;
    const auto key=QJsonDocument(QJsonObject{{"strength",settings.despillStrength},{"gain",settings.greenGain},
        {"chroma",settings.viewChromaScale},{"skin",settings.recoverSkin}}).toJson(QJsonDocument::Compact);
    if (key!=colourKey_) {clearColours();colourKey_=key;}
    const ColourId id{out.seconds,out.chunkIndex,out.coefficients};
    const auto hit=colours_.find(id);
    if (hit!=colours_.end() && hit->second.rgb.size()==out.records.size()*3) {
        const auto &rgb=hit->second.rgb;
        for (size_t i=0;i<out.records.size();++i) std::copy_n(rgb.data()+3*i,3,out.records[i].color);
        for (auto &p:out.points) std::copy_n(rgb.data()+size_t(p.id)*3,3,p.color);
        despillSh(out.sh,settings);out.colourFromCache=true;
        colourOrder_.splice(colourOrder_.end(),colourOrder_,hit->second.order);
        return;
    }
    if (hit!=colours_.end()) {colourBytes_-=hit->second.bytes();colourOrder_.erase(hit->second.order);colours_.erase(hit);}
    if (keep && cachingEnabled_) emit cacheComputing(2,generation_,processingRevision_);
    ++despillEvaluations_;
    vgs::Frame f;f.count=out.records.size();f.active=out.active;f.shCoefficients=out.coefficients;f.shRest.swap(out.sh);
    f.position.reserve(size_t(f.count)*3);f.colorDc.reserve(size_t(f.count)*3);f.opacity.reserve(size_t(f.count));
    for (const auto &r:out.records) {f.position.insert(f.position.end(),r.position,r.position+3);f.colorDc.insert(f.colorDc.end(),r.color,r.color+3);f.opacity.push_back(r.color[3]);}
    despillFrame(f,settings);
    for (size_t i=0;i<out.records.size();++i) std::copy_n(f.colorDc.data()+i*3,3,out.records[i].color);
    for (auto &p:out.points) std::copy_n(f.colorDc.data()+size_t(p.id)*3,3,p.color);
    out.sh.swap(f.shRest);
    // Move the computed RGB into retention: no extra full-frame copy. SH stays
    // in the reusable render frame and is transformed afresh on a colour hit.
    Colour colour;colour.rgb=std::move(f.colorDc);const size_t bytes=colour.bytes();
    if (!keep || !cachingEnabled_) return;
    if (bytes>colourBudget_ || !memory_.canRetain(bytes)) {cacheLimited_=true;return;}
    while (colourBytes_>colourBudget_-bytes && !colourOrder_.empty()) {cacheLimited_=true;evictColour();}
    colourOrder_.push_back(id);colour.order=std::prev(colourOrder_.end());
    colourBytes_+=bytes;colours_.emplace(id,std::move(colour));
#else
    Q_UNUSED(out);Q_UNUSED(modifiers);Q_UNUSED(keep);
#endif
}
void CaptureWorker::clearVisibility(int context) {
    if (context<=0) forgetExportVisibility();
    if (context<0) {visibilityKeys_={};visibility_.clear();visibilityOrder_.clear();visibilityBytes_=0;visibilityStageFrames_={};visibilityStageBytes_={};return;}
    for (auto it=visibility_.begin();it!=visibility_.end();) if (it->first.second==context) {visibilityBytes_-=it->second.bytes();it=visibility_.erase(it);} else ++it;
    visibilityOrder_.erase(std::remove_if(visibilityOrder_.begin(),visibilityOrder_.end(),[context](const auto &id) {return id.second==context;}),visibilityOrder_.end());
    visibilityKeys_[context].clear();
    visibilityStageFrames_[context]=visibilityStageBytes_[context]=0;
}
bool CaptureWorker::cachedVisibility(RenderFrame &out,const Project &project,int context) {
    Q_UNUSED(project);if (!cachingEnabled_) return false;
    const auto hit=visibility_.find({int(std::lround(out.seconds*info_.fps)),context});
    if (hit==visibility_.end()) return false;
    size_t count=0;for (uint32_t run:hit->second.mask->runs) count+=run;
    if (count!=out.points.size()) return false;
    size_t at=0;bool visible=true;
    for (uint32_t run:hit->second.mask->runs) {for (uint32_t k=0;k<run;++k) out.points[at++].modifierVisibility=visible ? 1.f : 0.f;visible=!visible;}
    out.greensApplied=hit->second.greensApplied;out.prune=hit->second.prune;out.visibilityFromCache=true;return true;
}
void CaptureWorker::keepVisibility(const RenderFrame &out,const Project &project,int context) {
    if (!project.previewPlaying || !cachingEnabled_) return;
    if (!memory_.canRetain((out.points.size()+1)*sizeof(uint32_t)+sizeof(Visibility)+128)) {cacheLimited_=true;return;}
    Visibility kept;kept.greensApplied=out.greensApplied;kept.prune=out.prune;bool visible=true;uint32_t run=0;
    auto mask=std::make_shared<ExportVisibilityMask>();mask->records=out.records.size();mask->points=out.points.size();
    for (const auto &p:out.points) {if ((p.modifierVisibility>=.5f)!=visible) {mask->runs.push_back(run);run=0;visible=!visible;}++run;}
    mask->runs.push_back(run);mask->runs.shrink_to_fit();kept.mask=mask;
    const VisibilityId id{int(std::lround(out.seconds*info_.fps)),context};
    auto &slot=visibility_[id];const auto replaced=slot.mask ? slot.bytes() : 0;visibilityBytes_-=replaced;visibilityStageBytes_[context]-=replaced;
    if (!slot.mask) {visibilityOrder_.push_back(id);++visibilityStageFrames_[context];}
    slot=std::move(kept);visibilityBytes_+=slot.bytes();visibilityStageBytes_[context]+=slot.bytes();
    if (!context && std::any_of(project.modifiers.begin(),project.modifiers.end(),[](const Modifier &m) {return m.active() && m.type==ModifierType::PurgeIsolated;})) {
        std::lock_guard<std::mutex> lock(exportStore_->mutex);exportStore_->key=visibilityKeys_[0];exportStore_->frames[id.first]=mask;
    }
    while (visibilityBytes_>VisibilityBudget && !visibilityOrder_.empty()) {
        cacheLimited_=true;
        const auto oldest=visibility_.find(visibilityOrder_.front());visibilityOrder_.pop_front();
        if (oldest!=visibility_.end()) {visibilityBytes_-=oldest->second.bytes();--visibilityStageFrames_[oldest->first.second];visibilityStageBytes_[oldest->first.second]-=oldest->second.bytes();if (!oldest->first.second) forgetExportVisibility(oldest->first.first);visibility_.erase(oldest);}
    }
}
size_t CaptureWorker::Visibility::bytes() const {return sizeof(Visibility)+sizeof(ExportVisibilityMask)+(mask ? mask->runs.capacity()*sizeof(uint32_t) : 0)+prune.size()*sizeof(PruneStats)+160;}
const std::vector<float> &CaptureWorker::pruneScores(size_t chunk) {
    if (auto it = pruneScores_.find(chunk); it != pruneScores_.end()) return it->second;
    double start, end;
#ifndef VGS_VIEWER
    if (ply_) {const auto &c=ply_->chunks().at(qsizetype(chunk));start=c.first/info_.fps;end=(c.first+c.frames)/info_.fps;} else
#endif
    if (mint_) { const auto &c = mint_->chunks().at(qsizetype(chunk)); start = c.start; end = c.start + c.duration; }
    else { const auto &c = vgs_->chunk(chunk); start = c.startSeconds; end = c.endSeconds; }
    std::vector<vgs::Frame> samples;
    for (double t : pruneSampleTimes(start, end)) {
        const auto f = frame(t, false);
        if (f->chunkIndex != chunk) throw std::runtime_error("Cannot sample the chunk to prune it.");
        vgs::Frame s; s.count = f->total; s.active = f->active;
        for (const auto &r : f->records) {
            s.position.insert(s.position.end(), r.position, r.position + 3); s.rotation.insert(s.rotation.end(), r.rotation, r.rotation + 4);
            s.scale.insert(s.scale.end(), r.scale, r.scale + 3); s.opacity.push_back(r.color[3]);
        }
        samples.push_back(std::move(s));
    }
    return pruneScores_[chunk] = contributionScores(samples, info_.antialiased);
}

FramePtr CaptureWorker::reusableFrame() {
    FramePtr out;
    for (auto &slot:framePool_) {
        if (!cachingEnabled_) break;
        if (!slot) {if (!memory_.canRetain(sizeof(RenderFrame))) break;slot=std::make_shared<RenderFrame>();}
        if (slot.use_count()==1) {out=slot;break;}
    }
    if (!out) out=std::make_shared<RenderFrame>();
    out->records.clear();out->active.clear();out->points.clear();out->morphMarkers.clear();out->prune.clear();out->greensApplied=false;out->visibilityFromCache=false;
    out->sourceMs=out->assemblyMs=0;out->morphShapeFromCache=false;out->colourFromCache=false;out->shBlend={};
    out->cache={};
    return out;
}
FramePtr CaptureWorker::frame(double time, bool includeSh) {
    QElapsedTimer timer; timer.start();
    time = std::clamp(time, 0.0, std::max(0.0, info_.duration - 1e-7));
    const float *p, *r, *s, *c, *o, *sh = nullptr;
    const uint8_t *a; uint64_t count; int coefficients = 0; size_t chunkIndex = 0;
    auto out=reusableFrame();
    MintFrame mintFrame;
#ifndef VGS_VIEWER
    vgs::Frame plyFrame;
    if (ply_) {
        plyFrame=ply_->frame(time,includeSh);const auto &f=plyFrame;
        p=f.position.data();r=f.rotation.data();s=f.scale.data();c=f.colorDc.data();o=f.opacity.data();a=f.active.data();count=f.count;
        sh=f.shRest.empty() ? nullptr : f.shRest.data();coefficients=f.shCoefficients;chunkIndex=size_t(f.chunkIndex);
    } else
#endif
    if (mint_) {
        QString error;
        if (!mint_->decode(time, &mintFrame, includeSh, &error, &out->sh)) throw std::runtime_error(error.toStdString());
        p = mintFrame.position.constData(); r = mintFrame.rotation.constData();
        s = mintFrame.scale.constData(); c = mintFrame.colorDc.constData();
        o = mintFrame.opacity.constData(); a = mintFrame.active.constData(); count = mintFrame.count;
        chunkIndex = size_t(mintFrame.chunkIndex);
        if (includeSh && !out->sh.empty()) { sh = out->sh.data(); coefficients = 15; }
    } else {
        const auto &f = vgs_->setTime(time, includeSh);
        p = f.positions; r = f.rotations; s = f.scales; c = f.colors;
        o = f.opacities; a = f.active; count = f.splatCount;
        sh = f.sphericalHarmonics; coefficients = sh ? f.shCoefficients : 0;
        chunkIndex = f.chunkIndex;
    }
    if (count > 10000000) throw std::runtime_error("The capture exceeds the limit of 10 million Gaussian records per frame.");
    out->sourceMs=timer.nsecsElapsed()/1e6;
    out->seconds = time; out->total = count; out->coefficients = coefficients;
    out->chunkIndex = chunkIndex;
    out->records.reserve(size_t(count)); out->active.reserve(size_t(count));
    out->points.reserve(size_t(count));
    if (!mint_) {if (coefficients) out->sh.assign(sh, sh + size_t(count) * size_t(coefficients) * 3);else out->sh.clear();}
    for (uint64_t i = 0; i < count; ++i) {
        Splat v{}; bool valid = true;
        for (int j = 0; j < 3; ++j) {
            v.position[j] = p[i*3+j]; v.scale[j] = s[i*3+j]; v.color[j] = c[i*3+j];
            valid &= std::isfinite(v.position[j]) && std::isfinite(v.color[j]);
        }
        for (int j = 0; j < 4; ++j) v.rotation[j] = r[i*4+j];
        v.color[3] = o[i]; v.id = float(i);
        out->records.push_back(v); out->active.push_back(a ? a[i] : 1);
        if (valid && (!a || a[i])) {
            PointVertex point{};
            std::copy_n(v.position,3,point.position); std::copy_n(v.color,3,point.color);
            point.id = v.id; out->points.push_back(point);
        }
    }
    out->decodeMs = timer.nsecsElapsed()/1e6;
    out->assemblyMs=out->decodeMs-out->sourceMs;
    trimFramePool();
    return out;
}
void CaptureWorker::openRequest(const QString &path, quint64 generation, bool sh, quint64 request) {
    const QScopedValueRollback<quint64> scope(request_,request);open(path,generation,sh);
}
#ifndef VGS_VIEWER
void CaptureWorker::openSourceRequest(const QString &path,quint64 generation,bool sh,quint64 request,Project project) {
    openingPly_=project.plySequence;openingPlyFps_=project.captureSettings.plyFps;openingPlyOffset_=project.captureSettings.plyRowOffset;
    openRequest(path,generation,sh,request);openingPly_={};openingPlyFps_=30;openingPlyOffset_=0;
}
#endif
void CaptureWorker::decodeRequest(double time, quint64 generation, bool sh, Project project, quint64 request) {
    const QScopedValueRollback<quint64> scope(request_,request);decode(time,generation,sh,std::move(project));
}
void CaptureWorker::open(const QString &path, quint64 generation, bool sh) {
    // Build a candidate first: a failed open leaves the existing capture usable.
    CaptureWorker candidate;
        candidate.cachingEnabled_=cachingEnabled_;
    try {
        const QString ext = QFileInfo(path).suffix().toLower();
        const double *bounds;
        candidate.info_.path = QFileInfo(path).absoluteFilePath();
        candidate.info_.title = QFileInfo(path).completeBaseName();
        candidate.info_.format = ext.toUpper();
#ifndef VGS_VIEWER
        if (ext=="ply") {
            auto saved=openingPly_;if (!saved.empty()) saved.rowOffset=openingPlyOffset_;
            candidate.ply_=std::make_unique<PlySequence>(path,saved,openingPlyFps_,[&](int done,int total,const QString &file) {emit sourceProgress(done,total,file,generation,request_);});
            candidate.info_.plySequence=candidate.ply_->spec();candidate.info_.fps=candidate.ply_->fps();candidate.info_.duration=candidate.ply_->frames()/candidate.info_.fps;
            candidate.info_.shDegree=candidate.ply_->degree();bounds=candidate.ply_->bounds();
        } else
#endif
        if (ext == "mint") {
            candidate.mint_ = std::make_unique<MintFile>(); QString error;
            if (!candidate.mint_->openReadOnly(path, &error)) throw std::runtime_error(error.toStdString());
            candidate.info_.duration = candidate.mint_->duration(); candidate.info_.fps = candidate.mint_->frameRate();
            candidate.info_.shDegree = candidate.mint_->shDegree();
            bounds = candidate.mint_->boundingBox();
            candidate.info_.antialiased = true;
            candidate.info_.audio = candidate.mint_->audio();
            if (!candidate.info_.audio.isEmpty()) candidate.info_.audioSuffix = QStringLiteral("mp3");
        } else if (ext == "vgs" || ext == "pgs") {
            candidate.source_ = std::make_unique<FileSource>(path);
            candidate.vgs_ = std::make_unique<vgsdec::Capture>(vgsdec::Capture::openStream(*candidate.source_));
            candidate.vgs_->setCachePolicy(cachingEnabled_ ? vgsdec::CachePolicy{1,1,512ull*1024*1024} : vgsdec::CachePolicy{0,0,1});
            candidate.vgs_->setThreadCount(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
            candidate.info_.duration = candidate.vgs_->duration(); candidate.info_.fps = candidate.vgs_->frameRate();
            candidate.info_.shDegree = int(candidate.vgs_->shDegree());
            if (!candidate.vgs_->metadata().title.empty()) candidate.info_.title = QString::fromStdString(candidate.vgs_->metadata().title);
            bounds = candidate.vgs_->bounds();
            candidate.info_.antialiased = candidate.vgs_->antialiased();
            candidate.info_.startSeconds = candidate.vgs_->startSeconds();
            if (candidate.vgs_->hasAudio()) {
                const auto track = candidate.vgs_->audio();
                candidate.info_.audio = QByteArray(reinterpret_cast<const char *>(track.data()), qsizetype(track.size()));
                using Format = vgsdec::Capture::AudioFormat;
                const auto format = candidate.vgs_->audioFormat();
                candidate.info_.audioSuffix = format == Format::Mp3 ? "mp3" : format == Format::Aac ? "m4a" : format == Format::Opus ? "opus" : format == Format::Wav ? "wav" : "";
            }
#ifdef VGS_VIEWER
        } else throw std::runtime_error("Unsupported format. Open a .vgs, .pgs or .mint file.");
#else
        } else throw std::runtime_error("Unsupported format. Open a .vgs, .pgs, .mint or .ply file.");
#endif
        if (!std::isfinite(candidate.info_.duration) || candidate.info_.duration <= 0 ||
            !std::isfinite(candidate.info_.fps) || candidate.info_.fps <= 0 ||
            candidate.info_.duration * candidate.info_.fps > std::numeric_limits<int>::max() - 1)
            throw std::runtime_error("The capture has an invalid timeline.");
        candidate.info_.frames = std::max(1, int(std::ceil(candidate.info_.duration * candidate.info_.fps - 1e-6)));
#ifndef VGS_VIEWER
        if (candidate.ply_) {
            for (int i=1;i<candidate.ply_->chunks().size();++i) candidate.info_.chunkBoundaries.append(candidate.ply_->chunks()[i].first/candidate.info_.fps);
        } else if (candidate.mint_) {
            const auto &chunks = candidate.mint_->chunks();
            for (qsizetype i = 1; i < chunks.size(); ++i)
                candidate.info_.chunkBoundaries.append(chunks[i].start);
        } else {
            for (size_t i = 1; i < candidate.vgs_->chunkCount(); ++i)
                candidate.info_.chunkBoundaries.append(candidate.vgs_->chunk(i).startSeconds);
        }
#endif
        for (int j = 0; j < 3; ++j) {
            if (!std::isfinite(bounds[j]) || !std::isfinite(bounds[j+3]) || bounds[j] > bounds[j+3])
                throw std::runtime_error("The capture has invalid spatial bounds.");
            candidate.info_.minimum[j] = float(bounds[j]); candidate.info_.maximum[j] = float(bounds[j+3]);
        }
        auto first = candidate.frame(0, sh);
        vgs_.reset(); source_.reset(); mint_.reset();
        source_ = std::move(candidate.source_); vgs_ = std::move(candidate.vgs_); mint_ = std::move(candidate.mint_);
#ifndef VGS_VIEWER
        ply_=std::move(candidate.ply_);
#endif
        framePool_={};clearColours();dependencyRevision_=0;dependencyMorphKey_.clear();info_ = candidate.info_; generation_ = generation; pruneScores_.clear(); morph_.reset();morphScratch_={}; morphKey_.clear(); clearVisibility();
        {const QFileInfo file(path);std::lock_guard<std::mutex> lock(exportStore_->mutex);exportStore_->asset=file.canonicalFilePath();exportStore_->fileSize=file.size();exportStore_->modified=file.lastModified().toMSecsSinceEpoch();}
        memoryTimer_->start();relieveMemoryPressure();
        emit opened(info_, first, generation);
        emit openedRequest(info_, first, generation, request_);
    } catch (const std::exception &e) {
        emit failed(QString::fromUtf8(e.what()), generation, true);
        emit failedRequest(QString::fromUtf8(e.what()), generation, true, request_);
    }
}
void CaptureWorker::decode(double time, quint64 generation, bool sh,Project project) {
    if (generation != generation_) { emit failed("Capture replaced.", generation, false);emit failedRequest("Capture replaced.",generation,false,request_);return; }
    try {
#ifndef VGS_VIEWER
        if (ply_) {
            if (project.captureSettings.plyRowOffset!=ply_->spec().rowOffset) {
                auto spec=project.plySequence.empty() ? ply_->spec() : project.plySequence;spec.rowOffset=project.captureSettings.plyRowOffset;spec.chunks.clear();
                auto replacement=std::make_unique<PlySequence>(info_.path,spec,project.captureSettings.plyFps);
                ply_=std::move(replacement);info_.plySequence=ply_->spec();info_.chunkBoundaries.clear();
                for (int start:ply_->spec().chunks) if (start>0) info_.chunkBoundaries.append(start/ply_->fps());
                project.plySequence=info_.plySequence;clearVisibility();clearColours();pruneScores_.clear();morph_.reset();morphKey_.clear();framePool_={};
                emit sourceReindexed(info_,generation);
            }
            if (ply_->fps()!=project.captureSettings.plyFps) clearColours();
            ply_->setFps(project.captureSettings.plyFps);info_.fps=ply_->fps();info_.duration=ply_->frames()/info_.fps;
        }
#endif
        preparePreview(project);
        relieveMemoryPressure();
        const auto out=evaluate(time,sh,project);
        emit decoded(out,generation);emit decodedRequest(out,generation,request_);
        // Decode the next VGS chunk in small pieces while this one is on screen.
        // Both the viewer and editor use this worker; doing it here keeps a chunk
        // boundary from forcing the next frame to decompress the entire chunk.
        if (cachingEnabled_ && vgs_ && memory_.canRetain(8*previewmemory::MiB)) {
            size_t next=out->chunkIndex==RenderFrame::MorphChunk ? vgs_->chunkAt(project.in) : out->chunkIndex+1;
            if (project.previewPlaying && project.loop && (next>=vgs_->chunkCount() ||
                (project.out>project.in && vgs_->chunk(next).startSeconds>project.out))) next=vgs_->chunkAt(project.in);
            const double budget=std::clamp(1000/info_.fps-out->decodeMs-4.0,3.0,8.0);
            try { if (next<vgs_->chunkCount() && next!=out->chunkIndex) vgs_->prepare(next,budget,sh ? vgsdec::Detail::Full : vgsdec::Detail::Base); }
            catch (const std::exception &) { /* The normal frame request reports bad data. */ }
        }
        relieveMemoryPressure();
    }
    catch (const std::exception &e) { emit failed(QString::fromUtf8(e.what()), generation, false);emit failedRequest(QString::fromUtf8(e.what()),generation,false,request_); }
}
FramePtr CaptureWorker::evaluate(double time,bool sh,const Project &project) {
    auto out=project.inMorph(time,info_.fps) ? morphFrame(time,sh,project) : processed(time,sh,project,project.modifiersAtFrame(std::round(time*info_.fps)),true);
    describeCache(*out,project,sh);if (!cachingEnabled_) {pruneScores_.clear();morphScratch_={};}return out;
}
void CaptureWorker::describeCache(RenderFrame &out,const Project &project,bool sh) {
    auto &cache=out.cache;cache.frame=int(std::lround(out.seconds*info_.fps));cache.revision=project.previewRevision;
    cache.enabled=cachingEnabled_;cache.morphTotalFrames=project.morphFrames(info_.fps);
    cache.timelineFrames=std::max(0,int(std::lround((project.playbackEnd(info_.fps)-project.in)*info_.fps))+1);
    cache.morph=out.chunkIndex==RenderFrame::MorphChunk;
    cache.visibility=visibility_.count({cache.frame,cache.morph ? 2 : 0})!=0;
    cache.colour=colours_.count({out.seconds,out.chunkIndex,out.coefficients})!=0;
    const bool validMorph=morph_ && morphKey_==project.previewMorphKey+":"+QByteArray::number(sh);
    if (validMorph) {cache.morphFrames=morph_->cachedFrames();cache.morphBytes=morph_->cachedBytes();}
    if (cache.morph && morph_) {
        double fraction=0;int source=0;project.inMorph(out.seconds,info_.fps,&fraction,&source);
        cache.shape=morph_->hasCachedShape(fraction,size_t(int(std::lround(project.out*info_.fps))-source));
        if (!cache.shape) cacheLimited_=true;
    }
    describeRetention(cache);lastCacheState_=cache;
}
void CaptureWorker::describeRetention(PreviewCacheState &cache) {
    cache.enabled=cachingEnabled_;cache.visibilityFrames=visibility_.size();cache.visibilityBytes=visibilityBytes_;
    cache.visibilityStageFrames=visibilityStageFrames_;cache.visibilityStageBytes=visibilityStageBytes_;
    cache.reusableBytes=reusableFrameBytes();cache.colourFrames=colours_.size();cache.colourBytes=colourBytes_;
    cache.morphFrames=morph_ ? morph_->cachedFrames() : 0;cache.morphBytes=morph_ ? morph_->cachedBytes() : 0;
    cache.memoryLimited=memory_.inspect().paused;
    cache.visibility=visibility_.count({cache.frame,cache.morph ? 2 : 0})!=0;
}
void CaptureWorker::inspectCache(quint64 generation,quint64 revision) {
    if (generation!=generation_ || revision!=lastCacheState_.revision) return;
    auto state=lastCacheState_;describeRetention(state);emit cacheStateReady(state,generation);
}
void CaptureWorker::precalculate(Project project,quint64 generation,quint64 request) {
    int done=0,total=0;QString error;cacheLimited_=false;
    QScopedValueRollback<quint64> running(activeCacheRequest_,request);
    try {
        if (generation!=generation_) throw std::runtime_error("Capture replaced.");
        preparePreview(project);
        if (!cachingEnabled_) throw std::runtime_error("Enable cache before baking it.");
        project.previewPlaying=true;
        for (qsizetype i=0;i<std::as_const(project.modifiers).size();++i) if (std::as_const(project.modifiers)[i].showErased) project.modifiers[i].showErased=false;
        const int first=int(std::lround(project.in*info_.fps)),last=int(std::lround(project.playbackEnd(info_.fps)*info_.fps));
        total=std::max(0,last-first+1);emit cacheProgress(0,total,generation,request);
        QElapsedTimer progress;progress.start();
        for (int f=first;f<=last && cancelCacheRequest_.load()!=request;++f) {
            relieveMemoryPressure();
            {const auto frame=evaluate(f/info_.fps,true,project);}
            ++done;
            if (progress.elapsed()>=80 || done==total) {emit cacheProgress(done,total,generation,request);progress.restart();}
        }
        relieveMemoryPressure();
    } catch (const std::exception &e) {if (cancelCacheRequest_.load()!=request) error=QString::fromUtf8(e.what());}
    emit cacheFinished(done,total,cacheLimited_,error,generation,request);
}
// A source frame with the modifiers of `stack` decided on it, as the viewport needs them:
// despilled when a Colour modifier despills always, and the splats that pruning, Erase,
// Purge Isolated (and Remove green, when the colours shown are despilled) remove marked.
FramePtr CaptureWorker::processed(double time,bool sh,const Project &project,const QVector<Modifier> &stack,bool cached,int context) {
    {
        QElapsedTimer processingTimer;processingTimer.start();auto out=frame(time,sh);CompiledModifiers modifiers(stack);
        // Answered by the cache when nothing has changed since the frame was played. While
        // Erase marks its picks rather than removing them, the frame is not one played.
        const bool masks=!modifiers.prunes.isEmpty() || !modifiers.erased.empty() || !modifiers.isolations.isEmpty() || (modifiers.despillPreview && !modifiers.greens.isEmpty());
        const bool cacheable=cached && masks && !modifiers.showErased;
        const bool answered=cacheable && cachedVisibility(*out,project,context);
        if (cached && !masks && context==0) clearVisibility(0);
        if (cacheable && !answered && cachingEnabled_ && project.previewPlaying) emit cacheComputing(1,generation_,processingRevision_);
        // A Colour modifier that despills always: the export's despill on what is shown. The
        // source's colours are kept for Remove green above it, which tests them, not these.
        std::vector<std::array<float,3>> sourceColours;
        if (modifiers.despillPreview && !modifiers.greens.isEmpty()) {sourceColours.resize(out->points.size());for (size_t i=0;i<out->points.size();++i) std::copy_n(out->points[i].color,3,sourceColours[i].data());}
        if (modifiers.despillPreview) applyDespill(*out,modifiers,project.previewPlaying);
        else if (context==0) clearColours();
        if (answered) {out->decodeMs=processingTimer.nsecsElapsed()/1e6;return out;}
        // Pruning is decided for the whole chunk; Purge Isolated then sees only what it keeps.
        std::vector<uint8_t> pruned;
        if (!modifiers.prunes.isEmpty()) pruned=pruneKeep(pruneScores(out->chunkIndex),modifiers.prunes,&out->prune);
        // Erase: what was picked by hand in this chunk goes too - or, while its picks are being
        // edited, stays and is marked (visibility 0.75, drawn highlighted).
        const bool erasing=!modifiers.erased.empty();
        auto erasedHere=[&](const PointVertex &p) {return erasing && modifiers.erases(int(out->chunkIndex),uint32_t(p.id));};
        auto unpruned=[&](const PointVertex &p) {return (pruned.empty() || pruned[size_t(p.id)]) && (modifiers.showErased || !erasedHere(p));};
        const bool greensHere=!sourceColours.empty();out->greensApplied=greensHere;
        auto sourceRgb=[&](size_t i) {const auto &p=out->points[i];return greensHere ? QVector3D(sourceColours[i][0],sourceColours[i][1],sourceColours[i][2]) : QVector3D(p.color[0],p.color[1],p.color[2]);};
        auto greenGone=[&](size_t i) {const auto &p=out->points[i];return greensHere && modifiers.removesColour(sourceRgb(i),{p.color[0],p.color[1],p.color[2]});};
        if (!modifiers.isolations.isEmpty()) {
            const auto model=project.transformAtFrame(std::round(out->seconds*info_.fps)).matrix();std::vector<QVector3D> positions(out->points.size());std::vector<uint8_t> keep(out->points.size());
            for (size_t i=0;i<positions.size();++i) {const auto &p=out->points[i];positions[i]=model.map({p.position[0],p.position[1],p.position[2]});keep[i]=unpruned(p) && modifiers.keeps(positions[i],sourceRgb(i),{p.color[0],p.color[1],p.color[2]});}
            ++isolationEvaluations_;applyIsolation(positions,keep,modifiers.isolations);for (size_t i=0;i<keep.size();++i) out->points[i].modifierVisibility=keep[i] ? 1.f : 0.f;
        } else if (!pruned.empty() || erasing || greensHere) for (size_t i=0;i<out->points.size();++i) out->points[i].modifierVisibility=unpruned(out->points[i]) && !greenGone(i) ? 1.f : 0.f;
        if (erasing && modifiers.showErased) for (auto &p:out->points) if (p.modifierVisibility>=.5f && erasedHere(p)) p.modifierVisibility=.75f;
        if (cacheable) keepVisibility(*out,project,context);
        out->decodeMs=processingTimer.nsecsElapsed()/1e6;
        return out;
    }
}
// One of a Loop morph's frames. The morph is fitted once for the End and Start frames as the
// modifiers above it leave them, and kept until any of that changes. Overlapping, it is also
// followed back through the frames before End (and before Start, leading in), and each of its
// frames carries the capture's own frame there.
FramePtr CaptureWorker::morphFrame(double time,bool sh,const Project &project) {
    QElapsedTimer timer;timer.start();
    const double fps=info_.fps;const int endFrame=int(std::lround(project.out*fps)),startFrame=int(std::lround(project.in*fps));
    double fraction=0;int sourceFrame=endFrame;project.inMorph(time,fps,&fraction,&sourceFrame);
    const bool overlaps=project.morphOverlaps(),leadIn=project.morphLeadsIn(fps);const int frames=project.morphFrames(fps);
    const size_t back=size_t(endFrame-sourceFrame);
    const QMatrix4x4 endModel=project.transformAtFrame(endFrame).matrix(),startModel=project.transformAtFrame(startFrame).matrix();
    bool invertible=false;const QMatrix4x4 unEnd=endModel.inverted(&invertible);
    if (!invertible) throw std::runtime_error("The loop morph needs an invertible transform at End.");
    // A frame's capture coordinates carried into End's: its transform, then End's taken back out.
    auto intoEnd=[&](int frame) {return unEnd*project.transformAtFrame(frame).matrix();};
    auto above=[&](int frame) {
        auto stack=project.modifiersAboveMorph(frame);
        for (qsizetype i=0;i<std::as_const(stack).size();++i) if (std::as_const(stack)[i].showErased) stack[i].showErased=false;
        return stack;
    };
    const QByteArray key=project.previewMorphKey+":"+QByteArray::number(sh);
    bool collectSources=false,sourceVisibility=true,sourceColour=true;
    auto cloud=[&](int frame,const QMatrix4x4 &into) {
        const auto stack=above(frame);
        const auto f=processed(frame/fps,sh,project,stack,true,1);const CompiledModifiers compiled(stack);const QMatrix4x4 model=project.transformAtFrame(frame).matrix();
        if (collectSources) {
            sourceVisibility &= visibility_.count({frame,1})!=0;
            sourceColour &= colours_.count({f->seconds,f->chunkIndex,f->coefficients})!=0;
        }
        // The rotation and uniform scale `into` carries a splat's orientation and size by.
        Eigen::Matrix3d a;for (int r=0;r<3;++r) for (int c=0;c<3;++c) a(r,c)=into(r,c);
        Eigen::JacobiSVD<Eigen::Matrix3d> svd(a,Eigen::ComputeFullU|Eigen::ComputeFullV);Eigen::Matrix3d rotation=svd.matrixU()*svd.matrixV().transpose();
        if (rotation.determinant()<0) rotation=-rotation;
        const Eigen::Quaterniond turn(rotation);const float grow=float(std::cbrt(std::abs(a.determinant())));
        loopmorph::Cloud c;c.shCoefficients=f->coefficients;c.reserve(f->points.size());const size_t terms=size_t(c.shCoefficients)*3;
        for (const auto &point:f->points) {
            if (!keepsPreviewPoint(point,model,compiled,f->greensApplied)) continue;
            const auto &r=f->records[size_t(point.id)];const QVector3D p=into.map({r.position[0],r.position[1],r.position[2]});
            const Eigen::Quaterniond q=(turn*Eigen::Quaterniond(r.rotation[3],r.rotation[0],r.rotation[1],r.rotation[2]).normalized()).normalized();
            c.position.insert(c.position.end(),{p.x(),p.y(),p.z()});c.rotation.insert(c.rotation.end(),{float(q.x()),float(q.y()),float(q.z()),float(q.w())});
            c.scale.insert(c.scale.end(),{r.scale[0]*grow,r.scale[1]*grow,r.scale[2]*grow});c.colour.insert(c.colour.end(),{point.color[0],point.color[1],point.color[2]});c.opacity.push_back(r.color[3]);
            if (terms) c.sh.insert(c.sh.end(),f->sh.begin()+std::ptrdiff_t(terms*size_t(point.id)),f->sh.begin()+std::ptrdiff_t(terms*(size_t(point.id)+1)));
        }
        return c;
    };
    if (!morph_ || key!=morphKey_) {
        if (cachingEnabled_ && project.previewPlaying) emit cacheComputing(4,generation_,processingRevision_);
        morph_.reset();morphKey_.clear();emit morphFitting(true);
        struct Done {CaptureWorker *worker;~Done() {emit worker->morphFitting(false);}} done{this};
        // Both frames in the End frame's capture coordinates: its transform draws the morph.
        const QMatrix4x4 toEnd=intoEnd(startFrame);
        auto endCloud=cloud(endFrame,QMatrix4x4()),startCloud=cloud(startFrame,toEnd);
        std::vector<loopmorph::Pair> pairs;for (const auto &pair:project.morphPairs(fps)) pairs.push_back({pair.end,toEnd.map(pair.start)});
        const auto cancelled=[this] {return activeCacheRequest_ && cancelCacheRequest_.load()==activeCacheRequest_;};
        auto morph=std::make_unique<loopmorph::Morph>(std::move(endCloud),std::move(startCloud),pairs,cancelled);
        // Followed on every splat the source shows there, whatever the modifiers keep.
        auto followBack=[&](bool start,int from) {
            std::vector<loopmorph::Track> track;
            for (int k=0;k<=frames && from-k>=0;++k) {
                const auto f=frame((from-k)/fps,false);const QMatrix4x4 into=intoEnd(from-k);loopmorph::Track t;t.chunk=(long long)f->chunkIndex;
                for (const auto &point:f->points) {
                    const auto &r=f->records[size_t(point.id)];const QVector3D p=into.map({r.position[0],r.position[1],r.position[2]});
                    t.position.insert(t.position.end(),{p.x(),p.y(),p.z()});t.colour.insert(t.colour.end(),{r.color[0],r.color[1],r.color[2]});t.record.push_back(uint32_t(point.id));
                }
                track.push_back(std::move(t));
            }
            morph->follow(start,track,cancelled);
        };
        if (overlaps) {followBack(false,endFrame);if (leadIn) followBack(true,startFrame);}
        morph_=std::move(morph);morphKey_=key;
        morph_->setCaching(cachingEnabled_,PreviewBudget-VisibilityBudget-FrameBudget);
        morph_->setCacheAdmission([this](size_t bytes) {return memory_.canRetain(bytes);});
    }
    loopmorph::Cloud liveEnd,liveStart;loopmorph::Morph::Live live;
    collectSources=true;
    if (overlaps) {
        live.back=back;liveEnd=cloud(sourceFrame,intoEnd(sourceFrame));live.end=&liveEnd;
        if (leadIn) {liveStart=cloud(startFrame-int(back),intoEnd(startFrame-int(back)));live.start=&liveStart;}
    }
    auto out=reusableFrame();const size_t shapeHits=morph_->cacheHits();
    if (cachingEnabled_ && project.previewPlaying && !morph_->hasCachedShape(fraction,overlaps ? back : 0)) emit cacheComputing(4,generation_,processingRevision_);
    out->cache.sourceVisibility=!overlaps || sourceVisibility;out->cache.sourceColour=!overlaps || sourceColour;
    const bool gpuSh=project.previewPlaying && !overlaps && sh && morph_->evaluatePreviewInto(fraction,&morphScratch_,&out->shBlend);
    if (gpuSh) {std::vector<float>().swap(out->sh);std::vector<float>().swap(morphScratch_.sh);}
    else {morphScratch_.sh.swap(out->sh);morph_->evaluateInto(fraction,live,&morphScratch_);morphScratch_.sh.swap(out->sh);}
    const auto &c=morphScratch_;
    out->seconds=time;out->total=c.size();out->coefficients=c.shCoefficients;out->chunkIndex=RenderFrame::MorphChunk;
    out->morphShapeFromCache=morph_->cacheHits()!=shapeHits;
    const auto pairs=project.previewPlaying ? QVector<Project::MorphPair>{} : project.morphPairs(fps);std::vector<QVector3D> markerPoints;markerPoints.reserve(size_t(pairs.size()));
    for (const auto &pair:pairs) markerPoints.push_back(pair.end);
    const auto carried=morph_->carry(markerPoints,fraction,overlaps ? back : 0,leadIn);
    for (qsizetype i=0;i<pairs.size();++i) out->morphMarkers.push_back({pairs[i].number,carried[size_t(i)]});
    out->records.resize(c.size());out->active.assign(c.size(),1);out->points.resize(c.size());
    for (size_t i=0;i<c.size();++i) {
        auto &r=out->records[i];std::copy_n(c.position.data()+3*i,3,r.position);std::copy_n(c.rotation.data()+4*i,4,r.rotation);std::copy_n(c.scale.data()+3*i,3,r.scale);
        std::copy_n(c.colour.data()+3*i,3,r.color);r.color[3]=c.opacity[i];r.id=float(i);
        auto &p=out->points[i];std::copy_n(r.position,3,p.position);std::copy_n(r.color,3,p.color);p.id=r.id;
    }
    // Below the morph, Purge Isolated applies to its frames as to any other. Pruning and Erase
    // pick splats of source chunks, and these splats are in none.
    const CompiledModifiers below(project.modifiersBelowMorph(endFrame));
    if (!below.isolations.isEmpty() && !cachedVisibility(*out,project,2)) {
        std::vector<QVector3D> positions(out->points.size());std::vector<uint8_t> keep(out->points.size(),1);
        for (size_t i=0;i<positions.size();++i) {const auto &p=out->points[i];positions[i]=endModel.map({p.position[0],p.position[1],p.position[2]});keep[i]=below.keepsPosition(positions[i]);}
        ++isolationEvaluations_;applyIsolation(positions,keep,below.isolations);for (size_t i=0;i<keep.size();++i) out->points[i].modifierVisibility=keep[i] ? 1.f : 0.f;
    }
    if (!below.isolations.isEmpty() && !out->visibilityFromCache) keepVisibility(*out,project,2);
    trimFramePool();out->decodeMs=timer.nsecsElapsed()/1e6;
    return out;
}
