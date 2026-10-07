#include "capturesettings.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

QJsonObject CaptureSettings::json() const {
    return {{"version",1},{"catalogueId",catalogueId},{"title",title},{"author",author},{"projectName",projectName},
        {"takeName",takeName},{"studio",studio},{"copyright",copyright},{"softwareName",softwareName},{"softwareVersion",softwareVersion},
        {"tags",QJsonArray::fromStringList(tags)},{"extraJson",extraJson},{"plain",plain},{"shDegree",shDegree},{"playbackMode",playbackMode},
        {"despill",despill},{"despillStrength",despillStrength},{"greenGain",greenGain},{"viewChromaScale",viewChromaScale},{"recoverSkin",recoverSkin}};
}
bool CaptureSettings::validate(QString *error) const {
    for (const auto &value : {catalogueId,title,author,projectName,takeName,studio,copyright,softwareName,softwareVersion})
        if (value.toUtf8().size()>1024) { *error = QStringLiteral("Metadata fields must not exceed 1024 UTF-8 bytes."); return false; }
    if (tags.size()>256) { *error = QStringLiteral("Use no more than 256 tags."); return false; }
    for (const auto &tag : tags) if (tag.toUtf8().size()>1024) { *error = QStringLiteral("Each tag must not exceed 1024 UTF-8 bytes."); return false; }
    if (shDegree<-1 || shDegree>3 || playbackMode<-1 || playbackMode>2 || !std::isfinite(despillStrength) || despillStrength<0 || despillStrength>1 ||
        !std::isfinite(greenGain) || greenGain<0 || greenGain>2 || !std::isfinite(viewChromaScale) || viewChromaScale<0 || viewChromaScale>1) {
        *error = QStringLiteral("Invalid processing or output settings."); return false;
    }
    if (extraJson.toUtf8().size()>1024*1024) { *error = QStringLiteral("Extra metadata must not exceed 1 MiB."); return false; }
    if (!extraJson.trimmed().isEmpty()) {
        QJsonParseError parse; const auto document = QJsonDocument::fromJson(extraJson.toUtf8(),&parse);
        if (parse.error!=QJsonParseError::NoError || document.isNull()) { *error = QStringLiteral("Extra metadata must be a valid JSON object or array."); return false; }
    }
    return true;
}
bool CaptureSettings::fromJson(const QJsonObject &o,CaptureSettings *result,QString *error) {
    bool valid = o["version"].toDouble()==1; CaptureSettings s;
    auto string = [&](const char *key) { valid &= o[key].isString(); return o[key].toString(); };
    auto boolean = [&](const char *key) { valid &= o[key].isBool(); return o[key].toBool(); };
    auto integer = [&](const char *key) { const auto v = o[key]; valid &= v.isDouble() && v.toDouble()==v.toInt(); return v.toInt(); };
    auto number = [&](const char *key) { valid &= o[key].isDouble(); return o[key].toDouble(); };
    s.catalogueId = string("catalogueId"); s.title = string("title"); s.author = string("author"); s.projectName = string("projectName");
    s.takeName = string("takeName"); s.studio = string("studio"); s.copyright = string("copyright"); s.softwareName = string("softwareName");
    s.softwareVersion = string("softwareVersion"); s.extraJson = string("extraJson");
    valid &= o["tags"].isArray(); for (const auto &tag : o["tags"].toArray()) { valid &= tag.isString(); s.tags.append(tag.toString()); }
    s.plain = boolean("plain"); s.despill = boolean("despill"); s.recoverSkin = boolean("recoverSkin");
    s.shDegree = integer("shDegree"); s.playbackMode = integer("playbackMode");
    s.despillStrength = number("despillStrength"); s.greenGain = number("greenGain"); s.viewChromaScale = number("viewChromaScale");
    if (!valid) { *error = QStringLiteral("Invalid metadata or processing configuration."); return false; }
    if (!s.validate(error)) return false; *result = s; return true;
}
