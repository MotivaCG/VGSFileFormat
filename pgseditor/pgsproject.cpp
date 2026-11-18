// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of PGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "pgsproject.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuaternion>
#include <QtMath>
#include <cmath>

namespace {
QJsonArray vec(const QVector3D &v) {return {v.x(),v.y(),v.z()};}
QVector3D vec(const QJsonValue &value,const QVector3D &fallback) {
    const auto a=value.toArray();if (a.size()!=3) return fallback;
    const QVector3D v(float(a[0].toDouble()),float(a[1].toDouble()),float(a[2].toDouble()));
    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z()) ? v : fallback;
}
// Relative to the project's folder when stored, absolute in memory.
QString stored(const QString &path,const QString &projectPath) {
    return path.isEmpty() ? QString() : QDir(QFileInfo(projectPath).absolutePath()).relativeFilePath(path);
}
QString restored(const QString &path,const QString &projectPath) {
    return path.isEmpty() ? QString() : QDir::cleanPath(QDir(QFileInfo(projectPath).absolutePath()).absoluteFilePath(path));
}
// The turn that sends -Y along `way`, as Euler XYZ degrees.
QVector3D rotationFor(const QVector3D &way) {
    const QQuaternion q=QQuaternion::rotationTo(QVector3D(0,-1,0),way.normalized());
    return Transform::fromMatrix([&] {QMatrix4x4 m;m.rotate(q);return m;}()).rotation;
}
} // namespace

QVector3D PgsLight::direction() const {return transform().rotationMatrix().mapVector(QVector3D(0,-1,0)).normalized();}
void PgsLight::setDirection(const QVector3D &way) {if (way.lengthSquared()>1e-12f) rotation=rotationFor(way);}
void PgsLight::angles(float *azimuth,float *elevation) const {
    const QVector3D from=-direction();
    *azimuth=float(qRadiansToDegrees(std::atan2(from.x(),from.z())));
    *elevation=float(qRadiansToDegrees(std::asin(std::clamp(from.y(),-1.f,1.f))));
}
void PgsLight::setAngles(float azimuth,float elevation) {
    const float a=qDegreesToRadians(azimuth),e=qDegreesToRadians(std::clamp(elevation,-89.9f,89.9f));
    setDirection(-QVector3D(std::cos(e)*std::sin(a),std::sin(e),std::cos(e)*std::cos(a)));
}
Transform PgsLight::transform() const {Transform t;t.position=position;t.rotation=rotation;return t;}
void PgsLight::setTransform(const Transform &t) {position=t.position;rotation=t.rotation;}
Viewport::ViewLight PgsLight::view(bool selected) const {
    Viewport::ViewLight l;l.directional=directional;l.enabled=enabled;l.selected=selected;l.position=position;l.direction=direction();
    l.colour=QVector3D(float(colour.redF()),float(colour.greenF()),float(colour.blueF()));l.intensity=intensity;l.range=range;
    return l;
}
QJsonObject PgsLight::json() const {
    return {{"name",name},{"type",directional ? "directional" : "point"},{"enabled",enabled},{"position",vec(position)},{"rotation",vec(rotation)},
            {"colour",colour.name(QColor::HexRgb)},{"intensity",double(intensity)},{"range",double(range)},
            {"turnSeconds",double(turnSeconds)},{"clockwise",clockwise}};
}
PgsLight PgsLight::fromJson(const QJsonObject &o) {
    PgsLight l;l.name=o.value("name").toString();l.directional=o.value("type").toString()=="directional";l.enabled=o.value("enabled").toBool(true);
    l.position=vec(o.value("position"),l.position);l.rotation=vec(o.value("rotation"),l.rotation);
    const QColor c(o.value("colour").toString());if (c.isValid()) l.colour=c;
    l.intensity=float(std::clamp(o.value("intensity").toDouble(1),0.0,1000.0));l.range=float(std::clamp(o.value("range").toDouble(0),0.0,10000.0));
    l.turnSeconds=float(std::clamp(o.value("turnSeconds").toDouble(0),0.0,86400.0));l.clockwise=o.value("clockwise").toBool();
    return l;
}
namespace {
// Degrees per second of one turn every `seconds`, signed: + counter-clockwise seen from above.
float rate(float seconds,bool clockwise) {return seconds>0 ? (clockwise ? -360.f : 360.f)/seconds : 0.f;}
}
float PgsLight::turnRate() const {return rate(turnSeconds,clockwise);}
PgsLight PgsLight::turned(float degrees) const {
    if (degrees==0) return *this;
    // About +Y: counter-clockwise seen from above, +X towards -Z.
    QMatrix4x4 turn;turn.rotate(degrees,0,1,0);
    PgsLight l=*this;l.position=turn.map(position);
    if (directional) l.setDirection(turn.mapVector(direction()));
    return l;
}
float PgsProject::rigTurnRate() const {return rate(rigTurnSeconds,rigClockwise);}
float PgsProject::lightAngle(int i,double seconds) const {
    if (i<0 || i>=lights.size()) return 0;
    return float(std::fmod(double(lights[i].turnRate()+rigTurnRate())*seconds,360.0));
}
bool PgsProject::anyTurning() const {
    if (rigTurnSeconds>0 && !lights.isEmpty()) return true;
    for (const auto &l:lights) if (l.turnSeconds>0) return true;
    return false;
}
PgsLight PgsLight::sun(const QVector3D &centre,float size) {
    PgsLight l;l.name=QStringLiteral("Sun");l.directional=true;l.intensity=1.5f;
    // From above, in front (+Z, where captures face) and to the left.
    const QVector3D from=QVector3D(-0.6f,1.2f,1.0f).normalized();
    l.rotation=rotationFor(-from);l.position=centre+from*std::max(size,0.5f)*0.35f;
    return l;
}
PgsLight PgsLight::bulb(const QVector3D &centre,float size) {
    PgsLight l;l.name=QStringLiteral("Light");l.directional=false;
    const float d=std::max(size,0.5f)*0.8f;l.position=centre+QVector3D(0.5f*d,0.3f*d,d);
    // As bright at the subject as a sun of 1.5: the inverse square at its distance.
    l.intensity=1.5f*(l.position-centre).lengthSquared();
    return l;
}

