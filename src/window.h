#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

#include "canvas.h"
#include "project.h"

namespace stylus {

// One row in the layer panel. Owned by the row's click-gesture signal, and
// registered with the Window so a rebuild can find the row for a layer index
// without walking the widget tree.
struct LayerRow {
    class Window* self = nullptr;
    size_t index = 0;
    GtkWidget* name = nullptr;
    GtkWidget* opacity = nullptr;
    GtkWidget* eye = nullptr;
    // Guards the inline opacity slider: only one may be open per row.
    bool editing = false;
};

// Scratch state for an in-progress rename, so Enter, Escape and focus loss can all
// clean the same entry up exactly once.
struct RenameCtx {
    class Window* self = nullptr;
    size_t index = 0;
    GtkWidget* parent = nullptr;
    GtkWidget* entry = nullptr;
    GtkWidget* label = nullptr;
    bool done = false;

    // Takes the entry back out of the row and restores the label.
    //
    // This runs from an idle callback rather than from inside the signal handler
    // that ended the rename: unparenting drops the container's last reference, which
    // finalises the entry, and GTK is still using that object after the handler
    // returns. Destroying it in place is how a widget ends up freed while its own
    // signal emission is still on the stack.
    ~RenameCtx();
};

// The application window: toolbar, canvas, layer panel and status bar.
class Window {
public:
    explicit Window(AppState* state);
    ~Window();

    GtkWidget* widget() const { return window_; }

    // Realises the widgets and applies the initial zoom. Must run after the
    // window is shown, otherwise sizes are still 0 and fit-to-screen bails out.
    void start();
    void scheduleStart();

    void refreshTools();
    void refreshLayers();
    // Rebuilds the layer panel on the next idle, for callers that run inside a handler
    // belonging to the panel being rebuilt.
    void queueLayersRefresh();

    // Row interactions.
    void selectLayer(size_t index);
    void beginRename(size_t index);
    // Single exit for all three ways a rename can end, so the teardown cannot drift
    // apart between them. `commit` false abandons the edit and keeps the old name.
    void endRename(RenameCtx* ctx, bool commit);
    void beginOpacityEdit(LayerRow* row);
    // Takes the inline slider back off the row and restores the readout. Shared by
    // both exit paths, neither of which may rebuild the panel: each runs from a
    // controller the row itself owns.
    void endOpacityEdit(LayerRow* row, GtkWidget* scale = nullptr);
    void refreshLayerHeader();
    GtkWidget* makePanelButton(const char* label, const char* tip);

    // True while the layer rows are being rebuilt, so the handlers they carry do
    // not echo a programmatic rebuild back into the state.
    bool rebuildingLayers() const { return rebuildingLayers_; }
    void refreshStatus(const std::string& text);
    void refreshZoom(const std::string& text);

    // Action slots in the layer header, so refreshLayerHeader can enable and
    // disable each button by position without string matching.
    enum LayerActionSlot {
        kActionDuplicate,
        kActionMoveUp,
        kActionMoveDown,
        kActionMerge,
        kActionDelete,
    };

    static constexpr int kSidebarWidth = 280;

    LayerRow* findLayerRow(size_t index) const;

private:
    static gboolean onKeyPressed(GtkEventControllerKey* ctrl, guint keyval, guint keycode,
                                 GdkModifierType state, Window* self);
    static gboolean onStatusExpired(gpointer self);
    static void drawPaperChip(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                              gpointer userData);
    static void drawPressureMeter(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                                  gpointer userData);
    // Live pen pressure, 0..1, and the rounded percentage last shown.
    void refreshPressure(double pressure);
    static void drawBrushPreview(GtkDrawingArea* area, cairo_t* cr, int width, int height,
                                 gpointer userData);

    void buildToolbar(GtkWidget* header);
    // Tilt, pressure-width and stabilizer toggles. These sit in the sidebar, not
    // the toolbar: on one row they pushed the file buttons off the end of the
    // window, and their labels inherited a dark-on-dark colour from the toolbar
    // group's stylesheet.
    void buildBrushOptions(GtkWidget* sidebar);
    // Draws the current brush footprint at its real document size, so the size
    // slider shows the mark the brush will actually leave.
    void refreshBrushPreview();
    void buildLayerPanel(GtkWidget* sidebar);
    void buildStatusBar(GtkWidget* bottom);
    void loadStyle();
    void applyTool(BrushKind kind);
    void applyColor(const std::string& hex);
    void applySize(double size);
    void applyPressureSensitivity(double level);
    void wireFileActions(GtkWidget* header);
    void reportError(const std::string& fallback);
    // Switches brushes when the pen is flipped to its eraser end.
    void setEraserEnd(bool active);

    void moveLayerAction(int delta);

