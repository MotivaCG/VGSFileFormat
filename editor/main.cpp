#include "mainwindow.h"
#include "exportcapture.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QPalette>
#include <QIcon>
#include <QLocale>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <QFileInfo>

int main(int argc, char *argv[])
{
    QSurfaceFormat format;
    format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24); format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication::setDesktopSettingsAware(false);
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom));
    QApplication a(argc, argv);
    a.setApplicationName("VGS Editor"); a.setOrganizationName("THE4DSCANNER");
    a.setWindowIcon(QIcon(":/icons/logo.png"));
    a.setStyle("Fusion");
    QPalette palette;
    // Match Gracia4DGSConverter's Fusion palette and green section headings.
    palette.setColor(QPalette::Window,QColor(43,43,43));
    palette.setColor(QPalette::WindowText,Qt::white);
    palette.setColor(QPalette::Base,QColor(25,25,25));
    palette.setColor(QPalette::AlternateBase,QColor(43,43,43));
    palette.setColor(QPalette::Text,Qt::white);
    palette.setColor(QPalette::Button,QColor(53,53,53));
    palette.setColor(QPalette::ButtonText,Qt::white);
    palette.setColor(QPalette::ToolTipBase,QColor(53,53,53));
    palette.setColor(QPalette::ToolTipText,Qt::white);
    palette.setColor(QPalette::Link,QColor(46,109,78));
    palette.setColor(QPalette::Highlight,QColor(46,109,78));
    palette.setColor(QPalette::HighlightedText,Qt::black);
    palette.setColor(QPalette::BrightText,Qt::red);
    palette.setColor(QPalette::Disabled,QPalette::Window,QColor(33,33,33));
    palette.setColor(QPalette::Disabled,QPalette::WindowText,QColor(120,120,120));
    palette.setColor(QPalette::Disabled,QPalette::Text,QColor(120,120,120));
    palette.setColor(QPalette::Disabled,QPalette::Base,QColor(30,30,30));
    palette.setColor(QPalette::Disabled,QPalette::Button,QColor(35,35,35));
    palette.setColor(QPalette::Disabled,QPalette::ButtonText,QColor(100,100,100));
    a.setPalette(palette);
    a.setStyleSheet("QWidget { font-family: 'Segoe UI'; font-size: 10pt; }"
        "QDockWidget::title { padding: 9px; background: #303030; }"
        "QGroupBox { border: 1px solid #555; border-radius: 4px; margin-top: 16px; padding: 12px 4px 4px 4px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }"
        "QDoubleSpinBox, QSpinBox { padding: 5px; min-height: 20px; }"
        "QPushButton { padding: 7px; }"
        "QToolButton:checked { background: #2e6d4e; border: 1px solid #41b018; border-radius: 3px; }"
        "QPushButton:checked { background: #2e6d4e; border: 1px solid #41b018; border-radius: 3px; }"
        "QLabel#assetTitle { font-size: 14pt; font-weight: 600; }"
        "QLabel#sectionTitle { color: rgb(240,60,90); font-size: 12pt; font-weight: 600; }"
        "QWidget#timeline { background: #2b2b2b; border-top: 1px solid #555; }"
        "QStatusBar { color: #aaa; }");
    QCommandLineParser parser; parser.addHelpOption();
    parser.addPositionalArgument("capture", "A .vgs, .pgs, .mint or .vgsproj file.");
    QCommandLineOption smoke("smoke-test", "Open, seek, verify a project and save a PNG preview.", "output");
    QCommandLineOption exportOption("export-capture", "Export a .vgsproj to VGS/PGS/MINT without opening the editor window.", "output");
    parser.addOption(smoke); parser.addOption(exportOption); parser.process(a);
    if (parser.isSet(exportOption)) {
        const auto paths=parser.positionalArguments();Project project;QString error;
        if (paths.size()!=1 || !Project::read(paths.first(),&project,&error)) {
            std::fprintf(stderr,"%s\n",paths.size()!=1 ? "Provide one .vgsproj file." : qPrintable(error));return 2;
        }
        const QString output=parser.value(exportOption),extension=QFileInfo(output).suffix().toLower();
        if (extension!="vgs" && extension!="pgs" && extension!="mint") {std::fprintf(stderr,"Choose a .vgs, .pgs or .mint output.\n");return 2;}
        project.captureSettings.plain=extension=="pgs";
        try {
            int last=-1;QString lastMessage;const auto result=exportCaptureFile(project,output,[&](int percent,const QString &message) {
                if (percent!=last || message!=lastMessage) {std::fprintf(stderr,"%d%% %s\n",percent,qPrintable(message));std::fflush(stderr);last=percent;lastMessage=message;}return true;
            });
            std::fprintf(stderr,"Exported %d frames, %llu kept samples, %llu removed; %lld bytes.\n",result.frames,
                static_cast<unsigned long long>(result.kept),static_cast<unsigned long long>(result.removed),static_cast<long long>(QFileInfo(output).size()));
            for (const auto &note:result.notes) std::fprintf(stderr,"%s\n",qPrintable(note));return 0;
        } catch (const std::exception &e) {std::fprintf(stderr,"Export failed: %s\n",e.what());return 2;}
    }
    // Automated previews must not overwrite the user's registry history/preferences.
    std::unique_ptr<QTemporaryDir> smokeSettings;
    if (parser.isSet(smoke)) {
        smokeSettings = std::make_unique<QTemporaryDir>();
        if (!smokeSettings->isValid()) return 2;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,smokeSettings->path());
    }
    if (parser.isSet(smoke)) qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto bytes = message.toUtf8(); std::fprintf(stderr,"%s\n",bytes.constData()); std::fflush(stderr);
    });
    MainWindow w(nullptr,smokeSettings ? smokeSettings->path()+"/presets" : QString());
    w.show();
    const auto paths = parser.positionalArguments();
    if (parser.isSet(smoke)) {
        if (paths.size() != 1) parser.showHelp(1);
        QTimer::singleShot(0,&w,[&] { w.smokeTest(paths.first(),parser.value(smoke)); });
    } else if (!paths.isEmpty()) QTimer::singleShot(0,&w,[&] { w.openPath(paths.first()); });
    return a.exec();
}
