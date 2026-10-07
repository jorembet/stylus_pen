#include "window.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "color.h"

namespace stylus {

namespace {

// Runs when GTK tears down a row's click-gesture signal, which is what owns the
// LayerRow payload. Without this the payload would leak on every rebuild.
void freeLayerRow(gpointer data, GClosure*) {
    delete static_cast<LayerRow*>(data);
}

constexpr int kDocWidth = 1600;
constexpr int kDocHeight = 1100;

constexpr int kPaletteCount = 12;

const char* const kPalette[kPaletteCount] = {
    "#1b1b1f", "#4b5563", "#b91c1c", "#ea580c",
    "#ca8a04", "#16a34a", "#0ea5e9", "#4f46e5",
    "#7c3aed", "#db2777", "#78350f", "#f5f5f4",
};

struct ToolEntry {
    BrushKind kind;
    const char* label;
    const char* accel;
};

constexpr int kToolCount = 4;

const ToolEntry kTools[kToolCount] = {
    {BrushKind::Pen, "Pen", "B"},
    {BrushKind::Pencil, "Pencil", "N"},
    {BrushKind::Marker, "Marker", "M"},
    {BrushKind::Eraser, "Eraser", "E"},
};

// ---- toolbar icons ----
//
// The four brush instruments are painted rather than looked up in an icon theme: no
// theme carries a nib, a pencil, a chisel marker and a rubber as one set, and the
// closest thing any of them has for "pen" is a tablet, which is a device rather than a
// mark. Every glyph is stroked on one 16-unit grid in the widget's own CSS colour, so
// an icon is recoloured exactly like the label beside it and hover, active and disabled
// need no code at all. The file and view actions have unambiguous theme glyphs and use
// those, the same way the layer panel's eye does.
enum class ToolbarIcon {
    Pen,
    Pencil,
    Marker,
    Eraser,
    Move,
    Fit,
    HandwritingToText,
};

// The theme icons are rendered at 16 and the painted ones get the same box, so the two
// kinds sit on one baseline instead of stepping up and down across the bar.
constexpr int kIconPixels = 16;
constexpr double kIconGrid = 16.0;

ToolbarIcon toolIcon(BrushKind kind) {
    switch (kind) {
        case BrushKind::Pencil:
            return ToolbarIcon::Pencil;
        case BrushKind::Marker:
            return ToolbarIcon::Marker;
        case BrushKind::Eraser:
            return ToolbarIcon::Eraser;
        case BrushKind::Pen:
        default:
            return ToolbarIcon::Pen;
    }
}

// Rounded rectangle as a path, for the shapes that want softened corners.
void iconRoundedRect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

// A two-stroke head at (x, y), pointing along the unit vector (dx, dy).
void iconArrowHead(cairo_t* cr, double x, double y, double dx, double dy, double size) {
    const double barbX = -dy * size * 0.55;
    const double barbY = dx * size * 0.55;
    cairo_move_to(cr, x - dx * size + barbX, y - dy * size + barbY);
    cairo_line_to(cr, x, y);
    cairo_line_to(cr, x - dx * size - barbX, y - dy * size - barbY);
    cairo_stroke(cr);
}

// One wavy stroke: what a pen leaves that is not yet text.
void iconWave(cairo_t* cr, double x0, double x1, double y, double amplitude) {
    constexpr int kHalfWaves = 4;
    const double step = (x1 - x0) / kHalfWaves;
    cairo_move_to(cr, x0, y);
    for (int i = 0; i < kHalfWaves; ++i) {
        const double xa = x0 + step * i;
        const double xb = xa + step;
        const double peak = y + ((i % 2 == 0) ? -amplitude : amplitude);
        cairo_curve_to(cr, xa + step * 0.35, peak, xb - step * 0.35, peak, xb, y);
    }
    cairo_stroke(cr);
}

// The three writing instruments are each drawn along the y axis about the origin, and
// the caller turns that axis a quarter. Their barrel and tip coordinates therefore read
// the same in all three, and a positive quarter turn is what points the tip down and to
// the left -- the way a right hand holds a pen against the page.
void iconPen(cairo_t* cr) {
    cairo_rectangle(cr, -2.2, -7.2, 4.4, 9.0);
    cairo_stroke(cr);
    // The nib tapers to a point, and the slit down its middle is what marks this as a
    // pen rather than a third chisel.
    cairo_move_to(cr, -2.2, 1.8);
    cairo_line_to(cr, 0.0, 7.2);
    cairo_line_to(cr, 2.2, 1.8);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, 0.0, 4.4);
    cairo_line_to(cr, 0.0, 7.2);
    cairo_stroke(cr);
}

void iconPencil(cairo_t* cr) {
    cairo_rectangle(cr, -2.3, -7.2, 4.6, 8.0);
    cairo_stroke(cr);
    cairo_move_to(cr, -2.3, 0.8);
    cairo_line_to(cr, 0.0, 6.4);
    cairo_line_to(cr, 2.3, 0.8);
    cairo_close_path(cr);
    cairo_stroke(cr);
    // Where the wood meets the lead, then the ferrule at the far end.
    cairo_move_to(cr, -2.3, 0.8);
    cairo_line_to(cr, 2.3, 0.8);
    cairo_stroke(cr);
    cairo_move_to(cr, -2.3, -4.8);
    cairo_line_to(cr, 2.3, -4.8);
    cairo_stroke(cr);
}

void iconMarker(cairo_t* cr) {
    // Wider and squatter than the pen and the pencil, with a flat chisel instead of a
    // point: the highlighter shape that already reads as "marker".
    cairo_rectangle(cr, -3.0, -7.0, 6.0, 8.2);
    cairo_stroke(cr);
    cairo_move_to(cr, -3.0, -4.4);
    cairo_line_to(cr, 3.0, -4.4);
    cairo_stroke(cr);
    cairo_move_to(cr, -3.0, 1.2);
    cairo_line_to(cr, 3.0, 1.2);
    cairo_line_to(cr, 1.9, 5.9);
    cairo_line_to(cr, -1.9, 5.9);
    cairo_close_path(cr);
    cairo_stroke(cr);
}

void paintToolbarIcon(cairo_t* cr, ToolbarIcon icon, const GdkRGBA& color, int width,
                      int height) {
    cairo_save(cr);
    // Drawn on a 16-unit grid and scaled to whatever GTK allocated, so a glyph is
    // re-stroked at the display's own scale instead of being a scaled-up raster.
    cairo_scale(cr, width / kIconGrid, height / kIconGrid);
    cairo_set_line_width(cr, 1.4);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);

    switch (icon) {
        case ToolbarIcon::Pen:
        case ToolbarIcon::Pencil:
        case ToolbarIcon::Marker: {
            cairo_save(cr);
            cairo_translate(cr, 8.0, 8.0);
            cairo_rotate(cr, M_PI / 4.0);
            if (icon == ToolbarIcon::Pen) {
                iconPen(cr);
            } else if (icon == ToolbarIcon::Pencil) {
                iconPencil(cr);
            } else {
                iconMarker(cr);
            }
            cairo_restore(cr);
            break;
        }
        case ToolbarIcon::Eraser: {
            // A rubber block, tilted to sit on the same diagonal as the three
            // instruments above. The slanted edge is the rubber; the squared-off end
            // behind it is the sleeve it is set into.
            cairo_save(cr);
            cairo_translate(cr, 8.0, 8.4);
            cairo_rotate(cr, -M_PI / 10.0);
            iconRoundedRect(cr, -6.6, -2.8, 13.2, 5.6, 1.3);
            cairo_stroke(cr);
            cairo_move_to(cr, -6.6, 0.9);
            cairo_line_to(cr, -3.4, -2.8);
            cairo_stroke(cr);
            cairo_move_to(cr, -1.8, -2.8);
            cairo_line_to(cr, -1.8, 2.8);
            cairo_stroke(cr);
            cairo_restore(cr);
            break;
        }
        case ToolbarIcon::Move: {
            // Four heads on a cross: the one shape every app uses for "drag this".
            constexpr double cx = 8.0;
            constexpr double cy = 8.0;
            cairo_move_to(cr, cx, cy - 6.0);
            cairo_line_to(cr, cx, cy + 6.0);
            cairo_move_to(cr, cx - 6.0, cy);
            cairo_line_to(cr, cx + 6.0, cy);
            cairo_stroke(cr);
            iconArrowHead(cr, cx, cy - 6.0, 0.0, -1.0, 2.6);
            iconArrowHead(cr, cx, cy + 6.0, 0.0, 1.0, 2.6);
            iconArrowHead(cr, cx - 6.0, cy, -1.0, 0.0, 2.6);
            iconArrowHead(cr, cx + 6.0, cy, 1.0, 0.0, 2.6);
            break;
        }
        case ToolbarIcon::Fit: {
            // Corner brackets: the page, pulled inside the window. Brackets rather than
            // a frame, because the button's job is the frame itself, not the page.
            constexpr double x0 = 1.8;
            constexpr double y0 = 1.8;
            constexpr double x1 = 14.2;
            constexpr double y1 = 14.2;
            constexpr double arm = 3.6;
            cairo_move_to(cr, x0 + arm, y0);
            cairo_line_to(cr, x0, y0);
            cairo_line_to(cr, x0, y0 + arm);
            cairo_move_to(cr, x1 - arm, y0);
            cairo_line_to(cr, x1, y0);
            cairo_line_to(cr, x1, y0 + arm);
            cairo_move_to(cr, x1, y1 - arm);
            cairo_line_to(cr, x1, y1);
            cairo_line_to(cr, x1 - arm, y1);
            cairo_move_to(cr, x0, y1 - arm);
            cairo_line_to(cr, x0, y1);
            cairo_line_to(cr, x0 + arm, y1);
            cairo_stroke(cr);
            break;
        }
        case ToolbarIcon::HandwritingToText: {
            // Waves over ruled lines. Both buttons that put text on a page differ only
            // in what the strokes look like, so the glyph shows both: the marks the pen
            // made, and the same marks once they have been recognised.
            iconWave(cr, 2.0, 14.0, 5.0, 1.6);
            cairo_move_to(cr, 2.0, 10.8);
            cairo_line_to(cr, 14.0, 10.8);
            cairo_stroke(cr);
            cairo_move_to(cr, 2.0, 13.6);
            cairo_line_to(cr, 10.8, 13.6);
            cairo_stroke(cr);
            break;
        }
    }
    cairo_restore(cr);
}

void drawToolbarIcon(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                     gpointer userData) {
    if (width <= 0 || height <= 0) return;
    // Read the colour from the widget rather than from a constant, so a hovered, active
    // or disabled button recolours its glyph exactly as it recolours its label.
    GdkRGBA color;
    gtk_widget_get_color(GTK_WIDGET(area), &color);
    paintToolbarIcon(cr, static_cast<ToolbarIcon>(GPOINTER_TO_INT(userData)), color, width,
                     height);
}

// An icon out of the current icon theme.
GtkWidget* themedIcon(const char* name) {
    GtkWidget* image = gtk_image_new_from_icon_name(name);
    gtk_image_set_pixel_size(GTK_IMAGE(image), kIconPixels);
    return image;
}

// A painted glyph, in a box the size of the themed ones.
GtkWidget* paintedIcon(ToolbarIcon icon) {
    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, kIconPixels, kIconPixels);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), &drawToolbarIcon,
                                   GINT_TO_POINTER(static_cast<int>(icon)), nullptr);
    return area;
}

// Puts an icon inside a button, with `label` beside it, or on its own when `label` is
// null. A button with a box for a child reports no label of its own, so the accessible
// label is set here rather than left to the reader to infer: `name` where the caller has
// one, otherwise the label that is drawn. A tooltip is not a substitute, since it is
// announced by some readers, silently, and only after a hover.
void setIconButtonContent(GtkWidget* button, GtkWidget* icon, const char* label,
                          const char* name) {
    if (label) {
        GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(content), icon);
        gtk_box_append(GTK_BOX(content), gtk_label_new(label));
        gtk_button_set_child(GTK_BUTTON(button), content);
    } else {
        gtk_button_set_child(GTK_BUTTON(button), icon);
    }
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   name ? name : label, -1);
}

GtkWidget* makeIconButton(GtkWidget* icon, const char* label, const char* name = nullptr) {
    GtkWidget* button = gtk_button_new();
    setIconButtonContent(button, icon, label, name);
    return button;
}

GtkWidget* makeIconToggle(GtkWidget* icon, const char* label, const char* name = nullptr) {
    GtkWidget* button = gtk_toggle_button_new();
    setIconButtonContent(button, icon, label, name);
    return button;
}

const char* const kStyleCss = R"CSS(
window.stylus {
  background: #1b1d21;
}
/* The canvas paints the page colour edge to edge, so any gap left by a
   fit-to-screen zoom reads as more sheet rather than as a dark border. */