namespace {
// Where a light around the subject comes from: azimuth from the front (+Z) towards +X,
// elevation above the horizon, both in degrees.
QVector3D from(float azimuth,float elevation) {
    const float a=qDegreesToRadians(azimuth),e=qDegreesToRadians(elevation);
    return QVector3D(std::cos(e)*std::sin(a),std::sin(e),std::cos(e)*std::cos(a));
}
struct Rig {
    QVector3D centre;float d;
    // A point light `distance` subject sizes away, as bright at the subject as `light` (1: as
    // the capture was lit), fading to nothing four sizes away.
    PgsLight point(const QString &name,const QColor &colour,float azimuth,float elevation,float light,float distance=1.f) const {
        PgsLight l;l.name=name;l.colour=colour;l.position=centre+from(azimuth,elevation)*d*distance;
        l.intensity=light*(l.position-centre).lengthSquared();l.range=4*d;return l;
    }
    PgsLight sun(const QString &name,const QColor &colour,float azimuth,float elevation,float light) const {
        PgsLight l;l.name=name;l.directional=true;l.colour=colour;l.intensity=light;l.setAngles(azimuth,elevation);
        l.position=centre+from(azimuth,elevation)*d*1.3f;return l; // its icon, clear of the subject
    }
};
} // namespace

