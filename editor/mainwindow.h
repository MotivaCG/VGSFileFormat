#pragma once
#include "captureworker.h"
#include "project.h"
#include "filehistory.h"
#include "presetstore.h"
#include <QElapsedTimer>
#include <QMainWindow>
#include <QThread>
#include <QTimer>
#include <optional>

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

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr,const QString &presetDirectory = {});
    ~MainWindow() override;
    void openPath(const QString &path);
    void smokeTest(const QString &path, const QString &output);
signals:
    void openRequested(QString path, quint64 generation, bool sh);
    void decodeRequested(double time, quint64 generation, bool sh,Project project);
protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    void buildUi();
    void syncUi();
    void dirty();
    void title();
    bool canDiscard();
    bool save(bool saveAs = false);
    void newProject();
    void setTime(double seconds, bool edited);
    int timelineFrame(double displayedValue) const;
    void requestFrame();
    void play(bool playing);
    void receiveFrame(FramePtr frame);
    void exportImage();
    void exportCapture();
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
    QThread thread_;
    CaptureWorker *worker_;
    QTimer playback_;
    QElapsedTimer clock_;
    double playStart_ = 0;
    Viewport *viewport_;
    ModifierPanel *modifierPanel_;
    AnimationPanel *animationProperties_;
    QGroupBox *isolationProperties_, *walkProperties_, *bakeProperties_, *pruneProperties_;
    QDoubleSpinBox *prunePercent_, *pruneProtect_;
    QLabel *pruneStatus_;
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
    QAction *saveAction_, *saveAsAction_, *imageAction_, *exportAction_, *gridAction_, *plyAction_ = nullptr, *taskAction_ = nullptr;
    QMenu *recentMenu_;
    QString smokeOutput_;
    int smokeStage_ = 0;
};
