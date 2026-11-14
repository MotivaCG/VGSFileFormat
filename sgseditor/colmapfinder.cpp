// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "colmapfinder.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QQuaternion>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <optional>

namespace {
bool hasModel(const QString &dir) {
    return QFileInfo::exists(dir+"/cameras.bin") && QFileInfo::exists(dir+"/images.bin");
}
// The parameters each COLMAP camera model has.
int parameterCount(int model) {
    switch (model) {
    case 0: return 3;  // SIMPLE_PINHOLE
    case 1: return 4;  // PINHOLE
    case 2: return 4;  // SIMPLE_RADIAL
    case 3: return 5;  // RADIAL
    case 4: return 8;  // OPENCV
    case 5: return 8;  // OPENCV_FISHEYE
    case 6: return 12; // FULL_OPENCV
    case 7: return 5;  // FOV
    case 8: return 4;  // SIMPLE_RADIAL_FISHEYE
    case 9: return 5;  // RADIAL_FISHEYE
    case 10: return 12; // THIN_PRISM_FISHEYE
    default: return -1;
    }
}
struct Reader {
    QByteArray bytes;qsizetype at=0;bool ok=true;
    template <class T> T take() {
        if (at+qsizetype(sizeof(T))>bytes.size()) {ok=false;return T{};}
        T v;std::memcpy(&v,bytes.constData()+at,sizeof(T));at+=qsizetype(sizeof(T));return qFromLittleEndian(v);
    }
    QString name() {
        const qsizetype end=bytes.indexOf('\0',at);if (end<0) {ok=false;return {};}
        const QString s=QString::fromUtf8(bytes.constData()+at,end-at);at=end+1;return s;
    }
    void skip(qsizetype n) {if (at+n>bytes.size()) ok=false;else at+=n;}
};
} // namespace

namespace {
// The model in `sparse`, with its images, read and checked.
ColmapModel readModel(const QString &folder,const QString &sparse) {
    ColmapModel m;m.folder=folder;m.sparse=sparse;
    // The images: beside the model, or beside the folder holding it.
    QDir up(m.sparse);
    for (int level=0;level<3 && m.images.isEmpty();++level) {
        if (QFileInfo(up.filePath("images")).isDir()) m.images=QDir::cleanPath(up.filePath("images"));
        if (!up.cdUp()) break;
    }
    // Cameras: their models.
    QFile cameras(m.sparse+"/cameras.bin");
    if (!cameras.open(QIODevice::ReadOnly)) {m.problem=QStringLiteral("Cannot read cameras.bin.");return m;}
    Reader c{cameras.readAll()};
    const quint64 cameraCount=c.take<quint64>();
    for (quint64 i=0;i<cameraCount && c.ok;++i) {
        c.take<quint32>();const int model=c.take<qint32>();c.take<quint64>();c.take<quint64>();
        const int parameters=parameterCount(model);if (parameters<0) {c.ok=false;break;}
        c.skip(qsizetype(parameters)*8);
        if (model!=0 && model!=1) m.undistorted=false;
    }
    if (!c.ok) {m.problem=QStringLiteral("cameras.bin is damaged or of an unknown camera model.");return m;}
    m.cameras=int(cameraCount);
    // Photos: their names and where their cameras stand.
    QFile images(m.sparse+"/images.bin");
    if (!images.open(QIODevice::ReadOnly)) {m.problem=QStringLiteral("Cannot read images.bin.");return m;}
    Reader r{images.readAll()};
    const quint64 imageCount=r.take<quint64>();QStringList names;
    for (quint64 i=0;i<imageCount && r.ok;++i) {
        r.take<quint32>();
        double q[4],t[3];for (double &v:q) v=r.take<double>();for (double &v:t) v=r.take<double>();
        r.take<quint32>();names << r.name();
        const quint64 points=r.take<quint64>();r.skip(qsizetype(points)*24);
        // World to camera x' = R x + t: the centre is -R^T t.
        const QQuaternion rotation{float(q[0]),float(q[1]),float(q[2]),float(q[3])};
        m.centres.push_back(-rotation.normalized().conjugated().rotatedVector(QVector3D(float(t[0]),float(t[1]),float(t[2]))));
    }
    if (!r.ok) {m.problem=QStringLiteral("images.bin is damaged.");return m;}
    m.photos=int(imageCount);
    if (m.images.isEmpty()) {m.problem=QStringLiteral("The model has no images folder beside it.");return m;}
    for (const auto &name:names) if (!QFileInfo::exists(m.images+"/"+name)) ++m.missing;
    if (!m.photos) m.problem=QStringLiteral("The model has no photos.");
    else if (m.missing) m.problem=QStringLiteral("%1 of the model's %2 photos are not in %3.").arg(m.missing).arg(m.photos).arg(QDir::toNativeSeparators(m.images));
    else if (!m.undistorted) m.problem=QStringLiteral("The model is not undistorted (its cameras are not PINHOLE): undistort it with COLMAP first.");
    return m;
}
// Where a model may be in `folder`, most likely first: the folder itself, an undistorted copy
// (COLMAP's image_undistorter writes undistorted/sparse beside the original sparse, which keeps
// its distortion), then sparse/ itself.
QStringList likelyModels(const QString &folder) {
    QStringList dirs;
    for (const QString &d:{QString(),QStringLiteral("/undistorted/sparse"),QStringLiteral("/undistorted/sparse/0"),QStringLiteral("/undistorted"),
                           QStringLiteral("/sparse"),QStringLiteral("/sparse/0")})
        dirs << QDir::cleanPath(folder+d);
    return dirs;
}
} // namespace

