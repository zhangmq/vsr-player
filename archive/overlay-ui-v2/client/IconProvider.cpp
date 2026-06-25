#include "IconProvider.h"

#include <QPainter>
#include <QPainterPath>
#include <QCache>
#include <QHash>

#include <cmath>

namespace vsr {

// ── SVG path data ──────────────────────────────────────────────────────

QString IconProvider::pathData(IconName name) {
    switch (name) {
    case IconName::SkipBack:
        return "M 19 20 L 9 12 L 19 4 Z M 5 5 H 8 V 19 H 5 Z";
    case IconName::Play:
        return "M 8 5 L 19 12 L 8 19 Z";
    case IconName::Pause:
        return "M 6 4 H 10 V 20 H 6 Z M 14 4 H 18 V 20 H 14 Z";
    case IconName::SkipForward:
        return "M 5 4 L 15 12 L 5 20 Z M 16 5 H 19 V 19 H 16 Z";
    case IconName::Stop:
        return "M 4 4 H 20 V 20 H 4 Z";
    case IconName::Volume:
        return "M 11 5 L 6 9 H 2 V 15 H 6 L 11 19 V 5 Z "
               "M 15.54 8.46 A 5 5 0 0 1 15.54 15.54";
    case IconName::VolumeMuted:
        return "M 11 5 L 6 9 H 2 V 15 H 6 L 11 19 V 5 Z "
               "M 17.72 8.28 L 23 13.56 M 23 8.28 L 17.72 13.56";
    case IconName::Settings:
        return "M 12 8 A 4 4 0 1 0 12 16 A 4 4 0 1 0 12 8 Z "
               "M 19.4 15 A 1.65 1.65 0 0 0 19.73 16.82 L 19.79 16.88 "
               "A 2 2 0 0 1 16.79 19.76 L 16.72 19.82 "
               "A 1.65 1.65 0 0 0 14.9 19.49 V 19.4 "
               "A 1.65 1.65 0 0 0 13.39 18.39 H 10.61 "
               "A 1.65 1.65 0 0 0 9.1 19.4 V 19.49 "
               "A 1.65 1.65 0 0 0 7.28 19.82 L 7.21 19.76 "
               "A 2 2 0 0 1 4.21 16.88 L 4.27 16.82 "
               "A 1.65 1.65 0 0 0 4.6 15 V 15 "
               "A 1.65 1.65 0 0 0 3.09 14 H 3 "
               "A 2 2 0 0 1 3 10 H 3.09 "
               "A 1.65 1.65 0 0 0 4.6 9 V 9 "
               "A 1.65 1.65 0 0 0 4.27 7.18 L 4.21 7.12 "
               "A 2 2 0 0 1 7.21 4.24 L 7.28 4.18 "
               "A 1.65 1.65 0 0 0 9.1 4.51 V 4.6 "
               "A 1.65 1.65 0 0 0 10.61 5.61 H 13.39 "
               "A 1.65 1.65 0 0 0 14.9 4.6 V 4.51 "
               "A 1.65 1.65 0 0 0 16.72 4.18 L 16.79 4.24 "
               "A 2 2 0 0 1 19.79 7.12 L 19.73 7.18 "
               "A 1.65 1.65 0 0 0 19.4 9 V 9 "
               "A 1.65 1.65 0 0 0 20.91 10 H 21 "
               "A 2 2 0 0 1 21 14 H 20.91 "
               "A 1.65 1.65 0 0 0 19.4 15 Z";
    case IconName::Playlist:
        return "M 4 4.5 A 1.5 1.5 0 0 1 4 7.5 A 1.5 1.5 0 0 1 4 4.5 Z "
               "M 8 5 H 21 V 7 H 8 Z "
               "M 8 11 H 21 V 13 H 8 Z "
               "M 8 17 H 21 V 19 H 8 Z";
    case IconName::Close:
        return "M 18 6 L 6 18 M 6 6 L 18 18";
    }
    return {};
}

// ── SVG arc → cubic bezier conversion (SVG 1.1 Appendix F.6) ───────────

static void arcToBezier(QPainterPath& pp,
                         qreal x1, qreal y1,
                         qreal rx, qreal ry, qreal phi,
                         bool largeArc, bool sweep,
                         qreal x2, qreal y2) {
    if (rx == 0 || ry == 0) {
        pp.lineTo(x2, y2);
        return;
    }

    rx = std::abs(rx);
    ry = std::abs(ry);
    phi = phi * M_PI / 180.0;

    qreal dx = (x1 - x2) / 2.0;
    qreal dy = (y1 - y2) / 2.0;
    qreal x1p =  std::cos(phi) * dx + std::sin(phi) * dy;
    qreal y1p = -std::sin(phi) * dx + std::cos(phi) * dy;

    qreal lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        qreal s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }

