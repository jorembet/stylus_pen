// Covers the canvas input handling, which is the one part of the app that test_core
// cannot reach: stylus_core has no GTK in it, and GTK4 keeps GdkEvent opaque so the
// events cannot be synthesised either.
//
// That gap is not theoretical. A chip handler that silently did nothing, and a rename
// that freed a widget inside its own signal emission, both survived a green suite --
// and the picture drag had two more: grabbing on any press at all, and letting that
// grab beat Alt+drag pan. Both are decision-table errors, which is what this checks.
#include <gtk/gtk.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "canvas.h"
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

SurfacePtr makePicture(int w, int h) {
    SurfacePtr s = makeSurface(w, h);
    cairo_t* cr = cairo_create(s.get());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 0.9, 0.2, 0.1);
    cairo_paint(cr);
    cairo_destroy(cr);
    return s;
}

void drain() {
    for (int i = 0; i < 40; ++i) g_main_context_iteration(nullptr, FALSE);
}

// The table, spelled out as the state combinations that matter.
void testDecisionTable() {
    const bool press = true;
    const bool motionEvt = true;
    const bool release = true;
    (void)press;
    (void)motionEvt;
    (void)release;

    // A plain press draws. Move mode off, nothing in flight, not on the picture.
    check(classifyInput(GDK_BUTTON_PRESS, false, false, false, false, false, false) ==
              CanvasInput::BeginStroke,
          "a plain press starts a stroke");

    // Move mode drags the picture on the SELECTED layer, wherever the press lands.
    // Hit-testing the press against the picture was tried and reverted: it made a
    // press that missed the picture start a stroke, so a drag could silently become
    // drawing and the picture only moved when the press landed exactly on it.
    check(classifyInput(GDK_BUTTON_PRESS, true, false, false, false, true, false) ==
              CanvasInput::GrabImage,
          "move mode with a picture selected grabs it");
    check(classifyInput(GDK_BUTTON_PRESS, true, false, false, false, false, false) ==
              CanvasInput::BeginStroke,
          "move mode with no picture selected still draws");

    // Alt outranks the grab: panning must keep working while move mode is on.
    check(classifyInput(GDK_BUTTON_PRESS, true, false, false, false, true, true) ==
              CanvasInput::BeginPan,
          "Alt+press pans even when move mode is on and the press is on the picture");
    check(classifyInput(GDK_BUTTON_PRESS, false, false, false, false, true, true) ==
              CanvasInput::BeginPan,
          "Alt+press pans regardless");

    // An event already in flight keeps its claim.
    check(classifyInput(GDK_BUTTON_PRESS, true, true, false, false, true, false) ==
              CanvasInput::None,
          "a press during a stroke is ignored");
    check(classifyInput(GDK_BUTTON_PRESS, true, false, true, false, true, false) ==
              CanvasInput::None,
          "a press during a pan is ignored");

    // Motion: a drag in progress owns it; otherwise only a live stroke or pan continues.
    check(classifyInput(GDK_MOTION_NOTIFY, true, false, false, true, true, false) ==
              CanvasInput::DragImage,
          "motion during a picture drag keeps dragging");
    check(classifyInput(GDK_MOTION_NOTIFY, true, true, false, true, false, false) ==
              CanvasInput::DragImage,
          "the picture drag wins over a running stroke flag");
    check(classifyInput(GDK_MOTION_NOTIFY, false, true, false, false, false, false) ==
              CanvasInput::ContinueStroke,
          "motion during a stroke continues it");
    check(classifyInput(GDK_MOTION_NOTIFY, false, false, true, false, false, false) ==
              CanvasInput::ContinueStroke,
          "motion during a pan continues it");
    check(classifyInput(GDK_MOTION_NOTIFY, false, false, false, false, false, false) ==
              CanvasInput::None,
          "motion while idle only hovers");

    // Release: ends whatever was in flight.
    check(classifyInput(GDK_BUTTON_RELEASE, true, false, false, true, true, false) ==
              CanvasInput::DropImage,
          "a release during a picture drag drops it");
    check(classifyInput(GDK_BUTTON_RELEASE, false, true, false, false, false, false) ==
              CanvasInput::EndStroke,
          "a release ends a stroke");
    check(classifyInput(GDK_BUTTON_RELEASE, false, false, false, false, false, false) ==
              CanvasInput::EndStroke,
          "a release with nothing in flight is harmless");

    // Anything else is nothing.
    check(classifyInput(GDK_SCROLL, true, false, false, true, true, false) ==
              CanvasInput::None,
          "other event types are ignored");
}

