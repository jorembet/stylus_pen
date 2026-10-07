#pragma once

#include <cairo/cairo.h>

#include "color.h"

namespace stylus {

// The ruling printed on the page underneath the artwork.
//
// This is page furniture, not ink: it is not on any layer, it cannot be erased,
// and hiding the bottom layer does not take it away. It is drawn between the page
// colour and the layer stack, so it stays behind everything the user paints.
enum class PaperKind {
    Plain = 0,     // no ruling at all
    Ruled = 1,     // horizontal lines only, like ruled notepaper
    Grid = 2,      // horizontal and vertical, like squared paper
    Dotted = 3,    // a dot at every intersection, like dotted notepaper
    Checkered = 4, // alternating squares, so a transparent region reads as one
};

constexpr int kPaperKindCount = 5;

// The band of spacings the page can express, in document pixels.
//
// These are the clamp's bounds *and* the sidebar slider's range, deliberately the
// same two numbers. They used to disagree -- the slider stopped at 96 while the
// format allowed 512 -- so a project saved with a wide pitch reopened with the
// slider pinned at its maximum next to a label showing a different number. One
// constant per end keeps the control and the document from ever disagreeing about
// what is legal.
//
// The lower bound is also what keeps paintPaper's loop count bounded: at 8px over a
// 1600x1100 page that is a few hundred marks, not one per pixel.
constexpr double kMinPaperSpacing = 8.0;
constexpr double kMaxPaperSpacing = 128.0;

struct Paper {
    PaperKind kind = PaperKind::Plain;

    // Distance between rules, in document pixels.
    //
    // Clamped on the way in. It arrives from a project file the user can edit, and
    // an unclamped 0.0001 would make the painter loop once per document pixel --
    // both a hang and a way to make the app allocate unboundedly.
    double spacing = 32.0;

    // True when the ruling contributes any marks at all. Spacing and kind are
    // both kept even while this is false, so switching Plain off and back on
    // returns to the ruling the user had chosen.
    bool active() const { return kind != PaperKind::Plain; }

    bool operator==(const Paper& other) const {
        return kind == other.kind && spacing == other.spacing;
    }
    bool operator!=(const Paper& other) const { return !(*this == other); }
};

double clampPaperSpacing(double spacing);

// Out of range for `kind` is false, so a corrupt file cannot select a kind that
// does not exist.
bool parsePaperKind(int value, PaperKind* out);
const char* paperKindName(PaperKind kind);

// Draws only the ruling -- never the page colour -- so the caller keeps control
// of what is underneath.
//
// Expects the context to be in document space with the origin at the page's
// top-left corner, and paints across the whole `width` x `height` page. The canvas
// draws it inside the zoom transform so the ruling scales with the artwork, while
// flatten() draws it at 1:1 into the exported PNG; both therefore produce the
// same ruling at the same document pitch.
//
// `page` decides the ink. A fixed dark ruling vanishes on a dark page and glares
// on a pale one, so the ink is picked against the page's own brightness -- the same
// reason the canvas picks its page-edge hairline that way.
//
// `zoom` is the current view scale, and only matters on screen: the stroke weight
// is floored so a rule never thins below one device pixel. Without that floor a
// fit-to-window view of a 1600x1100 page renders every line at well under a pixel,
// and antialiasing smears the ruling into a faint tint that no longer reads as
// paper. Pass 1.0 when drawing at document scale, as flatten() does.
void paintPaper(cairo_t* cr, const Paper& paper, const Color& page, int width, int height,
                double zoom = 1.0);

}  // namespace stylus