QStringList PgsLight::presetNames() {
    return {
            QStringLiteral("Drive 1: sodium and teal"),
            QStringLiteral("Drive 2: magenta and blue"),
            QStringLiteral("Red and cyan"),
            QStringLiteral("Three-point"),
            QStringLiteral("Daylight sun"),
            QStringLiteral("Sunset"),QStringLiteral("Soft studio"),
            QStringLiteral("Horror: green from below"),
            QStringLiteral("Campfire and moon"),
            QStringLiteral("Disco: turning colours")};
}
QVector<PgsLight> PgsLight::preset(int index,const QVector3D &centre,float size) {
    const Rig r{centre,std::max(size,0.5f)*0.6f};
    switch (index) {
    case 0: // a warm street lamp in front at one side, a bluish, barely green night all around the rest
        return {r.point("Sodium key",QColor(255,150,60),-60,20,2.0f,1.1f),r.point("Teal fill",QColor(30,150,200),55,10,2.2f,1.2f),
                r.point("Teal rim",QColor(50,175,215),155,30,4.0f)};
    case 1: return {r.point("Magenta rim",QColor(255,30,200),-105,15,6),r.point("Blue rim",QColor(30,90,255),105,15,8),
                    r.point("Violet fill",QColor(120,70,255),0,10,0.35f,1.4f)};
    case 2: return {r.point("Red",QColor(255,25,25),-90,5,5),r.point("Cyan",QColor(0,220,255),90,5,4)};
    case 3: return {r.point("Key",QColor(255,236,214),-40,35,2.2f,1.2f),r.point("Fill",QColor(190,210,255),45,10,0.6f,1.2f),
                    r.point("Rim",QColor(255,255,255),170,45,2.5f)};
    case 4: return {r.sun("Sun",QColor(255,244,229),-35,55,2.5f),r.sun("Sky",QColor(150,190,255),145,35,0.6f)};
    case 5: return {r.sun("Low sun",QColor(255,125,45),70,8,3),r.sun("Dusk sky",QColor(110,120,220),-110,40,0.8f)};
    case 6: return {r.point("Soft left",QColor(255,250,245),-45,20,1,1.3f),r.point("Soft right",QColor(255,250,245),45,20,1,1.3f),
                    r.sun("Top",QColor(255,255,255),0,85,0.5f)};
    case 7: return {r.point("Green from below",QColor(80,255,100),0,-55,3.5f,0.8f),r.point("Blue back",QColor(50,80,255),180,30,2)};
    case 8: return {r.point("Fire",QColor(255,110,35),20,-20,3.5f,0.8f),r.sun("Moon",QColor(130,160,255),-150,50,0.5f)};
    case 9: {
        // Three colours a third of a turn apart, going round at their own pace.
        QVector<PgsLight> lights{r.point("Magenta",QColor(255,30,200),0,25,3),r.point("Cyan",QColor(0,220,255),120,25,3),r.point("Yellow",QColor(255,210,0),240,25,3)};
        lights[0].turnSeconds=6;lights[1].turnSeconds=6;lights[2].turnSeconds=9;lights[2].clockwise=true;
        return lights;
    }
    default: return {};
    }
}
QVector<PgsLight> PgsLight::defaults(const QVector3D &centre,float size) {
    auto lights=preset(1,centre,size).mid(0,2); // the magenta and blue rims, without the fill
    for (auto &l:lights) l.enabled=false;
    return lights;
}

