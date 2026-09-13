// LING OS Qt6 桌面前端 —— 主窗口实现（先生 2026-09-12 · 0.5.1）
// 全页版：控制台/对话/预警/天气/视觉/智能家居/时间线/通知/媒体/知识库/可选项/日志/设置/关于
// 原则：无数据一律显示 --（不模拟）；命令统一走 /api/cmd
#include "main_window.h"
#include "terrain_painter.h"

#include <QApplication>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QColor>
#include <QFont>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

static const char *kFuiBg = "#1E2126";
static const char *kFuiPanel = "#2B3038";
static const char *kFuiGreen = "#6CF59A";
static const char *kFuiAmber = "#FFBE4D";
static const char *kFuiRed = "#FF4D4D";
static const char *kFuiGray = "#A9B1BC";
static const char *kFuiWhite = "#F4F6F8";

// ============================================================
// 构造 / 基础
// ============================================================
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), m_api(new ApiClient(this)) {
    buildUi();
    m_api->setServer(m_host, m_port);
    connect(m_api, &ApiClient::cmdFinished, this, &MainWindow::onCmdResult);
    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::refreshAll);
    m_refreshTimer->start(10000);   // 10s 轮询（真数据）
    refreshAll();
}

void MainWindow::log(const QString &line) {
    if (!m_logView) return;
    m_logView->append(QStringLiteral("[%1] %2")
                          .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), line));
}

QJsonObject MainWindow::callCmd(const QString &cmd, const QJsonObject &params) {
    const CmdResult r = m_api->callSync(cmd, params);
    return r.body;
}

void MainWindow::onCmdResult(const QString &cmd, CmdResult res) {
    Q_UNUSED(cmd)
    if (!res.ok && m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("● 离线 %1").arg(res.error.left(30)));
        m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-family:monospace").arg(kFuiAmber));
    }
}

void MainWindow::refreshAll() {
    refreshConsole();
    refreshAlert();
    refreshWeather();
    refreshVision();
    refreshHome();
    refreshTimeline();
    refreshNotify();
    refreshMedia();
    refreshKb();
    refreshOptions();
    if (m_statusLabel) {
        const QJsonObject r = callCmd(QStringLiteral("system_info"));
        if (r.value("status").toString() == QStringLiteral("ok")) {
            m_statusLabel->setText(QStringLiteral("● 已连接 %1:%2").arg(m_host).arg(m_port));
            m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-family:monospace").arg(kFuiGreen));
            if (m_hostLabel)
                m_hostLabel->setText(QStringLiteral("server · 统一 API /api/cmd · 真数据"));
        } else {
            m_statusLabel->setText(QStringLiteral("● 离线（主机未连——可本地浏览）"));
            m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-family:monospace").arg(kFuiAmber));
        }
    }
}

void MainWindow::promptServer() {
    bool ok = false;
    const QString h = QInputDialog::getText(this, QStringLiteral("连接主机"),
                                            QStringLiteral("server 地址 (host:port)"),
                                            QLineEdit::Normal,
                                            QStringLiteral("%1:%2").arg(m_host).arg(m_port), &ok);
    if (!ok || h.isEmpty()) return;
    const QStringList hp = h.split(':');
    m_host = hp.value(0);
    if (hp.size() > 1) m_port = hp.value(1).toInt();
    m_api->setServer(m_host, m_port);
    refreshAll();
}

// ============================================================
// 通用页面构造
// ============================================================
QWidget *MainWindow::makePage(const QString &title, const QString &subtitle) {
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(18, 14, 18, 14);
    auto *t = new QLabel(title);
    t->setStyleSheet(QStringLiteral("font-size:20px;font-weight:700;letter-spacing:3px;color:%1;font-family:monospace")
                         .arg(kFuiWhite));
    lay->addWidget(t);
    if (!subtitle.isEmpty()) {
        auto *s = new QLabel(subtitle);
        s->setStyleSheet(QStringLiteral("font-size:10px;color:%1;font-family:monospace").arg(kFuiGray));
        lay->addWidget(s);
    }
    return w;
}

