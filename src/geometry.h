#pragma once

#include <cairo/cairo.h>

#include <cmath>
#include <vector>

namespace stylus {

struct Point {
    double x = 0.0;
    double y = 0.0;
    double pressure = 0.5;
    double tiltX = 0.0;
    double tiltY = 0.0;
};

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    // Deliberately not named empty(): a perfectly horizontal line has a real
    // bounds rect with zero height, which is a usable region, not "nothing".
    // Use rectIsEmpty for the extent test and an empty point list for that.
    bool hasArea() const { return w > 0.0 && h > 0.0; }
    double right() const { return x + w; }
    double bottom() const { return y + h; }
};

// Viewport transform for the document: scale plus a translation measured in
// screen pixels from the centred position.
struct Viewport {
    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;

    double clampZoom(double z) const;
};

// Maps between widget coordinates and document coordinates.
//
// Input arrives in surface coordinates, so a widget offset has to come off
// before these are usable. Keeping that conversion here rather than inside the
// GTK layer is what makes the "stroke starts under the nib" behaviour
// testable without a display.
class BoardTransform {
public:
    BoardTransform(int docWidth, int docHeight) : docWidth_(docWidth), docHeight_(docHeight) {}

    void setViewport(double zoom, double panX, double panY);
    void setWidgetSize(double w, double h);

    double zoom() const { return viewport_.zoom; }

    // Applies the transform to a Cairo context. GTK4 hands the draw callback a
    // context whose units are already logical pixels, so no scale-factor
    // compensation belongs here; mixing the two puts the ink somewhere other
    // than where the widget was painted.
    void apply(cairo_t* cr) const;

    // Offset of the widget inside the surface its events are reported in.
    void setSurfaceOffset(double x, double y) { originX_ = x; originY_ = y; }

    // Surface coordinates of an event to widget coordinates.
    static Point surfaceToWidget(double surfaceX, double surfaceY, double originX,
                                 double originY);

    Point toDocument(double widgetX, double widgetY) const;

    // Surface coordinates of an event straight to document coordinates.
    Point surfaceToDocument(double surfaceX, double surfaceY) const;

    double documentOriginX() const;
    double documentOriginY() const;

private:
    int docWidth_;
    int docHeight_;
    Viewport viewport_;
    double widgetW_ = 0.0;
    double widgetH_ = 0.0;
    double originX_ = 0.0;
    double originY_ = 0.0;
};

double clampd(double v, double lo, double hi);

inline double distance(const Point& a, const Point& b) {
    return std::hypot(b.x - a.x, b.y - a.y);
}

double segmentDistance(const Point& a, const Point& b, const Point& p);

Rect boundsOf(const std::vector<Point>& points);

Rect rectUnion(const Rect& a, const Rect& b);

bool rectContains(const Rect& outer, const Point& p);

bool rectIsEmpty(const Rect& r);

Rect rectIntersect(const Rect& a, const Rect& b);

// Drops points closer than `tol` to the previous kept point, always keeping
// the first and last so the stroke keeps its endpoints.
std::vector<Point> simplify(const std::vector<Point>& points, double tol);

// Re-spaces a polyline so consecutive samples sit about `spacing` apart,
// interpolating pressure linearly. Keeps the original last point.
std::vector<Point> resample(const std::vector<Point>& points, double spacing);

double polylineLength(const std::vector<Point>& points);

// One-pass exponential smoothing. `weight` is how far each new sample pulls
// the running position; 1.0 disables smoothing.
Point smoothToward(const Point& previous, const Point& target, double weight);

}  // namespace stylus