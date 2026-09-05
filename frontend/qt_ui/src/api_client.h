// LING OS Qt6 桌面前端 —— 统一 API 客户端（HTTP POST /api/cmd）
// 0.4.3 设计：Qt/Web/App 同协议 {cmd,params} → {status,data,...}
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

struct CmdResult {
    bool ok = false;
    QJsonObject body;   // 完整响应 JSON（含 status/data/msg）
    QString error;
};

// 异步统一命令调用；每个请求发 cmd，完成后发 cmdFinished(result)
class ApiClient : public QObject {
    Q_OBJECT
public:
    explicit ApiClient(QObject *parent = nullptr);

    void setServer(const QString &host, int httpPort = 8080);
    bool isConfigured() const { return !m_host.isEmpty(); }

    void call(const QString &cmd, const QJsonObject &params = {});
    // 同步便捷（阻塞——供非 UI 线程/测试）
    CmdResult callSync(const QString &cmd, const QJsonObject &params = {});

signals:
    void cmdFinished(const QString &cmd, CmdResult result);
    void logMessage(const QString &line);

private:
    QString m_host;
    int m_port = 8080;
    QNetworkAccessManager *m_nam = nullptr;
};