QTextEdit *MainWindow::addOutput(QWidget *page, const QString &color) {
    auto *out = new QTextEdit(page);
    out->setReadOnly(true);
    out->setStyleSheet(QStringLiteral(
                           "QTextEdit{background:%1;color:%2;border:1px solid rgba(255,255,255,50);"
                           "font-family:monospace;font-size:12px;}")
                           .arg(kFuiBg, color));
    if (auto *lay = qobject_cast<QVBoxLayout *>(page->layout())) lay->addWidget(out);
    return out;
}

// JSON 渲染：键值对
void MainWindow::renderKeyValues(QTextEdit *out, const QJsonObject &obj, const QString &indent) {
    if (!out) return;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QJsonValue v = it.value();
        QString val;
        if (v.isString()) val = v.toString();
        else if (v.isBool()) val = v.toBool() ? QStringLiteral("是") : QStringLiteral("否");
        else if (v.isDouble()) val = QString::number(v.toDouble(), 'g', 6);
        else if (v.isNull()) val = QStringLiteral("--");
        else if (v.isArray()) val = QStringLiteral("[%1 项]").arg(v.toArray().size());
        else if (v.isObject()) val = QStringLiteral("{...}");
        out->append(QStringLiteral("%1  %2: %3").arg(indent, it.key(), val));
    }
}

// JSON 渲染：列表
void MainWindow::renderList(QTextEdit *out, const QJsonArray &arr, const QStringList &keys, int maxItems) {
    if (!out) return;
    if (arr.isEmpty()) {
        out->append(QStringLiteral("  （无数据 —— 显示 -- 不模拟）"));
        return;
    }
    const int n = qMin(arr.size(), maxItems);
    for (int i = 0; i < n; ++i) {
        const QJsonObject e = arr.at(i).toObject();
        QStringList parts;
        for (const QString &k : keys) {
            const QJsonValue v = e.value(k);
            if (v.isUndefined()) continue;
            QString s = v.isString() ? v.toString()
                                     : (v.isDouble() ? QString::number(v.toDouble(), 'g', 6)
                                                     : (v.isBool() ? (v.toBool() ? QStringLiteral("开")
                                                                                 : QStringLiteral("关"))
                                                                   : QString()));
            if (!s.isEmpty()) parts << QStringLiteral("%1=%2").arg(k, s);
        }
        if (e.isEmpty()) parts << QStringLiteral("{}");
        out->append(QStringLiteral("  [%1] %2").arg(i + 1).arg(parts.join(QStringLiteral(" · "))));
    }
    if (arr.size() > n)
        out->append(QStringLiteral("  …（共 %1 项，已显示 %2）").arg(arr.size()).arg(n));
}

// ============================================================
// 各页
// ============================================================
QWidget *MainWindow::buildConsolePage() {
    QWidget *w = makePage(QStringLiteral("▍CONTROL CONSOLE  控制台"),
                          QStringLiteral("真数据 · 经 /api/cmd · 无数据一律显示 --"));
    m_console = addOutput(w, kFuiGreen);
    auto *row = new QHBoxLayout;
    auto *btn = new QPushButton(QStringLiteral("↻ 刷新"));
    btn->setStyleSheet(QStringLiteral("QPushButton{background:transparent;border:1px solid rgba(255,255,255,80);color:%1;padding:6px 14px;}").arg(kFuiGreen));
    connect(btn, &QPushButton::clicked, this, &MainWindow::refreshAll);
    row->addWidget(btn);
    row->addStretch();
    if (auto *lay = qobject_cast<QVBoxLayout *>(w->layout())) lay->addLayout(row);
    return w;
}

QWidget *MainWindow::buildChatPage() {
    QWidget *w = makePage(QStringLiteral("▍CHAT  AI 对话"),
                          QStringLiteral("经 WS 2939（Qt 端为只读视图；完整对话请用 App/Web）"));
    m_chatView = addOutput(w, kFuiWhite);
    m_chatView->setPlainText(QStringLiteral("Qt 端对话为只读展示。\n发送请使用 App 或 Web UI（/ui）。\n无数据 —— 显示 -- 不模拟。"));
    return w;
}

