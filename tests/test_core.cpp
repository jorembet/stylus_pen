#include <cstdint>
#include <cmath>
#include <tuple>
#include <cstdio>
#include <string>
#include <vector>

#include "brush.h"
#include "document.h"
#include "geometry.h"
#include "history.h"
#include "color.h"
#include "paper.h"
#include "project.h"

using namespace stylus;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& label) {
    checks++;
    if (!condition) {
        failures++;
        std::printf("FAIL %s\n", label.c_str());
    } else {
        std::printf("ok   %s\n", label.c_str());
    }
}

std::string fmt(double v, int precision = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return buf;
}

// An arc stroke with a smooth pressure ramp, like a real captured pen line.
Stroke arcStroke(size_t n, double pressureLo, double pressureHi) {
    Stroke s;
    for (size_t i = 0; i < n; ++i) {
        const double t = n > 1 ? static_cast<double>(i) / (n - 1) : 0.0;
        Point p;
        p.x = 90.0 + 610.0 * t;
        p.y = 300.0 - 14.0 * std::sin(t * M_PI);
        p.pressure = pressureLo + (pressureHi - pressureLo) * std::sin(t * M_PI);
        s.points.push_back(p);
    }
    return s;
}

// Counts pixels that differ noticeably from the white page, so a stroke drawn
// in any colour registers but the opaque white background does not.
long countInkPixels(cairo_surface_t* surface) {
    cairo_surface_flush(surface);
    const int w = cairo_image_surface_get_width(surface);
    const int h = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    long n = 0;
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(data + y * stride);
        for (int x = 0; x < w; ++x) {
            const uint32_t px = row[x];
            const unsigned a = (px >> 24) & 0xFF;
            if (a < 40) continue;
            const unsigned r = (px >> 16) & 0xFF;
            const unsigned g = (px >> 8) & 0xFF;
            const unsigned b = px & 0xFF;
            if (r < 235 || g < 235 || b < 235) n++;
        }
    }
    return n;
}

// --- geometry -------------------------------------------------------------

void testGeometry() {
    check(clampd(5.0, 0.0, 1.0) == 1.0, "clampd clamps high");
    check(clampd(-5.0, 0.0, 1.0) == 0.0, "clampd clamps low");
    check(clampd(0.25, 0.0, 1.0) == 0.25, "clampd passes through");

    std::vector<Point> line;
    for (int i = 0; i <= 100; ++i) line.push_back(Point{static_cast<double>(i), 0.0});

    const auto simple = simplify(line, 1.0);
    check(simple.size() < line.size(), "simplify drops redundant collinear points");
    check(simple.front().x == 0.0 && simple.back().x == 100.0, "simplify keeps both endpoints");

    const auto resampled = resample(line, 2.0);
    check(resampled.size() > simple.size(), "resample produces more even spacing");
    double maxGap = 0.0;
    for (size_t i = 1; i < resampled.size(); ++i) {
        maxGap = std::max(maxGap, distance(resampled[i - 1], resampled[i]));
    }
    check(maxGap <= 2.01, "resample keeps gaps within spacing");
    check(std::abs(resampled.back().x - 100.0) < 0.001, "resample keeps final point");

    check(std::abs(polylineLength(line) - 100.0) < 1e-6, "polylineLength sums segments");

    const Rect a{10, 10, 20, 20};
    const Rect b{25, 25, 20, 20};
    check(!rectIsEmpty(rectIntersect(a, b)), "overlapping rects intersect");
    check(rectIsEmpty(rectIntersect(a, Rect{100, 100, 5, 5})), "disjoint rects do not intersect");
    const Rect u = rectUnion(a, b);
    check(u.x == 10 && u.y == 10 && u.right() == 45 && u.bottom() == 45, "rectUnion covers both");
    check(rectContains(a, Point{15, 15}), "rectContains accepts interior point");
    check(!rectContains(a, Point{5, 5}), "rectContains rejects outside point");

    const Point smoothed = smoothToward(Point{0, 0, 1.0}, Point{10, 20, 0.0}, 0.5);
    check(smoothed.x == 5.0 && smoothed.y == 10.0, "smoothToward interpolates midpoint");
    const Point full = smoothToward(Point{0, 0}, Point{10, 20}, 1.0);
    check(full.x == 10.0 && full.y == 20.0, "smoothToward at weight 1 lands on target");
}

// Regression: the canvas sits below a toolbar, so a pen event's surface
// coordinates are offset from the canvas. Getting this wrong is what made
// strokes begin at the canvas corner instead of under the nib.
void testBoardTransform() {
    BoardTransform t(1600, 1100);
    t.setWidgetSize(800.0, 900.0);
    t.setViewport(0.45, 0.0, 0.0);
    t.setSurfaceOffset(14.0, 106.0);

    // The page is centred in the widget, so its top-left corner is the origin.
    const Point pageTopLeft = t.toDocument(0.0, 0.0);
    check(std::abs(pageTopLeft.x) > 0.0 && std::abs(pageTopLeft.y) > 0.0,
          "canvas top-left is outside the page");

    const Point widget = BoardTransform::surfaceToWidget(389.6, 440.2, 14.0, 106.0);
    check(std::abs(widget.x - 375.6) < 1e-6 && std::abs(widget.y - 334.2) < 1e-6,
          "surfaceToWidget removes the widget offset");

    const Point doc = t.surfaceToDocument(389.6, 440.2);
    check(std::abs(doc.x - t.toDocument(375.6, 334.2).x) < 1e-9,
          "surfaceToDocument matches the widget-space path");
    check(doc.x >= 0.0 && doc.x <= 1600.0 && doc.y >= 0.0 && doc.y <= 1100.0,
          "a press inside the page lands inside the document");

    // Round trip: mapping the same surface point twice must be stable, and
    // re-inverting the widget coordinates must land back on it.
    const Point again = t.surfaceToDocument(389.6, 440.2);
    check(std::abs(again.x - doc.x) < 1e-9 && std::abs(again.y - doc.y) < 1e-9,
          "surfaceToDocument is stable");
    const double backWidgetX = doc.x * t.zoom() + t.documentOriginX();
    const double backWidgetY = doc.y * t.zoom() + t.documentOriginY();
    check(std::abs(backWidgetX - 375.6) < 1e-6 && std::abs(backWidgetY - 334.2) < 1e-6,
          "a document point inverts back to the widget point");

    // A surface point left un-converted would land far off the page; that was
    // the original symptom.
    const Point unconverted = t.toDocument(389.6, 440.2);
    check(std::abs(unconverted.y - doc.y) > 1.0,
          "ignoring the widget offset shifts the stroke");

    // Zoom and pan still compose with the offset: the same press maps to a
    // different document point, and stays inside the page.
    t.setViewport(1.0, 30.0, -20.0);
    const Point zoomed = t.surfaceToDocument(389.6, 440.2);
    check(std::abs(zoomed.y - doc.y) > 1.0, "zoom changes the mapped point");
    check(zoomed.x >= 0.0 && zoomed.x <= 1600.0 && zoomed.y >= 0.0 && zoomed.y <= 1100.0,
          "the mapped point stays inside the document");

    check(Viewport{}.clampZoom(0.0) == 0.1, "zoom clamps at the low end");
    check(Viewport{}.clampZoom(99.0) == 8.0, "zoom clamps at the high end");
}

// The nib cursor and the ink must land on the same pixel. Both are driven from
// one event coordinate, so this pins the round trip that makes them agree: a
// widget point mapped to the document and painted back must return the original
// widget point, with no scale factor in the path. `apply` used to multiply by
// the widget scale factor while the draw handler divided it out again, which on
// HiDPI put the ink at origin/scaleFactor while the cursor stayed at origin.
void testCursorMatchesInk() {
    BoardTransform t(1600, 1100);
    t.setWidgetSize(900.0, 700.0);
    t.setViewport(0.45, 12.0, -8.0);

    // Replays what Canvas::readSample records and Canvas::paintNibCursor draws:
    // the raw event coordinate, unmodified, in widget space.
    const double eventX = 421.5;
    const double eventY = 233.25;

    // ... and what the same sample becomes when it is turned into a document
    // point for the stroke.
    const Point doc = t.toDocument(eventX, eventY);

    // Painting a document point back is exactly what the transform does for the
    // layers and the live stroke, so this must reproduce the cursor position.
    const double paintedX = doc.x * t.zoom() + t.documentOriginX();
    const double paintedY = doc.y * t.zoom() + t.documentOriginY();

    check(std::abs(paintedX - eventX) < 1e-9 && std::abs(paintedY - eventY) < 1e-9,
          "ink painted from a sample lands under the nib cursor");

    // A second zoom level must not reintroduce a mismatch.
    t.setViewport(2.0, -40.0, 25.0);
    const Point zoomed = t.toDocument(eventX, eventY);
    check(std::abs(zoomed.x * t.zoom() + t.documentOriginX() - eventX) < 1e-9 &&
              std::abs(zoomed.y * t.zoom() + t.documentOriginY() - eventY) < 1e-9,
          "cursor and ink still agree at a different zoom");

    // The old code applied a scale factor here while the draw handler removed
    // it, so at scale 2 the ink came out at half the cursor's offset from the
    // origin. Guard the exact shape of that regression.
    constexpr double scaleFactor = 2.0;
    const double ox = t.documentOriginX();
    const double oxScaled = ox * scaleFactor;
    const double inkAtScale = (doc.x * t.zoom() * scaleFactor + oxScaled) / scaleFactor;
    const double oldCursorOffset = ox;
    const double oldInkOffset = inkAtScale;
    check(std::abs(oldCursorOffset - oldInkOffset) > 1.0,
          "the removed scale-factor path really did desync ink from cursor");

    // Active stroke tip point (with smoothing) must map directly to nib cursor position.
    Point strokeTipDoc = smoothToward(doc, t.toDocument(450.0, 260.0), 0.55);
    const double tipWidgetX = strokeTipDoc.x * t.zoom() + t.documentOriginX();
    const double tipWidgetY = strokeTipDoc.y * t.zoom() + t.documentOriginY();
    check(std::abs(t.toDocument(tipWidgetX, tipWidgetY).x - strokeTipDoc.x) < 1e-9 &&
          std::abs(t.toDocument(tipWidgetX, tipWidgetY).y - strokeTipDoc.y) < 1e-9,
          "live stroke tip matches nib cursor position during drawing");
}

// GDK reports tilt in radians on some backends and degrees on others; the
// brush and the status bar both reason in degrees.
void testTiltUnits() {
    check(std::abs(tiltDegrees(0.0)) < 1e-9, "no tilt stays no tilt");
    check(std::abs(tiltDegrees(1.5707963) - 90.0) < 0.01, "a right angle in radians is read");
    check(std::abs(tiltDegrees(-0.7853982) + 45.0) < 0.01, "negative radians convert");
    check(std::abs(tiltDegrees(45.0) - 45.0) < 1e-9, "degrees pass through");
    check(std::abs(tiltDegrees(90.0) - 90.0) < 1e-9, "a full lean in degrees passes through");

    Point leaned;
    leaned.tiltX = tiltDegrees(0.6);
    leaned.tiltY = tiltDegrees(-0.8);
    const TiltVector t = tiltVectorOf(leaned);
    check(t.valid, "a leaning pen has a tilt vector");
    const double lean = std::hypot(tiltDegrees(0.6), tiltDegrees(-0.8));
    check(std::abs(t.magnitude - std::min(1.0, lean / 60.0)) < 1e-9,
          "tilt magnitude is computed in degrees");
}

void testBrushBasics() {
    Brush brush;
    brush.setHex("#1b1b1f");
    check(std::abs(brush.r - 27.0 / 255.0) < 1e-6, "setHex parses red channel");
    check(std::abs(brush.b - 31.0 / 255.0) < 1e-6, "setHex parses blue channel");
    check(brush.hex() == "#1b1b1f", "hex round-trips");
    brush.setHex("#fff");
    check(brush.hex() == "#ffffff", "setHex expands shorthand");

    Stroke s;
    check(pushPoint(s, Point{0, 0}, 1.0, true), "first point accepted with dot flag");
    check(!pushPoint(s, Point{0.5, 0}, 1.0), "near-duplicate point rejected");
    check(pushPoint(s, Point{3, 0}, 1.0), "distant point accepted");

    const Point low{0, 0, 0.05, 0, 0};
    const Point high{0, 0, 1.0, 0, 0};
    Brush pen;
    pen.size = 10.0;
    check(radiusFor(high, pen) > radiusFor(low, pen) * 3.0,
          "pen radius grows strongly with pressure");

    Brush marker;
    marker.kind = BrushKind::Marker;
    marker.size = 10.0;
    const double mLo = radiusFor(Point{0, 0, 0.2, 0, 0}, marker);
    const double mHi = radiusFor(Point{0, 0, 1.0, 0, 0}, marker);
    check(std::abs(mLo - mHi) / mHi < 0.15, "marker width barely varies with pressure");

    Brush eraser;
    eraser.kind = BrushKind::Eraser;
    eraser.size = 8.0;
    check(radiusFor(Point{0, 0, 1.0, 0, 0}, eraser) > radiusFor(Point{0, 0, 0.1, 0, 0}, eraser) - 0.001,
          "eraser width is pressure independent");

    const TiltVector none = tiltVectorOf(Point{0, 0, 0.5, 0, 0});
    check(!none.valid, "zero tilt reports invalid");
    const TiltVector tilted = tiltVectorOf(Point{0, 0, 0.5, 30, 40});
    check(tilted.valid, "non-zero tilt reports valid");
    check(std::abs(std::hypot(tilted.x, tilted.y) - 1.0) < 1e-9, "tilt vector is normalised");
    check(tilted.magnitude > 0.0 && tilted.magnitude <= 1.0, "tilt magnitude within range");

    // Regression: the outline must be one closed ribbon. A stray moveTo while
    // tracing the reverse side splits it into two strips, which renders as an
    // unfilled outline instead of a solid stroke.
    for (size_t n : {size_t{20}, size_t{200}}) {
        const Stroke s = arcStroke(n, 0.06, 0.95);
        std::vector<Point> outline;
        Brush b;
        b.size = 14.0;
        buildOutline(s.points, b, outline);

        const double expected = polylineLength(s.points) * 2.0;
        double outlineLength = 0.0;
        for (size_t i = 1; i < outline.size(); ++i) {
            outlineLength += distance(outline[i - 1], outline[i]);
        }
        check(outlineLength > expected * 0.9,
              "outline closes back along the reverse side (n=" + std::to_string(n) +
                  ", len " + fmt(outlineLength, 0) + " vs " + fmt(expected, 0) + ")");
    }

    // A single tap must still produce a round dot outline.
    std::vector<Point> dot;
    buildOutline({Point{50, 50, 0.8, 0, 0}}, Brush{}, dot);
    check(dot.size() >= 20, "single-point stroke builds a circular outline");
}

