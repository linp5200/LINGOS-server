// LING OS Qt6 桌面前端 —— 主窗口（先生 2026-09-12 · 0.5.1 全页版）
// 页面：控制台/对话/预警/天气/视觉/智能家居/时间线/通知/媒体/知识库/可选项/日志/设置/关于
#pragma once

#include "api_client.h"
#include <QJsonObject>
#include <QMainWindow>

class QListWidget;
class QStackedWidget;
class QLabel;
class QTextEdit;
class QTimer;
class QLineEdit;
class QPushButton;
class TerrainBackground;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onNavChanged(int row);
    void onCmdResult(const QString &cmd, CmdResult res);
    void refreshAll();
    void promptServer();

private:
    void buildUi();
    void log(const QString &line);
    QJsonObject callCmd(const QString &cmd, const QJsonObject &params = {});
    void relayout();

    // ---- 通用页面构造 ----
    QWidget *makePage(const QString &title, const QString &subtitle);
    QTextEdit *addOutput(QWidget *page, const QString &color);
    QWidget *buildConsolePage();      // 控制台（结构化）
    QWidget *buildChatPage();         // 对话
    QWidget *buildAlertPage();
    QWidget *buildWeatherPage();
    QWidget *buildVisionPage();
    QWidget *buildHomePage();         // 智能家居
    QWidget *buildTimelinePage();
    QWidget *buildNotifyPage();
    QWidget *buildMediaPage();
    QWidget *buildKbPage();
    QWidget *buildOptionsPage();      // 可选项
    QWidget *buildLogPage();
    QWidget *buildAboutPage();

    // 渲染器
    void renderKeyValues(QTextEdit *out, const QJsonObject &obj, const QString &indent = QString());
    void renderList(QTextEdit *out, const QJsonArray &arr, const QStringList &keys, int maxItems = 30);

    // 各页刷新
    void refreshConsole();
    void refreshAlert();
    void refreshWeather();
    void refreshVision();
    void refreshHome();
    void refreshTimeline();
    void refreshNotify();
    void refreshMedia();
    void refreshKb();
    void refreshOptions();

    ApiClient *m_api = nullptr;
    QListWidget *m_nav = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_hostLabel = nullptr;
    QWidget *m_top = nullptr;
    TerrainBackground *m_bg = nullptr;

    QTextEdit *m_console = nullptr;
    QTextEdit *m_chatView = nullptr;
    QTextEdit *m_alertView = nullptr;
    QTextEdit *m_weatherView = nullptr;
    QTextEdit *m_visionView = nullptr;
    QTextEdit *m_homeView = nullptr;
    QTextEdit *m_timelineView = nullptr;
    QTextEdit *m_notifyView = nullptr;
    QTextEdit *m_mediaView = nullptr;
    QTextEdit *m_kbView = nullptr;
    QTextEdit *m_optionsView = nullptr;
    QTextEdit *m_logView = nullptr;

    QLineEdit *m_input = nullptr;
    QTimer *m_refreshTimer = nullptr;

    QString m_host = QStringLiteral("127.0.0.1");
    int m_port = 8080;

protected:
    void resizeEvent(QResizeEvent *) override;
};