.board {
  background: #fafafc;
}
.toolbar {
  background: #23262b;
  border-bottom: 1px solid #343941;
  padding: 6px;
}
.brand {
  font-weight: 700;
  padding: 0 10px 0 4px;
}
.tool-group, .swatch-group, .size-group, .check-group, .zoom-group {
  background: #2b2f35;
  border: 1px solid #343941;
  border-radius: 8px;
  padding: 3px 7px;
  margin: 0 5px;
  /* GTK's default foreground is near-black, which is unreadable on these dark
     groups. Without this every label in the toolbar rendered dark-on-dark. */
  color: #e6e8ec;
}
.options-box {
  background: #1f2227;
  border-bottom: 1px solid #343941;
}
.options-box label {
  color: #e6e8ec;
}
.sidebar-title {
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.6px;
  color: #9aa3af;
  padding: 8px 10px;
}
.option-check {
  color: #c9ced6;
  padding: 1px 0;
}
.option-check:hover {
  color: #ffffff;
}
.tool-group button {
  margin: 0 2px;
}
button.tool {
  background: #31363d;
  color: #e6e8ec;
  border: 1px solid #343941;
  border-radius: 6px;
  padding: 5px 11px;
}
button.tool:checked {
  background: #3d7ffb;
  border-color: #6ea8fe;
  color: #ffffff;
}
button.flat {
  background: #31363d;
  color: #e6e8ec;
  border: 1px solid #343941;
  border-radius: 6px;
  padding: 4px 9px;
  margin: 0 2px;
}
button.flat:hover {
  background: #3a4048;
}
button.swatch {
  padding: 0;
  min-width: 20px;
  min-height: 20px;
  border-radius: 50%;
  border: 1px solid #00000066;
  margin: 0 3px;
}
button.swatch:checked {
  outline: 2px solid #6ea8fe;
  outline-offset: 1px;
}
.sidebar {
  background: #23262b;
  border-left: 1px solid #343941;
}
.sidebar-title.with-border {
  border-bottom: 1px solid #343941;
}
.panel-head {
  border-bottom: 1px solid #343941;
  padding: 6px 8px;
}
.panel-title {
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.6px;
  color: #9aa3af;
  padding: 0 4px;
}
.panel-btn {
  font-size: 11px;
  font-weight: 600;
  padding: 3px 4px;
  margin: 0;
}
.panel-btn:disabled {
  opacity: 0.3;
}
.layer-row {
  background: #2b2f35;
  border: 1px solid transparent;
  border-radius: 6px;
  padding: 5px 7px;
  margin-bottom: 4px;
}
.layer-row:hover {
  background: #31363d;
}
.layer-row.selected {
  border-color: #6ea8fe;
  background: #2f3946;
}
.layer-row.selected:hover {
  background: #364354;
}
.layer-name {
  color: #e6e8ec;
}
.layer-meta {
  color: #9aa3af;
  font-size: 11px;
}
/* The readout doubles as its own edit trigger, so it needs to look pressable. */
.layer-opacity {
  background: #22262b;
  border: 1px solid #343941;
  border-radius: 4px;
  padding: 1px 6px;
  min-width: 42px;
}
.layer-opacity:hover {
  background: #2b2f35;
  border-color: #6ea8fe;
  color: #e6e8ec;
}
.layer-opacity-edit {
  min-height: 22px;
}
.layer-rename {
  background: #1b1e23;
  border: 1px solid #6ea8fe;
  border-radius: 4px;
  color: #e6e8ec;
  padding: 1px 6px;
}
.eye-icon {
  opacity: 0.85;
}
.layer-row button.eye:checked .eye-icon {
  opacity: 1;
}
.statusbar {
  background: #23262b;
  border-top: 1px solid #343941;
  padding: 5px 10px;
  color: #9aa3af;
  font-size: 12px;
}
.pressure-group {
  /* Sits between the device name and the status message, so it is boxed to read as
     its own instrument rather than as more label text. */
  background: #191b1f;
  border: 1px solid #343941;
  border-radius: 6px;
  padding: 2px 8px;
}
.pressure-label {
  color: #8b93a1;
  font-size: 11px;
}
.pressure-out {
  /* Monospace so the readout does not shuffle sideways as the digits change. */
  font-family: monospace;
  font-size: 11px;
  color: #d6dae1;
}
.brush-preview {
  background: #252830;
  border: 1px solid #343941;
  border-radius: 6px;
  min-width: 34px;
  min-height: 34px;
}
.hud {
  background: #0d0e10cc;
  color: #e6e8ec;
  border: 1px solid #343941;
  border-radius: 6px;
  padding: 4px 9px;
  font-family: monospace;
  font-size: 11px;
}
)CSS";

}  // namespace

Window::Window(AppState* state) : state_(state), window_(gtk_window_new()) {
    gtk_window_set_title(GTK_WINDOW(window_), "Stylus Pen");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1440, 940);
    gtk_window_set_resizable(GTK_WINDOW(window_), TRUE);
    gtk_widget_add_css_class(window_, "stylus");

    loadStyle();

    canvas_ = std::make_unique<Canvas>(state_);
    canvas_->setStatusCallback([this](const std::string& s) { refreshStatus(s); });
    canvas_->setZoomLabelCallback([this](const std::string& s) { refreshZoom(s); });
    canvas_->setEraserEndCallback([this](bool active) { setEraserEnd(active); });
    canvas_->setPressureCallback([this](double pressure) { refreshPressure(pressure); });

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    buildToolbar(root);

    GtkWidget* middle = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_hexpand(canvas_->widget(), TRUE);
    gtk_widget_set_vexpand(canvas_->widget(), TRUE);
    gtk_paned_set_start_child(GTK_PANED(middle), canvas_->widget());
    gtk_paned_set_resize_start_child(GTK_PANED(middle), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(middle), FALSE);
    buildLayerPanel(middle);
    gtk_paned_set_resize_end_child(GTK_PANED(middle), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(middle), FALSE);
    gtk_widget_set_hexpand(middle, TRUE);
    gtk_widget_set_vexpand(middle, TRUE);
    gtk_box_append(GTK_BOX(root), middle);

    buildStatusBar(root);
    gtk_window_set_child(GTK_WINDOW(window_), root);

    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(&Window::onKeyPressed), this);
    gtk_widget_add_controller(window_, keys);

    // GTK4 replaced "size-allocate" with "resize"; the callback signature
    // changed to carry dimensions in an int[2] rather than two arguments.
    g_signal_connect(canvas_->widget(), "resize", G_CALLBACK(+[](GtkWidget* w, int width,
                                                               int height, Canvas* c) {
        (void)w;
        (void)width;
        (void)height;
        c->onResize();
    }),
                     canvas_.get());

    // The layer panel only rebuilds for structural changes. Pixel changes repaint
    // the canvas but leave the list alone, because rebuilding it would destroy the
    // widget the user is interacting with -- a slider mid-drag, or the text entry a
    // rename is being typed into.
    listenerTokens_.push_back(state_->setLayersChanged([this]() { queueLayersRefresh(); }));
    // Availability has to track every history change, including a stroke landing.
    // The setChanged channel fires on every push because AppState pushes through
    // pushHistory, which notifies only after the entry exists.
    listenerTokens_.push_back(
        state_->setChanged([this]() { refreshHistoryButtons(); }));
    listenerTokens_.push_back(state_->setNameChanged([this](size_t index, const std::string& name) {
        if (LayerRow* row = findLayerRow(index); row && row->name) {
            gtk_label_set_text(GTK_LABEL(row->name), name.c_str());
        }
    }));
}

namespace {

// Disconnects every handler on this widget tree that was connected with `owner` as its
// user data.
//
// The sidebar handlers all capture a raw `Window*`, and GTK emits notify signals while
// it finalises widgets -- including on the way out when a window is closed. A notify
// that lands after ~Window() had begun ran the handler against a destroyed Window, so
// `state_` read back as whatever the freed memory happened to hold: commitImageScale()
// read a drag flag out of that garbage, and Document::active() then indexed a layer
// vector it no longer owned. That is the crash that closed the window.
//
// Walking the whole tree rather than listing the widgets individually means a handler
// added later cannot reintroduce it.
void disconnectHandlersFromTree(GtkWidget* widget, gpointer owner) {
    if (!widget || !GTK_IS_WIDGET(widget)) return;
    g_signal_handlers_disconnect_by_data(widget, owner);

    if (GListModel* controllers = gtk_widget_observe_controllers(widget)) {
        guint n = g_list_model_get_n_items(controllers);
        for (guint i = 0; i < n; ++i) {
            gpointer item = g_list_model_get_item(controllers, i);
            if (item) {
                g_signal_handlers_disconnect_by_data(item, owner);
                g_object_unref(item);
            }
        }
        g_object_unref(controllers);
    }

    for (GtkWidget* child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) {
        disconnectHandlersFromTree(child, owner);
    }
}

}  // namespace

Window::~Window() {
    if (alive_) *alive_ = false;
    // First, before anything else can emit a signal that reaches a half-destroyed
    // Window.
    disconnectHandlersFromTree(window_, this);

    if (startIdle_) {
        g_source_remove(startIdle_);
        startIdle_ = 0;
    }

    if (layersRefreshIdle_) {
        g_source_remove(layersRefreshIdle_);
        layersRefreshIdle_ = 0;
    }

    // The pending status timeout outlives the window otherwise, and its
    // callback would touch a destroyed label.
    if (statusTimeout_) {
        g_source_remove(statusTimeout_);
        statusTimeout_ = 0;
    }

    // Drop the AppState listeners too. They hold a raw `this`, and AppState is
    // owned separately from the window and outlives it, so a late notification
    // would call into a destroyed Window.
    for (size_t token : listenerTokens_) {
        if (state_) state_->clearListener(token);
    }
    listenerTokens_.clear();

    // The canvas registers its own listeners the same way.
    canvas_.reset();

    // The page-background colour dialog and the CSS provider behind its swatch are
    // ours and nothing else frees them.
    //
    // The provider needs removing before it is unreferenced: handing it to the
    // display took a second reference of its own, so unref'ing alone would leave the
    // display holding a provider whose CSS class no longer belongs to anything.
    // Both are checked because the display can already be gone during shutdown.
    if (backgroundProvider_) {
        GdkDisplay* display = gdk_display_get_default();
        if (display) {
            gtk_style_context_remove_provider_for_display(display,
                                                         GTK_STYLE_PROVIDER(backgroundProvider_));
        }
        g_clear_object(&backgroundProvider_);
    }
    g_clear_object(&colorDialog_);
}

void Window::loadStyle() {
    GtkCssProvider* provider = gtk_css_provider_new();

    // GTK CSS cannot match on a hex colour, so every swatch gets a generated
    // rule keyed on a stable class name.
    std::string css = kStyleCss;
    for (int i = 0; i < kPaletteCount; ++i) {
        char rule[160];
        std::snprintf(rule, sizeof(rule), "button.swatch.swatch-%d { background: %s; }\n", i,
                      kPalette[i]);
        css += rule;
    }
    gtk_css_provider_load_from_string(provider, css.c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void Window::start() {
    gtk_widget_set_visible(window_, TRUE);
    refreshTools();
    refreshLayers();
    canvas_->fitToWidget();

    // What is actually driving the canvas. Each of these can be true at once on a
    // convertible, and naming only one of them leaves the user guessing about the
    // other -- a laptop with a stylus and a touchscreen reports "Touch active" from
    // the finger, which is what drew the last stroke, even though the pen is present.
    std::string devices;
    if (canvas_->penSeen()) devices = "Stylus";
    if (canvas_->touchSeen()) {
        if (!devices.empty()) devices += " and touch";
        else devices = "Touch";
    }
    if (devices.empty()) {
        // No touch here either, so the honest message is about the mouse: it draws,
        // and its pressure comes from the same speed fallback.
        devices = "No stylus or touchscreen detected";
        refreshStatus(devices + " — a mouse draws, with pressure from speed");
    } else {
        refreshStatus(devices + " active — draw on the canvas to start a stroke");
    }
}

void Window::reportError(const std::string& fallback) {
    auto err = state_->lastError();
    refreshStatus(err ? *err : fallback);
}

gboolean Window::onStatusExpired(gpointer data) {
    auto* self = static_cast<Window*>(data);
    // Clear the id first: the source is already gone by the time this runs, and
    // a stale id would later be passed to g_source_remove again. GLib recycles
    // ids aggressively, so that removes whichever unrelated source had taken
    // this number, which is how the app used to die at random.
    self->statusTimeout_ = 0;
    if (self->statusMsg_) gtk_label_set_text(GTK_LABEL(self->statusMsg_), "");
    return G_SOURCE_REMOVE;
}

void Window::drawBrushPreview(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                              gpointer userData) {
    (void)area;
    auto* self = static_cast<Window*>(userData);

    // The chip is dark because the brush colour is usually dark too, and a dark
    // dot on the dark toolbar would be invisible.
    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.145, 0.157, 0.176);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);
    cairo_restore(cr);

    if (!self->brushPreview_ || width <= 0 || height <= 0) return;

    const double r = self->canvas_->brushPreviewRadius();
    const double alpha = self->canvas_->brushPreviewAlpha();
    const double cx = width * 0.5;
    const double cy = height * 0.5;

    const Brush& brush = self->state_->brush();
    cairo_save(cr);
    if (brush.kind == BrushKind::Eraser) {
        // An eraser leaves nothing behind, so show its footprint as an outline
        // instead of pretending it paints.
        cairo_set_source_rgba(cr, 0.85, 0.87, 0.9, 0.9);
        cairo_set_line_width(cr, 1.5);
        if (r >= 3.0) {
            cairo_arc(cr, cx, cy, r, 0.0, 2.0 * M_PI);
        } else {
            // Too small to read as a circle at this scale: mark it with a
            // diagonal slash, the conventional way to say "the big one".
            cairo_move_to(cr, cx - r - 1.0, cy - r - 1.0);
            cairo_line_to(cr, cx + r + 1.0, cy + r + 1.0);
        }
        cairo_stroke(cr);
    } else {
        cairo_set_source_rgba(cr, brush.r, brush.g, brush.b, alpha);
        cairo_arc(cr, cx, cy, std::max(0.75, r), 0.0, 2.0 * M_PI);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

void Window::refreshBrushPreview() {
    if (brushPreview_) gtk_widget_queue_draw(brushPreview_);
}

void Window::refreshStatus(const std::string& text) {
    if (!statusMsg_) return;
    gtk_label_set_text(GTK_LABEL(statusMsg_), text.c_str());
    if (statusTimeout_) {
        g_source_remove(statusTimeout_);
        statusTimeout_ = 0;
    }
    statusTimeout_ = g_timeout_add(5000, &Window::onStatusExpired, this);
}

void Window::setEraserEnd(bool active) {
    if (!statusDevice_) return;
    if (active) {
        applyTool(BrushKind::Eraser);
        refreshStatus("Eraser end of the stylus — drawing erases");
    } else {
        applyTool(BrushKind::Pen);
        refreshStatus("Stylus back to the pen tip");
    }
}

void Window::refreshZoom(const std::string& text) {
    if (zoomLabel_) gtk_label_set_text(GTK_LABEL(zoomLabel_), text.c_str());
}

void Window::applyTool(BrushKind kind) {
    state_->brush().kind = kind;
    state_->settings().tool = kind;
    refreshBrushPreview();
    if (moveImageButton_ && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(moveImageButton_))) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(moveImageButton_), FALSE);
    }
    if (!toolButtons_[0]) return;

    for (int i = 0; i < kToolCount; ++i) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons_[i]), kTools[i].kind == kind);
    }
}

void Window::applyColor(const std::string& hex) {
    state_->brush().setHex(hex);
    state_->settings().color = hex;
    refreshBrushPreview();
    if (!swatchButtons_[0]) return;

    for (int i = 0; i < kPaletteCount; ++i) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(swatchButtons_[i]), hex == kPalette[i]);
    }
}

void Window::applySize(double size) {
    state_->brush().size = size;
    state_->settings().size = size;
    refreshBrushPreview();
    if (!sizeOut_) return;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", size);
    gtk_label_set_text(GTK_LABEL(sizeOut_), buf);
}

void Window::applyPressureSensitivity(double level) {
    const double clamped = std::max(0.0, std::min(1.0, level));
    state_->brush().pressureSensitivity = clamped;
    state_->settings().pressureSensitivity = clamped;
    refreshBrushPreview();
    if (!sensitivityOut_) return;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(clamped * 100.0)));
    gtk_label_set_text(GTK_LABEL(sensitivityOut_), buf);
}

void Window::applyBackground(const std::string& hex) {
    if (!state_->setBackgroundColor(hex)) return;
    refreshBackgroundButton();
}