    qreal sign = (largeArc != sweep) ? 1.0 : -1.0;
    qreal sq = (rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p) /
               (rx * rx * y1p * y1p + ry * ry * x1p * x1p);
    sq = (sq < 0) ? 0 : sq;
    qreal coef = sign * std::sqrt(sq);
    qreal cxp = coef * (rx * y1p) / ry;
    qreal cyp = coef * -(ry * x1p) / rx;

    qreal cx = std::cos(phi) * cxp - std::sin(phi) * cyp + (x1 + x2) / 2.0;
    qreal cy = std::sin(phi) * cxp + std::cos(phi) * cyp + (y1 + y2) / 2.0;

    auto angle = [](qreal ux, qreal uy, qreal vx, qreal vy) -> qreal {
        qreal dot = ux * vx + uy * vy;
        qreal len = std::sqrt(ux * ux + uy * uy) * std::sqrt(vx * vx + vy * vy);
        if (len == 0) return 0;
        qreal a = std::acos(std::clamp(dot / len, -1.0, 1.0));
        qreal cross = ux * vy - uy * vx;
        return (cross < 0) ? -a : a;
    };

    qreal ux = (x1p - cxp) / rx;
    qreal uy = (y1p - cyp) / ry;
    qreal vx = (-x1p - cxp) / rx;
    qreal vy = (-y1p - cyp) / ry;

    qreal theta1 = angle(1, 0, ux, uy);
    qreal dtheta = angle(ux, uy, vx, vy);

    if (!sweep && dtheta > 0) dtheta -= 2 * M_PI;
    else if (sweep && dtheta < 0) dtheta += 2 * M_PI;

    int segments = (int)std::ceil(std::abs(dtheta) / (M_PI / 2.0));
    if (segments < 1) segments = 1;

    qreal dt = dtheta / segments;
    qreal t = theta1;

    for (int i = 0; i < segments; i++) {
        qreal alpha = std::sin(dt) * (std::sqrt(4 + 3 * std::pow(std::tan(dt / 2), 2)) - 1) / 3;

        qreal startX = cx + rx * std::cos(t);
        qreal startY = cy + ry * std::sin(t);
        qreal endX   = cx + rx * std::cos(t + dt);
        qreal endY   = cy + ry * std::sin(t + dt);

        qreal c1x = startX - alpha * rx * std::sin(t);
        qreal c1y = startY + alpha * ry * std::cos(t);
        qreal c2x = endX   + alpha * rx * std::sin(t + dt);
        qreal c2y = endY   - alpha * ry * std::cos(t + dt);

        qreal r1x = std::cos(phi) * (c1x - cx) - std::sin(phi) * (c1y - cy) + cx;
        qreal r1y = std::sin(phi) * (c1x - cx) + std::cos(phi) * (c1y - cy) + cy;
        qreal r2x = std::cos(phi) * (c2x - cx) - std::sin(phi) * (c2y - cy) + cx;
        qreal r2y = std::sin(phi) * (c2x - cx) + std::cos(phi) * (c2y - cy) + cy;
        qreal rex = std::cos(phi) * (endX - cx) - std::sin(phi) * (endY - cy) + cx;
        qreal rey = std::sin(phi) * (endX - cx) + std::cos(phi) * (endY - cy) + cy;

        pp.cubicTo(r1x, r1y, r2x, r2y, rex, rey);
        t += dt;
    }
}

// ── Pixmap cache ────────────────────────────────────────────────────────

struct PixmapKey {
    IconName name;
    int size;
    QColor color;
    bool operator==(const PixmapKey& o) const {
        return name == o.name && size == o.size && color == o.color;
    }
};