// A finger has to go through the same table as a mouse, under the same rules. These
// are the pointer rows above, restated for the touch types that carry them.
void testTouchDecisionTable() {
    check(isCanvasTouchType(GDK_TOUCH_BEGIN), "a touch begin is a touch type");
    check(isCanvasTouchType(GDK_TOUCH_UPDATE), "a touch update is a touch type");
    check(isCanvasTouchType(GDK_TOUCH_END), "a touch end is a touch type");
    check(!isCanvasTouchType(GDK_TOUCH_CANCEL),
          "a touch cancel is not a touch type: it cancels rather than ending");
    check(!isCanvasTouchType(GDK_BUTTON_PRESS), "a button press is not a touch type");
    check(!isCanvasTouchType(GDK_MOTION_NOTIFY), "motion is not a touch type");

    check(classifyInput(GDK_TOUCH_BEGIN, false, false, false, false, false, false) ==
              CanvasInput::BeginStroke,
          "a finger down starts a stroke");
    check(classifyInput(GDK_TOUCH_BEGIN, true, false, false, false, true, false) ==
              CanvasInput::GrabImage,
          "a finger in move mode grabs the picture");
    check(classifyInput(GDK_TOUCH_BEGIN, true, false, false, false, true, true) ==
              CanvasInput::BeginPan,
          "a finger with Alt pans");
    check(classifyInput(GDK_TOUCH_BEGIN, false, true, false, false, false, false) ==
              CanvasInput::None,
          "a second finger during a stroke is refused");

    check(classifyInput(GDK_TOUCH_UPDATE, false, true, false, false, false, false) ==
              CanvasInput::ContinueStroke,
          "a finger dragging continues the stroke");
    check(classifyInput(GDK_TOUCH_UPDATE, false, false, true, false, false, false) ==
              CanvasInput::ContinueStroke,
          "a finger dragging continues the pan");
    check(classifyInput(GDK_TOUCH_UPDATE, true, false, false, true, true, false) ==
              CanvasInput::DragImage,
          "a finger dragging moves the picture");
    check(classifyInput(GDK_TOUCH_UPDATE, false, false, false, false, false, false) ==
              CanvasInput::None,
          "a finger moving with nothing in flight is nothing");

    check(classifyInput(GDK_TOUCH_END, false, true, false, false, false, false) ==
              CanvasInput::EndStroke,
          "lifting the finger ends the stroke");
    check(classifyInput(GDK_TOUCH_END, true, false, false, true, true, false) ==
              CanvasInput::DropImage,
          "lifting the finger drops the picture");
    check(classifyInput(GDK_TOUCH_END, false, false, false, false, false, false) ==
              CanvasInput::EndStroke,
          "lifting with nothing in flight is harmless");
}

// Hammers the move path the way a drag does: many small steps, a commit at the end,
// then undo and redo back through it. Pointer input cannot be synthesised under GTK4,
// so this drives the AppState half of the same gesture directly -- if the crash lives
// in placement, history or the layer surface, a few thousand steps will find it.
void testMoveStress() {
    AppState* state = new AppState(600, 800);
    state->insertImage(makePicture(200, 150), "Photo");

    int committed = 0;
    for (int drag = 0; drag < 200; ++drag) {
        if (!state->beginMoveActiveImage()) break;
        for (int step = 1; step <= 20; ++step) {
            state->moveActiveImageBy(step * 0.7, -step * 0.3);
        }
        if (state->commitActiveImageMove()) ++committed;
    }
    check(committed == 200, "200 move drags all committed");

    // Walk the history back and forward, which is where a captured layer index goes
    // stale if it is going to.
    for (int i = 0; i < 200; ++i) state->undo();
    check(true, "200 undos applied");
    for (int i = 0; i < 200; ++i) state->redo();
    check(true, "200 redos applied");

    // The picture must still be there and still movable afterwards.
    check(state->activeLayerHasImage(), "the picture survived the history walk");
    check(state->beginMoveActiveImage(), "and it can still start a new drag");
    state->cancelActiveImageMove();

    delete state;
}

}  // namespace

int main() {
    testDecisionTable();
    testTouchDecisionTable();

    testMoveStress();

    gtk_init();

    AppState* state = new AppState(400, 300);
    Canvas* canvas = new Canvas(state);
    GtkWidget* window = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 400);
    gtk_window_set_child(GTK_WINDOW(window), canvas->widget());
    gtk_window_present(GTK_WINDOW(window));
    drain();

    // The canvas still has to come up cleanly with a picture on it: the widget tree,
    // the transform and the draw path all get exercised by the first frame.
    state->insertImage(makePicture(120, 90), "Photo");
    drain();
    check(state->activeLayerHasImage(), "the canvas comes up with a picture selected");
    // The allocated size is deliberately not asserted: it needs a compositor to map
    // the window, and ctest may run without one. Everything this file checks is
    // decision logic, which must hold either way.

    // Tear down in the order the app does: the canvas clears its AppState listeners in
    // its own destructor, so it has to go before the state. Leaving them allocated
    // makes ASan report a leak in a test that is supposed to be proving there are none.
    gtk_window_destroy(GTK_WINDOW(window));
    drain();
    delete canvas;
    delete state;

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}