// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "sgsqueue.h"
#include "colmapfinder.h"
#include "sgscapture.h"
#include "sgsproject.h"
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {
enum Column { PlyColumn, TrianglesColumn, ColmapColumn, StatusColumn };
const QList<QPair<QString, int>> kTriangles{{"50 000", 50000}, {"100 000", 100000}, {"200 000", 200000}, {"400 000", 400000}, {"800 000", 800000}};
bool samePath(const QString &a, const QString &b) {
    return QFileInfo(a).absoluteFilePath().compare(QFileInfo(b).absoluteFilePath(), Qt::CaseInsensitive) == 0;
}
QString stateName(SgsMeshQueue::State s) {
    switch (s) {
    case SgsMeshQueue::State::Running: return "running";
    case SgsMeshQueue::State::Done: return "done";
    case SgsMeshQueue::State::Failed: return "failed";
    case SgsMeshQueue::State::Cancelled: return "cancelled";
    default: return "waiting";
    }
}
SgsMeshQueue::State stateFrom(const QString &s) {
    if (s == "done") return SgsMeshQueue::State::Done;
    if (s == "failed") return SgsMeshQueue::State::Failed;
    if (s == "cancelled") return SgsMeshQueue::State::Cancelled;
    return SgsMeshQueue::State::Waiting; // a row running when the editor closed starts again
}
} // namespace

QString SgsMeshQueue::projectPath(const QString &ply) {
    const QFileInfo f(ply);
    return f.absolutePath() + "/" + f.completeBaseName() + ".sgsproj";
}
QString SgsMeshQueue::meshFolder(const QString &ply) {
    const QFileInfo f(ply);
    return f.absolutePath() + "/" + f.completeBaseName() + "_mesh";
}

SgsMeshQueue::SgsMeshQueue(Gs2Mesh *editorMesh, std::function<QString()> openPly, QWidget *parent)
    : QDialog(parent), editorMesh_(editorMesh), openPly_(std::move(openPly)), runner_(new Gs2Mesh(this)), poll_(new QTimer(this)) {
    setWindowTitle(tr("Mesh queue"));
    setAcceptDrops(true);
    resize(1000, 420);
    auto *layout = new QVBoxLayout(this);
    auto *about = new QLabel(tr("Each PLY becomes a project beside it (<name>.sgsproj, with fine normals and its photos) and its main mesh "
                                "(<name>_mesh), as Generate mesh makes them: open them afterwards to touch the mesh up and make its LODs. "
                                "Add PLYs from any folders, or drop them here. The photos are the COLMAP found beside each PLY; empty: from the splats alone."));
    about->setWordWrap(true);
    layout->addWidget(about);
    table_ = new QTableWidget(0, 4);
    table_->setHorizontalHeaderLabels({tr("PLY"), tr("Triangles"), tr("Photos (COLMAP)"), tr("Status")});
    table_->horizontalHeader()->setSectionResizeMode(PlyColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(TrianglesColumn, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(ColmapColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(StatusColumn, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setWordWrap(false); // one line per path, cut where the column ends
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(table_, 1);
    summary_ = new QLabel;
    layout->addWidget(summary_);
    auto *row = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add PLYs…"));
    photosButton_ = new QPushButton(tr("Photos…"));
    photosButton_->setToolTip(tr("The COLMAP folder of the selected PLY"));
    removeButton_ = new QPushButton(tr("Remove"));
    openButton_ = new QPushButton(tr("Open project"));
    startButton_ = new QPushButton(tr("Start"));
    stopButton_ = new QPushButton(tr("Stop"));
    auto *close = new QPushButton(tr("Close"));
    close->setToolTip(tr("The queue goes on while this window is closed"));
    for (auto *b : {add, photosButton_, removeButton_, openButton_}) row->addWidget(b);
    row->addStretch(1);
    for (auto *b : {startButton_, stopButton_, close}) row->addWidget(b);
    layout->addLayout(row);

    connect(add, &QPushButton::clicked, this, [this] {
        QSettings settings;
        const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Add PLYs to the queue"), settings.value("MeshQueue/Directory", settings.value("Sgs/Directory")).toString(),
                                                                tr("3D Gaussian Splatting PLY (*.ply)"));
        if (!paths.isEmpty()) settings.setValue("MeshQueue/Directory", QFileInfo(paths.first()).absolutePath());
        for (const auto &p : paths) addPly(p);
    });
    connect(photosButton_, &QPushButton::clicked, this, [this] {
        const int r = selectedRow();
        if (r < 0) return;
        const QString start = items_[r].colmap.isEmpty() ? QFileInfo(items_[r].ply).absolutePath() : items_[r].colmap;
        const QString dir = QFileDialog::getExistingDirectory(this, tr("COLMAP folder of %1").arg(QFileInfo(items_[r].ply).fileName()), start);
        if (!dir.isEmpty()) setColmap(r, dir);
    });
    connect(removeButton_, &QPushButton::clicked, this, [this] {if (const int r = selectedRow(); r >= 0) removeRow(r);});
    connect(openButton_, &QPushButton::clicked, this, [this] {
        const int r = selectedRow();
        if (r >= 0 && items_[r].state == State::Done) emit openRequested(projectPath(items_[r].ply));
    });
    connect(startButton_, &QPushButton::clicked, this, &SgsMeshQueue::start);
    connect(stopButton_, &QPushButton::clicked, this, &SgsMeshQueue::stop);
    connect(close, &QPushButton::clicked, this, &QDialog::hide);
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {if (!syncing_) syncButtons();});
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (!syncing_ && it->column() == ColmapColumn) setColmap(it->row(), it->text().trimmed());
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int r, int c) {
        if (c != ColmapColumn && r >= 0 && r < count() && items_[r].state == State::Done) emit openRequested(projectPath(items_[r].ply));
    });

    connect(runner_, &Gs2Mesh::progress, this, [this](QString stage, double, double total) {
        if (current_ < 0) return;
        items_[current_].status = tr("Making the mesh: %1, %2%").arg(stage).arg(int(total * 100));
        syncRow(current_);
    });
    connect(runner_, &Gs2Mesh::finished, this, [this](bool ok, QString error, QStringList files) {finishItem(ok, error, files);});
    poll_->setInterval(500);
    connect(poll_, &QTimer::timeout, this, &SgsMeshQueue::next);
    load();
    syncTable();
}

