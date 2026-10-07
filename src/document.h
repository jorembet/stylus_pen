#pragma once

#include <cairo/cairo.h>

#include <memory>
#include <string>
#include <vector>

#include "brush.h"
#include "color.h"
#include "geometry.h"
#include "paper.h"

namespace stylus {

struct SurfaceDeleter {
    void operator()(cairo_surface_t* s) const {
        if (s) cairo_surface_destroy(s);
    }
};

// Copyable and default-constructible: undo snapshots need to be copied into
// history lambdas, and "no snapshot" must be representable as a null pointer.
using SurfacePtr = std::shared_ptr<cairo_surface_t>;

inline SurfacePtr makeSurface(int w, int h) {
    return SurfacePtr(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h), SurfaceDeleter{});
}

class Layer {
public:
    // Where a placed image sits on the page.
    //
    // The image is rasterised into the layer surface rather than drawn live, and
    // this is only kept so a drag can put it somewhere else. Baking is what lets
    // snapshot(), stroke undo and the save format stay untouched: all three read the
    // surface, and a live image would be invisible to all of them.
    struct ImagePlacement {
        SurfacePtr source;  // the decoded original, at its natural size
        double x = 0.0;     // document-space top-left
        double y = 0.0;
        double scale = 1.0;
    };

    Layer(std::string name, int width, int height);
    ~Layer();
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    const std::string& name() const { return name_; }
    void setName(std::string n) { name_ = std::move(n); }

    bool visible() const { return visible_; }
    void setVisible(bool v) {
        if (visible_ == v) return;
        visible_ = v;
        ++revision_;
    }

    double opacity() const { return opacity_; }
    void setOpacity(double o) {
        const double clamped = clampd(o, 0.0, 1.0);
        if (opacity_ == clamped) return;
        opacity_ = clamped;
        ++revision_;
    }

    // Bumped whenever anything that affects how this layer composites changes.
    // The document's cached composite compares these to decide whether it is
    // still valid, so a visibility or opacity flip does not need to reach the
    // document explicitly to be picked up.
    unsigned long long revision() const { return revision_; }

    cairo_surface_t* surface() const { return surface_.get(); }
    int width() const { return width_; }
    int height() const { return height_; }

    void clear();
    bool isEmpty() const;

    // Draws this layer's content scaled into the current Cairo context,
    // honouring opacity. The context is saved/restored.
    void paint(cairo_t* cr) const;

    SurfacePtr snapshot() const;
    void restore(const SurfacePtr& snapshot);

    // ---- placed images ----

    bool hasImage() const { return image_.source != nullptr; }
    const ImagePlacement& image() const { return image_; }
    // The image's rectangle on the page, for hit-testing a drag.
    Rect imageBounds() const;
    // Replaces this layer's contents with `source` drawn at (x, y).
    bool setImage(SurfacePtr source, double x, double y, double scale);
    // Places the image absolutely, keeping the scale. Returns false when the layer
    // holds no image, so a stray drag cannot move nothing and still claim success.
    bool setImagePosition(double x, double y);
    bool moveImageBy(double dx, double dy);
    bool moveContentBy(double dx, double dy);
    // Rescales about the centre of the current bounds, so the picture grows and
    // shrinks around the spot it already occupies instead of sliding away from it as
    // the top-left stays pinned.
    bool setImageScale(double scale);
    // Records a placement without drawing anything, for restoring a snapshot.
    //
    // applyStack rebuilds each layer from a pixel snapshot and that snapshot already
    // shows the picture where it belonged. Re-rasterising there would wipe the very
    // pixels being restored, so the placement is adopted as metadata only -- without
    // this, undoing and redoing a structural change left a visible picture that could
    // no longer be dragged.
    void adoptImage(const ImagePlacement& placement) {
        image_ = placement;
        ++revision_;
    }

    struct TextInfo {
        std::string text;
        double fontSize = 36.0;
        Color color{0.1, 0.1, 0.12};
        bool isBold = true;
        bool isItalic = false;
        std::string fontFamily = "Sans";
        bool valid = false;
    };

