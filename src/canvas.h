#pragma once

#include <gtk/gtk.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "geometry.h"
#include "project.h"

namespace stylus {

// The drawing widget: owns pointer handling and paints the flattened document
// plus any in-progress stroke.
//
// Input arrives through a single GtkEventControllerLegacy rather than through
// click/drag/motion gestures. Tablet tools take an implicit grab, and the
// gesture wrappers report coordinates relative to a surface origin they never
// learn, so a pen stroke starts wherever the gesture thinks the origin is
// instead of under the nib. The raw event always carries the true position.
// What one raw input event means to the canvas.
//
// Split out of the event handler so the decision table can be checked directly. The
// table is where the subtle behaviour lives -- which mode grabs the picture, when
// Alt still pans, what a release ends -- and it was previously only reachable through
// synthesised GDK events, which GTK4 keeps opaque. Two bugs lived here for exactly
// that reason: grabbing the picture on any press at all, and letting that grab beat
// Alt+drag pan.
enum class CanvasInput {
    None,
    BeginStroke,
    ContinueStroke,
    EndStroke,
    BeginPan,
    GrabImage,
    DragImage,
    DropImage,
};

// Touch types this canvas acts on, as opposed to the emulated pointer events a
// touch also produces. GDK reports both for a finger, and the finger is the
// honest one: the touch event carries the contact itself, while the pointer
// events synthesised from it belong to whichever device the compositor picked.
bool isCanvasTouchType(GdkEventType type);

// `activeHasImage` is whether the selected layer holds a picture: move mode drags that
// one, from wherever the press lands.
//
// Touch types are folded into the same table as the pointer ones. A finger is not a
// lesser pointer -- it starts, continues and ends a stroke by exactly the same rules,
// so keeping it in a second table would be a second set of rules to keep in step.
CanvasInput classifyInput(GdkEventType type, bool moveMode, bool drawing, bool panning,
                          bool movingImage, bool activeHasImage, bool altDown);

class Canvas {
public:
    explicit Canvas(AppState* state);
    ~Canvas();

    GtkWidget* widget() const { return widget_; }
    AppState* state() const { return state_; }

    Viewport& viewport() { return viewport_; }

    void fitToWidget();
    void zoomBy(double factor, double anchorX, double anchorY);

    // Called once the widget has been given a real size, which is the first
    // moment a fit-to-screen can produce a meaningful zoom.
    void onResize();

    void setStatusCallback(std::function<void(const std::string&)> cb) { status_ = std::move(cb); }
    void setZoomLabelCallback(std::function<void(const std::string&)> cb) { zoomLabel_ = std::move(cb); }
    // Fires when the pen's eraser end goes down or comes back up.
    void setEraserEndCallback(std::function<void(bool)> cb) { eraserEnd_ = std::move(cb); }
    // Effective pressure for the most recent sample, 0..1. Fired for pen and
    // non-pen input alike so a mouse user sees the meter move; the value is what
    // the brush will actually use, so the readout matches the mark on the page.
    void setPressureCallback(std::function<void(double)> cb) { pressure_ = std::move(cb); }

    bool penSeen() const { return penSeen_; }
    bool touchSeen() const { return touchSeen_; }

    // Move mode: a press on the active image layer drags the image instead of
    // drawing. Off by default, because drawing is the primary job and a canvas that
    // moved pictures whenever a press landed on one would silently refuse to draw
    // over them.
    bool moveMode() const { return moveMode_; }
    void setMoveMode(bool on);
    void cancelImageMove();


    // Pressure the current brush would lay down at the nib. Painting it in the
    // toolbar is the only way to tell a soft brush from a hard one before
    // committing a stroke to the page.
    double brushPreviewRadius() const;
    double brushPreviewAlpha() const;

private:
    static void onDraw(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer userData);
    static gboolean onRawEvent(GtkEventController* ctrl, GdkEvent* event, Canvas* self);
    static void onScroll(GtkEventControllerScroll* ctrl, double dx, double dy, Canvas* self);

    static bool penToolType(GdkDeviceToolType type);

    void paintDocument(cairo_t* cr, int width, int height) const;

    // Paints every visible layer into the current context, in document space.
    void paintLayers(cairo_t* cr) const;