QWidget *MainWindow::buildAlertPage() {
    QWidget *w = makePage(QStringLiteral("▍ALERT  预警"),
                          QStringLiteral("CAP 分级（Minor/Moderate/Severe/Extreme）· alert_query"));
    m_alertView = addOutput(w, kFuiAmber);
    return w;
}

QWidget *MainWindow::buildWeatherPage() {
    QWidget *w = makePage(QStringLiteral("▍WEATHER  天气"),
                          QStringLiteral("Open-Meteo / wttr.in / 自定义源 · 无数据不模拟"));
    m_weatherView = addOutput(w, kFuiWhite);
    return w;
}

QWidget *MainWindow::buildVisionPage() {
    QWidget *w = makePage(QStringLiteral("▍VISION  视觉/监控"),
                          QStringLiteral("monitor_status / monitor_list · 预览 :8891"));
    m_visionView = addOutput(w, kFuiWhite);
    return w;
}

QWidget *MainWindow::buildHomePage() {
    QWidget *w = makePage(QStringLiteral("▍SMART HOME  智能家居"),
                          QStringLiteral("场景/区域/围栏/能源/自动化/设备发现 · home_overview"));
    m_homeView = addOutput(w, kFuiGreen);
    return w;
}

QWidget *MainWindow::buildTimelinePage() {
    QWidget *w = makePage(QStringLiteral("▍NVR TIMELINE  监控时间线"),
                          QStringLiteral("时间线/存储/录像保留 · nvr_overview"));
    m_timelineView = addOutput(w, kFuiWhite);
    return w;
}

QWidget *MainWindow::buildNotifyPage() {
    QWidget *w = makePage(QStringLiteral("▍NOTIFICATIONS  通知中心"),
                          QStringLiteral("统一收件箱 · 免打扰 · notify_list"));
    m_notifyView = addOutput(w, kFuiWhite);
    return w;
}

QWidget *MainWindow::buildMediaPage() {
    QWidget *w = makePage(QStringLiteral("▍MEDIA  媒体控制"),
                          QStringLiteral("播放器状态与音量 · media_list"));
    m_mediaView = addOutput(w, kFuiWhite);
    return w;
}

QWidget *MainWindow::buildKbPage() {
    QWidget *w = makePage(QStringLiteral("▍KNOWLEDGE BASE  知识库"),
                          QStringLiteral("文档与分块 · kb_list（上传/检索请用 App/Web）"));
    m_kbView = addOutput(w, kFuiWhite);
    return w;
}

QWidget *MainWindow::buildOptionsPage() {
    QWidget *w = makePage(QStringLiteral("▍OPTIONS  可选项"),
                          QStringLiteral("安全底线不可关 · 危险开关需确认 · options_list"));
    m_optionsView = addOutput(w, kFuiGreen);
    return w;
}

QWidget *MainWindow::buildLogPage() {
    QWidget *w = makePage(QStringLiteral("▍SYSTEM LOG  日志"),
                          QStringLiteral("运行日志（logdump / file_read）"));
    m_logView = addOutput(w, kFuiGray);
    return w;
}

QWidget *MainWindow::buildAboutPage() {
    QWidget *w = makePage(QStringLiteral("▍ABOUT  关于"),
                          QStringLiteral("LING OS · 本地优先 · 隐私第一"));
    QTextEdit *out = addOutput(w, kFuiWhite);
    out->setPlainText(QStringLiteral(
        "LING OS Qt6 桌面前端\n"
        "  版本    : 0.5.1（LN-0.5.1）\n"
        "  服务端  : 版本号经 system_info 实时获取（未连接显示 --）\n"
        "  数据根  : /LINGOS\n"
        "  仓库    : github.com/linp5200/LINGOS-server\n"
        "  界面    : FUI v2 · 灰白地形\n\n"
        "  安全    : 加密传输 · 权限矩阵 · 审计日志 · 隐私保护模式\n"
        "  说明    : 所有页面数据均来自主机实时接口，无数据一律显示 --（不模拟）\n"));
    return w;
}

