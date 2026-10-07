#pragma once

#include <cairo/cairo.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "brush.h"
#include "document.h"
#include "geometry.h"
#include "history.h"

#include "ocr.h"

namespace stylus {

// Copies a sub-rectangle of a surface. Used to snapshot just the area a stroke
// touched, which keeps undo cheap for long strokes.
//
// The request is clipped to the surface, so a stroke running off the edge of the
// page yields a snapshot smaller than `region` and placed at a different origin.
// `clipped` receives that actual rectangle, and it is what has to be handed back
// to blitRegion: blitting a clipped snapshot at the unclipped origin shifts the
// restored pixels away from where the stroke was.
SurfacePtr copyRegion(cairo_surface_t* src, const Rect& region, Rect* clipped);

// Restores a snapshot into the region of a destination surface it was copied
// from. The blit is clipped to that region, so ink elsewhere on the layer is
// left alone.
void blitRegion(const SurfacePtr& src, const Rect& region, cairo_surface_t* dst);

struct ProjectSettings {
    BrushKind tool = BrushKind::Pen;
    std::string color = "#1b1b1f";
    double size = 3.0;
    bool tiltShading = true;
    bool pressureWidth = true;
    bool stabilize = true;
    // 0..1, matching Brush::pressureSensitivity. Defaults to 1 so a file written
    // before this setting existed draws exactly as it did when it was saved.
    double pressureSensitivity = 1.0;
};

struct Project {
    int version = 1;
    int width = 1600;
    int height = 1100;
    size_t activeIndex = 0;
    ProjectSettings settings;
    // Page colour as "#rrggbb". Lives on the Project rather than in
    // ProjectSettings because it is document state: it is baked into an exported
    // PNG, so it belongs with the width and height, not with the brush.
    std::string background = "#fbfbfd";
    // Page ruling. Written as its own tag rather than folded into the settings
    // line, because it is page state like the colour, not a brush property.
    PaperKind paperKind = PaperKind::Plain;
    double paperSpacing = 32.0;
    size_t layerCount() const { return layerPngs.size(); }

    // One PNG payload per layer, bottom layer first.
    std::vector<std::string> layerPngs;
    std::vector<std::string> layerNames;
    std::vector<char> layerVisible;
    std::vector<double> layerOpacity;
};

std::string encodePng(cairo_surface_t* surface);
std::optional<SurfacePtr> decodePng(const std::string& bytes, int width, int height);

std::optional<Project> serializeProject(const Document& doc, const ProjectSettings& settings);

// On-disk container: a "STYLUSPEN <w> <h> <n>" header followed by <len>\n
// length-prefixed PNG payloads, one per layer from the bottom up.
std::string encodeProjectBlob(const Project& project);
bool decodeProjectBlob(const std::string& blob, Project* out);
std::optional<Document> deserializeProject(const Project& project);

bool writeFile(const std::string& path, const std::string& bytes);
std::optional<std::string> readFile(const std::string& path);

// Owns the drawing state: document, live stroke, history, and the undo/redo
// wiring for both strokes and structural layer edits.
class AppState {
public:
    // Each channel fans out to every registered listener.
    //
    // These used to hold a single callback each, which meant the second widget to
    // register silently replaced the first: Window registered after Canvas and
    // took over the layer channels, so hiding a layer or changing its opacity
    // refreshed the panel but never repainted the page. Both widgets listen now.
    using ChangedCallback = std::function<void()>;
    using LayersChangedCallback = std::function<void()>;
    // Fired when one layer's name changes, carrying the index and the new name.
    // Separate from the structural channel because a rename must not rebuild the
    // panel: the rename is committed from inside a widget the panel owns.
    using NameChangedCallback = std::function<void(size_t, const std::string&)>;

    AppState(int width = 1600, int height = 1100);

    Document& document() { return doc_; }
    const Document& document() const { return doc_; }
    History& history() { return history_; }

    Brush& brush() { return brush_; }
    const Brush& brush() const { return brush_; }

    ProjectSettings& settings() { return settings_; }
    const ProjectSettings& settings() const { return settings_; }

    // Listeners are appended, never replaced, and the token returned by each call is
    // what clearListener() takes. Removing from inside a callback is safe.
    size_t setChanged(ChangedCallback cb);
    size_t setLayersChanged(LayersChangedCallback cb);
    size_t setLayerPixelChanged(LayersChangedCallback cb);
    size_t setNameChanged(NameChangedCallback cb);