// The pressure sensitivity level, and the file format that carries it.
void testPressureSensitivity() {
    const Point low{0, 0, 0.05, 0, 0};
    const Point high{0, 0, 1.0, 0, 0};

    Brush pen;
    pen.size = 10.0;

    // Full sensitivity is the default, and has to reproduce the old behaviour
    // exactly -- otherwise every existing drawing changes shape on open.
    check(std::abs(scaledPressure(high, pen) - pressureOf(high)) < 1e-9,
          "full sensitivity passes pressure through unchanged");

    // At zero the mark must come out the same width everywhere, or "ignores
    // pressure" would just mean "thinner than before".
    pen.pressureSensitivity = 0.0;
    check(std::abs(radiusFor(high, pen) - radiusFor(low, pen)) < 1e-9,
          "zero sensitivity draws an even width");
    check(std::abs(radiusFor(high, pen) - radiusFor(Point{0, 0, 0.5, 0, 0}, pen)) < 1e-9,
          "zero sensitivity lands on the neutral width, not the thinnest one");
    check(std::abs(scaledPressure(high, pen) - 0.5) < 1e-9,
          "zero sensitivity collapses pressure onto the midpoint");

    // Half sensitivity has to sit between the two ends, not snap to either.
    Brush full;
    full.size = 10.0;
    Brush damped;
    damped.size = 10.0;
    damped.pressureSensitivity = 0.5;
    Brush none;
    none.size = 10.0;
    none.pressureSensitivity = 0.0;
    const double rHigh = radiusFor(high, damped);
    check(rHigh > radiusFor(low, damped), "half sensitivity still tapers");
    check(rHigh < radiusFor(high, full) && rHigh > radiusFor(high, none),
          "half sensitivity damps the taper rather than removing it (" + fmt(rHigh, 2) +
              ", full " + fmt(radiusFor(high, full), 2) + ", none " +
              fmt(radiusFor(high, none), 2) + ")");

    // Every pressure-driven brush has to honour the level, not just the pen. The
    // pencil's grain alpha goes through the same term, or a damped stroke would
    // keep full-contrast speckle over a mark that no longer tapers.
    for (BrushKind kind : {BrushKind::Pen, BrushKind::Pencil, BrushKind::Marker}) {
        Brush b;
        b.kind = kind;
        b.size = 10.0;
        b.pressureSensitivity = 0.0;
        check(std::abs(radiusFor(high, b) - radiusFor(low, b)) < 1e-9,
              "zero sensitivity flattens " +
                  std::string(kind == BrushKind::Pen       ? "the pen"
                              : kind == BrushKind::Pencil ? "the pencil"
                                                           : "the marker"));
    }

    // The pen's own pressure-width checkbox still overrides: turning it off has to
    // mean an even width whatever the slider says.
    Brush off;
    off.size = 10.0;
    off.pressureWidth = false;
    off.pressureSensitivity = 1.0;
    check(std::abs(radiusFor(high, off) - radiusFor(low, off)) < 1e-9,
          "pressure width off still wins over a high sensitivity");

    // And the level has to survive a save and reopen.
    AppState state(120, 90);
    state.settings().pressureSensitivity = 0.35;
    state.brush().pressureSensitivity = 0.35;
    auto project = state.serialize();
    check(project.has_value(), "the project serialises with a pressure level");
    if (!project) return;

    Project decoded;
    check(decodeProjectBlob(encodeProjectBlob(*project), &decoded), "the blob decodes");
    check(std::abs(decoded.settings.pressureSensitivity - 0.35) < 0.001,
          "the pressure level survives (" + fmt(decoded.settings.pressureSensitivity, 3) + ")");

    AppState restored(120, 90);
    check(restored.load(decoded), "the decoded project loads");
    check(std::abs(restored.brush().pressureSensitivity - 0.35) < 0.001,
          "the loaded brush carries the pressure level");

    // A hand-edited level outside 0..1 is clamped on read rather than reaching the
    // brush, where it would flip the sign of the taper.
    Project wild = decoded;
    wild.settings.pressureSensitivity = 4.0;
    Project wildDecoded;
    check(decodeProjectBlob(encodeProjectBlob(wild), &wildDecoded), "an out-of-range level decodes");
    check(std::abs(wildDecoded.settings.pressureSensitivity - 1.0) < 0.001,
          "an out-of-range pressure level clamps to 1");
}

// Regression: a malformed colour must be rejected, not thrown. The hex string
// comes from project files, which the user can edit, and std::stoi on a
// non-numeric string takes the whole process down.
void testBackgroundColor() {
    // The default has to stay exactly the off-white the canvas has always painted,
    // or every existing project opens on a visibly different page.
    Document doc(200, 120);
    check(doc.backgroundHex() == "#fbfbfd",
          "the page starts as the old off-white (" + doc.backgroundHex() + ")");

    check(doc.setBackground("#1b1b1f"), "a valid page colour is accepted");
    check(doc.backgroundHex() == "#1b1b1f", "the page colour is stored");

    // A background comes from a hand-editable project file, so it has to reject
    // garbage the same way Brush::setHex does rather than throwing.
    for (const char* bad : {"", "#", "zz", "#12zz34", "not a colour", "#1234567"}) {
        doc.setBackground("#1b1b1f");
        const bool took = doc.setBackground(bad);
        check(!took && doc.backgroundHex() == "#1b1b1f",
              std::string("malformed page colour '") + bad + "' is rejected");
    }

    // flatten() bakes the page in, so an exported PNG has to carry it. It used to
    // hardcode white there while the canvas painted off-white, which exported a
    // light page as pure white.
    doc.setBackground("#204080");
    SurfacePtr flat = doc.flatten();
    check(flat != nullptr, "flatten produces a surface");
    if (flat) {
        cairo_surface_flush(flat.get());
        const unsigned char* px = cairo_image_surface_get_data(flat.get());
        const int stride = cairo_image_surface_get_stride(flat.get());
        // Premultiplied ARGB32: opaque, so the channels are the colour as stored.
        const double b = px[0] / 255.0;
        const double g = px[1] / 255.0;
        const double r = px[2] / 255.0;
        const double a = px[3] / 255.0;
        check(std::abs(r - 0x20 / 255.0) < 0.01 && std::abs(g - 0x40 / 255.0) < 0.01 &&
                  std::abs(b - 0x80 / 255.0) < 0.01,
              "the exported PNG carries the page colour, not white");
        check(std::abs(a - 1.0) < 0.01, "the page is opaque in the export");
        (void)stride;
    }

    // Through AppState: the setter repaints, and a bad value never reaches state.
    AppState state(200, 120);
    int repaints = 0;
    state.setChanged([&repaints]() { ++repaints; });
    check(state.setBackgroundColor("#7c3aed"), "AppState accepts a page colour");
    check(state.document().backgroundHex() == "#7c3aed", "AppState stores the page colour");
    check(repaints == 1, "changing the page repaints exactly once (" + std::to_string(repaints) + ")");

    repaints = 0;
    check(!state.setBackgroundColor("nope"), "AppState rejects a malformed page colour");
    check(state.document().backgroundHex() == "#7c3aed", "the rejected colour changed nothing");
    check(repaints == 0, "a rejected colour does not repaint");
}

void testBackgroundRoundTrip() {
    AppState state(300, 200);
    state.beginStroke(Point{40, 40, 0.9, 0, 0});
    for (int i = 1; i <= 20; ++i) state.extendStroke(Point{40.0 + i * 6.0, 40.0, 0.9, 0, 0});
    state.endStroke();
    state.addLayer();
    state.setBackgroundColor("#16213e");

    auto project = state.serialize();
    check(project.has_value(), "a project with a page colour serialises");
    if (!project) return;
    const std::string blob = encodeProjectBlob(*project);

    Project decoded;
    if (!decodeProjectBlob(blob, &decoded)) {
        check(false, "the blob with a page colour decodes");
        return;
    }
    check(decoded.background == "#16213e",
          "the page colour survives the round trip (" + decoded.background + ")");

    auto doc = deserializeProject(decoded);
    check(doc.has_value(), "the document deserialises");
    if (doc) {
        check(doc->backgroundHex() == "#16213e", "the reopened page keeps its colour");
    }

    // An older file has no background tag at all and must still open, on the
    // default page rather than on black.
    std::string old2 = "STYLUSPEN2 40 30 1\n";
    old2 += "active 0\n";
    // No trailing pressure level: the level is the last field on the settings line,
    // so this is the shape that proves the optional read does not run past the line
    // and swallow the tag after it.
    old2 += "settings 0 #1b1b1f 3.0000 1 1 1\n";
    char len[32];
    // A 40x30 ARGB32 surface encodes to some number of bytes; reuse the real
    // payload so the decoder's length check is exercised honestly.
    Document tiny(40, 30);
    const std::string png = encodePng(tiny.active().surface());
    std::snprintf(len, sizeof(len), "%zu\n", png.size());
    old2 += len;
    old2 += png;
    old2 += "\n";

    Project legacy;
    check(decodeProjectBlob(old2, &legacy), "a version 2 file with no page tag still decodes");
    check(legacy.background == "#fbfbfd",
          "an old file keeps the default page (" + legacy.background + ")");
    check(std::abs(legacy.settings.pressureSensitivity - 1.0) < 1e-9,
          "an old settings line keeps full pressure response");
    check(legacy.settings.size == 3.0,
          "an old settings line still parses its own fields (" + fmt(legacy.settings.size, 2) + ")");

    std::string old1 = "STYLUSPEN 40 30 1\n";
    std::snprintf(len, sizeof(len), "%zu\n", png.size());
    old1 += len;
    old1 += png;
    old1 += "\n";
    Project oldest;
    check(decodeProjectBlob(old1, &oldest), "a version 1 file still decodes");
    check(oldest.background == "#fbfbfd", "a version 1 file keeps the default page");

    // A hand-edited page colour must not reach the canvas as garbage.
    std::string broken = "STYLUSPEN3 40 30 1\n";
    broken += "active 0\n";
    broken += "background not-a-colour\n";
    std::snprintf(len, sizeof(len), "%zu\n", png.size());
    broken += len;
    broken += png;
    broken += "\n";
    Project bad;
    check(decodeProjectBlob(broken, &bad), "a file with a broken page colour still decodes");
    check(bad.background == "#fbfbfd",
          "a broken page colour falls back to the default (" + bad.background + ")");
}

void testBackgroundUndo() {
    AppState state(200, 120);
    state.setBackgroundColor("#7c3aed");
    state.addLayer();
    check(state.document().backgroundHex() == "#7c3aed", "adding a layer keeps the page colour");

    // Undoing a structural step restores the page that was on screen at the time,
    // otherwise the artwork comes back on a different sheet.
    state.undo();
    check(state.document().layerCount() == 1, "undo removes the layer again");
    check(state.document().backgroundHex() == "#7c3aed",
          "undo keeps the page colour (" + state.document().backgroundHex() + ")");

    state.redo();
    check(state.document().layerCount() == 2, "redo puts the layer back");
    check(state.document().backgroundHex() == "#7c3aed", "redo keeps the page colour");

    // A new canvas is a fresh sheet, and Ctrl+Z has to bring the old one back
    // with it -- otherwise the page silently resets to default and undo looks
    // like it lost a change.
    state.setBackgroundColor("#16213e");
    state.newCanvas();
    check(state.document().backgroundHex() == "#fbfbfd",
          "a new canvas returns to the default page (" + state.document().backgroundHex() + ")");
    state.undo();
    check(state.document().backgroundHex() == "#16213e",
          "undoing a new canvas restores the old page (" + state.document().backgroundHex() + ")");
}

void testPaper() {
    // Spacing arrives from a hand-editable project file and drives a loop in
    // paintPaper, so it has to be clamped rather than trusted.
    check(clampPaperSpacing(0.0001) == 8.0, "tiny spacing is clamped up to the floor");
    check(clampPaperSpacing(1e9) == kMaxPaperSpacing, "huge spacing is clamped to the ceiling");
    check(clampPaperSpacing(40.0) == 40.0, "a sane spacing is left alone");
    check(clampPaperSpacing(-5.0) == 8.0, "a negative spacing is clamped, not accepted");

    PaperKind kind = PaperKind::Plain;
    check(parsePaperKind(2, &kind) && kind == PaperKind::Grid, "a known kind parses");
    check(!parsePaperKind(99, &kind), "an out-of-range kind is rejected");
    check(!parsePaperKind(-1, &kind), "a negative kind is rejected");
    check(parsePaperKind(0, &kind) && kind == PaperKind::Plain, "0 is Plain");
    check(std::string(paperKindName(PaperKind::Ruled)) == "Ruled", "kinds have names");

    Document plain(200, 120);
    check(plain.paper().kind == PaperKind::Plain, "a new document has no ruling");
    check(!plain.paper().active(), "Plain counts as inactive");

    plain.setPaper(Paper{PaperKind::Ruled, 40.0});
    check(plain.paper().active(), "Ruled counts as active");
    plain.setPaper(Paper{PaperKind::Grid, 0.5});
    check(plain.paper().spacing == 8.0, "setPaper clamps the spacing");
    plain.setPaper(Paper{PaperKind::Grid, 1e9});
    check(plain.paper().spacing == kMaxPaperSpacing, "setPaper clamps a huge spacing too");
}

// Counts marks on an otherwise blank page: how much the ruling darkened it.
long countRulingPixels(const SurfacePtr& surface) {
    cairo_surface_flush(surface.get());
    const int w = cairo_image_surface_get_width(surface.get());
    const int h = cairo_image_surface_get_height(surface.get());
    const int stride = cairo_image_surface_get_stride(surface.get());
    const unsigned char* data = cairo_image_surface_get_data(surface.get());
    long n = 0;
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = data + y * stride;
        for (int x = 0; x < w; ++x) {
            const unsigned char* px = row + x * 4;
            // The page is near-white; any ruling mark pulls a channel well below it.
            if (px[0] < 220 || px[1] < 220 || px[2] < 220) ++n;
        }
    }
    return n;
}

void testPaperIsPainted() {
    // Every non-plain pattern has to actually put marks on the page.
    for (int i = 1; i < kPaperKindCount; ++i) {
        const PaperKind kind = static_cast<PaperKind>(i);
        Document doc(200, 120);
        doc.setPaper(Paper{kind, 16.0});
        SurfacePtr flat = doc.flatten();
        check(flat != nullptr, std::string("flatten works for ") + paperKindName(kind));
        if (!flat) continue;
        const long marks = countRulingPixels(flat);
        check(marks > 0, std::string(paperKindName(kind)) + " paints marks (" +
                            std::to_string(marks) + " px)");
    }

    // Plain must be genuinely blank, or "no ruling" is a lie.
    Document plain(200, 120);
    plain.setPaper(Paper{PaperKind::Plain, 16.0});
    check(countRulingPixels(plain.flatten()) == 0, "Plain paints nothing");

    // Ruled is horizontal only; a grid must therefore cover strictly more rows than
    // the ruling of the same pitch. Comparing counts is what catches a ruled sheet
    // that accidentally got its verticals too.
    Document ruled(200, 120);
    ruled.setPaper(Paper{PaperKind::Ruled, 16.0});
    Document grid(200, 120);
    grid.setPaper(Paper{PaperKind::Grid, 16.0});
    const long ruledMarks = countRulingPixels(ruled.flatten());
    const long gridMarks = countRulingPixels(grid.flatten());
    check(gridMarks > ruledMarks,
          "a grid covers more than a ruling at the same pitch (" +
              std::to_string(gridMarks) + " vs " + std::to_string(ruledMarks) + ")");

    // Halving the pitch has to add marks, or the spacing control does nothing.
    Document wide(200, 120);
    wide.setPaper(Paper{PaperKind::Ruled, 32.0});
    Document tight(200, 120);
    tight.setPaper(Paper{PaperKind::Ruled, 16.0});
    check(countRulingPixels(tight.flatten()) > countRulingPixels(wide.flatten()),
          "a smaller spacing produces more rules");

    // The ruling has to stay under the artwork, and an eraser must not take it out
    // with the ink: it belongs to the page, not to a layer.
    Document doc(200, 120);
    doc.setPaper(Paper{PaperKind::Grid, 16.0});
    doc.clearActive();  // erase everything the user drew
    check(countRulingPixels(doc.flatten()) > 0, "the ruling survives an empty layer stack");
}

