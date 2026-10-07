#include "canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace stylus {

namespace {

// Tablet reports are coarse at the very start of a stroke: pressure often
// arrives as 0 before the tip loads up. A stroke that begins at 0 would draw a
// hairline, so the first samples are treated as light but not zero.
constexpr double kMinStrokePressure = 0.04;

// A mouse or finger reports no pressure axis. Rather than draw every stroke at a
// flat half pressure, pressure is derived from how fast the nib is travelling:
// slow strokes read as deliberate and heavy, fast ones as light. Without this
// the pressure-width curve is dead code for anyone without a tablet.
constexpr double kPressureFromSpeedFloor = 0.35;   // slowest movement
constexpr double kPressureFromSpeedCeiling = 0.75;  // fastest movement
constexpr double kSpeedForMinPressure = 26.0;      // widget px per sample

// One-pole filter on the pressure axis. Both rates are high enough to stay
// responsive; the asymmetry keeps a fast press from being mistaken for a slow
// one when the pen lands.
constexpr double kPressureRiseRate = 0.65;
constexpr double kPressureFallRate = 0.5;

// How far each new position pulls the stabilizer's running position. High enough
// that the stroke keeps up with the hand, low enough to take the edge off pen
// jitter. Applied to the raw event position rather than to the last committed
// sample, so the lag does not grow with the length of the stroke.
constexpr double kStabilizeWeight = 0.5;

double filterPressure(double previous, double raw, bool primed) {
    if (!primed) return raw;
    const double rate = raw > previous ? kPressureRiseRate : kPressureFallRate;
    return previous + (raw - previous) * rate;
}

double pressureFromSpeed(double moved) {
    const double t = clampd(moved / kSpeedForMinPressure, 0.0, 1.0);
    return kPressureFromSpeedCeiling - (kPressureFromSpeedCeiling - kPressureFromSpeedFloor) * t;
}

}  // namespace

bool Canvas::penToolType(GdkDeviceToolType type) {
    switch (type) {
        case GDK_DEVICE_TOOL_TYPE_PEN:
        case GDK_DEVICE_TOOL_TYPE_ERASER:
        case GDK_DEVICE_TOOL_TYPE_BRUSH:
        case GDK_DEVICE_TOOL_TYPE_PENCIL:
        case GDK_DEVICE_TOOL_TYPE_AIRBRUSH:
            return true;
        default:
            return false;
    }
}

Canvas::Canvas(AppState* state)
    : state_(state), widget_(gtk_drawing_area_new()),
      transform_(state->document().width(), state->document().height()) {
    GtkDrawingArea* area = GTK_DRAWING_AREA(widget_);

    gtk_widget_set_focusable(widget_, TRUE);
    gtk_widget_set_hexpand(widget_, TRUE);
    gtk_widget_set_vexpand(widget_, TRUE);
    gtk_widget_add_css_class(widget_, "board");
    gtk_widget_set_cursor_from_name(widget_, "none");

    gtk_drawing_area_set_draw_func(area, &Canvas::onDraw, this, nullptr);

    // One raw controller replaces the click, drag and motion gestures. Gestures
    // wrap events and re-derive coordinates through a surface origin that is
    // never established for tablet tools, so they reported the pen at the top
    // left of the canvas. Reading the event directly keeps the stroke anchored
    // under the nib.
    raw_ = gtk_event_controller_legacy_new();
    g_signal_connect(raw_, "event", G_CALLBACK(&Canvas::onRawEvent), this);
    gtk_widget_add_controller(widget_, raw_);

    scroll_ = gtk_event_controller_scroll_new(
        static_cast<GtkEventControllerScrollFlags>(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
                                                   GTK_EVENT_CONTROLLER_SCROLL_HORIZONTAL));
    g_signal_connect(scroll_, "scroll", G_CALLBACK(&Canvas::onScroll), this);
    gtk_widget_add_controller(widget_, GTK_EVENT_CONTROLLER(scroll_));

    // Kept as tokens rather than discarded: AppState outlives this widget, so a
    // notification arriving after destruction would call a freed Canvas.
    listenerTokens_.push_back(state_->setChanged([this]() { gtk_widget_queue_draw(widget_); }));
    listenerTokens_.push_back(state_->setLayersChanged([this]() { gtk_widget_queue_draw(widget_); }));
    listenerTokens_.push_back(
        state_->setLayerPixelChanged([this]() { gtk_widget_queue_draw(widget_); }));
}