    bool hasText() const { return textInfo_.valid && !textInfo_.text.empty(); }
    const TextInfo& textInfo() const { return textInfo_; }
    void setTextInfo(const TextInfo& info) { textInfo_ = info; textInfo_.valid = true; ++revision_; }
    void clearTextInfo() { textInfo_ = TextInfo{}; ++revision_; }

private:
    // Draws the placed image into the surface, replacing whatever was there.
    void rasteriseImage();

    std::string name_;
    bool visible_ = true;
    double opacity_ = 1.0;
    unsigned long long revision_ = 1;
    int width_;
    int height_;
    SurfacePtr surface_;
    ImagePlacement image_;
    TextInfo textInfo_;
};

class Document {
public:
    Document(int width = 1600, int height = 1100);

    int width() const { return width_; }
    int height() const { return height_; }

    // The page the layers sit on. It belongs to the document rather than to a
    // layer because it is what shows through the transparent artwork everywhere:
    // the canvas paints it, and flatten() bakes it into the exported PNG. A layer
    // cannot stand in for it, because hiding the bottom layer would take the page
    // with it.
    const Color& background() const { return background_; }
    // Ignores a malformed hex and leaves the current page colour alone, matching
    // how Brush::setHex treats a broken colour read from a project file.
    bool setBackground(const std::string& hex);
    void setBackground(const Color& color) { background_ = color; }
    std::string backgroundHex() const { return formatHexColor(background_); }

    // The ruling printed on the page, behind the artwork.
    const Paper& paper() const { return paper_; }
    void setPaper(Paper paper) {
        // Clamp the parameter, not a field that is about to be overwritten: doing
        // it in the wrong order leaves the raw spacing in place.
        paper.spacing = clampPaperSpacing(paper.spacing);
        paper_ = paper;
    }

    size_t layerCount() const { return layers_.size(); }
    Layer& layerAt(size_t index) { return *layers_[index]; }
    const Layer& layerAt(size_t index) const { return *layers_[index]; }

    size_t activeIndex() const { return activeIndex_; }
    void setActiveIndex(size_t i);
    Layer& active() { return *layers_[activeIndex_]; }
    const Layer& active() const { return *layers_[activeIndex_]; }

    size_t addLayer();
    Layer* duplicateActive();
    // Exchanges the layers at two positions. Indices out of range are ignored.
    // Used by reorder history, where undo and redo are the same swap.
    bool swapLayers(size_t a, size_t b);
    // Returns the removed layer, or nullptr when it was the last one.
    std::unique_ptr<Layer> removeActive();
    bool moveActive(int delta);
    // Composites the active layer into the one below it.
    bool mergeDown();

    void clearActive();
    void clearAll();

    // Composites every visible layer into a fresh surface at document size.
    SurfacePtr flatten() const;

    // Same as flatten(), but without the opaque white page behind the artwork.
    // The canvas needs this: it paints the page itself and clips the layer stack
    // to the page rectangle, and caching the composite here means a stroke only
    // recomposites when something actually changed rather than on every frame.
    SurfacePtr composite() const;

    // Drops the cached composite. Called whenever a layer's pixels or the stack
    // itself changes.
    void invalidateComposite();

    void replaceLayers(std::vector<std::unique_ptr<Layer>> layers, size_t activeIndex);

private:
    int width_;
    int height_;
    // #fbfbfd: the off-white the canvas has always painted. Stored as a Color so
    // the paint path never has to re-parse, and kept canonical by backgroundHex()
    // so a project round-trips byte-identically.
    Color background_{0.984, 0.984, 0.992};
    Paper paper_;
    std::vector<std::unique_ptr<Layer>> layers_;
    size_t activeIndex_ = 0;

    // Composite of the visible layers, rebuilt on demand. Painting every layer
    // on every frame is what made a long stroke stutter; the cached surface is
    // only valid until something invalidates it.
    mutable SurfacePtr compositeCache_;
    mutable unsigned long long compositeCacheSignature_ = 0;

    unsigned long long compositeSignature() const;
};

}  // namespace stylus