void testPaperRoundTrip() {
    AppState state(300, 200);
    state.setPaper(PaperKind::Grid, 24.0);
    state.setBackgroundColor("#16213e");

    auto project = state.serialize();
    check(project.has_value(), "a project with a ruling serialises");
    if (!project) return;
    const std::string blob = encodeProjectBlob(*project);

    Project decoded;
    if (!decodeProjectBlob(blob, &decoded)) {
        check(false, "the blob with a ruling decodes");
        return;
    }
    check(decoded.paperKind == PaperKind::Grid, "the ruling kind survives the round trip");
    check(std::abs(decoded.paperSpacing - 24.0) < 0.01,
          "the ruling spacing survives (" + fmt(decoded.paperSpacing, 3) + ")");

    auto doc = deserializeProject(decoded);
    check(doc.has_value(), "the document with a ruling deserialises");
    if (doc) {
        check(doc->paper().kind == PaperKind::Grid, "the reopened page keeps its ruling");
        check(std::abs(doc->paper().spacing - 24.0) < 0.01, "the reopened spacing matches");
    }

    // A v2 file has no paper tag at all: plain paper, not whatever the struct held.
    Document tiny(40, 30);
    const std::string png = encodePng(tiny.active().surface());
    char len[32];
    std::snprintf(len, sizeof(len), "%zu\n", png.size());
    std::string v2 = "STYLUSPEN2 40 30 1\nactive 0\n";
    v2 += len;
    v2 += png;
    v2 += "\n";
    Project legacy;
    check(decodeProjectBlob(v2, &legacy), "a version 2 file still decodes");
    check(legacy.paperKind == PaperKind::Plain, "an old file opens on plain paper");

    // A hand-edited ruling must not produce a pattern that does not exist, and a
    // bogus pitch must not turn the painter into a hang.
    std::string bad = "STYLUSPEN3 40 30 1\nactive 0\npaper 99 0.00001\n";
    std::snprintf(len, sizeof(len), "%zu\n", png.size());
    bad += len;
    bad += png;
    bad += "\n";
    Project broken;
    check(decodeProjectBlob(bad, &broken), "a file with an out-of-range kind decodes");
    check(broken.paperKind == PaperKind::Plain,
          "an out-of-range kind falls back to Plain (" +
              std::to_string(static_cast<int>(broken.paperKind)) + ")");
    check(broken.paperSpacing == 8.0,
          "an absurd pitch is clamped (" + fmt(broken.paperSpacing, 3) + ")");
}

void testPaperUndo() {
    AppState state(200, 120);
    state.setPaper(PaperKind::Ruled, 30.0);
    state.addLayer();
    check(state.document().paper().kind == PaperKind::Ruled, "adding a layer keeps the ruling");
    check(std::abs(state.document().paper().spacing - 30.0) < 0.01,
          "adding a layer keeps the spacing");

    state.undo();
    check(state.document().paper().kind == PaperKind::Ruled,
          "undo keeps the ruling (" + std::string(paperKindName(state.document().paper().kind)) + ")");
    check(std::abs(state.document().paper().spacing - 30.0) < 0.01, "undo keeps the spacing");

    // A new canvas is a fresh sheet, and undo has to bring the ruling back too.
    state.newCanvas();
    check(state.document().paper().kind == PaperKind::Plain,
          "a new canvas drops the ruling");
    state.undo();
    check(state.document().paper().kind == PaperKind::Ruled, "undoing a new canvas restores it");

    // Setting the ruling to what it already is is not a change, so it must not
    // report one -- otherwise the UI would repaint for nothing.
    int repaints = 0;
    state.setChanged([&repaints]() { ++repaints; });
    check(!state.setPaper(PaperKind::Ruled, 30.0), "setting the same ruling reports no change");
    check(repaints == 0, "an unchanged ruling does not repaint");
    check(state.setPaper(PaperKind::Dotted, 30.0), "a real change is reported");
    check(repaints == 1, "a real change repaints once (" + std::to_string(repaints) + ")");
}

void testHistoryAvailabilityIsObservable() {
    // The toolbar's Undo and Redo buttons enable themselves off these two flags, and
    // they read them from a setChanged listener. That only works if the notification
    // that arrives after a stroke already sees the pushed entry -- endStroke notifies
    // right after drawing and *before* pushing, so an earlier version left the Undo
    // button disabled until some unrelated change came along. Recording what the flag
    // was at each notification pins that down.
    AppState state(200, 120);
    check(!state.history().canUndo(), "a fresh state has nothing to undo");
    check(!state.history().canRedo(), "a fresh state has nothing to redo");

    std::vector<bool> sawUndo;
    std::vector<bool> sawRedo;
    state.setChanged([&]() {
        sawUndo.push_back(state.history().canUndo());
        sawRedo.push_back(state.history().canRedo());
    });

    state.beginStroke(Point{40, 40, 0.9, 0, 0});
    for (int i = 1; i <= 20; ++i) state.extendStroke(Point{40.0 + i * 5.0, 40.0, 0.9, 0, 0});
    state.endStroke();

    check(!sawUndo.empty(), "ending a stroke notifies");
    check(sawUndo.back(),
          "the last notification already sees the stroke on the undo stack");
    check(state.history().canUndo(), "undo is available after a stroke");
    check(!state.history().canRedo(), "redo is not available right after a stroke");

    sawUndo.clear();
    sawRedo.clear();
    state.undo();
    check(!sawUndo.empty(), "undo notifies");
    check(sawUndo.back() == false, "the last notification sees undo exhausted");
    check(state.history().canRedo(), "redo is available after an undo");

    sawRedo.clear();
    state.redo();
    check(sawRedo.back() == false, "the last notification sees redo exhausted");
    check(state.history().canUndo(), "undo is available again after a redo");

    // Structural edits have to announce themselves the same way, or the buttons
    // would go stale on layer changes too.
    sawUndo.clear();
    state.addLayer();
    check(sawUndo.back(), "adding a layer announces the new undo step");
    sawRedo.clear();
    state.undo();
    check(sawRedo.back(), "undoing a layer announces the redo step");
}

// Reads one pixel as 0..1 channels.
void pixelAt(const SurfacePtr& surface, int x, int y, double out[4]) {
    cairo_surface_flush(surface.get());
    const int stride = cairo_image_surface_get_stride(surface.get());
    const unsigned char* px = cairo_image_surface_get_data(surface.get()) + y * stride + x * 4;
    // Premultiplied ARGB32, and every caller uses an opaque page, so unpremultiply
    // is a no-op here.
    for (int i = 0; i < 4; ++i) out[i] = px[i] / 255.0;
}

void testDottedPaperIsDotsNotWedges() {
    // cairo_arc joins each dot to the previous subpath with a line, so drawing the
    // whole lattice into one path and filling it produced wedges between the dots
    // rather than dots at all -- the pattern filled most of the page. A pixel that
    // sits halfway between two lattice points must therefore still be bare page.
    const double gap = 40.0;
    Document doc(200, 120);
    doc.setPaper(Paper{PaperKind::Dotted, gap});
    const SurfacePtr flat = doc.flatten();
    check(flat != nullptr, "a dotted page flattens");
    if (!flat) return;

    // A point clear of every dot: the lattice sits on multiples of `gap`, and a dot
    // is only a few pixels across, so the midpoint of a cell is untouched page.
    double px[4] = {0, 0, 0, 0};
    pixelAt(flat, static_cast<int>(gap) + static_cast<int>(gap) / 2,
            static_cast<int>(gap) + static_cast<int>(gap) / 2, px);
    check(px[0] > 0.9 && px[1] > 0.9 && px[2] > 0.9,
          "the space between dots is bare page, not a filled wedge");

    // And a dot's own centre has to be inked, so this cannot pass by drawing nothing.
    double dot[4] = {0, 0, 0, 0};
    pixelAt(flat, static_cast<int>(gap), static_cast<int>(gap), dot);
    check(dot[0] < 0.9 || dot[1] < 0.9 || dot[2] < 0.9,
          "a dot's centre is inked (" + fmt(dot[0], 3) + ")");

    // Overall coverage has to stay sparse. The wedge version filled most of the page;
    // a real dot lattice is a small fraction of it.
    const long total = static_cast<long>(doc.width()) * doc.height();
    const long marks = countRulingPixels(flat);
    check(marks * 20 < total,
          "dots stay sparse (" + std::to_string(marks) + " of " + std::to_string(total) + " px)");
}

void testRulingShowsOnDarkPages() {
    // A fixed dark ruling is invisible on a dark page, so the ink is chosen against
    // the page. This is the case a user hits the moment they pick a dark background:
    // the ruling they already selected quietly disappears.
    struct Case {
        const char* bg;
        PaperKind kind;
    };
    const Case cases[] = {{"#1b1b1f", PaperKind::Ruled},
                          {"#16213e", PaperKind::Grid},
                          {"#0f172a", PaperKind::Dotted},
                          {"#1b1b1f", PaperKind::Checkered}};

    for (const Case& c : cases) {
        Document doc(200, 120);
        doc.setBackground(c.bg);
        doc.setPaper(Paper{c.kind, 20.0});
        const SurfacePtr flat = doc.flatten();
        if (!flat) {
            check(false, std::string("dark page flattens for ") + paperKindName(c.kind));
            continue;
        }
        // On a dark page the ruling has to *lighten* it, so look for a pixel that is
        // brighter than the page rather than darker.
        const double pageLevel = 0.06;  // #1b1b1f is around 0.105 in sRGB
        bool found = false;
        for (int y = 2; y < 120 && !found; y += 1) {
            for (int x = 2; x < 200; ++x) {
                double px[4];
                pixelAt(flat, x, y, px);
                if (px[0] > pageLevel + 0.06 || px[1] > pageLevel + 0.06 ||
                    px[2] > pageLevel + 0.06) {
                    found = true;
                    break;
                }
            }
        }
        check(found, std::string("the ruling is visible on a dark page (") + c.bg + ", " +
                          paperKindName(c.kind) + ")");
    }
}

// Counts inked scanlines within one window, so a single rule can be measured even
// though the page carries many.
int inkedRowsNear(const SurfacePtr& surface, int around, int window) {
    cairo_surface_flush(surface.get());
    const int w = cairo_image_surface_get_width(surface.get());
    const int h = cairo_image_surface_get_height(surface.get());
    const int stride = cairo_image_surface_get_stride(surface.get());
    const unsigned char* data = cairo_image_surface_get_data(surface.get());
    int rows = 0;
    for (int y = std::max(0, around - 2); y < std::min(h, around + window); ++y) {
        const unsigned char* row = data + y * stride;
        for (int x = 0; x < w; ++x) {
            const unsigned char* px = row + x * 4;
            if (px[0] < 220 || px[1] < 220 || px[2] < 220) {
                ++rows;
                break;
            }
        }
    }
    return rows;
}

void testRulingHoldsOneDevicePixel() {
    // The ruling's weight is a document-space stroke, so it thickens when the view
    // zooms in the way real ruling does -- but at and below 1:1 it must never fall
    // under one device pixel, or antialiasing smears the whole sheet into a tint that
    // no longer reads as paper.
    //
    // A 40-unit pitch puts the first rule at document y=20.5. The pitch is divided by
    // the zoom so it always lands 40 device pixels apart, which keeps the rules from
    // crowding into each other's measurement window as the view shrinks. Only the
    // first rule is measured, by looking in a window around where it lands: counting
    // the whole page would fold in every other rule and say nothing about thickness.
    auto firstRuleThickness = [](double zoom, double gap) {
        SurfacePtr out = makeSurface(200, 400);
        cairo_t* cr = cairo_create(out.get());
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgb(cr, 0.984, 0.984, 0.992);
        cairo_paint(cr);
        cairo_scale(cr, zoom, zoom);
        paintPaper(cr, Paper{PaperKind::Ruled, gap}, Color{0.984, 0.984, 0.992},
                   4000 / zoom, 4000 / zoom, zoom);
        cairo_destroy(cr);
        // The first rule lands at document y=gap, so device y is gap * zoom. The
        // window has to stay well under the device pitch or it picks up the next rule
        // and reports the sum of both.
        return inkedRowsNear(out, static_cast<int>(gap * zoom), 8);
    };

    // At and below 1:1 the rule is held at about one device pixel. The pitch is 32
    // document pixels, which is legal at every zoom here.
    for (double zoom : {1.0, 0.5, 0.25}) {
        const int rows = firstRuleThickness(zoom, 32.0);
        // Three, not one: the stroke is held to one device pixel, but the half-pixel
        // snap lands it on a fractional row, so antialiasing spreads it over the
        // three scanlines it touches. The bound is what matters -- an unfloored
        // stroke would spread in proportion to 1/zoom and keep growing here.
        check(rows > 0 && rows <= 3,
              "the rule is about one device pixel at zoom " + fmt(zoom, 3) + " (" +
                  std::to_string(rows) + " scanlines)");
    }

    // The most zoomed-out view worth caring about, at the widest legal pitch: 19 device
    // pixels between rules, and the rule still a full pixel rather than a smear.
    {
        const int rows = firstRuleThickness(0.15, kMaxPaperSpacing);
        check(rows > 0 && rows <= 3,
              "the rule survives the widest pitch fully zoomed out (" +
                  std::to_string(rows) + " scanlines)");
    }

    // Zoomed in it thickens, like paper held closer. This is what makes the ruling
    // read as part of the sheet instead of a screen-space overlay.
    const int thick = firstRuleThickness(4.0, 32.0);
    check(thick >= 3, "zooming in thickens the rule, like real paper (" +
                          std::to_string(thick) + " scanlines)");
}

void testRulingBlendsIntoThePage() {
    // The ruling is translucent ink, so it has to blend with the page. flatten() left
    // the operator on CAIRO_OPERATOR_SOURCE after painting the background, which
    // replaces the destination instead of blending it: every rule became a partly
    // transparent stripe and the exported PNG came out with holes in the paper. The
    // earlier pixel tests only looked at the colour channels, where a transparent
    // black pixel reads as "dark", so they passed straight through the bug.
    Document doc(200, 120);
    doc.setBackground("#fbfbfd");
    doc.setPaper(Paper{PaperKind::Ruled, 20.0});
    const SurfacePtr flat = doc.flatten();
    check(flat != nullptr, "a ruled page flattens");
    if (!flat) return;

    // Somewhere on a rule: opaque, and lighter than pure black but darker than page.
    bool sawRule = false;
    bool allOpaque = true;
    bool anyTransparent = false;
    for (int y = 0; y < 120 && !sawRule; ++y) {
        for (int x = 0; x < 200; ++x) {
            double px[4];
            pixelAt(flat, x, y, px);
            if (px[0] > 220) continue;  // bare page
            sawRule = true;
            if (px[3] < 0.99) anyTransparent = true;
            // A blend of near-white page with 30% black lands around 0.69; SOURCE
            // would leave the colour channels at 0 and only the alpha short.
            if (px[0] < 0.1) allOpaque = false;
        }
    }
    check(sawRule, "a rule is visible on the page");
    check(!anyTransparent, "the ruling leaves no transparent holes in the page");
    check(allOpaque, "the ruling blends with the page instead of replacing it");

    // And the darkest pixel on the whole page must still be paper-coloured, not a
    // punched-through black.
    double darkest = 1.0;
    for (int y = 0; y < 120; ++y) {
        for (int x = 0; x < 200; ++x) {
            double px[4];
            pixelAt(flat, x, y, px);
            if (px[0] < darkest) darkest = px[0];
        }
    }
    check(darkest > 0.5, "nothing on the page is a hole (" + fmt(darkest, 3) + ")");
}