Canvas::~Canvas() {
    // The listeners hold a raw `this` and AppState outlives the widget, so they
    // have to be dropped here rather than left to fire into freed memory on the
    // next stroke.
    for (size_t token : listenerTokens_) {
        if (state_) state_->clearListener(token);
    }
    listenerTokens_.clear();
}

double Canvas::brushPressure() const {
    return cursorValid_ ? lastPressure_ : 0.5;
}

double Canvas::brushPreviewRadius() const {
    // The footprint is drawn in document pixels, so it has to be scaled before it
    // becomes a widget-space size for the toolbar.
    return radiusFor({0.0, 0.0, brushPressure(), 0.0, 0.0}, state_->brush());
}

double Canvas::brushPreviewAlpha() const {
    switch (state_->brush().kind) {
        case BrushKind::Eraser:
            return 0.45;
        case BrushKind::Marker:
            return 0.35;
        case BrushKind::Pencil:
            return 0.9;
        case BrushKind::Pen:
        default:
            return 1.0;
    }
}

bool Canvas::readSample(GdkEvent* event, Point* out) {
    *out = Point{};

    double sx = 0.0;
    double sy = 0.0;
    if (!gdk_event_get_position(event, &sx, &sy)) return false;

    double wx = sx;
    double wy = sy;

    GtkWidget* root = GTK_WIDGET(gtk_widget_get_root(widget_));
    if (root != nullptr) {
        graphene_point_t p_in = GRAPHENE_POINT_INIT(static_cast<float>(sx), static_cast<float>(sy));
        graphene_point_t p_out;
        if (gtk_widget_compute_point(root, widget_, &p_in, &p_out)) {
            wx = p_out.x;
            wy = p_out.y;
        }
    }

    // Distance moved since the previous sample, used to derive pressure from
    // speed for devices that report none.
    const double moved = sampleValid_ ? std::hypot(wx - cursorX_, wy - cursorY_) : 0.0;
    speedPressure = sampleValid_ ? pressureFromSpeed(moved) : kPressureFromSpeedCeiling;

    out->x = wx;
    out->y = wy;

    cursorX_ = wx;
    cursorY_ = wy;
    cursorValid_ = true;

    // Only a pen carries pressure and tilt. Mouse and touch get pressure derived
    // from speed, so a stroke still tapers instead of laying down one flat width.
    if (eventIsPen(event)) {
        double pressure = 0.0;
        if (gdk_event_get_axis(event, GDK_AXIS_PRESSURE, &pressure)) {
            // Tablets report pressure with a step or two of jitter. Feeding the
            // raw value straight into the width curve makes the stroke edge
            // shimmer, so it goes through a one-pole filter that is quick to
            // rise and quick to fall: a deliberate ramp still tracks the hand
            // within a sample or two, while noise stops moving the outline.
            lastPressure_ = filterPressure(lastPressure_, clampd(pressure, 0.0, 1.0), pressurePrimed_);
            pressurePrimed_ = true;
        }
        double tiltX = 0.0;
        if (gdk_event_get_axis(event, GDK_AXIS_XTILT, &tiltX)) lastTiltX_ = tiltDegrees(tiltX);
        double tiltY = 0.0;
        if (gdk_event_get_axis(event, GDK_AXIS_YTILT, &tiltY)) lastTiltY_ = tiltDegrees(tiltY);

        out->pressure = lastPressure_;
        out->tiltX = lastTiltX_;
        out->tiltY = lastTiltY_;
    } else {
        out->pressure = speedPressure;
        out->tiltX = 0.0;
        out->tiltY = 0.0;
    }

    // A freshly landed tip reports zero pressure for a frame or two, which
    // would pinch the stroke to nothing at its most visible point.
    if (out->pressure < kMinStrokePressure) out->pressure = kMinStrokePressure;

    sampleValid_ = true;
    return true;
}

