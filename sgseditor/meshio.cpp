// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "meshio.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace {
[[noreturn]] void fail(const QString &message) {throw std::runtime_error(message.toStdString());}
// The textures an MTL's first material names.
void readMaterial(const QString &mtl,MeshData &mesh) {
    QFile file(mtl);if (!file.open(QIODevice::ReadOnly|QIODevice::Text)) return;
    const QDir dir=QFileInfo(mtl).dir();
    while (!file.atEnd()) {
        const QString line=QString::fromUtf8(file.readLine()).trimmed();
        const auto words=line.split(' ',Qt::SkipEmptyParts);if (words.size()<2) continue;
        const QString key=words[0].toLower(),name=words.last();
        if (key=="map_kd" && mesh.albedo.isEmpty()) mesh.albedo=dir.absoluteFilePath(name);
        else if ((key=="norm" || key=="map_bump" || key=="bump") && mesh.normalMap.isEmpty()) mesh.normalMap=dir.absoluteFilePath(name);
    }
}
} // namespace

QVector3D MeshData::minimum() const {
    QVector3D m(std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max());
    for (size_t i=0;i<vertices();++i) for (int a=0;a<3;++a) m[a]=std::min(m[a],positions[i*3+a]);
    return m;
}
QVector3D MeshData::maximum() const {
    QVector3D m(-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max());
    for (size_t i=0;i<vertices();++i) for (int a=0;a<3;++a) m[a]=std::max(m[a],positions[i*3+a]);
    return m;
}

MeshData readObj(const QString &path) {
    QFile file(path);if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot read %1.").arg(path));
    std::vector<float> v,vt,vn;MeshData mesh;
    std::map<std::tuple<int,int,int>,uint32_t> unique;bool missingNormals=false;
    const QByteArray all=file.readAll();
    qsizetype at=0;
    while (at<all.size()) {
        qsizetype end=all.indexOf('\n',at);if (end<0) end=all.size();
        const QByteArray line=all.mid(at,end-at).trimmed();at=end+1;
        if (line.isEmpty() || line[0]=='#') continue;
        const auto words=line.simplified().split(' ');
        const QByteArray &key=words[0];
        if (key=="v" && words.size()>=4) for (int i=1;i<4;++i) v.push_back(words[i].toFloat());
        else if (key=="vt" && words.size()>=3) {vt.push_back(words[1].toFloat());vt.push_back(words[2].toFloat());}
        else if (key=="vn" && words.size()>=4) for (int i=1;i<4;++i) vn.push_back(words[i].toFloat());
        else if (key=="mtllib" && words.size()>=2) readMaterial(QFileInfo(path).dir().absoluteFilePath(QString::fromUtf8(words.mid(1).join(' '))),mesh);
        else if (key=="f" && words.size()>=4) {
            std::vector<uint32_t> corners;
            for (int i=1;i<words.size();++i) {
                const auto parts=words[i].split('/');
                auto index=[&](int k,size_t count,int stride) -> int {
                    if (k>=parts.size() || parts[k].isEmpty()) return -1;
                    int n=parts[k].toInt();n=n<0 ? int(count/size_t(stride))+n : n-1;
                    if (n<0 || size_t(n)*size_t(stride)>=count) fail(QStringLiteral("%1: a face points past the vertices.").arg(path));
                    return n;
                };
                const auto key3=std::make_tuple(index(0,v.size(),3),index(1,vt.size(),2),index(2,vn.size(),3));
                if (std::get<0>(key3)<0) fail(QStringLiteral("%1: a face without positions.").arg(path));
                auto found=unique.find(key3);
                if (found==unique.end()) {
                    const uint32_t id=uint32_t(mesh.vertices());
                    for (int a=0;a<3;++a) mesh.positions.push_back(v[size_t(std::get<0>(key3))*3+a]);
                    if (std::get<1>(key3)>=0) {mesh.uvs.push_back(vt[size_t(std::get<1>(key3))*2]);mesh.uvs.push_back(vt[size_t(std::get<1>(key3))*2+1]);}
                    else {mesh.uvs.push_back(0);mesh.uvs.push_back(0);}
                    if (std::get<2>(key3)>=0) for (int a=0;a<3;++a) mesh.normals.push_back(vn[size_t(std::get<2>(key3))*3+a]);
                    else {missingNormals=true;for (int a=0;a<3;++a) mesh.normals.push_back(0);}
                    found=unique.emplace(key3,id).first;
                }
                corners.push_back(found->second);
            }
            for (size_t i=1;i+1<corners.size();++i) {mesh.indices.push_back(corners[0]);mesh.indices.push_back(corners[i]);mesh.indices.push_back(corners[i+1]);}
        }
    }
    if (mesh.indices.empty()) fail(QStringLiteral("%1 has no triangles.").arg(path));
    if (missingNormals) {
        // Area-weighted face normals, summed at each vertex.
        std::fill(mesh.normals.begin(),mesh.normals.end(),0.f);
        for (size_t t=0;t<mesh.triangles();++t) {
            const uint32_t *f=&mesh.indices[t*3];QVector3D p[3];
            for (int k=0;k<3;++k) p[k]=QVector3D(mesh.positions[f[k]*3],mesh.positions[f[k]*3+1],mesh.positions[f[k]*3+2]);
            const QVector3D n=QVector3D::crossProduct(p[1]-p[0],p[2]-p[0]);
            for (int k=0;k<3;++k) for (int a=0;a<3;++a) mesh.normals[f[k]*3+a]+=n[a];
        }
    }
    for (size_t i=0;i<mesh.vertices();++i) {
        QVector3D n(mesh.normals[i*3],mesh.normals[i*3+1],mesh.normals[i*3+2]);
        n=n.lengthSquared()>1e-20f ? n.normalized() : QVector3D(0,1,0);
        for (int a=0;a<3;++a) mesh.normals[i*3+a]=n[a];
    }
    return mesh;
}

