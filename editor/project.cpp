// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "project.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>
#include <algorithm>
#include <QtMath>
#include <QQuaternion>
#include <QSet>

QMatrix4x4 Transform::matrix() const {
    QMatrix4x4 m;
    m.translate(position);
    m.rotate(rotation.z(), 0, 0, 1);
    m.rotate(rotation.y(), 0, 1, 0);
    m.rotate(rotation.x(), 1, 0, 0);
    QMatrix4x4 h; h(0,1) = shear.x(); h(0,2) = shear.y(); h(1,2) = shear.z(); m *= h;
    m.scale(scale);
    return m;
}
Transform Transform::fromMatrix(const QMatrix4x4 &m) {
    Transform result; result.position = m.column(3).toVector3D();
    QVector3D x = m.column(0).toVector3D(), y = m.column(1).toVector3D(), z = m.column(2).toVector3D();
    const float sx = std::max(x.length(),1e-8f); x /= sx;
    const float xy = QVector3D::dotProduct(x,y); y -= x*xy;
    const float sy = std::max(y.length(),1e-8f); y /= sy;
    const float xz = QVector3D::dotProduct(x,z), yz = QVector3D::dotProduct(y,z); z -= x*xz+y*yz;
    const float sz = std::max(z.length(),1e-8f); z /= sz;
    result.scale = {sx,sy,sz}; result.shear = {xy/sy,xz/sz,yz/sz};
    QMatrix4x4 r; r.setColumn(0,QVector4D(x,0)); r.setColumn(1,QVector4D(y,0)); r.setColumn(2,QVector4D(z,0));
    const float ry = std::asin(std::clamp(-r(2,0),-1.0f,1.0f));
    const bool singular = std::abs(std::cos(ry))<1e-5f;
    result.rotation = {qRadiansToDegrees(singular ? 0.0f : std::atan2(r(2,1),r(2,2))),qRadiansToDegrees(ry),
        qRadiansToDegrees(singular ? std::atan2(-r(0,1),r(1,1)) : std::atan2(r(1,0),r(0,0)))};
    return result;
}
QMatrix4x4 Transform::rotationMatrix() const {
    QMatrix4x4 m;
    m.rotate(rotation.z(),0,0,1); m.rotate(rotation.y(),0,1,0); m.rotate(rotation.x(),1,0,0);
    return m;
}
Transform Transform::rotatedLocal(int axis,float degrees) const {
    auto m = rotationMatrix(); QVector3D direction; direction[axis] = 1;
    m.rotate(degrees,direction); // Postmultiply: rotate about a local axis.
    Transform result = *this;
    const float y = std::asin(std::clamp(-m(2,0),-1.0f,1.0f));
    const bool singular = std::abs(std::cos(y))<1e-5f;
    const float x = singular ? 0 : std::atan2(m(2,1),m(2,2));
    const float z = singular ? std::atan2(-m(0,1),m(1,1)) : std::atan2(m(1,0),m(0,0));
    result.rotation = {qRadiansToDegrees(x),qRadiansToDegrees(y),qRadiansToDegrees(z)};
    return result;
}
QVector3D Camera::position() const {
    const float y = qDegreesToRadians(yaw), p = qDegreesToRadians(pitch);
    return target + QVector3D(std::sin(y)*std::cos(p),std::sin(p),std::cos(y)*std::cos(p))*distance;
}
QMatrix4x4 Camera::viewMatrix() const {
    const float y = qDegreesToRadians(yaw), p = qDegreesToRadians(pitch);
    const QVector3D direction(std::sin(y)*std::cos(p),std::sin(p),std::cos(y)*std::cos(p));
    QVector3D up = pitch>89.5f ? QVector3D(0,0,-1) : pitch<-89.5f ? QVector3D(0,0,1) : QVector3D(0,1,0);
    up = QQuaternion::fromAxisAndAngle(direction,roll).rotatedVector(up);
    QMatrix4x4 view; view.lookAt(target+direction*distance,target,up); return view;
}
bool CropVolume::contains(const QVector3D &position) const {
    if (!enabled) return true;
    const QVector3D p = transform.matrix().inverted().map(position);
    if (p.y()<0 || p.y()>height) return false;
    return shape==CropShape::Box ? std::abs(p.x())<=width*0.5f && std::abs(p.z())<=depth*0.5f
        : insideEllipse(p.x(),p.z());
}
static QJsonArray vec(const QVector3D &v) { return {v.x(), v.y(), v.z()}; }
QJsonObject Project::json(const QString &path) const {
    return {{"format", "vgs-editor-project"}, {"version", 8},
        {"asset", QDir(QFileInfo(path).absolutePath()).relativeFilePath(asset)},
        {"transform", QJsonObject{{"position", vec(transform.position)}, {"rotation", vec(transform.rotation)}, {"scale", vec(transform.scale)}, {"shear",vec(transform.shear)}}},
        {"camera", QJsonObject{{"target", vec(camera.target)}, {"yaw", camera.yaw}, {"pitch", camera.pitch}, {"roll",camera.roll}, {"distance", camera.distance}, {"preset", int(camera.preset)}, {"orthographic", camera.orthographic}}},
        {"crop", QJsonObject{{"enabled",crop().enabled},{"space","world"},{"shape",crop().shape==CropShape::Box ? "box" : "cylinder"},
            {"width",crop().width},{"depth",crop().depth},{"radius",crop().radius},{"height",crop().height},
            {"position",vec(crop().transform.position)},{"rotation",vec(crop().transform.rotation)},{"scale",vec(crop().transform.scale)}, {"shear",vec(crop().transform.shear)}}},
        {"modifiers",[&] {
            QJsonArray items=modifierJson();
            for (qsizetype i=0;i<items.size();++i) {auto item=items[i].toObject();if (item["type"]!="audio" || item["audio"].toObject()["source"]!="file") continue;
                auto audio=item["audio"].toObject();audio["file"]=QDir(QFileInfo(path).absolutePath()).relativeFilePath(audio["file"].toString());item["audio"]=audio;items[i]=item;}
            return items;}()},{"selection",QJsonObject{{"modifier",selectedModifier}}},
        {"spaces",QJsonArray{int(spaces[0]),int(spaces[1]),int(spaces[2])}},
        {"captureSettings",captureSettings.json()},
        {"timeline", QJsonObject{{"time", time}, {"in", in}, {"out", out}, {"speed", speed}, {"loop", loop}}},
        {"view", QJsonObject{{"grid", grid}, {"sh", true}, {"pointSize", pointSize}}}};
}
bool Project::write(const QString &path, QString *error) const {
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(json(path)).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = file.errorString(); return false;
    }
    return true;
}
bool unpackRecords(const QString &text,std::vector<uint32_t> *records); // modifiers.cpp
bool Project::read(const QString &path, Project *result, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return false; }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error!=QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("Invalid project or unsupported project version."); return false;
    }
    return fromJson(doc.object(),QFileInfo(path).absolutePath(),result,error);
}
bool Project::fromJson(const QJsonObject &root,const QString &baseDirectory,Project *result,QString *error) {
    auto fail = [&] { *error = QStringLiteral("Invalid project or unsupported project version."); return false; };
    const double version = root["version"].toDouble();
    if (root["format"] != "vgs-editor-project" || version<1 || version>8 || version!=std::floor(version) ||
        !root["asset"].isString() || root["asset"].toString().isEmpty()) return fail();
    Project p;
    p.asset = QDir::cleanPath(QDir(baseDirectory).absoluteFilePath(root["asset"].toString()));
    bool valid = true;
    // Values are stored as floats, so a limit written back reads a rounding off it (the
    // 0.0001 m crop minimum as 9.99999975e-05): within a millionth it is the limit.
    auto inRange = [](double n, double lo, double hi) {
        return std::isfinite(n) && n >= lo - std::abs(lo)*1e-6 && n <= hi + std::abs(hi)*1e-6;
    };
    auto number = [&](const QJsonObject &o, const char *key, double lo, double hi) {
        const auto value = o[key];
        const double n = value.toDouble();
        valid &= value.isDouble() && inRange(n, lo, hi);
        return std::clamp(n, lo, hi);
    };
    auto vector = [&](const QJsonObject &o, const char *key, double lo, double hi) {
        const auto a = o[key].toArray(); QVector3D v;
        if (a.size() != 3) { valid = false; return v; }
        for (int i = 0; i < 3; ++i) {
            const double n = a[i].toDouble();
            valid &= a[i].isDouble() && inRange(n, lo, hi);
            v[i] = float(std::clamp(n, lo, hi));
        }
        return v;
    };
    const auto t = root["transform"].toObject(), c = root["camera"].toObject();
    p.transform.position = vector(t, "position", -1e6, 1e6);
    p.transform.rotation = vector(t, "rotation", -36000, 36000);
    p.transform.scale = vector(t, "scale", 0.0001, 10000);
    p.camera.target = vector(c, "target", -1e7, 1e7);
    p.camera.yaw = float(number(c, "yaw", -36000, 36000));
    p.camera.pitch = float(number(c, "pitch", -90, 90));
    p.camera.distance = float(number(c, "distance", 0.001, 1e7));
    if (root["version"].toInt()>=2) {
        const double preset = number(c,"preset",0,6);
        valid &= preset==std::floor(preset) && c["orthographic"].isBool();
        p.camera.preset = ViewPreset(int(preset)); p.camera.orthographic = c["orthographic"].toBool();
        p.camera.roll = float(number(c,"roll",-36000,36000));
        if (p.camera.preset==ViewPreset::Free && p.camera.orthographic) return fail();
        if (version<7) {
        const auto crop = root["crop"].toObject();
        valid &= crop["enabled"].isBool(); p.crop().enabled = crop["enabled"].toBool();
        p.crop().radius = p.crop().radiusZ = float(number(crop,"radius",0.0001,1e6)); p.crop().height = float(number(crop,"height",0.0001,1e6));
        p.crop().width = p.crop().depth = 2*p.crop().radius;
        if (root["version"].toInt()>=4) {
            if (crop["shape"]!="box" && crop["shape"]!="cylinder") return fail();
            p.crop().shape = crop["shape"]=="box" ? CropShape::Box : CropShape::Cylinder;
            p.crop().width = float(number(crop,"width",0.0001,1e6)); p.crop().depth = float(number(crop,"depth",0.0001,1e6));
        }
        p.crop().transform.position = vector(crop,"position",-1e6,1e6);
        p.crop().transform.rotation = vector(crop,"rotation",-36000,36000);
        p.crop().transform.scale = vector(crop,"scale",0.0001,10000);
        if (root["version"].toInt()==2 && p.crop().enabled) {
            // Version 2 placed the origin at the centre; preserve its volume with a base pivot.
            p.crop().transform.position += p.crop().transform.matrix().mapVector({0,-p.crop().height*0.5f,0});
        }
    }
        }
    if (root["version"].toInt()>=3) {
        p.transform.shear = vector(t,"shear",-1e6,1e6);
        if (version<7) p.crop().transform.shear = vector(root["crop"].toObject(),"shear",-1e6,1e6);
        const auto spaces = root["spaces"].toArray(); if (spaces.size()!=3) return fail();
        for (int i=0; i<3; ++i) {
            const double space = spaces[i].toDouble(-1);
            valid &= space==0 || space==1; p.spaces[i] = space==1 ? CoordinateSpace::Local : CoordinateSpace::Global;
        }
    }
    if (root["version"].toInt()>=5 && !CaptureSettings::fromJson(root["captureSettings"].toObject(),&p.captureSettings,error)) return false;
    if (version==6 && root["crop"].toObject()["space"]!="world") return fail();
    if (version<6 && p.crop().enabled) p.crop().transform = Transform::fromMatrix(p.transform.matrix()*p.crop().transform.matrix());
    if (version>=7) {
        if (!root["modifiers"].isArray()) return fail();
        p.modifiers.clear();QSet<QString> ids;
        auto identity=[&](const QJsonObject &item,const char *key,int maximum) {
            const auto value=item[key];const QString text=value.toString().trimmed();valid &= value.isString() && !text.isEmpty() && text.size()<=maximum;return text;
        };
        for (const auto &value:root["modifiers"].toArray()) {
            if (!value.isObject()) return fail();const auto item=value.toObject();Modifier m;
            m.id=identity(item,"id",80);m.name=identity(item,"name",120);valid &= !ids.contains(m.id) && item["enabled"].isBool() && item["timeline"]=="full";ids.insert(m.id);m.enabled=item["enabled"].toBool();
            if (item["type"]=="crop") {
                const auto c=item["crop"].toObject();valid &= c["space"]=="world" && (c["shape"]=="box" || c["shape"]=="cylinder");m.crop.enabled=m.enabled;m.crop.shape=c["shape"]=="box" ? CropShape::Box : CropShape::Cylinder;
                m.crop.radius=float(number(c,"radius",.0001,1e6));m.crop.radiusZ=c.contains("radiusZ") ? float(number(c,"radiusZ",.0001,1e6)) : m.crop.radius;m.crop.height=float(number(c,"height",.0001,1e6));m.crop.width=float(number(c,"width",.0001,1e6));m.crop.depth=float(number(c,"depth",.0001,1e6));
                m.crop.transform.position=vector(c,"position",-1e6,1e6);m.crop.transform.rotation=vector(c,"rotation",-36000,36000);m.crop.transform.scale=vector(c,"scale",.0001,10000);m.crop.transform.shear=vector(c,"shear",-1e6,1e6);
                // Older projects have neither: keep, shown in red while editing.
                valid &= !c.contains("mode") || c["mode"]=="keep" || c["mode"]=="remove";m.crop.remove=c["mode"]=="remove";
                valid &= !c.contains("editPreview") || c["editPreview"]=="red" || c["editPreview"]=="hide";m.crop.showRemovedInRed=c["editPreview"]!="hide";
                // Optional: an animated crop's keys, kept even while it is static.
                m.cropAnimation.still=m.crop;
                if (c.contains("animation")) {
                    const auto a=c["animation"].toObject();valid &= (a["mode"]=="static" || a["mode"]=="animated") && a["interpolation"]=="linear-slerp" && a["keys"].isArray();QSet<int> frames;
                    for (const auto &value:a["keys"].toArray()) {
                        if (!value.isObject()) return fail();const auto key=value.toObject();const double frame=number(key,"frame",0,1000000);valid &= frame==std::floor(frame) && !frames.contains(int(frame));frames.insert(int(frame));
                        CropVolume pose;pose.transform.position=vector(key,"position",-1e6,1e6);pose.transform.rotation=vector(key,"rotation",-36000,36000);pose.transform.scale=vector(key,"scale",.0001,10000);pose.transform.shear=vector(key,"shear",-1e6,1e6);
                        pose.radius=float(number(key,"radius",.0001,1e6));pose.radiusZ=float(number(key,"radiusZ",.0001,1e6));pose.height=float(number(key,"height",.0001,1e6));pose.width=float(number(key,"width",.0001,1e6));pose.depth=float(number(key,"depth",.0001,1e6));
                        m.cropAnimation.setKey(int(frame),pose);
                    }
                    m.cropAnimation.animated=a["mode"]=="animated" && !m.cropAnimation.keys.isEmpty();
                }
            } else if (item["type"]=="remove-green") {
                m.type=ModifierType::RemoveGreen;const auto g=item["green"].toObject();
                m.green.minimumSaturation=float(number(g,"minimumSaturation",0,1));m.green.hueTolerance=float(number(g,"hueTolerance",0,180));valid &= g["targetHue"].toDouble()==120 && g["colourSource"]=="dc";
                valid &= !g.contains("colourSpace") || g["colourSpace"]=="srgb" || g["colourSpace"]=="linear-rgb";
                m.green.linearRgb=g["colourSpace"]=="linear-rgb";
            } else if (item["type"]=="animate-transform") {
                m.type=ModifierType::AnimateTransform;
                if (item.contains("animation")) {
                    const auto a=item["animation"].toObject();valid &= a["space"]=="reference-offset" && a["interpolation"]=="linear-slerp" && a["keys"].isArray();QSet<int> frames;
                    for (const auto &value:a["keys"].toArray()) {
                        if (!value.isObject()) return fail();const auto key=value.toObject();const double frame=number(key,"frame",0,1000000);valid &= frame==std::floor(frame) && !frames.contains(int(frame));frames.insert(int(frame));
                        Transform offset;offset.position=vector(key,"position",-1e6,1e6);offset.rotation=vector(key,"rotation",-36000,36000);offset.scale=vector(key,"scale",.0001,10000);offset.shear=vector(key,"shear",-1e6,1e6);m.animation.setKey(int(frame),offset);
                    }
                }
            } else if (item["type"]=="purge-isolated") {
                m.type=ModifierType::PurgeIsolated;
                if (item.contains("isolation")) {const auto p=item["isolation"].toObject();const double n=number(p,"neighbour",1,256);valid &= n==std::floor(n);m.isolation.neighbour=int(n);m.isolation.medianPercent=number(p,"medianPercent",0,1000000);}
            } else if (item["type"]=="bake-antialiasing") {
                m.type=ModifierType::BakeAntialiasing;const auto b=item["bake"].toObject();
                m.bakeDistance=number(b,"distance",.01,1000);const double height=number(b,"screenHeight",16,16384);
                valid &= height==std::floor(height) && (!b.contains("verticalFov") || b["verticalFov"].toDouble()==45);m.bakeScreenHeight=int(height);
            } else if (item["type"]=="erase") {
                m.type=ModifierType::Erase;const auto e=item["erase"].toObject();valid &= e["identity"]=="source-chunk-record" && e["chunks"].isArray();
                for (const auto &value:e["chunks"].toArray()) {
                    const auto c=value.toObject();const double chunk=number(c,"chunk",0,10000000);valid &= chunk==std::floor(chunk) && !m.erased.count(int(chunk));
                    std::vector<uint32_t> records;valid &= unpackRecords(c["records"].toString(),&records);if (valid) m.erased[int(chunk)]=std::move(records);
                }
            } else if (item["type"]=="colour") {
                m.type=ModifierType::Colour;const auto c=item["colour"].toObject();
                m.colourExposure=number(c,"exposure",-8,8);m.colourTemperature=number(c,"temperature",-4,4);m.colourTint=number(c,"tint",-4,4);m.colourSaturation=number(c,"saturation",0,4);
                valid &= c["despill"].isBool();m.colourDespill=c["despill"].toBool() ? Modifier::DespillOnExport : Modifier::DespillNone;
                if (c.contains("despillMode")) {const auto mode=c["despillMode"].toString();valid &= mode=="none" || mode=="export" || mode=="always";
                    m.colourDespill=mode=="always" ? Modifier::DespillAlways : mode=="export" ? Modifier::DespillOnExport : Modifier::DespillNone;}m.colourDespillStrength=number(c,"despillStrength",0,1);
                // Added after the first Colour modifiers were saved: those take the defaults.
                if (c.contains("opacity")) m.colourOpacity=number(c,"opacity",0,16);
                if (c.contains("greenGain")) m.colourGreenGain=number(c,"greenGain",0,2);
                if (c.contains("viewChroma")) m.colourViewChroma=number(c,"viewChroma",0,1);
                if (c.contains("recoverSkin")) {valid &= c["recoverSkin"].isBool();m.colourRecoverSkin=c["recoverSkin"].toBool();}
            } else if (item["type"]=="audio") {
                m.type=ModifierType::Audio;const auto a=item["audio"].toObject();m.audioOffset=number(a,"offset",-36000,36000);
                valid &= a["source"]=="capture" || a["source"]=="file";
                if (a["source"]=="file") {
                    // Kept relative to the project, like the capture; an absolute path still works.
                    const QString file=a["file"].toString();valid &= !file.isEmpty();
                    m.audioFile=QFileInfo(file).isRelative() ? QDir(baseDirectory).absoluteFilePath(file) : file;
                }
            } else if (item["type"]=="prune-low-contribution") {
                m.type=ModifierType::PruneLowContribution;const auto p=item["prune"].toObject();
                m.prune.percent=number(p,"percent",0,90);m.prune.protectAbove=number(p,"protectAbove",0,1000);valid &= p["protectUnit"]=="px-1080p-mean";
            } else if (item["type"]=="walk") {
                m.type=ModifierType::Walk;const auto w=item["walk"].toObject();valid &= w["axis"]=="+z";m.walkSpeed=number(w,"speed",0,100);
                valid &= !w.contains("display") || w["display"]=="m/s" || w["display"]=="km/h";m.walkKmh=w["display"]=="km/h";
            } else return fail();
            p.modifiers.append(m);
        }
        p.selectedModifier=root["selection"].toObject()["modifier"].toString();
        if (!p.modifier()) p.selectedModifier=p.modifiers.isEmpty() ? QString() : p.modifiers.front().id;
    }
    const auto tl = root["timeline"].toObject(), view = root["view"].toObject();
    p.time = number(tl, "time", 0, 1e9); p.in = number(tl, "in", 0, 1e9);
    p.out = number(tl, "out", 0, 1e9); p.speed = number(tl, "speed", 0.1, 4);
    p.pointSize = number(view, "pointSize", 1, 12);
    valid &= tl["loop"].isBool() && view["grid"].isBool() && view["sh"].isBool();
    // Keep reading version-1 projects, but their old SH toggle no longer affects rendering.
    p.loop = tl["loop"].toBool(); p.grid = view["grid"].toBool();
    // Despill used to be an export setting; it is a Colour modifier's now. A project, preset or
    // task that despilled keeps doing so, with the same settings, as a modifier at the end.
    if (p.captureSettings.despill) {
        Modifier despill;despill.id=newId();despill.name=QStringLiteral("Despill");despill.type=ModifierType::Colour;despill.colourDespill=Modifier::DespillOnExport;
        const auto &s=p.captureSettings;despill.colourDespillStrength=s.despillStrength;despill.colourGreenGain=s.greenGain;despill.colourViewChroma=s.viewChromaScale;despill.colourRecoverSkin=s.recoverSkin;
        p.modifiers.append(despill);p.captureSettings.despill=false;
    }
    if (!valid || p.out < p.in || p.time < p.in || p.time > p.out) return fail();
    *result = p; return true;
}