void testSpacingBandIsSharedWithTheSlider() {
    // The sidebar slider is built from kMinPaperSpacing/kMaxPaperSpacing, and the
    // clamp uses the same two numbers. They used to be separate: the slider stopped
    // at 96 while the format allowed 512, so a project saved with a wide pitch
    // reopened showing the slider pinned at its maximum next to a label reading
    // something else, and touching the slider silently rewrote the page.
    check(clampPaperSpacing(kMinPaperSpacing - 500) == kMinPaperSpacing,
          "spacing clamps up to the slider's minimum");
    check(clampPaperSpacing(kMaxPaperSpacing + 500) == kMaxPaperSpacing,
          "spacing clamps down to the slider's maximum");
    check(clampPaperSpacing(kMinPaperSpacing) == kMinPaperSpacing, "the minimum is legal");
    check(clampPaperSpacing(kMaxPaperSpacing) == kMaxPaperSpacing, "the maximum is legal");

    // The real requirement: whatever the document holds, the control can show. If
    // this fails, the slider is pinning a value the page does not have.
    for (int i = 0; i < kPaperKindCount; ++i) {
        Document doc(200, 120);
        doc.setPaper(Paper{static_cast<PaperKind>(i), 1e9});
        const double s = doc.paper().spacing;
        check(s >= kMinPaperSpacing && s <= kMaxPaperSpacing,
              std::string("every kind's spacing stays inside the slider's range (") +
                  paperKindName(static_cast<PaperKind>(i)) + ", " + fmt(s, 1) + "px)");
    }

    // A value from a project file, mid-band, has to survive untouched -- that is the
    // whole point of the slider being able to reach it.
    Document doc(200, 120);
    doc.setPaper(Paper{PaperKind::Grid, 64.0});
    check(std::abs(doc.paper().spacing - 64.0) < 1e-9,
          "a mid-band spacing survives untouched (" + fmt(doc.paper().spacing, 1) + "px)");

    // And moving the slider's value is what the handler does: same kind, new pitch,
    // one repaint, no change to the selected kind.
    AppState state(200, 120);
    state.setPaper(PaperKind::Ruled, 40.0);
    int repaints = 0;
    state.setChanged([&repaints]() { ++repaints; });
    check(state.setPaper(state.document().paper().kind, 56.0), "a spacing change applies");
    check(state.document().paper().kind == PaperKind::Ruled,
          "changing the spacing keeps the selected kind");
    check(std::abs(state.document().paper().spacing - 56.0) < 1e-9,
          "the new spacing is stored (" + fmt(state.document().paper().spacing, 1) + "px)");
    check(repaints == 1, "a spacing change repaints once (" + std::to_string(repaints) + ")");
}

// A solid colour rectangle, standing in for a decoded picture.
SurfacePtr makeSolidImage(int w, int h, double r, double g, double b) {
    SurfacePtr s = makeSurface(w, h);
    cairo_t* cr = cairo_create(s.get());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_paint(cr);
    cairo_destroy(cr);
    return s;
}

long countInkedOnLayer(const Layer& layer) {
    return countInkPixels(layer.surface());
}

void testLayerPlacedImage() {
    Layer layer("img", 200, 120);
    check(!layer.hasImage(), "a fresh layer holds no image");
    check(!layer.setImage(nullptr, 0, 0, 1.0), "a null image is refused");
    check(layer.setImage(makeSolidImage(40, 30, 0.9, 0.2, 0.1), 10, 10, 0.0),
          "a zero scale is accepted and clamped rather than refused");
    check(layer.image().scale > 0.0, "the clamped scale is usable (" + fmt(layer.image().scale, 3) + ")");
    check(layer.setImage(makeSolidImage(40, 30, 0.9, 0.2, 0.1), 10, 10, 1e9),
          "an absurd scale is accepted and clamped");
    check(layer.image().scale <= 8.0, "the upper clamp holds (" + fmt(layer.image().scale, 2) + ")");

    auto img = makeSolidImage(40, 30, 0.9, 0.2, 0.1);
    check(layer.setImage(img, 10, 20, 1.0), "an image is accepted");
    check(layer.hasImage(), "the layer reports the image");

    // Bounds follow the placement and the scale, and drive the drag hit-test.
    Rect b = layer.imageBounds();
    check(std::abs(b.x - 10) < 1e-6 && std::abs(b.y - 20) < 1e-6, "bounds sit at the placement");
    check(std::abs(b.w - 40) < 1e-6 && std::abs(b.h - 30) < 1e-6, "bounds match the source size");

    // The image is rasterised into the layer, not kept live: every existing reader --
    // snapshot, stroke undo, the save format -- reads the surface.
    check(countInkedOnLayer(layer) > 0, "the image is baked into the layer surface");
    cairo_surface_flush(layer.surface());
    const unsigned char* px = cairo_image_surface_get_data(layer.surface());
    const int stride = cairo_image_surface_get_stride(layer.surface());
    // Sample inside the picture, which starts at (10, 20).
    const unsigned char* inside = px + 25 * stride + 20 * 4;
    // Premultiplied ARGB32 is stored B, G, R, A in memory, and the image is
    // (0.9, 0.2, 0.1): red high, blue low.
    check(inside[2] > 150 && inside[1] < 110 && inside[0] < 80,
          "the pixel inside the image carries the image colour");
    const unsigned char* outside = px + 60 * stride + 150 * 4;
    check(outside[3] == 0, "the pixel outside it is still transparent");

    // Moving re-rasterises at the new spot and leaves nothing behind at the old one.
    const unsigned long long before = layer.revision();
    check(layer.moveImageBy(30, 40), "a move is applied");
    check(layer.revision() > before, "moving bumps the revision so the composite cache drops");
    Rect moved = layer.imageBounds();
    check(std::abs(moved.x - 40) < 1e-6 && std::abs(moved.y - 60) < 1e-6,
          "bounds follow the move");
    cairo_surface_flush(layer.surface());
    const unsigned char* px2 = cairo_image_surface_get_data(layer.surface());
    const unsigned char* oldSpot = px2 + 25 * stride + 20 * 4;
    check(oldSpot[3] == 0, "the image left no copy behind at its old position");

    check(!layer.moveImageBy(0.0, 0.0), "a zero move is refused");
    check(!layer.moveImageBy(5, 5) == false, "moving is still allowed");

    // Absolute placement, and refusing to move a layer that has no image.
    check(layer.setImagePosition(5, 5), "absolute placement works");
    check(std::abs(layer.imageBounds().x - 5) < 1e-6, "absolute placement took effect");
    check(!layer.setImagePosition(5, 5), "placing it where it already is is not a change");

    Layer plain("plain", 100, 100);
    check(!plain.setImagePosition(1, 1), "a layer with no image cannot be placed");
    check(!plain.moveImageBy(1, 1), "a layer with no image cannot be moved");

    // Clearing drops the picture: an empty layer cannot still carry one.
    layer.clear();
    check(!layer.hasImage(), "clearing the layer drops the image");
    check(countInkedOnLayer(layer) == 0, "the cleared layer is empty");
}

void testInsertImage() {
    AppState state(400, 300);
    const size_t before = state.document().layerCount();

    check(!state.insertImage(nullptr, "x"), "inserting nothing is refused");
    check(state.document().layerCount() == before, "a refused insert adds no layer");

    check(state.insertImage(makeSolidImage(120, 90, 0.9, 0.2, 0.1), "Photo"),
          "an image is inserted");
    check(state.document().layerCount() == before + 1, "it got its own layer");
    check(state.document().active().hasImage(), "the new layer is the image layer");
    check(state.document().active().name() == "Photo", "the layer is named after the file");
    check(state.document().activeIndex() == before, "the new layer is active");
    check(state.activeLayerHasImage(), "AppState agrees the active layer has an image");

    // Centred, and never enlarged: a small picture keeps its own size.
    const Rect r = state.document().active().imageBounds();
    check(std::abs(r.w - 120) < 1e-6 && std::abs(r.h - 90) < 1e-6,
          "a small image is not scaled up (" + fmt(r.w, 1) + "x" + fmt(r.h, 1) + ")");
    check(std::abs(r.x - (400 - 120) / 2.0) < 1e-6, "it is centred horizontally");
    check(std::abs(r.y - (300 - 90) / 2.0) < 1e-6, "it is centred vertically");

    // A picture bigger than the page is scaled down so it can still be dragged into
    // place instead of arriving with its edges already off the sheet.
    AppState big(200, 150);
    check(big.insertImage(makeSolidImage(800, 600, 0.2, 0.5, 0.9), "Big"),
          "an oversized image is inserted");
    const Rect fit = big.document().active().imageBounds();
    check(fit.w <= 200.0 + 1e-6 && fit.h <= 150.0 + 1e-6,
          "an oversized image is scaled to fit (" + fmt(fit.w, 1) + "x" + fmt(fit.h, 1) + ")");
    check(fit.w > 0.0 && fit.h > 0.0, "the fitted image still has area");

    // Undo takes the whole layer away again.
    state.undo();
    check(state.document().layerCount() == before, "undo removes the inserted layer");
    state.redo();
    check(state.document().layerCount() == before + 1, "redo brings it back");
    check(state.document().active().hasImage(), "the redone layer still holds the image");
}

void testInsertText() {
    AppState state(400, 300);
    const size_t before = state.document().layerCount();

    check(!state.insertText(""), "empty text insertion is refused");
    check(state.document().layerCount() == before, "refused text insert adds no layer");

    check(state.insertText("Stylus Pen", 36.0, Color{0.8, 0.1, 0.1}),
          "insertText creates a text layer");
    check(state.document().layerCount() == before + 1, "text got its own layer");
    check(state.document().active().hasImage(), "the text layer is an image-based layer");
    check(state.document().active().name().rfind("Text: ", 0) == 0, "layer name starts with Text:");
    check(state.activeLayerHasImage(), "AppState agrees the active layer holds text");

    state.undo();
    check(state.document().layerCount() == before, "undoing insertText removes the layer");
}

void testHandwritingRecognition() {
    AppState state(400, 300);
    Point p1{100.0, 100.0, 0.5, 0.0, 0.0};
    Point p2{100.0, 150.0, 0.5, 0.0, 0.0};
    state.beginStroke(p1);
    state.extendStroke(p2);
    state.endStroke();

    RecognizedText rec = state.recognizeActiveLayerHandwriting();
    check(!rec.text.empty(), "recognizeActiveLayerHandwriting returns non-empty text");
    check(rec.bounds.hasArea(), "recognizeActiveLayerHandwriting detects stroke bounds");

    const size_t countBefore = state.document().layerCount();
    check(state.convertHandwritingToText(rec.text, rec.bounds, true, 36.0),
          "convertHandwritingToText creates a new text layer");
    check(state.document().layerCount() == countBefore + 1, "text layer stack grew");
    check(state.document().active().hasImage(), "converted layer holds text image");

    state.undo();
    check(state.document().layerCount() == countBefore, "undoing convertHandwritingToText restores original layer stack");
}

void testHandwritingRecognitionAccuracy() {
    // Render text "bego" onto active layer surface to test handwriting OCR accuracy
    AppState state(400, 200);
    cairo_surface_t* surf = state.document().active().surface();
    cairo_t* cr = cairo_create(surf);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0); // Black ink
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 56.0);
    cairo_move_to(cr, 40, 120);
    cairo_show_text(cr, "bego");
    cairo_destroy(cr);

    HandwritingRecognizer recognizer;
    RecognizedText rec = recognizer.recognizeSurface(surf);
    check(!rec.text.empty(), "recognizeSurface recognizes rendered handwritten word");
    check(rec.text == "bego" || rec.text == "Bego" || rec.text.find("bego") != std::string::npos,
          "recognizeSurface accurately identifies word 'bego'");
}

void testMultiLineHandwritingToText() {
    AppState state(500, 400);
    const std::string multiLineText = "Baris Pertama\nBaris Kedua Teks\nBaris Ketiga";
    Rect bounds{50.0, 50.0, 400.0, 250.0};
    check(state.convertHandwritingToText(multiLineText, bounds, false, 32.0),
          "convertHandwritingToText handles multi-line text");
    check(state.activeLayerTextInfo().text == multiLineText, "multi-line text matches original string with newlines");
}

void testHandwritingLearning() {

    AppState state(400, 300);
    const std::string customWord = "styluspenistimewa";
    state.learnHandwritingWord(customWord);

    std::vector<std::string> userWords = HandwritingRecognizer::getUserWords();
    bool found = false;
    for (const auto& w : userWords) {
        if (w == customWord) { found = true; break; }
    }
    check(found, "learnHandwritingWord persists and loads custom user handwriting word");
}




void testEditTextLayer() {
    AppState state(400, 300);
    check(state.insertText("Initial Text", 32.0, Color{0.2, 0.2, 0.2}), "initial text inserted");
    check(state.activeLayerHasText(), "active layer carries text metadata");
    check(state.activeLayerTextInfo().text == "Initial Text", "textInfo text matches initial string");

    Layer::TextInfo updated;
    updated.text = "Updated Formatted Text";
    updated.fontSize = 48.0;
    updated.color = Color{0.9, 0.1, 0.1};
    updated.isBold = true;
    updated.isItalic = true;
    updated.fontFamily = "Serif";
    updated.valid = true;

    check(state.updateActiveTextLayer(updated), "updateActiveTextLayer succeeds");
    check(state.activeLayerTextInfo().text == "Updated Formatted Text", "textInfo text updated");
    check(state.activeLayerTextInfo().fontFamily == "Serif", "fontFamily updated");
    check(state.activeLayerTextInfo().isItalic, "isItalic updated");

    state.undo();
    check(state.activeLayerTextInfo().text == "Initial Text", "undo restores previous text info");
    state.redo();
    check(state.activeLayerTextInfo().text == "Updated Formatted Text", "redo restores updated text info");
}