ColmapModel findColmap(const QString &folder) {
    ColmapModel none;none.folder=QDir::cleanPath(folder);
    if (folder.isEmpty() || !QFileInfo(folder).isDir()) {none.problem=QStringLiteral("The folder does not exist.");return none;}
    // The likely places, then every folder below; the first model gs2mesh can use, else the
    // first one found, with what is wrong with it.
    QStringList candidates=likelyModels(none.folder);
    QDirIterator it(none.folder,QDir::Dirs|QDir::NoDotAndDotDot,QDirIterator::Subdirectories);
    for (int seen=0;it.hasNext() && seen<2000;++seen) candidates << QDir::cleanPath(it.next());
    QStringList tried;std::optional<ColmapModel> first;
    for (const auto &dir:candidates) {
        if (tried.contains(dir,Qt::CaseInsensitive) || !hasModel(dir)) continue;
        tried << dir;
        ColmapModel m=readModel(none.folder,dir);
        if (m.usable()) return m;
        if (!first) first=m;
    }
    if (first) return *first;
    none.problem=QStringLiteral("No COLMAP model (cameras.bin and images.bin) in this folder.");
    return none;
}

QString colmapMatches(const ColmapModel &m,const QVector3D &minimum,const QVector3D &maximum) {
    if (m.centres.empty()) return QStringLiteral("The model has no cameras.");
    const QVector3D centre=(minimum+maximum)*0.5f,half=(maximum-minimum)*0.5f;
    const float radius=std::max(half.length(),1e-4f);
    // Cameras around the subject: most outside its box, none absurdly far, and on several sides.
    int outside=0,far=0;QVector3D mean;
    for (const auto &c:m.centres) {
        const QVector3D d=c-centre;
        if (std::abs(d.x())>half.x() || std::abs(d.y())>half.y() || std::abs(d.z())>half.z()) ++outside;
        if (d.length()>60*radius) ++far;
        mean+=d.normalized();
    }
    const int n=int(m.centres.size());
    if (far*2>n) return QStringLiteral("The photos' cameras are far from the splats: the model is of another capture, or at another scale.");
    if (outside*4<n*3) return QStringLiteral("Many of the photos' cameras stand inside the splats: the model is of another capture, or in another frame.");
    if (n>=6 && (mean/float(n)).length()>0.9f) return QStringLiteral("The photos' cameras all stand on one side, away from the splats: the model is probably in another frame.");
    return {};
}

QString suggestColmap(const QString &plyPath) {
    const QFileInfo ply(plyPath);QDir dir=ply.dir();
    QStringList candidates{dir.path()};
    for (const auto &entry:dir.entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        const QString name=entry.fileName().toLower();
        if (name.contains("colmap") || name.contains("undistort") || name.contains("sparse") || name.startsWith(ply.completeBaseName().toLower())) candidates << entry.absoluteFilePath();
    }
    for (const auto &candidate:candidates) {
        // Only near ones: the PLY's own folder and those beside it, not a search of the disk.
        bool any=false;for (const auto &d:likelyModels(candidate)) any=any || hasModel(d);
        if (!any) continue;
        const auto m=findColmap(candidate);
        if (m.usable()) return m.folder;
    }
    return {};
}
