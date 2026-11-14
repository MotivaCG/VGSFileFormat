// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "gs2meshrunner.h"
#include "gs2mesh.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QLibrary>
#include <QMutex>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

QJsonObject MeshSettings::json() const {
    QJsonArray levels;for (int l:lods) levels.append(l);
    return {{"triangles",triangles},{"lods",levels},{"textureSize",textureSize},{"normalMap",normalMap},
            {"voxelMm",double(voxelMm)},{"virtualResolution",virtualResolution},{"denoise",denoise}};
}
MeshSettings MeshSettings::fromJson(const QJsonObject &o) {
    MeshSettings s;
    s.triangles=std::clamp(o.value("triangles").toInt(s.triangles),1000,5000000);
    if (o.contains("lods")) {s.lods.clear();for (const auto &v:o.value("lods").toArray()) if (v.toInt()>0 && s.lods.size()<4) s.lods.append(std::clamp(v.toInt(),100,5000000));}
    s.textureSize=std::clamp(o.value("textureSize").toInt(s.textureSize),256,16384);
    s.normalMap=o.value("normalMap").toBool(s.normalMap);
    s.voxelMm=float(std::clamp(o.value("voxelMm").toDouble(s.voxelMm),0.5,50.0));
    s.virtualResolution=std::clamp(o.value("virtualResolution").toInt(s.virtualResolution),256,4096);
    s.denoise=o.value("denoise").toBool(s.denoise);
    return s;
}
QString MeshSettings::summary() const {
    auto k=[](int n) {return n>=1000 ? QStringLiteral("%1k").arg(n/1000.0,0,'g',4) : QString::number(n);};
    QStringList levels;for (int l:lods) levels << k(l);
    return QStringLiteral("%1 triangles, %2, %3 texture").arg(k(triangles),levels.isEmpty() ? QStringLiteral("no LODs") : QStringLiteral("LODs %1").arg(levels.join(", ")))
        .arg(textureSize);
}

// The DLL's entry points, resolved by name: nothing links against gs2mesh.
struct Gs2Mesh::Library {
    QLibrary dll;
    decltype(&gs2m_version) version=nullptr;
    decltype(&gs2m_default_config) defaults=nullptr;
    decltype(&gs2m_create) create=nullptr;
    decltype(&gs2m_destroy) destroy=nullptr;
    decltype(&gs2m_run) run=nullptr;
    decltype(&gs2m_cancel) cancel=nullptr;
    decltype(&gs2m_last_error) lastError=nullptr;
    decltype(&gs2m_output_file) outputFile=nullptr;
};

QStringList Gs2Mesh::searchPaths() {
    QStringList paths;
    const QString app=QCoreApplication::applicationDirPath();
    if (qEnvironmentVariableIsSet("GS2MESH_DIR")) paths << qEnvironmentVariable("GS2MESH_DIR");
    paths << app+"/gs2mesh" << app;
#ifdef SGS_GS2MESH_DIR
    paths << QStringLiteral(SGS_GS2MESH_DIR); // the submodule's install, for builds run where they were made
#endif
    return paths;
}

QString Gs2Mesh::defaultModel(Model model) {
    // FoundationStereo: the ViT-S at 640 x 1280 gs2mesh was tuned with, else any; MoGe-2 ViT-L.
    const QStringList names=model==Model::Stereo ? QStringList{"foundation_stereo_vits_640x1280.onnx","foundation_stereo*.onnx"}
                                                 : QStringList{"moge-2-vitl-dynamic.onnx","moge*.onnx"};
    for (const auto &dir:searchPaths()) {
        if (!QFileInfo::exists(QDir(dir).filePath("gs2mesh.dll"))) continue;
        const QDir models(QDir(dir).filePath("models"));
        for (const auto &name:names) {const auto found=models.entryList({name},QDir::Files,QDir::Name);if (!found.isEmpty()) return models.filePath(found.first());}
    }
#ifdef SGS_STEREO_MODEL
    if (model==Model::Stereo && QFileInfo::exists(QStringLiteral(SGS_STEREO_MODEL))) return QStringLiteral(SGS_STEREO_MODEL);
#endif
#ifdef SGS_MOGE_MODEL
    if (model==Model::MoGe && QFileInfo::exists(QStringLiteral(SGS_MOGE_MODEL))) return QStringLiteral(SGS_MOGE_MODEL);
#endif
    return {};
}

std::shared_ptr<Gs2Mesh::Library> Gs2Mesh::library(QString *why) {
    static QMutex mutex;QMutexLocker lock(&mutex);
    static std::weak_ptr<Library> loaded;
    if (auto existing=loaded.lock()) return existing;
    QString reasons;
    for (const auto &dir:searchPaths()) {
        const QString path=QDir(dir).absoluteFilePath("gs2mesh.dll");
        if (!QFileInfo::exists(path)) continue;
        auto lib=std::make_shared<Library>();lib->dll.setFileName(path);
#ifdef Q_OS_WIN
        // Its libraries (OpenCV, ONNX Runtime, CUDA) are beside it, and ONNX Runtime loads more of
        // them later: that folder is searched for as long as the editor runs.
        SetDllDirectoryW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(QDir(dir).absolutePath()).utf16()));
