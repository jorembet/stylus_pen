#pragma once

#include <cairo/cairo.h>

#include <string>
#include <vector>

#include "geometry.h"

namespace stylus {

enum class BrushKind { Pen, Pencil, Marker, Eraser };

struct Brush {
    BrushKind kind = BrushKind::Pen;
    double size = 3.0;
    double r = 0.106;  // #1b1b1f as linear-ish sRGB fractions
    double g = 0.106;
    double b = 0.122;
    bool tiltShading = true;
    bool pressureWidth = true;
    // How strongly the nib reacts to pressure, 0..1. At 1 pressure is used as
    // read; at 0 every sample collapses onto the neutral midpoint, so the mark
    // comes out the same width all the way along. See scaledPressure.
    double pressureSensitivity = 1.0;

    void setHex(const std::string& hex);
    std::string hex() const;
};

struct Stroke {
    std::vector<Point> points;

    void clear() { points.clear(); }
    bool empty() const { return points.empty(); }
    size_t size() const { return points.size(); }
};

// Appends only if the new sample is meaningfully far from the last one, so
// hovering does not flood the buffer. `dot` forces acceptance.
bool pushPoint(Stroke& stroke, const Point& p, double minSpacing = 0.35, bool dot = false);

double pressureOf(const Point& p);

// Pressure as the brush will actually respond to it, with the sensitivity level
// applied: the reading is pulled toward the neutral 0.5 midpoint by how little
// the brush cares. Every pressure-driven term goes through this rather than
// through pressureOf directly, or the width would follow pressure while the
// pencil grain's alpha kept following the raw reading.
double scaledPressure(const Point& p, const Brush& brush);

// Stroke half-width in document pixels at a given sample.
double radiusFor(const Point& p, const Brush& brush);

struct TiltVector {
    double x = 0.0;
    double y = 0.0;
    double magnitude = 0.0;  // 0..1, normalised tilt angle
    bool valid = false;
};

TiltVector tiltVectorOf(const Point& p);

// Normalises a tilt reading to degrees. GDK reports tilt in radians on some
// backends and degrees on others, and the rest of the app reasons in degrees.
double tiltDegrees(double raw);

// Builds the closed outline of the stroke: left side forward, then right side
// in reverse. Returned as one polygon so Cairo fills it as a solid ribbon.
void buildOutline(const std::vector<Point>& points, const Brush& brush,
                  std::vector<Point>& out);

// Draws the stroke with the brush's alpha, blend mode and tilt shading applied.
void drawStroke(cairo_t* cr, const Stroke& stroke, const Brush& brush);

// Same as drawStroke but on a freshly created context with its own save/restore.
void drawStrokeOn(cairo_surface_t* surface, const Stroke& stroke, const Brush& brush);

Rect strokeBounds(const std::vector<Point>& points, const Brush& brush);

}  // namespace stylus