// LING OS Qt6 —— 灰白地形等高线背景（先生 2026-09-04 定稿语言）
// 算法：高斯峰海拔场 + Marching Squares + 线性插值（与 HTML 原型/App FuiTerrainBackground 同源）
#pragma once

#include <QColor>
#include <QPainter>
#include <QPaintEvent>
#include <QVector>
#include <QWidget>

// 透明地形背景 widget：置于内容之下（用 lower() 或叠放）；白线淡显，重绘成本低
class TerrainBackground : public QWidget {
    Q_OBJECT
public:
    explicit TerrainBackground(QWidget *parent = nullptr);
    void setOpacity(double o) { m_opacity = o; update(); }
    void setSeed(int s) { m_seed = s; update(); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    double m_opacity = 0.10;
    int m_seed = 20260904;
};