    // Clears the filtered pressure and the speed-derived fallback. Called on
    // every stroke boundary so no stroke inherits state from the previous one.
    void resetPressureState();

    // The nib mark, painted at the last event position so the cursor and the
    // ink share one coordinate source.
    void paintNibCursor(cairo_t* cr) const;

    // Widget-space position of a raw event, plus pressure and tilt. Returns
    // false when the event carries no position at all. Tablet axes are cached,
    // so this updates member state and is not const.
    bool readSample(GdkEvent* event, Point* out);
    // True for pen-family input, including the eraser end.
    bool eventIsPen(GdkEvent* event) const;
    // Touch input is dropped while a pen has been seen, so a resting palm does
    // not draw on a convertible screen.
    bool eventIsIgnoredTouch(GdkEvent* event) const;
    // True for a pointer event GDK synthesised from a touch. These duplicate the touch
    // events, which are handled directly, and are therefore dropped.
    bool eventIsEmulatedTouchPointer(GdkEvent* event) const;

    // Whether this touch event belongs to the finger currently drawing.
    //
    // The first contact to land claims the canvas and later ones are refused, so a
    // second finger resting on the screen cannot drag the outline away mid-stroke,
    // and lifting it cannot end the stroke early. Contacts are told apart by their
    // event sequence, which is what GdkEventSequence exists for: a sequence links
    // the events of one contact and nothing else.
    //
    // Claims are dropped when the stroke ends, so the next finger down starts fresh
    // even if the previous one never sent an end event.
    bool touchIsDrawingContact(GdkEvent* event);

    void beginInput(const Point& raw, GdkModifierType mods);
    void moveInput(const Point& raw);
    void endInput();
    void reportPen(double pressure, double tilt);
    // Same for a finger, which has no pressure axis of its own.
    void reportTouch(double pressure);

    AppState* state_;
    GtkWidget* widget_ = nullptr;
    GtkEventController* raw_ = nullptr;
    GtkEventController* scroll_ = nullptr;

    Viewport viewport_;
    BoardTransform transform_;

    bool panning_ = false;
    bool moveMode_ = false;
    // True between the press that grabbed the image and the release that let it go.
    bool movingImage_ = false;
    double lastX_ = 0.0;
    double lastY_ = 0.0;
    bool penSeen_ = false;
    // Separate from penSeen_ because the two are not interchangeable: a stylus makes
    // touch suppressed, so a convertible that has reported a pen must not have its
    // touchscreen counted as a drawing surface as well.
    bool touchSeen_ = false;

    // Set once the user zooms or pans. Until then the canvas keeps refitting on
    // every resize, so the page fills the widget instead of being sized once
    // against a transient pre-layout allocation.
    bool viewTakenOver_ = false;

    // The touch contact that owns the canvas, and whether there is one at all.
    // The two are kept apart because a backend that supplies no event sequence
    // still has to be able to draw: null means "unnamed finger", not "no finger".
    bool touchClaimed_ = false;
    const GdkEventSequence* touchSequence_ = nullptr;

    // Tokens for the AppState listeners this canvas owns, so the destructor can
    // drop them instead of leaving callbacks pointing at freed memory.
    std::vector<size_t> listenerTokens_;

    // Last event position in widget coordinates. Both the nib cursor and the
    // stroke mapping read these, which is what keeps them identical.
    double cursorX_ = 0.0;
    double cursorY_ = 0.0;
    bool cursorValid_ = false;

    // Tablet axes only appear on some events of a stroke, so the last real
    // reading is carried forward instead of snapping back to the default.
    double lastPressure_ = 0.5;
    double lastTiltX_ = 0.0;
    double lastTiltY_ = 0.0;
    bool eraserDown_ = false;

    // Filtered pressure output and the derived-from-speed pressure used for
    // devices with no pressure axis.
    bool pressurePrimed_ = false;
    double speedPressure = 0.5;

    // Stabilizer state, kept in widget space and seeded on press so the first
    // sample is not pulled toward a stale position.
    double smoothX_ = 0.0;
    double smoothY_ = 0.0;
    bool sampleValid_ = false;

    double brushPressure() const;

    std::function<void(const std::string&)> status_;
    std::function<void(const std::string&)> zoomLabel_;
    std::function<void(bool)> eraserEnd_;
    std::function<void(double)> pressure_;
};

}  // namespace stylus