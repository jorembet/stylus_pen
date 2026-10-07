#include "document.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace stylus {

namespace {

SurfacePtr cloneSurface(cairo_surface_t* src) {
    const int w = cairo_image_surface_get_width(src);
    const int h = cairo_image_surface_get_height(src);
    SurfacePtr dst = makeSurface(w, h);

    cairo_t* cr = cairo_create(dst.get());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, src, 0.0, 0.0);
    cairo_paint(cr);
    cairo_destroy(cr);
    return dst;
}

}  // namespace

Layer::Layer(std::string name, int width, int height)
    : name_(std::move(name)), width_(width), height_(height), surface_(makeSurface(width, height)) {}

Layer::~Layer() = default;

void Layer::clear() {
    // Clearing a layer drops any placed image with it: the image *is* the layer's
    // content, so an empty layer cannot still carry a picture.
    image_ = ImagePlacement{};
    textInfo_ = TextInfo{};
    cairo_t* cr = cairo_create(surface_.get());
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface_.get());
    ++revision_;
}

Rect Layer::imageBounds() const {
    if (!image_.source) return Rect{};
    const int sw = cairo_image_surface_get_width(image_.source.get());
    const int sh = cairo_image_surface_get_height(image_.source.get());
    return Rect{image_.x, image_.y, sw * image_.scale, sh * image_.scale};
}

bool Layer::setImage(SurfacePtr source, double x, double y, double scale) {
    if (!source) return false;
    if (cairo_surface_get_type(source.get()) != CAIRO_SURFACE_TYPE_IMAGE) return false;
    if (cairo_image_surface_get_width(source.get()) <= 0 ||
        cairo_image_surface_get_height(source.get()) <= 0) {
        return false;
    }

    // The layer becomes the image. Anything already on it is replaced, which is the
    // deal an image layer makes: it is a dedicated picture layer, not a drawing
    // surface the picture happens to sit on.
    image_.source = std::move(source);
    image_.x = x;
    image_.y = y;
    image_.scale = clampd(scale, 0.02, 8.0);
    rasteriseImage();
    return true;
}

bool Layer::setImagePosition(double x, double y) {
    if (!image_.source) return false;
    if (image_.x == x && image_.y == y) return false;
    image_.x = x;
    image_.y = y;
    rasteriseImage();
    return true;
}

bool Layer::moveImageBy(double dx, double dy) {
    if (!image_.source) return false;
    if (dx == 0.0 && dy == 0.0) return false;
    image_.x += dx;
    image_.y += dy;
    rasteriseImage();
    return true;
}

bool Layer::moveContentBy(double dx, double dy) {
    if (dx == 0.0 && dy == 0.0) return false;
    if (hasImage()) {
        return moveImageBy(dx, dy);
    }
    if (!surface_ || !surface_.get()) return false;
    if (cairo_surface_status(surface_.get()) != CAIRO_STATUS_SUCCESS) return false;
    cairo_surface_flush(surface_.get());

    SurfacePtr temp = makeSurface(width_, height_);
    if (!temp || !temp.get() || cairo_surface_status(temp.get()) != CAIRO_STATUS_SUCCESS) return false;

    cairo_t* cr = cairo_create(temp.get());
    if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, surface_.get(), dx, dy);
        cairo_paint(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(temp.get());

    surface_ = std::move(temp);
    ++revision_;
    return true;
}

bool Layer::setImageScale(double scale) {
    if (!image_.source) return false;
    const double clamped = clampd(scale, 0.02, 8.0);
    if (clamped == image_.scale) return false;

    const Rect before = imageBounds();
    const double centreX = before.x + before.w * 0.5;
    const double centreY = before.y + before.h * 0.5;

    const int sw = cairo_image_surface_get_width(image_.source.get());
    const int sh = cairo_image_surface_get_height(image_.source.get());

    image_.scale = clamped;
    // Re-anchor so the centre lands back where it was. Resizing about the top-left
    // instead makes the picture creep away from the pointer on every step.
    image_.x = centreX - sw * clamped * 0.5;
    image_.y = centreY - sh * clamped * 0.5;
    rasteriseImage();
    return true;
}

