// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of PGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#include "editortheme.h"
#include "pgsversion.h"
#include "pgswindow.h"
#include "gs2meshrunner.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QApplication>
#include <QIcon>
#include <QLocale>
#include <QSurfaceFormat>

int main(int argc, char *argv[]) {
    QSurfaceFormat format;
    format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24); format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication::setDesktopSettingsAware(false);
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom)); // 1.00, as the editor
    QApplication app(argc, argv);
    app.setApplicationName("PGS Editor"); app.setOrganizationName("VGS");
    app.setApplicationVersion(QStringLiteral(PGS_EDITOR_VERSION_NAME).mid(1));
    app.setWindowIcon(QIcon(":/icons/logo_pgs.png")); // the PGS editor's red logo
    // --self-test <file>: whether gs2mesh loads, written to the file, and out: for a deployment
    // or an installation to be checked without opening a window.
    if (const int at=int(app.arguments().indexOf("--self-test"));at>0 && at+1<app.arguments().size()) {
        QString why,version;const bool mesh=Gs2Mesh::available(&why,&version);
        QFile out(app.arguments().at(at+1));
        if (!out.open(QIODevice::WriteOnly)) return 2;
        out.write(QJsonDocument(QJsonObject{{"pgsEditor",QStringLiteral(PGS_EDITOR_VERSION_NAME)},{"qt",QString::fromLatin1(qVersion())},
            {"gs2mesh",mesh},{"gs2meshVersion",version},{"gs2meshProblem",why}}).toJson());
        return mesh ? 0 : 1;
    }
    EditorTheme::install();
    PgsWindow window; window.showMaximized();
    if (app.arguments().size() > 1) window.openFile(app.arguments().at(1));
    return app.exec();
}
