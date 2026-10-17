// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Proprietary software owned by Víctor M. Feliz.
// ScanMeNow and The4DScanner are licensed for internal use only.
// No ownership, sale, redistribution, sublicensing or modification rights
// are granted. All other rights remain reserved to the copyright holder.
// See LICENSE.md for the limited use grant and applicable terms.
// Other uses require prior written authorisation, subject to mandatory law.

#pragma once
#include "captureworker.h"
#include "project.h"
#include "filehistory.h"
#include "presetstore.h"
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QThread>
#include <QTimer>
#include <map>
#include <set>
#include <optional>
#include <memory>
#include <functional>
#include "exporttask.h"

class Viewport;
class ModifierPanel;
class AnimationPanel;
class QDoubleSpinBox;
class QSpinBox;
class RangeSlider;
class QLabel;
class QPushButton;
class QCheckBox;
class QTableWidget;
class QAction;
class QCloseEvent;
class QMenu;
class QToolButton;
class QSlider;
class QButtonGroup;
class QComboBox;
class QGroupBox;
class QFormLayout;
class LoadingOverlay;
class QProgressDialog;
class AudioPreview;
class EditorAutomation;
enum class EditorCloseResult { Ready, Waiting, Cancelled };

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr,const QString &presetDirectory = {},const QString &systemPresetDirectory = {});
    ~MainWindow() override;
    void openPath(const QString &path);
    enum class Interaction { Gui, Automation };
    bool openPath(const QString &path, Interaction interaction, bool discardUnsaved, QString *error);
    bool saveTo(const QString &path, QString *error);
    // Application operations used by automation; the transport never accesses UI internals.
    QJsonObject automationState(const QString &connectionId) const;
    QJsonObject automationCommand(const QString &method, const QJsonObject &arguments, const QString &connectionId);
    ExportTask prepareAutomationExport(const QString &path, bool overwrite);
    // Automation's other background jobs, prepared on the editor's thread: the frame shown as a
    // PLY (its project's time is that frame), the size estimate, and a saved task file.
    ExportTask prepareAutomationFramePly(const QString &path, bool overwrite);
    ExportTask prepareAutomationEstimate();
    ExportTask prepareAutomationTask(const QString &taskPath);
    // Inspection images of the capture on screen: the view as shown, a survey of nine views of
    // this frame and, optionally, top and front views across Start/End (motion), which visits
    // frames and so finishes later, returning to the frame shown. `render` -1 keeps the
    // viewport's mode, 0 points, 1 splats. `done` gets the images and their description, or
    // an error. Only one runs at a time.
    struct InspectionOptions { bool motion = false, showRemoved = false; int render = -1, motionFrames = 6; std::optional<QVector3D> target; double distance = 0; };
    struct Inspection { QImage current, survey, motion; QJsonObject meta; };
    void inspect(const InspectionOptions &options, std::function<void(const Inspection &, const QString &)> done, int waited = 0);
    // Validates an MCP request and starts the inspection; `reply` gets the answer, PNGs in base64.
    void automationInspect(const QJsonObject &arguments, std::function<void(const QJsonObject &)> reply);
    QImage composeInspection(const Inspection &inspection);
    // Export > Inspection image after its file dialog: renders, composes and saves to `path`.
    void exportInspectionTo(const QString &path);
    // Unavailable when this installation has no MCP adapter; `reason` then says why.
    void setMcpAvailable(bool available, const QString &reason = {});
    void setMcpConnectionState(bool enabled);
    void rememberMcpEnabled(bool enabled);
    void setMcpCloseHandler(std::function<EditorCloseResult()> handler) { mcpCloseHandler_=std::move(handler); }
    void smokeTest(const QString &path, const QString &output);
    void precalculateCache();
    void benchmarkPlayback(const QString &path,const QString &output);
    // Autosave: every two minutes while there are unsaved changes, a copy of the project in a
    // file of this session's own, removed once the project is saved, replaced or closed
    // without saving. A copy still there at start-up is from a session that ended unexpectedly.
    void autosaveNow();
    QString autosaveFile() const;
    void setAutosaveDirectory(const QString &directory) { autosaveDirectory_ = directory; }
    // Offers the newest copy another session left, once; recover() opens one as unsaved changes
    // to the project it came from.
    void offerRecovery();
    bool recover(const QString &autosave);
    bool mcpPreferredEnabled() const { return settings_.value("MCP/Enabled",false).toBool(); }
signals:
    void closeAborted();
    void mcpEnabledRequested(bool enabled);
    void openRequested(QString path, quint64 generation, bool sh, quint64 request);
    void decodeRequested(double time, quint64 generation, bool sh,Project project, quint64 request);
protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    bool remoteCommand_ = false;
    struct InspectionFraming { QVector3D centre; double orthographic = 0, perspective = 0; };
    struct InspectionRun {
        InspectionOptions options; Inspection result; std::function<void(const Inspection &, const QString &)> done;
        QList<int> frames; int index = 0; bool measuring = true, measured = false, splats = false, remote = false;
        double original = 0; quint64 generation = 0; QVector3D minimum, maximum; InspectionFraming framing;
        QList<QImage> tops, fronts; QJsonArray columns; QElapsedTimer waited;
    };
    std::unique_ptr<InspectionRun> inspection_;
    static InspectionFraming inspectionFraming(const QVector3D &minimum, const QVector3D &maximum);
    QImage renderSurvey(const InspectionFraming &framing, bool splats, bool showRemoved, QJsonArray *cells);
    void advanceInspection();
    QString automationError_;
    QString automationViewId(const QString &connectionId) const;
    void requireAutomationReady() const;
    void automationChanged();
    void validateAutomationProject(const QJsonObject &root, Project *result) const;
    void reportOperationError(const QString &message, bool remote);
    std::function<EditorCloseResult()> mcpCloseHandler_;
    bool closePending_ = false;
    QByteArray approvedCloseEdits_;
    QString approvedCloseProject_;
    void buildUi();
    void syncUi();
    void syncPlaybackUi();
    void dirty();
    void title();
    bool canDiscard();
    bool save(bool saveAs = false);
    void newProject();
    void setTime(double seconds, bool edited);
    // A frame from a timeline field; `tail` lets it reach the Loop morph's frames after End.
    int timelineFrame(double displayedValue, bool tail = false) const;
    void requestFrame();
    void play(bool playing);
    void receiveFrame(FramePtr frame);
    void exportImage();
    void exportInspection();
    void exportCapture();
    QByteArray thumbnailJpeg();
    void estimateExport();
    // The frame on screen, edited, as a 3D Gaussian Splatting .ply.
    void exportFrame();
    // A .vgstask beside the chosen output, and the queue that runs such tasks.
    void exportTask();
    void processTasks();
    void showError(const QString &message);
    void updateRecentMenu();
    void setTransformMode(TransformMode mode);
    void toggleTransformMode(TransformMode mode);
    void activateTransformShortcut(TransformMode mode);
    void syncTransformButtons();
    void syncTransformFields();
    void editCrop(bool editing);
    void fitCrop();
    void fitCurrentTarget();
    void applyCropPreset(float radius);
    void toggleCoordinateSpace(int group);
    void clearCrop();
    void resetTransform();
    void savePreset();
    void loadSelectedPreset();
    void applyPresetSettings(const Project &settings);
    void refreshPresets(const QString &selectedPath = {});
    void openPresetFolder();
    void editCaptureSettings();
    void syncModifiers();
    void revealModifierProperties();
    Project defaultProject() const;
    QSettings settings_;
    FileHistory history_{settings_};
    PresetStore presetStore_;
    Project project_;
    CaptureInfo info_;
    QString projectPath_;
    std::optional<Project> pendingProject_;
    QString pendingProjectPath_;
    bool loaded_ = false, loading_ = false, decoding_ = false, pendingDecode_ = false, syncing_ = false;
    quint64 serial_ = 0, generation_ = 0, openingGeneration_ = 0;
    quint64 requestSerial_ = 0, openingRequest_ = 0, decodeRequest_ = 0;
    bool cachePreparing_=false;
    quint64 cacheRequest_=0;
    QProgressDialog *cacheDialog_=nullptr;
    QAction *cacheAction_=nullptr;
    QAction *cacheEnabledAction_=nullptr;
    void launchPrecalculate();
    void showCacheInfo();
    bool openingRemote_ = false, decodeRemote_ = false, pendingDecodeRemote_ = false;
    QThread thread_;
    CaptureWorker *worker_;
    QTimer playback_, autosave_;
    QString autosaveDirectory_, recoveredProject_, recoveryFile_;
    bool recovering_ = false;
    void discardAutosave();
    QElapsedTimer clock_;
    double playStart_ = 0;
    Viewport *viewport_;
    ModifierPanel *modifierPanel_;
    AnimationPanel *animationProperties_;
    QGroupBox *isolationProperties_, *walkProperties_, *bakeProperties_, *pruneProperties_;
    QDoubleSpinBox *prunePercent_, *pruneProtect_;
    QLabel *pruneStatus_;
    // Audio: its panel, and a player per active Audio modifier, by its id, all following the
    // timeline together.
    QGroupBox *audioProperties_;
    QLabel *audioSource_;
    QPushButton *audioCaptureTrack_;
    QDoubleSpinBox *audioOffset_, *audioVolume_;
    QCheckBox *audioLoop_;
    std::map<QString,AudioPreview *> audio_;
    // Colour: exposure, white balance, saturation and despill.
    QGroupBox *colourProperties_;
    QDoubleSpinBox *colourOpacity_, *colourExposure_, *colourTemperature_, *colourTint_, *colourSaturation_, *colourDespillStrength_, *colourGreenGain_, *colourViewChroma_;
    QComboBox *colourDespill_;
    QCheckBox *colourRecoverSkin_;
    QLabel *colourNote_;
    // Erase: the tools that pick splats, and the chunk on screen (picks are per source chunk).
    QGroupBox *eraseProperties_;
    QToolButton *eraseBrush_, *eraseLasso_;
    QDoubleSpinBox *eraseBrushSize_;
    QLabel *eraseStatus_;
    QPushButton *eraseClearChunk_, *eraseClearAll_;
    QToolButton *eraseEdit_;
    // Eraser's Edit: on, the brush or lasso picks and the picks show in pink; off, the viewport
    // shows the result, the picks removed, with the usual navigation. Tab toggles it.
    bool eraseEditing_ = true;
    void setEraseEditing(bool editing);
    int screenChunk_ = -1;
    void syncEraseTool();
    void applyStroke(const std::vector<uint32_t> &records, int mode);
    // Loop morph: its length in seconds and in frames, its numbered markers, and placing them
    // with a click on the End frame or the Start frame.
    QGroupBox *morphProperties_;
    QDoubleSpinBox *morphSeconds_;
    QSpinBox *morphFrames_;
    QCheckBox *morphLeadIn_;
    QToolButton *morphPlaceEnd_, *morphPlaceStart_;
    // The morph's own transport: first and last frame, a frame either way, and play the seam.
    QWidget *morphTransport_;
    QPushButton *morphPlay_, *morphSeamPlay_;
    QTableWidget *morphMarkerTable_;
    QPushButton *morphRemoveMarker_, *morphClearMarkers_;
    QLabel *morphStatus_;
    enum class MorphSide { None, End, Start };
    MorphSide morphPlacing_ = MorphSide::None;
    // The side placing began on: a finished pair goes back to it for the next marker.
    MorphSide morphHome_ = MorphSide::Start;
    int morphSelected_ = 0; // the marker the next click places, 0 for none
    // The markers selected in the list, by number: one or several, for removing.
    std::set<int> morphChosen_;
    // The points placed this session, newest last, for Backspace.
    std::vector<std::pair<int,MorphSide>> morphHistory_;
    // Where the next marker probably goes on the side being placed, in the world.
    std::optional<QVector3D> morphSuggestion_;
    // The Loop morph's own playback, over and over: its frames alone, or the seam - a second
    // before End, the morph, and a second after Start.
    enum class MorphPlay { None, Morph, Seam };
    MorphPlay morphPlaying_ = MorphPlay::None;
    double morphPhase_ = 0, resumeMorphPhase_ = 0;
    void setMorphPlacing(MorphSide side);
    void switchMorphSide(MorphSide side, int number);
    void placeMorphMarker(const QVector3D &world);
    void removeLastMorphPoint();
    int morphFrameOf(MorphSide side) const;
    bool morphPlaced(const MorphMarker &marker, MorphSide side) const;
    // The number a click on a side places: the selected marker; else the first with its other
    // point and not this one; else the next new number.
    int morphTarget(MorphSide side) const;
    // How far each marker's End point is from where the other pairs put it (by number), and the
    // deviation above which one is flagged as a likely mis-pairing.
    std::map<int,float> morphOffsets(const Modifier &m,float *flagAbove) const;
    // Where marker `number` probably goes on `side`, from its point on the other side and the
    // other pairs, onto the nearest splat shown; none with fewer than three pairs.
    std::optional<QVector3D> morphGuess(const Modifier &m,MorphSide side,int number) const;
    // Places marker `number` (0: the next one) at a world point on the side shown.
    bool placeMorphPoint(Modifier &m,MorphSide side,int number,const QVector3D &world,int *placed);
    // Whether the worker is fitting the Loop morph now.
    bool morphFitting_ = false;
    // How playback went before a Loop morph's fit paused it, to go on once it is done.
    std::optional<MorphPlay> resumeAfterFit_;
    void showMorph(const Modifier &m);
    // The markers of the selected Loop morph on the frame shown, for the viewport.
    void syncMorphMarkers();
    // The frame the transform is evaluated at: the playhead's, End's on a Loop morph's frames.
    int poseFrame() const;
    bool showingMorph() const;
    // The source frame the viewport shows, which can lag the playhead while it decodes; -1 for
    // none or one of a Loop morph's frames. Markers are drawn on the frame they belong to.
    int shownFrame() const;
    void syncViewportPose();
    // With a Loop morph selected, Ctrl+Space, Ctrl+Home and Ctrl+End act on its frames.
    bool morphSelected() const;
    void toggleMorphPlay(MorphPlay mode);
    // What the Audio modifier plays, and where in it the timeline is now.
    void syncAudio();
    // What the pruning did to the chunk on screen, from the last decoded frame.
    std::vector<PruneStats> pruneStats_;
    void showPruneStatus();
    QDoubleSpinBox *bakeDistance_;
    QSpinBox *bakeScreenHeight_;
    QLabel *bakeSizeLabel_;
    QComboBox *cropModeCombo_, *cropPreviewCombo_;
    QToolButton *walkUnits_;
    void showWalkSpeed(const Modifier &);
    QSpinBox *isolationNeighbour_;
    QDoubleSpinBox *isolationPercent_, *walkSpeed_;
    QJsonObject processingState_;
    quint64 previewRevision_ = 0;
    QGroupBox *cropProperties_, *greenProperties_;
    QLabel *parametersHeading_;
    QDoubleSpinBox *greenSaturation_, *greenHue_;
    QCheckBox *greenLinearRgb_;
    QWidget *tools_, *timeline_, *displayControls_;
    QDoubleSpinBox *transform_[3][3], *speed_, *pointSize_;
    QDoubleSpinBox *cropRadius_, *cropRadiusZ_, *cropHeight_;
    QDoubleSpinBox *cropWidth_, *cropDepth_;
    QComboBox *cropShapeCombo_, *cropAnimationCombo_;
    QWidget *cropKeys_;
    QTableWidget *cropKeyTable_;
    QPushButton *cropRemoveKey_;
    int currentFrame() const;
    void alignTimeline();
    // While the selected crop is animated, an edit of its pose or size is a key at this frame.
    void keyCrop();
    void setCropAnimated(bool animated);
    void showCropKeys(const Modifier &m);
    // Undo and redo of edits to the project: the capture transform, the modifiers, their
    // parameters and keys, the Start/End range and the export settings. Not the view, the
    // playhead, display options or opening a capture, which starts the history afresh.
    struct UndoStep { Project project; QByteArray key; };
    QVector<UndoStep> undo_, redo_;
    Project undoBase_;   // the project as of the last edit recorded
    QByteArray undoKey_; // and what of it is undone, to tell an edit from a view change
    // One drag, or one burst of typing in a field, is one step.
    bool undoOpen_ = false, undoPressed_ = false, undoBatch_ = false;
    quint64 undoPress_ = 0, undoKeys_ = 0;
    QWidget *undoFocus_ = nullptr;
    QElapsedTimer undoClock_;
    QAction *undoAction_ = nullptr, *redoAction_ = nullptr;
    QAction *mcpAction_ = nullptr;
    static QByteArray undoKey(const Project &project);
    void recordUndo();
    void resetUndo();
    void stepUndo(bool redo);
    void updateUndoActions();
    // Over the viewport and timeline while a capture opens.
    LoadingOverlay *loadingOverlay_ = nullptr;
    QComboBox *renderStyle_ = nullptr, *splatSh_ = nullptr;
    QFormLayout *cropForm_;
    QDoubleSpinBox *frameSpin_, *inFrame_, *outFrame_;
    RangeSlider *slider_;
    QLabel *assetLabel_, *metadata_;
    QLabel *cropStatus_;
    QGroupBox *transformBox_;
    QPushButton *playButton_;
    QPushButton *resetTransformButton_, *savePresetButton_;
    QPushButton *captureSettingsButton_;
    QComboBox *presetCombo_ = nullptr;
    QGroupBox *presetBox_;
    QToolButton *presetFolderButton_;
    QCheckBox *loop_;
    QToolButton *ghostButton_, *timelineSecondsButton_;
    QSlider *ghostOpacitySlider_;
    QButtonGroup *transformModes_;
    QToolButton *modeButtons_[3];
    QToolButton *spaceButtons_[3];
    QPushButton *cropEditButton_;
    QPushButton *t4dsPresetButton_, *smnPresetButton_;
    QPushButton *cropFitButton_, *cropClearButton_;
    QAction *estimateAction_ = nullptr;
    QAction *saveAction_, *saveAsAction_, *imageAction_, *exportAction_, *gridAction_, *axesAction_, *markerAction_, *plyAction_ = nullptr, *taskAction_ = nullptr, *inspectionAction_ = nullptr;
    QMenu *recentMenu_;
    QString smokeOutput_;
    int smokeStage_ = 0;
};
