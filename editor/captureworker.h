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
#include "mintfile.h"
#include "project.h"
#ifndef VGS_VIEWER // the viewer reads no PLY sequences
#include "plysequence.h"
#endif
#include "pruning.h"
#include "loopmorph.h"
#include "previewmemory.h"
#include "vgsdecoder/vgsdecoder.h"
#include <QObject>
#include <QTimer>
#include <QVector3D>
#include <deque>
#include <array>
#include <map>
#include <memory>
#include <list>
#include <tuple>
#include <atomic>
#include <vector>

struct Splat {
    float position[3], rotation[4], scale[3], color[4], id;
};
struct PointVertex {
    float position[3], color[3], id;
    float modifierVisibility=1;
};
struct PreviewCacheState {
    int frame=0;
    quint64 revision=0;
    bool morph=false,visibility=false,colour=false,shape=false;
    bool sourceVisibility=false,sourceColour=false,memoryLimited=false,enabled=true;
    int morphTotalFrames=0;
    int timelineFrames=0;
    size_t visibilityFrames=0,colourFrames=0,morphFrames=0;
    size_t visibilityBytes=0,colourBytes=0,morphBytes=0;
    std::array<size_t,3> visibilityStageFrames{},visibilityStageBytes{};
    size_t reusableBytes=0;
};
Q_DECLARE_METATYPE(PreviewCacheState)
struct RenderFrame {
    // Full decoded records, including inactive/transparent entries. Preview buffers
    // below are a separate view; they never mutate or discard the source attributes.
    std::vector<Splat> records;
    std::vector<uint8_t> active;
    std::vector<PointVertex> points;
    std::vector<float> sh; // fully evaluated coefficients, except when shBlend supplies them
    loopmorph::ShBlend shBlend;
    int coefficients = 0;
    double seconds = 0;
    quint64 total = 0;
    size_t chunkIndex = 0;
    // The chunkIndex of a Loop morph's frames: their splats belong to no source chunk.
    static constexpr size_t MorphChunk = size_t(-1);
    // On a Loop morph's frame, where each marker pair has been carried to, in End's capture
    // coordinates: the marker's number and its point.
    std::vector<std::pair<int, QVector3D>> morphMarkers;
    double decodeMs = 0;
    double sourceMs = 0, assemblyMs = 0;
    bool visibilityFromCache = false;
    bool morphShapeFromCache = false;
    bool colourFromCache = false;
    PreviewCacheState cache;
    // What each active Prune low contribution did to this frame's chunk, in stack order.
    std::vector<PruneStats> prune;
    // Remove green already decided into modifierVisibility, because the colours shown are
    // despilled and a filter above Color must test the source's: the viewport skips it.
    bool greensApplied = false;
};
using FramePtr = std::shared_ptr<RenderFrame>;
Q_DECLARE_METATYPE(FramePtr)
struct CaptureInfo {
    QString path, title, format;
    PlySequenceSpec plySequence;
    double duration = 0, fps = 30;
    int frames = 0;
    int shDegree = -1;
    // Trained with anti-aliasing: Gracia .mint captures always, a .vgs when its header says
    // so. Splats are then drawn with the matching opacity compensation.
    bool antialiased = false;
    // The capture's own sound track, as delivered, and its suffix ("mp3", "m4a",
    // "opus", "wav"); and where the capture starts on its source's timeline, which is where
    // that track starts playing.
    QByteArray audio;
    QString audioSuffix;
    double startSeconds = 0;
    // Internal chunk changes, in seconds on the capture timeline; editor display only.
    QVector<double> chunkBoundaries;
    QVector3D minimum, maximum;
};
Q_DECLARE_METATYPE(CaptureInfo)

struct ExportVisibilityMask {
    std::vector<uint32_t> runs;
    size_t records=0,points=0;
};
struct PreviewExportStore;

