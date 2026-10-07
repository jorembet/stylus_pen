#include "paper.h"

#include <algorithm>
#include <cmath>

#include "color.h"
#include "geometry.h"

namespace stylus {

namespace {

// Ink weights for the ruling. Strong enough to read as ruled paper rather than as a
// faint tint -- the earlier 0.16 looked like compression noise, not like a page.
constexpr double kLineAlpha = 0.30;
constexpr double kDotAlpha = 0.42;
// 0.10 was invisible in practice: blended over a near-white page it lands at about
// 0.89, which reads as a very faint wash rather than as a checkerboard. Real
// transparency backdrops sit noticeably stronger than this.
constexpr double kCheckerAlpha = 0.16;

// Never let the stroke fall below one device pixel, so zooming out thins the ruling
// no further.
constexpr double kMinDeviceLineWidth = 1.0;

// The ruling's weight at 1:1. Anything heavier sits too far forward of the artwork.
constexpr double kLineWidth = 1.0;

// Snap to a whole pixel. A rule drawn across a half pixel is two grey rows on a
// HiDPI surface, which reads as a smear instead of a line.
double snap(double v) { return std::floor(v) + 0.5; }

// Ruling ink picked against the page, mirroring the canvas's page-edge hairline.
struct Ink {
    double v;  // every channel: 0 is black, 1 is white
    double alpha;
};

Ink inkFor(const Color& page) {
    // Rec. 601 luma is close enough to perceived lightness for choosing a
    // contrasting ink, and much cheaper than a gamma-aware conversion.
    const double luma = 0.299 * page.r + 0.587 * page.g + 0.114 * page.b;
    // A mid-grey page reads as neither, so bias the threshold low and keep dark ink
    // for anything that is not clearly dark.
    return luma < 0.45 ? Ink{1.0, kLineAlpha} : Ink{0.0, kLineAlpha};
}

void setInk(cairo_t* cr, const Ink& ink, double alpha) {
    cairo_set_source_rgba(cr, ink.v, ink.v, ink.v, alpha);
}

}  // namespace

double clampPaperSpacing(double spacing) {
    // Both ends are shared with the sidebar slider (see kMinPaperSpacing), so the
    // control can always represent whatever the document holds.
    return clampd(spacing, kMinPaperSpacing, kMaxPaperSpacing);
}

bool parsePaperKind(int value, PaperKind* out) {
    if (!out) return false;
    if (value < 0 || value >= kPaperKindCount) return false;
    *out = static_cast<PaperKind>(value);
    return true;
}

const char* paperKindName(PaperKind kind) {
    switch (kind) {
        case PaperKind::Ruled:
            return "Ruled";
        case PaperKind::Grid:
            return "Grid";
        case PaperKind::Dotted:
            return "Dotted";
        case PaperKind::Checkered:
            return "Checkered";
        case PaperKind::Plain:
        default:
            return "Plain";
    }
}

void paintPaper(cairo_t* cr, const Paper& paper, const Color& page, int width, int height,
                double zoom) {
    if (!cr || !paper.active() || width <= 0 || height <= 0) return;

    const double gap = clampPaperSpacing(paper.spacing);
    const Ink ink = inkFor(page);

// The stroke lives in document space, so it scales with the zoom the way real ruling
// does -- zooming in gives a thicker line, because that is what looking closely at
// paper gives you.
//
// Below 1:1 that runs the line under a device pixel and antialiasing smears the whole
// ruling into a faint tint that no longer reads as paper. The inverse of the zoom
// fixes the on-screen thickness at one pixel, and taking the larger of the two gives
// paper-like thickening when zoomed in without the fade when zoomed out.
const double scale = std::max(zoom, 0.01);
const double lineWidth = std::max(kLineWidth, kMinDeviceLineWidth / scale);

    cairo_save(cr);

    // Never draw past the page. Without this a ruler line at the last interval
    // overhangs the edge, and on the canvas that overhang escapes the clip the
    // layers use and shows up in the margin.
    cairo_rectangle(cr, 0.0, 0.0, width, height);
    cairo_clip(cr);

    switch (paper.kind) {
        case PaperKind::Checkered: {
            // Alternating squares, so a transparent area is obvious instead of
            // reading as the same white as the rest of the page.
            setInk(cr, ink, kCheckerAlpha);
            const double side = gap * 2.0;
            // Row 0 offset by a half tile, which is what turns a plain checker into
            // the diagonal-staggered pattern people recognise as transparency.
            for (int row = 0; row * side < height; ++row) {
                const double y = row * side;
                const double offset = (row % 2 == 0) ? 0.0 : side * 0.5;
                for (double x = offset; x < width; x += side) {
                    cairo_rectangle(cr, x, y, side * 0.5, side * 0.5);
                }
            }
            cairo_fill(cr);
            break;
        }

        case PaperKind::Dotted: {
            setInk(cr, ink, kDotAlpha);
            // Sized off the gap but floored, so a tight pitch still gives dots you
            // can see rather than a grey wash.
            const double r = std::max(0.9, gap * 0.04);
            for (double y = gap; y < height; y += gap) {
                for (double x = gap; x < width; x += gap) {
                    // Start a fresh subpath per dot. Without this, cairo_arc joins
                    // each dot to the end of the previous one with a straight line,
                    // and one fill turns the whole lattice into a single filled
                    // polygon -- wedges between the dots instead of dots.
                    cairo_new_sub_path(cr);
                    cairo_arc(cr, x, y, r, 0.0, 2.0 * M_PI);
                }
            }
            cairo_fill(cr);
            break;
        }

        case PaperKind::Grid: {
            setInk(cr, ink, ink.alpha);
            cairo_set_line_width(cr, lineWidth);
            for (double x = snap(gap); x < width; x += gap) {
                cairo_move_to(cr, x, 0.0);
                cairo_line_to(cr, x, height);
            }
            for (double y = snap(gap); y < height; y += gap) {
                cairo_move_to(cr, 0.0, y);
                cairo_line_to(cr, width, y);
            }
            cairo_stroke(cr);
            break;
        }

        case PaperKind::Ruled: {
            setInk(cr, ink, ink.alpha);
            cairo_set_line_width(cr, lineWidth);
            // A ruled sheet carries a heavier line at the top margin, the way real
            // notepaper does. That single asymmetric line is most of what makes a
            // ruled page read as ruled rather than as a striped rectangle.
            cairo_move_to(cr, 0.0, snap(gap));
            cairo_line_to(cr, width, snap(gap));
            cairo_stroke(cr);

            setInk(cr, ink, ink.alpha * 0.8);
            cairo_set_line_width(cr, lineWidth);
            for (double y = snap(gap * 2.0); y < height; y += gap) {
                cairo_move_to(cr, 0.0, y);
                cairo_line_to(cr, width, y);
            }
            cairo_stroke(cr);
            break;
        }

        case PaperKind::Plain:
        default:
            break;
    }

    cairo_restore(cr);
}

}  // namespace stylus