// ============================================================
// 各页刷新
// ============================================================
void MainWindow::refreshConsole() {
    if (!m_console) return;
    m_console->clear();
    m_console->append(QStringLiteral("LING OS 控制台（真数据 —— 经 /api/cmd）\n"));
    const QJsonObject r = callCmd(QStringLiteral("system_info"));
    if (r.value("status").toString() == QStringLiteral("ok")) {
        const QJsonObject d = r.value("data").toObject();
        const double cpu = d.value("cpu_usage").toDouble(-1);
        const double mem = d.value("total_ram").toDouble(-1);
        const double memFree = d.value("free_ram").toDouble(-1);
        const double disk = d.value("disk_usage").toDouble(-1);
        const int up = d.value("uptime").toInt(-1);
        const QString upStr = up >= 0
            ? QStringLiteral("%1h%2m").arg(up / 3600).arg((up % 3600) / 60)
            : QStringLiteral("--");
        m_console->append(QStringLiteral("  host     : %1 (统一 API /api/cmd)").arg(m_host));
        m_console->append(QStringLiteral("  cpu      : %1")
                              .arg(cpu >= 0 ? QString::number(cpu, 'f', 0) + "%" : QStringLiteral("--")));
        m_console->append(QStringLiteral("  内存     : %1 MB 空闲 %2 MB")
                              .arg(mem >= 0 ? QString::number(mem) : QStringLiteral("--"),
                                   memFree >= 0 ? QString::number(memFree) : QStringLiteral("--")));
        m_console->append(QStringLiteral("  磁盘     : %1  运行 %2")
                              .arg(disk >= 0 ? QString::number(disk, 'f', 0) + "%" : QStringLiteral("--"),
                                   upStr));
        m_console->append(QStringLiteral("  版本     : %1")
                              .arg(d.value("version").toString(d.value("internal_version").toString(QStringLiteral("--")))));
    } else {
        m_console->append(QStringLiteral("  [ 离线 ] server 未连接 —— 显示 --（不模拟）。\n  点「连接主机」后自动刷新。"));
    }
}

void MainWindow::refreshAlert() {
    if (!m_alertView) return;
    m_alertView->clear();
    const QJsonObject r = callCmd(QStringLiteral("alert_query"));
    if (r.value("status").toString() != QStringLiteral("ok")) {
        m_alertView->setPlainText(QStringLiteral("预警读取失败/主机未连 —— 显示 -- 不模拟"));
        return;
    }
    QJsonArray arr = r.value("data").toArray();
    if (arr.isEmpty()) arr = r.value("events").toArray();
    if (arr.isEmpty()) {
        m_alertView->append(QStringLiteral("[ 暂无预警 —— 系统正常 ]"));
        return;
    }
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject e = arr.at(i).toObject();
        m_alertView->append(QStringLiteral("[%1] %2   %3")
                                .arg(e.value("level").toString(QStringLiteral("?")).toUpper(),
                                     e.value("title").toString(),
                                     e.value("time").toString()));
        const QString c = e.value("content").toString();
        if (!c.isEmpty()) m_alertView->append(QStringLiteral("    %1").arg(c));
    }
}

