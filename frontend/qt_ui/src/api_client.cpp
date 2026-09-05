#include "api_client.h"
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>

ApiClient::ApiClient(QObject *parent) : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

void ApiClient::setServer(const QString &host, int httpPort) {
    m_host = host;
    m_port = httpPort;
}

void ApiClient::call(const QString &cmd, const QJsonObject &params) {
    if (m_host.isEmpty()) {
        emit cmdFinished(cmd, {false, {}, QStringLiteral("server 未配置（设置→主机连接）")});
        return;
    }
    QJsonObject req;
    req["cmd"] = cmd;
    if (!params.isEmpty()) req["params"] = params;
    QNetworkRequest rq(QUrl(QStringLiteral("http://%1:%2/api/cmd").arg(m_host).arg(m_port)));
    rq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    rq.setTransferTimeout(15000);
    QNetworkReply *rep = m_nam->post(rq, QJsonDocument(req).toJson(QJsonDocument::Compact));
    connect(rep, &QNetworkReply::finished, this, [this, rep, cmd]() {
        CmdResult res;
        res.ok = (rep->error() == QNetworkReply::NoError);
        if (res.ok) {
            const QByteArray raw = rep->readAll();
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isObject()) res.body = doc.object();
            else { res.ok = false; res.error = QString::fromUtf8(raw.left(200)); }
        } else {
            res.error = rep->errorString();
        }
        rep->deleteLater();
        emit cmdFinished(cmd, res);
    });
}

CmdResult ApiClient::callSync(const QString &cmd, const QJsonObject &params) {
    CmdResult out;
    QNetworkRequest rq(QUrl(QStringLiteral("http://%1:%2/api/cmd").arg(m_host).arg(m_port)));
    rq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    rq.setTransferTimeout(15000);
    QJsonObject req;
    req["cmd"] = cmd;
    if (!params.isEmpty()) req["params"] = params;
    QNetworkReply *rep = m_nam->post(rq, QJsonDocument(req).toJson(QJsonDocument::Compact));
    // 同步等待（仅测试/后台线程）
    QEventLoop loop;
    connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    out.ok = (rep->error() == QNetworkReply::NoError);
    if (out.ok) {
        const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
        if (doc.isObject()) out.body = doc.object();
        else { out.ok = false; out.error = QStringLiteral("非 JSON 响应"); }
    } else {
        out.error = rep->errorString();
    }
    rep->deleteLater();
    return out;
}
