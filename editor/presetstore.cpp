#include "presetstore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace {
QString suffix(PresetScope scope) { return scope==PresetScope::Metadata ? "presetmetadata" : "preset"; }
QString format(PresetScope scope) { return scope==PresetScope::Metadata ? "vgs-editor-metadata-preset" : "vgs-editor-preset"; }
QJsonArray appliesTo(PresetScope scope,int version=6) {
    if (scope==PresetScope::Metadata) return {"capture-metadata","processing","output"};
    QJsonArray result{"capture-transform",version==3 ? "crop" : "modifiers"};
    if (version<6) result.append("view");
    result.append("playback");result.append("reference-spaces");return result;
}
}

PresetStore::PresetStore(const QString &directory) {
    directory_ = QDir::cleanPath(directory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/presets" : QFileInfo(directory).absoluteFilePath());
}
bool PresetStore::ensureDirectory(QString *error) const {
    if (QDir().mkpath(directory_)) return true;
    *error = QStringLiteral("Could not create the presets folder:\n%1").arg(QDir::toNativeSeparators(directory_)); return false;
}
bool PresetStore::read(const QString &path,EditorPreset *result,QString *error) const {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return false; }
    if (file.size()>4*1024*1024) { *error = QStringLiteral("The preset file is too large."); return false; }
    QJsonParseError parse; const auto document = QJsonDocument::fromJson(file.readAll(),&parse); const auto root = document.object();
    const QString name = root["name"].toString().trimmed();
    auto fail = [&] { *error = QStringLiteral("Invalid preset or unsupported preset version."); return false; };
    const double version = root["version"].toDouble();
    const bool legacy = version==1;
    if (parse.error!=QJsonParseError::NoError || !document.isObject() ||
        (version!=1 && version!=2 && version!=3 && version!=4 && version!=5 && version!=6) || name.isEmpty() || name.size()>120 || !root["configuration"].isObject()) return fail();
    PresetScope scope = PresetScope::Editor;
    if (!legacy) {
        if (root["scope"]!="editor" && root["scope"]!="capture-metadata") return fail();
        if (root["scope"]=="capture-metadata") scope = PresetScope::Metadata;
    }
    if (version>=3) {
        if (root["format"]!=format(scope) || QFileInfo(path).suffix().compare(suffix(scope),Qt::CaseInsensitive)!=0 ||
            root["appliesTo"].toArray()!=appliesTo(scope,int(version))) return fail();
    } else if (root["format"]!="vgs-editor-preset" || QFileInfo(path).suffix().compare("json",Qt::CaseInsensitive)!=0) return fail();
    auto configuration = root["configuration"].toObject(); const bool hasMetadata = configuration.contains("captureSettings");
    if (scope==PresetScope::Metadata) {
        if (configuration.size()!=1 || !hasMetadata) return fail();
        Project settings;
        if (!CaptureSettings::fromJson(configuration["captureSettings"].toObject(),&settings.captureSettings,error)) return fail();
        *result = {name,settings,scope,false,true}; return true;
    }
    if (!legacy && hasMetadata) return fail();
    // Preview settings from older presets are deliberately ignored, including
    // malformed values. Project parsing still expects these unrelated fields.
    const auto defaults=Project().json(path);
    configuration["view"]=defaults["view"];configuration["camera"]=defaults["camera"];
    const auto playback = configuration.take("playback").toObject();
    configuration["format"] = "vgs-editor-project";
    configuration["version"] = configuration["modifiers"].isArray() ? (version>=5 ? 8 : 7) : configuration["crop"].toObject()["space"]=="world" ? 6 : hasMetadata ? 5 : configuration["crop"].toObject().contains("shape") ? 4 : 3;
    if (!hasMetadata) configuration["captureSettings"] = CaptureSettings().json();
    configuration["asset"] = "__preset_capture__";
    configuration["timeline"] = QJsonObject{{"time",0},{"in",0},{"out",0},{"speed",playback["speed"]},{"loop",playback["loop"]}};
    Project settings;
    if (!Project::fromJson(configuration,directory_,&settings,error)) return fail();
    *result = {name,settings,scope,legacy,hasMetadata}; return true;
}
bool PresetStore::read(const QString &path,PresetScope scope,EditorPreset *result,QString *error) const {
    if (QFileInfo(path).suffix().compare(suffix(scope),Qt::CaseInsensitive)!=0) {
        *error = QStringLiteral("Select a .%1 preset for this panel.").arg(suffix(scope)); return false;
    }
    EditorPreset candidate;
    if (!read(path,&candidate,error)) return false;
    if (candidate.legacy || candidate.scope!=scope) {
        *error = QStringLiteral("This preset belongs to a different panel."); return false;
    }
    *result = std::move(candidate); return true;
}

