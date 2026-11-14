// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "sgsply.h"
#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
struct Property {QByteArray name;char kind=0;int offset=0;};
int bytesOf(char kind) {return kind=='d' ? 8 : kind=='f' || kind=='i' || kind=='I' ? 4 : kind=='h' || kind=='H' ? 2 : 1;}
char kindOf(const QByteArray &type) {
    if (type=="float" || type=="float32") return 'f';
    if (type=="double" || type=="float64") return 'd';
    if (type=="char" || type=="int8") return 'b';
    if (type=="uchar" || type=="uint8") return 'B';
    if (type=="short" || type=="int16") return 'h';
    if (type=="ushort" || type=="uint16") return 'H';
    if (type=="int" || type=="int32") return 'i';
    if (type=="uint" || type=="uint32") return 'I';
    return 0;
}
} // namespace

QString SgsPly::comment(const QString &key) const {
    for (const auto &c:comments) if (c.startsWith(key+' ')) return c.mid(key.size()+1).trimmed();
    return {};
}

SgsPly readSgsPly(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("Cannot read %1: %2").arg(path,file.errorString()));
    auto error=[&](const QString &reason) {fail(QStringLiteral("%1: %2").arg(path,reason));};
    if (file.readLine(16).trimmed()!="ply") error("not a PLY file.");
    SgsPly out;std::vector<Property> properties;size_t count=0;int stride=0;
    bool ascii=false,big=false,format=false,vertex=false,other=false,ended=false;qint64 header=0;
    while (!file.atEnd()) {
        const QByteArray line=file.readLine(65538);header+=line.size();
        if (header>1<<20) error("its header is too long.");
        const auto words=line.simplified().split(' ');if (words.isEmpty() || words[0].isEmpty()) continue;
        if (words[0]=="format") {
            if (words.size()!=3) error("invalid format line.");
            ascii=words[1]=="ascii";big=words[1]=="binary_big_endian";format=ascii || big || words[1]=="binary_little_endian";
        } else if (words[0]=="comment" || words[0]=="obj_info") {
            out.comments << QString::fromUtf8(line.trimmed().mid(words[0].size()).trimmed());
        } else if (words[0]=="element") {
            if (words.size()!=3) error("invalid element line.");
            if (words[1]=="vertex" && !vertex && properties.empty()) {
                bool ok=false;const qulonglong n=words[2].toULongLong(&ok);
                if (!ok || n>10000000) error("it holds more than 10 million splats.");
                count=size_t(n);vertex=true;
            } else if (!vertex) error("the vertex element must come first.");
            else other=true;
        } else if (words[0]=="property" && vertex && !other) {
            if (words.size()!=3 || !kindOf(words[1])) error("lists and unknown types are not Gaussian attributes.");
            const char kind=kindOf(words[1]);
            properties.push_back({words[2],kind,stride});stride+=bytesOf(kind);
        } else if (words[0]=="end_header") {ended=true;break;}
    }
    if (!ended || !format || !vertex || !stride) error("incomplete or unsupported header.");
    auto index=[&](const char *name) {for (size_t i=0;i<properties.size();++i) if (properties[i].name==name) return int(i);return -1;};
    for (const char *name:{"x","y","z"}) if (index(name)<0) error(QStringLiteral("no %1 property.").arg(name));
    if (index("f_dc_0")<0 && index("red")<0) error("no colour (f_dc_0 or red): not a Gaussian splatting PLY.");
    int rest=0;for (const auto &p:properties) if (p.name.startsWith("f_rest_")) ++rest;
    if (rest!=0 && rest!=9 && rest!=24 && rest!=45) error("its spherical harmonics need 0, 9, 24 or 45 f_rest properties.");
    const int coefficients=rest/3;out.shDegree=rest==45 ? 3 : rest==24 ? 2 : rest==9 ? 1 : 0;
    // The rows, as bytes or, for ASCII, as numbers.
    QByteArray data;std::vector<double> text;
    if (ascii) {
        text.reserve(count*properties.size());
        for (size_t row=0;row<count;++row) {
            const auto words=file.readLine(1<<17).simplified().split(' ');
            if (words.size()<qsizetype(properties.size())) error("truncated ASCII vertex.");
            for (size_t i=0;i<properties.size();++i) {bool ok=false;text.push_back(words[qsizetype(i)].toDouble(&ok));if (!ok) error("invalid number.");}
        }
    } else {
        data=file.read(qint64(count)*stride);
        if (data.size()!=qint64(count)*stride) error("truncated vertex data.");
    }
    auto value=[&](size_t row,int field) -> double {
        if (ascii) return text[row*properties.size()+size_t(field)];
        const auto &p=properties[size_t(field)];const uchar *at=reinterpret_cast<const uchar *>(data.constData())+row*size_t(stride)+size_t(p.offset);
        auto u16=[&] {return big ? qFromBigEndian<quint16>(at) : qFromLittleEndian<quint16>(at);};
        auto u32=[&] {return big ? qFromBigEndian<quint32>(at) : qFromLittleEndian<quint32>(at);};
        switch (p.kind) {
        case 'f': {const quint32 bits=u32();float v;std::memcpy(&v,&bits,4);return v;}
        case 'd': {const quint64 bits=big ? qFromBigEndian<quint64>(at) : qFromLittleEndian<quint64>(at);double v;std::memcpy(&v,&bits,8);return v;}
        case 'b': return qint8(*at);
        case 'B': return *at;
        case 'h': return qint16(u16());
        case 'H': return u16();
        case 'i': return qint32(u32());
        default: return u32();
        }
    };
    const int xyz[]={index("x"),index("y"),index("z")},dc[]={index("f_dc_0"),index("f_dc_1"),index("f_dc_2")};
    const int rgb[]={index("red"),index("green"),index("blue")},scale[]={index("scale_0"),index("scale_1"),index("scale_2")};
    const int rot[]={index("rot_0"),index("rot_1"),index("rot_2"),index("rot_3")},opacity=index("opacity");
    const int normal[]={index("nx"),index("ny"),index("nz")};
    std::vector<int> restFields;for (int i=0;i<rest;++i) {restFields.push_back(index(("f_rest_"+QByteArray::number(i)).constData()));if (restFields.back()<0) error("an f_rest property is missing.");}
    auto &f=out.frame;f.count=count;f.shCoefficients=coefficients;
    f.position.resize(count*3);f.rotation.resize(count*4);f.scale.resize(count*3);f.colorDc.resize(count*3);f.opacity.resize(count);f.active.resize(count);
    f.shRest.resize(count*size_t(coefficients)*3);
    const bool normals=normal[0]>=0 && normal[1]>=0 && normal[2]>=0;bool anyNormal=false;
    if (normals) f.normal.assign(count*3,0.f);
    for (size_t row=0;row<count;++row) {
        for (int c=0;c<3;++c) {
            f.position[row*3+c]=float(value(row,xyz[c]));
            f.scale[row*3+c]=scale[c]<0 ? .002f : float(std::exp(value(row,scale[c])));
            f.colorDc[row*3+c]=dc[c]>=0 ? float(.5+.28209479177387814*value(row,dc[c])) : rgb[c]>=0 ? float(value(row,rgb[c])/255.) : .5f;
        }
        f.opacity[row]=opacity<0 ? 1.f : float(1/(1+std::exp(-value(row,opacity))));
        // The file's quaternion is wxyz; the frame's xyzw.
        double norm=0;
        for (int c=0;c<4;++c) {const double v=rot[c]<0 ? (c==0 ? 1. : 0.) : value(row,rot[c]);norm+=v*v;f.rotation[row*4+size_t((c+3)%4)]=float(v);}
        if (!(norm>1e-30) || !std::isfinite(norm)) {f.rotation[row*4]=f.rotation[row*4+1]=f.rotation[row*4+2]=0;f.rotation[row*4+3]=1;norm=1;}
        for (int c=0;c<4;++c) f.rotation[row*4+c]/=float(std::sqrt(norm));
        // f_rest is channel-major: every coefficient's red, then green, then blue.
        for (int k=0;k<coefficients;++k) for (int c=0;c<3;++c) f.shRest[(row*size_t(coefficients)+size_t(k))*3+size_t(c)]=float(value(row,restFields[size_t(c*coefficients+k)]));
        bool finite=std::isfinite(f.opacity[row]);
        for (int c=0;c<3;++c) finite=finite && std::isfinite(f.position[row*3+c]) && std::isfinite(f.scale[row*3+c]) && std::isfinite(f.colorDc[row*3+c]);
        if (!finite) {f.opacity[row]=0;for (int c=0;c<3;++c) {f.position[row*3+c]=0;f.scale[row*3+c]=0;}}
        f.active[row]=finite && f.opacity[row]>1.f/255 && std::max({f.scale[row*3],f.scale[row*3+1],f.scale[row*3+2]})>1e-4f;
        if (normals) {
            float n[3];double length=0;
            for (int c=0;c<3;++c) {n[c]=float(value(row,normal[c]));length+=double(n[c])*n[c];}
            if (length>1e-12 && std::isfinite(length)) {for (int c=0;c<3;++c) f.normal[row*3+c]=float(n[c]/std::sqrt(length));anyNormal=true;}
        }
    }
    for (float &v:f.shRest) if (!std::isfinite(v)) v=0;
    // 3DGS training writes nx, ny, nz as zeros: no normals at all.
    if (!anyNormal) f.normal.clear();
    return out;
}