void Window::refreshBackgroundButton() {
    const std::string hex = state_->document().backgroundHex();

    // CSS cannot match on a colour value, so the swatch is painted by a rule
    // keyed on a class that only this button carries.
    if (backgroundProvider_) {
        const std::string css = "button.background-swatch { background: " + hex + "; }";
        gtk_css_provider_load_from_string(backgroundProvider_, css.c_str());
    }

    // The swatch shows the colour and nothing else, so the hex itself has to go
    // in the accessible name and the tooltip -- otherwise the button is an
    // unlabelled colour blob to a screen reader.
    gtk_widget_set_tooltip_text(backgroundButton_, ("Page background " + hex).c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(backgroundButton_),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, ("Page background " + hex).c_str(),
                                   -1);
}

void Window::refreshTools() {
    // Guard the whole sync: applyTool/applyColor also guard, but the checkbox
    // updates below would otherwise fire while buttons are mid-update.
    updatingTools_ = true;
    applyTool(state_->brush().kind);
    applyColor(state_->settings().color);
    applySize(state_->brush().size);
    refreshBrushPreview();
    if (tiltCheck_) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(tiltCheck_), state_->settings().tiltShading);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(pressureCheck_),
                                    state_->settings().pressureWidth);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(stabilizeCheck_),
                                    state_->settings().stabilize);
    }
    if (pressureScale_) {
        // applyPressureSensitivity writes the slider's own value back through this
        // path, so the label is refreshed alongside it rather than left showing
        // whatever the last drag put there.
        applyPressureSensitivity(state_->settings().pressureSensitivity);
    }
    // The page colour is not a brush setting but it moves with everything else
    // that comes from a project file, so it is re-synced here too.
    refreshBackgroundButton();
    refreshPaperOptions();
    // Loading a project clears the history, so both buttons have to re-read it.
    refreshHistoryButtons();
    updatingTools_ = false;
}

void Window::buildToolbar(GtkWidget* root) {
    // Two rows rather than one. Every button now carries an icon as well as its label,
    // and a single row of them needs about 2000px -- some 500px past the window's default
    // width, which is what pushed the zoom buttons clean off the end of the bar. The top
    // row is what you draw with; the bottom row is what you do to the document, and how
    // you look at it.
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(bar, "toolbar");

    GtkWidget* toolsRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* actionsRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    GtkWidget* brand = gtk_label_new("Stylus Pen");
    gtk_widget_add_css_class(brand, "brand");
    gtk_box_append(GTK_BOX(toolsRow), brand);

    GtkWidget* tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(tools, "tool-group");
    for (int i = 0; i < kToolCount; ++i) {
        GtkWidget* b = makeIconToggle(paintedIcon(toolIcon(kTools[i].kind)), kTools[i].label);
        gtk_widget_add_css_class(b, "tool");
        // The tooltip names the tool and its shortcut. It used to carry the shortcut
        // alone, which left "B" standing in for "Pen" to anyone who had not already
        // worked out which letter belonged to which tool.
        const std::string tip = std::string(kTools[i].label) + " (" + kTools[i].accel + ")";
        gtk_widget_set_tooltip_text(b, tip.c_str());
        g_signal_connect(b, "toggled",
                         G_CALLBACK(+[](GtkToggleButton* btn, Window* self) {
                             if (self->updatingTools_) return;
                             if (!gtk_toggle_button_get_active(btn)) return;
                             for (int k = 0; k < kToolCount; ++k) {
                                 if (self->toolButtons_[k] == GTK_WIDGET(btn)) {
                                     self->applyTool(kTools[k].kind);
                                     break;
                                 }
                             }
                         }),
                         this);
        toolButtons_[i] = b;
        gtk_box_append(GTK_BOX(tools), b);
    }
    gtk_box_append(GTK_BOX(toolsRow), tools);

    GtkWidget* swatches = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(swatches, "swatch-group");
    for (int i = 0; i < kPaletteCount; ++i) {
        GtkWidget* b = gtk_toggle_button_new();
        gtk_widget_add_css_class(b, "swatch");
        gtk_widget_set_size_request(b, 20, 20);
        // GTK4 removed gtk_widget_override_background_color, so the colour
        // comes from a generated CSS rule keyed on this class.
        {
            char cls[32];
            std::snprintf(cls, sizeof(cls), "swatch-%d", i);
            gtk_widget_add_css_class(b, cls);
        }
        gtk_widget_set_tooltip_text(b, kPalette[i]);
        gtk_accessible_update_property(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       kPalette[i], -1);
        g_signal_connect(b, "toggled",
                         G_CALLBACK(+[](GtkToggleButton* btn, Window* self) {
                             if (self->updatingTools_) return;
                             if (!gtk_toggle_button_get_active(btn)) return;
                             for (int k = 0; k < kPaletteCount; ++k) {
                                 if (self->swatchButtons_[k] == GTK_WIDGET(btn)) {
                                     self->applyColor(kPalette[k]);
                                     if (self->state_->brush().kind == BrushKind::Eraser) {
                                         self->applyTool(BrushKind::Pen);
                                     }
                                     break;
                                 }
                             }
                         }),
                         this);
        swatchButtons_[i] = b;
        gtk_box_append(GTK_BOX(swatches), b);
    }
    gtk_box_append(GTK_BOX(toolsRow), swatches);

    GtkWidget* sizeBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sizeBox, "size-group");
    GtkWidget* sizeLabel = gtk_label_new("Size");
    gtk_widget_set_margin_end(sizeLabel, 6);
    gtk_box_append(GTK_BOX(sizeBox), sizeLabel);

    sizeScale_ = gtk_scale_new_with_range(GtkOrientation::GTK_ORIENTATION_HORIZONTAL, 0.5, 40.0, 0.5);
    gtk_scale_set_draw_value(GTK_SCALE(sizeScale_), FALSE);
    // Still 112 rather than wider. This row has a line to itself now, but the actions
    // row below is the one that runs out of width first, and every pixel here is a pixel
    // the file buttons on it cannot have.
    gtk_widget_set_size_request(sizeScale_, 112, -1);
    g_signal_connect(sizeScale_, "value-changed", G_CALLBACK(+[](GtkRange* r, Window* self) {
        self->applySize(gtk_range_get_value(r));
    }),
                     this);
    gtk_box_append(GTK_BOX(sizeBox), sizeScale_);

    sizeOut_ = gtk_label_new("3.0");
    gtk_widget_set_size_request(sizeOut_, 38, -1);
    gtk_widget_set_margin_start(sizeOut_, 6);
    gtk_box_append(GTK_BOX(sizeBox), sizeOut_);

    // A live footprint of the mark the current brush leaves at the current
    // pressure. Without it the size slider is just a number, and the difference
    // between a 3 and a 4 nib is invisible until a stroke is committed.
    brushPreview_ = gtk_drawing_area_new();
    gtk_widget_add_css_class(brushPreview_, "brush-preview");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(brushPreview_), &Window::drawBrushPreview,
                                   this, nullptr);
    gtk_box_append(GTK_BOX(sizeBox), brushPreview_);

    gtk_box_append(GTK_BOX(toolsRow), sizeBox);

    wireFileActions(actionsRow);

    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(actionsRow), spacer);

    GtkWidget* zooms = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(zooms, "zoom-group");
    // The zoom group is icon-only: the three magnifiers say what they do, the readout
    // beside them says how far along the current one is, and the labels the glyphs
    // replace were one character each.
    GtkWidget* zOut = makeIconButton(themedIcon("zoom-out-symbolic"), nullptr, "Zoom out");
    GtkWidget* zFit = makeIconButton(paintedIcon(ToolbarIcon::Fit), nullptr, "Fit page to window");
    GtkWidget* zIn = makeIconButton(themedIcon("zoom-in-symbolic"), nullptr, "Zoom in");
    for (GtkWidget* b : {zOut, zFit, zIn}) gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_tooltip_text(zOut, "Zoom out");
    gtk_widget_set_tooltip_text(zFit, "Fit the page to the window (Ctrl+0)");
    gtk_widget_set_tooltip_text(zIn, "Zoom in");
    zoomLabel_ = gtk_label_new("100%");
    gtk_widget_set_size_request(zoomLabel_, 46, -1);

    g_signal_connect(zOut, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
        self->canvas_->zoomBy(1.0 / 1.2, -1.0, -1.0);
    }), this);
    g_signal_connect(zFit, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
        self->canvas_->fitToWidget();
    }), this);
    g_signal_connect(zIn, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
        self->canvas_->zoomBy(1.2, -1.0, -1.0);
    }), this);

    gtk_box_append(GTK_BOX(zooms), zOut);
    gtk_box_append(GTK_BOX(zooms), zFit);
    gtk_box_append(GTK_BOX(zooms), zIn);
    gtk_box_append(GTK_BOX(zooms), zoomLabel_);
    gtk_box_append(GTK_BOX(actionsRow), zooms);

    gtk_box_append(GTK_BOX(bar), toolsRow);
    gtk_box_append(GTK_BOX(bar), actionsRow);
    gtk_box_append(GTK_BOX(root), bar);
}

void Window::refreshHistoryButtons() {
    const History& history = state_->history();
    if (undoButton_) gtk_widget_set_sensitive(undoButton_, history.canUndo());
    if (redoButton_) gtk_widget_set_sensitive(redoButton_, history.canRedo());
}

void Window::wireFileActions(GtkWidget* bar) {
    GtkWidget* group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(group, "check-group");

    // Undo and redo live here, next to the tools: they are the actions reached for
    // constantly, so they belong on the main bar. New page and Reset are page-level
    // and sit in the sidebar instead -- all four in one row pushes the toolbar about
    // 230px past the window's default width, which is what previously shoved the
    // file buttons off the end.
    //
    // The glyphs are the theme's own, which is what the rest of the app already does
    // for the layer eye. Save and PNG export are the one pair that needs care: Adwaita
    // draws both as an arrow dropping onto a tray, so the export takes the dashed
    // "save as" glyph to keep the two apart at 16px.
    undoButton_ = makeIconButton(themedIcon("edit-undo-symbolic"), "Undo");
    redoButton_ = makeIconButton(themedIcon("edit-redo-symbolic"), "Redo");
    GtkWidget* imageBtn = makeIconButton(themedIcon("insert-image-symbolic"), "Image");
    GtkWidget* textBtn = makeIconButton(themedIcon("insert-text-symbolic"), "Text");
    GtkWidget* ocrBtn =
        makeIconButton(paintedIcon(ToolbarIcon::HandwritingToText), "Pen → Text");
    moveImageButton_ = makeIconToggle(paintedIcon(ToolbarIcon::Move), "Move image");

    GtkWidget* save = makeIconButton(themedIcon("document-save-symbolic"), "Save");
    GtkWidget* open = makeIconButton(themedIcon("document-open-symbolic"), "Open");
    // "PNG" rather than "Export PNG": the icon and the tooltip carry the full meaning,
    // and the row no longer has the width to spell it out as well.
    GtkWidget* exportBtn = makeIconButton(themedIcon("document-save-as-symbolic"), "PNG");

    for (GtkWidget* b : {undoButton_, redoButton_, imageBtn, textBtn, ocrBtn, save, open, exportBtn}) {
        gtk_widget_add_css_class(b, "flat");
    }
    gtk_widget_add_css_class(moveImageButton_, "flat");

    gtk_widget_set_tooltip_text(undoButton_, "Undo (Ctrl+Z)");
    gtk_widget_set_tooltip_text(redoButton_, "Redo (Ctrl+Shift+Z or Ctrl+Y)");
    gtk_widget_set_tooltip_text(imageBtn, "Insert a picture as its own layer");
    gtk_widget_set_tooltip_text(textBtn, "Insert custom text onto a new layer");
    gtk_widget_set_tooltip_text(ocrBtn, "Convert handwriting on active layer to text (Ctrl+Shift+T)");
    gtk_widget_set_tooltip_text(moveImageButton_,
                                "Drag the active layer's picture to reposition it. "
                                "Turn this off to draw on the canvas again.");
    gtk_widget_set_tooltip_text(exportBtn, "Export the flattened artwork as a PNG");
    gtk_widget_set_tooltip_text(save, "Save the project (Ctrl+S)");
    gtk_widget_set_tooltip_text(open, "Open a saved project");

    g_signal_connect(imageBtn, "clicked",
                     G_CALLBACK(+[](GtkButton*, Window* self) { self->chooseImageToInsert(); }),
                     this);
    g_signal_connect(textBtn, "clicked",
                     G_CALLBACK(+[](GtkButton*, Window* self) { self->showInsertTextDialog(); }),
                     this);
    g_signal_connect(ocrBtn, "clicked",
                     G_CALLBACK(+[](GtkButton*, Window* self) { self->showHandwritingToTextDialog(); }),
                     this);
    g_signal_connect(moveImageButton_, "toggled",
                     G_CALLBACK(+[](GtkToggleButton* btn, Window* self) {
                         if (self->updatingTools_) return;
                         self->canvas_->setMoveMode(gtk_toggle_button_get_active(btn));
                         self->refreshMoveMode();
                     }),
                     this);
    g_signal_connect(undoButton_, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                         self->state_->undo();
                         self->refreshLayers();
                         self->refreshBackgroundButton();
                         self->refreshPaperOptions();
                     }),
                     this);
    g_signal_connect(redoButton_, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                         self->state_->redo();
                         self->refreshLayers();
                         self->refreshBackgroundButton();
                         self->refreshPaperOptions();
                     }),
                     this);
    g_signal_connect(save, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                         self->saveProject();
                     }),
                     this);
    g_signal_connect(open, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                         self->openProject();
                     }),
                     this);
    g_signal_connect(exportBtn, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                         self->exportPng();
                     }),
                     this);

    for (GtkWidget* b : {undoButton_, redoButton_, imageBtn, textBtn, ocrBtn, moveImageButton_, save, open,
                         exportBtn}) {
        gtk_box_append(GTK_BOX(group), b);
    }
    gtk_box_append(GTK_BOX(bar), group);
}