void Canvas::resetPressureState() {
    // A fresh stroke must not inherit the previous one's filtered pressure, or
    // the first millimetre of every stroke is shaded by whatever the pen was
    // doing before it was picked up.
    lastPressure_ = 0.5;
    pressurePrimed_ = false;
    speedPressure = kPressureFromSpeedCeiling;
    sampleValid_ = false;
}

bool Canvas::eventIsPen(GdkEvent* event) const {
    GdkDeviceTool* tool = gdk_event_get_device_tool(event);
    if (tool != nullptr) return penToolType(gdk_device_tool_get_tool_type(tool));

    GdkDevice* device = gdk_event_get_device(event);
    return device != nullptr && gdk_device_get_source(device) == GDK_SOURCE_PEN;
}

bool Canvas::eventIsIgnoredTouch(GdkEvent* event) const {
    if (!penSeen_) return false;
    GdkDevice* device = gdk_event_get_device(event);
    return device != nullptr && gdk_device_get_source(device) == GDK_SOURCE_TOUCHSCREEN;
}

bool Canvas::eventIsEmulatedTouchPointer(GdkEvent* event) const {
    if (!gdk_event_get_pointer_emulated(event)) return false;
    GdkDevice* device = gdk_event_get_device(event);
    if (device == nullptr) return false;
    const auto source = gdk_device_get_source(device);
    return source == GDK_SOURCE_TOUCHSCREEN || source == GDK_SOURCE_TOUCHPAD;
}

bool Canvas::touchIsDrawingContact(GdkEvent* event) {
    const GdkEventSequence* sequence = gdk_event_get_event_sequence(event);
    const GdkEventType type = gdk_event_get_event_type(event);

    if (type == GDK_TOUCH_BEGIN) {
        // A second finger landing while one is already drawing is refused rather
        // than queued: it must not steal the canvas, and the finger already down
        // keeps its claim.
        if (touchClaimed_) return false;
        touchClaimed_ = true;
        touchSequence_ = sequence;
        return true;
    }

    // An update or end with no finger on the canvas belongs to a contact this
    // widget never saw land, which happens after a cancel. Acting on it would
    // continue or end a stroke nobody started.
    if (!touchClaimed_) return false;

    // Only the claiming finger's own follow-up events are acted on. Without this a
    // second finger lifting would end the first one's stroke part-way along.
    //
    // A null sequence is not treated as a mismatch. GDK only supplies one where the
    // backend does, and refusing every anonymous contact would mean a touchscreen
    // that reports no sequences cannot draw at all.
    if (sequence != nullptr && touchSequence_ != nullptr && sequence != touchSequence_) {
        return false;
    }

    if (type == GDK_TOUCH_END) {
        touchClaimed_ = false;
        touchSequence_ = nullptr;
    }
    return true;
}

void Canvas::reportPen(double pressure, double tilt) {
    penSeen_ = true;
    if (!status_) return;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Stylus active · pressure %.2f · tilt %d°", pressure,
                  static_cast<int>(std::lround(tilt)));
    status_(buf);
}

void Canvas::reportTouch(double pressure) {
    touchSeen_ = true;
    if (!status_) return;
    // Says where the pressure came from rather than showing the number bare. A finger
    // has no pressure axis, so the value is derived from speed, and a reading that
    // moves on its own looks like a hardware fault unless that is said out loud.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Touch active · pressure %.2f (from speed)", pressure);
    status_(buf);
}