QJsonObject PgsProject::json(const QString &projectPath) const {
    QJsonArray lightArray;for (const auto &l:lights) lightArray.append(l.json());
    QJsonArray files;for (const auto &f:meshFiles) files.append(stored(f,projectPath));
    QJsonObject root{{"format","pgsproject"},{"version",1},{"ply",stored(ply,projectPath)},
        {"colmap",QJsonObject{{"folder",stored(colmap,projectPath)},{"declined",colmapDeclined}}},
        {"lights",lightArray},{"lightsTurn",QJsonObject{{"seconds",double(rigTurnSeconds)},{"clockwise",rigClockwise}}},{"selfIllumination",double(selfIllumination)},{"estimatedNormals",estimatedNormals},{"fineNormals",fineNormals},
        {"mesh",QJsonObject{{"settings",mesh.json()},{"files",files},{"inputs",meshInputs},{"edited",meshEdited.isEmpty() ? QString() : stored(meshEdited,projectPath)},{"lodsSource",lodsSource}}},
        {"pgs",QJsonObject{{"includeMesh",pgsIncludesMesh},{"mesh",pgsMesh},{"textureSize",pgsTextureSize}}}};
    if (cameraSet) root["camera"]=QJsonObject{{"target",vec(camera.target)},{"yaw",camera.yaw},{"pitch",camera.pitch},{"roll",camera.roll},{"distance",camera.distance},{"orthographic",camera.orthographic}};
    return root;
}
PgsProject PgsProject::fromJson(const QJsonObject &root,const QString &projectPath) {
    if (root.value("format").toString()!="pgsproject") throw std::runtime_error("Not a PGS Editor project.");
    if (root.value("version").toInt()>1) throw std::runtime_error("This project was saved by a newer PGS Editor.");
    PgsProject p;p.ply=restored(root.value("ply").toString(),projectPath);
    const auto colmap=root.value("colmap").toObject();p.colmap=restored(colmap.value("folder").toString(),projectPath);p.colmapDeclined=colmap.value("declined").toBool();
    for (const auto &l:root.value("lights").toArray()) p.lights.append(PgsLight::fromJson(l.toObject()));
    p.selfIllumination=float(std::clamp(root.value("selfIllumination").toDouble(0.1),0.0,1.0));
    const auto turn=root.value("lightsTurn").toObject();
    // a project saved without it turns as a new capture does: once every 5 s
    p.rigTurnSeconds=float(std::clamp(turn.value("seconds").toDouble(PgsProject{}.rigTurnSeconds),0.0,86400.0));p.rigClockwise=turn.value("clockwise").toBool();
    p.estimatedNormals=root.value("estimatedNormals").toBool();
    p.fineNormals=p.estimatedNormals && root.value("fineNormals").toBool();
    const auto mesh=root.value("mesh").toObject();p.mesh=MeshSettings::fromJson(mesh.value("settings").toObject());
    for (const auto &f:mesh.value("files").toArray()) p.meshFiles << restored(f.toString(),projectPath);
    p.meshInputs=mesh.value("inputs").toString();
    if (const QString edited=mesh.value("edited").toString();!edited.isEmpty()) p.meshEdited=restored(edited,projectPath);
    p.lodsSource=mesh.value("lodsSource").toString();
    const auto pgs=root.value("pgs").toObject();
    p.pgsIncludesMesh=pgs.value("includeMesh").toBool(true);p.pgsMesh=pgs.value("mesh").toInt(-1);p.pgsTextureSize=std::clamp(pgs.value("textureSize").toInt(2048),0,16384);
    if (root.contains("camera")) {
        const auto c=root.value("camera").toObject();p.cameraSet=true;
        p.camera.target=vec(c.value("target"),{});p.camera.yaw=float(c.value("yaw").toDouble(25));p.camera.pitch=float(c.value("pitch").toDouble(12));
        p.camera.roll=float(c.value("roll").toDouble(0));p.camera.distance=float(std::max(1e-3,c.value("distance").toDouble(4)));p.camera.orthographic=c.value("orthographic").toBool();
    }
    if (p.ply.isEmpty()) throw std::runtime_error("The project names no PLY.");
    return p;
}
QString PgsProject::currentMeshInputs() const {
    const QFileInfo file(ply);
    const QByteArray key=QJsonDocument(QJsonObject{{"ply",file.absoluteFilePath()},{"size",double(file.size())},{"modified",file.lastModified().toMSecsSinceEpoch()},
        {"colmap",colmapDeclined ? QString() : QFileInfo(colmap).absoluteFilePath()},{"settings",[this] {auto s=mesh;s.lods.clear();return s.json();}()}}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(key,QCryptographicHash::Sha256).toHex().left(32));
}
QStringList PgsProject::meshObjs() const {
    QStringList objs;
    for (const auto &f:meshFiles) if (f.endsWith(".obj",Qt::CaseInsensitive) && !f.endsWith("_dense.obj",Qt::CaseInsensitive) && f!=meshEdited) objs << f;
    if (!objs.isEmpty() && !meshEdited.isEmpty()) objs[0]=meshEdited;
    return objs;
}
QString PgsProject::generatedObj() const {
    for (const auto &f:meshFiles) if (f.endsWith(".obj",Qt::CaseInsensitive) && !f.endsWith("_dense.obj",Qt::CaseInsensitive) && f!=meshEdited) return f;
    return {};
}
QString PgsProject::fileSignature(const QString &path) {
    const QFileInfo file(path);
    if (!file.exists()) return {};
    return QStringLiteral("%1|%2|%3").arg(file.fileName()).arg(file.size()).arg(file.lastModified().toMSecsSinceEpoch());
}
bool PgsProject::lodsOutOfDate() const {
    const QStringList objs=meshObjs();
    return objs.size()>1 && !lodsSource.isEmpty() && lodsSource!=fileSignature(objs.first());
}
QString PgsProject::pgsMeshFile() const {
    const QStringList objs=meshObjs();
    if (objs.isEmpty()) return {};
    if (pgsMesh<0) return objs.last();
    return objs.value(std::min(pgsMesh,int(objs.size())-1));
}
