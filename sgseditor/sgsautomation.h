// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of SGS Editor, under the VGS Editor licence (editor/LICENSE.md).

#pragma once
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <functional>

// The private loopback bridge the SGS Editor's MCP adapter (mcp/server.py) talks to: 127.0.0.1, a random
// port and token per session, written to <app data>/mcp/<session>.json for the adapter to find. One JSON
// request per connection ({"token", "method", "arguments"}), one JSON answer ({"ok", "result" | "error"}).
// Requests run on the UI thread through `handler`, which throws std::runtime_error to refuse one.
class SgsAutomation final : public QObject {
public:
    using Handler = std::function<QJsonObject(const QString &method, const QJsonObject &arguments)>;
    SgsAutomation(QObject *parent, Handler handler);
    ~SgsAutomation() override;
    bool start(QString *error);
    void stop();
    bool enabled() const { return server_.isListening(); }
    QString sessionId() const { return sessionId_; }
    // Where the session files go: the adapter looks there.
    static QString sessionDirectory();
private:
    Handler handler_;
    QTcpServer server_;
    QString sessionId_, token_, sessionFile_;
    void accept();
};