SgsMeshQueue::~SgsMeshQueue() {
    running_ = false;
    runner_->cancel();
}

int SgsMeshQueue::selectedRow() const {
    const auto rows = table_->selectionModel()->selectedRows();
    return rows.isEmpty() ? -1 : rows.first().row();
}

bool SgsMeshQueue::addPly(const QString &path) {
    const QFileInfo f(path);
    if (f.suffix().compare("ply", Qt::CaseInsensitive) != 0 || !f.isFile()) return false;
    for (const auto &it : items_)
        if (samePath(it.ply, path)) return false;
    Item it;
    it.ply = f.absoluteFilePath();
    it.colmap = suggestColmap(it.ply);
    it.status = QFileInfo::exists(projectPath(it.ply)) ? tr("Waiting; its project is kept and gets the new mesh") : tr("Waiting");
    items_.append(it);
    save();
    syncTable();
    return true;
}

void SgsMeshQueue::setTriangles(int row, int triangles) {
    if (row < 0 || row >= count() || row == current_ || triangles <= 0) return;
    items_[row].triangles = triangles;
    save();
}

void SgsMeshQueue::setColmap(int row, const QString &folder) {
    if (row < 0 || row >= count() || row == current_) return;
    QString clean = folder.trimmed().isEmpty() ? QString() : QDir::cleanPath(folder.trimmed());
    if (!clean.isEmpty())
        if (const auto m = findColmap(clean); m.usable()) clean = m.sparse; // the model itself, as the editor keeps it
    items_[row].colmap = clean;
    save();
    syncRow(row);
}

void SgsMeshQueue::removeRow(int row) {
    if (row < 0 || row >= count() || row == current_) return;
    items_.remove(row);
    if (current_ > row) --current_;
    save();
    syncTable();
}

void SgsMeshQueue::start() {
    if (running_) return;
    QString why;
    if (!Gs2Mesh::available(&why)) {
        QMessageBox::warning(this, windowTitle(), tr("gs2mesh is not available:\n\n%1").arg(why));
        return;
    }
    // Rows that failed or were stopped are tried again.
    for (auto &it : items_)
        if (it.state == State::Failed || it.state == State::Cancelled) it.state = State::Waiting, it.status = tr("Waiting");
    running_ = true;
    emit runningChanged(true);
    poll_->start();
    syncTable();
    next();
}

void SgsMeshQueue::stop() {
    if (!running_) return;
    running_ = false;
    poll_->stop();
    if (current_ >= 0) runner_->cancel(); // its end marks the row Cancelled
    emit runningChanged(false);
    syncTable();
}

