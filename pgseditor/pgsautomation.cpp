// SPDX-License-Identifier: LicenseRef-VGS-Editor-Proprietary
// Copyright (c) 2026 Víctor M. Feliz. All rights reserved.
//
// Part of PGS Editor, under the VGS Editor licence (editor/LICENSE.md).


#include "pgsautomation.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <stdexcept>

namespace {
// Compared without an early exit on the first differing character.
bool tokenMatches(const QString &candidate,const QString &expected) {
    const QByteArray a=candidate.toUtf8(),b=expected.toUtf8();
    if (a.size()!=b.size() || b.isEmpty()) return false;
    volatile unsigned char difference=0;
    for (qsizetype i=0;i<b.size();++i) difference=static_cast<unsigned char>(difference | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    return difference==0;
}
}

QString PgsAutomation::sessionDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/mcp";
}

PgsAutomation::PgsAutomation(QObject *parent,Handler handler) : QObject(parent),handler_(std::move(handler)) {
    sessionId_=QUuid::createUuid().toString(QUuid::WithoutBraces);
    connect(&server_,&QTcpServer::newConnection,this,&PgsAutomation::accept);
}
PgsAutomation::~PgsAutomation() {stop();}

void PgsAutomation::stop() {
    server_.close();
    if (!sessionFile_.isEmpty()) QFile::remove(sessionFile_);
    sessionFile_.clear();token_.clear();
}

bool PgsAutomation::start(QString *error) {
    if (server_.isListening()) return true;
    auto failed=[this,error](const QString &message) {if (error) *error=message;stop();return false;};
    const QString directory=sessionDirectory();
    if (!QDir().mkpath(directory)) return failed(tr("Cannot create %1.").arg(QDir::toNativeSeparators(directory)));
    if (!server_.listen(QHostAddress::LocalHost,0)) return failed(server_.errorString());
    token_=QUuid::createUuid().toString(QUuid::WithoutBraces)+QUuid::createUuid().toString(QUuid::WithoutBraces);
    sessionFile_=QDir(directory).absoluteFilePath(sessionId_+".json");
    QSaveFile file(sessionFile_);
    if (!file.open(QIODevice::WriteOnly)) return failed(file.errorString());
    file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
    const QJsonObject descriptor{{"format","pgs-editor-automation"},{"version",1},{"session_id",sessionId_},
        {"pid",double(QCoreApplication::applicationPid())},{"host","127.0.0.1"},{"port",server_.serverPort()},
        {"token",token_},{"created",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (file.write(QJsonDocument(descriptor).toJson())<0 || !file.commit()) return failed(file.errorString());
    return true;
}

void PgsAutomation::accept() {
    while (auto *socket=server_.nextPendingConnection()) {
        socket->setReadBufferSize(1024*1024+1);
        auto *timeout=new QTimer(socket);timeout->setSingleShot(true);timeout->start(15000);
        connect(timeout,&QTimer::timeout,socket,&QTcpSocket::abort);
        connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        connect(socket,&QTcpSocket::readyRead,this,[this,socket,timeout] {
            if (!enabled()) {socket->abort();return;}
            if (socket->property("handled").toBool()) return;
            if (socket->bytesAvailable()>1024*1024) {socket->abort();return;}
            if (!socket->canReadLine()) return;
            socket->setProperty("handled",true);timeout->stop();
            QJsonObject response;
            try {
                QJsonParseError parse;
                const QJsonDocument doc=QJsonDocument::fromJson(socket->readLine(),&parse);
                if (parse.error!=QJsonParseError::NoError || !doc.isObject()) throw std::runtime_error("Invalid JSON request.");
                const QJsonObject request=doc.object();
                if (!tokenMatches(request["token"].toString(),token_)) throw std::runtime_error("Unauthorised connection.");
                if (!request["method"].isString() || !request["arguments"].isObject()) throw std::runtime_error("method must be a string and arguments an object.");
                QJsonObject result=handler_(request["method"].toString(),request["arguments"].toObject());
                result["session_id"]=sessionId_;
                response={{"ok",true},{"result",result}};
            } catch (const std::exception &e) {
                response={{"ok",false},{"error",QString::fromUtf8(e.what())}};
            }
            socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n');
            socket->disconnectFromHost();
        });
    }
}