void MainWindow::refreshWeather() {
    if (!m_weatherView) return;
    m_weatherView->clear();
    const QJsonObject r = callCmd(QStringLiteral("weather_current"));
    if (r.value("status").toString() != QStringLiteral("ok")) {
        m_weatherView->setPlainText(QStringLiteral("天气源不可达 —— 显示 -- 不模拟"));
        return;
    }
    const QJsonObject d = r.value("data").toObject();
    /* 【0.5.2 修复】字段回退读取（服务端别名 feels_like/wind_speed；city 在顶层） */
    auto pick = [](const QJsonObject &o, const QStringList &keys) -> QJsonValue {
        for (const QString &k : keys) {
            const QJsonValue v = o.value(k);
            if (!v.isUndefined() && !v.isNull()) return v;
        }
        return QJsonValue();
    };
    auto txt = [&pick](const QJsonObject &o, const QStringList &keys) -> QString {
        const QJsonValue v = pick(o, keys);
        if (v.isUndefined() || v.isNull()) return QStringLiteral("--");
        return v.toVariant().toString();
    };
    QString city = txt(r, {QStringLiteral("city")});
    if (city == QStringLiteral("--")) city = txt(d, {QStringLiteral("city")});
    m_weatherView->append(QStringLiteral("城市    : %1").arg(city));
    m_weatherView->append(QStringLiteral("温度    : %1 °C").arg(txt(d, {QStringLiteral("temp"), QStringLiteral("temperature")})));
    m_weatherView->append(QStringLiteral("体感    : %1 °C").arg(txt(d, {QStringLiteral("feels_like"), QStringLiteral("feels"), QStringLiteral("apparent_temperature")})));
    m_weatherView->append(QStringLiteral("湿度    : %1 %").arg(txt(d, {QStringLiteral("humidity")})));
    m_weatherView->append(QStringLiteral("风速    : %1 km/h").arg(txt(d, {QStringLiteral("wind_speed"), QStringLiteral("wind"), QStringLiteral("wind_speed_10m")})));
    m_weatherView->append(QStringLiteral("风向    : %1 °").arg(txt(d, {QStringLiteral("wind_direction"), QStringLiteral("wind_dir")})));
    m_weatherView->append(QStringLiteral("气压    : %1 hPa").arg(txt(d, {QStringLiteral("pressure"), QStringLiteral("surface_pressure")})));
    m_weatherView->append(QStringLiteral("UV      : %1").arg(txt(d, {QStringLiteral("uv"), QStringLiteral("uv_index")})));
    m_weatherView->append(QStringLiteral("\n—— 7 日预报 ——"));
    const QJsonObject f = callCmd(QStringLiteral("weather_forecast"));
    if (f.value("status").toString() == QStringLiteral("ok")) {
        /* 【0.5.2】daily 在 data 内（原代码读顶层 —— 结构不符导致空列表） */
        QJsonArray daily = f.value("data").toObject().value("daily").toArray();
        if (daily.isEmpty()) daily = f.value("daily").toArray();  /* 兼容旧结构 */
        renderList(m_weatherView, daily,
                   {QStringLiteral("date"), QStringLiteral("temp_min"), QStringLiteral("temp_max"),
                    QStringLiteral("code")});
    } else {
        m_weatherView->append(QStringLiteral("  （预报不可达 —— 显示 --）"));
    }
}

void MainWindow::refreshVision() {
    if (!m_visionView) return;
    m_visionView->clear();
    const QJsonObject r = callCmd(QStringLiteral("monitor_status"));
    if (r.value("status").toString() != QStringLiteral("ok")) {
        m_visionView->setPlainText(QStringLiteral("监控未连接 —— 显示 -- 不模拟"));
        return;
    }
    renderKeyValues(m_visionView, r.value("data").toObject(), QStringLiteral("  "));
    m_visionView->append(QStringLiteral("\n—— 摄像头 ——"));
    renderList(m_visionView, callCmd(QStringLiteral("monitor_list")).value("data").toArray(),
               {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("status"),
                QStringLiteral("source")});
}

void MainWindow::refreshHome() {
    if (!m_homeView) return;
    m_homeView->clear();
    const QJsonObject r = callCmd(QStringLiteral("home_overview"));
    if (r.value("status").toString() != QStringLiteral("ok")) {
        m_homeView->setPlainText(QStringLiteral("智能家居不可达 —— 显示 -- 不模拟"));
        return;
    }
    renderKeyValues(m_homeView, r.value("data").toObject(), QStringLiteral("  "));
    m_homeView->append(QStringLiteral("\n—— 场景 ——"));
    renderList(m_homeView, callCmd(QStringLiteral("scene_list")).value("data").toArray(),
               {QStringLiteral("name"), QStringLiteral("id")}, 20);
    m_homeView->append(QStringLiteral("\n—— 自动化 ——"));
    renderList(m_homeView, callCmd(QStringLiteral("automation_list")).value("data").toArray(),
               {QStringLiteral("name"), QStringLiteral("trigger"), QStringLiteral("enabled")}, 20);
}

