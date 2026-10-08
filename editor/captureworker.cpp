#include "captureworker.h"
#include "isolation.h"
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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
    vgs_.reset(); source_.reset(); mint_.reset(); info_ = {}; generation_ = 0;
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
            bounds = candidate.mint_->boundingBox();
        } else if (ext == "vgs" || ext == "pgs") {
            candidate.source_ = std::make_unique<FileSource>(path);
            candidate.vgs_ = std::make_unique<vgsdec::Capture>(vgsdec::Capture::openStream(*candidate.source_));
            candidate.vgs_->setCachePolicy({1, 1, 512ull * 1024 * 1024});
            candidate.info_.duration = candidate.vgs_->duration(); candidate.info_.fps = candidate.vgs_->frameRate();
            if (!candidate.vgs_->metadata().title.empty()) candidate.info_.title = QString::fromStdString(candidate.vgs_->metadata().title);
            bounds = candidate.vgs_->bounds();
        } else throw std::runtime_error("Unsupported format. Open a .vgs, .pgs or .mint file.");
        if (!std::isfinite(candidate.info_.duration) || candidate.info_.duration <= 0 ||
            !std::isfinite(candidate.info_.fps) || candidate.info_.fps <= 0 ||
            candidate.info_.duration * candidate.info_.fps > std::numeric_limits<int>::max() - 1)
            throw std::runtime_error("The capture has an invalid timeline.");
        candidate.info_.frames = std::max(1, int(std::ceil(candidate.info_.duration * candidate.info_.fps - 1e-6)));
        for (int j = 0; j < 3; ++j) {
            if (!std::isfinite(bounds[j]) || !std::isfinite(bounds[j+3]) || bounds[j] > bounds[j+3])
                throw std::runtime_error("The capture has invalid spatial bounds.");
            candidate.info_.minimum[j] = float(bounds[j]); candidate.info_.maximum[j] = float(bounds[j+3]);
        }
        auto first = candidate.frame(0, sh);
        vgs_.reset(); source_.reset(); mint_.reset();
        source_ = std::move(candidate.source_); vgs_ = std::move(candidate.vgs_); mint_ = std::move(candidate.mint_);
        info_ = candidate.info_; generation_ = generation;
        emit opened(info_, first, generation);
    } catch (const std::exception &e) { emit failed(QString::fromUtf8(e.what()), generation, true); }
}
void CaptureWorker::decode(double time, quint64 generation, bool sh,Project project) {
    if (generation != generation_) { emit failed("Capture replaced.", generation, false); return; }
    try {
        QElapsedTimer processingTimer;processingTimer.start();auto out=frame(time,sh);CompiledModifiers modifiers(project.modifiersAtFrame(std::round(time*info_.fps)));
        if (!modifiers.isolations.isEmpty()) {
            const auto model=project.transformAtFrame(std::round(out->seconds*info_.fps)).matrix();std::vector<QVector3D> positions(out->points.size());std::vector<uint8_t> keep(out->points.size());
            for (size_t i=0;i<positions.size();++i) {const auto &p=out->points[i];positions[i]=model.map({p.position[0],p.position[1],p.position[2]});keep[i]=modifiers.keeps(positions[i],{p.color[0],p.color[1],p.color[2]});}
            applyIsolation(positions,keep,modifiers.isolations);for (size_t i=0;i<keep.size();++i) out->points[i].modifierVisibility=keep[i] ? 1.f : 0.f;
        }
        out->decodeMs=processingTimer.nsecsElapsed()/1e6;emit decoded(out,generation);
    }
    catch (const std::exception &e) { emit failed(QString::fromUtf8(e.what()), generation, false); }
}
