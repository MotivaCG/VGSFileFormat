// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#pragma once
#include <QJsonObject>
#include <QStringList>
#include <QVector>

// A frozen import, independent of files subsequently added beside the capture. Projects
// keep it, so the viewer builds this alone; reading the sequence is plysequence.h's.
struct PlySequenceSpec {
    QStringList files;
    QVector<qint64> numbers;
    QVector<int> chunks;
    QString births;
    qint64 birthOrigin=0;
    int rowOffset=0;
    bool empty() const {return files.isEmpty();}
    QJsonObject json(const QString &base) const;
    static PlySequenceSpec fromJson(const QJsonObject &,const QString &base);
};