void MainWindow::refreshTimeline() {
    if (!m_timelineView) return;
    m_timelineView->clear();
    const QJsonObject r = callCmd(QStringLiteral("nvr_overview"));
    if (r.value("status").toString() != QStringLiteral("ok")) {
        m_timelineView->setPlainText(QStringLiteral("监控/NVR 不可达 —— 显示 -- 不模拟"));
        return;
    }
    renderKeyValues(m_timelineView, r.value("data").toObject(), QStringLiteral("  "));
    m_timelineView->append(QStringLiteral("\n—— 最近事件 ——"));
    QJsonObject p;
    p.insert(QStringLiteral("hours"), 24);
    p.insert(QStringLiteral("limit"), 50);
    const QJsonObject tq = callCmd(QStringLiteral("timeline_query"), p);
    renderList(m_timelineView, tq.value("data").toObject().value("items").toArray(),
               {QStringLiteral("kind"), QStringLiteral("camera"), QStringLiteral("ts")}, 30);
}

void MainWindow::refreshNotify() {
    if (!m_notifyView) return;
    m_notifyView->clear();
    QJsonObject p;
    p.insert(QStringLiteral("limit"), 50);
    const QJsonObject r = callCmd(QStringLiteral("notify_list"), p);
    if (r.value("status").toString() != QStringLiteral("ok")) {
        m_notifyView->setPlainText(QStringLiteral("通知服务不可达 —— 显示 -- 不模拟"));
        return;
    }
    const QJsonObject d = r.value("data").toObject();
    m_notifyView->append(QStringLiteral("  共 %1 · 未读 %2 · 免打扰 %3\n")
                             .arg(d.value("total").toInt())
                             .arg(d.value("unread").toInt())
                             .arg(d.value("dnd").toBool() ? QStringLiteral("开") : QStringLiteral("关")));
    renderList(m_notifyView, d.value("items").toArray(),
               {QStringLiteral("level"), QStringLiteral("title"), QStringLiteral("source")}, 40);
}

void MainWindow::refreshMedia() {
    if (!m_mediaView) return;
    m_mediaView->clear();
    const QJsonObject r = callCmd(QStringLiteral("media_list"));
    if (r.value("status").toString() != QStringLiteral("ok")) {
        m_mediaView->setPlainText(QStringLiteral("媒体服务不可达 —— 显示 -- 不模拟"));
        return;
    }
    const QJsonArray arr = r.value("data").toArray();
    if (arr.isEmpty()) {
        m_mediaView->append(QStringLiteral("  （无媒体设备 —— 显示 -- 不模拟）"));
        return;
    }
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject e = arr.at(i).toObject();
        m_mediaView->append(QStringLiteral("  %1  [%2]  音量 %3%")
                                .arg(e.value("entity_id").toString(),
                                     e.value("state").toString(),
                                     QString::number(qRound(e.value("volume").toDouble() * 100))));
        const QString t = e.value("media_title").toString();
        if (!t.isEmpty()) m_mediaView->append(QStringLiteral("      ♪ %1").arg(t));
    }
}

void MainWindow::refreshKb() {
    if (!m_kbView) return;
    m_kbView->clear();
    const QJsonObject r = callCmd(QStringLiteral("kb_list"));
    if (r.value("status").toString() != QStringLiteral("ok")) {
        m_kbView->setPlainText(QStringLiteral("知识库不可达 —— 显示 -- 不模拟"));
        return;
    }
    const QJsonObject d = r.value("data").toObject();
    m_kbView->append(QStringLiteral("  文档 %1 篇 · 分块 %2\n")
                         .arg(d.value("docs").toArray().size())
                         .arg(d.value("total_chunks").toInt()));
    renderList(m_kbView, d.value("docs").toArray(),
               {QStringLiteral("name"), QStringLiteral("chunks"), QStringLiteral("chars")}, 40);
}

