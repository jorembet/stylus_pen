#include <gtk/gtk.h>

#include <cstdio>
#include <string>

#include "platform.h"
#include "project.h"
#include "window.h"

namespace {

// Matches Icon= in data/stylus-pen.desktop, so the window carries the same
// picture the launcher shows.
constexpr const char* kIconName = "stylus-pen";

// Adds the directories this build can be launched from to the icon theme's search
// path.
//
// gtk_window_set_icon_name() resolves a name through the icon theme, and the theme
// only searches the installed prefixes, so a run straight from the build directory
// finds nothing and the window falls back to the generic GTK icon -- the running
// window then looks nothing like the launcher entry. Registering the source and
// install locations makes ./run.sh and the installed app resolve to the same file.
void addIconSearchPaths() {
    const std::string dir = stylus::executableDirectory();
    if (dir.empty()) return;

    // build/bin/stylus-pen -> <repo>/data, and <prefix>/bin/stylus-pen ->
    // <prefix>/share/icons/hicolor/scalable/apps.
    const std::string candidates[] = {
        stylus::joinPath(dir, "../../data"),
        stylus::joinPath(dir, "../share/icons/hicolor/scalable/apps"),
    };

    GdkDisplay* display = gdk_display_get_default();
    if (!display) return;
    GtkIconTheme* theme = gtk_icon_theme_get_for_display(display);
    for (const std::string& candidate : candidates) {
        if (g_file_test(candidate.c_str(), G_FILE_TEST_IS_DIR)) {
            gtk_icon_theme_add_search_path(theme, candidate.c_str());
        }
    }
}

// Owns the window and the state together, so their order of destruction is a property
// of one destructor instead of the order two g_object_set_data_full() keys happened to
// be registered in.
//
// This ordering is load bearing. GTK emits notify signals while it finalises widgets, so
// tearing the window down runs the sidebar handlers one last time, and those handlers
// call into the AppState. When the state went first, that last call landed on a freed
// AppState: commitImageScale() read a drag flag out of released memory, and
// Document::active() indexed a vector whose storage had already been torn down. The
// app took the window down with it -- intermittently, because it depends on which
// widget finalisation happens to emit a notify.
struct Session {
    stylus::AppState* state = nullptr;
    stylus::Window* window = nullptr;

    ~Session() {
        // The window owns signal handlers that call the state, so it has to be gone
        // before the state is.
        delete window;
        delete state;
    }
};

void onActivate(GtkApplication* app, gpointer userData) {
    (void)userData;

    // Reuse the existing window if the app is activated again (second launch,
    // or a session restore) instead of building a second one.
    GList* windows = gtk_application_get_windows(app);
    if (windows != nullptr) {
        gtk_window_present(GTK_WINDOW(windows->data));
        return;
    }

    addIconSearchPaths();

    auto* session = new Session();
    session->state = new stylus::AppState(1600, 1100);
    session->window = new stylus::Window(session->state);
    stylus::AppState* state = session->state;
    stylus::Window* window = session->window;

    gtk_window_set_icon_name(GTK_WINDOW(window->widget()), kIconName);

    // A GtkWindow only holds the application alive once it is associated with
    // it; without this the app exits immediately after activate returns.
    gtk_window_set_application(GTK_WINDOW(window->widget()), app);

    // One key, one owner: the destructor above fixes the teardown order.
    g_object_set_data_full(G_OBJECT(app), "stylus-session", session,
                           [](gpointer p) { delete static_cast<Session*>(p); });

    gtk_window_present(GTK_WINDOW(window->widget()));
    window->scheduleStart();
}

}  // namespace

int main(int argc, char** argv) {
    GtkApplication* app =
        gtk_application_new("com.local.styluspen", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(onActivate), nullptr);

    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
