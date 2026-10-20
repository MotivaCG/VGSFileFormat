// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
#include "plysequencespec.h"
#include <QDir>
#include <QJsonArray>
#include <stdexcept>

namespace {
[[noreturn]] void fail(const QString &message) {throw std::runtime_error(message.toStdString());}
QString resolved(const QString &base,const QString &path) {return path.isEmpty() ? QString() : QDir::cleanPath(QDir(base).absoluteFilePath(path));}
QString relative(const QString &base,const QString &path) {return path.isEmpty() ? QString() : QDir(base).relativeFilePath(path);}
}

QJsonObject PlySequenceSpec::json(const QString &base) const {
    QJsonArray paths,indices,starts;for (const auto &file:files) paths.append(relative(base,file));for (auto n:numbers) indices.append(QString::number(n));for (int n:chunks) starts.append(n);
    return {{"version",1},{"files",paths},{"numbers",indices},{"chunks",starts},{"birthEvents",relative(base,births)},{"birthOrigin",QString::number(birthOrigin)},{"rowOffset",rowOffset}};
}
PlySequenceSpec PlySequenceSpec::fromJson(const QJsonObject &o,const QString &base) {
    PlySequenceSpec s;if (o.isEmpty()) return s;
    if (o["version"]!=1 || !o["files"].isArray() || !o["numbers"].isArray() || !o["chunks"].isArray()) fail("Invalid saved PLY sequence.");
    const auto paths=o["files"].toArray(),numbers=o["numbers"].toArray();if (paths.isEmpty() || paths.size()!=numbers.size() || paths.size()>1000000) fail("Invalid saved PLY frame list.");
    for (int i=0;i<paths.size();++i) {bool ok=false;const auto n=numbers[i].toString().toLongLong(&ok);
        if (!paths[i].isString() || paths[i].toString().isEmpty() || !ok || n<0) fail("Invalid saved PLY frame.");s.files.append(resolved(base,paths[i].toString()));s.numbers.append(n);}
    for (const auto &v:o["chunks"].toArray()) {const int n=v.toInt(-1);if (!v.isDouble() || n<0 || n>=s.files.size() || (!s.chunks.isEmpty() && n<=s.chunks.last())) fail("Invalid PLY chunk boundary.");s.chunks.append(n);}
    if (s.chunks.isEmpty() || s.chunks.first()!=0) fail("PLY chunks must start at frame zero.");
    bool ok=false;s.birthOrigin=o["birthOrigin"].toString().toLongLong(&ok);s.rowOffset=o["rowOffset"].toInt(-1);
    if (!ok || s.birthOrigin<0 || s.rowOffset<0 || s.rowOffset>10000000) fail("Invalid PLY birth-event mapping.");
    s.births=resolved(base,o["birthEvents"].toString());return s;
}