void Window::buildImageSection(GtkWidget* box) {
    imageSection_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(imageSection_, "options-box");
    gtk_widget_set_visible(imageSection_, FALSE);

    GtkWidget* title = gtk_label_new("IMAGE");
    gtk_widget_add_css_class(title, "sidebar-title");
    gtk_box_append(GTK_BOX(imageSection_), title);

    GtkWidget* scaleRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(scaleRow, "option-check");

    GtkWidget* scaleLabel = gtk_label_new("Size");
    gtk_widget_set_hexpand(scaleLabel, TRUE);
    gtk_widget_set_halign(scaleLabel, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(scaleRow), scaleLabel);

    // Percentages rather than a raw factor: "150%" is a thing a person means, and it
    // is what the readout shows too, so the two cannot drift apart.
    imageScale_ = gtk_scale_new_with_range(GtkOrientation::GTK_ORIENTATION_HORIZONTAL, 10.0,
                                           400.0, 5.0);
    gtk_scale_set_draw_value(GTK_SCALE(imageScale_), FALSE);
    gtk_widget_set_size_request(imageScale_, 110, -1);
    gtk_widget_set_tooltip_text(imageScale_, "Picture size, as a percentage of its original");
    g_signal_connect(imageScale_, "value-changed", G_CALLBACK(+[](GtkRange* r, Window* self) {
                         if (self->updatingTools_) return;
                         self->state_->setImageScaleLive(gtk_range_get_value(r) / 100.0);
                         self->refreshImageSection();
                         // Committed on focus loss rather than per step, so a sweep
                         // of the slider is one undo entry.
                     }),
                     this);
    gtk_box_append(GTK_BOX(scaleRow), imageScale_);

    imageScaleOut_ = gtk_label_new("100%");
    gtk_widget_add_css_class(imageScaleOut_, "pressure-out");
    gtk_widget_set_size_request(imageScaleOut_, 42, -1);
    gtk_box_append(GTK_BOX(scaleRow), imageScaleOut_);
    gtk_box_append(GTK_BOX(imageSection_), scaleRow);

    // The sweep is committed when the slider is let go of, not per step. Focus loss is
    // the same signal the opacity slider uses, and it covers both the pointer and the
    // keyboard without either dragging the commit into the value-changed handler.
    g_signal_connect(imageScale_, "notify::has-focus",
                     G_CALLBACK(+[](GtkWidget* w, Window* self) {
                         if (gtk_widget_has_focus(w)) return;
                         self->state_->commitImageScale();
                         self->refreshImageSection();
                     }),
                     this);

    GtkWidget* actionRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(actionRow, "option-check");
    const char* labels[] = {"Fit page", "Centre", "Edit Text", "Delete"};
    for (int i = 0; i < 4; ++i) {
        GtkWidget* b = gtk_button_new_with_label(labels[i]);
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_set_hexpand(b, TRUE);
        gtk_box_append(GTK_BOX(actionRow), b);
        if (i == 0) {
            gtk_widget_set_tooltip_text(b, "Scale the picture to fill the page");
            g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                                 if (!self->state_->scaleActiveImageToPage()) return;
                                 self->refreshImageSection();
                                 self->state_->commitImageScale();
                                 self->refreshImageSection();
                             }),
                             this);
        } else if (i == 1) {
            gtk_widget_set_tooltip_text(b, "Centre the picture on the page");
            g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                                 self->state_->centreActiveImage();
                                 self->refreshImageSection();
                             }),
                             this);
        } else if (i == 2) {
            gtk_widget_set_tooltip_text(b, "Edit text content, size, font and formatting");
            g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                                 self->showEditTextDialog();
                             }),
                             this);
        } else {
            gtk_widget_set_tooltip_text(b, "Delete this picture/text layer (undoable)");
            g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                                 if (!self->state_->deleteActiveImage()) {
                                     self->reportError("Could not delete the layer");
                                     return;
                                 }
                                 self->refreshLayers();
                                 self->refreshImageSection();
                                 self->refreshMoveMode();
                             }),
                             this);
        }
    }
    gtk_box_append(GTK_BOX(imageSection_), actionRow);

    gtk_widget_set_margin_start(imageSection_, 6);
    gtk_widget_set_margin_end(imageSection_, 6);
    gtk_widget_set_margin_bottom(imageSection_, 6);
    gtk_box_append(GTK_BOX(box), imageSection_);
}

void Window::refreshImageSection() {
    const bool hasImage = state_->activeLayerHasImage();
    if (imageSection_) gtk_widget_set_visible(imageSection_, hasImage);
    if (!hasImage) return;

    const bool wasUpdating = updatingTools_;
    updatingTools_ = true;
    if (imageScale_) {
        gtk_range_set_value(GTK_RANGE(imageScale_), state_->document().active().image().scale * 100.0);
    }
    updatingTools_ = wasUpdating;

    if (imageScaleOut_) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%",
                      static_cast<int>(std::lround(state_->document().active().image().scale * 100.0)));
        gtk_label_set_text(GTK_LABEL(imageScaleOut_), buf);
    }
}

void Window::chooseImageToInsert() {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Insert image");

    auto* pair = new std::pair<Window*, std::shared_ptr<bool>>(this, alive_);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(window_), nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            auto* p = static_cast<std::pair<Window*, std::shared_ptr<bool>>*>(data);
            Window* self = p->first;
            std::shared_ptr<bool> alive = p->second;
            auto* dlg = GTK_FILE_DIALOG(source);
            g_autoptr(GError) error = nullptr;
            GFile* file = gtk_file_dialog_open_finish(dlg, result, &error);
            if (file) {
                if (alive && *alive && self) {
                    self->insertImageFromFile(file);
                }
                g_object_unref(file);
            }
            delete p;
        },
        pair);
}

void Window::insertImageFromFile(GFile* file) {
    if (!file) return;

    g_autoptr(GError) error = nullptr;
    GdkTexture* texture = gdk_texture_new_from_file(file, &error);
    if (!texture) {
        reportError("Could not read that image");
        refreshStatus(error ? error->message : "Could not read that image");
        return;
    }

    // Re-encoded to PNG in memory so the picture can join the document as an ordinary
    // layer payload. The .styluspen format already stores one PNG per layer, so a
    // project with a picture in it saves and reloads with no format change at all.
    g_autoptr(GBytes) png = gdk_texture_save_to_png_bytes(texture);
    const int width = gdk_texture_get_width(texture);
    const int height = gdk_texture_get_height(texture);
    g_object_unref(texture);

    if (!png || width <= 0 || height <= 0) {
        reportError("Could not read that image");
        return;
    }

    gsize size = 0;
    const char* raw = static_cast<const char*>(g_bytes_get_data(png, &size));
    // Decoded at zero size so the surface keeps the picture's own dimensions instead
    // of being scaled onto the page; insertImage does the fitting.
    auto decoded = decodePng(std::string(raw, size), 0, 0);
    if (!decoded) {
        reportError("Could not read that image");
        return;
    }

    g_autofree char* base = g_file_get_basename(file);
    if (!state_->insertImage(*decoded, base ? base : "Image")) {
        reportError("Could not insert that image");
        return;
    }

    refreshLayers();
    refreshTools();
    if (moveImageButton_) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(moveImageButton_), TRUE);
    }
    refreshMoveMode();
    refreshImageSection();
    refreshStatus("Image inserted — drag on canvas to position it");
}

namespace {
struct TextDialogCtx {
    Window* self;
    std::shared_ptr<bool> alive;
    GtkWidget* dialog;
    GtkWidget* entry;
    GtkWidget* spin;
    bool done = false;
};
}  // namespace

void Window::showInsertTextDialogConfirm(void* data) {
    auto* c = static_cast<TextDialogCtx*>(data);
    if (!c || c->done) return;
    c->done = true;

    if (c->alive && *c->alive && c->self) {
        const char* txt = gtk_editable_get_text(GTK_EDITABLE(c->entry));
        const double sz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(c->spin));
        if (txt && std::strlen(txt) > 0) {
            if (c->self->canvas_) c->self->canvas_->cancelImageMove();
            c->self->state_->insertTextWithCurrentBrush(txt, sz);
            c->self->queueLayersRefresh();
            c->self->refreshTools();
            if (c->self->moveImageButton_) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->self->moveImageButton_), TRUE);
            }
            c->self->refreshMoveMode();
            c->self->refreshImageSection();
            c->self->refreshStatus("Text inserted — drag on canvas to position it");
        }
    }
    if (c->dialog && GTK_IS_WIDGET(c->dialog)) {
        gtk_window_destroy(GTK_WINDOW(c->dialog));
    }
}

void Window::showInsertTextDialog() {
    if (canvas_) canvas_->cancelImageMove();
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Insert Text");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 340, 180);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    GtkWidget* label = gtk_label_new("Enter text to insert:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), "Hello World");
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget* fontRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* fontLabel = gtk_label_new("Font size:");
    gtk_box_append(GTK_BOX(fontRow), fontLabel);

    GtkWidget* fontSpin = gtk_spin_button_new_with_range(12.0, 144.0, 4.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fontSpin), 36.0);
    gtk_box_append(GTK_BOX(fontRow), fontSpin);
    gtk_box_append(GTK_BOX(box), fontRow);

    GtkWidget* btnRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btnRow, GTK_ALIGN_END);

    GtkWidget* cancelBtn = gtk_button_new_with_label("Cancel");
    GtkWidget* insertBtn = gtk_button_new_with_label("Insert");
    gtk_widget_add_css_class(insertBtn, "suggested-action");

    gtk_box_append(GTK_BOX(btnRow), cancelBtn);
    gtk_box_append(GTK_BOX(btnRow), insertBtn);
    gtk_box_append(GTK_BOX(box), btnRow);

    gtk_window_set_child(GTK_WINDOW(dialog), box);

    auto* ctx = new TextDialogCtx{this, alive_, dialog, entry, fontSpin, false};
    g_object_set_data_full(G_OBJECT(dialog), "text-ctx", ctx, [](gpointer p) {
        delete static_cast<TextDialogCtx*>(p);
    });

    g_signal_connect_swapped(cancelBtn, "clicked", G_CALLBACK(gtk_window_destroy), dialog);

    g_signal_connect_swapped(insertBtn, "clicked", G_CALLBACK(+[](GtkWidget* d, Window* self) {
        if (GTK_IS_WIDGET(d)) self->showInsertTextDialogConfirm(g_object_get_data(G_OBJECT(d), "text-ctx"));
    }), dialog);

    g_signal_connect_swapped(entry, "activate", G_CALLBACK(+[](GtkWidget* d, Window* self) {
        if (GTK_IS_WIDGET(d)) self->showInsertTextDialogConfirm(g_object_get_data(G_OBJECT(d), "text-ctx"));
    }), dialog);

    gtk_window_present(GTK_WINDOW(dialog));
}

namespace {
struct HwrDialogCtx {
    Window* self;
    std::shared_ptr<bool> alive;
    GtkWidget* dialog;
    GtkWidget* entry;
    GtkWidget* spin;
    GtkWidget* replaceCheck;
    Rect bounds;
    bool done = false;
};
}  // namespace

void Window::showHandwritingToTextDialogConfirm(void* data) {
    auto* c = static_cast<HwrDialogCtx*>(data);
    if (!c || c->done) return;
    c->done = true;

    if (c->alive && *c->alive && c->self) {
        std::string txt;
        if (GTK_IS_TEXT_VIEW(c->entry)) {
            GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(c->entry));
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buf, &start, &end);
            char* t = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
            if (t) {
                txt = t;
                g_free(t);
            }
        } else if (GTK_IS_EDITABLE(c->entry)) {
            const char* t = gtk_editable_get_text(GTK_EDITABLE(c->entry));
            if (t) txt = t;
        }

        const double sz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(c->spin));
        const bool replace = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->replaceCheck));
        if (!txt.empty()) {
            if (c->self->canvas_) c->self->canvas_->cancelImageMove();
            c->self->state_->learnHandwritingWord(txt);
            c->self->state_->convertHandwritingToText(txt, c->bounds, replace, sz);
            c->self->queueLayersRefresh();

            c->self->refreshTools();
            if (c->self->moveImageButton_) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->self->moveImageButton_), TRUE);
            }
            c->self->refreshMoveMode();
            c->self->refreshImageSection();
            c->self->refreshStatus("Handwriting converted to digital text");
        }
    }
    if (c->dialog && GTK_IS_WIDGET(c->dialog)) {
        gtk_window_destroy(GTK_WINDOW(c->dialog));
    }
}

void Window::showHandwritingToTextDialog() {
    if (canvas_) canvas_->cancelImageMove();
    RecognizedText rec = state_->recognizeActiveLayerHandwriting();

    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Convert Pen Handwriting to Text");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 280);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    GtkWidget* label = gtk_label_new("Recognized handwriting text (multi-line):");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scrolled, -1, 95);
    GtkWidget* textView = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textView), GTK_WRAP_WORD_CHAR);
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textView));
    std::string initialText = rec.text.empty() ? "Handwritten Text" : rec.text;
    gtk_text_buffer_set_text(buf, initialText.c_str(), -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), textView);
    gtk_box_append(GTK_BOX(box), scrolled);

    GtkWidget* fontRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* fontLabel = gtk_label_new("Font size:");
    gtk_box_append(GTK_BOX(fontRow), fontLabel);

    GtkWidget* fontSpin = gtk_spin_button_new_with_range(12.0, 144.0, 4.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fontSpin), 36.0);
    gtk_box_append(GTK_BOX(fontRow), fontSpin);
    gtk_box_append(GTK_BOX(box), fontRow);

    GtkWidget* replaceCheck = gtk_check_button_new_with_label("Clear original handwriting layer");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(replaceCheck), TRUE);
    gtk_box_append(GTK_BOX(box), replaceCheck);

    GtkWidget* btnRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btnRow, GTK_ALIGN_END);

    GtkWidget* cancelBtn = gtk_button_new_with_label("Cancel");
    GtkWidget* convertBtn = gtk_button_new_with_label("Convert & Insert");
    gtk_widget_add_css_class(convertBtn, "suggested-action");

    gtk_box_append(GTK_BOX(btnRow), cancelBtn);
    gtk_box_append(GTK_BOX(btnRow), convertBtn);
    gtk_box_append(GTK_BOX(box), btnRow);

    gtk_window_set_child(GTK_WINDOW(dialog), box);

    auto* ctx = new HwrDialogCtx{this, alive_, dialog, textView, fontSpin, replaceCheck, rec.bounds, false};
    g_object_set_data_full(G_OBJECT(dialog), "hwr-ctx", ctx, [](gpointer p) {
        delete static_cast<HwrDialogCtx*>(p);
    });

    g_signal_connect_swapped(cancelBtn, "clicked", G_CALLBACK(gtk_window_destroy), dialog);

    g_signal_connect_swapped(convertBtn, "clicked", G_CALLBACK(+[](GtkWidget* d, Window* self) {
        if (GTK_IS_WIDGET(d)) self->showHandwritingToTextDialogConfirm(g_object_get_data(G_OBJECT(d), "hwr-ctx"));
    }), dialog);

    gtk_window_present(GTK_WINDOW(dialog));

}

namespace {
struct EditTextCtx {
    Window* self;
    std::shared_ptr<bool> alive;
    GtkWidget* dialog;
    GtkWidget* entry;
    GtkWidget* spin;
    GtkWidget* boldCheck;
    GtkWidget* italicCheck;
    GtkWidget* fontCombo;
    Color currentColor{0.1, 0.1, 0.12};
    bool done = false;
};
}  // namespace