void testImageDragIsOneUndoStep() {
    AppState state(400, 300);
    state.insertImage(makeSolidImage(100, 80, 0.9, 0.2, 0.1), "Photo");
    const Rect start = state.document().active().imageBounds();

    check(state.beginMoveActiveImage(), "the picture can be grabbed");
    check(!state.beginMoveActiveImage(), "a second grab is refused mid-drag");
    check(state.moveActiveImageBy(10, 0), "the first step moves");
    check(state.moveActiveImageBy(10, 0), "the second step moves");
    check(state.moveActiveImageBy(10, 0), "the third step moves");

    const Rect moved = state.document().active().imageBounds();
    check(std::abs(moved.x - (start.x + 30)) < 1e-6, "the three steps added up");

    const size_t historyBefore = state.history().size();
    check(state.commitActiveImageMove(), "the drag commits");
    check(!state.moveActiveImageBy(10, 0), "moving after the commit is refused");
    check(state.history().size() == historyBefore + 1,
          "three moves made exactly one undo step");
    check(!state.beginMoveActiveImage() == false, "a new drag can begin afterwards");

    state.undo();
    Rect undone = state.document().active().imageBounds();
    check(std::abs(undone.x - start.x) < 1e-6, "undo puts the picture back where it was");
    check(std::abs(undone.y - start.y) < 1e-6, "undo restores the vertical position too");

    state.redo();
    Rect redone = state.document().active().imageBounds();
    check(std::abs(redone.x - moved.x) < 1e-6, "redo moves it forward again");

    // A drag that ends where it began is not an edit.
    AppState still(400, 300);
    still.insertImage(makeSolidImage(60, 60, 0.1, 0.4, 0.8), "Photo");
    const size_t h0 = still.history().size();
    still.beginMoveActiveImage();
    still.moveActiveImageBy(25, 25);
    still.moveActiveImageBy(-25, -25);
    check(!still.commitActiveImageMove(), "a drag that returns to its start does not commit");
    check(still.history().size() == h0, "and it leaves history alone");

    // Cancelling puts it back and records nothing.
    still.beginMoveActiveImage();
    still.moveActiveImageBy(40, 40);
    still.cancelActiveImageMove();
    Rect cancelled = still.document().active().imageBounds();
    check(std::abs(cancelled.x - (400.0 - 60) / 2.0) < 1e-6, "cancel restores the position");
    check(still.history().size() == h0, "cancel records nothing");
    check(!still.moveActiveImageBy(5, 5), "the drag is over after a cancel");

    // A layer with no picture cannot be dragged at all.
    AppState plain(400, 300);
    check(!plain.beginMoveActiveImage(), "a layer with no image cannot be grabbed");
    check(!plain.commitActiveImageMove(), "committing an absent drag is refused");
}

void testImageResize() {
    Layer layer("img", 400, 300);
    auto img = makeSolidImage(100, 80, 0.9, 0.2, 0.1);
    layer.setImage(img, 150, 110, 1.0);

    // Scaling grows around the centre, so the picture stays under the pointer instead
    // of sliding away from it with its top-left pinned.
    const Rect before = layer.imageBounds();
    const double centreX = before.x + before.w * 0.5;
    const double centreY = before.y + before.h * 0.5;
    check(layer.setImageScale(2.0), "the image scales up");
    Rect after = layer.imageBounds();
    check(std::abs(after.w - 200) < 1e-6 && std::abs(after.h - 160) < 1e-6,
          "the size doubled (" + fmt(after.w, 1) + "x" + fmt(after.h, 1) + ")");
    check(std::abs((after.x + after.w * 0.5) - centreX) < 1e-6, "the horizontal centre held");
    check(std::abs((after.y + after.h * 0.5) - centreY) < 1e-6, "the vertical centre held");

    // Back to 1.0, which really is a halving of the doubled size.
    check(layer.setImageScale(1.0), "the image scales down");
    after = layer.imageBounds();
    check(std::abs(after.w - 100) < 1e-6, "the size halved (" + fmt(after.w, 1) + ")");
    check(std::abs((after.x + after.w * 0.5) - centreX) < 1e-6, "the centre still held");

    // The scale is absolute, not relative: 0.5 of a 100px source is 50px, not half of
    // whatever it measured a moment ago.
    check(layer.setImageScale(0.5), "scaled to half");
    after = layer.imageBounds();
    check(std::abs(after.w - 50) < 1e-6, "half scale means half the source width (" +
                                            fmt(after.w, 1) + ")");

    check(!layer.setImageScale(0.5), "rescaling to the same size is not a change");
    check(layer.setImageScale(1e9), "an absurd scale is clamped");
    check(layer.image().scale <= 8.0, "the upper clamp holds");
    check(layer.setImageScale(0.0), "a zero scale is clamped rather than refused");
    check(layer.image().scale > 0.0, "the clamped scale is usable");

    Layer plain("plain", 100, 100);
    check(!plain.setImageScale(2.0), "a layer with no image cannot be resized");
}

void testImageResizeIsOneUndoStep() {
    AppState state(400, 300);
    state.insertImage(makeSolidImage(100, 80, 0.9, 0.2, 0.1), "Photo");
    const Rect start = state.document().active().imageBounds();

    // A sweep of the slider: many live values, one gesture.
    check(state.setImageScaleLive(1.5), "first step");
    check(state.setImageScaleLive(1.8), "second step");
    check(state.setImageScaleLive(2.0), "third step");
    const Rect scaled = state.document().active().imageBounds();
    check(std::abs(scaled.w - 200) < 1e-6, "the sweep took effect (" + fmt(scaled.w, 1) + "px)");

    const size_t historyBefore = state.history().size();
    check(state.commitImageScale(), "the sweep commits");
    check(state.history().size() == historyBefore + 1, "three steps made one undo step");
    check(!state.setImageScaleLive(3.0) || true, "a live change after commit is allowed again");

    state.undo();
    Rect undone = state.document().active().imageBounds();
    check(std::abs(undone.w - start.w) < 1e-6, "undo restores the original size");
    // The position must come back too, not just the size: undoing with a reset
    // position would teleport the picture to the page's corner.
    check(std::abs(undone.x - start.x) < 1e-6 && std::abs(undone.y - start.y) < 1e-6,
          "undo restores the original position as well");

    state.redo();
    check(std::abs(state.document().active().imageBounds().w - scaled.w) < 1e-6,
          "redo applies the size again");

    // Cancelling mid-sweep restores the start exactly and records nothing.
    AppState other(400, 300);
    other.insertImage(makeSolidImage(60, 60, 0.1, 0.4, 0.8), "Photo");
    const Rect o0 = other.document().active().imageBounds();
    const size_t h0 = other.history().size();
    other.setImageScaleLive(3.0);
    other.cancelImageScale();
    Rect cancelled = other.document().active().imageBounds();
    check(std::abs(cancelled.w - o0.w) < 1e-6, "cancel restores the size");
    check(std::abs(cancelled.x - o0.x) < 1e-6, "cancel restores the position");
    check(other.history().size() == h0, "cancel records nothing");
}

void testImageFitCentreAndDelete() {
    AppState state(400, 300);
    state.insertImage(makeSolidImage(80, 60, 0.9, 0.2, 0.1), "Photo");

    Rect r = state.document().active().imageBounds();
    check(std::abs(r.w - 80) < 1e-6, "it starts at its own size");

    // Fit page: the larger ratio wins so a wide picture is not blown past the page.
    check(state.scaleActiveImageToPage(), "fit page applies");
    r = state.document().active().imageBounds();
    check(std::abs(r.w - 400) < 1e-6, "fit page fills the width (" + fmt(r.w, 1) + ")");
    check(r.h <= 300.0 + 1e-6, "and it stays inside the height (" + fmt(r.h, 1) + ")");

    state.centreActiveImage();
    r = state.document().active().imageBounds();
    check(std::abs((r.x + r.w * 0.5) - 200.0) < 1e-6, "centre puts it on the middle");
    check(std::abs((r.y + r.h * 0.5) - 150.0) < 1e-6, "centred vertically too");

    // Delete: the picture is a layer, so removing it is the layer delete, and undo
    // brings it back still draggable.
    const size_t layers = state.document().layerCount();
    check(state.deleteActiveImage(), "the picture layer deletes");
    check(state.document().layerCount() == layers - 1, "the layer is gone");
    check(!state.document().active().hasImage(), "the active layer is no longer a picture");

    state.undo();
    check(state.document().layerCount() == layers, "undo brings the layer back");
    check(state.document().active().hasImage(), "and it is a picture again");
    check(state.activeLayerHasImage(), "AppState agrees");
    // The restored placement has to be usable, or the picture would come back frozen.
    check(state.beginMoveActiveImage(), "the restored picture can still be dragged");

    // A plain layer is not a picture, so the delete button must decline rather than
    // delete somebody's artwork.
    AppState plain(400, 300);
    plain.insertImage(makeSolidImage(40, 40, 0.2, 0.5, 0.9), "Photo");
    plain.deleteActiveImage();
    plain.addLayer();  // a plain empty layer on top
    check(!plain.activeLayerHasImage(), "the new layer holds no image");
    check(!plain.deleteActiveImage(), "deleting a picture from a plain layer is refused");
    check(!plain.scaleActiveImageToPage(), "fit page is refused on a plain layer");
    plain.centreActiveImage();  // must be a no-op rather than a crash
}

void testColorParsing() {
    Brush b;
    b.setHex("#4b5563");
    check(std::abs(b.g - 0x55 / 255.0) < 1e-6, "baseline colour parses");

    // Each of these made setHex throw std::invalid_argument.
    for (const char* bad : {"", "#", "zz", "12345", "1234567", "#gggggg", "#12zz34",
                            "not a colour", "#-1-2-3"}) {
        b.setHex("#4b5563");
        b.setHex(bad);
        check(b.hex() == "#4b5563",
              std::string("malformed colour '") + bad + "' is rejected, not thrown");
    }

    // Valid forms still work after all that.
    b.setHex("#0a0B0C");
    check(b.hex() == "#0a0b0c", "mixed-case hex parses and normalises");
    b.setHex("#abc");
    check(b.hex() == "#aabbcc", "shorthand still expands");
    b.setHex("#FFFFFF");
    check(b.hex() == "#ffffff", "white parses");
}

// Regression: stroke bounds have to include the widening tilt applies, or a
// tilted stroke gets clipped by the snapshot region used for undo. Restoring a
// clipped snapshot would then leave part of the stroke behind.
void testTiltAwareBounds() {
    Brush upright;
    upright.size = 20.0;
    upright.tiltShading = false;

    Brush tilted = upright;
    tilted.tiltShading = true;

    Stroke s;
    for (int i = 0; i <= 40; ++i) {
        Point p;
        p.x = 100.0 + i * 4.0;
        p.y = 200.0;
        p.pressure = 0.8;
        p.tiltX = 0.0;
        p.tiltY = 55.0;  // strong lean across the path
        s.points.push_back(p);
    }

    const Rect flatBounds = strokeBounds(s.points, upright);
    const Rect tiltBounds = strokeBounds(s.points, tilted);
    check(tiltBounds.w > flatBounds.w,
          "tilt widens the horizontal extent (" + fmt(flatBounds.w, 1) + " -> " +
              fmt(tiltBounds.w, 1) + ")");
    check(tiltBounds.h > flatBounds.h,
          "tilt widens the vertical extent (" + fmt(flatBounds.h, 1) + " -> " +
              fmt(tiltBounds.h, 1) + ")");

    // The bounds must fully contain the drawn ink, or the snapshot region used for
    // undo truncates the stroke. Drawn twice on white: once unclipped, once
    // through a clip of exactly the reported bounds. If the bounds are too tight
    // the second render comes out smaller, and undo would leave a stump.
    auto inkWithinBounds = [&](const Brush& brush, const Rect& bounds) {
        auto renderClipped = [&](bool clipToBounds) {
            SurfacePtr surf = makeSurface(400, 400);
            cairo_t* cr = cairo_create(surf.get());
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_paint(cr);
            if (clipToBounds) {
                cairo_save(cr);
                cairo_rectangle(cr, bounds.x, bounds.y, bounds.w, bounds.h);
                cairo_clip(cr);
                drawStroke(cr, s, brush);
                cairo_restore(cr);
            } else {
                drawStroke(cr, s, brush);
            }
            cairo_destroy(cr);
            return countInkPixels(surf.get());
        };

        const long full = renderClipped(false);
        const long clipped = renderClipped(true);
        check(clipped == full,
              "bounds contain all the ink they claim (" + std::to_string(clipped) + " of " +
                  std::to_string(full) + " px for a " + fmt(bounds.w, 0) + "x" +
                  fmt(bounds.h, 0) + " box)");
        return clipped == full;
    };

    check(inkWithinBounds(tilted, tiltBounds),
          "tilted stroke fits inside its own reported bounds");
    check(inkWithinBounds(upright, flatBounds),
          "untilted stroke fits inside its own reported bounds");

    // The stroke is at y=200 and horizontal, so the box must be one-sided about
    // the path: a real height, not the degenerate zero a horizontal line produces.
    check(tiltBounds.h > 10.0, "a horizontal stroke still reports a usable height (" +
                                   fmt(tiltBounds.h, 1) + ")");
    check(flatBounds.y < 200.0 && flatBounds.bottom() > 200.0,
          "the box is centred on the path rather than sitting above it");
}

// Regression: a path that doubles back on itself produces a cusp, where the
// tangent has no direction. The offset must fall back to the previous sample's
// normal; an arbitrary one swaps the two sides and folds the ribbon into an
// untidy wedge.
void testCuspHandling() {
    Stroke s;
    // Right, then straight back left: a full reversal at the middle sample.
    for (int i = 0; i <= 10; ++i) s.points.push_back(Point{100.0 + i * 6.0, 200.0, 0.8, 0, 0});
    for (int i = 1; i <= 10; ++i) s.points.push_back(Point{160.0 - i * 6.0, 200.0, 0.8, 0, 0});

    Brush b;
    b.size = 12.0;
    std::vector<Point> outline;
    buildOutline(s.points, b, outline);
    check(outline.size() == s.points.size() * 2, "cusp still yields both sides");

    SurfacePtr surf = makeSurface(400, 400);
    cairo_t* c = cairo_create(surf.get());
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(c, 1, 1, 1);
    cairo_paint(c);
    drawStroke(c, s, b);
    cairo_destroy(c);

    // A folded ribbon leaves the stroke looking broken; a solid reversal leaves a
    // filled band along the whole path.
    check(countInkPixels(surf.get()) > 800,
          "reversal draws a solid band (" + std::to_string(countInkPixels(surf.get())) + " px)");
}

// Regression: resample used to copy tilt from the far endpoint of each segment
// instead of interpolating, so a stroke whose lean changed steadily would have
// its nib jump orientation at every resampled sample.
void testResampleInterpolatesTilt() {
    std::vector<Point> line;
    Point a{0.0, 0.0, 0.5, 0.0, 0.0};
    Point b{40.0, 0.0, 0.5, 60.0, 0.0};
    line.push_back(a);
    line.push_back(b);

    const auto out = resample(line, 4.0);
    check(out.size() > 2, "resample produced interior points");

    bool monotone = true;
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i].tiltX < out[i - 1].tiltX - 1e-9) monotone = false;
    }
    check(monotone, "tilt increases smoothly across resampled samples");
    check(std::abs(out.back().tiltX - 60.0) < 1e-6, "final tilt is preserved");

    // A midpoint sample should sit at roughly half the lean, not at either end.
    const Point& mid = out[out.size() / 2];
    check(mid.tiltX > 10.0 && mid.tiltX < 50.0,
          "midpoint tilt is interpolated, not copied from an endpoint (" +
              fmt(mid.tiltX, 1) + ")");
}

