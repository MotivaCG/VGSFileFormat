// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include "gs2meshrunner.h"
#include "meshio.h"
#include "meshsculpt.h"
#include "sgscapture.h"
#include "sgsexport.h"
#include "sgsproject.h"
#include <QElapsedTimer>
#include <QMainWindow>
#include <map>
#include <memory>
#include <QHash>
#include <QImage>
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
class QToolBar;
class QToolButton;
class QToolBar;
class SgsAutomation;
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
    // The fine estimate on another thread, the window waiting with a progress dialog.
    void fineNormals(QString *note);
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
    bool loadMesh(int level = -1); // -1: the level shown now
    // Starts gs2mesh on the LODs of the project's main mesh, asking nothing.
    bool startLods();
    Viewport *viewport() const { return viewport_; }
    const SgsProject &project() const { return project_; }
    SgsProject &project() { return project_; }
    const SgsCapture *capture() const { return capture_ ? &*capture_ : nullptr; }
    Gs2Mesh *meshMaker() const { return gs2mesh_; }
    int selectedLight() const { return selected_; }
    // Starts gs2mesh with the project's settings and COLMAP, asking nothing (tests and scripts).
    bool startMesh();
    // The splats or the mesh (Tab).
    void setShowMesh(bool mesh);
    bool showsMesh() const { return showMesh_; }
    // Sculpting the main mesh (SculptMesh), in mesh view on the main mesh only, with T4DEditor's
    // brushes and controls: on the mesh the left button paints, the right one the opposite (Draw,
    // Inflate, Pinch), Ctrl inverts and Alt smooths; off it the left button orbits and the right
    // one pans. Navigate (Esc) leaves the mouse to the view.
    // Then T4DEditor's texture brushes, painting the albedo: the right button paints the opposite
    // (Paint and Unpaint, Lighten and Darken); Clone takes its source from an Alt+click.
    enum SculptTool { Navigate, Move, Draw, Inflate, Pinch, Smooth, Relax, Flatten, Erase, Paint, Unpaint, Clone, Blur, Lighten, Darken };
    static bool isTextureTool(int tool) { return tool >= Paint; }
    static QString sculptToolName(int tool);
    static QString sculptToolHint(int tool);
    static bool sculptToolHasOpposite(int tool);
    void setSculptTool(int tool);
    int sculptTool() const { return sculptTool_; }
    bool canSculpt() const;
    // The main mesh made sculptable (read once); false when there is none.
    bool ensureSculpt();
    SculptMesh *sculpt() const { return sculpt_.get(); }
    // A drag over the mesh as it goes (Viewport::paintStroke): phase 0 pressed, 1 moved, 2
    // released; the right button paints the opposite.
    void sculptStroke(const QPointF &position, int phase, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    // The brush (T4DEditor's): radius in cm, strength and hardness in %, along the surface or in space.
    void setBrush(double radiusCm, double strength, double hardness);
    void fixSpikes();
    void removeLoosePieces();
    void undoSculpt();
    void redoSculpt();
    // The edits not yet written: <main>_edited.obj, its material beside it. False after saying why.
    bool sculptDirty() const;
    bool saveSculpt();
    // Back to the mesh as gs2mesh made it: the edits and their files go.
    void revertSculpt(bool ask = true);
    QString editedObjPath() const;
    // One request of the MCP adapter (sgsautomation.h, mcp/server.py), on the UI thread: the
    // editor's state, opening and saving, normals, photos, mesh settings, gs2mesh's mesh and LODs
    // (started, followed, cancelled), the view and a picture of it, and the export. Throws
    // std::runtime_error with what stops it. sgswindowautomation.cpp.
    QJsonObject automation(const QString &method, const QJsonObject &arguments);
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
    void generateLods();
    QString meshFolder() const;
    void updateRecent(const QString &path = {});
    void exportDialog();
    bool sgsOptionsDialog();
    void showLog();
    // What is seen of light i, kept as its pose at time 0.
    void storeShown(int i, const SgsLight &shown);
    void syncTurning();
    QPushButton *directionButton(const QString &name);
    static bool clockwise(const QPushButton *button);
    void setClockwise(QPushButton *button, bool clockwise);
    // While gs2mesh runs, the rest of the editor waits: only the mesh's part (its progress and
    // Cancel) and looking around the view stay on.
    void setMeshBusy(bool busy);
    void setLightIconsShown(bool shown);
    // Beside the view cube: Show (splats or mesh) only when there is a mesh, its look only when it is shown.
    void syncDisplay();
    void syncMeshInfo();
    void syncSculpt();
    void sculptChanged();
    void syncBrush();

    Viewport *viewport_;
    std::optional<SgsCapture> capture_;
    SgsProject project_;
    QString projectPath_;
    bool modified_ = false;
    int selected_ = -1;
    Gs2Mesh *gs2mesh_;
    QStringList meshLog_;
    QString meshStageName_;
    std::shared_ptr<const Viewport::ViewMesh> mesh_;
    // Panel
    QLabel *captureInfo_, *normalsInfo_, *photosInfo_, *meshInfo_, *meshStage_, *selfValue_;
    QPushButton *normalsButton_, *photosButton_, *photosClear_, *meshButton_, *meshCancel_;
    QProgressBar *meshProgress_;
    // Buttons that read "Label: value" and switch on a click, as the view cube's Background.
    QPushButton *display_ = nullptr, *meshLook_ = nullptr, *meshLevelButton_ = nullptr, *lightIconsButton_ = nullptr;
    bool showMesh_ = false, meshGray_ = false;
    int meshLevel_ = 0; // the main mesh, or LOD n
    // Meshes read so far, and their albedos (the LODs share the main mesh's).
    std::map<QString, std::shared_ptr<const Viewport::ViewMesh>> meshCache_;
    QHash<QString, QImage> albedoCache_;
    bool lodsJob_ = false;
    QPushButton *lodsButton_ = nullptr;
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
    QPushButton *rigDirection_ = nullptr, *lightDirection_ = nullptr; // property "clockwise"
    // Sculpting.
    std::unique_ptr<SculptMesh> sculpt_;
    quint64 sculptSaved_ = 0;
    int sculptTool_ = Navigate;
    bool grayBeforeSculpt_ = false, strokeLive_ = false;
    SculptMesh::Brush strokeBrush_ = SculptMesh::Brush::Smooth;
    QPointF lastDab_;
    QVector3D grabOrigin_, grabNormal_;
    QWidget *sculptSection_ = nullptr;
    QList<QAction *> sculptTools_; // by SculptTool; Navigate's has no button
    QToolBar *sculptToolbar_ = nullptr;
    QToolButton *brushMenu_ = nullptr, *textureMenu_ = nullptr;
    QColor paintColour_ = QColor(220, 60, 60);
    QPushButton *paintColourButton_ = nullptr;
    bool cloneSourceSet_ = false, cloneOffsetSet_ = false;
    QVector3D cloneSource_, cloneOffset_;
    void textureDab(const QPointF &position);
    void syncPaintColour();
    QDoubleSpinBox *brushRadius_ = nullptr, *brushStrength_ = nullptr, *brushHardness_ = nullptr;
    QComboBox *brushReach_ = nullptr;
    QLabel *sculptInfo_ = nullptr;
    QPushButton *sculptUndo_ = nullptr, *sculptRedo_ = nullptr, *sculptRevert_ = nullptr;
    QAction *undoAction_ = nullptr, *redoAction_ = nullptr;
    // MCP (Edit > Enable MCP, remembered): the loopback bridge its adapter talks to.
    SgsAutomation *automation_ = nullptr;
    QAction *mcpAction_ = nullptr;
    void setMcpEnabled(bool enabled);
    QJsonObject automationState() const;
    QString meshError_;      // why the last mesh or LODs failed; empty when they did not
    bool quietMesh_ = false; // started over MCP: a failure is reported there, not in a dialog
    bool inspectingFile_ = false; // a mesh file from disk is drawn (view_mesh_file), not the project's
    QString inspectedName_;       // its file name
};