class CaptureWorker : public QObject {
    Q_OBJECT
public:
    explicit CaptureWorker(QObject *parent = nullptr,previewmemory::Probe memoryProbe = {});
    ~CaptureWorker() override;
public slots:
    void clear();
    // Also checked while paused, on the worker thread.
    void relieveMemoryPressure();
    void open(const QString &path, quint64 generation, bool sh);
    void decode(double time, quint64 generation, bool sh,Project project = {});
    void openRequest(const QString &path, quint64 generation, bool sh, quint64 request);
#ifndef VGS_VIEWER
    void openSourceRequest(const QString &path,quint64 generation,bool sh,quint64 request,Project project);
#endif
    void decodeRequest(double time, quint64 generation, bool sh, Project project, quint64 request);
    void precalculate(Project project,quint64 generation,quint64 request);
    void setCachingEnabled(bool enabled);
    void inspectCache(quint64 generation,quint64 revision);
public:
    // Cancellation is safe from the GUI thread, including before the job starts.
    void cancelPrecalculate(quint64 request) {cancelCacheRequest_.store(request);}
    // The visibility cache: frames held, and the memory they take.
    size_t visibilityFrames() const { return visibility_.size(); }
    size_t visibilityBytes() const { return visibilityBytes_; }
    size_t isolationEvaluations() const { return isolationEvaluations_; }
    size_t reusableFrameBytes() const;
    size_t morphCacheBytes() const {return morph_ ? morph_->cachedBytes() : 0;}
    size_t colourCacheBytes() const {return colourBytes_;}
    size_t colourCacheFrames() const {return colours_.size();}
    size_t despillEvaluations() const {return despillEvaluations_;}
    void setColourCacheBudget(size_t bytes);
    // Weak references to existing masks: no cache snapshot or full-frame copy is retained.
    static std::function<bool(int,const std::vector<uint8_t> &,std::vector<uint8_t> &)> exportVisibilityLookup(const Project &);
    size_t exportVisibilityHits() const;
    // Combined retention limit: visibility + reusable source buffers + morph shapes.
    static constexpr size_t PreviewBudget = 4ull * 1024 * 1024 * 1024;
    static constexpr size_t FrameBudget = 256ull * 1024 * 1024;
    static constexpr size_t VisibilityBudget = 256ull * 1024 * 1024;
    static constexpr size_t ColourBudget = 6ull * 1024 * 1024 * 1024;
public slots:
signals:
    void opened(CaptureInfo info, FramePtr frame, quint64 generation);
    void sourceProgress(int done,int total,QString path,quint64 generation,quint64 request);
    void sourceReindexed(CaptureInfo info,quint64 generation);
    void decoded(FramePtr frame, quint64 generation);
    void failed(QString message, quint64 generation, bool opening);
    void openedRequest(CaptureInfo info, FramePtr frame, quint64 generation, quint64 request);
    void decodedRequest(FramePtr frame, quint64 generation, quint64 request);
    void failedRequest(QString message, quint64 generation, bool opening, quint64 request);
    // A Loop morph is being fitted (a second or so on a large capture), and when it is done.
    void morphFitting(bool fitting);
    void cacheProgress(int done,int total,quint64 generation,quint64 request);
    void cacheFinished(int done,int total,bool limited,QString error,quint64 generation,quint64 request);
    void cacheReduced(quint64 generation);
    void cacheComputing(int groups,quint64 generation,quint64 revision);
    void cacheStateReady(PreviewCacheState state,quint64 generation);
private:
    class FileSource;
    std::unique_ptr<FileSource> source_;
    std::unique_ptr<vgsdec::Capture> vgs_;
    std::unique_ptr<MintFile> mint_;
#ifndef VGS_VIEWER
    std::unique_ptr<PlySequence> ply_;
    PlySequenceSpec openingPly_;
    double openingPlyFps_=30;
    int openingPlyOffset_=0;
#endif
    CaptureInfo info_;
    quint64 generation_ = 0;
    quint64 request_ = 0;
    std::atomic<quint64> cancelCacheRequest_{0};
    bool cacheLimited_=false;
    quint64 activeCacheRequest_=0;
    bool cachingEnabled_=true;
    PreviewCacheState lastCacheState_;
    void describeRetention(PreviewCacheState &);
    quint64 processingRevision_=0,dependencyRevision_=0;
    std::array<QByteArray,3> dependencyVisibilityKeys_;
    QByteArray dependencyMorphKey_,dependencyColourKey_;
    void preparePreview(Project &project);
    previewmemory::Guard memory_;
    QTimer *memoryTimer_ = nullptr;
    // Prune low contribution's scores, per source chunk: they depend on the capture alone.
    std::map<size_t, std::vector<float>> pruneScores_;
    // Reuse only frames no consumer holds (viewport, sorter, queued signal or caller).
    // At most three retained buffers; extra frames held by inspections remain independent.
    std::array<FramePtr,3> framePool_;
    void trimFramePool();
    FramePtr frame(double time, bool sh);
    FramePtr reusableFrame();
    FramePtr evaluate(double time,bool sh,const Project &project);
    void describeCache(RenderFrame &,const Project &,bool sh);
    // `cached`: a frame of the timeline, whose visibility the cache may answer or keep.
    FramePtr processed(double time, bool sh, const Project &project, const QVector<Modifier> &stack, bool cached = false, int context = 0);
    // What Crop, Remove green, Purge Isolated, Erase and Prune leave visible on each frame, as
    // played: the points' visibility as alternate runs, visible first. Kept while the
    // project's modifiers, transform and processing stay as they were - any change empties it
    // - and filled only during playback, up to VisibilityBudget, the oldest going first.
    struct Visibility {std::shared_ptr<const ExportVisibilityMask> mask;bool greensApplied=false;std::vector<PruneStats> prune;size_t bytes() const;};
    std::shared_ptr<PreviewExportStore> exportStore_;
    void forgetExportVisibility(int frame=-1);
    std::array<QByteArray,3> visibilityKeys_;
    // Source timeline, source shaped above a morph, and final morph frames have
    // different masks even at the same time and with the same number of points.
    using VisibilityId = std::pair<int,int>;
    std::map<VisibilityId, Visibility> visibility_;
    std::deque<VisibilityId> visibilityOrder_;
    size_t visibilityBytes_ = 0;
    std::array<size_t,3> visibilityStageFrames_{},visibilityStageBytes_{};
    size_t isolationEvaluations_ = 0;
    // Full-precision corrected RGB, including skin recovery. SH is deliberately
    // not retained per frame. The SH degree is part of the identity because skin
    // classification inspects the source's view-dependent colour.
    using ColourId=std::tuple<double,size_t,int>;
    struct Colour {
        std::vector<float> rgb;
        std::list<ColourId>::iterator order;
        size_t bytes() const {return sizeof(Colour)+sizeof(ColourId)+128+rgb.capacity()*sizeof(float);}
    };
    QByteArray colourKey_;
    std::map<ColourId,Colour> colours_;
    std::list<ColourId> colourOrder_;
    size_t colourBytes_=0,despillEvaluations_=0,colourBudget_=ColourBudget;
    void clearColours();
    size_t evictColour();
    void applyDespill(RenderFrame &,const CompiledModifiers &,bool keep);
    void clearVisibility(int context=-1);
    bool cachedVisibility(RenderFrame &frame,const Project &project,int context);
    void keepVisibility(const RenderFrame &frame,const Project &project,int context);
    FramePtr morphFrame(double time, bool sh, const Project &project);
    // The Loop morph fitted last, and what it was fitted for.
    std::unique_ptr<loopmorph::Morph> morph_;
    loopmorph::Cloud morphScratch_;
    QByteArray morphKey_;
    const std::vector<float> &pruneScores(size_t chunk);
};
