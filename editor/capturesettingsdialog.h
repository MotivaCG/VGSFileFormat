#pragma once
#include "presetstore.h"
#include <QDialog>

class QLineEdit;
class QComboBox;
class QPlainTextEdit;
class QCheckBox;
class QDoubleSpinBox;
class CaptureSettingsDialog : public QDialog {
    Q_OBJECT
public:
    CaptureSettingsDialog(const Project &project,PresetStore &store,QWidget *parent = nullptr);
    CaptureSettings settings() const;
    void setSettings(const CaptureSettings &settings);
protected:
    void accept() override;
private:
    void refreshPresets(const QString &selected = {});
    void savePreset();
    Project project_;
    PresetStore &store_;
    QLineEdit *fields_[9], *tags_;
    QPlainTextEdit *extraJson_;
    QComboBox *presets_, *shDegree_, *playback_;
    QCheckBox *despill_, *recoverSkin_;
    QDoubleSpinBox *strength_, *gain_, *chroma_;
};