void Layer::rasteriseImage() {
    if (!surface_ || !surface_.get() || cairo_surface_status(surface_.get()) != CAIRO_STATUS_SUCCESS) return;
    cairo_t* cr = cairo_create(surface_.get());
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        return;
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);

    if (image_.source && cairo_surface_status(image_.source.get()) == CAIRO_STATUS_SUCCESS) {
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_translate(cr, image_.x, image_.y);
        cairo_scale(cr, image_.scale, image_.scale);
        cairo_set_source_surface(cr, image_.source.get(), 0.0, 0.0);
        cairo_paint(cr);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface_.get());
    ++revision_;
}

bool Layer::isEmpty() const {
    cairo_surface_t* s = surface_.get();
    cairo_surface_flush(s);

    const int stride = cairo_image_surface_get_stride(s);
    const int h = cairo_image_surface_get_height(s);
    const unsigned char* data = cairo_image_surface_get_data(s);
    if (!data) return true;

    // A blank ARGB32 pixel has zero alpha, so alpha alone decides emptiness.
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(data + y * stride);
        for (int x = 0; x < cairo_image_surface_get_width(s); ++x) {
            if ((row[x] >> 24) != 0) return false;
        }
    }
    return true;
}

void Layer::paint(cairo_t* cr) const {
    if (!visible_ || !surface_ || !surface_.get()) return;
    if (cairo_surface_status(surface_.get()) != CAIRO_STATUS_SUCCESS) return;
    cairo_save(cr);

    if (opacity_ >= 1.0) {
        // Opaque: normal source-over keeps the stroke colours intact.
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_surface(cr, surface_.get(), 0.0, 0.0);
        cairo_paint(cr);
    } else {
        // A mask surface carries only the alpha channel, which would flatten
        // the layer to greyscale. Compositing into a scratch group scaled by
        // the layer opacity preserves colour and antialiasing.
        cairo_push_group(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_surface(cr, surface_.get(), 0.0, 0.0);
        cairo_paint(cr);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, opacity_);
    }

    cairo_restore(cr);
}

SurfacePtr Layer::snapshot() const {
    return cloneSurface(surface_.get());
}

void Layer::restore(const SurfacePtr& snap) {
    if (!snap) return;
    cairo_t* cr = cairo_create(surface_.get());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, snap.get(), 0.0, 0.0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface_.get());
    ++revision_;
}

Document::Document(int width, int height) : width_(width), height_(height) {
    layers_.push_back(std::make_unique<Layer>("Background", width, height));
}

void Document::setActiveIndex(size_t i) {
    if (layers_.empty()) return;
    activeIndex_ = std::min(i, layers_.size() - 1);
}

size_t Document::addLayer() {
    auto layer = std::make_unique<Layer>("Layer " + std::to_string(layers_.size() + 1),
                                         width_, height_);
    layers_.insert(layers_.begin() + static_cast<long>(activeIndex_ + 1), std::move(layer));
    activeIndex_ += 1;
    invalidateComposite();
    return activeIndex_;
}

Layer* Document::duplicateActive() {
    auto copy = std::make_unique<Layer>(active().name() + " copy", width_, height_);
    Layer* raw = copy.get();
    cairo_t* cr = cairo_create(copy->surface());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, active().surface(), 0.0, 0.0);
    cairo_paint(cr);
    cairo_destroy(cr);
    copy->setVisible(active().visible());
    copy->setOpacity(active().opacity());

    layers_.insert(layers_.begin() + static_cast<long>(activeIndex_ + 1), std::move(copy));
    activeIndex_ += 1;
    invalidateComposite();
    return raw;
}

bool Document::swapLayers(size_t a, size_t b) {
    if (a >= layers_.size() || b >= layers_.size() || a == b) return false;
    std::swap(layers_[a], layers_[b]);
    invalidateComposite();
    return true;
}

std::unique_ptr<Layer> Document::removeActive() {
    if (layers_.size() <= 1) return nullptr;
    auto removed = std::move(layers_[activeIndex_]);
    layers_.erase(layers_.begin() + static_cast<long>(activeIndex_));
    if (activeIndex_ >= layers_.size()) activeIndex_ = layers_.size() - 1;
    invalidateComposite();
    return removed;
}

