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
CaptureWorker::CaptureWorker(QObject *parent) : QObject(parent) {}
CaptureWorker::~CaptureWorker() = default;
void CaptureWorker::clear() {
    vgs_.reset(); source_.reset(); mint_.reset(); info_ = {}; generation_ = 0; pruneScores_.clear(); morph_.reset(); morphKey_.clear(); clearVisibility();
}
void CaptureWorker::clearVisibility() {visibilityKey_.clear();visibility_.clear();visibilityOrder_.clear();visibilityBytes_=0;}
size_t CaptureWorker::Visibility::bytes() const {return sizeof(Visibility)+runs.size()*sizeof(uint32_t)+prune.size()*sizeof(PruneStats)+64;}
const std::vector<float> &CaptureWorker::pruneScores(size_t chunk) {
    if (auto it = pruneScores_.find(chunk); it != pruneScores_.end()) return it->second;
    double start, end;
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

FramePtr CaptureWorker::frame(double time, bool includeSh) {
    QElapsedTimer timer; timer.start();
    time = std::clamp(time, 0.0, std::max(0.0, info_.duration - 1e-7));
    const float *p, *r, *s, *c, *o, *sh = nullptr;
    const uint8_t *a; uint64_t count; int coefficients = 0; size_t chunkIndex = 0;
    MintFrame mintFrame;
    if (mint_) {
        QString error;
        if (!mint_->decode(time, &mintFrame, includeSh, &error)) throw std::runtime_error(error.toStdString());
        p = mintFrame.position.constData(); r = mintFrame.rotation.constData();
        s = mintFrame.scale.constData(); c = mintFrame.colorDc.constData();
        o = mintFrame.opacity.constData(); a = mintFrame.active.constData(); count = mintFrame.count;
        chunkIndex = size_t(mintFrame.chunkIndex);
        if (includeSh) { sh = mintFrame.shRest.constData(); coefficients = 15; }
    } else {
        const auto &f = vgs_->setTime(time, includeSh);
        p = f.positions; r = f.rotations; s = f.scales; c = f.colors;
        o = f.opacities; a = f.active; count = f.splatCount;
        sh = f.sphericalHarmonics; coefficients = sh ? f.shCoefficients : 0;
        chunkIndex = f.chunkIndex;
    }
    if (count > 10000000) throw std::runtime_error("The capture exceeds the limit of 10 million Gaussian records per frame.");
    auto out = std::make_shared<RenderFrame>();
    out->seconds = time; out->total = count; out->coefficients = coefficients;
    out->chunkIndex = chunkIndex;
    out->records.reserve(size_t(count)); out->active.reserve(size_t(count));
    out->points.reserve(size_t(count));
    if (coefficients) out->sh.assign(sh, sh + size_t(count) * size_t(coefficients) * 3);
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
    return out;
}
void CaptureWorker::openRequest(const QString &path, quint64 generation, bool sh, quint64 request) {
    const QScopedValueRollback<quint64> scope(request_,request);open(path,generation,sh);
}
void CaptureWorker::decodeRequest(double time, quint64 generation, bool sh, Project project, quint64 request) {
    const QScopedValueRollback<quint64> scope(request_,request);decode(time,generation,sh,std::move(project));
}
void CaptureWorker::open(const QString &path, quint64 generation, bool sh) {
    // Build a candidate first: a failed open leaves the existing capture usable.
    CaptureWorker candidate;
    try {
        const QString ext = QFileInfo(path).suffix().toLower();
        const double *bounds;
        candidate.info_.path = QFileInfo(path).absoluteFilePath();
        candidate.info_.title = QFileInfo(path).completeBaseName();
        candidate.info_.format = ext.toUpper();
        if (ext == "mint") {
            candidate.mint_ = std::make_unique<MintFile>(); QString error;
            if (!candidate.mint_->open(path, &error)) throw std::runtime_error(error.toStdString());
            candidate.info_.duration = candidate.mint_->duration(); candidate.info_.fps = candidate.mint_->frameRate();
            candidate.info_.shDegree = 3;
            bounds = candidate.mint_->boundingBox();
            candidate.info_.antialiased = true;
        } else if (ext == "vgs" || ext == "pgs") {
            candidate.source_ = std::make_unique<FileSource>(path);
            candidate.vgs_ = std::make_unique<vgsdec::Capture>(vgsdec::Capture::openStream(*candidate.source_));
            candidate.vgs_->setCachePolicy({1, 1, 512ull * 1024 * 1024});
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
        } else throw std::runtime_error("Unsupported format. Open a .vgs, .pgs or .mint file.");
        if (!std::isfinite(candidate.info_.duration) || candidate.info_.duration <= 0 ||
            !std::isfinite(candidate.info_.fps) || candidate.info_.fps <= 0 ||
            candidate.info_.duration * candidate.info_.fps > std::numeric_limits<int>::max() - 1)
            throw std::runtime_error("The capture has an invalid timeline.");
        candidate.info_.frames = std::max(1, int(std::ceil(candidate.info_.duration * candidate.info_.fps - 1e-6)));
#ifndef VGS_VIEWER
        if (candidate.mint_) {
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
        info_ = candidate.info_; generation_ = generation; pruneScores_.clear(); morph_.reset(); morphKey_.clear(); clearVisibility();
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
        const auto out=project.inMorph(time,info_.fps) ? morphFrame(time,sh,project) : processed(time,sh,project,project.modifiersAtFrame(std::round(time*info_.fps)),true);
        emit decoded(out,generation);emit decodedRequest(out,generation,request_);
        // Decode the next VGS chunk in small pieces while this one is on screen.
        // Both the viewer and editor use this worker; doing it here keeps a chunk
        // boundary from forcing the next frame to decompress the entire chunk.
        if (vgs_ && out->chunkIndex + 1 < vgs_->chunkCount()) {
            try { vgs_->prepare(out->chunkIndex + 1, 3.0, vgsdec::Detail::Full); }
            catch (const std::exception &) { /* The normal frame request reports bad data. */ }
        }
    }
    catch (const std::exception &e) { emit failed(QString::fromUtf8(e.what()), generation, false);emit failedRequest(QString::fromUtf8(e.what()),generation,false,request_); }
}
// A source frame with the modifiers of `stack` decided on it, as the viewport needs them:
// despilled when a Colour modifier despills always, and the splats that pruning, Erase,
// Purge Isolated (and Remove green, when the colours shown are despilled) remove marked.
FramePtr CaptureWorker::processed(double time,bool sh,const Project &project,const QVector<Modifier> &stack,bool cached) {
    {
        QElapsedTimer processingTimer;processingTimer.start();auto out=frame(time,sh);CompiledModifiers modifiers(stack);
        // Answered by the cache when nothing has changed since the frame was played. While
        // Erase marks its picks rather than removing them, the frame is not one played.
        const bool masks=!modifiers.prunes.isEmpty() || !modifiers.erased.empty() || !modifiers.isolations.isEmpty() || (modifiers.despillPreview && !modifiers.greens.isEmpty());
        const bool cacheable=cached && masks && !modifiers.showErased;const int frameIndex=int(std::lround(out->seconds*info_.fps));
        if (cacheable) {
            const auto v=[](const QVector3D &x) {return QJsonArray{x.x(),x.y(),x.z()};};
            const QByteArray key=QJsonDocument(QJsonObject{{"modifiers",project.modifierJson()},{"processing",project.captureSettings.json()},
                {"transform",QJsonArray{v(project.transform.position),v(project.transform.rotation),v(project.transform.scale)}}}).toJson(QJsonDocument::Compact);
            if (key!=visibilityKey_) {clearVisibility();visibilityKey_=key;}
        } else if (cached && !masks) clearVisibility();
        const auto hit=cacheable ? visibility_.find(frameIndex) : visibility_.end();
        bool answered=false;
        if (hit!=visibility_.end()) {
            size_t total=0;for (const uint32_t run:hit->second.runs) total+=run;
            if (total==out->points.size()) {
                size_t at=0;bool visible=true;
                for (const uint32_t run:hit->second.runs) {for (uint32_t k=0;k<run;++k) out->points[at++].modifierVisibility=visible ? 1.f : 0.f;visible=!visible;}
                out->greensApplied=hit->second.greensApplied;out->prune=hit->second.prune;answered=true;
            }
        }
        // A Colour modifier that despills always: the export's despill on what is shown. The
        // source's colours are kept for Remove green above it, which tests them, not these.
        std::vector<std::array<float,3>> sourceColours;
        if (modifiers.despillPreview && !modifiers.greens.isEmpty()) {sourceColours.resize(out->points.size());for (size_t i=0;i<out->points.size();++i) std::copy_n(out->points[i].color,3,sourceColours[i].data());}
        if (modifiers.despillPreview) {
            vgs::Frame f;f.count=out->records.size();f.active=out->active;f.shCoefficients=out->coefficients;f.shRest=out->sh;
            for (const auto &r:out->records) {f.position.insert(f.position.end(),r.position,r.position+3);f.colorDc.insert(f.colorDc.end(),r.color,r.color+3);f.opacity.push_back(r.color[3]);}
            CaptureSettings settings;settings.despillStrength=modifiers.despillStrength;settings.greenGain=modifiers.greenGain;settings.viewChromaScale=modifiers.viewChroma;settings.recoverSkin=modifiers.recoverSkin;
#ifndef VGS_VIEWER
            despillFrame(f,settings);
#endif
            for (size_t i=0;i<out->records.size();++i) std::copy_n(f.colorDc.data()+i*3,3,out->records[i].color);
            for (auto &p:out->points) std::copy_n(f.colorDc.data()+size_t(p.id)*3,3,p.color);
            out->sh=f.shRest;
        }
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
            applyIsolation(positions,keep,modifiers.isolations);for (size_t i=0;i<keep.size();++i) out->points[i].modifierVisibility=keep[i] ? 1.f : 0.f;
        } else if (!pruned.empty() || erasing || greensHere) for (size_t i=0;i<out->points.size();++i) out->points[i].modifierVisibility=unpruned(out->points[i]) && !greenGone(i) ? 1.f : 0.f;
        if (erasing && modifiers.showErased) for (auto &p:out->points) if (p.modifierVisibility>=.5f && erasedHere(p)) p.modifierVisibility=.75f;
        if (cacheable && project.previewPlaying) {
            Visibility kept;kept.greensApplied=out->greensApplied;kept.prune=out->prune;bool visible=true;uint32_t run=0;
            for (const auto &p:out->points) {if ((p.modifierVisibility>=.5f)!=visible) {kept.runs.push_back(run);run=0;visible=!visible;}++run;}
            kept.runs.push_back(run);kept.runs.shrink_to_fit();
            auto &slot=visibility_[frameIndex];visibilityBytes_-=slot.runs.empty() ? 0 : slot.bytes();if (slot.runs.empty()) visibilityOrder_.push_back(frameIndex);
            slot=std::move(kept);visibilityBytes_+=slot.bytes();
            while (visibilityBytes_>VisibilityBudget && !visibilityOrder_.empty()) {
                const auto oldest=visibility_.find(visibilityOrder_.front());visibilityOrder_.pop_front();
                if (oldest!=visibility_.end()) {visibilityBytes_-=oldest->second.bytes();visibility_.erase(oldest);}
            }
        }
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
        for (auto &m:stack) m.showErased=false; // what Erase removes, even while its picks are edited
        return stack;
    };
    auto matrixJson=[](const QMatrix4x4 &m) {QJsonArray a;for (int i=0;i<16;++i) a.append(double(m.constData()[i]));return a;};
    auto stackJson=[](const QVector<Modifier> &stack) {Project p;p.modifiers=stack;return p.modifierJson();};
    Project own;own.modifiers={*project.loopMorph()};
    // The frames an overlapping morph shows, and those before Start it leads into: how each is
    // placed and what shapes it, since the morph's frames are kept once worked out.
    QJsonArray followedFrames;
    if (overlaps) for (int k=1;k<=frames;++k) for (const int frame:{endFrame-k,leadIn ? startFrame-k : -1}) if (frame>=0)
        followedFrames.append(QJsonObject{{"frame",frame},{"model",matrixJson(project.transformAtFrame(frame).matrix())},{"stack",stackJson(above(frame))}});
    const QByteArray key=QJsonDocument(QJsonObject{{"end",stackJson(above(endFrame))},{"start",stackJson(above(startFrame))},{"morph",own.modifierJson()},
        {"endFrame",endFrame},{"startFrame",startFrame},{"endModel",matrixJson(endModel)},{"startModel",matrixJson(startModel)},{"sh",sh},
        {"frames",frames},{"leadIn",leadIn},{"followed",followedFrames}}).toJson(QJsonDocument::Compact);
    auto cloud=[&](int frame,const QMatrix4x4 &into) {
        const auto stack=above(frame);
        const auto f=processed(frame/fps,sh,project,stack);const CompiledModifiers compiled(stack);const QMatrix4x4 model=project.transformAtFrame(frame).matrix();
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
        morph_.reset();morphKey_.clear();emit morphFitting(true);
        struct Done {CaptureWorker *worker;~Done() {emit worker->morphFitting(false);}} done{this};
        // Both frames in the End frame's capture coordinates: its transform draws the morph.
        const QMatrix4x4 toEnd=intoEnd(startFrame);
        auto endCloud=cloud(endFrame,QMatrix4x4()),startCloud=cloud(startFrame,toEnd);
        std::vector<loopmorph::Pair> pairs;for (const auto &pair:project.morphPairs(fps)) pairs.push_back({pair.end,toEnd.map(pair.start)});
        auto morph=std::make_unique<loopmorph::Morph>(std::move(endCloud),std::move(startCloud),pairs);
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
            morph->follow(start,track);
        };
        if (overlaps) {followBack(false,endFrame);if (leadIn) followBack(true,startFrame);}
        morph_=std::move(morph);morphKey_=key;
        morph_->setCaching(true); // played again, its frames are not deformed again
    }
    loopmorph::Cloud liveEnd,liveStart;loopmorph::Morph::Live live;
    if (overlaps) {
        live.back=back;liveEnd=cloud(sourceFrame,intoEnd(sourceFrame));live.end=&liveEnd;
        if (leadIn) {liveStart=cloud(startFrame-int(back),intoEnd(startFrame-int(back)));live.start=&liveStart;}
    }
    const auto c=morph_->evaluate(fraction,live);
    auto out=std::make_shared<RenderFrame>();
    out->seconds=time;out->total=c.size();out->coefficients=c.shCoefficients;out->chunkIndex=RenderFrame::MorphChunk;out->sh=c.sh;
    for (const auto &pair:project.morphPairs(fps)) out->morphMarkers.push_back({pair.number,morph_->carry(pair.end,fraction,overlaps ? back : 0,leadIn)});
    out->records.resize(c.size());out->active.assign(c.size(),1);out->points.resize(c.size());
    for (size_t i=0;i<c.size();++i) {
        auto &r=out->records[i];std::copy_n(c.position.data()+3*i,3,r.position);std::copy_n(c.rotation.data()+4*i,4,r.rotation);std::copy_n(c.scale.data()+3*i,3,r.scale);
        std::copy_n(c.colour.data()+3*i,3,r.color);r.color[3]=c.opacity[i];r.id=float(i);
        auto &p=out->points[i];std::copy_n(r.position,3,p.position);std::copy_n(r.color,3,p.color);p.id=r.id;
    }
    // Below the morph, Purge Isolated applies to its frames as to any other. Pruning and Erase
    // pick splats of source chunks, and these splats are in none.
    const CompiledModifiers below(project.modifiersBelowMorph(endFrame));
    if (!below.isolations.isEmpty()) {
        std::vector<QVector3D> positions(out->points.size());std::vector<uint8_t> keep(out->points.size(),1);
        for (size_t i=0;i<positions.size();++i) {const auto &p=out->points[i];positions[i]=endModel.map({p.position[0],p.position[1],p.position[2]});keep[i]=below.keepsPosition(positions[i]);}
        applyIsolation(positions,keep,below.isolations);for (size_t i=0;i<keep.size();++i) out->points[i].modifierVisibility=keep[i] ? 1.f : 0.f;
    }
    out->decodeMs=timer.nsecsElapsed()/1e6;
    return out;
}
