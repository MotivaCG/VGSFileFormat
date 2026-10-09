#include "mainwindow.h"
#include "exportcapture.h"
#include "exporttask.h"
#include <QDir>
#include "displayscaling.h"
#include "editortheme.h"

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
#include <QScreen>
#include <QWindow>

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
    EditorTheme::install();
    QCommandLineParser parser; parser.addHelpOption();
    parser.addPositionalArgument("capture", "A .vgs, .pgs, .mint or .vgsproj file.");
    QCommandLineOption smoke("smoke-test", "Open, seek, verify a project and save a PNG preview.", "output");
    QCommandLineOption smokeScreen("smoke-screen", "Maximize an automated preview on the specified screen index.", "index");
    QCommandLineOption exportOption("export-capture", "Export a .vgsproj to VGS/PGS/MINT without opening the editor window.", "output");
    QCommandLineOption tasksOption("process-tasks", "Export the .vgstask files given, one after another, without opening the editor window.");
    parser.addOption(smoke);parser.addOption(smokeScreen); parser.addOption(exportOption); parser.addOption(tasksOption); parser.process(a);
    if (parser.isSet(tasksOption)) {
        const auto paths=parser.positionalArguments();
        if (paths.isEmpty()) {std::fprintf(stderr,"Give one or more .vgstask files.\n");return 2;}
        StayAwake awake;int done=0,failed=0;
        for (int i=0;i<paths.size();++i) {
            ExportTask task;QString error;
            std::fprintf(stderr,"[%d/%d] %s\n",i+1,int(paths.size()),qPrintable(QDir::toNativeSeparators(paths[i])));
            if (!readExportTask(paths[i],&task,&error)) {std::fprintf(stderr,"  failed: %s\n",qPrintable(error));++failed;continue;}
            try {
                int last=-1;const auto result=runExportTask(task,[&](int percent,const QString &message) {
                    if (percent!=last) {std::fprintf(stderr,"  %3d%% %s\n",percent,qPrintable(message));std::fflush(stderr);last=percent;}return true;
                });
                std::fprintf(stderr,"  done: %d frames to %s, %lld bytes\n",result.frames,qPrintable(QDir::toNativeSeparators(task.output)),static_cast<long long>(QFileInfo(task.output).size()));++done;
            } catch (const std::exception &e) {std::fprintf(stderr,"  failed: %s\n",e.what());++failed;}
        }
        std::fprintf(stderr,"%d done, %d failed.\n",done,failed);return failed ? 1 : 0;
    }
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
    if (parser.isSet(smoke) && parser.isSet(smokeScreen)) {
        bool valid=false;const int index=parser.value(smokeScreen).toInt(&valid);const auto screens=QGuiApplication::screens();
        if (!valid || index<0 || index>=screens.size()) {std::fprintf(stderr,"Invalid smoke screen index.\n");return 2;}
        auto *screen=screens[index];w.windowHandle()->setScreen(screen);w.move(screen->availableGeometry().topLeft()+QPoint(10,10));w.showMaximized();
        qInfo("Preview screen: %s, logical %dx%d, DPR %.2f",qPrintable(screen->name()),screen->size().width(),screen->size().height(),screen->devicePixelRatio());
    }
    const auto paths = parser.positionalArguments();
    if (parser.isSet(smoke)) {
        if (paths.size() != 1) parser.showHelp(1);
        QTimer::singleShot(0,&w,[&] { w.smokeTest(paths.first(),parser.value(smoke)); });
    } else if (!paths.isEmpty()) QTimer::singleShot(0,&w,[&] { w.openPath(paths.first()); });
    else QTimer::singleShot(0,&w,[&] { w.offerRecovery(); }); // what a session that ended unexpectedly left
    return a.exec();
}
