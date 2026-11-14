// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>

// What gs2mesh is asked to make (gs2mesh.h, gs2m_config): its defaults, as its own sample runs.
struct MeshSettings {
    int triangles = 200000;          // the main mesh
    QVector<int> lods{40000, 3000};  // lower LODs sharing its textures; the last is the "low" one
    int textureSize = 4096;
    bool normalMap = true;
    float voxelMm = 3;               // TSDF voxel
    int virtualResolution = 1024;    // the virtual views rendered from the splats
    bool denoise = true;
    QJsonObject json() const;
    static MeshSettings fromJson(const QJsonObject &);
    bool operator==(const MeshSettings &o) const { return json()==o.json(); }
    QString summary() const; // "200k triangles, LODs 40k and 3k, 4096 texture"
};

// gs2mesh, loaded when first used (gs2mesh.dll and its CUDA and ONNX libraries): the editor
// runs without it, and only mesh generation needs an NVIDIA GPU.
class Gs2Mesh : public QObject {
    Q_OBJECT
public:
    struct Job {
        QString ply, outputDir, name;
        QString sparse, images;          // COLMAP; both empty: from the splats alone
        QString stereoModel, mogeModel;  // optional ONNX models, used with photos only
        QString engineCache;
        MeshSettings settings;
    };
    explicit Gs2Mesh(QObject *parent = nullptr);
    ~Gs2Mesh() override;
    // Whether gs2mesh can be loaded, and if not why; its version when it can.
    static bool available(QString *why = nullptr, QString *version = nullptr);
    // Where it was found or would be looked for.
    static QStringList searchPaths();
    // The ONNX models gs2mesh uses with photos: a models folder beside gs2mesh.dll (the deployed
    // editor's), else where this build was told they are (SGS_STEREO_MODEL, SGS_MOGE_MODEL);
    // empty when there is none.
    enum class Model { Stereo, MoGe };
    static QString defaultModel(Model);
    // Runs it on a thread of its own. One job at a time.
    bool start(const Job &job);
    void cancel();
    bool running() const { return running_; }
signals:
    // stage: gs2mesh's ("render", "fusion", "bake"...); fractions 0..1.
    void progress(QString stage, double stageProgress, double totalProgress);
    void logged(int level, QString message);
    // The files written, main mesh (.obj) first; or why it failed.
    void finished(bool ok, QString error, QStringList files);
private:
    struct Library;
    static std::shared_ptr<Library> library(QString *why);
    std::atomic<bool> running_{false};
    std::atomic<void *> context_{nullptr};
    std::shared_ptr<Library> lib_;
};