void Canvas::beginInput(const Point& raw, GdkModifierType mods) {
    if (mods & GDK_ALT_MASK) {
        panning_ = true;
        lastX_ = raw.x;
        lastY_ = raw.y;
        return;
    }

    // A pen that starts in range reports its last pressure before the tip
    // settles; resetting here keeps a new stroke independent of the previous.
    lastTiltX_ = 0.0;
    lastTiltY_ = 0.0;
    resetPressureState();

    panning_ = false;
    // Seed the stabilizer on the nib's real position, so the first sample is not
    // dragged toward wherever the pen happened to be before it landed.
    smoothX_ = raw.x;
    smoothY_ = raw.y;
    cursorX_ = raw.x;
    cursorY_ = raw.y;
    cursorValid_ = true;

    Point p = transform_.toDocument(raw.x, raw.y);
    p.pressure = raw.pressure;
    p.tiltX = raw.tiltX;
    p.tiltY = raw.tiltY;
    state_->beginStroke(p);
}

void Canvas::moveInput(const Point& raw) {
    if (panning_) {
        viewTakenOver_ = true;
        viewport_.panX += raw.x - lastX_;
        viewport_.panY += raw.y - lastY_;
        lastX_ = raw.x;
        lastY_ = raw.y;
        gtk_widget_queue_draw(widget_);
        return;
    }

    if (!state_->drawing()) return;

    double useX = raw.x;
    double useY = raw.y;
    if (state_->settings().stabilize) {
        // Track the raw hand position, not the last committed sample. The pen's
        // own jitter is what needs removing; the user's intent is not.
        const Point smoothed =
            smoothToward({smoothX_, smoothY_, raw.pressure, raw.tiltX, raw.tiltY}, raw, kStabilizeWeight);
        smoothX_ = smoothed.x;
        smoothY_ = smoothed.y;
        useX = smoothX_;
        useY = smoothY_;
    }

    Point p = transform_.toDocument(useX, useY);
    p.pressure = raw.pressure;
    p.tiltX = raw.tiltX;
    p.tiltY = raw.tiltY;

    state_->extendStroke(p);
}

void Canvas::endInput() {
    if (panning_) {
        panning_ = false;
        gtk_widget_queue_draw(widget_);
        return;
    }
    state_->endStroke();
    resetPressureState();
}

bool isCanvasTouchType(GdkEventType type) {
    return type == GDK_TOUCH_BEGIN || type == GDK_TOUCH_UPDATE || type == GDK_TOUCH_END;
}

CanvasInput classifyInput(GdkEventType type, bool moveMode, bool drawing, bool panning,
                            bool movingImage, bool activeHasImage, bool altDown) {
    switch (type) {
        // A finger is folded into the pointer cases rather than given its own, so
        // both start, continue and end a stroke under exactly the same rules. Its
        // pressure comes from the same speed-derived fallback a mouse uses.
        case GDK_BUTTON_PRESS:
        case GDK_TOUCH_BEGIN:
            if (drawing || panning) return CanvasInput::None;
            if (altDown) return CanvasInput::BeginPan;
            if (moveMode && activeHasImage) return CanvasInput::GrabImage;
            return CanvasInput::BeginStroke;

        case GDK_MOTION_NOTIFY:
        case GDK_TOUCH_UPDATE:
            if (movingImage) return CanvasInput::DragImage;
            if (drawing || panning) return CanvasInput::ContinueStroke;
            return CanvasInput::None;

        case GDK_BUTTON_RELEASE:
        case GDK_TOUCH_END:
            if (movingImage) return CanvasInput::DropImage;
            return CanvasInput::EndStroke;

        default:
            return CanvasInput::None;
    }
}

