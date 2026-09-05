#include "main_window.h"
#include "terrain_painter.h"

#include <QApplication>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
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

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), m_api(new ApiClient(this)) {
    buildUi();
    m_api->setServer(m_host, m_port);
    connect(m_api, &ApiClient::cmdFinished, this, &MainWindow::onCmdResult);
    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::refreshConsole);
    m_refreshTimer->start(10000);   // 10s 轮询（真数据）
    refreshConsole();
    refreshHealth();
}

void MainWindow::log(const QString &line) {
    if (!m_logView) return;
    m_logView->append(QStringLiteral("[%1] %2")
                          .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), line));
}

QJsonObject MainWindow::callCmd(const QString &cmd) {
    const CmdResult r = m_api->callSync(cmd, {});
    return r.body;
}

void MainWindow::onCmdResult(const QString &cmd, CmdResult res) {
    Q_UNUSED(cmd)
    if (!res.ok) {
        m_statusLabel->setText(QStringLiteral("● 离线 %1").arg(res.error.left(30)));
        m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-family:monospace").arg(kFuiAmber));
    }
}

void MainWindow::refreshHealth() {
    // GET /system/health 真值（用 callSync 经 /api/cmd 也可，此处直接 HTTP）
    const CmdResult r = m_api->callSync(QStringLiteral("system_info"), {});
    if (r.ok) {
        const QJsonObject d = r.body.value("data").toObject();
        m_statusLabel->setText(QStringLiteral("● 已连接 %1:%2").arg(m_host).arg(m_port));
        m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-family:monospace").arg(kFuiGreen));
        m_hostLabel->setText(QStringLiteral("server 0.4.3 · LN-0.4.3 · 统一 API /api/cmd"));
    } else {
        m_statusLabel->setText(QStringLiteral("● 离线（主机未连——可本地浏览）"));
        m_statusLabel->setStyleSheet(QStringLiteral("color:%1;font-family:monospace").arg(kFuiAmber));
    }
}

