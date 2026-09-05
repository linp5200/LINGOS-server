// LING OS Qt6 桌面前端 —— 主窗口（侧栏导航 + 堆叠页；先生 UI 设计：控制台/对话/预警/天气/视觉/日志/系统/设置）
#pragma once

#include "api_client.h"
#include <QMainWindow>
#include <QJsonObject>

class QListWidget;
class QStackedWidget;
class QLabel;
class QTextEdit;
class QTimer;
class TerrainBackground;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onNavChanged(int row);
    void onCmdResult(const QString &cmd, CmdResult res);
    void refreshConsole();
    void refreshHealth();
    void promptServer();

private:
    void buildUi();
    QWidget *buildConsolePage();
    QWidget *buildLogPage();
    QWidget *buildAboutPage();
    void log(const QString &line);
    QJsonObject callCmd(const QString &cmd);
    void relayout();

    ApiClient *m_api;
    QListWidget *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_hostLabel = nullptr;
    QWidget *m_top = nullptr;
    TerrainBackground *m_bg = nullptr;
    QTextEdit *m_console = nullptr;
    QTextEdit *m_logView = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QString m_host = "127.0.0.1";
    int m_port = 8080;

protected:
    void resizeEvent(QResizeEvent *) override;
};
