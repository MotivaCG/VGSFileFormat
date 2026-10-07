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
class QAction;
class QCloseEvent;
class QMenu;
class QToolButton;
class QButtonGroup;
class QComboBox;
class QGroupBox;
class QFormLayout;

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
    void requestFrame();
    void play(bool playing);
    void receiveFrame(FramePtr frame);
    void exportImage();
    void exportCapture();
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
    QGroupBox *isolationProperties_;
    QSpinBox *isolationNeighbour_;
    QDoubleSpinBox *isolationPercent_;
    QJsonObject processingState_;
    QGroupBox *cropProperties_, *greenProperties_;
    QLabel *parametersHeading_;
    QDoubleSpinBox *greenSaturation_, *greenHue_;
    QCheckBox *greenLinearRgb_;
    QWidget *tools_, *timeline_, *displayControls_;
    QDoubleSpinBox *transform_[3][3], *speed_, *pointSize_;
    QDoubleSpinBox *cropRadius_, *cropHeight_;
    QDoubleSpinBox *cropWidth_, *cropDepth_;
    QComboBox *cropShapeCombo_;
    QFormLayout *cropForm_;
    QSpinBox *frameSpin_, *inFrame_, *outFrame_;
    RangeSlider *slider_;
    QLabel *assetLabel_, *metadata_, *timeLabel_;
    QLabel *transformTarget_, *cropStatus_;
    QPushButton *playButton_;
    QPushButton *resetTransformButton_, *savePresetButton_;
    QPushButton *captureSettingsButton_;
    QComboBox *presetCombo_ = nullptr;
    QGroupBox *presetBox_;
    QToolButton *presetFolderButton_;
    QCheckBox *loop_, *grid_;
    QToolButton *ghostButton_;
    QButtonGroup *transformModes_;
    QToolButton *modeButtons_[3];
    QToolButton *spaceButtons_[3];
    QPushButton *cropEditButton_;
    QPushButton *t4dsPresetButton_, *smnPresetButton_;
    QPushButton *cropFitButton_, *cropClearButton_;
    QAction *saveAction_, *saveAsAction_, *imageAction_, *exportAction_;
    QMenu *recentMenu_;
    QString smokeOutput_;
    int smokeStage_ = 0;
};