void SgsMeshQueue::next() {
    if (!running_ || current_ >= 0 || runner_->running()) return;
    if (editorMesh_ && editorMesh_->running()) {
        summary_->setText(tr("Waiting for the mesh the editor is making."));
        return;
    }
    int row = -1;
    for (int i = 0; i < count() && row < 0; ++i)
        if (items_[i].state == State::Waiting) row = i;
    if (row < 0) {
        running_ = false;
        poll_->stop();
        syncTable();
        emit runningChanged(false);
        emit finishedAll();
        return;
    }
    Item &it = items_[row];
    auto fail = [&](const QString &why) {
        it.state = State::Failed, it.status = why;
        save();
        syncRow(row);
        emit itemFinished(row, false);
    };
    if (!QFileInfo::exists(it.ply)) return fail(tr("The PLY is missing."));
    if (openPly_ && samePath(openPly_(), it.ply)) return fail(tr("Open in the editor: open another capture, then Start again."));
    Gs2Mesh::Job job;
    job.ply = it.ply;
    job.outputDir = meshFolder(it.ply);
    job.name = QFileInfo(it.ply).completeBaseName();
    job.settings.triangles = it.triangles;
    job.settings.lods.clear(); // made by hand afterwards, from the mesh touched up
    if (!it.colmap.isEmpty()) {
        const auto m = findColmap(it.colmap);
        if (!m.usable()) return fail(tr("The photos cannot be used: %1").arg(m.problem));
        job.sparse = m.sparse, job.images = m.images;
        Gs2Mesh::usePhotoModels(job);
    }
    if (!QDir().mkpath(job.outputDir)) return fail(tr("Cannot create %1.").arg(QDir::toNativeSeparators(job.outputDir)));
    current_ = row;
    it.state = State::Running, it.status = tr("Starting gs2mesh…");
    clock_.start();
    syncTable();
    runner_->start(job); // a refusal arrives as finished(false)
}

void SgsMeshQueue::finishItem(bool ok, const QString &error, const QStringList &files) {
    const int row = current_;
    current_ = -1;
    if (row < 0 || row >= count()) return;
    Item &it = items_[row];
    const double minutes = clock_.elapsed() / 60000.0;
    if (ok) {
        QString why;
        if (writeProject(it, files, &why)) it.state = State::Done, it.status = tr("Done in %1 min: %2").arg(minutes, 0, 'f', 1).arg(QFileInfo(projectPath(it.ply)).fileName());
        else it.state = State::Failed, it.status = tr("The mesh was made, the project could not be written: %1").arg(why);
    } else if (error == QLatin1String("Cancelled.")) {
        it.state = State::Cancelled, it.status = tr("Stopped");
    } else {
        it.state = State::Failed, it.status = tr("gs2mesh: %1").arg(error);
    }
    save();
    syncTable();
    emit itemFinished(row, it.state == State::Done);
    // The next row: from the poll, once the runner has let go of this job.
}

bool SgsMeshQueue::writeProject(const Item &it, const QStringList &files, QString *error) const {
    const QString path = projectPath(it.ply);
    SgsProject p;
    bool existing = false;
    if (QFile f(path); f.open(QIODevice::ReadOnly)) {
        try {
            const auto doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) p = SgsProject::fromJson(doc.object(), QFileInfo(path).absoluteFilePath()), existing = true;
        } catch (const std::exception &) {
        }
    }
    if (!existing || !samePath(p.ply, it.ply)) {
        // As an import: two rim lights, off, around the capture.
        try {
            const SgsCapture capture = SgsCapture::load(it.ply);
            p = SgsProject{};
            p.lights = SgsLight::defaults((capture.minimum + capture.maximum) * 0.5f, (capture.maximum - capture.minimum).length());
        } catch (const std::exception &e) {
            if (error) *error = QString::fromUtf8(e.what());
            return false;
        }
    }
    p.ply = it.ply;
    p.colmap = it.colmap;
    p.colmapDeclined = it.colmap.isEmpty();
    p.mesh.triangles = it.triangles;
    p.meshFiles = files;
    p.meshEdited.clear();
    p.lodsSource.clear();
    p.meshInputs = p.currentMeshInputs();
    // Our fine normals, made when the project is opened (they are not stored).
    p.estimatedNormals = p.fineNormals = true;
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(p.json(QFileInfo(path).absoluteFilePath())).toJson()) < 0 || !out.commit()) {
        if (error) *error = out.errorString();
        return false;
    }
    return true;
}