gboolean Canvas::onRawEvent(GtkEventController*, GdkEvent* event, Canvas* self) {
    const GdkEventType type = gdk_event_get_event_type(event);

    if (!self->state_->isImageMoving()) {
        self->movingImage_ = false;
    }

    if (type == GDK_PROXIMITY_OUT || type == GDK_LEAVE_NOTIFY || type == GDK_GRAB_BROKEN ||
        type == GDK_TOUCH_CANCEL) {
        if (self->state_->drawing()) {
            self->state_->endStroke();
            self->resetPressureState();
            // A cancel reaches here too, and it is not the nib leaving range, so the
            // message has to name whichever actually happened.
            if (self->status_) {
                self->status_(type == GDK_TOUCH_CANCEL
                                  ? "Touch cancelled — stroke ended"
                                  : "Stylus left range — stroke ended");
            }
        }
        // A cancelled contact sends no TOUCH_END, so the claim would otherwise stay
        // held and every finger after it would be refused as a second contact.
        self->touchClaimed_ = false;
        self->touchSequence_ = nullptr;
        if (self->movingImage_ || self->state_->isImageMoving()) {
            self->movingImage_ = false;
            self->state_->cancelActiveImageMove();
        }
        self->sampleValid_ = false;
        self->panning_ = false;
        if (self->eraserDown_) {
            self->eraserDown_ = false;
            if (self->eraserEnd_) self->eraserEnd_(false);
        }
        return GDK_EVENT_PROPAGATE;
    }

    const bool touchEvent = isCanvasTouchType(type);

    // The pointer events GDK synthesises from a touch are dropped, because the touch
    // events themselves are now handled. Left in, they arrive as a second, partial
    // copy of the same gesture: the emulated pointer belongs to whichever contact the
    // compositor decided owns it, so with two fingers down it reports one finger's
    // motion and the other's release, and the stroke jumps about between them.
    // Restricted to touch-sourced devices, so a real mouse on a touchscreen, which is
    // not emulated but looks the same, still draws.
    //
    // Ahead of the eraser check below, which would otherwise read every finger as the
    // pen not being an eraser and keep switching the brush back mid-stroke.
    if (!touchEvent && self->eventIsEmulatedTouchPointer(event)) return GDK_EVENT_STOP;

    if (type == GDK_BUTTON_PRESS) {
        GdkDeviceTool* tool = gdk_event_get_device_tool(event);
        const bool eraser =
            tool != nullptr && gdk_device_tool_get_tool_type(tool) == GDK_DEVICE_TOOL_TYPE_ERASER;
        if (eraser && !self->eraserDown_) {
            self->eraserDown_ = true;
            if (self->eraserEnd_) self->eraserEnd_(true);
        } else if (!eraser && self->eraserDown_) {
            self->eraserDown_ = false;
            if (self->eraserEnd_) self->eraserEnd_(false);
        }
    }

    if (type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE) {
        const guint button = gdk_button_event_get_button(event);
        if (button != GDK_BUTTON_PRIMARY) {
            if (self->movingImage_ || self->state_->isImageMoving()) {
                self->movingImage_ = false;
                self->state_->cancelActiveImageMove();
            }
            return GDK_EVENT_PROPAGATE;
        }
    } else if (type != GDK_MOTION_NOTIFY && !touchEvent) {
        return GDK_EVENT_PROPAGATE;
    }

    // A finger is followed through its touch events, not through the pointer events
    // GDK synthesises from them. The synthesised pair describes "a pointer happened
    // to move", which arrives with no contact behind it: a palm resting on the screen
    // mid-stroke then drags the outline, and lifting a finger the canvas never saw
    // arrive ends whatever stroke is running. The touch events carry the contact
    // itself, so they are the ones with an answer to "is the finger still down".
    if (self->eventIsIgnoredTouch(event)) return GDK_EVENT_STOP;
    if (touchEvent && !self->touchIsDrawingContact(event)) return GDK_EVENT_STOP;

    Point sample;
    if (!self->readSample(event, &sample)) return GDK_EVENT_PROPAGATE;

    if (self->eventIsPen(event)) {
        self->reportPen(sample.pressure, std::hypot(sample.tiltX, sample.tiltY));
    } else if (touchEvent) {
        self->reportTouch(sample.pressure);
    }

    if (self->pressure_) self->pressure_(self->brushPressure());

    const GdkModifierType mods = gdk_event_get_modifier_state(event);
    const CanvasInput action =
        classifyInput(type, self->moveMode_, self->state_->drawing(), self->panning_,
                      self->movingImage_, self->state_->canMoveActiveLayer(),
                      (mods & GDK_ALT_MASK) != 0);

    switch (action) {
        case CanvasInput::GrabImage:
            if (!self->state_->beginMoveActiveImage()) {
                self->movingImage_ = false;
                return GDK_EVENT_STOP;
            }
            self->movingImage_ = true;
            self->lastX_ = sample.x;
            self->lastY_ = sample.y;
            self->cursorX_ = sample.x;
            self->cursorY_ = sample.y;
            self->cursorValid_ = true;
            gtk_widget_queue_draw(self->widget_);
            return GDK_EVENT_STOP;

        case CanvasInput::DragImage: {
            const double zoom = self->transform_.zoom();
            if (zoom > 1e-6) {
                const double dx = (sample.x - self->lastX_) / zoom;
                const double dy = (sample.y - self->lastY_) / zoom;
                if (std::isfinite(dx) && std::isfinite(dy)) {
                    self->state_->moveActiveImageBy(dx, dy);
                }
            }
            self->lastX_ = sample.x;
            self->lastY_ = sample.y;
            self->cursorX_ = sample.x;
            self->cursorY_ = sample.y;
            self->cursorValid_ = true;
            gtk_widget_queue_draw(self->widget_);
            return GDK_EVENT_STOP;
        }

        case CanvasInput::DropImage:
            if (self->movingImage_ || self->state_->isImageMoving()) {
                self->movingImage_ = false;
                self->state_->commitActiveImageMove();
                gtk_widget_queue_draw(self->widget_);
            }
            return GDK_EVENT_STOP;

        case CanvasInput::BeginPan:
        case CanvasInput::BeginStroke:
            self->beginInput(sample, mods);
            break;

        case CanvasInput::ContinueStroke:
            self->moveInput(sample);
            break;

        case CanvasInput::EndStroke:
            self->endInput();
            break;

        case CanvasInput::None:
            // Hover: repaint so the nib cursor follows the pointer.
            if (!self->state_->drawing() && !self->panning_) {
                gtk_widget_queue_draw(self->widget_);
            }
            break;
    }

    return GDK_EVENT_PROPAGATE;
}