void MainWindow::refreshOptions() {
    if (!m_optionsView) return;
    m_optionsView->clear();
    const QJsonObject r = callCmd(QStringLiteral("options_list"));
    if (r.value("status").toString() != QStringLiteral("ok")) {
        m_optionsView->setPlainText(QStringLiteral("可选项不可达 —— 显示 -- 不模拟"));
        return;
    }
    QJsonObject d = r.value("data").toObject();
    if (d.isEmpty()) d = r;
    m_optionsView->append(QStringLiteral("  隐私保护模式: %1\n")
                              .arg(d.value("privacy_mode").toBool() ? QStringLiteral("已启用")
                                                                    : QStringLiteral("未启用")));
    const QJsonArray opts = d.value("options").toArray();
    QString lastGroup;
    static const QStringList groupNames = {QStringLiteral("安全"), QStringLiteral("隐私"),
                                           QStringLiteral("功能"), QStringLiteral("界面"),
                                           QStringLiteral("语音"), QStringLiteral("AI"),
                                           QStringLiteral("数据"), QStringLiteral("连接")};
    for (int i = 0; i < opts.size(); ++i) {
        const QJsonObject o = opts.at(i).toObject();
        const int g = o.value("group").toInt();
        const QString gn = (g >= 0 && g < groupNames.size()) ? groupNames.at(g) : QStringLiteral("其他");
        if (gn != lastGroup) {
            m_optionsView->append(QStringLiteral("\n▍%1").arg(gn));
            lastGroup = gn;
        }
        const int kind = o.value("kind").toInt();
        const QString tag = (kind == 0) ? QStringLiteral("🔒")
                                        : (o.value("value").toBool() ? QStringLiteral("开")
                                                                     : QStringLiteral("关"));
        const QString warn = o.value("dangerous").toBool() ? QStringLiteral(" ⚠") : QString();
        m_optionsView->append(QStringLiteral("   %1  %2%3")
                                  .arg(tag, o.value("name_zh").toString(), warn));
    }
    m_optionsView->append(QStringLiteral("\n  （修改请用 App / Web —— Qt 端为只读视图）"));
}