QString PresetStore::presetPath(const QString &name,PresetScope scope) const {
    QString slug = name.normalized(QString::NormalizationForm_C).toCaseFolded().left(60);
    slug.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"),"_");
    while (slug.endsWith('.') || slug.endsWith(' ')) slug.chop(1);
    if (slug.isEmpty()) slug = "preset";
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(name.normalized(QString::NormalizationForm_C).toCaseFolded().toUtf8(),QCryptographicHash::Sha256).toHex().left(12));
    return QDir(directory_).filePath("preset-"+slug+"-"+hash+"."+suffix(scope));
}

bool PresetStore::writePreset(const QString &path,const QString &name,const Project &settings,PresetScope scope,QString *error) const {
    if (!ensureDirectory(error)) return false;
    auto configuration = settings.json(path);
    configuration.remove("format"); configuration.remove("version"); configuration.remove("asset"); configuration.remove("timeline");
    configuration.remove("view");configuration.remove("camera");
    configuration["playback"] = QJsonObject{{"speed",settings.speed},{"loop",settings.loop}};
    if (scope==PresetScope::Metadata) configuration = QJsonObject{{"captureSettings",settings.captureSettings.json()}};
    else configuration.remove("captureSettings");
    const QJsonObject root{{"format",format(scope)},{"version",scope==PresetScope::Metadata ? 4 : 6},{"name",name},{"scope",scope==PresetScope::Metadata ? "capture-metadata" : "editor"},{"appliesTo",appliesTo(scope)},{"configuration",configuration}};
    const auto bytes = QJsonDocument(root).toJson(); QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) { *error = file.errorString(); return false; }
    return true;
}

void PresetStore::migrateLegacyPresets() const {
    const auto files = QDir(directory_).entryInfoList({"*.json"},QDir::Files | QDir::NoSymLinks,QDir::Name);
    // Import the newer scoped JSON files first. Originals remain untouched, and an
    // existing native preset always wins so an old template cannot overwrite edits.
    for (bool mixed : {false,true}) for (const auto &file : files) {
        EditorPreset preset; QString error;
        if (!read(file.absoluteFilePath(),&preset,&error) || preset.legacy!=mixed) continue;
        auto import = [&](PresetScope scope) {
            const QString path = presetPath(preset.name,scope);
            if (!QFileInfo::exists(path)) writePreset(path,preset.name,preset.settings,scope,&error);
        };
        if (preset.legacy) {
            import(PresetScope::Editor);
            if (preset.hasMetadata) import(PresetScope::Metadata);
        } else import(preset.scope);
    }
}

QVector<PresetEntry> PresetStore::list(PresetScope scope) const {
    migrateLegacyPresets();
    QVector<PresetEntry> result;
    const auto files = QDir(directory_).entryInfoList({"*."+suffix(scope)},QDir::Files | QDir::NoSymLinks,QDir::Name);
    for (const auto &file : files) {
        EditorPreset preset; QString error;
        if (!read(file.absoluteFilePath(),scope,&preset,&error)) continue;
        bool duplicate = false;
        for (const auto &entry : result) duplicate |= entry.name.compare(preset.name,Qt::CaseInsensitive)==0;
        if (!duplicate) result.append({preset.name,file.absoluteFilePath()});
    }
    std::sort(result.begin(),result.end(),[](const PresetEntry &a,const PresetEntry &b) { return a.name.compare(b.name,Qt::CaseInsensitive)<0; });
    return result;
}

bool PresetStore::save(const QString &requestedName,const Project &settings,QString *resultPath,QString *error,PresetScope scope) const {
    const QString name = requestedName.trimmed();
    if (name.isEmpty() || name.size()>120) { *error = QStringLiteral("Enter a preset name between 1 and 120 characters."); return false; }
    QString path;
    for (const auto &entry : list(scope)) if (entry.name.compare(name,Qt::CaseInsensitive)==0) { path = entry.path; break; }
    if (path.isEmpty()) path = presetPath(name,scope);
    if (!writePreset(path,name,settings,scope,error)) return false;
    if (resultPath) *resultPath = path; return true;
}
