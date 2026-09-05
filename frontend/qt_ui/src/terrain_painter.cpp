#include "terrain_painter.h"
#include <QPainterPath>
#include <QtMath>

// 确定性 LCG（同 seed 同地形）
static double rnd(unsigned &s) {
    s = s * 1103515245u + 12345u;
    return (s & 0x7fffffff) / double(0x7fffffff);
}

TerrainBackground::TerrainBackground(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void TerrainBackground::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const double W = width(), H = height();
    if (W < 10 || H < 10) return;
    const int gw = qBound(20, int(W / 14), 90);
    const int gh = qBound(16, int(H / 14), 70);
    const int n = gw * gh;
    QVector<double> field(n, 0.0);

    // 1) 高斯峰
    unsigned st = unsigned(m_seed);
    const int peaks = 5;
    struct Pk { double cx, cy, rx, ry, a, rot; };
    QVector<Pk> ps;
    for (int i = 0; i < peaks; ++i) {
        Pk k;
        k.cx = rnd(st); k.cy = rnd(st);
        k.rx = 0.09 + rnd(st) * 0.20; k.ry = 0.09 + rnd(st) * 0.20;
        k.a = 0.9 + rnd(st) * 1.4;
        k.rot = rnd(st) * M_PI;
        ps.append(k);
    }
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            const double nx = x / double(gw - 1), ny = y / double(gh - 1);
            double v = 0;
            for (const auto &k : ps) {
                const double dx = nx - k.cx, dy = ny - k.cy;
                const double ex = dx * qCos(k.rot) + dy * qSin(k.rot);
                const double ey = -dx * qSin(k.rot) + dy * qCos(k.rot);
                const double s = (ex / k.rx) * (ex / k.rx) + (ey / k.ry) * (ey / k.ry);
                v += k.a * qExp(-s);
            }
            v += 0.16 * (qSin(nx * 7.1 + m_seed) + qCos(ny * 5.7 + m_seed)
                         + 0.5 * qSin((nx + ny) * 11.9));
            field[y * gw + x] = v;
        }
    }
    double mn = 1e18, mx = -1e18;
    for (double f : field) { if (f < mn) mn = f; if (f > mx) mx = f; }

    QPen pen;
    pen.setCapStyle(Qt::RoundCap);
    QPainterPath path;

    // 2) Marching Squares 等值线（10 层，计曲线每 4 层加粗）
    for (int lv = 0; lv < 10; ++lv) {
        const double th = mn + (mx - mn) * (lv + 1) / 11.0;
        const bool index = (lv % 4 == 3);
        path = QPainterPath();
        for (int y = 0; y < gh - 1; ++y) {
            for (int x = 0; x < gw - 1; ++x) {
                const int i = y * gw + x;
                const double tl = field[i], tr = field[i + 1];
                const double br = field[i + gw + 1], bl = field[i + gw];
                auto lerpV = [&](double a, double b) {
                    double t = (th - a) / (b - a);
                    if (t < 0) t = 0; else if (t > 1) t = 1;
                    return t;
                };
                struct Pt { double x, y; };
                QVector<Pt> pts;
                if ((tl > th) != (tr > th)) pts.append({x + lerpV(tl, tr), double(y)});
                if ((tr > th) != (br > th)) pts.append({double(x + 1), y + lerpV(tr, br)});
                if ((bl > th) != (br > th)) pts.append({x + lerpV(bl, br), double(y + 1)});
                if ((tl > th) != (bl > th)) pts.append({double(x), y + lerpV(tl, bl)});
                auto mov = [&](const Pt &q) {
                    path.moveTo(q.x / (gw - 1) * W, q.y / (gh - 1) * H);
                };
                auto lin = [&](const Pt &q) {
                    path.lineTo(q.x / (gw - 1) * W, q.y / (gh - 1) * H);
                };
                if (pts.size() == 2) {
                    mov(pts[0]); lin(pts[1]);
                } else if (pts.size() == 4) {
                    const double c = (tl + tr + bl + br) * 0.25;
                    if (c > th) { mov(pts[0]); lin(pts[1]); mov(pts[2]); lin(pts[3]); }
                    else { mov(pts[0]); lin(pts[3]); mov(pts[1]); lin(pts[2]); }
                }
            }
        }
        QColor c(255, 255, 255, int(255 * m_opacity * (index ? 1.0 : 0.5)));
        pen.setColor(c);
        pen.setWidthF(index ? 1.1 : 0.55);
        p.setPen(pen);
        p.drawPath(path);
    }

    // 3) 淡坐标网格
    QColor gc(255, 255, 255, int(255 * m_opacity * 0.25));
    pen.setColor(gc);
    pen.setWidthF(0.4);
    p.setPen(pen);
    QPainterPath grid;
    for (int g = 1; g < 6; ++g) { double px = W * g / 6; grid.moveTo(px, 0); grid.lineTo(px, H); }
    for (int g = 1; g < 5; ++g) { double py = H * g / 5; grid.moveTo(0, py); grid.lineTo(W, py); }
    p.drawPath(grid);
}
