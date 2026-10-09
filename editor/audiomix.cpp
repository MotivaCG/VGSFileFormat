// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#include "audiomix.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
QString seconds(double value) { return QString::number(value,'f',6); }
}

QString ffmpegPath() {
    const QString configured=qEnvironmentVariable("VGS_FFMPEG");
    if (!configured.isEmpty()) return QFileInfo(configured).isFile() ? QFileInfo(configured).absoluteFilePath() : QString();
#ifdef Q_OS_WIN
    const QString name=QStringLiteral("ffmpeg.exe");
#else
    const QString name=QStringLiteral("ffmpeg");
#endif
    const QString beside=QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools/")+name);
    return QFileInfo(beside).isFile() ? beside : QString();
}

QByteArray mixAudioAac(const QVector<AudioInput> &inputs,double length,const std::function<bool()> &cancelled) {
    const QString ffmpeg=ffmpegPath();
    if (ffmpeg.isEmpty()) throw std::runtime_error("ffmpeg was not found beside the editor (tools/ffmpeg.exe).");
    if (inputs.isEmpty() || !(length>0)) throw std::runtime_error("There is no audio to mix.");
    QTemporaryDir dir;if (!dir.isValid()) throw std::runtime_error("Cannot create a temporary folder for the audio.");
    const QString output=dir.filePath(QStringLiteral("soundtrack.m4a"));
    QStringList args{"-hide_banner","-nostdin","-loglevel","error","-y"};
    QStringList chains,labels;
    for (int i=0;i<inputs.size();++i) {
        const auto &input=inputs[i];
        if (!QFileInfo(input.path).isFile()) throw std::runtime_error(QStringLiteral("Cannot read the audio file %1.").arg(input.path).toStdString());
        // A looped track is read endlessly and trimmed in the graph, which works however far
        // in it starts; a plain one seeks, which is quicker.
        if (input.loop) args << "-stream_loop" << "-1";
        else if (input.start>0) args << "-ss" << seconds(input.start);
        args << "-i" << QDir::toNativeSeparators(input.path);
        QString chain=QStringLiteral("[%1:a]").arg(i);
        if (input.loop && input.start>0) chain+=QStringLiteral("atrim=start=%1,asetpts=PTS-STARTPTS,").arg(seconds(input.start));
        chain+=QStringLiteral("aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,volume=%1").arg(seconds(std::clamp(input.volume,0.,1.)));
        if (input.start<0) {const qint64 delay=qint64(std::llround(-input.start*1000));chain+=QStringLiteral(",adelay=%1:all=1").arg(delay);}
        chains << chain+QStringLiteral("[a%1]").arg(i);labels << QStringLiteral("[a%1]").arg(i);
    }
    // Mixed at their own levels, not divided among themselves, then padded with silence to
    // the length and cut there.
    QString graph=chains.join(';')+';';
    graph+=inputs.size()>1 ? labels.join("")+QStringLiteral("amix=inputs=%1:duration=longest:normalize=0,apad[out]").arg(inputs.size())
                           : labels.first()+QStringLiteral("apad[out]");
    args << "-filter_complex" << graph << "-map" << "[out]" << "-t" << seconds(length)
         << "-vn" << "-c:a" << "aac" << "-b:a" << "160k" << "-movflags" << "+faststart" << QDir::toNativeSeparators(output);
    QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(ffmpeg,args);
    if (!process.waitForStarted(10000)) throw std::runtime_error("Cannot start ffmpeg.");
    while (!process.waitForFinished(100)) {
        if (cancelled && cancelled()) {process.kill();process.waitForFinished(5000);throw std::runtime_error("Export canceled.");}
        if (process.state()==QProcess::NotRunning) break;
    }
    if (process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0) {
        const QString message=QString::fromUtf8(process.readAllStandardError()).trimmed();
        throw std::runtime_error(QStringLiteral("ffmpeg could not make the soundtrack:\n%1").arg(message.right(800)).toStdString());
    }
    QFile file(output);if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("ffmpeg wrote no soundtrack.");
    const QByteArray bytes=file.readAll();if (bytes.isEmpty()) throw std::runtime_error("ffmpeg wrote an empty soundtrack.");
    return bytes;
}
