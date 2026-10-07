#include "filehistory.h"
#include <QDir>
#include <QFileInfo>

QString FileHistory::normalized(const QString &path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
bool FileHistory::samePath(const QString &a, const QString &b) {
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}
QStringList FileHistory::recent() const {
    QStringList result;
    for (const QString &path : settings_.value("RecentFiles").toStringList()) {
        if (path.isEmpty()) continue;
        const QString clean = normalized(path);
        bool duplicate = false;
        for (const QString &entry : result) duplicate |= samePath(entry, clean);
        if (!duplicate) result.append(clean);
        if (result.size() == MaximumRecentFiles) break;
    }
    return result;
}
void FileHistory::remember(const QString &path) {
    const QString clean = normalized(path);
    QStringList files = recent();
    for (int i = files.size()-1; i >= 0; --i) if (samePath(files[i], clean)) files.removeAt(i);
    files.prepend(clean);
    while (files.size() > MaximumRecentFiles) files.removeLast();
    const QString category = QFileInfo(clean).suffix().compare("vgsproj",Qt::CaseInsensitive) == 0 ? "Project" : "Capture";
    settings_.setValue("RecentFiles",files);
    settings_.setValue("Files/Last" + category,clean);
    settings_.setValue("Files/Last" + category + "Dir",QFileInfo(clean).absolutePath());
    settings_.sync();
}
void FileHistory::clearRecent() { settings_.remove("RecentFiles"); settings_.sync(); }
QString FileHistory::directory(const QString &category) const {
    const QString saved = settings_.value("Files/Last" + category + "Dir").toString();
    if (!saved.isEmpty() && QDir(saved).exists()) return saved;
    const QString file = settings_.value("Files/Last" + category).toString();
    if (!file.isEmpty() && QDir(QFileInfo(file).absolutePath()).exists()) return QFileInfo(file).absolutePath();
    return QDir::homePath();
}
QString FileHistory::openPath(const QString &category) const {
    const QString file = settings_.value("Files/Last" + category).toString();
    return !file.isEmpty() && QFileInfo(file).isFile() ? file : directory(category);
}
QString FileHistory::savePath(const QString &category, const QString &name) const {
    return QDir(directory(category)).filePath(name);
}
void FileHistory::rememberImage(const QString &path) {
    const QString clean = normalized(path);
    settings_.setValue("Files/LastImage",clean);
    settings_.setValue("Files/LastImageDir",QFileInfo(clean).absolutePath());
    settings_.sync();
}