// ============================================================
// UI 构建
// ============================================================
void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("LING OS · Qt6 UI 0.5.1"));
    resize(1280, 800);
    setStyleSheet(QStringLiteral("QMainWindow{background:%1;} QWidget{background:transparent;}").arg(kFuiBg));

    m_bg = new TerrainBackground(this);
    m_bg->setOpacity(0.09);

    // 顶栏
    m_top = new QWidget(this);
    auto *topLay = new QHBoxLayout(m_top);
    topLay->setContentsMargins(16, 0, 16, 0);
    auto *logo = new QLabel(QStringLiteral("LING OS"));
    logo->setStyleSheet(QStringLiteral("font-size:18px;font-weight:700;letter-spacing:4px;color:%1;").arg(kFuiWhite));
    m_hostLabel = new QLabel;
    m_hostLabel->setStyleSheet(QStringLiteral("color:%1;font-family:monospace;font-size:10px;").arg(kFuiGray));
    m_statusLabel = new QLabel;
    topLay->addWidget(logo);
    topLay->addSpacing(14);
    topLay->addWidget(m_hostLabel);
    topLay->addStretch();
    topLay->addWidget(m_statusLabel);

    // 侧栏
    m_nav = new QListWidget(this);
    m_nav->setStyleSheet(QStringLiteral(
        "QListWidget{background:%1;border:none;border-right:1px solid rgba(255,255,255,20);"
        "font-family:monospace;font-size:13px;color:%2;}"
        "QListWidget::item{padding:8px 14px;letter-spacing:2px;}"
        "QListWidget::item:selected{background:rgba(108,245,154,30);color:%3;border-left:2px solid %3;}")
            .arg(kFuiPanel, kFuiGray, kFuiGreen));

    // 导航分组（用不可选项做小节标题）
    auto addGroup = [this](const QString &name) {
        auto *it = new QListWidgetItem(name);
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(QColor(120, 130, 140));
        QFont f = it->font();
        f.setPointSize(9);
        it->setFont(f);
        m_nav->addItem(it);
    };
    auto addPageItem = [this](const QString &label) { m_nav->addItem(label); };

    addGroup(QStringLiteral("总览"));
    addPageItem(QStringLiteral("▣ 控制台"));
    addGroup(QStringLiteral("对话与 AI"));
    addPageItem(QStringLiteral("◈ AI 对话"));
    addPageItem(QStringLiteral("▣ 知识库"));
    addGroup(QStringLiteral("监控"));
    addPageItem(QStringLiteral("▲ 预警"));
    addPageItem(QStringLiteral("☁ 天气"));
    addPageItem(QStringLiteral("◉ 视觉/监控"));
    addPageItem(QStringLiteral("▬ 时间线"));
    addGroup(QStringLiteral("智能家居"));
    addPageItem(QStringLiteral("⌂ 智能家居"));
    addGroup(QStringLiteral("组织"));
    addPageItem(QStringLiteral("◔ 通知中心"));
    addPageItem(QStringLiteral("♪ 媒体"));
    addPageItem(QStringLiteral("⚑ 可选项"));
    addGroup(QStringLiteral("系统"));
    addPageItem(QStringLiteral("≡ 日志"));
    addPageItem(QStringLiteral("⬢ 关于"));
    addGroup(QStringLiteral("连接"));
    addPageItem(QStringLiteral("⇄ 连接主机"));

    connect(m_nav, &QListWidget::currentRowChanged, this, &MainWindow::onNavChanged);

    // 堆叠页（顺序与导航条目一致）
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildConsolePage());     // 1  控制台
    m_stack->addWidget(buildChatPage());        // 2  对话
    m_stack->addWidget(buildKbPage());          // 3  知识库
    m_stack->addWidget(buildAlertPage());       // 4  预警
    m_stack->addWidget(buildWeatherPage());     // 5  天气
    m_stack->addWidget(buildVisionPage());      // 6  视觉
    m_stack->addWidget(buildTimelinePage());    // 7  时间线
    m_stack->addWidget(buildHomePage());        // 8  智能家居
    m_stack->addWidget(buildNotifyPage());      // 9  通知中心
    m_stack->addWidget(buildMediaPage());       // 10 媒体
    m_stack->addWidget(buildOptionsPage());     // 11 可选项
    m_stack->addWidget(buildLogPage());         // 12 日志
    m_stack->addWidget(buildAboutPage());       // 13 关于
    m_stack->addWidget(new QWidget);            // 14 连接（占位）

    m_nav->setCurrentRow(1);
    relayout();
}

void MainWindow::relayout() {
    const int W = width(), H = height();
    const int topH = 40, sideW = 210;
    if (m_bg) m_bg->setGeometry(0, 0, W, H);
    if (m_top) m_top->setGeometry(0, 0, W, topH);
    if (m_nav) m_nav->setGeometry(0, topH, sideW, H - topH);
    if (m_stack) m_stack->setGeometry(sideW, topH, W - sideW, H - topH);
}

void MainWindow::resizeEvent(QResizeEvent *) {
    relayout();
}

void MainWindow::onNavChanged(int row) {
    if (!m_stack || row < 0) return;
    // 导航含分组标题（不可选），需映射到页索引
    int pageIndex = -1;
    int current = 0;
    for (int i = 0; i < m_nav->count(); ++i) {
        if (m_nav->item(i)->flags() == Qt::NoItemFlags) continue;   // 分组标题
        if (i == row) { pageIndex = current; break; }
        ++current;
    }
    if (pageIndex < 0 || pageIndex >= m_stack->count()) return;

    // 「连接主机」= 最后一页 → 弹对话框
    if (pageIndex == m_stack->count() - 1) {
        promptServer();
        m_nav->setCurrentRow(1);
        return;
    }
    m_stack->setCurrentIndex(pageIndex);
}