void Canvas::paintDocument(cairo_t* cr, int width, int height) const {
    const double zoom = transform_.zoom();
    const double ox = transform_.documentOriginX();
    const double oy = transform_.documentOriginY();
    const double docW = state_->document().width() * zoom;
    const double docH = state_->document().height() * zoom;

    // The page colour covers the whole widget, not just the page rectangle: a
    // margin left by a fit-to-screen zoom then reads as more sheet rather than as
    // a dark band, so the canvas is one continuous page with its edges falling
    // outside the window.
    const Color& page = state_->document().background();
    cairo_save(cr);
    cairo_set_source_rgb(cr, page.r, page.g, page.b);
    cairo_rectangle(cr, 0.0, 0.0, width, height);
    cairo_fill(cr);

    // A hairline at the page edge, but only where the page does not already reach
    // the widget border. Drawn outside the clip below, so it is the boundary and
    // not a frame around the whole canvas.
    //
    // It was a fixed translucent black, which disappears the moment the page goes
    // dark and the edge stops telling you where the sheet ends. Picking the line
    // against the page's own brightness keeps that boundary readable on any
    // background.
    const bool insetX = ox > 0.5 || ox + docW < width - 0.5;
    const bool insetY = oy > 0.5 || oy + docH < height - 0.5;
    if (insetX || insetY) {
        // Rec. 601 luma is close enough to perceived lightness for picking a
        // contrasting line, and costs less than a proper gamma-aware conversion.
        const double luma = 0.299 * page.r + 0.587 * page.g + 0.114 * page.b;
        const bool dark = luma < 0.5;
        const double v = dark ? 1.0 : 0.0;
        cairo_set_source_rgba(cr, v, v, v, dark ? 0.28 : 0.22);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, std::floor(ox) + 0.5, std::floor(oy) + 0.5, std::round(docW),
                        std::round(docH));
        cairo_stroke(cr);
    }
    cairo_restore(cr);

    // transform_.apply pushes, so the matching restore is at the end of this function
    // rather than immediately after the call.
    transform_.apply(cr);

    // The ruling, in document space so it zooms with the artwork instead of
    // floating over it at a fixed screen pitch. Drawn here rather than in
    // paintLayers because it belongs to the page, not to any layer. The zoom and
    // page colour go along so the ink holds one device pixel and stays readable on
    // a dark page.
    const Document& doc = state_->document();
    paintPaper(cr, doc.paper(), doc.background(), doc.width(), doc.height(), transform_.zoom());

    const bool liveStroke = state_->drawing() && !state_->liveStroke().empty();

    paintLayers(cr);

    // The live stroke has to be composited with the layers, not over the page.
    // An eraser draws with CAIRO_OPERATOR_CLEAR, which straight onto the widget
    // would erase the page too, leaving a hole rather than revealing the layer
    // below. Compositing into a scratch group first confines the clear to the
    // artwork.
    const bool liveEraser = liveStroke && state_->brush().kind == BrushKind::Eraser;
    if (liveEraser) cairo_push_group(cr);

    paintLayers(cr);
    if (liveStroke) drawStroke(cr, state_->liveStroke(), state_->brush());

    if (liveEraser) {
        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
    }

    cairo_restore(cr);

    paintNibCursor(cr);
}

