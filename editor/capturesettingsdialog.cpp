#include "capturesettingsdialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShortcut>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

CaptureSettingsDialog::CaptureSettingsDialog(const Project &project,PresetStore &store,QWidget *parent)
    : QDialog(parent),project_(project),store_(store) {
    setWindowTitle(tr("Capture metadata & processing")); resize(820,700);
    auto *layout = new QVBoxLayout(this); auto *presetRow = new QHBoxLayout;
    presets_ = new QComboBox; presets_->setObjectName("metadataPresetCombo"); presets_->setMinimumContentsLength(20); presetRow->addWidget(presets_,1);
    presets_->setToolTip(tr("Load a capture-metadata preset, keeping the current Title and Catalogue ID. Editor/crop presets are listed separately in the main panel."));
    auto *save = new QPushButton(tr("Save preset…")); presetRow->addWidget(save);
    save->setToolTip(tr("Save metadata, processing and output settings under a name (Ctrl+Shift+P). Editor/crop settings are not included."));
    auto *folder = new QToolButton; QPixmap icon = style()->standardIcon(QStyle::SP_DirOpenIcon).pixmap(24,24);
    QPainter tint(&icon); tint.setCompositionMode(QPainter::CompositionMode_SourceIn); tint.fillRect(icon.rect(),Qt::white); tint.end();
    QIcon folderIcon;folderIcon.addPixmap(icon,QIcon::Normal);
    QPixmap disabled=icon;QPainter grey(&disabled);grey.setCompositionMode(QPainter::CompositionMode_SourceIn);grey.fillRect(disabled.rect(),QColor("#777777"));grey.end();folderIcon.addPixmap(disabled,QIcon::Disabled);
    folder->setIcon(folderIcon); folder->setFixedSize(34,34); folder->setToolTip(tr("Open the editor presets folder (Ctrl+Alt+P).")); presetRow->addWidget(folder);
    layout->addLayout(presetRow); auto *columns = new QHBoxLayout;
    auto *metadata = new QGroupBox(tr("Metadata")); auto *form = new QFormLayout(metadata);
    const QString labels[] = {tr("Title"),tr("Catalogue ID"),tr("Author"),tr("Project"),tr("Take"),tr("Capture studio"),tr("Copyright"),tr("Software"),tr("Software version")};
    const char *ids[] = {"title","catalogueId","author","projectName","takeName","studio","copyright","softwareName","softwareVersion"};
    for (int i=0; i<9; ++i) { fields_[i] = new QLineEdit; fields_[i]->setMaxLength(1024); fields_[i]->setObjectName(QString::fromLatin1(ids[i])); form->addRow(labels[i],fields_[i]); }
    fields_[1]->setToolTip(tr("Catalogue identifier. The VGS UUID will be derived from the encoded content."));
    tags_ = new QLineEdit; tags_->setPlaceholderText(tr("Comma-separated tags")); form->addRow(tr("Tags"),tags_);
    extraJson_ = new QPlainTextEdit; extraJson_->setPlaceholderText(tr("Optional JSON metadata")); extraJson_->setMinimumHeight(110); form->addRow(tr("Extra JSON"),extraJson_);
    columns->addWidget(metadata,1);
    auto *processing = new QGroupBox(tr("Processing and output")); auto *options = new QFormLayout(processing);
    shDegree_ = new QComboBox; shDegree_->addItem(tr("Keep source"),-1); for (int i=0; i<=3; ++i) shDegree_->addItem(tr("Degree %1").arg(i),i); options->addRow(tr("SH degree"),shDegree_);
    playback_ = new QComboBox; playback_->addItem(tr("Keep source"),-1); playback_->addItem(tr("Once"),0); playback_->addItem(tr("Loop"),1); playback_->addItem(tr("Ping-pong"),2); options->addRow(tr("Playback"),playback_);
    despill_ = new QCheckBox(tr("Apply despill")); options->addRow(despill_);
    strength_ = new QDoubleSpinBox; strength_->setRange(0,1); strength_->setDecimals(4); strength_->setSingleStep(0.05); options->addRow(tr("Strength"),strength_);
    gain_ = new QDoubleSpinBox; gain_->setRange(0,2); gain_->setDecimals(3); gain_->setSingleStep(0.01); options->addRow(tr("Green gain"),gain_);
    chroma_ = new QDoubleSpinBox; chroma_->setRange(0,1); chroma_->setDecimals(4); chroma_->setSingleStep(0.05); options->addRow(tr("View chroma"),chroma_);
    recoverSkin_ = new QCheckBox(tr("Recover skin colour")); recoverSkin_->setToolTip(tr("Recover skin tones from nearby clean skin while protecting neutral clothing.")); options->addRow(recoverSkin_);
    // Despill is a Colour modifier's setting now; these stay only so a preset that carries it
    // still reads, and are not shown.
    for (QWidget *field : std::initializer_list<QWidget *>{despill_,strength_,gain_,chroma_,recoverSkin_}) options->setRowVisible(field,false);
    auto *note = new QLabel(tr("Processing options are saved for export. Despill is in the Color modifier.")); note->setWordWrap(true); options->addRow(note);
    columns->addWidget(processing,1); layout->addLayout(columns,1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply")); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setToolTip(tr("Apply metadata and processing options (Ctrl+Enter)."));
    buttons->button(QDialogButtonBox::Cancel)->setToolTip(tr("Discard changes in this panel (Esc)."));
    auto *applyKey = new QShortcut(QKeySequence("Ctrl+Return"),this); connect(applyKey,&QShortcut::activated,buttons->button(QDialogButtonBox::Ok),&QPushButton::click);
    connect(buttons,&QDialogButtonBox::accepted,this,&CaptureSettingsDialog::accept); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(despill_,&QCheckBox::toggled,this,[this](bool enabled) { for (auto *spin : {strength_,gain_,chroma_}) spin->setEnabled(enabled); recoverSkin_->setEnabled(enabled); });
    connect(save,&QPushButton::clicked,this,&CaptureSettingsDialog::savePreset);
    auto *saveKey = new QShortcut(QKeySequence("Ctrl+Shift+P"),this); connect(saveKey,&QShortcut::activated,save,&QPushButton::click);
    auto *folderKey = new QShortcut(QKeySequence("Ctrl+Alt+P"),this); connect(folderKey,&QShortcut::activated,folder,&QToolButton::click);
    connect(folder,&QToolButton::clicked,this,[this] { QString error; if (!store_.ensureDirectory(&error) || !QDesktopServices::openUrl(QUrl::fromLocalFile(store_.directory()))) QMessageBox::warning(this,tr("Presets folder"),error.isEmpty() ? tr("Could not open the presets folder.") : error); });
    connect(presets_,&QComboBox::activated,this,[this](int) {
        if (presets_->currentData().toString().isEmpty()) return;
        EditorPreset preset; QString error; if (!store_.read(presets_->currentData().toString(),PresetScope::Metadata,&preset,&error)) { QMessageBox::warning(this,tr("Preset"),error); return; }
        auto options = preset.settings.captureSettings;
        options.title = fields_[0]->text(); options.catalogueId = fields_[1]->text();
        setSettings(options);
    });
    setSettings(project.captureSettings); refreshPresets();
}
CaptureSettings CaptureSettingsDialog::settings() const {
    CaptureSettings s; QString *values[] = {&s.title,&s.catalogueId,&s.author,&s.projectName,&s.takeName,&s.studio,&s.copyright,&s.softwareName,&s.softwareVersion};
    for (int i=0; i<9; ++i) *values[i] = fields_[i]->text().trimmed();
    for (const auto &tag : tags_->text().split(',',Qt::SkipEmptyParts)) if (!tag.trimmed().isEmpty()) s.tags.append(tag.trimmed());
    s.extraJson = extraJson_->toPlainText(); s.shDegree = shDegree_->currentData().toInt(); s.playbackMode = playback_->currentData().toInt();
    s.despill = despill_->isChecked(); s.despillStrength = strength_->value(); s.greenGain = gain_->value(); s.viewChromaScale = chroma_->value(); s.recoverSkin = recoverSkin_->isChecked();
    s.despill = false; // a Colour modifier's setting now
    return s;
}
void CaptureSettingsDialog::setSettings(const CaptureSettings &s) {
    const QString values[] = {s.title,s.catalogueId,s.author,s.projectName,s.takeName,s.studio,s.copyright,s.softwareName,s.softwareVersion};
    for (int i=0; i<9; ++i) fields_[i]->setText(values[i]); tags_->setText(s.tags.join(", ")); extraJson_->setPlainText(s.extraJson);
    shDegree_->setCurrentIndex(shDegree_->findData(s.shDegree)); playback_->setCurrentIndex(playback_->findData(s.playbackMode));
    despill_->setChecked(s.despill); strength_->setValue(s.despillStrength); gain_->setValue(s.greenGain); chroma_->setValue(s.viewChromaScale); recoverSkin_->setChecked(s.recoverSkin);
    for (auto *spin : {strength_,gain_,chroma_}) spin->setEnabled(s.despill); recoverSkin_->setEnabled(s.despill);
}
void CaptureSettingsDialog::accept() { QString error; if (!settings().validate(&error)) { QMessageBox::warning(this,tr("Capture settings"),error); return; } QDialog::accept(); }
void CaptureSettingsDialog::refreshPresets(const QString &path) {
    QSignalBlocker blocker(presets_); presets_->clear(); presets_->addItem(tr("Load preset…"),QString());
    for (const auto &entry : store_.list(PresetScope::Metadata)) presets_->addItem(entry.name,entry.path);
    const int index = presets_->findData(path); if (index>=0) presets_->setCurrentIndex(index);
    presets_->setEnabled(presets_->count()>1);
}
void CaptureSettingsDialog::savePreset() {
    const auto options = settings(); QString error; if (!options.validate(&error)) { QMessageBox::warning(this,tr("Preset"),error); return; }
    bool accepted; const QString name = QInputDialog::getText(this,tr("Save metadata preset"),tr("Metadata preset name:"),QLineEdit::Normal,presets_->currentIndex()>0 ? presets_->currentText() : QString(),&accepted).trimmed();
    if (!accepted) return;
    for (const auto &entry : store_.list(PresetScope::Metadata)) if (entry.name.compare(name,Qt::CaseInsensitive)==0 && QMessageBox::question(this,tr("Replace preset"),tr("Replace metadata preset '%1'?").arg(entry.name))!=QMessageBox::Yes) return;
    Project snapshot = project_; snapshot.captureSettings = options; QString path;
    if (!store_.save(name,snapshot,&path,&error,PresetScope::Metadata)) { QMessageBox::warning(this,tr("Preset"),error); return; } refreshPresets(path);
}