    // Drops a listener by token. Used when a widget is destroyed mid-session.
    void clearListener(size_t token);

    bool drawing() const { return drawing_; }
    const Stroke& liveStroke() const { return live_; }

    // Starts a stroke at `p`. Ignored when a stroke is already in progress, because
    // a pen contact can arrive as two press events in the same frame.
    void beginStroke(const Point& p);
    // Appends a sample. Returns false if the sample was rejected as a duplicate.
    bool extendStroke(const Point& p);
    void endStroke();
    void cancelStroke();

    void undo();
    void redo();

    void addLayer();
    void duplicateLayer();
    void deleteLayer();
    void moveLayer(int delta);
    void mergeDown();
    void toggleVisibility(size_t index);

    // Per-layer properties. Each one changes how the layer composites, so each one
    // re-renders the canvas.
    //
    // None of these announce a structural change. A rename or an opacity change
    // leaves the layer stack exactly as it was, and announcing one makes the panel
    // rebuild itself mid-interaction -- which destroys the widget the caller is
    // standing in and leaves that caller writing to freed memory.
    void setLayerName(size_t index, std::string name);
    void clearLayer(size_t index);

    // Opacity is edited by dragging a slider, so it arrives as a stream of values.
    // The live calls apply without touching history, and one entry is pushed when
    // the gesture ends: a drag would otherwise leave one undo step per pixel of
    // movement.
    void setLayerOpacityLive(size_t index, double opacity);
    void commitLayerOpacity(size_t index);
    // Abandons a drag and puts the layer back where it started, recording nothing.
    void cancelLayerOpacity(size_t index);

    void clearActiveLayer();
    void clearAllLayers();
    void newCanvas();

    // ---- placed images ----

    // Adds `source` as a new dedicated image layer, centred on the page and scaled
    // down only if it is larger than the page. Undo removes it again.
    bool insertImage(SurfacePtr source, std::string name);

    // Renders custom text into a new text layer placed on the page.
    bool insertText(const std::string& text, double fontSize = 36.0, const Color& color = Color{0.1, 0.1, 0.12});
    bool insertTextFormatted(const Layer::TextInfo& info, std::optional<double> customX = std::nullopt, std::optional<double> customY = std::nullopt);
    bool insertTextWithCurrentBrush(const std::string& text, double fontSize = 36.0);
    bool updateActiveTextLayer(const Layer::TextInfo& info);
    bool activeLayerHasText() const;
    Layer::TextInfo activeLayerTextInfo() const;

    // Recognizes text from active layer handwriting strokes.
    RecognizedText recognizeActiveLayerHandwriting() const;
    // Converts handwriting on active layer into a new text layer placed at bounds, optionally clearing original.
    bool convertHandwritingToText(const std::string& text, const Rect& bounds, bool replaceOriginal, double fontSize = 36.0);
    // Learns confirmed handwriting word so recognition adapts over time
    void learnHandwritingWord(const std::string& word);


    // Dragging an image is a stream of live updates that collapse into one history
    // entry, the same shape as an opacity drag: a per-motion push would leave one
    // undo step per pixel of travel.
    bool beginMoveActiveImage();
    // `dx`/`dy` are document-space deltas.
    bool moveActiveImageBy(double dx, double dy);
    bool commitActiveImageMove();
    void cancelActiveImageMove();
    bool isImageMoving() const { return imageDrag_.active; }
    // True while the active layer holds an image, which is what decides whether a
    // drag in move mode moves anything.
    bool activeLayerHasImage() const;
    bool canMoveActiveLayer() const;

    // Resizing a picture, same stream-then-commit shape as a move and an opacity
    // drag: a slider emits a value per step, and one undo entry per step would bury
    // the stroke history.
    bool setImageScaleLive(double scale);
    bool commitImageScale();
    void cancelImageScale();
    // Doubles the picture, or fits it to the page if it is already bigger.
    bool scaleActiveImageToPage();
    void centreActiveImage();
    // Deletes the active picture layer, reporting false when the active layer holds
    // no image so the caller can say why nothing happened.
    bool deleteActiveImage();

    // Repaints the page. Returns false and changes nothing when the hex is
    // malformed, so a bad value can never reach the canvas.
    //
    // Deliberately not undoable on its own: the page colour is not artwork, and a
    // history entry per colour tweak would drown the stroke history. It does
    // travel through undo whenever a structural step is undone, because
    // captureStack carries it.
    bool setBackgroundColor(const std::string& hex);