void Canvas::paintLayers(cairo_t* cr) const {
    const Document& doc = state_->document();

    // Paint the stack from the cached composite rather than every layer. Without
    // this a long stroke repaints the whole 1600x1100 stack on every motion event,
    // which is what made drawing stutter on a big canvas. The cache is invalidated
    // whenever a layer's pixels change, so the only cost left per frame is one
    // scaled blit.
    //
    // Clip to the page so artwork never spills past its own bounds; the margin
    // around it is already page-coloured.
    cairo_save(cr);
    cairo_rectangle(cr, 0.0, 0.0, doc.width(), doc.height());
    cairo_clip(cr);

    const SurfacePtr flat = doc.composite();
    if (flat) {
        cairo_set_source_surface(cr, flat.get(), 0.0, 0.0);
        cairo_paint(cr);
    }
    cairo_restore(cr);
}

void Canvas::paintNibCursor(cairo_t* cr) const {
    if (!cursorValid_) return;

    // Drawn in widget space, unscaled and unzoomed, because it marks the nib
    // and not the document: it is drawn from the very same event coordinates
    // the stroke samples are mapped from, so what the crosshair points at is
    // exactly where the ink starts. The stock arrow cannot do this — its
    // hotspot is a corner, so the pointer reads as offset from the tip.
    double x = cursorX_;
    double y = cursorY_;
    if (state_->drawing() && !state_->liveStroke().empty()) {
        const Point& last = state_->liveStroke().points.back();
        x = last.x * transform_.zoom() + transform_.documentOriginX();
        y = last.y * transform_.zoom() + transform_.documentOriginY();
    }

    const double gap = 5.0;
    const double arm = 9.0;
    const double radius = 2.0;

    cairo_save(cr);

    // A dark halo first keeps the mark legible over the artwork without needing to
    // know what colour is underneath it.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, x - gap - arm, y);
    cairo_line_to(cr, x - gap, y);
    cairo_move_to(cr, x + gap, y);
    cairo_line_to(cr, x + gap + arm, y);
    cairo_move_to(cr, x, y - gap - arm);
    cairo_line_to(cr, x, y - gap);
    cairo_move_to(cr, x, y + gap);
    cairo_line_to(cr, x, y + gap + arm);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x - gap - arm, y);
    cairo_line_to(cr, x - gap, y);
    cairo_move_to(cr, x + gap, y);
    cairo_line_to(cr, x + gap + arm, y);
    cairo_move_to(cr, x, y - gap - arm);
    cairo_line_to(cr, x, y - gap);
    cairo_move_to(cr, x, y + gap);
    cairo_line_to(cr, x, y + gap + arm);
    cairo_stroke(cr);

    cairo_arc(cr, x, y, radius, 0.0, 2.0 * M_PI);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
    cairo_set_line_width(cr, 3.0);
    cairo_stroke(cr);

    cairo_arc(cr, x, y, radius, 0.0, 2.0 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    cairo_restore(cr);
}