namespace {
struct Binary {
    QByteArray bytes;QJsonArray views,accessors;
    // A buffer view of `data`, 4-byte aligned; returns its index.
    int view(const QByteArray &data,int target=0) {
        while (bytes.size()%4) bytes.append('\0');
        QJsonObject v{{"buffer",0},{"byteOffset",bytes.size()},{"byteLength",data.size()}};
        if (target) v["target"]=target;
        bytes.append(data);views.append(v);return int(views.size()-1);
    }
    int accessor(int view,int componentType,qsizetype count,const QString &type,const QJsonArray &minimum={},const QJsonArray &maximum={}) {
        QJsonObject a{{"bufferView",view},{"componentType",componentType},{"count",count},{"type",type}};
        if (!minimum.isEmpty()) {a["min"]=minimum;a["max"]=maximum;}
        accessors.append(a);return int(accessors.size()-1);
    }
};
QByteArray floats(const std::vector<float> &values) {
    QByteArray out(qsizetype(values.size()*4),Qt::Uninitialized);
    for (size_t i=0;i<values.size();++i) qToLittleEndian(values[i],out.data()+i*4);
    return out;
}
QImage fitted(const QImage &image,int side) {
    if (side<=0 || std::max(image.width(),image.height())<=side) return image;
    return image.scaled(side,side,Qt::KeepAspectRatio,Qt::SmoothTransformation);
}
QByteArray encoded(const QImage &image,const char *format,int quality) {
    QByteArray out;QBuffer buffer(&out);buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer,format,quality)) fail(QStringLiteral("Cannot encode a texture as %1.").arg(format));
    return out;
}
} // namespace