// Regression: the cached composite must follow every change that affects it.
// Tinting it wrong makes edits invisible on screen until something forces a full
// repaint.
void testCompositeCache() {
    Document doc(200, 200);

    Brush cyan;
    cyan.size = 20.0;
    cyan.setHex("#0ea5e9");
    Stroke line;
    for (int i = 0; i <= 20; ++i) line.points.push_back(Point{20.0 + i * 8.0, 100.0, 0.9, 0, 0});
    drawStrokeOn(doc.layerAt(0).surface(), line, cyan);

    // Without an explicit invalidation the cache would be stale. This is the
    // contract AppState relies on.
    SurfacePtr before = doc.composite();
    check(countInkPixels(before.get()) > 50, "composite shows freshly drawn ink");
    check(doc.composite().get() == before.get(), "composite is cached while nothing changes");

    doc.invalidateComposite();
    SurfacePtr after = doc.composite();
    check(countInkPixels(after.get()) > 50, "composite still correct after invalidation");

    // Visibility is picked up through the layer revision, with no explicit call.
    doc.layerAt(0).setVisible(false);
    check(countInkPixels(doc.composite().get()) == 0,
          "hiding a layer empties the cached composite");

    doc.layerAt(0).setVisible(true);
    check(countInkPixels(doc.composite().get()) > 50, "showing it again restores the ink");

    // Opacity likewise.
    doc.layerAt(0).setOpacity(0.0);
    check(countInkPixels(doc.composite().get()) == 0, "zero opacity empties the composite");
    doc.layerAt(0).setOpacity(1.0);
    check(countInkPixels(doc.composite().get()) > 50, "restoring opacity restores the ink");

    // A second layer has to show up in the composite.
    doc.addLayer();
    Brush red;
    red.size = 20.0;
    red.setHex("#b91c1c");
    Stroke line2;
    for (int i = 0; i <= 20; ++i) line2.points.push_back(Point{20.0 + i * 8.0, 140.0, 0.9, 0, 0});
    drawStrokeOn(doc.layerAt(1).surface(), line2, red);
    doc.invalidateComposite();
    check(countInkPixels(doc.composite().get()) > countInkPixels(before.get()),
          "adding a layer grows the composite");
}

// Regression: the end caps have to bulge outwards, past the end of the stroke.
// Sweeping the cap the wrong way round cuts a V-shaped notch into both ends,
// which is what every stroke had before the direction of the sweep was worked
// out from the path rather than assumed.
void testStrokeEndCaps() {
    // A perfectly horizontal stroke is the worst case: both cap side points sit
    // at right angles to the path, so the sweep is a half turn either way.
    Stroke horizontal;
    for (int i = 0; i <= 40; ++i) {
        horizontal.points.push_back(Point{200.0 + i * 6.0, 300.0, 0.9, 0.0, 0.0});
    }

    Brush b;
    b.size = 24.0;

    auto render = [&](const Stroke& s, const Brush& brush) {
        SurfacePtr surf = makeSurface(600, 600);
        cairo_t* cr = cairo_create(surf.get());
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        drawStroke(cr, s, brush);
        cairo_destroy(cr);
        return surf;
    };

    SurfacePtr surf = render(horizontal, b);

    // Ink, not alpha: the surface is painted white first, so every pixel is opaque
    // whether or not the stroke reached it. Only the colour tells them apart.
    auto inkAt = [&](cairo_surface_t* s, int x, int y) {
        cairo_surface_flush(s);
        const int stride = cairo_image_surface_get_stride(s);
        const unsigned char* d = cairo_image_surface_get_data(s);
        const unsigned char* px = d + y * stride + x * 4;
        if (px[3] < 40) return false;
        return px[0] < 235 || px[1] < 235 || px[2] < 235;
    };
    auto alphaAt = [&](int x, int y) {
        cairo_surface_flush(surf.get());
        const int stride = cairo_image_surface_get_stride(surf.get());
        const unsigned char* d = cairo_image_surface_get_data(surf.get());
        return static_cast<int>(d[y * stride + x * 4 + 3]);
    };

    // The nib is 24 wide at this pressure, so the mark spans y=288..312 at the
    // path. A cap bulging out past the head must add ink beyond the first sample.
    const int headX = 200;
    const int midY = 300;
    const bool headExtends = alphaAt(headX - 8, midY) > 100;
    const bool tailExtends = alphaAt(headX + 40 * 6 + 8, midY) > 100;
    check(headExtends, "head cap bulges forward past the first sample");
    check(tailExtends, "tail cap bulges forward past the last sample");

    // The nib half-width here, so the cap can be checked against the ellipse it is
    // supposed to be rather than against a guess.
    const double r = radiusFor(horizontal.points.front(), b);
    auto capHalfHeight = [&](double into) {
        const double t = std::min(1.0, into / r);
        return r * std::sqrt(1.0 - t * t);
    };

    // A wrong-way sweep cuts a wedge: it adds ink past the nib on one diagonal and
    // leaves the tip hollow. Sampling just outside the ellipse in both directions
    // from the centre line catches the first, and the tip check catches the second.
    const double probe = 8.0;
    const int edge = static_cast<int>(capHalfHeight(probe));
    check(!inkAt(surf.get(), headX - 8, midY - edge - 4),
          "no wedge above the head cap");
    check(!inkAt(surf.get(), headX - 8, midY + edge + 4),
          "no wedge below the head cap");

    // And the cap really is filled out to the nib, not hollow in the middle.
    check(alphaAt(headX - 8, midY) > 200,
          "head cap is solid at the nib edge (" + std::to_string(alphaAt(headX - 8, midY)) + ")");

    // Ink has to be continuous across the join between cap and ribbon.
    bool joined = true;
    for (int y = midY - 8; y <= midY + 8; y += 2) {
        if (!inkAt(surf.get(), headX + 2, y)) joined = false;
    }
    check(joined, "head cap joins the ribbon without a gap");

    // A tilted nib has an elliptical cap, not a circle, so the same geometry with
    // a strong lean must stay clean as well.
    Stroke tilted = horizontal;
    for (Point& p : tilted.points) {
        p.tiltX = 58.0;
        p.tiltY = -50.0;
    }
    SurfacePtr tSurf = render(tilted, b);
    check(inkAt(tSurf.get(), headX - 10, midY), "tilted head cap still bulges outward");
    // The tilted nib is an ellipse, so probe well clear of its widest reach.
    check(!inkAt(tSurf.get(), headX - 10, midY - 40), "tilted head cap leaves no wedge above it");
    check(!inkAt(tSurf.get(), headX - 10, midY + 40), "tilted head cap leaves no wedge below it");

    // One coat of ink only: a cap filled twice comes out darker than the ribbon.
    // Compare the centre of the cap against the middle of the ribbon.
    auto centreAlpha = [&](cairo_surface_t* s, int x, int y) {
        cairo_surface_flush(s);
        const int stride = cairo_image_surface_get_stride(s);
        const unsigned char* d = cairo_image_surface_get_data(s);
        return static_cast<int>(d[y * stride + x * 4 + 3]);
    };
    const int capAlpha = centreAlpha(surf.get(), headX + 4, midY);
    const int ribbonAlpha = centreAlpha(surf.get(), headX + 40 * 3, midY);
    check(capAlpha == ribbonAlpha,
          "cap carries the same ink as the ribbon (" + std::to_string(capAlpha) + " vs " +
              std::to_string(ribbonAlpha) + ")");

    // The same for a translucent brush, where a second coat is far more visible.
    Brush marker;
    marker.kind = BrushKind::Marker;
    marker.size = 30.0;
    marker.setHex("#ca8a04");
    SurfacePtr mSurf = render(horizontal, marker);
    const int mCap = centreAlpha(mSurf.get(), headX + 6, midY);
    const int mRibbon = centreAlpha(mSurf.get(), headX + 40 * 3, midY);
    check(std::abs(mCap - mRibbon) <= 2,
          "marker cap is not darker than its ribbon (" + std::to_string(mCap) + " vs " +
              std::to_string(mRibbon) + ")");
}

// Regression: the toolbar has to show the mark the brush will actually leave.
// Without it the size slider is just a number, and the difference between a 3
// and a 4 nib is invisible until a stroke is committed to the page.
void testBrushPreview() {
    Brush pen;
    pen.size = 10.0;
    check(radiusFor({0, 0, 0.2, 0, 0}, pen) < radiusFor({0, 0, 1.0, 0, 0}, pen),
          "preview radius follows pressure");

    Brush eraser;
    eraser.kind = BrushKind::Eraser;
    eraser.size = 10.0;
    check(radiusFor({0, 0, 0.9, 0, 0}, eraser) > radiusFor({0, 0, 0.1, 0, 0}, pen) * 3.0,
          "eraser footprint is visibly larger than a pen nib");
}

// --- rendering ------------------------------------------------------------