static size_t qHash(const PixmapKey& k, size_t seed = 0) {
    return ::qHash(static_cast<int>(k.name), seed) ^
           ::qHash(k.size, seed) ^
           ::qHash(k.color.rgba(), seed);
}

static QCache<PixmapKey, QPixmap> s_cache(64);
static QHash<IconName, QPainterPath> s_pathCache;

static QPainterPath buildPath(IconName name) {
    auto it = s_pathCache.find(name);
    if (it != s_pathCache.end())
        return *it;

    QString data = IconProvider::pathData(name);
    QPainterPath pp;

    QStringList tokens;
    QString numBuf;
    for (int i = 0; i < data.size(); i++) {
        QChar ch = data[i];
        if (ch == ' ' || ch == ',' || ch == '\t' || ch == '\n') {
            if (!numBuf.isEmpty()) { tokens.append(numBuf); numBuf.clear(); }
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            if (!numBuf.isEmpty()) { tokens.append(numBuf); numBuf.clear(); }
            tokens.append(ch.toUpper());
        } else if (ch == '.' || ch == '-' || (ch >= '0' && ch <= '9')) {
            numBuf.append(ch);
        }
    }
    if (!numBuf.isEmpty()) tokens.append(numBuf);

    qreal x = 0, y = 0;
    qreal startX = 0, startY = 0;

    for (int i = 0; i < tokens.size(); ) {
        QString cmd = tokens[i++];
        if (cmd == "M") {
            qreal px = tokens[i++].toDouble();
            qreal py = tokens[i++].toDouble();
            x = px; y = py;
            startX = px; startY = py;
            pp.moveTo(px, py);
        } else if (cmd == "L") {
            qreal px = tokens[i++].toDouble();
            qreal py = tokens[i++].toDouble();
            x = px; y = py;
            pp.lineTo(px, py);
        } else if (cmd == "Z") {
            pp.closeSubpath();
            x = startX; y = startY;
        } else if (cmd == "A") {
            qreal rx       = tokens[i++].toDouble();
            qreal ry       = tokens[i++].toDouble();
            qreal phi      = tokens[i++].toDouble();
            bool  largeArc = (int)tokens[i++].toDouble() != 0;
            bool  sweep    = (int)tokens[i++].toDouble() != 0;
            qreal ex       = tokens[i++].toDouble();
            qreal ey       = tokens[i++].toDouble();
            arcToBezier(pp, x, y, rx, ry, phi, largeArc, sweep, ex, ey);
            x = ex; y = ey;
        } else if (cmd == "H") {
            qreal px = tokens[i++].toDouble();
            pp.lineTo(px, y);
            x = px;
        } else if (cmd == "V") {
            qreal py = tokens[i++].toDouble();
            pp.lineTo(x, py);
            y = py;
        } else if (cmd == "C") {
            qreal c1x = tokens[i++].toDouble();
            qreal c1y = tokens[i++].toDouble();
            qreal c2x = tokens[i++].toDouble();
            qreal c2y = tokens[i++].toDouble();
            qreal ex  = tokens[i++].toDouble();
            qreal ey  = tokens[i++].toDouble();
            pp.cubicTo(c1x, c1y, c2x, c2y, ex, ey);
            x = ex; y = ey;
        }
    }

    s_pathCache.insert(name, pp);
    return pp;
}

QPixmap IconProvider::pixmap(IconName name, int size, const QColor& color) {
    PixmapKey key{name, size, color};
    if (auto* cached = s_cache.object(key))
        return *cached;

    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath pp = buildPath(name);

    QRectF bounds = pp.boundingRect();
    if (bounds.width() > 0 && bounds.height() > 0) {
        qreal pad = size * 0.1;
        qreal sx = (size - pad * 2) / bounds.width();
        qreal sy = (size - pad * 2) / bounds.height();
        qreal s = qMin(sx, sy);
        qreal tx = pad - bounds.x() * s;
        qreal ty = pad - bounds.y() * s;
        painter.translate(tx, ty);
        painter.scale(s, s);
    }
    painter.fillPath(pp, color);
    painter.end();

    s_cache.insert(key, new QPixmap(pm));
    return pm;
}

}  // namespace vsr
