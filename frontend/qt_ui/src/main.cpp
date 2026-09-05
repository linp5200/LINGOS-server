// LING OS Qt6 桌面前端入口（0.4.3——先生裁决取代 GTK3 lingos_gui）
#include "main_window.h"

#include <QApplication>
#include <QFontDatabase>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("LING OS UI"));
    app.setApplicationVersion(QStringLiteral("0.4.3"));
    // 无显示环境（CI/服务器）可 offscreen 运行：QT_QPA_PLATFORM=offscreen
    MainWindow win;
    win.show();
    return app.exec();
}
