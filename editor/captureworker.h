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
#include "pruning.h"
#include "loopmorph.h"
#include "vgsdecoder/vgsdecoder.h"
#include <QObject>
#include <QVector3D>
#include <deque>
#include <map>
#include <memory>
#include <vector>

struct Splat {
    float position[3], rotation[4], scale[3], color[4], id;
};
struct PointVertex {
    float position[3], color[3], id;
    float modifierVisibility=1;
};
struct RenderFrame {
    // Full decoded records, including inactive/transparent entries. Preview buffers
    // below are a separate view; they never mutate or discard the source attributes.
    std::vector<Splat> records;
    std::vector<uint8_t> active;
    std::vector<PointVertex> points;
    std::vector<float> sh;
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
    double duration = 0, fps = 30;
    int frames = 0;
    int shDegree = -1;
    // Trained with anti-aliasing: Gracia .mint captures always, a .vgs when its header says
    // so. Splats are then drawn with the matching opacity compensation.
    bool antialiased = false;
    // A .vgs's own sound track, as delivered, and the suffix its format goes by ("mp3", "m4a",
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

class CaptureWorker : public QObject {
    Q_OBJECT
public:
    explicit CaptureWorker(QObject *parent = nullptr);
    ~CaptureWorker() override;
public slots:
    void clear();
    void open(const QString &path, quint64 generation, bool sh);
    void decode(double time, quint64 generation, bool sh,Project project = {});
    void openRequest(const QString &path, quint64 generation, bool sh, quint64 request);
    void decodeRequest(double time, quint64 generation, bool sh, Project project, quint64 request);
public:
    // The visibility cache: frames held, and the memory they take.
    size_t visibilityFrames() const { return visibility_.size(); }
    size_t visibilityBytes() const { return visibilityBytes_; }
    static constexpr size_t VisibilityBudget = 256ull * 1024 * 1024;
public slots:
signals:
    void opened(CaptureInfo info, FramePtr frame, quint64 generation);
    void decoded(FramePtr frame, quint64 generation);
    void failed(QString message, quint64 generation, bool opening);
    void openedRequest(CaptureInfo info, FramePtr frame, quint64 generation, quint64 request);
    void decodedRequest(FramePtr frame, quint64 generation, quint64 request);
    void failedRequest(QString message, quint64 generation, bool opening, quint64 request);
    // A Loop morph is being fitted (a second or so on a large capture), and when it is done.
    void morphFitting(bool fitting);
private:
    class FileSource;
    std::unique_ptr<FileSource> source_;
    std::unique_ptr<vgsdec::Capture> vgs_;
    std::unique_ptr<MintFile> mint_;
    CaptureInfo info_;
    quint64 generation_ = 0;
    quint64 request_ = 0;
    // Prune low contribution's scores, per source chunk: they depend on the capture alone.
    std::map<size_t, std::vector<float>> pruneScores_;
    FramePtr frame(double time, bool sh);
    // `cached`: a frame of the timeline, whose visibility the cache may answer or keep.
    FramePtr processed(double time, bool sh, const Project &project, const QVector<Modifier> &stack, bool cached = false);
    // What Crop, Remove green, Purge Isolated, Erase and Prune leave visible on each frame, as
    // played: the points' visibility as alternate runs, visible first. Kept while the
    // project's modifiers, transform and processing stay as they were - any change empties it
    // - and filled only during playback, up to VisibilityBudget, the oldest going first.
    struct Visibility {std::vector<uint32_t> runs;bool greensApplied=false;std::vector<PruneStats> prune;size_t bytes() const;};
    QByteArray visibilityKey_;
    std::map<int, Visibility> visibility_;
    std::deque<int> visibilityOrder_;
    size_t visibilityBytes_ = 0;
    void clearVisibility();
    FramePtr morphFrame(double time, bool sh, const Project &project);
    // The Loop morph fitted last, and what it was fitted for.
    std::unique_ptr<loopmorph::Morph> morph_;
    QByteArray morphKey_;
    const std::vector<float> &pruneScores(size_t chunk);
};