    // Page ruling. Spacing is clamped, and an unknown kind is ignored rather than
    // guessed at, so a hand-edited file cannot pick a pattern that does not exist.
    // Reports whether anything was applied.
    bool setPaper(PaperKind kind, double spacing);

    std::optional<Project> serialize() const;
    bool load(const Project& project);
    std::optional<std::string> exportPng() const;

    std::optional<std::string> lastError() const { return lastError_; }

private:
    // One channel per kind of change. Tokens are indices into the vector, and a
    // cleared slot is left in place with an empty callable so live indices stay
    // stable across removals.
    struct Channel {
        std::vector<ChangedCallback> listeners;
    };

    // Separate storage because the callback signature carries the layer index and
    // name; a shared vector would mean type-erasing the arguments away.
    struct NameChannel {
        std::vector<NameChangedCallback> listeners;
    };

    size_t addListener(Channel& channel, ChangedCallback cb);

    void notify() {
        for (const ChangedCallback& cb : changed_.listeners) {
            if (cb) cb();
        }
    }
    void notifyLayers() {
        for (const ChangedCallback& cb : layersChanged_.listeners) {
            if (cb) cb();
        }
    }
    void notifyLayerPixels() {
        for (const ChangedCallback& cb : layerChanged_.listeners) {
            if (cb) cb();
        }
    }
    void notifyNameChanged(size_t index, const std::string& name) {
        for (const NameChangedCallback& cb : nameChanged_.listeners) {
            if (cb) cb(index, name);
        }
    }

    Document doc_;
    History history_;
    Brush brush_;
    ProjectSettings settings_;

    Stroke live_;
    size_t liveLayerIndex_ = 0;
    bool drawing_ = false;

    std::optional<std::string> lastError_;

    // Opacity of a layer at the point a drag started, held until the gesture ends
    // so the whole drag collapses into a single undo entry.
    struct ImageMoveDrag {
        size_t index = 0;
        bool active = false;
        bool moved = false;
        double startX = 0.0;
        double startY = 0.0;
        SurfacePtr startSnapshot;
        bool hasImage = false;
    };
    ImageMoveDrag imageDrag_;

    struct ImageScaleDrag {
        size_t index = 0;
        bool active = false;
        double startX = 0.0;
        double startY = 0.0;
        double startScale = 1.0;
    };
    ImageScaleDrag imageScaleDrag_;

    // Shared by the move and scale commits: restores a whole placement and repaints.
    void applyImagePlacement(size_t index, double x, double y, double scale);
    bool commitImagePlacement(size_t index, const char* label, double fromX, double fromY,
                              double fromScale, double toX, double toY, double toScale);

    struct OpacityDrag {
        size_t index = 0;
        double before = 1.0;
        double after = 1.0;
        bool active = false;
    };
    OpacityDrag opacityDrag_;

    struct StackState {
        std::vector<SurfacePtr> images;
        std::vector<std::string> names;
        std::vector<char> visible;
        std::vector<double> opacity;
        // Kept alongside the pixels so a picture stays dragable across an undo. The
        // placement is shared, not copied: the decoded source is immutable, so every
        // history entry refers to the same surface rather than holding another copy.
        std::vector<std::optional<Layer::ImagePlacement>> placements;
        std::vector<Layer::TextInfo> textInfos;
        size_t activeIndex = 0;
        // Carried so undoing a structural step restores the page the user was
        // looking at, instead of silently resetting it to the default.
        Color background{0.984, 0.984, 0.992};
        Paper paper;
    };

    StackState captureStack() const;
    void applyStack(const StackState& state);
    void pushStructural(const std::string& label, const std::function<void()>& apply);

    // The only way onto history_.push.
    //
    // Every push changes whether undo and redo are available, and the toolbar
    // buttons read those two flags. Pushing through history_ directly meant the
    // notification that re-enables the buttons fired *before* the entry existed --
    // endStroke notifies right after drawing, before pushing -- so the buttons sat
    // disabled until some later unrelated change came along. Pushing first and
    // notifying second makes the state they read already final.
    void pushHistory(std::string label, std::function<void()> undo, std::function<void()> redo);

    Channel changed_;
    Channel layersChanged_;
    Channel layerChanged_;
    NameChannel nameChanged_;
};

}  // namespace stylus