void Window::showEditTextDialogConfirm(void* data) {
    auto* c = static_cast<EditTextCtx*>(data);
    if (!c || c->done) return;
    c->done = true;

    if (c->alive && *c->alive && c->self) {
        const char* txt = gtk_editable_get_text(GTK_EDITABLE(c->entry));
        const double sz = gtk_spin_button_get_value(GTK_SPIN_BUTTON(c->spin));
        const bool bold = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->boldCheck));
        const bool italic = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->italicCheck));

        const char* fontName = "Sans";
        if (GtkDropDown* drop = GTK_DROP_DOWN(c->fontCombo)) {
            guint selected = gtk_drop_down_get_selected(drop);
            if (selected == 1) fontName = "Serif";
            else if (selected == 2) fontName = "Monospace";
        }

        if (txt && std::strlen(txt) > 0) {
            Layer::TextInfo info;
            info.text = txt;
            info.fontSize = sz;
            info.color = c->currentColor;
            info.isBold = bold;
            info.isItalic = italic;
            info.fontFamily = fontName;
            info.valid = true;

            if (c->self->canvas_) c->self->canvas_->cancelImageMove();
            if (c->self->state_->activeLayerHasText()) {
                c->self->state_->updateActiveTextLayer(info);
            } else {
                c->self->state_->insertTextFormatted(info);
            }
            c->self->queueLayersRefresh();
            c->self->refreshTools();
            if (c->self->moveImageButton_) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->self->moveImageButton_), TRUE);
            }
            c->self->refreshMoveMode();
            c->self->refreshImageSection();
            c->self->refreshStatus("Text layer updated");
        }
    }
    if (c->dialog && GTK_IS_WIDGET(c->dialog)) {
        gtk_window_destroy(GTK_WINDOW(c->dialog));
    }
}

void Window::showEditTextDialog() {
    if (canvas_) canvas_->cancelImageMove();
    Layer::TextInfo info = state_->activeLayerTextInfo();
    if (!info.valid) {
        info.text = "Sample Text";
        info.fontSize = 36.0;
        info.color = Color{state_->brush().r, state_->brush().g, state_->brush().b};
        info.isBold = true;
        info.isItalic = false;
        info.fontFamily = "Sans";
    }

    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), state_->activeLayerHasText() ? "Edit & Format Text" : "Insert & Format Text");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 380, 260);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    GtkWidget* label = gtk_label_new("Text Content:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), info.text.c_str());
    gtk_box_append(GTK_BOX(box), entry);

    // Font Options Row (Size & Font Family)
    GtkWidget* fontRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* fontLabel = gtk_label_new("Size:");
    gtk_box_append(GTK_BOX(fontRow), fontLabel);

    GtkWidget* fontSpin = gtk_spin_button_new_with_range(8.0, 200.0, 2.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(fontSpin), info.fontSize);
    gtk_box_append(GTK_BOX(fontRow), fontSpin);

    GtkWidget* familyLabel = gtk_label_new("Font:");
    gtk_widget_set_margin_start(familyLabel, 12);
    gtk_box_append(GTK_BOX(fontRow), familyLabel);

    const char* fonts[] = {"Sans", "Serif", "Monospace", nullptr};
    GtkWidget* fontCombo = gtk_drop_down_new_from_strings(fonts);
    if (info.fontFamily == "Serif") gtk_drop_down_set_selected(GTK_DROP_DOWN(fontCombo), 1);
    else if (info.fontFamily == "Monospace") gtk_drop_down_set_selected(GTK_DROP_DOWN(fontCombo), 2);
    else gtk_drop_down_set_selected(GTK_DROP_DOWN(fontCombo), 0);
    gtk_box_append(GTK_BOX(fontRow), fontCombo);

    gtk_box_append(GTK_BOX(box), fontRow);

    // Style Options Row (Bold, Italic)
    GtkWidget* styleRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* boldCheck = gtk_check_button_new_with_label("Bold");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(boldCheck), info.isBold);
    gtk_box_append(GTK_BOX(styleRow), boldCheck);

    GtkWidget* italicCheck = gtk_check_button_new_with_label("Italic");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(italicCheck), info.isItalic);
    gtk_box_append(GTK_BOX(styleRow), italicCheck);

    gtk_box_append(GTK_BOX(box), styleRow);

    GtkWidget* btnRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btnRow, GTK_ALIGN_END);

    GtkWidget* cancelBtn = gtk_button_new_with_label("Cancel");
    GtkWidget* applyBtn = gtk_button_new_with_label(state_->activeLayerHasText() ? "Update Text" : "Insert Text");
    gtk_widget_add_css_class(applyBtn, "suggested-action");

    gtk_box_append(GTK_BOX(btnRow), cancelBtn);
    gtk_box_append(GTK_BOX(btnRow), applyBtn);
    gtk_box_append(GTK_BOX(box), btnRow);

    gtk_window_set_child(GTK_WINDOW(dialog), box);

    auto* ctx = new EditTextCtx{this, alive_, dialog, entry, fontSpin, boldCheck, italicCheck, fontCombo, info.color, false};
    g_object_set_data_full(G_OBJECT(dialog), "edit-text-ctx", ctx, [](gpointer p) {
        delete static_cast<EditTextCtx*>(p);
    });

    g_signal_connect_swapped(cancelBtn, "clicked", G_CALLBACK(gtk_window_destroy), dialog);

    g_signal_connect_swapped(applyBtn, "clicked", G_CALLBACK(+[](GtkWidget* d, Window* self) {
        if (GTK_IS_WIDGET(d)) self->showEditTextDialogConfirm(g_object_get_data(G_OBJECT(d), "edit-text-ctx"));
    }), dialog);

    g_signal_connect_swapped(entry, "activate", G_CALLBACK(+[](GtkWidget* d, Window* self) {
        if (GTK_IS_WIDGET(d)) self->showEditTextDialogConfirm(g_object_get_data(G_OBJECT(d), "edit-text-ctx"));
    }), dialog);

    gtk_window_present(GTK_WINDOW(dialog));
}

void Window::refreshMoveMode() {
    const bool on = canvas_ && canvas_->moveMode();
    const bool usable = on && state_->canMoveActiveLayer();

    if (moveImageButton_) {
        gtk_widget_set_sensitive(moveImageButton_, state_->canMoveActiveLayer());
    }
    if (!on) return;

    if (!usable) {
        refreshStatus("Move mode is on, but there are no layers to move");
    } else {
        refreshStatus("Drag on the canvas to move the selected layer");
    }
}

void Window::buildBrushOptions(GtkWidget* sidebar) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "options-box");

    GtkWidget* title = gtk_label_new("BRUSH OPTIONS");
    gtk_widget_add_css_class(title, "sidebar-title");
    gtk_box_append(GTK_BOX(box), title);

    auto addCheck = [&](const char* label, const char* tip, GtkWidget** slot) {
        GtkWidget* c = gtk_check_button_new_with_label(label);
        gtk_widget_set_tooltip_text(c, tip);
        gtk_widget_add_css_class(c, "option-check");
        *slot = c;
        gtk_box_append(GTK_BOX(box), c);
        return c;
    };

    addCheck("Tilt shading", "Model a flat nib: the mark widens along the lean", &tiltCheck_);
    addCheck("Pressure width", "Stroke width follows stylus pressure", &pressureCheck_);

    // How strongly pressure moves the mark, as a percentage. This is a slider rather
    // than another checkbox because the useful setting is almost never one of the two
    // ends: a pen that is fully pressure-driven is hard to lay down a flat line with,
    // and a pen that ignores pressure cannot taper at all. The checkbox above stays the
    // coarse on/off; this dials how far it swings.
    GtkWidget* sensitivityRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(sensitivityRow, "option-check");

    GtkWidget* sensitivityLabel = gtk_label_new("Pressure level");
    gtk_widget_set_hexpand(sensitivityLabel, TRUE);
    gtk_widget_set_halign(sensitivityLabel, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(sensitivityRow), sensitivityLabel);

    pressureScale_ = gtk_scale_new_with_range(GtkOrientation::GTK_ORIENTATION_HORIZONTAL,
                                              0.0, 100.0, 5.0);
    gtk_scale_set_draw_value(GTK_SCALE(pressureScale_), FALSE);
    gtk_widget_set_size_request(pressureScale_, 110, -1);
    gtk_widget_set_tooltip_text(
        pressureScale_,
        "How much the nib reacts to pressure. 0% draws an even width, 100% follows "
        "the stylus fully.");
    gtk_accessible_update_property(GTK_ACCESSIBLE(pressureScale_),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Pressure sensitivity, as a percentage", -1);
    g_signal_connect(pressureScale_, "value-changed", G_CALLBACK(+[](GtkRange* r, Window* self) {
                         // Guarded like the paper slider: refreshTools() writes the
                         // value back, and without this the sync would re-enter here
                         // and rewrite the setting it is reading.
                         if (self->updatingTools_) return;
                         self->applyPressureSensitivity(gtk_range_get_value(r) / 100.0);
                     }),
                     this);
    gtk_box_append(GTK_BOX(sensitivityRow), pressureScale_);

    sensitivityOut_ = gtk_label_new("100%");
    // Wide enough for three digits, which is every value the range can produce, so the
    // readout does not resize the row as it changes.
    gtk_widget_set_size_request(sensitivityOut_, 42, -1);
    gtk_widget_set_tooltip_text(sensitivityOut_, "Pressure sensitivity");
    gtk_box_append(GTK_BOX(sensitivityRow), sensitivityOut_);
    gtk_box_append(GTK_BOX(box), sensitivityRow);

    addCheck("Stabilizer", "Smooth out nib jitter while you draw", &stabilizeCheck_);

    // Page colour. Not a check and not a palette swatch, because the page can be
    // any colour rather than one of a fixed set, so this is a button showing the
    // current one that opens the system colour dialog.
    GtkWidget* pageRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(pageRow, "option-check");

    GtkWidget* pageLabel = gtk_label_new("Page");
    gtk_widget_set_hexpand(pageLabel, TRUE);
    gtk_widget_set_halign(pageLabel, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pageRow), pageLabel);

    backgroundProvider_ = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(backgroundProvider_),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    backgroundButton_ = gtk_button_new();
    gtk_widget_add_css_class(backgroundButton_, "background-swatch");
    gtk_widget_add_css_class(backgroundButton_, "flat");
    gtk_widget_set_size_request(backgroundButton_, 74, 24);
    gtk_widget_set_halign(backgroundButton_, GTK_ALIGN_END);
    gtk_widget_set_tooltip_text(backgroundButton_, "Choose the page background colour");
    gtk_box_append(GTK_BOX(pageRow), backgroundButton_);

    // New page and Reset sit here rather than in the toolbar: both are page-level
    // actions, which is what this sidebar section is about, and the toolbar has no
    // room left. Reset deliberately touches only the page -- colour and ruling -- and
    // leaves the artwork alone. Clearing the drawing is New page's job, and a button
    // that threw away work under a name that sounds cosmetic would be a trap.
    GtkWidget* sheetRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(sheetRow, "option-check");

    GtkWidget* newPage = gtk_button_new_with_label("New page");
    GtkWidget* resetPage = gtk_button_new_with_label("Reset");
    for (GtkWidget* b : {newPage, resetPage}) {
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_set_hexpand(b, TRUE);
    }
    gtk_widget_set_tooltip_text(newPage, "Start a new sheet: clears the artwork");
    gtk_widget_set_tooltip_text(resetPage,
                                "Default page colour and no ruling, artwork kept");

    g_signal_connect(newPage, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                         self->state_->newCanvas();
                         self->refreshLayers();
                         self->refreshBackgroundButton();
                         self->refreshPaperOptions();
                     }),
                     this);
    g_signal_connect(resetPage, "clicked", G_CALLBACK(+[](GtkButton*, Window* self) {
                         // Not undoable, the same as picking a colour: the page is
                         // furniture, not a drawing step. Routing this through
                         // newCanvas would wipe the artwork, which is the one thing
                         // this button promises not to do.
                         self->applyBackground("#fbfbfd");
                         self->applyPaper(PaperKind::Plain, 32.0);
                     }),
                     this);

    gtk_box_append(GTK_BOX(sheetRow), newPage);
    gtk_box_append(GTK_BOX(sheetRow), resetPage);

    gtk_box_append(GTK_BOX(box), pageRow);
    gtk_box_append(GTK_BOX(box), sheetRow);

    // The dialog is stateless between uses -- the colour to start from is an
    // argument to choose_rgba -- so it only has to exist, not be kept in sync.
    colorDialog_ = gtk_color_dialog_new();
    gtk_color_dialog_set_title(colorDialog_, "Page background");
    // Alpha would let a pick land on a partly transparent page, which the file
    // format has nowhere to put: the tag carries rgb only.
    gtk_color_dialog_set_with_alpha(colorDialog_, FALSE);

    refreshBackgroundButton();

    // The extra parentheses are load-bearing: the lambda body contains a
    // braced-init-list, and a comma inside {} does not protect a macro argument
    // from being split -- only parentheses do.
    g_signal_connect(backgroundButton_, "clicked",
                     G_CALLBACK((+[](GtkButton*, Window* self) {
                         const Color& page = self->state_->document().background();
                         GdkRGBA initial{page.r, page.g, page.b, 1.0};
                         gtk_color_dialog_choose_rgba(
                             self->colorDialog_, GTK_WINDOW(self->window_), &initial, nullptr,
                             +[](GObject* source, GAsyncResult* result, gpointer data) {
                                 // GAsyncReadyCallback is fixed at void*, so the
                                 // Window comes back through a cast rather than as
                                 // a typed parameter.
                                 auto* inner = static_cast<Window*>(data);
                                 GError* err = nullptr;
                                 GdkRGBA* rgba = gtk_color_dialog_choose_rgba_finish(
                                     GTK_COLOR_DIALOG(source), result, &err);
                                 if (rgba) {
                                     // Some backends can hand back a component
                                     // outside 0..1; formatHexColor clamps before
                                     // the value is painted or written to a file.
                                     inner->applyBackground(formatHexColor(
                                         {rgba->red, rgba->green, rgba->blue}));
                                     gdk_rgba_free(rgba);
                                 }
                                 if (err) {
                                     // Closing the dialog is a normal outcome, not
                                     // something to report in the status bar.
                                     if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                                         inner->reportError("Could not read the chosen colour");
                                     }
                                     g_error_free(err);
                                 }
                             },
                             self);
                     })),
                     this);

    // GtkCheckButton is a GtkWidget subclass, not a GtkToggleButton: it has its
    // own get/set_active and a "toggled" signal carrying no arguments.
    g_signal_connect(tiltCheck_, "toggled",
                     G_CALLBACK(+[](GtkCheckButton* b, Window* self) {
                         self->state_->brush().tiltShading = gtk_check_button_get_active(b);
                         self->state_->settings().tiltShading = self->state_->brush().tiltShading;
                         self->refreshBrushPreview();
                     }),
                     this);
    g_signal_connect(pressureCheck_, "toggled",
                     G_CALLBACK(+[](GtkCheckButton* b, Window* self) {
                         self->state_->brush().pressureWidth = gtk_check_button_get_active(b);
                         self->state_->settings().pressureWidth = self->state_->brush().pressureWidth;
                         self->refreshBrushPreview();
                     }),
                     this);
    g_signal_connect(stabilizeCheck_, "toggled",
                     G_CALLBACK(+[](GtkCheckButton* b, Window* self) {
                         self->state_->settings().stabilize = gtk_check_button_get_active(b);
                     }),
                     this);

    buildPaperOptions(box);
    buildImageSection(box);

    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_box_append(GTK_BOX(sidebar), box);
}