bool Document::moveActive(int delta) {
    const long target = static_cast<long>(activeIndex_) + delta;
    if (target < 0 || target >= static_cast<long>(layers_.size())) return false;
    auto moved = std::move(layers_[activeIndex_]);
    layers_.erase(layers_.begin() + static_cast<long>(activeIndex_));
    layers_.insert(layers_.begin() + target, std::move(moved));
    activeIndex_ = static_cast<size_t>(target);
    invalidateComposite();
    return true;
}

bool Document::mergeDown() {
    if (activeIndex_ == 0) return false;
    Layer& top = *layers_[activeIndex_];
    Layer& bottom = *layers_[activeIndex_ - 1];

    cairo_t* cr = cairo_create(bottom.surface());
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    top.paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(bottom.surface());

    layers_.erase(layers_.begin() + static_cast<long>(activeIndex_));
    activeIndex_ -= 1;
    invalidateComposite();
    return true;
}

void Document::clearActive() {
    active().clear();
    invalidateComposite();
}

void Document::clearAll() {
    for (auto& l : layers_) l->clear();
    invalidateComposite();
}

// A cheap signature of everything that affects the composite. Layers are not
// hashed by content: pixel writes go straight to the surface and would make this
// an O(page) check, so the caller bumps Document::invalidateComposite() after
// drawing instead, and the revisions here cover the flag and opacity changes
// that have no such call site.
unsigned long long Document::compositeSignature() const {
    unsigned long long sig = 1469598103934665603ull;  // FNV offset basis
    auto mix = [&sig](unsigned long long v) {
        sig ^= v;
        sig *= 1099511628211ull;  // FNV prime
    };
    mix(layers_.size());
    for (const auto& l : layers_) mix(l->revision());
    return sig;
}

SurfacePtr Document::composite() const {
    const unsigned long long sig = compositeSignature();
    if (compositeCache_ && compositeCacheSignature_ == sig) return compositeCache_;

    SurfacePtr out = makeSurface(width_, height_);
    cairo_t* cr = cairo_create(out.get());
    // Explicitly SOURCE: the surface starts zeroed, and relying on the default
    // OVER against a transparent surface leaves premultiplied garbage in the
    // untouched areas for some formats.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    for (const auto& l : layers_) l->paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(out.get());

    compositeCache_ = out;
    compositeCacheSignature_ = sig;
    return compositeCache_;
}

void Document::invalidateComposite() {
    compositeCache_.reset();
    compositeCacheSignature_ = 0;
}

bool Document::setBackground(const std::string& hex) {
    Color parsed;
    if (!parseHexColor(hex, &parsed)) return false;
    background_ = parsed;
    return true;
}

SurfacePtr Document::flatten() const {
    SurfacePtr out = makeSurface(width_, height_);
    cairo_t* cr = cairo_create(out.get());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    // The page colour goes in first, so exported PNG matches what the canvas
    // shows. It used to be hardcoded white here while the canvas painted an
    // off-white, so a light background exported as pure white.
    cairo_set_source_rgb(cr, background_.r, background_.g, background_.b);
    cairo_paint(cr);
    // Back to OVER before the ruling. The ruling is translucent ink, and SOURCE
    // replaces the destination outright instead of blending: every rule punched a
    // partly transparent stripe through the page, so an exported PNG came out with
    // holes in the paper rather than lines drawn on it.
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    // The ruling goes in before the layers, so it stays behind the artwork. Shared
    // with Canvas so the exported PNG and the screen show the same pitch.
    paintPaper(cr, paper_, background_, width_, height_);
    for (const auto& l : layers_) l->paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(out.get());
    return out;
}

void Document::replaceLayers(std::vector<std::unique_ptr<Layer>> layers, size_t activeIndex) {
    if (layers.empty()) layers.push_back(std::make_unique<Layer>("Background", width_, height_));
    layers_ = std::move(layers);
    setActiveIndex(activeIndex);
    invalidateComposite();
}

}  // namespace stylus