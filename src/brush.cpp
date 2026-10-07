#include "brush.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "color.h"

namespace stylus {

namespace {

constexpr double kTiltFullScale = 60.0;  // degrees mapped to full magnitude
constexpr double kTiltWidening = 1.1;    // nib stretch at full tilt
constexpr double kMarkerAlpha = 0.22;
constexpr double kPencilAlpha = 0.85;

// Unit normal of the path tangent at index i, averaged across the neighbouring
// segments so the outline does not jitter at direction changes.
Point normalAt(const std::vector<Point>& points, size_t i) {
    const size_t first = i == 0 ? 0 : i - 1;
    const size_t last = i + 1 < points.size() ? i + 1 : i;

    double dx = 0.0;
    double dy = 0.0;
    if (i > 0) {
        dx += points[i].x - points[first].x;
        dy += points[i].y - points[first].y;
    }
    if (last > i) {
        dx += points[last].x - points[i].x;
        dy += points[last].y - points[i].y;
    }

    const double len = std::hypot(dx, dy);
    if (len < 1e-9) {
        // A cusp, where the path doubles back on itself. Reuse the previous
        // sample's normal: a fresh arbitrary one would make the two sides swap
        // places and the ribbon fold over itself into an untidy wedge.
        for (size_t j = i; j-- > 0;) {
            const double px = points[j + 1].x - points[j].x;
            const double py = points[j + 1].y - points[j].y;
            const double plen = std::hypot(px, py);
            if (plen >= 1e-9) return Point{-py / plen, px / plen, 0.0, 0.0, 0.0};
        }
        return Point{0.0, -1.0, 0.0, 0.0, 0.0};
    }
    return Point{-dy / len, dx / len, 0.0, 0.0, 0.0};
}

// Unit tangent of the path at index i, averaged across the neighbouring segments.
// Falls back to the nearest usable earlier segment at a cusp, where the path
// doubles back and has no tangent of its own.
Point tangentAt(const std::vector<Point>& points, size_t i) {
    const size_t first = i == 0 ? 0 : i - 1;
    const size_t last = i + 1 < points.size() ? i + 1 : i;

    double dx = 0.0;
    double dy = 0.0;
    if (i > 0) {
        dx += points[i].x - points[first].x;
        dy += points[i].y - points[first].y;
    }
    if (last > i) {
        dx += points[last].x - points[i].x;
        dy += points[last].y - points[i].y;
    }
    double len = std::hypot(dx, dy);
    if (len >= 1e-9) return Point{dx / len, dy / len, 0.0, 0.0, 0.0};

    for (size_t j = i + 1; j-- > 0;) {
        const double px = points[j + 1].x - points[j].x;
        const double py = points[j + 1].y - points[j].y;
        const double plen = std::hypot(px, py);
        if (plen >= 1e-9) return Point{px / plen, py / plen, 0.0, 0.0, 0.0};
    }
    return Point{1.0, 0.0, 0.0, 0.0, 0.0};
}

// A tilted stylus presents an elliptical nib, so the mark gets wider along the
// lean direction and narrower across it. Decomposing the surface normal into
// components parallel and perpendicular to the tilt axis gives that shape.
void offsetSides(const std::vector<Point>& points, const Brush& brush, double radiusScale,
                 std::vector<Point>& left, std::vector<Point>& right) {
    left.clear();
    right.clear();
    left.reserve(points.size());
    right.reserve(points.size());

    for (size_t i = 0; i < points.size(); ++i) {
        const Point& p = points[i];
        const Point n = normalAt(points, i);
        // A cusp, where the path reverses direction, has no usable normal. The
        // offset there falls back to the previous sample's, which keeps the two
        // sides from crossing over each other and inverting the ribbon.
        const double r = radiusFor(p, brush) * radiusScale;

        double nx = n.x;
        double ny = n.y;

        const TiltVector tilt = brush.tiltShading ? tiltVectorOf(p) : TiltVector{};
        if (tilt.valid) {
            const double along = nx * tilt.x + ny * tilt.y;
            const double across = nx * -tilt.y + ny * tilt.x;
            // Slightly wider along the lean and slightly narrower across it, so
            // the mark reads as a flat nib without ballooning.
            const double k = kTiltWidening * tilt.magnitude;
            const double scaledAlong = along * (1.0 + k);
            const double scaledAcross = across * (1.0 - 0.35 * k);
            nx = tilt.x * scaledAlong + (-tilt.y) * scaledAcross;
            ny = tilt.y * scaledAlong + tilt.x * scaledAcross;
        }

        left.push_back(Point{p.x + nx * r, p.y + ny * r});
        right.push_back(Point{p.x - nx * r, p.y - ny * r});
    }
}

// A tilted nib is a flat ellipse, not a circle: it is wider along the lean and
// narrower across it. Returns that ellipse's own half-extents.
void nibExtents(const Point& p, const Brush& brush, double* along, double* across) {
    const double r = radiusFor(p, brush);
    *along = r;
    *across = r;
    if (!brush.tiltShading) return;
    const TiltVector tilt = tiltVectorOf(p);
    if (!tilt.valid) return;
    *along = r * (1.0 + kTiltWidening * tilt.magnitude);
    *across = r * (1.0 - 0.35 * kTiltWidening * tilt.magnitude);
}

// Fills the stroke outline as one closed shape: left side forward, a rounded cap,
// right side back, a rounded cap.
//
// The caps are part of the same polygon rather than separate circles. Filling
// them separately composites the ink twice across the overlap, which showed up as
// darker blotches at the ends of a translucent brush and as a seam across the
// eraser; adding them to one path instead would make the default nonzero winding
// rule cancel wherever a cap winds against the ribbon, punching a hole in the
// mark. Tracing them into the outline keeps one coat of ink and no cancellation.
void fillOutline(cairo_t* cr, const std::vector<Point>& points, const Brush& brush) {
    if (points.empty()) return;

    std::vector<Point> outline;
    if (points.size() == 1) {
        // A tap has no direction to round a cap around, so it is just a dot.
        double along = 0.0;
        double across = 0.0;
        nibExtents(points.front(), brush, &along, &across);
        if (along <= 0.0 || across <= 0.0) return;
        const TiltVector tilt = brush.tiltShading ? tiltVectorOf(points.front()) : TiltVector{};
        const double angle = tilt.valid ? std::atan2(tilt.y, tilt.x) : 0.0;
        const int steps = 32;
        outline.reserve(steps);
        for (int i = 0; i < steps; ++i) {
            const double a = angle + 2.0 * M_PI * i / steps;
            outline.push_back(Point{points.front().x + std::cos(a) * along,
                                    points.front().y + std::sin(a) * across});
        }
    } else {
        std::vector<Point> left, right;
        offsetSides(points, brush, 1.0, left, right);

        // Arc of the nib's own shape, swept from one side point round the outside of the
        // endpoint to the other. `outward` is the direction pointing away from the
        // stroke, and it is what picks the way round: the short way round is not
        // always the outside, and taking it produced the pinched, notched ends
        // visible on a horizontal stroke.
        auto appendCap = [&](const Point& centre, const Point& from, const Point& to,
                             const Point& outward) {
            double along = 0.0;
            double across = 0.0;
            nibExtents(centre, brush, &along, &across);
            if (along <= 0.0 || across <= 0.0) return;

            const TiltVector tilt = brush.tiltShading ? tiltVectorOf(centre) : TiltVector{};
            const double angle = tilt.valid ? std::atan2(tilt.y, tilt.x) : 0.0;

            // Angles measured in the nib's own frame, so the ellipse sweep follows
            // the lean rather than the screen axes.
            const double fromAngle = std::atan2(from.y - centre.y, from.x - centre.x) - angle;
            const double toAngle = std::atan2(to.y - centre.y, to.x - centre.x) - angle;

            

            // From one side point to the other there are exactly two ways round: the direct
            // sweep, and that sweep turned through a full turn. One bulges the cap out
            // past the end of the stroke, the other cuts back into the ribbon.
            //
            // Turning a sweep by a further full turn negates the direction at its
            // midpoint, so the two candidate bulges are one unit vector and its
            // negation. A dot product against the outward direction picks between
            // them unambiguously.
            //
            // The raw difference is used rather than a value folded into (-pi, pi]:
            // a half turn is the common case here, and folding it collapses both
            // candidates onto the same angle, which reintroduced the wedge-shaped
            // notch this is meant to prevent.
            const double direct = toAngle - fromAngle;

            // The cap's bulge is at the midpoint of the sweep, counted from where
            // the sweep starts rather than from zero. Using the sweep on its own
            // pointed the test at the wrong part of the nib and left a wedge-shaped
            // notch at both ends of a tilted stroke.
            const double mid = fromAngle + direct * 0.5;
            const double ux = std::cos(mid);
            const double uy = std::sin(mid);

            // Outward direction, rotated into the nib's frame alongside the angles.
            const double ox = std::cos(angle) * outward.x + std::sin(angle) * outward.y;
            const double oy = -std::sin(angle) * outward.x + std::cos(angle) * outward.y;

            // Prefer the direct sweep when its midpoint already points outward;
            // otherwise take the long way round, whose midpoint is the opposite one.
            const double sweep =
                (ux * ox + uy * oy) >= 0.0 ? direct : direct + 2.0 * M_PI;

            // Steps proportional to the sweep, so the cap stays round at any size.
            const int steps =
                std::max(3, static_cast<int>(std::ceil(std::fabs(sweep) / (M_PI / 8.0))));
            for (int i = 1; i < steps; ++i) {
                const double a = fromAngle + sweep * (static_cast<double>(i) / steps);
                outline.push_back(Point{centre.x + std::cos(a + angle) * along,
                                        centre.y + std::sin(a + angle) * across});
            }
        };

        outline.reserve(left.size() * 2 + 32);
        outline.insert(outline.end(), left.begin(), left.end());

        // Tail: outward is the direction of travel at the last sample.
        Point tailOut = tangentAt(points, points.size() - 1);
        appendCap(points.back(), left.back(), right.back(), tailOut);

        for (size_t i = right.size(); i-- > 0;) outline.push_back(right[i]);

        // Head: outward points back the way the stroke came from.
        Point headOut = tangentAt(points, 0);
        headOut.x = -headOut.x;
        headOut.y = -headOut.y;
        appendCap(points.front(), right.front(), left.front(), headOut);
    }

    if (outline.size() < 3) return;

    cairo_new_path(cr);
    cairo_move_to(cr, outline.front().x, outline.front().y);
    for (size_t i = 1; i < outline.size(); ++i) {
        cairo_line_to(cr, outline[i].x, outline[i].y);
    }
    cairo_close_path(cr);
    cairo_fill(cr);
}

// Grainy tooth typical of graphite: sparse low-alpha specks along the path.
void drawPencilGrain(cairo_t* cr, const std::vector<Point>& points, const Brush& brush) {
    if (points.empty()) return;
    cairo_save(cr);
    for (size_t i = 0; i < points.size(); i += 2) {
        const Point& p = points[i];
        // The grain's alpha follows the same scaled pressure as the mark's width,
        // so a low sensitivity fades the speckle instead of leaving it dark over a
        // stroke that no longer tapers.
        const double a = scaledPressure(p, brush);
        const double rad = radiusFor(p, brush);
        // The specks sit inside the stroke, so they are offset by less than the
        // nib half-width; at light pressure that offset has to shrink too or the
        // grain falls outside the mark it is meant to texture.
        const double spread = rad * 0.5 * a;
        cairo_set_source_rgba(cr, brush.r, brush.g, brush.b, 0.16 * a);
        cairo_new_path(cr);
        // The offsets walk a fixed sequence rather than random, so the same
        // stroke always grains the same way and undo is exact.
        cairo_arc(cr, p.x + std::sin(i * 1.7) * spread,
                  p.y + std::cos(i * 2.3) * spread,
                  std::max(0.35, rad * 0.22 * a), 0.0, 2.0 * M_PI);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

}  // namespace

void Brush::setHex(const std::string& hex) {
    // Parsing lives in parseHexColor so the page background can reuse it. It
    // leaves `out` alone on malformed input, which is what keeps a hand-edited
    // project file from throwing and killing the process.
    Color parsed{r, g, b};
    if (!parseHexColor(hex, &parsed)) return;
    r = parsed.r;
    g = parsed.g;
    b = parsed.b;
}

std::string Brush::hex() const { return formatHexColor(Color{r, g, b}); }

bool pushPoint(Stroke& stroke, const Point& p, double minSpacing, bool dot) {
    if (!dot && !stroke.points.empty()) {
        if (distance(stroke.points.back(), p) < minSpacing) return false;
    }
    stroke.points.push_back(p);
    return true;
}

double pressureOf(const Point& p) {
    if (p.pressure > 0.0) return clampd(p.pressure, 0.02, 1.0);
    return 0.5;
}

double scaledPressure(const Point& p, const Brush& brush) {
    const double raw = pressureOf(p);
    // Pull the reading toward neutral rather than scaling it toward zero: scaling
    // would leave a zero-sensitivity brush at its thinnest width instead of at the
    // middle of its range, which is not what "ignores pressure" should look like.
    const double neutral = 0.5;
    return neutral + (raw - neutral) * clampd(brush.pressureSensitivity, 0.0, 1.0);
}

double radiusFor(const Point& p, const Brush& brush) {
    const double size = brush.size;
    switch (brush.kind) {
        case BrushKind::Pencil:
            return size * 0.9 * (0.55 + 0.45 * std::sqrt(scaledPressure(p, brush)));
        case BrushKind::Marker:
            return size * 1.6 * (0.9 + 0.1 * scaledPressure(p, brush));
        case BrushKind::Eraser:
            return size * 2.2;
        case BrushKind::Pen:
        default:
            if (!brush.pressureWidth) return size * 0.6;
            return size * (0.12 + 0.88 * std::pow(scaledPressure(p, brush), 0.72));
    }
}

TiltVector tiltVectorOf(const Point& p) {
    const double len = std::hypot(p.tiltX, p.tiltY);
    TiltVector out;
    if (len < 1.0) return out;
    out.x = p.tiltX / len;
    out.y = p.tiltY / len;
    out.magnitude = clampd(len / kTiltFullScale, 0.0, 1.0);
    out.valid = true;
    return out;
}

double tiltDegrees(double raw) {
    // GDK hands out tilt in radians on some backends and degrees on others. A
    // stylus never leans past 90 degrees, so a value larger than a radian's
    // worth of lean can only already be degrees.
    constexpr double kMaxRadians = 1.6;  // 91.7 degrees
    return std::fabs(raw) <= kMaxRadians ? raw * 180.0 / M_PI : raw;
}

void buildOutline(const std::vector<Point>& points, const Brush& brush, std::vector<Point>& out) {
    out.clear();
    if (points.empty()) return;

    if (points.size() == 1) {
        const double r = radiusFor(points.front(), brush);
        const Point& c = points.front();
        const int kSteps = 24;
        for (int i = 0; i < kSteps; ++i) {
            const double a = 2.0 * M_PI * i / kSteps;
            out.push_back(Point{c.x + std::cos(a) * r, c.y + std::sin(a) * r});
        }
        return;
    }

    std::vector<Point> left, right;
    offsetSides(points, brush, 1.0, left, right);

    out.reserve(left.size() * 2);
    out.insert(out.end(), left.begin(), left.end());
    out.insert(out.end(), right.rbegin(), right.rend());
}

void drawStroke(cairo_t* cr, const Stroke& stroke, const Brush& brush) {
    const std::vector<Point>& points = stroke.points;
    if (points.empty()) return;

    const bool eraser = brush.kind == BrushKind::Eraser;
    const bool marker = brush.kind == BrushKind::Marker;

    cairo_save(cr);
    if (eraser) {
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    } else {
        if (marker) {
            cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
        } else {
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        }
        double alpha = 1.0;
        if (marker) alpha = kMarkerAlpha;
        else if (brush.kind == BrushKind::Pencil) alpha = kPencilAlpha;
        cairo_set_source_rgba(cr, brush.r, brush.g, brush.b, alpha);
    }

    fillOutline(cr, points, brush);

    if (brush.kind == BrushKind::Pencil) drawPencilGrain(cr, points, brush);

    cairo_restore(cr);
}

void drawStrokeOn(cairo_surface_t* surface, const Stroke& stroke, const Brush& brush) {
    cairo_t* cr = cairo_create(surface);
    drawStroke(cr, stroke, brush);
    cairo_destroy(cr);
}

// Half-width of the nib as actually drawn, including the widening a tilt applies.
// The outline, the caps and the bounds all have to agree on this, or the shape
// gets clipped in one place and not another.
double nibHalfWidth(const Point& p, const Brush& brush) {
    const double r = radiusFor(p, brush);
    if (!brush.tiltShading) return r;
    const TiltVector tilt = tiltVectorOf(p);
    if (!tilt.valid) return r;
    return r * (1.0 + kTiltWidening * tilt.magnitude);
}

Rect strokeBounds(const std::vector<Point>& points, const Brush& brush) {
    if (points.empty()) return Rect{};

    // Note: boundsOf on its own returns a zero-height rect for a perfectly
    // horizontal stroke, and Rect::empty() treats that as "nothing here". Bailing
    // out on it meant a straight horizontal or vertical line got no padding and no
    // undo snapshot at all, so undo silently did nothing for the most ordinary
    // stroke there is. Only the empty point list is a real empty.
    Rect r = boundsOf(points);

    double maxRadius = 0.0;
    for (const Point& p : points) maxRadius = std::max(maxRadius, nibHalfWidth(p, brush));
    // The extra pixels cover antialiasing and the midpoint smoothing, which
    // bows the outline slightly outside the offset points.
    const double pad = maxRadius + 2.0;
    r.x -= pad;
    r.y -= pad;
    r.w += pad * 2.0;
    r.h += pad * 2.0;
    return r;
}

}  // namespace stylus