void Window::drawPaperChip(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                           gpointer userData) {
    const auto kind = static_cast<PaperKind>(GPOINTER_TO_INT(userData));
    (void)area;

    // The chip draws the real pattern at a fixed on-screen pitch rather than the
    // document pitch. Scaling the document spacing down into a 26px chip would make
    // a grid and a ruled sheet look identical -- at 32px apart, both fit inside one
    // row -- which is exactly the distinction the chip exists to show. So each
    // preview uses a spacing chosen to read at this size, and shows more than one
    // interval so a ruled sheet reads as ruled.
    // 9 rather than something tighter: at 8 the rules land on the border inset and
    // the chip reads as a grid of boxes instead of as a sample of the page.
    const double gap = 9.0;

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.984, 0.984, 0.992);
    cairo_paint(cr);

    Paper chip;
    chip.kind = kind;
    chip.spacing = gap;
    // The chip's own size is the page size. Scaling it down to "one rule per cell"
    // (width / gap) left a 7x7 page inside a 40x28 chip, which is too small to hold a
    // single rule -- every chip came out blank, which is exactly what a blank row of
    // pattern buttons looks like.
    paintPaper(cr, chip, Color{0.984, 0.984, 0.992}, width, height);

    // A light hairline, only to mark the chip's edge. Heavier than this and it
    // competes with the ruling, which is the thing the chip is here to show. The
    // selected state comes from the surrounding toggle button instead.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.14);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void Window::buildPaperOptions(GtkWidget* box) {
    GtkWidget* title = gtk_label_new("PAPER");
    gtk_widget_add_css_class(title, "sidebar-title");
    gtk_box_append(GTK_BOX(box), title);

    // One toggle per pattern, each chipping a real preview of it. Text labels would
    // need five words to describe five patterns; the preview is read at a glance and
    // still names itself for a screen reader.
    GtkWidget* chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(chips, "paper-chips");

    for (int i = 0; i < kPaperKindCount; ++i) {
        const PaperKind kind = static_cast<PaperKind>(i);
        GtkWidget* b = gtk_toggle_button_new();
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_add_css_class(b, "paper-chip");

        GtkWidget* chip = gtk_drawing_area_new();
        // Tall enough to show several intervals with clear margins either side. One
        // rule per chip cannot be told apart from a border.
        gtk_widget_set_size_request(chip, 44, 30);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(chip), &Window::drawPaperChip,
                                       GINT_TO_POINTER(i), nullptr);
        gtk_button_set_child(GTK_BUTTON(b), chip);

        // The tooltip states the outcome, not just the name. The chips carry no text
        // of their own, so "Page ruling: Ruled" left the reader to work out that the
        // chip is what draws the ruling -- and Plain, the default, draws nothing.
        char tip[96];
        std::snprintf(tip, sizeof(tip), "%s", paperKindName(kind));
        switch (kind) {
            case PaperKind::Plain:
                std::snprintf(tip, sizeof(tip), "Plain: no ruling on the page");
                break;
            case PaperKind::Ruled:
                std::snprintf(tip, sizeof(tip), "Ruled: horizontal lines only");
                break;
            case PaperKind::Grid:
                std::snprintf(tip, sizeof(tip), "Grid: horizontal and vertical lines");
                break;
            case PaperKind::Dotted:
                std::snprintf(tip, sizeof(tip), "Dotted: a dot at every intersection");
                break;
            case PaperKind::Checkered:
                std::snprintf(tip, sizeof(tip), "Checkered: backdrop for transparency");
                break;
        }
        gtk_widget_set_tooltip_text(b, tip);
        gtk_accessible_update_property(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       paperKindName(kind), -1);

        // The chip's kind is carried on the widget rather than captured, because the
        // handler is a captureless lambda. Reading it back is what makes the chip
        // choose a pattern at all: this used to pass the document's *current* kind,
        // which is Plain on a fresh document, so every chip called setPaper with
        // Plain and nothing happened -- the button pressed and the canvas stayed
        // blank.
        g_object_set_data(G_OBJECT(b), "paper-kind", GINT_TO_POINTER(i));

        g_signal_connect(b, "toggled", G_CALLBACK(+[](GtkToggleButton* btn, Window* self) {
                             if (self->updatingTools_) return;
                             // Only the "on" direction is a request, same as the
                             // layer visibility toggle: a programmatic unset while
                             // syncing must not clear the selection.
                             if (!gtk_toggle_button_get_active(btn)) return;
                             const int kindValue =
                                 GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "paper-kind"));
                             PaperKind chosen = PaperKind::Plain;
                             if (!parsePaperKind(kindValue, &chosen)) return;
                             // Spacing is kept: changing the pattern should not
                             // discard the pitch the user already dialled in.
                             self->applyPaper(chosen, self->state_->document().paper().spacing);
                         }),
                         this);

        paperButtons_[i] = b;
        gtk_box_append(GTK_BOX(chips), b);
    }
    gtk_box_append(GTK_BOX(box), chips);

    GtkWidget* gapRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(gapRow, "option-check");

    GtkWidget* gapLabel = gtk_label_new("Spacing");
    gtk_widget_set_hexpand(gapLabel, TRUE);
    gtk_widget_set_halign(gapLabel, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(gapRow), gapLabel);

    // The range is the model's own, not a second guess at it: a slider that cannot
    // reach a legal spacing shows the wrong page and silently rewrites it the first
    // time it is touched.
    paperScale_ = gtk_scale_new_with_range(GtkOrientation::GTK_ORIENTATION_HORIZONTAL,
                                           kMinPaperSpacing, kMaxPaperSpacing, 2.0);
    gtk_scale_set_draw_value(GTK_SCALE(paperScale_), FALSE);
    gtk_widget_set_size_request(paperScale_, 120, -1);
    gtk_widget_set_tooltip_text(paperScale_,
                                "Distance between rules, in document pixels. Takes "
                                "effect as soon as a pattern is picked.");
    // Readable on its own, so the slider does not have to draw a value on top of the
    // groove to be legible.
    gtk_accessible_update_property(GTK_ACCESSIBLE(paperScale_),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Rule spacing in document pixels", -1);
    g_signal_connect(paperScale_, "value-changed", G_CALLBACK(+[](GtkRange* r, Window* self) {
                         if (self->updatingTools_) return;
                         self->applyPaper(self->state_->document().paper().kind,
                                          gtk_range_get_value(r));
                     }),
                     this);
    gtk_box_append(GTK_BOX(gapRow), paperScale_);

    // Always usable, including while the page is Plain.
    //
    // It used to be greyed out until a pattern was picked, which is defensible in
    // principle -- there is nothing to space out on a blank page. But Plain is the
    // default and what Reset returns to, so the control looked broken on a new
    // document and after every reset. The spacing is a page setting that outlives the
    // kind, so letting it be set while Plain is active means the number is already
    // right by the time a pattern is chosen.
    paperGapOut_ = gtk_label_new("32");
    // Wide enough for three digits, which is every value the range can produce.
    gtk_widget_set_size_request(paperGapOut_, 34, -1);
    gtk_widget_set_tooltip_text(paperGapOut_, "px between rules");
    gtk_box_append(GTK_BOX(gapRow), paperGapOut_);
    gtk_box_append(GTK_BOX(box), gapRow);
}

void Window::applyPaper(PaperKind kind, double spacing) {
    if (!state_->setPaper(kind, spacing)) return;
    refreshPaperOptions();

    // Moving the spacing on a blank page changes nothing you can see, which reads as
    // the control being broken rather than as nothing being drawn yet. Say so, rather
    // than leaving the user to work out that another row of buttons is the missing
    // step.
    if (kind == PaperKind::Plain) {
        refreshStatus("Spacing is set -- pick a pattern above to draw the ruling");
    }
}

void Window::refreshPaperOptions() {
    const Paper paper = state_->document().paper();

    // The chips and the slider both feed back into applyPaper, so syncing them is
    // guarded the same way the toolbar and brush checkboxes are.
    const bool wasUpdating = updatingTools_;
    updatingTools_ = true;

    for (int i = 0; i < kPaperKindCount; ++i) {
        if (paperButtons_[i]) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(paperButtons_[i]),
                                         static_cast<PaperKind>(i) == paper.kind);
        }
    }
    if (paperScale_) {
        gtk_range_set_value(GTK_RANGE(paperScale_), paper.spacing);
    }
    if (paperGapOut_) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(paper.spacing)));
        gtk_label_set_text(GTK_LABEL(paperGapOut_), buf);
    }

    updatingTools_ = wasUpdating;
}

void Window::buildLayerPanel(GtkWidget* middle) {
    GtkWidget* sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sidebar, kSidebarWidth, -1);
    gtk_widget_set_hexpand(sidebar, FALSE);
    gtk_widget_add_css_class(sidebar, "sidebar");

    // The brush options live in the sidebar rather than the toolbar. All on one
    // row they pushed the file buttons off the end of a 1500px window, and the
    // label text was dark-on-dark because the toolbar group's colour was never set.
    buildBrushOptions(sidebar);

    // Header row: label plus the actions that apply to the whole stack.
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(head, "panel-head");
    GtkWidget* title = gtk_label_new("LAYERS");
    gtk_widget_add_css_class(title, "panel-title");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(head), title);

    layerHeader_ = head;

    // Member-function pointers cannot travel through a GObject signal, so each
    // button gets its own small handler. Grouping them in one box keeps them
    // aligned and makes enabling/disabling them together trivial.
    struct ActionButton {
        GtkWidget* button;
        void (Window::*action)();
        int slot;  // LayerActionSlot, or -1 for a button that is always enabled
    };
    // The slot is stored on the button so refreshLayerHeader can enable and
    // disable it by identity instead of by position, which is fragile the moment
    // a button is added to the row.
    // Plain text rather than glyphs. The arrow and duplicate symbols came out as
    // stray dots and vertical bars under the default font, which read as broken
    // buttons rather than as actions.
    const ActionButton actions[] = {
        {makePanelButton("+", "Add a layer above the current one (Ctrl+Shift+N)"),
         &Window::addLayerAction, -1},
        {makePanelButton("Dup", "Duplicate the current layer (Ctrl+J)"),
         &Window::duplicateLayerAction, kActionDuplicate},
        {makePanelButton("Up", "Move the current layer up"),
         &Window::moveLayerUpAction, kActionMoveUp},
        {makePanelButton("Dn", "Move the current layer down"),
         &Window::moveLayerDownAction, kActionMoveDown},
        {makePanelButton("Merge", "Merge into the layer below (Ctrl+E)"),
         &Window::mergeDownAction, kActionMerge},
        {makePanelButton("Del", "Delete the current layer"),
         &Window::deleteLayerAction, kActionDelete},
    };

    GtkWidget* actionBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (const ActionButton& a : actions) {
        g_object_set_data(G_OBJECT(a.button), "action-slot", GINT_TO_POINTER(a.slot));

        // Member-function pointers cannot travel through a GObject signal, so each
        // button gets its own small handler.
        if (a.action == &Window::addLayerAction) {
            g_signal_connect(a.button, "clicked",
                                     G_CALLBACK(+[](GtkButton*, Window* self) {
                                         self->addLayerAction();
                                     }),
                                     this);
        } else if (a.action == &Window::duplicateLayerAction) {
            g_signal_connect(a.button, "clicked",
                                     G_CALLBACK(+[](GtkButton*, Window* self) {
                                         self->duplicateLayerAction();
                                     }),
                                     this);
        } else if (a.action == &Window::moveLayerUpAction) {
            g_signal_connect(a.button, "clicked",
                                     G_CALLBACK(+[](GtkButton*, Window* self) {
                                         self->moveLayerAction(+1);
                                     }),
                                     this);
        } else if (a.action == &Window::moveLayerDownAction) {
            g_signal_connect(a.button, "clicked",
                                     G_CALLBACK(+[](GtkButton*, Window* self) {
                                         self->moveLayerAction(-1);
                                     }),
                                     this);
        } else if (a.action == &Window::mergeDownAction) {
            g_signal_connect(a.button, "clicked",
                                     G_CALLBACK(+[](GtkButton*, Window* self) {
                                         self->mergeDownAction();
                                     }),
                                     this);
        } else if (a.action == &Window::deleteLayerAction) {
            g_signal_connect(a.button, "clicked",
                                     G_CALLBACK(+[](GtkButton*, Window* self) {
                                         self->deleteLayerAction();
                                     }),
                                     this);
        }
        gtk_box_append(GTK_BOX(actionBox), a.button);
    }
    layerActionButtons_ = actionBox;
    gtk_box_append(GTK_BOX(head), actionBox);
    gtk_box_append(GTK_BOX(sidebar), head);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    layerList_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(layerList_, 6);
    gtk_widget_set_margin_bottom(layerList_, 6);
    gtk_widget_set_margin_start(layerList_, 6);
    gtk_widget_set_margin_end(layerList_, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), layerList_);
    gtk_box_append(GTK_BOX(sidebar), scroll);

    if (GTK_IS_PANED(middle)) {
        gtk_paned_set_end_child(GTK_PANED(middle), sidebar);
    } else {
        gtk_box_append(GTK_BOX(middle), sidebar);
    }
}

