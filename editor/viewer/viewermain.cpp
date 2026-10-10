// SPDX-License-Identifier: LicenseRef-VGS-Decoder-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of VGS Viewer, supplied under the VGS Decoder licence (decoder/LICENSE.md).

#include "viewerwindow.h"
#include "viewerversion.h"
#include "editortheme.h"
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
    app.setApplicationName("VGS Viewer"); app.setOrganizationName("THE4DSCANNER");
    app.setApplicationVersion(QStringLiteral(VIEWER_VERSION_NAME).mid(1));
    app.setWindowIcon(QIcon(":/icons/logo.png"));
    EditorTheme::install();
    ViewerWindow window; window.show();
    if (app.arguments().size() > 1) window.openFile(app.arguments().at(1));
    return app.exec();
}
