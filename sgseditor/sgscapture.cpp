// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "sgscapture.h"
#include <QFileInfo>
#include <algorithm>
#include <cmath>

QString normalsSourceText(NormalsSource source) {
    switch (source) {
    case NormalsSource::File: return QStringLiteral("from the PLY");
    case NormalsSource::Rig: return QStringLiteral("estimated with the rig's cameras");
    case NormalsSource::Surrounding: return QStringLiteral("estimated with cameras all around");
    default: return QStringLiteral("none");
    }
}

SgsCapture SgsCapture::load(const QString &path) {
    SgsPly ply=readSgsPly(path);
    SgsCapture c;c.path=QFileInfo(path).absoluteFilePath();c.frame=std::move(ply.frame);c.shDegree=ply.shDegree;c.comments=ply.comments;
    c.normals=c.frame.normal.empty() ? NormalsSource::None : NormalsSource::File;
    // The box, trimmed so a few floaters do not stretch it.
    std::vector<float> axis;axis.reserve(size_t(c.frame.count));
    for (int a=0;a<3;++a) {
        axis.clear();
        for (size_t i=0;i<size_t(c.frame.count);++i) if (c.frame.active[i]) axis.push_back(c.frame.position[i*3+size_t(a)]);
        if (axis.empty()) {c.minimum[a]=-1;c.maximum[a]=1;continue;}
        const size_t cut=axis.size()/100;
        std::nth_element(axis.begin(),axis.begin()+ptrdiff_t(cut),axis.end());c.minimum[a]=axis[cut];
        std::nth_element(axis.begin(),axis.end()-1-ptrdiff_t(cut),axis.end());c.maximum[a]=axis[axis.size()-1-cut];
    }
    c.visible=size_t(std::count(c.frame.active.begin(),c.frame.active.end(),uint8_t(1)));
    if (!c.visible) throw std::runtime_error(QStringLiteral("%1 has no visible splats.").arg(path).toStdString());
    return c;
}

std::optional<gaussiannormals::Rig> SgsCapture::rig() const {
    for (const auto &comment:comments) {
        if (!comment.startsWith(QStringLiteral("capture_rig "))) continue;
        const QString name=comment.mid(12).trimmed().toUpper();
        if (name==QLatin1String("SMNV1") || name==QLatin1String("SMN01") || name==QLatin1String("SCANMENOW_V1")) return gaussiannormals::Rig::SmnV1;
        if (name==QLatin1String("THE4DSCANNER")) return gaussiannormals::Rig::The4DScanner;
    }
    return std::nullopt;
}

void SgsCapture::estimateNormals(QString *note,int surrounding,bool fine) {
    applyNormals(computeNormals(surrounding,fine),note);
}

SgsCapture::NormalsEstimate SgsCapture::computeNormals(int surrounding,bool fine) const {
    gaussiannormals::Splats splats;splats.count=size_t(frame.count);splats.position=frame.position.data();splats.rotation=frame.rotation.data();
    splats.scale=frame.scale.data();splats.opacity=frame.opacity.data();splats.use=frame.active.data();
    // Cameras all around, whichever rig the PLY names: a rig's own cameras, at SMNForge's 1 %,
    // put the side wrong several times as often (editor/README.md, Normals).
    NormalsEstimate out;out.surrounding=surrounding;out.fine=fine;
    out.normals.assign(size_t(frame.count)*3,0.f);
    out.result=gaussiannormals::estimateNormals(splats,{},out.normals.data(),surrounding,
                                                fine ? gaussiannormals::kFineVisibility : gaussiannormals::kSurroundingVisibility);
    return out;
}

void SgsCapture::applyNormals(NormalsEstimate estimate,QString *note) {
    const int surrounding=estimate.surrounding;
    frame.normal=std::move(estimate.normals);
    normals=NormalsSource::Surrounding;
    if (note) *note=QStringLiteral("%1 cameras all around the splats decided which way each faces%2.").arg(surrounding).arg(estimate.fine ? QStringLiteral(", seeing finely") : QString());
}

FramePtr SgsCapture::renderFrame() const {
    auto out=std::make_shared<RenderFrame>();const size_t n=size_t(frame.count);
    out->total=n;out->coefficients=frame.shCoefficients;out->sh=frame.shRest;
    out->records.reserve(n);out->active.reserve(n);out->points.reserve(visible);
    for (size_t i=0;i<n;++i) {
        Splat v{};
        for (int j=0;j<3;++j) {v.position[j]=frame.position[i*3+j];v.scale[j]=frame.scale[i*3+j];v.color[j]=frame.colorDc[i*3+j];}
        for (int j=0;j<4;++j) v.rotation[j]=frame.rotation[i*4+j];
        v.color[3]=frame.opacity[i];v.id=float(i);
        out->records.push_back(v);out->active.push_back(frame.active[i]);
        if (frame.active[i]) {
            PointVertex point{};std::copy_n(v.position,3,point.position);std::copy_n(v.color,3,point.color);point.id=v.id;
            out->points.push_back(point);
        }
    }
    out->normals=frame.normal;
    return out;
}