#endif
        if (!lib->dll.load()) {reasons=lib->dll.errorString();continue;}
        auto resolve=[&](auto &fn,const char *name) {fn=reinterpret_cast<std::remove_reference_t<decltype(fn)>>(lib->dll.resolve(name));return fn!=nullptr;};
        if (!(resolve(lib->version,"gs2m_version") && resolve(lib->defaults,"gs2m_default_config") && resolve(lib->create,"gs2m_create") && resolve(lib->destroy,"gs2m_destroy")
              && resolve(lib->run,"gs2m_run") && resolve(lib->cancel,"gs2m_cancel") && resolve(lib->lastError,"gs2m_last_error") && resolve(lib->outputFile,"gs2m_output_file"))) {
            reasons=QStringLiteral("%1 is not a gs2mesh this editor knows.").arg(QDir::toNativeSeparators(path));continue;
        }
        // The header this editor was built with must be the DLL's.
        gs2m_config config{};lib->defaults(&config);
        if (config.struct_size!=sizeof(gs2m_config)) {reasons=QStringLiteral("%1 is another version of gs2mesh than this editor's.").arg(QDir::toNativeSeparators(path));continue;}
        loaded=lib;return lib;
    }
    if (why) *why=reasons.isEmpty() ? QStringLiteral("gs2mesh.dll was not found beside the editor (gs2mesh folder).") : reasons;
    return {};
}

bool Gs2Mesh::available(QString *why,QString *version) {
    const auto lib=library(why);
    if (lib && version) *version=QString::fromUtf8(lib->version());
    return bool(lib);
}

Gs2Mesh::Gs2Mesh(QObject *parent) : QObject(parent) {}
Gs2Mesh::~Gs2Mesh() {
    cancel();
    // The job's thread holds this object: it must end first.
    while (running_) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void Gs2Mesh::cancel() {
    if (auto *ctx=static_cast<gs2m_context *>(context_.load()); ctx && lib_) lib_->cancel(ctx);
}

bool Gs2Mesh::start(const Job &job) {
    if (running_) return false;
    QString why;lib_=library(&why);
    if (!lib_) {emit finished(false,why,{});return false;}
    running_=true;
    std::thread([this,job,lib=lib_] {
        // Strings the configuration points to live here until the run is over.
        const QByteArray ply=job.ply.toUtf8(),out=job.outputDir.toUtf8(),name=job.name.toUtf8(),sparse=job.sparse.toUtf8(),images=job.images.toUtf8();
        const QByteArray stereo=job.stereoModel.toUtf8(),moge=job.mogeModel.toUtf8(),cache=job.engineCache.toUtf8();
        const bool photos=!job.sparse.isEmpty() && !job.images.isEmpty();
        gs2m_config c{};lib->defaults(&c);
        c.ply_path=ply.constData();c.output_dir=out.constData();c.output_name=name.constData();
        c.sparse_dir=photos ? sparse.constData() : nullptr;c.images_dir=photos ? images.constData() : nullptr;
        c.stereo_model=photos && !stereo.isEmpty() ? stereo.constData() : nullptr;
        c.moge_model=photos && !moge.isEmpty() ? moge.constData() : nullptr;
        c.engine_cache_dir=cache.isEmpty() ? nullptr : cache.constData();
        const auto &s=job.settings;
        c.target_triangles=s.triangles;c.target_vertices=0;
        for (int i=0;i<4;++i) c.lod_triangles[i]=i<s.lods.size() ? s.lods[i] : 0;
        c.texture_size=s.textureSize;c.bake_normal_map=s.normalMap ? 1 : 0;
        c.tsdf_voxel=s.voxelMm/1000.f;c.virtual_resolution=s.virtualResolution;c.denoise=s.denoise ? 1 : 0;
        struct Feed {Gs2Mesh *self;QElapsedTimer clock;QString stage;int percent=-1;} feed{this,{},{},-1};feed.clock.start();
        auto onProgress=[](const char *stage,float stageProgress,float total,void *user) {
            auto *f=static_cast<Feed *>(user);const QString s=QString::fromUtf8(stage);const int percent=int(total*1000);
            // Every stage change, and otherwise no more than ten times a second.
            if (s==f->stage && (percent==f->percent || f->clock.elapsed()<100)) return;
            f->stage=s;f->percent=percent;f->clock.restart();
            emit f->self->progress(s,double(stageProgress),double(total));
        };
        auto onLog=[](gs2m_log_level level,const char *message,void *user) {
            emit static_cast<Feed *>(user)->self->logged(int(level),QString::fromUtf8(message));
        };
        gs2m_context *ctx=lib->create(&c,onProgress,onLog,&feed);
        // running_ falls only after the last signal: the destructor waits for it.
        if (!ctx) {emit finished(false,QStringLiteral("gs2mesh refused the job (out of memory, or another version)."),{});running_=false;return;}
        context_=ctx;
        const gs2m_status status=lib->run(ctx);
        context_=nullptr;
        QStringList files;QString error;
        if (status==GS2M_OK) for (int i=0;;++i) {const char *file=lib->outputFile(ctx,i);if (!file) break;files << QString::fromUtf8(file);}
        else error=status==GS2M_CANCELLED ? QStringLiteral("Cancelled.") : QString::fromUtf8(lib->lastError(ctx));
        lib->destroy(ctx);
        emit finished(status==GS2M_OK,error,files);
        running_=false;
    }).detach();
    return true;
}