GtkWidget* Window::makePanelButton(const char* label, const char* tip) {
    GtkWidget* b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_add_css_class(b, "panel-btn");
    // Wide enough for the longest word, and no wider: the six buttons share one
    // row in a 280px sidebar next to the LAYERS heading.
    gtk_widget_set_size_request(b, 44, 26);
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

void Window::refreshLayers() {
    if (!layerList_) return;

    // The rebuild tears down every row. Handlers on those rows must not act on
    // the state they are in the middle of writing, so the flag guards both the
    // programmatic "set active" calls below and the row callbacks themselves.
    rebuildingLayers_ = true;

    std::vector<std::shared_ptr<LayerRow>> oldRows = std::move(layerRows_);
    layerRows_.clear();

    for (const auto& row : oldRows) {
        if (row) {
            row->self = nullptr;
            row->name = nullptr;
            row->opacity = nullptr;
            row->eye = nullptr;
        }
    }

    while (GtkWidget* child = gtk_widget_get_first_child(layerList_)) {
        gtk_widget_unparent(child);
    }

    const Document& doc = state_->document();
    const size_t active = doc.activeIndex();

    // Top of the list is the topmost layer.
    for (size_t i = doc.layerCount(); i-- > 0;) {
        const Layer& layer = doc.layerAt(i);

        auto payload = std::make_shared<LayerRow>(LayerRow{this, i, nullptr, nullptr, nullptr});
        LayerRow* rawPayload = payload.get();

        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(row, "layer-row");
        if (i == active) gtk_widget_add_css_class(row, "selected");

        // Visibility. An icon rather than a bare toggle, so the row is readable at
        // a glance instead of relying on the pressed state of a blank button.
        GtkWidget* eye = gtk_toggle_button_new();
        gtk_widget_add_css_class(eye, "flat");
        gtk_widget_add_css_class(eye, "eye");
        gtk_widget_set_size_request(eye, 26, 24);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(eye), layer.visible());
        {
            GtkWidget* icon = gtk_image_new_from_icon_name(
                layer.visible() ? "view-reveal-symbolic" : "view-conceal-symbolic");
            gtk_image_set_pixel_size(GTK_IMAGE(icon), 14);
            gtk_widget_add_css_class(icon, "eye-icon");
            // GtkToggleButton has no set_child; the generic child API is the one
            // GTK4 offers.
            gtk_button_set_child(GTK_BUTTON(eye), icon);
        }
        gtk_widget_set_tooltip_text(eye, layer.visible() ? "Hide this layer" : "Show this layer");
        gtk_accessible_update_property(GTK_ACCESSIBLE(eye), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       layer.visible() ? "Hide layer" : "Show layer", -1);
        g_signal_connect(eye, "toggled", G_CALLBACK(+[](GtkToggleButton* btn, LayerRow* p) {
                             if (!p || !p->self || p->self->rebuildingLayers()) return;
                             if (!gtk_toggle_button_get_active(btn)) return;
                             p->self->state_->toggleVisibility(p->index);
                         }),
                         rawPayload);
        rawPayload->eye = eye;

        GtkWidget* name = gtk_label_new(layer.name().c_str());
        gtk_widget_add_css_class(name, "layer-name");
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PangoEllipsizeMode::PANGO_ELLIPSIZE_END);
        gtk_widget_set_tooltip_text(name, "Double-click to rename");
        rawPayload->name = name;

        GtkWidget* opacity = gtk_button_new();
        gtk_widget_add_css_class(opacity, "layer-opacity");
        char meta[32];
        std::snprintf(meta, sizeof(meta), "%d%%",
                      static_cast<int>(std::lround(layer.opacity() * 100.0)));
        {
            GtkWidget* value = gtk_label_new(meta);
            gtk_widget_add_css_class(value, "layer-meta");
            gtk_button_set_child(GTK_BUTTON(opacity), value);
        }
        gtk_widget_set_tooltip_text(opacity, "Change this layer's opacity");
        gtk_accessible_update_property(GTK_ACCESSIBLE(opacity), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       meta, -1);
        g_signal_connect(opacity, "clicked", G_CALLBACK(+[](GtkButton*, LayerRow* p) {
                             if (!p || !p->self || p->self->rebuildingLayers()) return;
                             p->self->beginOpacityEdit(p);
                         }),
                         rawPayload);
        rawPayload->opacity = opacity;

        gtk_box_append(GTK_BOX(row), eye);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), opacity);

        GtkGesture* click = GTK_GESTURE(gtk_gesture_click_new());
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
        g_signal_connect(
            click, "pressed",
            G_CALLBACK(+[](GtkGesture*, int, double, double, gpointer data) {
                auto* p = static_cast<LayerRow*>(data);
                if (!p || !p->self || p->self->rebuildingLayers()) return;
                p->self->selectLayer(p->index);
            }),
            rawPayload);

        GtkGesture* dbl = GTK_GESTURE(gtk_gesture_click_new());
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(dbl), GDK_BUTTON_PRIMARY);
        gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(dbl), FALSE);
        g_signal_connect(
            dbl, "pressed",
            G_CALLBACK(+[](GtkGesture*, int n_press, double, double, gpointer data) {
                auto* p = static_cast<LayerRow*>(data);
                if (!p || !p->self || p->self->rebuildingLayers()) return;
                if (n_press < 2) return;
                p->self->beginRename(p->index);
            }),
            rawPayload);

        gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
        gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(dbl));

        gtk_box_append(GTK_BOX(layerList_), row);
        layerRows_.push_back(payload);
    }

    rebuildingLayers_ = false;
    refreshLayerHeader();
    refreshImageSection();
    refreshMoveMode();
}

void Window::scheduleStart() {
    if (startIdle_) return;
    startIdle_ = g_idle_add(
        [](gpointer data) -> gboolean {
            auto* self = static_cast<Window*>(data);
            self->startIdle_ = 0;
            self->start();
            return G_SOURCE_REMOVE;
        },
        this);
}

void Window::queueLayersRefresh() {
    // One rebuild per batch, no matter how many rows were touched.
    if (layersRefreshQueued_) return;
    layersRefreshQueued_ = true;
    layersRefreshIdle_ = g_idle_add(
        [](gpointer data) -> gboolean {
            auto* self = static_cast<Window*>(data);
            self->layersRefreshIdle_ = 0;
            self->layersRefreshQueued_ = false;
            self->refreshLayers();
            return G_SOURCE_REMOVE;
        },
        this);
}

void Window::selectLayer(size_t index) {
    if (rebuildingLayers_) return;
    if (index >= state_->document().layerCount()) return;
    if (state_->document().activeIndex() == index) return;
    if (canvas_) canvas_->cancelImageMove();
    state_->cancelActiveImageMove();
    state_->document().setActiveIndex(index);

    // Deferred, not immediate. This runs from the row's own GtkGesture "pressed"
    // handler, and refreshLayers() tears every row down and rebuilds it -- the row that
    // is mid-press included. Destroying the gesture whose signal emission is still on
    // the stack takes the window down with it: GTK keeps using the widget for the rest
    // of the click sequence after the handler returns. Selecting a layer is the step
    // right before Move image, which is why this showed up as a move-image crash.
    queueLayersRefresh();
}

void Window::beginRename(size_t index) {
    if (rebuildingLayers_) return;
    LayerRow* row = findLayerRow(index);
    if (!row || !row->name) return;

    GtkWidget* parent = gtk_widget_get_parent(row->name);
    if (!parent) return;

    GtkWidget* entry = gtk_entry_new();
    gtk_widget_add_css_class(entry, "layer-rename");
    gtk_editable_set_text(GTK_EDITABLE(entry), state_->document().layerAt(index).name().c_str());

    // Swap the label for the entry in place so the row does not move under the
    // pointer.
    gtk_widget_set_visible(row->name, FALSE);
    gtk_box_insert_child_after(GTK_BOX(parent), entry, row->name);

    auto* ctx = new RenameCtx{this, index, parent, entry, row->name};
    // The label is about to be hidden and the entry takes its place. Holding a
    // reference means a layer panel rebuild in between cannot leave the destructor
    // writing to a freed label.
    g_object_ref(row->name);

    // Enter commits. Escape abandons, which is what a text field is expected to
    // do and stops a stray Escape from wiping a layer name.
    g_signal_connect(entry, "activate",
                     G_CALLBACK(+[](GtkEntry*, RenameCtx* c) { c->self->endRename(c, true); }),
                     ctx);

    g_signal_connect(entry, "key-pressed",
                     G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint,
                                    GdkModifierType, RenameCtx* c) -> gboolean {
                         if (keyval != GDK_KEY_Escape) return FALSE;
                         c->self->endRename(c, false);
                         return TRUE;  // handled
                     }),
                     ctx);

    // Focus loss commits too, so clicking away cannot leave a layer stuck in edit
    // mode with the old label hidden.
    g_signal_connect(entry, "notify::has-focus",
                     G_CALLBACK(+[](GtkWidget* w, RenameCtx* c) {
                         if (gtk_widget_has_focus(w)) return;
                         c->self->endRename(c, true);
                     }),
                     ctx);

    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

void Window::endRename(RenameCtx* ctx, bool commit) {
    if (!ctx || ctx->done) return;
    ctx->done = true;

    // Read the name and apply it while the entry is still alive and parented, so
    // nothing the state change does can race the teardown below.
    if (commit && ctx->self && ctx->index < state_->document().layerCount()) {
        state_->setLayerName(ctx->index, gtk_editable_get_text(GTK_EDITABLE(ctx->entry)));
    }

    // The teardown is deferred to the next main-loop turn. Unparenting the entry here
    // would drop the container's last reference and finalise the very widget whose
    // signal handler is on the stack, and GTK keeps using it after this returns.
    g_idle_add(
        [](gpointer data) -> gboolean {
            delete static_cast<RenameCtx*>(data);
            return G_SOURCE_REMOVE;
        },
        ctx);
}

stylus::RenameCtx::~RenameCtx() {
    if (GTK_IS_WIDGET(entry)) {
        GtkWidget* parent_ = gtk_widget_get_parent(entry);
        if (parent_) gtk_box_remove(GTK_BOX(parent_), entry);
    }
    if (label) {
        gtk_widget_set_visible(label, TRUE);
        // The reference taken in beginRename. Dropping it here is safe because this
        // destructor runs from an idle, with no signal emission in flight.
        g_object_unref(label);
    }
}

LayerRow* Window::findLayerRow(size_t index) const {
    for (const auto& row : layerRows_) {
        if (row && row->index == index) return row.get();
    }
    return nullptr;
}

void Window::beginOpacityEdit(LayerRow* row) {
    if (rebuildingLayers_ || !row || row->editing) return;
    if (row->index >= state_->document().layerCount()) return;

    GtkWidget* parent = gtk_widget_get_parent(row->opacity);
    if (!parent) return;

    // A small inline slider on the row itself, so the value stays visible while it
    // is being dragged rather than hiding behind a dialog.
    GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
    gtk_widget_add_css_class(scale, "layer-opacity-edit");
    gtk_widget_set_size_request(scale, 86, -1);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);

    // Seeded before the handler is connected, so restoring the current value does
    // not register as a user edit and push a pointless history entry.
    gtk_range_set_value(GTK_RANGE(scale), state_->document().layerAt(row->index).opacity());

    // Live updates go through setLayerOpacityLive, which deliberately does not
    // rebuild the panel: doing so here would destroy this slider and this row while
    // their own handler was running.
    g_signal_connect(scale, "value-changed", G_CALLBACK(+[](GtkRange* r, LayerRow* p) {
                         if (p->self->rebuildingLayers()) return;
                         p->self->state_->setLayerOpacityLive(p->index, gtk_range_get_value(r));
                         // Update the readout in place. A full rebuild would be
                         // simpler but would pull the slider out from under the drag.
                         if (GtkWidget* value = gtk_widget_get_first_child(p->opacity)) {
                             char buf[16];
                             std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(std::lround(
                                                 gtk_range_get_value(r) * 100.0)));
                             gtk_label_set_text(GTK_LABEL(value), buf);
                         }
                     }),
                     row);

    // Closing the slider ends the gesture, which is what collapses the drag into
    // one undo entry.
    g_signal_connect(
        scale, "notify::has-focus", G_CALLBACK(+[](GtkWidget* w, LayerRow* p) {
            if (gtk_widget_has_focus(w)) return;
            if (!p->editing) return;
            p->self->state_->commitLayerOpacity(p->index);
            p->self->endOpacityEdit(p, w);
        }),
        row);

    // Escape abandons the edit and puts the value back, matching what a text field
    // would do rather than silently keeping a half-finished drag.
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed",
                     G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint,
                                    GdkModifierType, LayerRow* p) -> gboolean {
                         if (keyval != GDK_KEY_Escape || !p->editing) return FALSE;
                         p->editing = false;
                         p->self->state_->cancelLayerOpacity(p->index);
                         // Put the readout back in place and drop the slider here
                         // rather than rebuilding the panel: this handler is
                         // running from a controller owned by the row, so tearing
                         // the row down inside it would leave this frame writing to
                         // freed memory.
                         p->self->endOpacityEdit(p);
                         return TRUE;
                     }),
                     row);
    g_object_set_data(G_OBJECT(scale), "opacity-keys", keys);
    gtk_widget_add_controller(scale, keys);

    row->editing = true;
    gtk_widget_set_visible(row->opacity, FALSE);
    gtk_box_insert_child_after(GTK_BOX(parent), scale, row->opacity);
    gtk_widget_grab_focus(scale);
}

void Window::endOpacityEdit(LayerRow* row, GtkWidget* scale) {
    if (!row) return;
    GtkWidget* slider = scale;
    if (!slider && row->opacity) {
        GtkWidget* next = gtk_widget_get_next_sibling(row->opacity);
        if (next && gtk_widget_has_css_class(next, "layer-opacity-edit")) {
            slider = next;
        }
    }
    row->editing = false;

    if (slider && GTK_IS_WIDGET(slider) && gtk_widget_get_parent(slider)) {
        gtk_box_remove(GTK_BOX(gtk_widget_get_parent(slider)), slider);
    }
    if (row->opacity && GTK_IS_WIDGET(row->opacity)) gtk_widget_set_visible(row->opacity, TRUE);

    // Restore the readout to the value the layer actually ended up at, which
    // differs from the slider position if the edit was cancelled.
    if (row->index < state_->document().layerCount()) {
        if (GtkWidget* value = gtk_widget_get_first_child(row->opacity)) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d%%",
                          static_cast<int>(std::lround(
                              state_->document().layerAt(row->index).opacity() * 100.0)));
            gtk_label_set_text(GTK_LABEL(value), buf);
        }
    }
}

void Window::refreshLayerHeader() {
    if (!layerActionButtons_) return;

    const Document& doc = state_->document();
    const size_t active = doc.activeIndex();
    const size_t count = doc.layerCount();

    // Disable what cannot be done to the current layer, rather than letting the
    // user press a button that can only report an error.
    struct Enable {
        int slot;
        bool on;
    };
    const bool canMoveUp = active + 1 < count;
    const bool canMoveDown = active > 0;
    const Enable states[] = {
        {kActionDuplicate, true},
        {kActionMoveUp, canMoveUp},
        {kActionMoveDown, canMoveDown},
        {kActionMerge, active > 0},
        {kActionDelete, count > 1},
    };

    // Read the slot back off the widget's own data, which is where it was stored.
    // g_object_get would look for a GObject property and find nothing.
    for (GtkWidget* child = gtk_widget_get_first_child(layerActionButtons_); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        const int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "action-slot"));
        for (const Enable& e : states) {
            if (slot == e.slot) gtk_widget_set_sensitive(child, e.on);
        }
    }
}

