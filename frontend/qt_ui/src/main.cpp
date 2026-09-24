// LING OS Qt6 桌面前端入口（0.4.3——先生裁决取代 GTK3 lingos_gui）
#include "main_window.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFile>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("LING OS UI"));
    /* 【2026-09-18】版本动态化（原硬编码 0.4.3——与实际 v0.6.x 不符）：
     * 读 /LINGOS/version（服务端唯一版本源），失败则 — 不假报 */
    {
        QString ver = QStringLiteral("--");
        QFile vf(QStringLiteral("/LINGOS/version"));
        if (vf.open(QIODevice::ReadOnly)) {
            ver = QString::fromUtf8(vf.readAll()).trimmed();
            vf.close();
            if (ver.isEmpty()) ver = QStringLiteral("--");
        }
        app.setApplicationVersion(ver);
    }
    // 无显示环境（CI/服务器）可 offscreen 运行：QT_QPA_PLATFORM=offscreen
    MainWindow win;
    win.show();
    return app.exec();
}