void Canvas::onDraw(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer userData) {
    auto* self = static_cast<Canvas*>(userData);
    (void)area;

    self->transform_.setWidgetSize(width, height);
    self->transform_.setViewport(self->viewport_.zoom, self->viewport_.panX, self->viewport_.panY);

    // GTK4 already hands the draw callback a context in logical pixels with an
    // identity matrix: the allocation it passes is unscaled, so the widget
    // coordinates the events carry and the coordinates painted here share one
    // space. Scaling by the widget's scale factor here used to move the ink
    // away from the nib on every HiDPI display.
    self->paintDocument(cr, width, height);
}

void Canvas::onScroll(GtkEventControllerScroll* ctrl, double dx, double dy, Canvas* self) {
    const GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(ctrl));
    if (mods & GDK_CONTROL_MASK) {
        self->zoomBy(std::exp(-dy * 0.002), -1.0, -1.0);
    } else {
        self->viewTakenOver_ = true;
        self->viewport_.panX -= dx;
        self->viewport_.panY -= dy;
        gtk_widget_queue_draw(self->widget_);
    }
}

void Canvas::setMoveMode(bool on) {
    if (moveMode_ == on) return;
    if (movingImage_) {
        movingImage_ = false;
        state_->cancelActiveImageMove();
    }
    moveMode_ = on;
    gtk_widget_queue_draw(widget_);
}

void Canvas::cancelImageMove() {
    if (movingImage_) {
        movingImage_ = false;
        state_->cancelActiveImageMove();
        gtk_widget_queue_draw(widget_);
    }
}

void Canvas::zoomBy(double factor, double anchorX, double anchorY) {
    // An explicit fit resets the takeover; any other zoom is the user taking over.
    if (std::fabs(factor - 1.0) > 1e-9) viewTakenOver_ = true;

    const double oldZoom = viewport_.zoom;
    const double nextZoom = viewport_.clampZoom(oldZoom * factor);
    if (std::abs(nextZoom - oldZoom) < 1e-9) return;

    if (anchorX >= 0.0 && anchorY >= 0.0) {
        // Keep the anchor point fixed on screen while scaling.
        const double cx = anchorX - transform_.documentOriginX();
        const double cy = anchorY - transform_.documentOriginY();
        viewport_.panX -= cx * (nextZoom - oldZoom) / oldZoom;
        viewport_.panY -= cy * (nextZoom - oldZoom) / oldZoom;
    }
    viewport_.zoom = nextZoom;

    if (zoomLabel_) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(nextZoom * 100.0)));
        zoomLabel_(buf);
    }
    gtk_widget_queue_draw(widget_);
}

void Canvas::onResize() {
    const double w = gtk_widget_get_width(widget_);
    const double h = gtk_widget_get_height(widget_);
    if (w <= 1.0 || h <= 1.0) return;

    // Keep the page filling the canvas until the user takes over the view.
    //
    // Fitting once on the first resize was not enough: the first allocation is a
    // transient size from before the sidebar and status bar are laid out, so the
    // zoom was locked to that and the page ended up floating in a margin. Every
    // later resize is re-fitted instead, which is what makes the canvas stay
    // full-bleed as the window is resized.
    if (viewTakenOver_) return;
    fitToWidget();
}

void Canvas::fitToWidget() {
    // Back to filling the canvas on every resize.
    viewTakenOver_ = false;

    const double w = gtk_widget_get_width(widget_);
    const double h = gtk_widget_get_height(widget_);
    if (w <= 1.0 || h <= 1.0) return;

    // Fill the canvas: the page is scaled to cover the widget rather than fit
    // inside it, so there is no margin and no letterboxing. The document keeps its
    // own aspect ratio, so the excess falls off one axis and is simply not drawn.
    const double zx = w / state_->document().width();
    const double zy = h / state_->document().height();
    viewport_.zoom = viewport_.clampZoom(std::max(zx, zy));
    viewport_.panX = 0.0;
    viewport_.panY = 0.0;

    if (zoomLabel_) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(viewport_.zoom * 100.0)));
        zoomLabel_(buf);
    }
    gtk_widget_queue_draw(widget_);
}

}  // namespace stylus