void Window::buildStatusBar(GtkWidget* root) {
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(bar, "statusbar");

    // A stylus, a touchscreen and a mouse are all ways in, so the label waits for
    // any of them rather than naming only the pen.
    statusDevice_ = gtk_label_new("Waiting for stylus, touch or mouse");
    statusMsg_ = gtk_label_new("");
    gtk_widget_set_hexpand(statusMsg_, TRUE);

    gtk_box_append(GTK_BOX(bar), statusDevice_);

    // A live pressure meter rather than a number buried in the device label: the
    // whole point of a pressure-sensitive brush is watching the level you are asking
    // for, and a figure that only appears while a pen is in use gives nothing to a
    // mouse user even though their strokes still carry pressure.
    GtkWidget* pressureGroup = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(pressureGroup, "pressure-group");

    GtkWidget* pressureLabel = gtk_label_new("Pressure");
    gtk_widget_add_css_class(pressureLabel, "pressure-label");
    gtk_box_append(GTK_BOX(pressureGroup), pressureLabel);

    pressureMeter_ = gtk_drawing_area_new();
    gtk_widget_add_css_class(pressureMeter_, "pressure-meter");
    gtk_widget_set_size_request(pressureMeter_, 150, 14);
    gtk_widget_set_valign(pressureMeter_, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(pressureMeter_,
                                "Pressure the brush is using right now, 0-100%");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pressureMeter_), &Window::drawPressureMeter,
                                   this, nullptr);
    gtk_box_append(GTK_BOX(pressureGroup), pressureMeter_);

    pressureOut_ = gtk_label_new("--");
    gtk_widget_add_css_class(pressureOut_, "pressure-out");
    gtk_widget_set_size_request(pressureOut_, 42, -1);
    gtk_box_append(GTK_BOX(pressureGroup), pressureOut_);

    // Not "pen pressure": the meter moves for a finger and a mouse too, since both
    // have their pressure derived from speed. Naming only the pen would leave the
    // readout unexplained for everyone not using a tablet.
    gtk_accessible_update_property(GTK_ACCESSIBLE(pressureOut_),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Current nib pressure", -1);
    gtk_box_append(GTK_BOX(bar), pressureGroup);

    gtk_box_append(GTK_BOX(bar), statusMsg_);

    GtkWidget* hud = gtk_label_new("pen  ·  pressure / tilt appear here while drawing");
    gtk_widget_add_css_class(hud, "hud");
    gtk_box_append(GTK_BOX(bar), hud);

    gtk_box_append(GTK_BOX(root), bar);
}

// The meter's job is to report level, so it is legible first and colour-faithful
// second. The default ink is near-black (#1b1b1f) and the status bar is near-black, so
// a faithful fill looked like an empty meter at low pressures -- the one case where
// the reading matters most. The ink is lightened just far enough to clear the bar,
// which keeps the hue preview while making the level readable.
Color legibleInk(const Color& ink) {
    constexpr double kBarLuma = 0.155;  // #23262b, the status bar background
    const double luma = 0.299 * ink.r + 0.587 * ink.g + 0.114 * ink.b;
    const double target = kBarLuma + 0.30;
    if (luma >= target) return ink;

    const double t = clampd((target - luma) / (1.0 - luma), 0.0, 1.0);
    return Color{ink.r + (1.0 - ink.r) * t, ink.g + (1.0 - ink.g) * t,
                 ink.b + (1.0 - ink.b) * t};
}

void Window::drawPressureMeter(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                               gpointer userData) {
    const auto* self = static_cast<Window*>(userData);
    (void)area;

    // 0 means "no sample yet", which is deliberately not the same as zero pressure:
    // showing a full-width empty track reads as "the pen is resting", while a flat
    // track with dashes says nothing has arrived yet.
    const bool known = self->pressureSeen_;

    const double radius = height / 2.0;
    cairo_save(cr);

    // Track.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.14);
    cairo_rectangle(cr, 0.0, 0.0, width, height);
    cairo_new_sub_path(cr);
    cairo_arc(cr, radius, radius, radius, 0.0, 2.0 * M_PI);
    cairo_fill(cr);

    if (!known) {
        // A short stub so the control looks empty rather than broken.
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.28);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, radius, height / 2.0);
        cairo_line_to(cr, width - radius, height / 2.0);
        cairo_stroke(cr);
        cairo_restore(cr);
        return;
    }

    // Fill, in the ink the brush is about to lay down, so the meter doubles as a
    // preview of the colour at that pressure.
    const double p = clampd(self->pressure_, 0.0, 1.0);
    const double filled = radius + (width - 2.0 * radius) * p;
    if (filled > radius) {
        const Brush& brush = self->state_->brush();
        const Color ink = legibleInk(Color{brush.r, brush.g, brush.b});
        cairo_set_source_rgb(cr, ink.r, ink.g, ink.b);
        cairo_rectangle(cr, 0.0, 0.0, filled, height);
        cairo_new_sub_path(cr);
        cairo_arc(cr, filled - radius, radius, radius, -M_PI / 2.0, M_PI / 2.0);
        cairo_fill(cr);
    }

    // A tick at the fill edge, so the level is readable even where the ink colour is
    // close to the track.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.45);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, std::floor(filled) + 0.5, 1.0);
    cairo_line_to(cr, std::floor(filled) + 0.5, height - 1.0);
    cairo_stroke(cr);

    cairo_restore(cr);
}

void Window::refreshPressure(double pressure) {
    const double clamped = clampd(pressure, 0.0, 1.0);

    // Repainting on every motion event would be pure waste once the rounded
    // percentage stops changing, and motion events arrive in bursts.
    const int percent = static_cast<int>(std::lround(clamped * 100.0));
    if (pressureSeen_ && percent == pressurePercent_) return;
    pressureSeen_ = true;
    pressurePercent_ = percent;

    if (pressureMeter_) gtk_widget_queue_draw(pressureMeter_);
    if (pressureOut_) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", percent);
        gtk_label_set_text(GTK_LABEL(pressureOut_), buf);
    }
}

void Window::addLayerAction() {
    if (canvas_) canvas_->cancelImageMove();
    state_->addLayer();
    refreshLayers();
}

void Window::duplicateLayerAction() {
    if (canvas_) canvas_->cancelImageMove();
    state_->duplicateLayer();
    refreshLayers();
}

void Window::deleteLayerAction() {
    if (canvas_) canvas_->cancelImageMove();
    state_->deleteLayer();
    if (state_->lastError()) reportError("Need at least one layer");
    refreshLayers();
}

void Window::mergeDownAction() {
    if (canvas_) canvas_->cancelImageMove();
    state_->mergeDown();
    if (state_->lastError()) reportError("Nothing below to merge into");
    refreshLayers();
}

void Window::moveLayerAction(int delta) {
    if (canvas_) canvas_->cancelImageMove();
    const size_t before = state_->document().activeIndex();
    state_->moveLayer(delta);
    // moveLayer is a no-op at the ends, and the buttons are disabled there anyway,
    // so this only skips a redundant rebuild.
    if (state_->document().activeIndex() == before) return;
    refreshLayers();
}

void Window::saveProject() {
    auto project = state_->serialize();
    if (!project) {
        refreshStatus("Could not encode project");
        return;
    }

    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Stylus Pen project");
    gtk_file_dialog_set_initial_name(dialog, "drawing.styluspen");

    auto* pair = new std::pair<Window*, std::shared_ptr<bool>>(this, alive_);
    gtk_file_dialog_save(
        dialog, GTK_WINDOW(window_), nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            auto* p = static_cast<std::pair<Window*, std::shared_ptr<bool>>*>(data);
            Window* self = p->first;
            std::shared_ptr<bool> alive = p->second;
            auto* dlg = GTK_FILE_DIALOG(source);
            g_autoptr(GError) error = nullptr;
            GFile* file = gtk_file_dialog_save_finish(dlg, result, &error);
            if (file) {
                if (alive && *alive && self) {
                    g_autoptr(GFileOutputStream) out =
                        g_file_replace(file, nullptr, FALSE, G_FILE_CREATE_NONE, nullptr, &error);
                    if (out) {
                        auto project = self->state_->serialize();
                        if (project) {
                            const std::string blob = encodeProjectBlob(*project);
                            gsize written = 0;
                            if (g_output_stream_write_all(G_OUTPUT_STREAM(out), blob.data(), blob.size(),
                                                          &written, nullptr, &error)) {
                                self->refreshStatus("Project saved");
                            } else {
                                self->refreshStatus("Could not write project");
                            }
                        } else {
                            self->refreshStatus("Could not encode project");
                        }
                    } else {
                        self->refreshStatus("Could not open file for writing");
                    }
                }
                g_object_unref(file);
            }
            delete p;
        },
        pair);

    g_object_unref(dialog);
}

void Window::openProject() {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Stylus Pen project");

    auto* pair = new std::pair<Window*, std::shared_ptr<bool>>(this, alive_);
    gtk_file_dialog_open(
        dialog, GTK_WINDOW(window_), nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            auto* p = static_cast<std::pair<Window*, std::shared_ptr<bool>>*>(data);
            Window* self = p->first;
            std::shared_ptr<bool> alive = p->second;
            auto* dlg = GTK_FILE_DIALOG(source);
            g_autoptr(GError) error = nullptr;
            GFile* file = gtk_file_dialog_open_finish(dlg, result, &error);
            if (file) {
                if (alive && *alive && self) {
                    g_autoptr(GFileInputStream) in = g_file_read(file, nullptr, &error);
                    if (in) {
                        g_autoptr(GBytes) bytes = g_input_stream_read_bytes(G_INPUT_STREAM(in),
                                                                            64 * 1024 * 1024, nullptr, &error);
                        if (bytes) {
                            gsize size = 0;
                            const char* raw = static_cast<const char*>(g_bytes_get_data(bytes, &size));
                            Project project;
                            if (decodeProjectBlob(std::string(raw, size), &project) && self->state_->load(project)) {
                                self->refreshStatus("Project opened");
                                self->refreshTools();
                                self->queueLayersRefresh();
                            } else {
                                self->refreshStatus("Unrecognised or corrupt project file");
                            }
                        } else {
                            self->refreshStatus("Could not read file");
                        }
                    } else {
                        self->refreshStatus("Could not read file");
                    }
                }
                g_object_unref(file);
            }
            delete p;
        },
        pair);

    g_object_unref(dialog);
}

void Window::exportPng() {
    auto png = state_->exportPng();
    if (!png) {
        refreshStatus("Could not encode PNG");
        return;
    }

    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export PNG");
    gtk_file_dialog_set_initial_name(dialog, "stylus-pen.png");

    auto* pair = new std::pair<Window*, std::shared_ptr<bool>>(this, alive_);
    gtk_file_dialog_save(
        dialog, GTK_WINDOW(window_), nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            auto* p = static_cast<std::pair<Window*, std::shared_ptr<bool>>*>(data);
            Window* self = p->first;
            std::shared_ptr<bool> alive = p->second;
            auto* dlg = GTK_FILE_DIALOG(source);
            g_autoptr(GError) error = nullptr;
            GFile* file = gtk_file_dialog_save_finish(dlg, result, &error);
            if (file) {
                if (alive && *alive && self) {
                    auto png = self->state_->exportPng();
                    if (png) {
                        g_autoptr(GFileOutputStream) out =
                            g_file_replace(file, nullptr, FALSE, G_FILE_CREATE_NONE, nullptr, &error);
                        if (out) {
                            gsize written = 0;
                            if (g_output_stream_write_all(G_OUTPUT_STREAM(out), png->data(), png->size(),
                                                          &written, nullptr, &error)) {
                                self->refreshStatus("PNG exported");
                            } else {
                                self->refreshStatus("Could not write PNG");
                            }
                        }
                    } else {
                        self->refreshStatus("Could not encode PNG");
                    }
                }
                g_object_unref(file);
            }
            delete p;
        },
        pair);

    g_object_unref(dialog);
}

gboolean Window::onKeyPressed(GtkEventControllerKey* ctrl, guint keyval, guint keycode,
                              GdkModifierType modifiers, Window* self) {
    (void)ctrl;
    (void)keycode;

    if (self->canvas_) self->canvas_->cancelImageMove();
    const bool ctrlDown = (modifiers & GDK_CONTROL_MASK) != 0;

    if (ctrlDown) {
        switch (keyval) {
            case GDK_KEY_z:
                if (modifiers & GDK_SHIFT_MASK) {
                    self->state_->redo();
                } else {
                    self->state_->undo();
                }
                self->refreshLayers();
                return TRUE;
            case GDK_KEY_y:
                self->state_->redo();
                self->refreshLayers();
                return TRUE;
            case GDK_KEY_s:
                self->saveProject();
                return TRUE;
            case GDK_KEY_e:
                self->mergeDownAction();
                return TRUE;
            case GDK_KEY_j:
                self->duplicateLayerAction();
                return TRUE;
            case GDK_KEY_t:
            case GDK_KEY_T:
                if (modifiers & GDK_SHIFT_MASK) {
                    self->showHandwritingToTextDialog();
                    return TRUE;
                } else {
                    self->showInsertTextDialog();
                    return TRUE;
                }
            case GDK_KEY_n:
                if (modifiers & GDK_SHIFT_MASK) {
                    self->addLayerAction();
                    return TRUE;
                }
                break;
            case GDK_KEY_0:
                self->canvas_->fitToWidget();
                return TRUE;
            case GDK_KEY_plus:
            case GDK_KEY_equal:
            case GDK_KEY_KP_Add:
                self->canvas_->zoomBy(1.2, -1.0, -1.0);
                return TRUE;
            case GDK_KEY_minus:
            case GDK_KEY_KP_Subtract:
                self->canvas_->zoomBy(1.0 / 1.2, -1.0, -1.0);
                return TRUE;
            default:
                break;
        }
    }

    switch (keyval) {
        case GDK_KEY_b:
            self->applyTool(BrushKind::Pen);
            return TRUE;
        case GDK_KEY_n:
            self->applyTool(BrushKind::Pencil);
            return TRUE;
        case GDK_KEY_m:
            self->applyTool(BrushKind::Marker);
            return TRUE;
        case GDK_KEY_e:
            self->applyTool(BrushKind::Eraser);
            return TRUE;
        case GDK_KEY_bracketleft:
            self->applySize(std::max(0.5, self->state_->brush().size - 1.0));
            gtk_range_set_value(GTK_RANGE(self->sizeScale_), self->state_->brush().size);
            return TRUE;
        case GDK_KEY_bracketright:
            self->applySize(std::min(40.0, self->state_->brush().size + 1.0));
            gtk_range_set_value(GTK_RANGE(self->sizeScale_), self->state_->brush().size);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

}  // namespace stylus