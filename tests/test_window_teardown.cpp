// Builds and destroys the real Window, repeatedly, with a picture layer active and focus
// moved on and off the image scale slider.
//
// This covers the one part of the app no other test reaches: window lifetime. The
// sidebar handlers capture a raw Window*, and GTK emits notify signals while it
// finalises widgets, so a notify landing after ~Window() had begun would run a handler
// against a destroyed Window. Window::disconnectHandlersFromTree() is the guard for
// that, and this is where it is exercised.
//
// Being straight about what this does and does not do: it did not reproduce the
// intermittent crash that guard was written for. That one was observed twice, under
// UBSan, as a drag flag read out of freed memory with a layer vector already torn down;
// disabling the guard no longer brings it back, so the link between guard and crash is
// unproven. Treat this as a lifetime smoke test that happens to cover the guard's code
// path -- not as proof the crash is fixed. See the note in README.md.

#include <gtk/gtk.h>

#include <cstdio>

#include "project.h"
#include "window.h"

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

void drain() {
    for (int i = 0; i < 40; ++i) g_main_context_iteration(nullptr, FALSE);
}

// Builds a window, shows it, and tears it down again. Any notify that escapes into a
// dead Window runs here, under ASan.
// Finds the scale slider in the sidebar, which is the widget whose notify::has-focus
// handler reached into the AppState.
GtkWidget* findScale(GtkWidget* widget) {
    if (GTK_IS_SCALE(widget)) return widget;
    for (GtkWidget* child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget* found = findScale(child)) return found;
    }
    return nullptr;
}

void oneCycle(int index) {
    AppState* state = new AppState(400, 300);

    // The IMAGE section, and with it the scale slider, only exists once a picture layer
    // is active. That is the state the crash was seen in.
    state->insertImage(makeSurface(80, 60), "Photo");

    Window* window = new Window(state);

    gtk_window_present(GTK_WINDOW(window->widget()));
    drain();

    // Move focus on and off the slider: notify::has-focus is what drove the handler.
    if (GtkWidget* scale = findScale(window->widget())) {
        gtk_widget_grab_focus(scale);
        drain();
        gtk_widget_grab_focus(window->widget());
        drain();
    }

    // Destroying the GtkWindow is what a user closing the window does. The Window
    // object itself is deleted afterwards, mirroring main.cpp: the app owns both and
    // the window has to go first, because its handlers call the state.
    gtk_window_destroy(GTK_WINDOW(window->widget()));
    drain();

    delete window;
    delete state;
    drain();

    if ((index + 1) % 5 == 0) std::printf("     ...%d cycles\n", index + 1);
}

}  // namespace

int main() {
    gtk_init();

    // Fifteen is enough to be confident and still fast. The unfixed build fails well
    // inside the first few.
    for (int i = 0; i < 15; ++i) oneCycle(i);
    // Selecting a layer defers its panel rebuild to an idle. That deferral is the fix
    // for the crash: selecting ran from the row's own GtkGesture "pressed" handler, and
    // rebuilding the panel there tore down the very gesture whose emission was still on
    // the stack. This drives selectLayer() directly -- a stylus cannot be synthesised --
    // and checks the rebuild lands and the picture layer stays selectable, since that
    // is the layer Move image acts on.
    {
        AppState* state = new AppState(400, 300);
        state->addLayer();
        state->insertImage(makeSurface(64, 48), "Photo");
        Window* window = new Window(state);
        gtk_window_present(GTK_WINDOW(window->widget()));
        drain();

        const size_t pictureIndex = state->document().layerCount() - 1;
        window->selectLayer(pictureIndex);
        drain();
        check(state->document().activeIndex() == pictureIndex,
              "selecting the picture layer moved the selection");
        check(state->activeLayerHasImage(), "the picture layer is the one selected");
        check(state->beginMoveActiveImage(), "and it is draggable");

        window->selectLayer(0);
        drain();
        check(state->document().activeIndex() == 0, "selecting another layer moved back");
        check(!state->activeLayerHasImage(), "and the picture is no longer the active layer");
        check(!state->beginMoveActiveImage(), "so Move image has nothing to drag");

        window->selectLayer(pictureIndex);
        drain();
        check(state->activeLayerHasImage(), "selecting it again brings the picture back");

        gtk_window_destroy(GTK_WINDOW(window->widget()));
        drain();
        delete window;
        delete state;
    }

    {
        AppState* state = new AppState(400, 300);
        state->insertImage(makeSurface(80, 60), "Photo");
        Window* window = new Window(state);
        gtk_window_present(GTK_WINDOW(window->widget()));
        drain();
        check(findScale(window->widget()) != nullptr, "the image scale slider was found");
        gtk_window_destroy(GTK_WINDOW(window->widget()));
        drain();
        delete window;
        delete state;
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}