QByteArray writeGlb(const MeshData &mesh,const QImage &albedo,const QImage &normalMap,const GlbOptions &options) {
    if (mesh.indices.empty()) fail("The mesh has no triangles.");
    Binary b;const qsizetype n=qsizetype(mesh.vertices());
    const QVector3D lo=mesh.minimum(),hi=mesh.maximum();
    const int positions=b.accessor(b.view(floats(mesh.positions),34962),5126,n,"VEC3",{lo.x(),lo.y(),lo.z()},{hi.x(),hi.y(),hi.z()});
    const int normals=b.accessor(b.view(floats(mesh.normals),34962),5126,n,"VEC3");
    // glTF counts texture coordinates from the image's top: OBJ's v is turned over.
    std::vector<float> uv(mesh.uvs);for (size_t i=1;i<uv.size();i+=2) uv[i]=1.f-uv[i];
    const int texcoords=b.accessor(b.view(floats(uv),34962),5126,n,"VEC2");
    QByteArray indexBytes;const bool small=n<=65535;
    indexBytes.resize(qsizetype(mesh.indices.size()*(small ? 2 : 4)));
    for (size_t i=0;i<mesh.indices.size();++i) {
        if (small) qToLittleEndian(quint16(mesh.indices[i]),indexBytes.data()+i*2);
        else qToLittleEndian(quint32(mesh.indices[i]),indexBytes.data()+i*4);
    }
    const int indices=b.accessor(b.view(indexBytes,34963),small ? 5123 : 5125,qsizetype(mesh.indices.size()),"SCALAR");
    QJsonArray images,textures,samplers{QJsonObject{{"magFilter",9729},{"minFilter",9987},{"wrapS",33071},{"wrapT",33071}}};
    QJsonObject pbr{{"metallicFactor",0.0},{"roughnessFactor",0.8}};
    QJsonObject material{{"name","capture"},{"pbrMetallicRoughness",pbr}};
    auto image=[&](const QImage &source,const char *format,const QString &mime,int quality) {
        const QImage scaled=fitted(source,options.textureSize).convertToFormat(QImage::Format_RGB888);
        images.append(QJsonObject{{"bufferView",b.view(encoded(scaled,format,quality))},{"mimeType",mime}});
        textures.append(QJsonObject{{"sampler",0},{"source",int(images.size()-1)}});
        return int(textures.size()-1);
    };
    if (!albedo.isNull()) {pbr["baseColorTexture"]=QJsonObject{{"index",image(albedo,"JPG","image/jpeg",options.jpegQuality)}};material["pbrMetallicRoughness"]=pbr;}
    if (options.normalMap && !normalMap.isNull()) material["normalTexture"]=QJsonObject{{"index",image(normalMap,"PNG","image/png",-1)}};
    QJsonObject attributes{{"POSITION",positions},{"NORMAL",normals},{"TEXCOORD_0",texcoords}};
    QJsonObject root{
        {"asset",QJsonObject{{"version","2.0"},{"generator","SGS Editor"}}},
        {"scene",0},{"scenes",QJsonArray{QJsonObject{{"nodes",QJsonArray{0}}}}},
        {"nodes",QJsonArray{QJsonObject{{"name","capture"},{"mesh",0}}}},
        {"meshes",QJsonArray{QJsonObject{{"name","capture"},{"primitives",QJsonArray{QJsonObject{{"attributes",attributes},{"indices",indices},{"material",0},{"mode",4}}}}}}},
        {"materials",QJsonArray{material}},
        {"accessors",b.accessors},{"bufferViews",b.views}};
    if (!images.isEmpty()) {root["images"]=images;root["textures"]=textures;root["samplers"]=samplers;}
    while (b.bytes.size()%4) b.bytes.append('\0');
    root["buffers"]=QJsonArray{QJsonObject{{"byteLength",b.bytes.size()}}};
    QByteArray json=QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (json.size()%4) json.append(' ');
    QByteArray out;
    auto u32=[&](quint32 value) {char word[4];qToLittleEndian(value,word);out.append(word,4);};
    u32(0x46546C67);u32(2);u32(quint32(12+8+json.size()+8+b.bytes.size()));
    u32(quint32(json.size()));u32(0x4E4F534A);out.append(json);
    u32(quint32(b.bytes.size()));u32(0x004E4942);out.append(b.bytes);
    return out;
}

GlbInfo inspectGlb(const QByteArray &glb) {
    GlbInfo info;
    auto u32=[&](qsizetype at) {return qFromLittleEndian<quint32>(glb.constData()+at);};
    if (glb.size()<20 || u32(0)!=0x46546C67 || u32(4)!=2 || u32(8)!=quint32(glb.size())) {info.error=QStringLiteral("not a glTF 2.0 binary");return info;}
    const quint32 jsonLength=u32(12);
    if (u32(16)!=0x4E4F534A || 20+qsizetype(jsonLength)>glb.size()) {info.error=QStringLiteral("no JSON chunk");return info;}
    QJsonParseError parse{};const auto doc=QJsonDocument::fromJson(glb.mid(20,jsonLength),&parse);
    if (parse.error!=QJsonParseError::NoError || !doc.isObject()) {info.error=QStringLiteral("invalid JSON chunk");return info;}
    info.json=doc.object();
    const qsizetype binAt=20+jsonLength;
    if (binAt+8<=glb.size() && u32(binAt+4)==0x004E4942) info.binary=glb.mid(binAt+8,u32(binAt));
    if (info.json.value("asset").toObject().value("version").toString()!="2.0") {info.error=QStringLiteral("not glTF 2.0");return info;}
    const auto accessors=info.json.value("accessors").toArray();
    const auto views=info.json.value("bufferViews").toArray();
    for (const auto &mesh:info.json.value("meshes").toArray()) for (const auto &primitive:mesh.toObject().value("primitives").toArray()) {
        const auto p=primitive.toObject();
        const int position=p.value("attributes").toObject().value("POSITION").toInt(-1),index=p.value("indices").toInt(-1);
        if (position<0 || position>=accessors.size() || index<0 || index>=accessors.size()) {info.error=QStringLiteral("a primitive without positions or indices");return info;}
        info.vertices+=size_t(accessors[position].toObject().value("count").toInt());
        info.triangles+=size_t(accessors[index].toObject().value("count").toInt())/3;
    }
    for (const auto &view:views) {
        const auto v=view.toObject();
        if (v.value("byteOffset").toInt()+v.value("byteLength").toInt()>info.binary.size()) {info.error=QStringLiteral("a buffer view past the binary chunk");return info;}
    }
    info.images=int(info.json.value("images").toArray().size());info.materials=int(info.json.value("materials").toArray().size());
    return info;
}
