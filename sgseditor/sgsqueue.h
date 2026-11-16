// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "gs2meshrunner.h"
#include <QDialog>
#include <QElapsedTimer>
#include <QString>
#include <QVector>
#include <functional>

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

// Capture > Mesh queue: PLYs from any folders made, one after another, into a project with its
// mesh, to be opened, touched up and given LODs by hand afterwards. Each PLY gets beside it
// <name>.sgsproj (fine normals, the COLMAP, two rim lights off, as an import) and <name>_mesh/
// with the main mesh, as Generate mesh makes it (no LODs). A project already there keeps its
// lights and view and gets the new mesh. The queue runs gs2mesh on its own while the editor is
// used, waiting while the editor's own gs2mesh runs; it is kept between sessions.
class SgsMeshQueue : public QDialog {
    Q_OBJECT
public:
    enum class State { Waiting, Running, Done, Failed, Cancelled };
    struct Item {
        QString ply;
        int triangles = MeshSettings{}.triangles;
        QString colmap;   // the COLMAP folder; empty: from the splats alone
        State state = State::Waiting;
        QString status;   // what happened, for the table
    };
    // editorMesh: the editor's own gs2mesh, waited for; openPly: the PLY open in the editor
    // (its project is not written behind its back).
    SgsMeshQueue(Gs2Mesh *editorMesh, std::function<QString()> openPly, QWidget *parent = nullptr);
    ~SgsMeshQueue() override;
    // A PLY with the default triangles and the COLMAP found beside it. False: not a PLY, or listed.
    bool addPly(const QString &path);
    int count() const { return int(items_.size()); }
    const Item &item(int row) const { return items_[row]; }
    void setTriangles(int row, int triangles);
    void setColmap(int row, const QString &folder);
    void removeRow(int row);
    // Works through the waiting rows; stop() cancels the one being made and stops.
    void start();
    void stop();
    bool running() const { return running_; }
    static QString projectPath(const QString &ply);
    static QString meshFolder(const QString &ply);
signals:
    void runningChanged(bool running);
    void itemFinished(int row, bool ok);
    void finishedAll();
    // Open project, on a finished row.
    void openRequested(QString projectPath);
protected:
    void dragEnterEvent(class QDragEnterEvent *) override;
    void dropEvent(class QDropEvent *) override;
private:
    void next();
    void finishItem(bool ok, const QString &error, const QStringList &files);
    bool writeProject(const Item &item, const QStringList &files, QString *error) const;
    void syncTable();
    void syncRow(int row);
    void syncButtons(); // and the summary
    void save() const;
    void load();
    int selectedRow() const;

    Gs2Mesh *editorMesh_;
    std::function<QString()> openPly_;
    Gs2Mesh *runner_;
    QVector<Item> items_;
    int current_ = -1;
    bool running_ = false;
    QTimer *poll_;
    QElapsedTimer clock_;
    QTableWidget *table_;
    QLabel *summary_;
    QPushButton *startButton_, *stopButton_, *removeButton_, *openButton_, *photosButton_;
    bool syncing_ = false;
};
