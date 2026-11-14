// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "sgsexport.h"
#include "exportcapture.h"
#include "nativeexport.h"
#include "sgsversion.h"
#include "vgscodec.h"
#include "vgssign.h"
#include "vgsdecoder/vgsdecoder.h"
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryFile>
#include <cmath>

void exportSgsPly(const SgsCapture &capture,const QString &destination) {
    if (QFileInfo(destination).absoluteFilePath().compare(capture.path,Qt::CaseInsensitive)==0)
        throw std::runtime_error("Choose a destination other than the PLY the editor has open.");
    writePly(capture.frame,destination); // editor/exportcapture.cpp: normals included when the frame has them
}

SgsExportResult exportSgs(const SgsCapture &capture,const SgsExportOptions &options,const QString &destination,const SgsProgress &progress) {
    auto report=[&](int percent,const QString &stage) {if (progress) progress(percent,stage);};
    if (QFileInfo(destination).absoluteFilePath().compare(capture.path,Qt::CaseInsensitive)==0)
        throw std::runtime_error("Choose a destination other than the PLY the editor has open.");
    const int degree=options.shDegree<0 ? capture.shDegree : std::min(options.shDegree,capture.shDegree);
    // Only the visible splats: the others are never drawn, and a still has no later instant
    // they could appear at.
    vgs::Frame frame;const auto &f=capture.frame;const size_t coefficients=size_t(f.shCoefficients);
    const bool normals=f.normal.size()==size_t(f.count)*3;
    frame.count=0;frame.shCoefficients=f.shCoefficients;
    for (size_t i=0;i<size_t(f.count);++i) {
        if (!f.active[i]) continue;
        ++frame.count;
        frame.position.insert(frame.position.end(),f.position.begin()+ptrdiff_t(i*3),f.position.begin()+ptrdiff_t(i*3+3));
        frame.rotation.insert(frame.rotation.end(),f.rotation.begin()+ptrdiff_t(i*4),f.rotation.begin()+ptrdiff_t(i*4+4));
        frame.scale.insert(frame.scale.end(),f.scale.begin()+ptrdiff_t(i*3),f.scale.begin()+ptrdiff_t(i*3+3));
        frame.colorDc.insert(frame.colorDc.end(),f.colorDc.begin()+ptrdiff_t(i*3),f.colorDc.begin()+ptrdiff_t(i*3+3));
        frame.opacity.push_back(f.opacity[i]);frame.active.push_back(1);
        frame.shRest.insert(frame.shRest.end(),f.shRest.begin()+ptrdiff_t(i*coefficients*3),f.shRest.begin()+ptrdiff_t((i+1)*coefficients*3));
        if (normals) frame.normal.insert(frame.normal.end(),f.normal.begin()+ptrdiff_t(i*3),f.normal.begin()+ptrdiff_t(i*3+3));
    }
    if (!frame.count) throw std::runtime_error("The capture has no visible splats.");
    report(5,QStringLiteral("Packing the splats"));
    vgs::DecodedChunk chunk=packExportFrame(frame,degree);
    if (normals) setFrameNormals(chunk,frame);

    vgs::Header header;header.timeNumerator=1;header.timeDenominator=30;header.frameCount=1;header.durationTicks=1;header.shDegree=uint32_t(degree);
    header.chunks.resize(1);header.chunks[0].startTick=0;header.chunks[0].intervals=1;
    vgs::EncodeOptions encode;encode.shDegree=uint32_t(degree);encode.signer=vgs::authoringSigner();
    encode.playbackMode=vgs::PlaybackMode::Loop;encode.renderHints=0; // a PLY is plain 3DGS
    encode.chunkThreads=1;
    auto utf=[](const QString &v) {return v.toUtf8().toStdString();};
    encode.metadata.title=utf(options.title.isEmpty() ? QFileInfo(capture.path).completeBaseName() : options.title);
    encode.metadata.author=utf(options.author);encode.metadata.copyright=utf(options.copyright);
    encode.metadata.softwareName="SGS Editor";encode.metadata.softwareVersion=utf(QStringLiteral(SGS_EDITOR_VERSION_NAME).mid(1));
    if (!options.thumbnailJpeg.isEmpty()) encode.extras.push_back({vgs::ThumbnailExtra,vgs::Jpeg,vgs::Bytes(options.thumbnailJpeg.begin(),options.thumbnailJpeg.end())});
    if (!options.glb.isEmpty()) encode.extras.push_back({vgs::ProxyMeshExtra,vgs::Glb,vgs::Bytes(options.glb.begin(),options.glb.end())});
    QJsonObject provenance=options.provenance;
    provenance["sgsEditorVersion"]=QStringLiteral(SGS_EDITOR_VERSION_NAME);provenance["source"]=QFileInfo(capture.path).fileName();
    provenance["normals"]=normals ? normalsSourceText(capture.normals) : QStringLiteral("none");
    const QByteArray json=QJsonDocument(provenance).toJson(QJsonDocument::Compact);
    encode.extras.push_back({vgs::MetadataExtra2,vgs::JsonUtf8,vgs::Bytes(json.begin(),json.end())});

    QTemporaryFile temporary(QFileInfo(destination).absolutePath()+"/.sgs-export-XXXXXX");
    if (!temporary.open()) throw std::runtime_error(temporary.errorString().toStdString());
    report(15,QStringLiteral("Encoding"));
    auto provider=[&](size_t) {return chunk;};
    auto sink=[&](uint64_t offset,const uint8_t *data,size_t count) {
        if (!temporary.seek(qint64(offset)) || temporary.write(reinterpret_cast<const char *>(data),qint64(count))!=qint64(count)) throw std::runtime_error(temporary.errorString().toStdString());
    };
    vgs::encodeSequence(header,provider,sink,encode,[&](int done,int total) {report(15+70*done/std::max(1,total),QStringLiteral("Encoding"));return true;});
    if (!temporary.flush()) throw std::runtime_error(temporary.errorString().toStdString());
    // Read back as any player would: authentic, one frame, every splat, the mesh intact.
    report(88,QStringLiteral("Verifying"));
    SgsExportResult result;
    {
        auto verified=vgsdec::Capture::openFile(temporary.fileName().toStdString());
        const auto &decoded=verified.setTime(0,true);
        if (decoded.splatCount!=frame.count) throw std::runtime_error("Verification failed: the file does not hold every splat.");
        if (normals && !decoded.normals) throw std::runtime_error("Verification failed: the normals were not written.");
        if (!options.glb.isEmpty()) {
            const auto mesh=verified.proxyMesh();
            if (mesh.size()!=size_t(options.glb.size()) || std::memcmp(mesh.data(),options.glb.constData(),mesh.size())!=0) throw std::runtime_error("Verification failed: the mesh does not read back.");
        }
    }
    QSaveFile output(destination);output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error(output.errorString().toStdString());
    temporary.seek(0);
    while (!temporary.atEnd()) {const QByteArray bytes=temporary.read(4*1024*1024);if (bytes.isEmpty() || output.write(bytes)!=bytes.size()) throw std::runtime_error("Cannot save the capture.");}
    if (!output.commit()) throw std::runtime_error(output.errorString().toStdString());
    result.bytes=quint64(QFileInfo(destination).size());
    result.notes << QStringLiteral("%1 splats, SH degree %2.").arg(frame.count).arg(degree);
    if (capture.frame.count!=frame.count) result.notes << QStringLiteral("%1 invisible splats left out.").arg(capture.frame.count-frame.count);
    result.notes << (normals ? QStringLiteral("Normals %1.").arg(normalsSourceText(capture.normals)) : QStringLiteral("No normals: players cannot light it."));
    if (!options.glb.isEmpty()) result.notes << QStringLiteral("Mesh: %1 KB.").arg(options.glb.size()/1024);
    report(100,QStringLiteral("Done"));
    return result;
}