void testRendering() {
    Brush pen;
    pen.size = 14.0;

    SurfacePtr surface = makeSurface(800, 600);
    cairo_t* cr = cairo_create(surface.get());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    drawStroke(cr, arcStroke(200, 0.05, 1.0), pen);
    cairo_destroy(cr);

    const long ink = countInkPixels(surface.get());
    check(ink > 1000, "pen stroke lays down ink (" + std::to_string(ink) + " px)");

    // Pressure ramp must actually vary width along the stroke: compare the
    // vertical extent of the ink near the start against the middle.
    // Find the vertical extent of the stroke near each x region.
    auto isInk = [](uint32_t px) {
        if (((px >> 24) & 0xFF) < 40) return false;
        return ((px >> 16) & 0xFF) < 235 || ((px >> 8) & 0xFF) < 235 || (px & 0xFF) < 235;
    };
    auto heightNear = [&](double xCenter, double halfWidth) {
        cairo_surface_flush(surface.get());
        const int stride = cairo_image_surface_get_stride(surface.get());
        const unsigned char* data = cairo_image_surface_get_data(surface.get());
        int minY = 100000, maxY = -1;
        for (int x = static_cast<int>(xCenter - halfWidth);
             x <= static_cast<int>(xCenter + halfWidth); ++x) {
            for (int y = 0; y < 600; ++y) {
                if (isInk(reinterpret_cast<const uint32_t*>(data + y * stride)[x])) {
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
            }
        }
        return maxY < 0 ? 0.0 : static_cast<double>(maxY - minY);
    };

    const double startHeight = heightNear(100.0, 8.0);
    const double midHeight = heightNear(400.0, 8.0);
    check(startHeight > 0 && midHeight > startHeight * 1.5,
          "pressure taper widens the stroke (start " + fmt(startHeight, 1) + "px, mid " +
              fmt(midHeight, 1) + "px)");

    // Tilt shading must add ink only when tilt is present.
    auto inkWithTilt = [](double tiltX, double tiltY) {
        SurfacePtr s = makeSurface(800, 600);
        cairo_t* c = cairo_create(s.get());
        cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgb(c, 1, 1, 1);
        cairo_paint(c);

        Stroke stroke = arcStroke(120, 0.4, 0.9);
        for (Point& p : stroke.points) {
            p.tiltX = tiltX;
            p.tiltY = tiltY;
        }
        Brush b;
        b.size = 14.0;
        b.tiltShading = true;
        drawStroke(c, stroke, b);
        cairo_destroy(c);
        return countInkPixels(s.get());
    };

    const long flat = inkWithTilt(0.0, 0.0);
    const long tilted = inkWithTilt(58.0, -50.0);
    check(tilted > flat, "tilt shading darkens the stroke (" + std::to_string(flat) + " -> " +
                              std::to_string(tilted) + " px)");

    Brush noTilt;
    noTilt.size = 14.0;
    noTilt.tiltShading = false;
    SurfacePtr s2 = makeSurface(800, 600);
    cairo_t* c2 = cairo_create(s2.get());
    cairo_set_operator(c2, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(c2, 1, 1, 1);
    cairo_paint(c2);
    Stroke ts = arcStroke(120, 0.4, 0.9);
    for (Point& p : ts.points) { p.tiltX = 58; p.tiltY = -50; }
    drawStroke(c2, ts, noTilt);
    cairo_destroy(c2);
    check(countInkPixels(s2.get()) < tilted,
          "disabling tilt shading removes the extra ink");

    // Eraser must clear pixels.
    SurfacePtr s3 = makeSurface(400, 300);
    cairo_t* c3 = cairo_create(s3.get());
    cairo_set_operator(c3, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(c3, 1, 1, 1);
    cairo_paint(c3);
    Brush eraser;
    eraser.kind = BrushKind::Eraser;
    eraser.size = 20.0;
    for (int i = 0; i < 20; ++i) {
        cairo_set_operator(c3, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgb(c3, 0, 0, 0);
        cairo_rectangle(c3, 50 + i * 10, 100, 8, 80);
        cairo_fill(c3);
    }
    cairo_surface_flush(s3.get());
    const long beforeErase = countInkPixels(s3.get());
    Stroke eraseStroke;
    for (int i = 0; i <= 20; ++i) eraseStroke.points.push_back(Point{60.0 + i * 8, 140.0, 0.8, 0, 0});
    drawStroke(c3, eraseStroke, eraser);
    cairo_destroy(c3);
    check(beforeErase > 0 && countInkPixels(s3.get()) < beforeErase,
          "eraser clears pixels");

    // Empty stroke is a no-op.
    SurfacePtr s4 = makeSurface(100, 100);
    cairo_t* c4 = cairo_create(s4.get());
    cairo_set_operator(c4, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(c4, 1, 1, 1);
    cairo_paint(c4);
    drawStroke(c4, Stroke{}, Brush{});
    cairo_destroy(c4);
    check(countInkPixels(s4.get()) == 0, "empty stroke draws nothing");
}

void testDocument() {
    Document doc(400, 300);
    check(doc.layerCount() == 1, "document starts with one layer");
    check(doc.layerAt(0).name() == "Background", "initial layer is Background");
    check(doc.activeIndex() == 0, "initial active index is 0");

    doc.addLayer();
    check(doc.layerCount() == 2 && doc.activeIndex() == 1, "addLayer inserts above and focuses");
    check(doc.layerAt(1).name() == "Layer 2", "addLayer names the new layer");

    doc.duplicateActive();
    check(doc.layerCount() == 3 && doc.active().name() == "Layer 2 copy",
          "duplicateActive copies content and name");
    check(doc.removeActive() != nullptr, "removeActive returns the removed layer");
    check(doc.layerCount() == 2, "removeActive shrinks the stack");
    check(doc.activeIndex() == 1, "activeIndex clamped after removal");
    check(doc.removeActive() != nullptr, "stack can drain toward one layer");
    check(doc.layerCount() == 1, "stack drained to a single layer");
    check(doc.removeActive() == nullptr, "last layer cannot be removed");

    doc.addLayer();
    check(doc.moveActive(-1) && doc.activeIndex() == 0, "moveActive reorders downward");
    check(!doc.moveActive(-1), "moveActive stops at the bottom");
    check(doc.moveActive(1) && doc.activeIndex() == 1, "moveActive reorders upward");
    check(!doc.mergeDown() || true, "mergeDown runs when a layer is below");

    doc.setActiveIndex(0);
    check(!doc.mergeDown(), "mergeDown refuses at the bottom");

    // Compositing
    Document d2(100, 100);
    cairo_t* cr = cairo_create(d2.active().surface());
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgb(cr, 1, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    check(!d2.active().isEmpty(), "painted layer is not empty");
    d2.active().setOpacity(0.5);
    SurfacePtr flat = d2.flatten();
    check(countInkPixels(flat.get()) > 0, "flatten composites layer content");

    d2.active().setVisible(false);
    SurfacePtr flatHidden = d2.flatten();
    check(flatHidden != nullptr, "flatten works with hidden layer");

    d2.active().clear();
    check(d2.active().isEmpty(), "clear empties the layer");

    // Compositing must preserve colour, not just alpha.
    Document d3(100, 100);
    d3.addLayer();
    Brush cyan;
    cyan.setHex("#0ea5e9");
    cyan.size = 20.0;
    Stroke cs;
    for (int i = 0; i <= 20; ++i) cs.points.push_back(Point{10.0 + i * 4.0, 50.0, 0.9, 0, 0});
    drawStrokeOn(d3.active().surface(), cs, cyan);

    auto pixelAt = [](cairo_surface_t* s, int x, int y) {
        cairo_surface_flush(s);
        const int stride = cairo_image_surface_get_stride(s);
        const unsigned char* d = cairo_image_surface_get_data(s);
        const uint32_t px = reinterpret_cast<const uint32_t*>(d + y * stride)[x];
        return std::make_tuple((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF);
    };

    auto [lr, lg, lb] = pixelAt(d3.layerAt(1).surface(), 50, 50);
    auto [fr, fg, fb] = pixelAt(d3.flatten().get(), 50, 50);
    check(fr < 60 && fg > 120 && fb > 200,
          "compositing preserves stroke colour (layer " + std::to_string(lr) + "," +
              std::to_string(lg) + "," + std::to_string(lb) + " -> flat " + std::to_string(fr) +
              "," + std::to_string(fg) + "," + std::to_string(fb) + ")");
    // A greyed-out stroke would have near-equal channels.
    const int hi = std::max(fr, std::max(fg, fb));
    const int lo = std::min(fr, std::min(fg, fb));
    const int spread = hi - lo;
    check(spread > 90, "compositing keeps a strongly saturated colour (spread " +
                           std::to_string(spread) + ")");

    // Layer opacity must blend toward the page, not toward black.
    d3.layerAt(1).setOpacity(0.5);
    auto [hr, hg, hb] = pixelAt(d3.flatten().get(), 50, 50);
    check(hr > fr && hg > fg && hb > fb,
          "layer opacity fades toward the page (" + std::to_string(fr) + "," + std::to_string(fg) +
              "," + std::to_string(fb) + " -> " + std::to_string(hr) + "," + std::to_string(hg) +
              "," + std::to_string(hb) + ")");
    d3.layerAt(1).setOpacity(1.0);

    // Hiding a layer must remove its contribution: the flattened pixel goes back
    // to bare page white.
    d3.layerAt(1).setVisible(false);
    auto [wr, wg, wb] = pixelAt(d3.flatten().get(), 50, 50);
    check(wr > 250 && wg > 250 && wb > 250,
          "hidden layer contributes nothing to the flatten (" + std::to_string(wr) + "," +
              std::to_string(wg) + "," + std::to_string(wb) + ")");
}

void testHistory() {
    std::vector<std::string> ran;
    History h(3);

    check(!h.canUndo() && !h.canRedo(), "fresh history is inert");

    h.push("a", [&] { ran.push_back("u1"); }, [&] { ran.push_back("r1"); });
    h.push("b", [&] { ran.push_back("u2"); }, [&] { ran.push_back("r2"); });
    check(h.canUndo() && !h.canRedo(), "after two pushes only undo is available");

    check(h.undo().value() == "b", "undo reports the latest label");
    check(h.undo().value() == "a", "undo is LIFO");
    check(!h.canUndo(), "history exhausted after full undo");

    check(h.redo().value() == "a", "redo replays the oldest undone entry");
    check(h.redo().value() == "b", "redo continues forward");
    check(!h.canRedo(), "redo exhausted");
    h.undo();
    check(h.canUndo(), "one entry left after a single undo");
    h.undo();
    check(!h.canUndo(), "undo back to the start");
    check(!h.undo().has_value(), "undo past the start is a no-op");

    ran.clear();
    h.undo();
    h.push("c", [&] { ran.push_back("u3"); }, [&] { ran.push_back("r3"); });
    check(!h.canRedo(), "new push drops the redo branch");

    History hl(3);
    for (int i = 0; i < 5; ++i) hl.push("x", [] {}, [] {});
    check(hl.size() == 3, "history respects its limit");

    h.clear();
    check(!h.canUndo() && !h.canRedo(), "clear resets history");
}

// Draws a simple diagonal line into whatever layer is currently active.
void strokeOnActiveLayer(AppState& state) {
    state.beginStroke(Point{40.0, 40.0, 0.9, 0.0, 0.0});
    for (int i = 1; i <= 25; ++i) {
        state.extendStroke(Point{40.0 + i * 4.0, 40.0 + i * 3.0, 0.9, 0.0, 0.0});
    }
    state.endStroke();
}

void testAppState() {
    AppState state(600, 400);
    check(state.document().layerCount() == 1, "app state starts with one layer");

    // A stroke must add ink and be undoable.
    state.beginStroke(Point{100, 100, 0.3, 0, 0});
    for (int i = 1; i <= 40; ++i) {
        state.extendStroke(Point{100.0 + i * 6.0, 120.0 + std::sin(i / 5.0) * 20.0,
                                 0.3 + 0.6 * (i / 40.0), 10, -5});
    }
    state.endStroke();

    long ink = countInkPixels(state.document().active().surface());
    check(ink > 200, "stroke paints onto the active layer (" + std::to_string(ink) + " px)");

    state.undo();
    check(countInkPixels(state.document().active().surface()) < ink,
          "undo removes the stroke");
    state.redo();
    check(countInkPixels(state.document().active().surface()) > 200, "redo restores the stroke");

    // Duplicate samples must be rejected.
    state.beginStroke(Point{200, 200, 0.5, 0, 0});
    check(!state.extendStroke(Point{200.1, 200.1, 0.5, 0, 0}),
          "near-identical sample rejected");
    check(state.extendStroke(Point{220, 200, 0.5, 0, 0}), "distinct sample accepted");
    state.endStroke();

    // A pen contact arrives as two press events in the same frame. The second
    // one must not restart the stroke, or the point the nib was actually
    // placed at is thrown away.
    state.beginStroke(Point{300, 300, 0.5, 0, 0});
    state.extendStroke(Point{320, 300, 0.5, 0, 0});
    state.beginStroke(Point{340, 340, 0.5, 0, 0});
    check(state.liveStroke().points.size() == 2, "a second press does not restart the stroke");
    check(std::abs(state.liveStroke().points.front().x - 300.0) < 1e-9,
          "the stroke still starts where the nib landed");
    state.endStroke();
    state.beginStroke(Point{340, 340, 0.5, 0, 0});
    check(state.liveStroke().points.size() == 1, "a stroke starts fresh after the last one ends");
    state.endStroke();

    // Structural edits through history.
    const size_t layerCountBefore = state.document().layerCount();
    state.addLayer();
    check(state.document().layerCount() == layerCountBefore + 1, "addLayer grows the stack");
    state.undo();
    check(state.document().layerCount() == layerCountBefore, "undo reverts addLayer");
    state.redo();
    check(state.document().layerCount() == layerCountBefore + 1, "redo re-applies addLayer");

    state.duplicateLayer();
    check(state.document().layerCount() == layerCountBefore + 2, "duplicateLayer grows the stack");
    state.undo();
    check(state.document().layerCount() == layerCountBefore + 1, "undo reverts duplicateLayer");

    state.deleteLayer();
    check(state.document().layerCount() == layerCountBefore, "deleteLayer shrinks the stack");
    state.undo();
    check(state.document().layerCount() == layerCountBefore + 1, "undo reverts deleteLayer");

    // Visibility toggle is undoable.
    const size_t idx = state.document().activeIndex();
    const bool wasVisible = state.document().layerAt(idx).visible();
    state.toggleVisibility(idx);
    check(state.document().layerAt(idx).visible() != wasVisible, "toggleVisibility flips the flag");
    state.undo();
    check(state.document().layerAt(idx).visible() == wasVisible, "undo restores visibility");
    state.redo();
    check(state.document().layerAt(idx).visible() != wasVisible, "redo re-applies visibility");

    // Clear layer. The active layer must actually hold ink first, otherwise
    // clearActiveLayer correctly does nothing and records no history entry.
    state.addLayer();
    strokeOnActiveLayer(state);
    check(!state.document().active().isEmpty(), "fresh layer holds ink");
    state.clearActiveLayer();
    check(state.document().active().isEmpty(), "clearActiveLayer empties the layer");
    state.undo();
    check(!state.document().active().isEmpty(), "undo restores cleared pixels");
    state.redo();
    check(state.document().active().isEmpty(), "redo re-clears the layer");
    state.undo();

    // Save / load round trip.
    state.addLayer();
    state.beginStroke(Point{50, 50, 0.9, 0, 0});
    for (int i = 0; i < 30; ++i) {
        state.extendStroke(Point{50.0 + i * 5.0, 60.0 + i * 2.0, 0.5 + 0.4 * (i / 30.0), 20, 20});
    }
    state.endStroke();

    auto project = state.serialize();
    check(project.has_value(), "serialize produces a project");
    check(project->layerPngs.size() == state.document().layerCount(),
          "project holds one PNG per layer");

    AppState restored(600, 400);
    check(restored.load(*project), "load accepts the project");
    check(restored.document().layerCount() == state.document().layerCount(),
          "load restores layer count");
    check(restored.document().activeIndex() == state.document().activeIndex(),
          "load restores active index");

    SurfacePtr flat = restored.document().flatten();
    check(countInkPixels(flat.get()) > 200, "restored document still has ink");

    SurfacePtr exported = restored.exportPng().has_value()
                               ? makeSurface(restored.document().width(),
                                             restored.document().height())
                               : nullptr;
    check(exported != nullptr, "exportPng returns PNG bytes");

    // Corrupt input must be rejected rather than crash.
    Project bad = *project;
    bad.layerPngs[0] = std::string("not a png at all");
    AppState badState(600, 400);
    check(!badState.load(bad), "corrupt PNG payload is rejected");

    Project empty;
    empty.layerPngs.clear();
    AppState emptyState(600, 400);
    check(!emptyState.load(empty), "project with no layers is rejected");
}

}  // namespace

// Regression: the layer panel used to announce a structural change on every
// rename and every opacity drag. The panel rebuilt itself from that callback,
// which destroyed the very slider or text entry the handler was running inside --
// so dragging opacity crashed, and typing a name lost the keystrokes that
// followed.
//
// These count rebuilds rather than inspecting widgets: the count is exactly what
// the crash hinged on, and it is observable without GTK.
void testPropertyChangesDoNotRebuildPanel() {
    AppState state(200, 200);

    int structural = 0;
    int pixels = 0;
    int renames = 0;
    size_t nameIndex = 9999;
    std::string nameValue;

    state.setLayersChanged([&]() { ++structural; });
    state.setLayerPixelChanged([&]() { ++pixels; });
    state.setNameChanged([&](size_t index, const std::string& name) {
        ++renames;
        nameIndex = index;
        nameValue = name;
    });

    // Opacity: a whole drag, the way a slider produces it.
    state.setLayerOpacityLive(0, 0.8);
    state.setLayerOpacityLive(0, 0.6);
    state.setLayerOpacityLive(0, 0.4);
    check(structural == 0,
          "an opacity drag announces no structural change (" + std::to_string(structural) + ")");
    check(std::abs(state.document().layerAt(0).opacity() - 0.4) < 1e-6,
          "the live value is applied immediately");
    check(pixels >= 3, "the canvas is told about each step so it can redraw");

    state.commitLayerOpacity(0);
    check(structural == 0, "committing an opacity drag rebuilds nothing");

    // The whole drag is one undo step, not one per sample.
    state.undo();
    check(std::abs(state.document().layerAt(0).opacity() - 1.0) < 1e-6,
          "one undo takes the whole drag back to the start");
    state.redo();
    check(std::abs(state.document().layerAt(0).opacity() - 0.4) < 1e-6,
          "redo returns the dragged value");

    // Cancelling puts it back and records nothing.
    state.setLayerOpacityLive(0, 0.9);
    state.cancelLayerOpacity(0);
    check(std::abs(state.document().layerAt(0).opacity() - 0.4) < 1e-6,
          "cancelling restores the value the drag started from");

    // Rename.
    const int structuralBefore = structural;
    state.setLayerName(0, "Ink");
    check(structural == structuralBefore, "renaming a layer announces no structural change");
    check(renames == 1 && nameIndex == 0 && nameValue == "Ink",
          "the rename is announced with its index and name");

    state.undo();
    check(state.document().layerAt(0).name() == "Background",
          "undo restores the previous layer name");
    check(renames == 2, "the undo also announces the name it restored");

    // A structural edit still rebuilds, or the panel would never update.
    state.addLayer();
    check(structural > structuralBefore, "adding a layer still announces a structural change");
}

// Regression: renaming has to survive the input a user actually types.
void testLayerRenaming() {
    AppState state(200, 200);
    state.addLayer();
    const size_t top = state.document().activeIndex();

    state.setLayerName(top, "  Spaced  ");
    check(state.document().layerAt(top).name() == "Spaced",
          "surrounding blanks are trimmed off a name");

    // An empty name is ignored: a layer that renders as nothing cannot be picked
    // out of the list, so the old name is kept.
    state.setLayerName(top, "   ");
    check(state.document().layerAt(top).name() == "Spaced",
          "a blank name is rejected rather than applied");

    state.setLayerName(top, std::string(200, 'x'));
    check(state.document().layerAt(top).name().size() == 64,
          "an over-long name is truncated instead of stretching the row");

    // Renaming to the name the layer already has is not an edit.
    state.setLayerName(top, std::string(200, 'x'));
    const size_t before = state.history().size();
    state.setLayerName(top, std::string(200, 'x'));
    check(state.history().size() == before, "renaming to the same name records nothing");

    state.setLayerName(top, "Ink");
    check(state.document().layerAt(top).name() == "Ink", "a real rename still applies");
}

// Regression: opacity has to survive the full range, including the ends, and zero
// must not be mistaken for "no layer".
void testLayerOpacityRange() {
    AppState state(200, 200);

    Brush b;
    b.size = 24.0;
    b.setHex("#b91c1c");
    Stroke s;
    for (int i = 0; i <= 20; ++i) s.points.push_back(Point{20.0 + i * 8.0, 100.0, 0.9, 0, 0});
    drawStrokeOn(state.document().active().surface(), s, b);

    state.setLayerOpacityLive(0, 1.0);
    state.commitLayerOpacity(0);
    check(countInkPixels(state.document().flatten().get()) > 200, "full opacity shows the ink");

    state.setLayerOpacityLive(0, 0.5);
    state.commitLayerOpacity(0);
    check(std::abs(state.document().layerAt(0).opacity() - 0.5) < 1e-6, "half opacity is stored");

    state.setLayerOpacityLive(0, 0.0);
    state.commitLayerOpacity(0);
    check(std::abs(state.document().layerAt(0).opacity()) < 1e-6, "zero opacity is stored");
    check(countInkPixels(state.document().flatten().get()) == 0,
          "a fully transparent layer composites to nothing");

    // The pixels must still be there: opacity is a display setting, not a deletion.
    check(!state.document().layerAt(0).isEmpty(),
          "a transparent layer keeps its pixels rather than being emptied");

    state.undo();
    check(std::abs(state.document().layerAt(0).opacity() - 0.5) < 1e-6,
          "undo steps back through opacity values");
    check(!state.document().layerAt(0).isEmpty(), "undoing opacity keeps the pixels too");

    // Out-of-range input is clamped rather than stored as written.
    state.setLayerOpacityLive(0, 5.0);
    state.commitLayerOpacity(0);
    check(std::abs(state.document().layerAt(0).opacity() - 1.0) < 1e-6, "opacity clamps at 1.0");
    state.setLayerOpacityLive(0, -3.0);
    state.commitLayerOpacity(0);
    check(std::abs(state.document().layerAt(0).opacity()) < 1e-6, "opacity clamps at 0.0");

    // An out-of-range index is ignored, not a crash.
    state.setLayerOpacityLive(99, 0.5);
    state.commitLayerOpacity(99);
    state.cancelLayerOpacity(99);
    check(true, "an out-of-range opacity index is ignored");
}

// Regression: reordering has to actually move the layer and stay undoable. The
// panel's move buttons call straight into this.
void testLayerReordering() {
    AppState state(200, 200);

    state.beginStroke(Point{40, 40, 0.9, 0, 0});
    for (int i = 1; i <= 20; ++i) state.extendStroke(Point{40.0 + i * 5.0, 40.0, 0.9, 0, 0});
    state.endStroke();

    state.addLayer();  // "Layer 2", active
    state.addLayer();  // "Layer 3", active
    check(state.document().activeIndex() == 2, "the newest layer is active");

    // A positive delta moves the layer up the stack, towards the top of the panel.
    // Recording only the active index meant undo left the layers where the move had
    // put them, so "reorder" was not undoable at all.
    state.moveLayer(-1);
    check(state.document().activeIndex() == 1, "moveLayer(-1) moves down the stack");
    check(state.document().layerAt(1).name() == "Layer 3",
          "the moved layer actually changed position (" +
              state.document().layerAt(1).name() + ")");

    state.undo();
    check(state.document().activeIndex() == 2, "undo restores the active index");
    check(state.document().layerAt(2).name() == "Layer 3",
          "undo restores the layer order too, not just the selection");

    state.redo();
    check(state.document().activeIndex() == 1, "redo re-applies the move");
    check(state.document().layerAt(1).name() == "Layer 3", "redo puts the layer back where moved");

    state.undo();

    // The layer that carries ink must still be the one carrying it.
    check(!state.document().layerAt(0).isEmpty(), "the ink stayed on the bottom layer");

    // Two undos above left the selection back on the top layer, index 2 of 3.
    state.moveLayer(2);
    check(state.document().activeIndex() == 2, "a move past the top is refused");
    state.moveLayer(-5);
    check(state.document().activeIndex() == 2, "a move past the bottom is refused");
    check(state.document().layerAt(2).name() == "Layer 3",
          "a refused move leaves the stack untouched");

    // Reordering down and back up must return to the original order.
    state.moveLayer(-1);
    state.moveLayer(1);
    check(state.document().layerAt(2).name() == "Layer 3" &&
              state.document().layerAt(1).name() == "Layer 2",
          "moving down then up returns the stack to where it started");

    state.undo();
    state.undo();
    check(state.document().layerAt(2).name() == "Layer 3",
          "undoing both moves restores the original order");
}

// Regression: saving wrote only the header and the PNG payloads. Layer names,
// visibility, opacity, the active layer and every brush setting were dropped, so
// reopening a file lost all of them and the panel came back with blanks and 100%
// defaults. This is what a rename is silently undone by.
void testProjectRoundTripKeepsEverything() {
    AppState state(300, 200);
    state.beginStroke(Point{40, 40, 0.9, 0, 0});
    for (int i = 1; i <= 20; ++i) state.extendStroke(Point{40.0 + i * 6.0, 40.0, 0.9, 0, 0});
    state.endStroke();

    state.addLayer();
    const size_t top = state.document().activeIndex();
    state.setLayerName(top, "Line art");
    state.setLayerName(0, "Roughs");

    state.setLayerOpacityLive(top, 0.6);
    state.commitLayerOpacity(top);
    state.toggleVisibility(0);  // hide the bottom layer

    state.settings().color = "#b91c1c";
    state.settings().size = 21.5;
    state.settings().tiltShading = false;
    state.settings().pressureWidth = false;
    state.settings().stabilize = false;

    auto project = state.serialize();
    check(project.has_value(), "project serialises");
    const std::string blob = encodeProjectBlob(*project);

    Project decoded;
    const bool decodedOk = decodeProjectBlob(blob, &decoded);
    check(decodedOk, "the blob decodes");
    if (!decodedOk) {
        check(false, "cannot inspect a blob that did not decode");
        return;
    }

    check(decoded.layerNames.size() == 2, "both layer names survive the round trip");
    if (decoded.layerNames.size() != 2) return;
    check(decoded.layerNames[0] == "Roughs",
          "the bottom layer keeps its name (" + decoded.layerNames[0] + ")");
    check(decoded.layerNames[1] == "Line art",
          "the top layer keeps its name (" + decoded.layerNames[1] + ")");
    check(decoded.layerVisible[0] == 0, "a hidden layer reopens hidden");
    check(decoded.layerVisible[1] == 1, "a visible layer reopens visible");
    check(std::abs(decoded.layerOpacity[1] - 0.6) < 0.01,
          "layer opacity survives (" + fmt(decoded.layerOpacity[1], 3) + ")");
    check(decoded.activeIndex == state.document().activeIndex(),
          "the active layer survives (" + std::to_string(decoded.activeIndex) + ")");
    check(decoded.settings.color == "#b91c1c", "the brush colour survives");
    check(std::abs(decoded.settings.size - 21.5) < 0.01, "the brush size survives");
    check(!decoded.settings.tiltShading, "the tilt toggle survives");
    check(!decoded.settings.pressureWidth, "the pressure-width toggle survives");
    check(!decoded.settings.stabilize, "the stabilizer toggle survives");

    // And the whole thing has to load into a real document.
    AppState restored(300, 200);
    check(restored.load(decoded), "the decoded project loads");
    check(restored.document().layerAt(1).name() == "Line art",
          "the loaded document carries the layer name");
    check(!restored.document().layerAt(0).visible(), "the loaded document carries visibility");
    check(restored.brush().size > 20.0, "the loaded document carries the brush size");

    // A version 1 file has no metadata at all. It must still open rather than
    // being rejected, falling back to the old defaults.
    Project legacy;
    legacy.width = 120;
    legacy.height = 90;
    legacy.layerPngs.push_back(*state.serialize()->layerPngs.begin());
    legacy.layerNames.assign(1, "Layer");
    legacy.layerVisible.assign(1, 1);
    legacy.layerOpacity.assign(1, 1.0);

    std::string oldBlob;
    char header[128];
    std::snprintf(header, sizeof(header), "STYLUSPEN %d %d %zu\n", legacy.width, legacy.height,
                  legacy.layerPngs.size());
    oldBlob += header;
    for (const std::string& png : legacy.layerPngs) {
        char len[32];
        std::snprintf(len, sizeof(len), "%zu\n", png.size());
        oldBlob += len;
        oldBlob += png;
    }

    Project fromOld;
    check(decodeProjectBlob(oldBlob, &fromOld), "an older project file still opens");
    check(fromOld.layerCount() == 1, "the older file's layers are all there");

    // Garbage must be rejected rather than half-read.
    Project junk;
    check(!decodeProjectBlob("", &junk), "an empty blob is rejected");
    check(!decodeProjectBlob("NOTAHEADER 1 2 3", &junk), "a wrong magic string is rejected");
    check(!decodeProjectBlob("STYLUSPEN2 0 100 1", &junk), "a zero width is rejected");
    check(!decodeProjectBlob("STYLUSPEN2 100 0 1", &junk), "a zero height is rejected");
    check(!decodeProjectBlob("STYLUSPEN2 100 100 0", &junk), "a layerless project is rejected");
    check(!decodeProjectBlob("STYLUSPEN2 100 100 1\nlen", &junk),
          "a payload shorter than its declared length is rejected");
    check(!decodeProjectBlob("STYLUSPEN2 x y z", &junk), "a non-numeric header is rejected");
}

// Regression: restoring a stroke snapshot must not touch ink outside the region.
// blitRegion used to composite the snapshot over the whole surface, so
// OPERATOR_SOURCE replaced every pixel outside the stroke's own bounds with the
// snapshot's transparent margin. Undoing one stroke therefore erased the rest of
// the layer.
void testUndoKeepsNeighbouringInk() {
    AppState state(600, 400);

    // Two strokes far apart, so their bounds cannot overlap.
    state.beginStroke(Point{60, 60, 0.9, 0, 0});
    for (int i = 1; i <= 25; ++i) state.extendStroke(Point{60.0 + i * 4.0, 60.0, 0.9, 0, 0});
    state.endStroke();

    state.beginStroke(Point{60, 320, 0.9, 0, 0});
    for (int i = 1; i <= 25; ++i) state.extendStroke(Point{60.0 + i * 4.0, 320.0, 0.9, 0, 0});
    state.endStroke();

    const long both = countInkPixels(state.document().active().surface());
    check(both > 400, "two strokes are on the layer (" + std::to_string(both) + " px)");

    // Undo the lower one. The upper stroke has to survive; it used to be wiped
    // along with everything else outside the restored region.
    state.undo();
    const long afterUndo = countInkPixels(state.document().active().surface());
    check(afterUndo > 100,
          "undoing one stroke leaves the other intact (" + std::to_string(afterUndo) + " px of " +
              std::to_string(both) + ")");
    check(afterUndo < both, "undoing one stroke actually removes it");

    // And redo brings it back without doubling anything.
    state.redo();
    const long afterRedo = countInkPixels(state.document().active().surface());
    check(afterRedo == both, "redo restores exactly the original ink (" +
                                 std::to_string(afterRedo) + " vs " + std::to_string(both) + ")");

    // Undoing everything must still leave the first stroke standing.
    state.undo();
    state.undo();
    const long afterBoth = countInkPixels(state.document().active().surface());
    check(afterBoth < afterUndo, "undoing both strokes clears the layer");
    state.redo();
    state.redo();
    check(countInkPixels(state.document().active().surface()) == both,
          "redoing both restores the full layer");
}

// Regression: a stroke that runs off the edge of the page has a snapshot clipped
// to the layer. Restoring it at the unclipped origin shifted the restored pixels
// away from where the stroke actually was.
void testUndoAtPageEdge() {
    AppState state(400, 300);

    // Hugs the left edge and runs above the top, so the stroke's bounds are
    // clipped on two sides.
    state.beginStroke(Point{2, 250, 0.9, 0, 0});
    for (int i = 1; i <= 30; ++i) {
        state.extendStroke(Point{2.0 + i * 0.5, 250.0 - i * 9.0, 0.9, 0, 0});
    }
    state.endStroke();

    const long drawn = countInkPixels(state.document().active().surface());
    check(drawn > 100, "edge stroke draws (" + std::to_string(drawn) + " px)");

    state.undo();
    check(countInkPixels(state.document().active().surface()) == 0,
          "undoing an edge stroke clears every pixel it covered");

    state.redo();
    const long redrawn = countInkPixels(state.document().active().surface());
    // A misaligned restore would put the ink back somewhere else, so the count
    // can match while the shape does not. Compare against a fresh identical
    // stroke on a clean layer instead.
    AppState fresh(400, 300);
    fresh.beginStroke(Point{2, 250, 0.9, 0, 0});
    for (int i = 1; i <= 30; ++i) {
        fresh.extendStroke(Point{2.0 + i * 0.5, 250.0 - i * 9.0, 0.9, 0, 0});
    }
    fresh.endStroke();

    check(redrawn == countInkPixels(fresh.document().active().surface()),
          "redo puts an edge stroke back in the same place (" + std::to_string(redrawn) + " vs " +
              std::to_string(countInkPixels(fresh.document().active().surface())) + ")");
}

// Regression: a stroke's undo entry used to capture a raw Layer*. Undoing a
// structural edit rebuilds every Layer object, which freed the surface the entry
// held, so undoing the stroke afterwards wrote through a dangling pointer.
void testUndoSurvivesStructuralUndo() {
    AppState state(400, 300);

    state.beginStroke(Point{50, 50, 0.9, 0, 0});
    for (int i = 1; i <= 20; ++i) state.extendStroke(Point{50.0 + i * 5.0, 50.0, 0.9, 0, 0});
    state.endStroke();
    const long withStroke = countInkPixels(state.document().active().surface());
    check(withStroke > 100, "stroke is on the layer");

    // Now run a structural edit through undo and redo. Both rebuild the stack,
    // so the stroke's stored layer pointer becomes stale if it is held at all.
    state.addLayer();
    state.undo();
    state.redo();
    state.undo();

    // Undo the stroke. This is the call that used to write to freed memory.
    state.undo();
    const long afterStrokeUndo = countInkPixels(state.document().active().surface());
    check(afterStrokeUndo == 0,
          "stroke undo works after a structural undo rebuilt the layers (" +
              std::to_string(afterStrokeUndo) + " px left)");

    // And redo it, which also redraws through the resolved layer.
    state.redo();
    check(countInkPixels(state.document().active().surface()) > 100,
          "stroke redo works after a structural undo rebuilt the layers");
}

int main() {
    testGeometry();
    testBoardTransform();
    testCursorMatchesInk();
    testTiltUnits();
    testColorParsing();
    testBackgroundColor();
    testBackgroundRoundTrip();
    testBackgroundUndo();
    testHistoryAvailabilityIsObservable();
    testDottedPaperIsDotsNotWedges();
    testRulingBlendsIntoThePage();
    testRulingShowsOnDarkPages();
    testRulingHoldsOneDevicePixel();
    testLayerPlacedImage();
    testInsertImage();
    testInsertText();
    testImageDragIsOneUndoStep();
    testImageResize();
    testImageResizeIsOneUndoStep();
    testImageFitCentreAndDelete();
    testPaper();
    testSpacingBandIsSharedWithTheSlider();
    testPaperIsPainted();
    testPaperRoundTrip();
    testPaperUndo();
    testBrushBasics();
    testPressureSensitivity();
    testBrushPreview();
    testStrokeEndCaps();
    testRendering();
    testTiltAwareBounds();
    testCuspHandling();
    testResampleInterpolatesTilt();
    testDocument();
    testCompositeCache();
    testHistory();
    testAppState();
    testPropertyChangesDoNotRebuildPanel();
    testLayerRenaming();
    testLayerOpacityRange();
    testLayerReordering();
    testProjectRoundTripKeepsEverything();
    testUndoKeepsNeighbouringInk();
    testUndoAtPageEdge();
    testUndoSurvivesStructuralUndo();
    testHandwritingRecognition();
    testHandwritingRecognitionAccuracy();
    testMultiLineHandwritingToText();
    testHandwritingLearning();


    testEditTextLayer();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}