    // Page background. The button shows the current colour and opens the system
    // colour dialog; applyBackground is the only place the state is changed.
    void applyBackground(const std::string& hex);
    void refreshBackgroundButton();

    // Placed images and text.
    void chooseImageToInsert();
    void insertImageFromFile(GFile* file);
    void showInsertTextDialog();
    void showInsertTextDialogConfirm(void* data);
    void showHandwritingToTextDialog();
    void showHandwritingToTextDialogConfirm(void* data);
    void showEditTextDialog();
    void showEditTextDialogConfirm(void* data);
    void refreshMoveMode();
    // The picture controls. The whole section is hidden unless the active layer
    // actually holds a picture, so it never takes up room for something it cannot do.
    void buildImageSection(GtkWidget* box);
    void refreshImageSection();

    // Page ruling chips plus the spacing slider.
    void buildPaperOptions(GtkWidget* box);
    void applyPaper(PaperKind kind, double spacing);
    void refreshPaperOptions();
    // Undo and redo availability, so the toolbar buttons disable themselves rather
    // than silently doing nothing.
    void refreshHistoryButtons();

    void saveProject();
    void openProject();
    void exportPng();

    using LayerAction = void (Window::*)();

    void addLayerAction();
    void duplicateLayerAction();
    void deleteLayerAction();
    void mergeDownAction();
    // Reorder shortcuts: the panel buttons call moveLayerAction directly, these
    // exist so the header table can name a bound member function.
    void moveLayerUpAction() { moveLayerAction(+1); }
    void moveLayerDownAction() { moveLayerAction(-1); }

    AppState* state_;
    GtkWidget* window_ = nullptr;
    std::unique_ptr<Canvas> canvas_;

    GtkWidget* statusDevice_ = nullptr;
    GtkWidget* statusMsg_ = nullptr;
    GtkWidget* zoomLabel_ = nullptr;
    GtkWidget* sizeScale_ = nullptr;
    GtkWidget* sizeOut_ = nullptr;
    GtkWidget* layerList_ = nullptr;
    GtkWidget* layerHeader_ = nullptr;
    GtkWidget* layerActionButtons_ = nullptr;
    GtkWidget* brushPreview_ = nullptr;

    // Rows for the current layer stack, topmost first as displayed. Valid only
    // between rebuilds; the rebuild clears it before destroying the widgets.
    std::vector<std::shared_ptr<LayerRow>> layerRows_;

    GtkWidget* toolButtons_[4] = {nullptr, nullptr, nullptr, nullptr};
    GtkWidget* swatchButtons_[12] = {};
    GtkWidget* tiltCheck_ = nullptr;
    GtkWidget* pressureCheck_ = nullptr;
    GtkWidget* pressureScale_ = nullptr;
    GtkWidget* sensitivityOut_ = nullptr;
    GtkWidget* stabilizeCheck_ = nullptr;
    GtkWidget* backgroundButton_ = nullptr;
    // Holds the swatch rule for backgroundButton_. GTK4 dropped
    // gtk_widget_override_background_color, so the colour is painted by CSS keyed
    // on a class, the same trick the toolbar palette uses.
    GtkCssProvider* backgroundProvider_ = nullptr;
    GtkColorDialog* colorDialog_ = nullptr;
    GtkWidget* paperButtons_[kPaperKindCount] = {};
    GtkWidget* paperScale_ = nullptr;
    GtkWidget* paperGapOut_ = nullptr;
    GtkWidget* undoButton_ = nullptr;
    GtkWidget* redoButton_ = nullptr;
    GtkWidget* moveImageButton_ = nullptr;
    GtkWidget* imageSection_ = nullptr;
    GtkWidget* imageScale_ = nullptr;
    GtkWidget* imageScaleOut_ = nullptr;

    // True while the toolbar is being synchronised from state, so the button
    // handlers do not echo the programmatic updates back.
    bool updatingTools_ = false;

    // Same idea for the layer rows: rebuilding one must not fire a selection or
    // visibility change back into AppState, which would fight the rebuild.
    bool rebuildingLayers_ = false;
    bool layersRefreshQueued_ = false;
    guint layersRefreshIdle_ = 0;
    guint startIdle_ = 0;

    // Tokens for the AppState listeners this window owns, so they can be dropped
    // without disturbing the ones the canvas registered.
    std::vector<size_t> listenerTokens_;

    guint statusTimeout_ = 0;

    // Live pressure meter. pressureSeen_ is separate from the value because "no
    // sample yet" has to look different from "the pen is resting at zero".
    GtkWidget* pressureMeter_ = nullptr;
    GtkWidget* pressureOut_ = nullptr;
    double pressure_ = 0.0;
    int pressurePercent_ = -1;
    bool pressureSeen_ = false;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace stylus