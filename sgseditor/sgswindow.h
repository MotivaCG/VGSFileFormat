// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "gs2meshrunner.h"
#include "meshio.h"
#include "sgscapture.h"
#include "sgsexport.h"
#include "sgsproject.h"
#include <QElapsedTimer>
#include <QMainWindow>
#include <memory>
#include <optional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QProgressBar;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTimer;
class Viewport;

// The SGS editor: one static capture (a 3DGS PLY), its normals, a mesh made from it with
// gs2mesh, lights to preview it under, and its export as .ply or .sgs.
class SgsWindow : public QMainWindow {
    Q_OBJECT
public:
    SgsWindow();
    ~SgsWindow() override;
    // Opens a PLY, or a project (.sgsproj). False, after saying why, when it cannot.
    bool openFile(const QString &path);
    bool openPly(const QString &path);
    bool openProject(const QString &path);
    bool saveProject(const QString &path);
    // The capture's normals by our method, replacing the PLY's (on this thread).
    void estimateNormals();
    void addLight(bool directional);
    void deleteLight();
    void selectLight(int index);
    // Replaces the lights with a ready-made lighting (SgsLight::presetNames).
    void applyPreset(int index);
    // Turning lights (SgsLight::turnSeconds, SgsProject::rigTurnSeconds): a clock turns them
    // while it plays. Selecting or editing a light pauses it; edits are of the light as it is
    // seen then, kept as its pose at time 0 so that playing on goes on from there.
    void setTurning(bool play);
    bool turning() const;
    void setLightTime(double seconds);
    double lightTime() const { return lightTime_; }
    // Light i as it lights now: its pose at time 0 turned by the clock.
    SgsLight shownLight(int i) const;
    // A light's or every light's turn, keeping the lights where they are seen now.
    void setLightTurn(int i, float seconds, bool clockwise);
    void setRigTurn(float seconds, bool clockwise);
    void setSelfIllumination(float amount);
    // Exports without asking where; the options are the project's.
    SgsExportResult exportSgsTo(const QString &path);
    void exportPlyTo(const QString &path);
    // Loads the mesh the project names (the main one) into the viewport; false when it has none.
    bool loadMesh();
    Viewport *viewport() const { return viewport_; }
    const SgsProject &project() const { return project_; }
    SgsProject &project() { return project_; }
    const SgsCapture *capture() const { return capture_ ? &*capture_ : nullptr; }
    Gs2Mesh *meshMaker() const { return gs2mesh_; }
    int selectedLight() const { return selected_; }
    // Starts gs2mesh with the project's settings and COLMAP, asking nothing (tests and scripts).
    bool startMesh();
signals:
    void meshFinished(bool ok, QString error);
protected:
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
private:
    void buildPanel();
    void buildMenus();
    void syncTitle();
    void syncCapture();
    void syncPhotos();
    void syncMesh();
    void syncLights();      // the list and the viewport
    void syncLightEditor(); // the selected light's fields
    void pushLights();      // the project's lights to the viewport
    void modified();
    bool maybeSave();
    void generateMesh();
    // Asks where the capture's COLMAP is, when the project does not know yet. False: cancelled.
    bool askColmap(bool force);
    bool meshDialog();
    QString meshFolder() const;
    void updateRecent(const QString &path = {});
    void exportDialog();
    bool sgsOptionsDialog();
    void showLog();
    // What is seen of light i, kept as its pose at time 0.
    void storeShown(int i, const SgsLight &shown);
    void syncTurning();
    // While gs2mesh runs, the rest of the editor waits: only the mesh's part (its progress and
    // Cancel) and looking around the view stay on.
    void setMeshBusy(bool busy);
    void setLightIconsShown(bool shown);
    // Beside the view cube: Show (splats or mesh) only when there is a mesh, its look only when it is shown.
    void syncDisplay();

    Viewport *viewport_;
    std::optional<SgsCapture> capture_;
    SgsProject project_;
    QString projectPath_;
    bool modified_ = false;
    int selected_ = -1;
    Gs2Mesh *gs2mesh_;
    QStringList meshLog_;
    std::shared_ptr<const Viewport::ViewMesh> mesh_;
    // Panel
    QLabel *captureInfo_, *normalsInfo_, *photosInfo_, *meshInfo_, *meshStage_, *selfValue_;
    QPushButton *normalsButton_, *photosButton_, *photosClear_, *meshButton_, *meshCancel_;
    QProgressBar *meshProgress_;
    // Buttons that read "Label: value" and switch on a click, as the view cube's Background.
    QPushButton *display_ = nullptr, *meshLook_ = nullptr, *lightIconsButton_ = nullptr;
    bool showMesh_ = false, meshGray_ = false;
    QWidget *displayControls_ = nullptr;
    QSlider *selfIllumination_;
    QListWidget *lightList_;
    QPushButton *deleteLight_;
    QWidget *lightEditor_;
    QLineEdit *lightName_;
    QPushButton *lightColour_;
    QDoubleSpinBox *lightIntensity_, *lightRange_, *lightX_, *lightY_, *lightZ_, *lightAzimuth_, *lightElevation_;
    QWidget *pointFields_, *directionFields_;
    QMenu *recent_;
    bool syncing_ = false;
    bool meshBusy_ = false;
    QAction *lightIconsAction_ = nullptr;
    QList<QAction *> busyActions_;
    double lightTime_ = 0, turnFrom_ = 0;
    QTimer *turnTimer_;
    QElapsedTimer turnClock_;
    QPushButton *turnPlay_ = nullptr;
    QLabel *turnTime_ = nullptr;
    QDoubleSpinBox *rigTurn_ = nullptr, *lightTurn_ = nullptr;
    QComboBox *rigDirection_ = nullptr, *lightDirection_ = nullptr;
};