void MainWindow::refreshConsole() {
    if (!m_console) return;
    m_console->clear();
    m_console->append(QStringLiteral("LING OS server 0.4.3 控制台（真数据——经 /api/cmd）\n"));
    const QJsonObject r = callCmd(QStringLiteral("system_info"));
    if (r.value("status").toString() == "ok") {
        const QJsonObject d = r.value("data").toObject();
        const double cpu = d.value("cpu_usage").toDouble(-1);
        const double mem = d.value("total_ram").toDouble(-1);
        const double memFree = d.value("free_ram").toDouble(-1);
        const double disk = d.value("disk_usage").toDouble(-1);
        const int up = d.value("uptime").toInt(-1);
        const QString upStr = up >= 0 ? QStringLiteral("%1h%2m").arg(up / 3600).arg((up % 3600) / 60) : QStringLiteral("--");
        m_console->append(QStringLiteral("  host     : %1 (统一 API /api/cmd)").arg(m_host));
        m_console->append(QStringLiteral("  cpu      : %1%")
                              .arg(cpu >= 0 ? QString::number(cpu, 'f', 0) : "--"));
        m_console->append(QStringLiteral("  内存     : %1 MB 空闲 %2 MB")
                              .arg(mem >= 0 ? QString::number(mem) : "--")
                              .arg(memFree >= 0 ? QString::number(memFree) : "--"));
        m_console->append(QStringLiteral("  磁盘     : %1%  运行 %2")
                              .arg(disk >= 0 ? QString::number(disk, 'f', 0) + "%" : "--")
                              .arg(upStr));
    } else {
        m_console->append(QStringLiteral("  [ 离线 ] server 未连接——显示 --（不模拟）。设置→连接主机后自动刷新。"));
    }
    const QJsonObject a = callCmd(QStringLiteral("alert_query"));
    if (a.value("status").toString() == "ok") {
        const QJsonArray arr = a.value("data").toArray();
        m_console->append(QStringLiteral("\n预警（最近 %1 条）:").arg(arr.size()));
        int n = qMin(arr.size(), 5);
        for (int i = 0; i < n; ++i) {
            const QJsonObject e = arr.at(i).toObject();
            m_console->append(QStringLiteral("  [%1] %2 — %3")
                                  .arg(e.value("level").toString("?"),
                                       e.value("title").toString(),
                                       e.value("time").toString()));
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
    refreshConsole();
    refreshHealth();
}

QWidget *MainWindow::buildConsolePage() {
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(18, 14, 18, 14);
    auto *t = new QLabel(QStringLiteral("▍CONTROL CONSOLE  控制台"));
    t->setStyleSheet(QStringLiteral("font-size:20px;font-weight:700;letter-spacing:3px;color:%1;font-family:monospace").arg(kFuiWhite));
    lay->addWidget(t);
    m_console = new QTextEdit;
    m_console->setReadOnly(true);
    m_console->setStyleSheet(QStringLiteral("QTextEdit{background:%1;color:%2;border:1px solid rgba(255,255,255,50);font-family:monospace;font-size:12px;}").arg(kFuiBg, kFuiGreen));
    lay->addWidget(m_console);
    auto *row = new QHBoxLayout;
    auto *btn = new QPushButton(QStringLiteral("↻ 刷新"));
    btn->setStyleSheet(QStringLiteral("QPushButton{background:transparent;border:1px solid rgba(255,255,255,80);color:%1;padding:6px 14px;}").arg(kFuiGreen));
    connect(btn, &QPushButton::clicked, this, &MainWindow::refreshConsole);
    row->addWidget(btn);
    row->addStretch();
    lay->addLayout(row);
    return w;
}

QWidget *MainWindow::buildLogPage() {
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(18, 14, 18, 14);
    auto *t = new QLabel(QStringLiteral("▍SYSTEM LOG  日志"));
    t->setStyleSheet(QStringLiteral("font-size:20px;font-weight:700;letter-spacing:3px;color:%1;font-family:monospace").arg(kFuiWhite));
    lay->addWidget(t);
    m_logView = new QTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setStyleSheet(QStringLiteral("QTextEdit{background:%1;color:%2;border:1px solid rgba(255,255,255,50);font-family:monospace;font-size:12px;}").arg(kFuiBg, kFuiGray));
    lay->addWidget(m_logView);
    log(QStringLiteral("日志页——连接 server 后显示实时日志（/LINGOS/log JSON 流，未来经 WS）"));
    return w;
}

QWidget *MainWindow::buildAboutPage() {
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(18, 14, 18, 14);
    auto *t = new QLabel(QStringLiteral("▍ABOUT  关于"));
    t->setStyleSheet(QStringLiteral("font-size:20px;font-weight:700;letter-spacing:3px;color:%1;font-family:monospace").arg(kFuiWhite));
    lay->addWidget(t);
    auto *info = new QLabel(QStringLiteral(
        "LING OS Qt6 桌面前端 v0.4.3（先生 2026-09-04 设计：取代 GTK3）\n"
        "· 统一 API /api/cmd · 灰白地形背景（高斯峰+Marching Squares）\n"
        "· 中英双语 UI 后续 · 与 web_ui(8080/ui)/App 同数据源\n"
        "\n部署：mkdir build && cd build && cmake .. && make\n"
        "依赖：Qt6 Widgets/Network（目标机 qt6-base-dev）"));
    info->setStyleSheet(QStringLiteral("color:%1;font-family:monospace;font-size:12px;").arg(kFuiGray));
    info->setWordWrap(true);
    lay->addWidget(info);
    lay->addStretch();
    return w;
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("LING OS · Qt6 UI 0.4.3"));
    resize(1280, 800);
    setStyleSheet(QStringLiteral("QMainWindow{background:%1;} QWidget{background:transparent;}").arg(kFuiBg));

    // 地形背景（底层）
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

    // 侧栏导航
    m_nav = new QListWidget(this);
    m_nav->setStyleSheet(QStringLiteral(
        "QListWidget{background:%1;border:none;border-right:1px solid rgba(255,255,255,20);font-family:monospace;font-size:13px;color:%2;}"
        "QListWidget::item{padding:10px 16px;letter-spacing:2px;}"
        "QListWidget::item:selected{background:rgba(108,245,154,30);color:%3;border-left:2px solid %3;}")
            .arg(kFuiPanel, kFuiGray, kFuiGreen));
    const QStringList items = {QStringLiteral("▣ 控制台"), QStringLiteral("◈ 连接主机"),
                               QStringLiteral("▲ 预警"), QStringLiteral("☁ 天气"),
                               QStringLiteral("◉ 视觉"), QStringLiteral("≡ 日志"),
                               QStringLiteral("⚙ 设置"), QStringLiteral("⬢ 关于")};
    m_nav->addItems(items);
    connect(m_nav, &QListWidget::currentRowChanged, this, &MainWindow::onNavChanged);

    // 堆叠页
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildConsolePage());   // 0
    m_stack->addWidget(new QWidget);          // 1 连接占位（promptServer 直接弹）
    m_stack->addWidget(new QWidget);          // 2 预警（后续完善页）
    m_stack->addWidget(new QWidget);          // 3 天气占位
    m_stack->addWidget(new QWidget);          // 4 视觉占位
    m_stack->addWidget(buildLogPage());       // 5
    m_stack->addWidget(new QWidget);          // 6 设置占位
    m_stack->addWidget(buildAboutPage());     // 7

    m_nav->setCurrentRow(0);
    relayout();
}

void MainWindow::relayout() {
    const int W = width(), H = height();
    const int topH = 40, sideW = 200;
    if (m_bg) m_bg->setGeometry(0, 0, W, H);
    if (m_top) m_top->setGeometry(0, 0, W, topH);
    if (m_nav) m_nav->setGeometry(0, topH, sideW, H - topH);
    if (m_stack) m_stack->setGeometry(sideW, topH, W - sideW, H - topH);
}

void MainWindow::resizeEvent(QResizeEvent *) {
    relayout();
}

void MainWindow::onNavChanged(int row) {
    if (row < 0 || row >= m_stack->count()) return;
    if (row == 1) {            // 连接主机（对话框）
        promptServer();
        m_nav->setCurrentRow(0);
        return;
    }
    m_stack->setCurrentIndex(row);
}
