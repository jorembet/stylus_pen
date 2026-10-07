#include "geometry.h"

#include <algorithm>

namespace stylus {

namespace {

constexpr double kMinZoom = 0.1;
constexpr double kMaxZoom = 8.0;

}  // namespace

double Viewport::clampZoom(double z) const {
    return clampd(z, kMinZoom, kMaxZoom);
}

void BoardTransform::setViewport(double zoom, double panX, double panY) {
    viewport_.zoom = clampd(zoom, kMinZoom, kMaxZoom);
    viewport_.panX = panX;
    viewport_.panY = panY;
}

void BoardTransform::setWidgetSize(double w, double h) {
    widgetW_ = w;
    widgetH_ = h;
}

double BoardTransform::documentOriginX() const {
    return (widgetW_ - docWidth_ * viewport_.zoom) * 0.5 + viewport_.panX;
}

double BoardTransform::documentOriginY() const {
    return (widgetH_ - docHeight_ * viewport_.zoom) * 0.5 + viewport_.panY;
}

void BoardTransform::apply(cairo_t* cr) const {
    cairo_save(cr);
    cairo_translate(cr, documentOriginX(), documentOriginY());
    cairo_scale(cr, viewport_.zoom, viewport_.zoom);
}

Point BoardTransform::surfaceToWidget(double surfaceX, double surfaceY, double originX,
                                      double originY) {
    Point p;
    p.x = surfaceX - originX;
    p.y = surfaceY - originY;
    return p;
}

Point BoardTransform::toDocument(double widgetX, double widgetY) const {
    Point p;
    p.x = (widgetX - documentOriginX()) / viewport_.zoom;
    p.y = (widgetY - documentOriginY()) / viewport_.zoom;
    p.pressure = 0.5;
    return p;
}

Point BoardTransform::surfaceToDocument(double surfaceX, double surfaceY) const {
    const Point widget = surfaceToWidget(surfaceX, surfaceY, originX_, originY_);
    Point p = toDocument(widget.x, widget.y);
    p.pressure = 0.5;
    return p;
}

double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

double segmentDistance(const Point& a, const Point& b, const Point& p) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double lenSq = dx * dx + dy * dy;
    if (lenSq <= 1e-12) return std::hypot(p.x - a.x, p.y - a.y);

    double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq;
    t = clampd(t, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

Rect boundsOf(const std::vector<Point>& points) {
    if (points.empty()) return Rect{};
    double minX = points.front().x, maxX = points.front().x;
    double minY = points.front().y, maxY = points.front().y;
    for (const Point& p : points) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

Rect rectUnion(const Rect& a, const Rect& b) {
    if (rectIsEmpty(a)) return b;
    if (rectIsEmpty(b)) return a;
    const double x0 = std::min(a.x, b.x);
    const double y0 = std::min(a.y, b.y);
    const double x1 = std::max(a.right(), b.right());
    const double y1 = std::max(a.bottom(), b.bottom());
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

bool rectContains(const Rect& outer, const Point& p) {
    return p.x >= outer.x && p.x <= outer.right() && p.y >= outer.y && p.y <= outer.bottom();
}

bool rectIsEmpty(const Rect& r) {
    return !(r.w > 0.0 && r.h > 0.0);
}

Rect rectIntersect(const Rect& a, const Rect& b) {
    const double x0 = std::max(a.x, b.x);
    const double y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.right(), b.right());
    const double y1 = std::min(a.bottom(), b.bottom());
    if (x1 <= x0 || y1 <= y0) return Rect{};
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

std::vector<Point> simplify(const std::vector<Point>& points, double tol) {
    if (points.size() < 3) return points;
    std::vector<Point> out;
    out.reserve(points.size());
    out.push_back(points.front());
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        if (segmentDistance(out.back(), points[i + 1], points[i]) >= tol) out.push_back(points[i]);
    }
    out.push_back(points.back());
    return out;
}

std::vector<Point> resample(const std::vector<Point>& points, double spacing) {
    if (points.size() < 2 || spacing <= 0.0) return points;

    std::vector<Point> out;
    out.reserve(points.size() * 2);
    out.push_back(points.front());

    double carry = 0.0;
    for (size_t i = 1; i < points.size(); ++i) {
        const Point& prev = points[i - 1];
        const Point& cur = points[i];
        const double d = distance(prev, cur);
        if (d <= 1e-9) continue;

        double t = (spacing - carry) / d;
        while (t <= 1.0) {
            Point p;
            p.x = prev.x + (cur.x - prev.x) * t;
            p.y = prev.y + (cur.y - prev.y) * t;
            p.pressure = prev.pressure + (cur.pressure - prev.pressure) * t;
            // Tilt is interpolated along with the position. Taking it from the
            // far endpoint instead makes the nib jump orientation at every
            // resampled sample, which shows up as a wobbling edge on a stroke
            // whose lean changes steadily.
            p.tiltX = prev.tiltX + (cur.tiltX - prev.tiltX) * t;
            p.tiltY = prev.tiltY + (cur.tiltY - prev.tiltY) * t;
            out.push_back(p);
            t += spacing / d;
        }
        carry = std::fmod(carry + d, spacing);
    }
    out.push_back(points.back());
    return out;
}

double polylineLength(const std::vector<Point>& points) {
    double total = 0.0;
    for (size_t i = 1; i < points.size(); ++i) total += distance(points[i - 1], points[i]);
    return total;
}

Point smoothToward(const Point& previous, const Point& target, double weight) {
    const double w = clampd(weight, 0.0, 1.0);
    Point out;
    out.x = previous.x + (target.x - previous.x) * w;
    out.y = previous.y + (target.y - previous.y) * w;
    out.pressure = previous.pressure + (target.pressure - previous.pressure) * w;
    out.tiltX = target.tiltX;
    out.tiltY = target.tiltY;
    return out;
}

}  // namespace stylus