void SgsMeshQueue::syncTable() {
    syncing_ = true;
    const int selected = selectedRow();
    table_->setRowCount(count());
    for (int r = 0; r < count(); ++r) {
        const Item &it = items_[r];
        // its name first, in sight whatever the column's width; its folder after
        auto *ply = new QTableWidgetItem(tr("%1  —  %2").arg(QFileInfo(it.ply).fileName(), QDir::toNativeSeparators(QFileInfo(it.ply).absolutePath())));
        ply->setFlags(ply->flags() & ~Qt::ItemIsEditable);
        ply->setToolTip(tr("%1\nProject: %2\nMesh: %3").arg(QDir::toNativeSeparators(it.ply), QDir::toNativeSeparators(projectPath(it.ply)), QDir::toNativeSeparators(meshFolder(it.ply))));
        table_->setItem(r, PlyColumn, ply);
        // one choice per row, kept (a replaced cell widget lingers until deleted)
        auto *tris = qobject_cast<QComboBox *>(table_->cellWidget(r, TrianglesColumn));
        if (!tris) {
            tris = new QComboBox;
            for (const auto &t : kTriangles) tris->addItem(t.second == MeshSettings{}.triangles ? tr("%1 (default)").arg(t.first) : t.first, t.second);
            connect(tris, &QComboBox::currentIndexChanged, this, [this, r, tris] {if (!syncing_) setTriangles(r, tris->currentData().toInt());});
            table_->setCellWidget(r, TrianglesColumn, tris);
        }
        int at = tris->findData(it.triangles);
        if (at < 0) tris->addItem(QString::number(it.triangles), it.triangles), at = tris->count() - 1;
        tris->setCurrentIndex(at);
        tris->setEnabled(it.state != State::Running);
        auto *colmap = new QTableWidgetItem(QDir::toNativeSeparators(it.colmap));
        colmap->setToolTip(tr("The COLMAP model gs2mesh takes the photos from (double-click to type it, Photos… to choose it); empty: from the splats alone"));
        if (it.state == State::Running) colmap->setFlags(colmap->flags() & ~Qt::ItemIsEditable);
        table_->setItem(r, ColmapColumn, colmap);
        auto *status = new QTableWidgetItem;
        status->setFlags(status->flags() & ~Qt::ItemIsEditable);
        table_->setItem(r, StatusColumn, status);
        syncRow(r);
    }
    if (selected >= 0 && selected < count()) table_->selectRow(selected);
    syncing_ = false;
    syncButtons();
}

void SgsMeshQueue::syncButtons() {
    const int r = selectedRow();
    const bool editable = r >= 0 && r != current_;
    photosButton_->setEnabled(editable);
    removeButton_->setEnabled(editable);
    openButton_->setEnabled(r >= 0 && items_[r].state == State::Done);
    startButton_->setEnabled(!running_ && count() > 0);
    stopButton_->setEnabled(running_);
    int waiting = 0, done = 0, failed = 0;
    for (const auto &it : items_) waiting += it.state == State::Waiting, done += it.state == State::Done, failed += it.state == State::Failed || it.state == State::Cancelled;
    summary_->setText(tr("%1 waiting, %2 done, %3 failed or stopped%4").arg(waiting).arg(done).arg(failed).arg(running_ ? tr(" · running") : QString()));
}

void SgsMeshQueue::syncRow(int row) {
    if (row < 0 || row >= table_->rowCount()) return;
    const bool was = syncing_;
    syncing_ = true;
    const Item &it = items_[row];
    if (auto *colmap = table_->item(row, ColmapColumn); colmap && colmap->text() != QDir::toNativeSeparators(it.colmap))
        colmap->setText(QDir::toNativeSeparators(it.colmap));
    if (auto *status = table_->item(row, StatusColumn)) {
        status->setText(it.status.isEmpty() ? tr("Waiting") : it.status);
        status->setToolTip(status->text());
        status->setForeground(it.state == State::Failed ? QColor(220, 80, 70) : it.state == State::Done ? QColor(90, 180, 110) : palette().color(QPalette::Text));
    }
    syncing_ = was;
}

void SgsMeshQueue::save() const {
    QJsonArray rows;
    for (const auto &it : items_)
        rows.append(QJsonObject{{"ply", it.ply}, {"triangles", it.triangles}, {"colmap", it.colmap}, {"state", stateName(it.state)}, {"status", it.status}});
    QSettings().setValue("MeshQueue/Items", QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact)));
}

void SgsMeshQueue::load() {
    items_.clear();
    const auto doc = QJsonDocument::fromJson(QSettings().value("MeshQueue/Items").toString().toUtf8());
    for (const auto &v : doc.array()) {
        const auto o = v.toObject();
        Item it;
        it.ply = o.value("ply").toString();
        if (it.ply.isEmpty()) continue;
        it.triangles = std::max(1000, o.value("triangles").toInt(it.triangles));
        it.colmap = o.value("colmap").toString();
        it.state = stateFrom(o.value("state").toString());
        it.status = it.state == State::Waiting ? tr("Waiting") : o.value("status").toString();
        items_.append(it);
    }
}

void SgsMeshQueue::dragEnterEvent(QDragEnterEvent *event) {
    for (const auto &url : event->mimeData()->urls())
        if (url.toLocalFile().endsWith(".ply", Qt::CaseInsensitive)) {
            event->acceptProposedAction();
            return;
        }
}

void SgsMeshQueue::dropEvent(QDropEvent *event) {
    for (const auto &url : event->mimeData()->urls()) addPly(url.toLocalFile());
}
