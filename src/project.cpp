#include "project.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <sstream>

#include "color.h"

namespace stylus {

namespace {

// Copies `region` out of `src` into a fresh surface, clipped to the source bounds.
// `out` receives the integer-aligned rectangle that was actually copied, so the
// caller can blit the result back to the same place.
SurfacePtr regionSurface(cairo_surface_t* src, const Rect& region, Rect* out) {
    if (out) *out = Rect{};

    const int sw = cairo_image_surface_get_width(src);
    const int sh = cairo_image_surface_get_height(src);

    Rect clipped = rectIntersect(region, Rect{0.0, 0.0, static_cast<double>(sw), static_cast<double>(sh)});
    if (rectIsEmpty(clipped)) return SurfacePtr{};

    const int x = static_cast<int>(std::floor(clipped.x));
    const int y = static_cast<int>(std::floor(clipped.y));
    const int w = static_cast<int>(std::ceil(clipped.right())) - x;
    const int h = static_cast<int>(std::ceil(clipped.bottom())) - y;
    if (w <= 0 || h <= 0) return SurfacePtr{};

    SurfacePtr result = makeSurface(w, h);
    if (out) *out = Rect{static_cast<double>(x), static_cast<double>(y), static_cast<double>(w),
                          static_cast<double>(h)};

    const int srcStride = cairo_image_surface_get_stride(src);
    const unsigned char* srcData = cairo_image_surface_get_data(src);
    const int dstStride = cairo_image_surface_get_stride(result.get());
    unsigned char* dstData = cairo_image_surface_get_data(result.get());
    if (!srcData || !dstData) return SurfacePtr{};

    for (int row = 0; row < h; ++row) {
        const unsigned char* s = srcData + (y + row) * srcStride + x * 4;
        std::memcpy(dstData + row * dstStride, s, static_cast<size_t>(w) * 4);
    }
    cairo_surface_mark_dirty(result.get());
    return result;
}

cairo_status_t pngWriteCallback(void* closure, const unsigned char* data, unsigned int length) {
    auto* target = static_cast<std::string*>(closure);
    target->append(reinterpret_cast<const char*>(data), length);
    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t pngWrite(cairo_surface_t* surface, std::string* out) {
    return cairo_surface_write_to_png_stream(surface, &pngWriteCallback, out);
}

struct PngReadState {
    const std::string* bytes;
    size_t offset = 0;
    bool overflow = false;
};

cairo_status_t pngRead(void* closure, unsigned char* data, unsigned int length) {
    auto* state = static_cast<PngReadState*>(closure);
    if (state->offset + length > state->bytes->size()) {
        state->overflow = true;
        return CAIRO_STATUS_READ_ERROR;
    }
    std::memcpy(data, state->bytes->data() + state->offset, length);
    state->offset += length;
    return CAIRO_STATUS_SUCCESS;
}

}  // namespace

SurfacePtr copyRegion(cairo_surface_t* src, const Rect& region, Rect* clipped) {
    if (clipped) *clipped = Rect{};
    if (!src) return SurfacePtr{};

    Rect effective;
    if (clipped) {
        effective = *clipped;
    }

    cairo_surface_flush(src);
    SurfacePtr out = regionSurface(src, region, &effective);
    if (clipped) *clipped = effective;
    return out;
}

void blitRegion(const SurfacePtr& src, const Rect& region, cairo_surface_t* dst) {
    if (!src || !dst || rectIsEmpty(region)) return;

    // The snapshot only covers `region`, but cairo_paint composites over the whole
    // clip extents. Without a clip, OPERATOR_SOURCE replaces every pixel outside
    // the region with the transparent parts of the snapshot, so restoring one
    // stroke would erase the rest of the layer.
    cairo_t* cr = cairo_create(dst);
    cairo_save(cr);
    cairo_rectangle(cr, region.x, region.y, region.w, region.h);
    cairo_clip(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, src.get(), region.x, region.y);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_destroy(cr);
    cairo_surface_flush(dst);
}

std::string encodePng(cairo_surface_t* surface) {
    std::string out;
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) return out;
    if (pngWrite(surface, &out) != CAIRO_STATUS_SUCCESS) out.clear();
    return out;
}

std::optional<SurfacePtr> decodePng(const std::string& bytes, int width, int height) {
    PngReadState state;
    state.bytes = &bytes;
    cairo_surface_t* raw = cairo_image_surface_create_from_png_stream(pngRead, &state);
    if (state.overflow || !raw || cairo_surface_status(raw) != CAIRO_STATUS_SUCCESS) {
        if (raw) cairo_surface_destroy(raw);
        return std::nullopt;
    }
    SurfacePtr owned(raw, &cairo_surface_destroy);

    const bool sizeMatches = width > 0 && height > 0 &&
                             cairo_image_surface_get_width(raw) == width &&
                             cairo_image_surface_get_height(raw) == height;
    if (sizeMatches || width <= 0 || height <= 0) {
        return owned;
    }

    SurfacePtr out = makeSurface(width, height);
    cairo_t* cr = cairo_create(out.get());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, raw, 0.0, 0.0);
    cairo_paint(cr);
    cairo_destroy(cr);
    return out;
}

std::optional<Project> serializeProject(const Document& doc, const ProjectSettings& settings) {
    Project p;
    p.width = doc.width();
    p.height = doc.height();
    p.activeIndex = doc.activeIndex();
    p.settings = settings;
    p.background = doc.backgroundHex();
    p.paperKind = doc.paper().kind;
    p.paperSpacing = doc.paper().spacing;

    for (size_t i = 0; i < doc.layerCount(); ++i) {
        const Layer& l = doc.layerAt(i);
        std::string png = encodePng(l.surface());
        if (png.empty()) return std::nullopt;
        p.layerPngs.push_back(std::move(png));
        p.layerNames.push_back(l.name());
        p.layerVisible.push_back(l.visible() ? 1 : 0);
        p.layerOpacity.push_back(l.opacity());
    }
    return p;
}

std::string encodeProjectBlob(const Project& project) {
    std::string blob;
    char header[128];
    // Version 3 adds the page background. The metadata block is a tagged stream,
    // so older readers would stop at the new "background" tag and then fail to
    // parse the payloads behind it; bumping the magic keeps that failure explicit
    // instead of a confusing decode error halfway through the layers.
    std::snprintf(header, sizeof(header), "STYLUSPEN3 %d %d %zu\n", project.width, project.height,
                  project.layerPngs.size());
    blob += header;

    // Everything below used to be dropped on save: only the header and the PNG
    // payloads were written, so reopening a project lost every layer name, the
    // visibility and opacity of each layer, which layer was active, and all of the
    // brush settings. Reading it back filled in blanks and 100% defaults.
    char meta[256];
    std::snprintf(meta, sizeof(meta), "active %zu\n", project.activeIndex);
    blob += meta;

    const ProjectSettings& st = project.settings;
    // The sensitivity level rides as a trailing field rather than in its own tag.
    // It is a brush setting like the rest, and the reader takes it optionally: a
    // file without it keeps the default of full response.
    std::snprintf(meta, sizeof(meta), "settings %d %s %.4f %d %d %d %.4f\n",
                  static_cast<int>(st.tool), st.color.c_str(), st.size,
                  st.tiltShading ? 1 : 0, st.pressureWidth ? 1 : 0, st.stabilize ? 1 : 0,
                  st.pressureSensitivity);
    blob += meta;

    // Only version 3 writes this; older files simply have no tag and keep the
    // default page colour.
    std::snprintf(meta, sizeof(meta), "background %s\n", project.background.c_str());
    blob += meta;

    // The ruling is page furniture rather than a brush setting, so it gets its own
    // tag. Kind travels as an integer for the same reason the tool does: there is
    // nothing to escape, and an out-of-range value is a one-line check on read.
    std::snprintf(meta, sizeof(meta), "paper %d %.4f\n", static_cast<int>(project.paperKind),
                  project.paperSpacing);
    blob += meta;

    // One "layer" line per layer, in stack order, before the payloads.
    for (size_t i = 0; i < project.layerPngs.size(); ++i) {
        const std::string name =
            i < project.layerNames.size() ? project.layerNames[i] : std::string("Layer");
        const bool visible = i < project.layerVisible.size() ? project.layerVisible[i] != 0 : true;
        const double opacity = i < project.layerOpacity.size() ? project.layerOpacity[i] : 1.0;
        // Names come from the user and may hold spaces or newlines, so the length
        // is written explicitly rather than trying to escape them.
        std::snprintf(meta, sizeof(meta), "layer %zu ", name.size());
        blob += meta;
        blob += name;
        std::snprintf(meta, sizeof(meta), " %d %.4f\n", visible ? 1 : 0, opacity);
        blob += meta;
    }

    // Each payload is followed by a newline. Without it the next length line is
    // glued straight onto the payload's last byte, and reading the second layer's
    // length picks up that byte too -- so a two-layer project could not be reopened
    // at all, while a one-layer project happened to survive.
    for (const std::string& png : project.layerPngs) {
        char len[32];
        std::snprintf(len, sizeof(len), "%zu\n", png.size());
        blob += len;
        blob += png;
        blob += '\n';
    }
    return blob;
}

namespace {

// Reads one whitespace-delimited token from `pos`, advancing past it.
// Returns false when the input has run out or the token is empty.
bool nextToken(const std::string& blob, size_t* pos, std::string* out) {
    while (*pos < blob.size() && std::isspace(static_cast<unsigned char>(blob[*pos]))) ++(*pos);
    const size_t start = *pos;
    while (*pos < blob.size() && !std::isspace(static_cast<unsigned char>(blob[*pos]))) ++(*pos);
    if (*pos == start) return false;
    *out = blob.substr(start, *pos - start);
    return true;
}

bool toSize(const std::string& text, size_t* out) {
    try {
        *out = static_cast<size_t>(std::stoull(text));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool toDouble(const std::string& text, double* out) {
    try {
        *out = std::stod(text);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

bool decodeProjectBlob(const std::string& blob, Project* out) {
    if (!out) return false;

    size_t pos = 0;
    std::string magic;
    if (!nextToken(blob, &pos, &magic)) return false;

    std::string widthText;
    std::string heightText;
    std::string countText;
    if (!nextToken(blob, &pos, &widthText) || !nextToken(blob, &pos, &heightText) ||
        !nextToken(blob, &pos, &countText)) {
        return false;
    }

    size_t width = 0;
    size_t height = 0;
    size_t count = 0;
    if (!toSize(widthText, &width) || !toSize(heightText, &height) || !toSize(countText, &count)) {
        return false;
    }
    if (width == 0 || height == 0 || count == 0) return false;
    if (width > 100000 || height > 100000 || count > 100000) return false;

    Project project;
    project.width = static_cast<int>(width);
    project.height = static_cast<int>(height);
    project.layerPngs.reserve(count);
    project.layerNames.assign(count, "Layer");
    project.layerVisible.assign(count, 1);
    project.layerOpacity.assign(count, 1.0);

    const bool version3 = (magic == "STYLUSPEN3");
    const bool hasMetadata = version3 || (magic == "STYLUSPEN2");
    if (!hasMetadata && magic != "STYLUSPEN") return false;

    // Version 2 carries the document's settings and each layer's name, visibility
    // and opacity between the header and the payloads. Version 3 adds the page
    // background. A version 1 file jumps straight to the payloads and keeps the
    // defaults filled in above.
    size_t nextLayerSlot = 0;
    if (hasMetadata) {
        // Skip the remainder of the header line.
        while (pos < blob.size() && blob[pos] != '\n') ++pos;
        if (pos < blob.size()) ++pos;

        std::string tag;
        while (pos < blob.size()) {
            const size_t lineStart = pos;
            if (!nextToken(blob, &pos, &tag)) break;

            if (tag == "active") {
                std::string value;
                size_t index = 0;
                if (nextToken(blob, &pos, &value) && toSize(value, &index)) {
                    project.activeIndex = std::min(index, count - 1);
                }
                continue;
            }

            if (tag == "settings") {
                // Where this line ends, so the optional trailing field can be told
                // apart from the next tag. nextToken skips newlines as well as
                // spaces, so asking for one more token unconditionally would read
                // the following tag's name into the sensitivity slot on a file
                // written before the field existed.
                const size_t settingsLineEnd = blob.find('\n', pos);

                std::string toolText;
                std::string colour;
                std::string sizeText;
                std::string tiltText;
                std::string widthText2;
                std::string stabiliseText;
                if (nextToken(blob, &pos, &toolText) && nextToken(blob, &pos, &colour) &&
                    nextToken(blob, &pos, &sizeText) && nextToken(blob, &pos, &tiltText) &&
                    nextToken(blob, &pos, &widthText2) && nextToken(blob, &pos, &stabiliseText)) {
                    const long tool = std::strtol(toolText.c_str(), nullptr, 10);
                    const int toolCount = static_cast<int>(BrushKind::Eraser) + 1;
                    if (tool >= 0 && tool < toolCount) {
                        project.settings.tool = static_cast<BrushKind>(tool);
                    }
                    project.settings.color = colour;
                    double sizeValue = 0.0;
                    if (toDouble(sizeText, &sizeValue)) {
                        project.settings.size = clampd(sizeValue, 0.5, 40.0);
                    }
                    project.settings.tiltShading = tiltText != "0";
                    project.settings.pressureWidth = widthText2 != "0";
                    project.settings.stabilize = stabiliseText != "0";

                    // Optional trailing field. Only read while there is still input
                    // left on this line, so an older settings line leaves the level
                    // at its full-response default instead of swallowing the next tag.
                    std::string sensitivityText;
                    if (pos < settingsLineEnd && nextToken(blob, &pos, &sensitivityText)) {
                        double sensitivity = 1.0;
                        if (toDouble(sensitivityText, &sensitivity)) {
                            project.settings.pressureSensitivity = clampd(sensitivity, 0.0, 1.0);
                        }
                    }
                }
                continue;
            }

            if (tag == "background") {
                std::string colour;
                // Validated here rather than at paint time: a hand-edited colour
                // should leave the default page, not turn the canvas black.
                if (nextToken(blob, &pos, &colour)) {
                    Color parsed;
                    if (parseHexColor(colour, &parsed)) {
                        project.background = formatHexColor(parsed);
                    }
                }
                continue;
            }

            if (tag == "paper") {
                std::string kindText;
                std::string gapText;
                if (nextToken(blob, &pos, &kindText) && nextToken(blob, &pos, &gapText)) {
                    long kind = 0;
                    char* end = nullptr;
                    kind = std::strtol(kindText.c_str(), &end, 10);
                    PaperKind parsedKind = PaperKind::Plain;
                    // An unparsable or unknown kind falls back to Plain: a corrupt
                    // file should lose the ruling, not select a pattern that does
                    // not exist.
                    if (end && *end == '\0') parsePaperKind(static_cast<int>(kind), &parsedKind);
                    project.paperKind = parsedKind;
                    double gap = 0.0;
                    if (toDouble(gapText, &gap)) {
                        project.paperSpacing = clampPaperSpacing(gap);
                    }
                }
                continue;
            }

            if (tag == "layer") {
                std::string nameLenText;
                if (!nextToken(blob, &pos, &nameLenText)) break;
                size_t nameLen = 0;
                if (!toSize(nameLenText, &nameLen)) break;

                if (nextLayerSlot >= count) return false;

                // Skip the single separating space, then take exactly nameLen
                // bytes: the name itself may contain spaces.
                if (pos < blob.size() && blob[pos] == ' ') ++pos;
                if (pos + nameLen > blob.size()) return false;

                const size_t at = nextLayerSlot++;
                project.layerNames[at] = blob.substr(pos, nameLen);
                pos += nameLen;

                // Trailing " <visible> <opacity>".
                std::string visibleText;
                std::string opacityText;
                if (nextToken(blob, &pos, &visibleText) && nextToken(blob, &pos, &opacityText)) {
                    project.layerVisible[at] = visibleText != "0" ? 1 : 0;
                    double opacity = 1.0;
                    if (toDouble(opacityText, &opacity)) {
                        project.layerOpacity[at] = clampd(opacity, 0.0, 1.0);
                    }
                }
                continue;
            }

            // Not a metadata tag: this is the first payload length, so rewind.
            pos = lineStart;
            break;
        }
    }

    std::string lengthText;
    for (size_t i = 0; i < count; ++i) {
        size_t len = 0;
        if (!nextToken(blob, &pos, &lengthText) || !toSize(lengthText, &len)) return false;

        // nextToken stops on the digit, so it leaves the newline that ends the
        // length line in front of the payload. Skipping it matters: without it the
        // payload is read one byte late, which prepends the newline and drops the
        // PNG's final byte.
        if (pos < blob.size() && blob[pos] == '\n') ++pos;

        if (pos + len > blob.size()) return false;
        project.layerPngs.push_back(blob.substr(pos, len));
        pos += len;

        // And the newline the writer puts after each payload, so the next length
        // is not read as an extension of this one's last byte.
        if (pos < blob.size() && blob[pos] == '\n') ++pos;
    }

    if (project.layerPngs.size() != count) return false;

    *out = std::move(project);
    return true;
}

std::optional<Document> deserializeProject(const Project& project) {
    if (project.width <= 0 || project.height <= 0) return std::nullopt;
    if (project.layerPngs.empty()) return std::nullopt;
    if (project.layerPngs.size() != project.layerNames.size()) return std::nullopt;

    std::vector<std::unique_ptr<Layer>> layers;
    layers.reserve(project.layerPngs.size());

    for (size_t i = 0; i < project.layerPngs.size(); ++i) {
        auto decoded = decodePng(project.layerPngs[i], project.width, project.height);
        if (!decoded) return std::nullopt;

        auto layer = std::make_unique<Layer>(project.layerNames[i], project.width, project.height);
        cairo_t* cr = cairo_create(layer->surface());
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, decoded->get(), 0.0, 0.0);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_flush(layer->surface());

        if (i < project.layerVisible.size()) layer->setVisible(project.layerVisible[i] != 0);
        if (i < project.layerOpacity.size()) layer->setOpacity(project.layerOpacity[i]);
        layers.push_back(std::move(layer));
    }

    Document doc(project.width, project.height);
    // Set after replaceLayers so a malformed colour cannot leave a half-built
    // document behind; setBackground reports whether it took.
    doc.setBackground(project.background);
    // Defaults for both, so a file that predates either tag still opens on plain
    // unspaced paper rather than on whatever the struct happened to hold.
    doc.setPaper(Paper{project.paperKind, project.paperSpacing});
    doc.replaceLayers(std::move(layers), project.activeIndex);
    return doc;
}

bool writeFile(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

AppState::AppState(int width, int height) : doc_(width, height) {
    brush_.setHex(settings_.color);
    brush_.kind = settings_.tool;
}

size_t AppState::addListener(Channel& channel, ChangedCallback cb) {
    channel.listeners.push_back(std::move(cb));
    return channel.listeners.size() - 1;
}

size_t AppState::setChanged(ChangedCallback cb) {
    return addListener(changed_, std::move(cb));
}

size_t AppState::setLayersChanged(LayersChangedCallback cb) {
    return addListener(layersChanged_, std::move(cb));
}

size_t AppState::setLayerPixelChanged(LayersChangedCallback cb) {
    return addListener(layerChanged_, std::move(cb));
}

size_t AppState::setNameChanged(NameChangedCallback cb) {
    nameChanged_.listeners.push_back(std::move(cb));
    return nameChanged_.listeners.size() - 1;
}

void AppState::clearListener(size_t token) {
    // The token alone does not say which channel, so all three are swept. An
    // already-cleared slot stays put, which keeps every other token valid.
    auto drop = [token](Channel& channel) {
        if (token < channel.listeners.size()) channel.listeners[token] = nullptr;
    };
    drop(changed_);
    drop(layersChanged_);
    drop(layerChanged_);
    if (token < nameChanged_.listeners.size()) nameChanged_.listeners[token] = nullptr;
}

void AppState::beginStroke(const Point& p) {
    // A pen contact reports the tip and its barrel button landing in the same
    // frame, which GDK hands over as two press events. Restarting on the second
    // one would discard the sample the user actually placed the nib at, so a
    // stroke already in progress wins.
    if (drawing_) return;

    live_.clear();
    // The layer is tracked by index, never by pointer: undoing a structural edit
    // replaces every Layer object, so a pointer captured here would dangle.
    liveLayerIndex_ = doc_.activeIndex();
    drawing_ = true;
    pushPoint(live_, p, 0.0, true);
    notify();
}

bool AppState::extendStroke(const Point& p) {
    if (!drawing_) return false;
    if (!pushPoint(live_, p)) return false;
    notify();
    return true;
}

void AppState::endStroke() {
    if (!drawing_) return;
    drawing_ = false;

    const size_t layerIndex = liveLayerIndex_;
    liveLayerIndex_ = 0;

    if (layerIndex >= doc_.layerCount() || live_.points.empty()) {
        live_.clear();
        notify();
        return;
    }

    if (settings_.stabilize && polylineLength(live_.points) > 6.0) {
        live_.points = resample(simplify(live_.points, 0.5), 1.1);
    }

    const Brush usedBrush = brush_;
    Layer& layer = doc_.layerAt(layerIndex);
    const Stroke stroke = live_;
    live_.clear();

    // Clip the snapshot to the layer so a stroke running off the page restores at
    // the offset it was captured from.
    Rect region;
    SurfacePtr before = copyRegion(layer.surface(), strokeBounds(stroke.points, usedBrush), &region);

    if (!before) {
        // Nothing to snapshot means the stroke is entirely off-page. Drawing it
        // would still be correct, but there is no region to undo within.
        drawStrokeOn(layer.surface(), stroke, usedBrush);
        cairo_surface_flush(layer.surface());
        doc_.invalidateComposite();
        notify();
        return;
    }

    drawStrokeOn(layer.surface(), stroke, usedBrush);
    cairo_surface_flush(layer.surface());
    doc_.invalidateComposite();
    notify();
    notifyLayerPixels();

    // The undo entry resolves the layer through the document on every call, so a
    // structural undo in between cannot leave it holding a freed surface.
    pushHistory(
        "Stroke",
        [this, layerIndex, region, before]() {
            if (layerIndex >= doc_.layerCount()) return;
            blitRegion(before, region, doc_.layerAt(layerIndex).surface());
            doc_.invalidateComposite();
            notify();
            notifyLayers();
            notifyLayerPixels();
        },
        [this, layerIndex, region, before, stroke, usedBrush]() {
            if (layerIndex >= doc_.layerCount()) return;
            // Re-apply the captured region, then redraw the stroke on top.
            blitRegion(before, region, doc_.layerAt(layerIndex).surface());
            drawStrokeOn(doc_.layerAt(layerIndex).surface(), stroke, usedBrush);
            cairo_surface_flush(doc_.layerAt(layerIndex).surface());
            doc_.invalidateComposite();
            notify();
            notifyLayers();
            notifyLayerPixels();
        });
}

void AppState::cancelStroke() {
    if (!drawing_) return;
    drawing_ = false;
    live_.clear();
    liveLayerIndex_ = 0;
    notify();
}

void AppState::undo() {
    cancelActiveImageMove();
    if (auto label = history_.undo()) {
        lastError_ = std::nullopt;
        notify();
        notifyLayers();
        notifyLayerPixels();
    }
}

void AppState::redo() {
    cancelActiveImageMove();
    if (auto label = history_.redo()) {
        lastError_ = std::nullopt;
        notify();
        notifyLayers();
        notifyLayerPixels();
    }
}

AppState::StackState AppState::captureStack() const {
    StackState state;
    state.activeIndex = doc_.activeIndex();
    state.background = doc_.background();
    state.paper = doc_.paper();
    state.images.reserve(doc_.layerCount());
    for (size_t i = 0; i < doc_.layerCount(); ++i) {
        const Layer& l = doc_.layerAt(i);
        state.images.push_back(l.snapshot());
        state.names.push_back(l.name());
        state.visible.push_back(l.visible() ? 1 : 0);
        state.opacity.push_back(l.opacity());
        state.placements.push_back(l.hasImage() ? std::optional<Layer::ImagePlacement>(l.image())
                                                : std::nullopt);
        state.textInfos.push_back(l.textInfo());
    }
    return state;
}

void AppState::applyStack(const StackState& state) {
    std::vector<std::unique_ptr<Layer>> layers;
    layers.reserve(state.images.size());
    for (size_t i = 0; i < state.images.size(); ++i) {
        auto layer = std::make_unique<Layer>(state.names[i], doc_.width(), doc_.height());
        if (state.images[i]) {
            cairo_t* cr = cairo_create(layer->surface());
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_surface(cr, state.images[i].get(), 0.0, 0.0);
            cairo_paint(cr);
            cairo_destroy(cr);
        }
        if (i < state.visible.size()) layer->setVisible(state.visible[i] != 0);
        if (i < state.opacity.size()) layer->setOpacity(state.opacity[i]);
        // Adopted, not re-rasterised: the snapshot painted above already has the
        // picture at this position.
        if (i < state.placements.size() && state.placements[i] &&
            state.placements[i]->source) {
            layer->adoptImage(*state.placements[i]);
        }
        if (i < state.textInfos.size()) layer->setTextInfo(state.textInfos[i]);
        layers.push_back(std::move(layer));
    }
    doc_.replaceLayers(std::move(layers), state.activeIndex);
    doc_.setBackground(state.background);
    doc_.setPaper(state.paper);
    doc_.invalidateComposite();
    notify();
    notifyLayers();
}

void AppState::pushStructural(const std::string& label, const std::function<void()>& apply) {
    StackState before = captureStack();
    apply();
    notify();
    notifyLayers();
    StackState after = captureStack();
    pushHistory(label, [this, before]() { applyStack(before); },
                  [this, after]() { applyStack(after); });
    notifyLayerPixels();
}

void AppState::pushHistory(std::string label, std::function<void()> undo,
                          std::function<void()> redo) {
    history_.push(std::move(label), std::move(undo), std::move(redo));
    // After, never before: see the note on the declaration.
    notify();
}

void AppState::addLayer() {
    cancelActiveImageMove();
    pushStructural("Add layer", [this]() { doc_.addLayer(); });
}

void AppState::duplicateLayer() {
    cancelActiveImageMove();
    if (doc_.layerCount() < 1) return;
    pushStructural("Duplicate layer", [this]() { doc_.duplicateActive(); });
}

void AppState::deleteLayer() {
    cancelActiveImageMove();
    if (doc_.layerCount() <= 1) {
        lastError_ = "Need at least one layer";
        return;
    }
    pushStructural("Delete layer", [this]() { doc_.removeActive(); });
}

void AppState::moveLayer(int delta) {
    cancelActiveImageMove();
    const size_t count = doc_.layerCount();
    const size_t before = doc_.activeIndex();
    if (!doc_.moveActive(delta)) return;

    doc_.invalidateComposite();
    notify();
    notifyLayers();
    notifyLayerPixels();

    // Record the order of the whole stack, not just the active index. Undo used to
    // restore only the index and leave the layers where the move had put them, so
    // "reorder" was not undoable at all -- the panel came back showing a different
    // stack from the one the history claimed to restore, and any later index the
    // user touched referred to a different layer.
    std::vector<std::string> order;
    order.reserve(count);
    for (size_t i = 0; i < count; ++i) order.push_back(doc_.layerAt(i).name());

    const size_t after = doc_.activeIndex();

    // Swapping two positions is its own inverse, so the same move undoes and redoes
    // the reorder. That also keeps the captured layer objects out of the history,
    // which matters because a structural undo replaces every Layer object.
    const size_t swapped = (delta > 0) ? after : before;
    const size_t moved = (delta > 0) ? before : after;

    pushHistory(
        "Reorder layer",
        [this, before, order, swapped, moved]() {
            // The guard catches a structural edit having changed the stack size in
            // between; swapping then would move the wrong layer.
            if (doc_.layerCount() != order.size()) return;
            doc_.swapLayers(moved, swapped);
            doc_.setActiveIndex(before);
            notify();
            notifyLayers();
            notifyLayerPixels();
        },
        [this, after, order, swapped, moved]() {
            if (doc_.layerCount() != order.size()) return;
            doc_.swapLayers(moved, swapped);
            doc_.setActiveIndex(after);
            notify();
            notifyLayers();
            notifyLayerPixels();
        });
}

void AppState::mergeDown() {
    if (doc_.activeIndex() == 0) {
        lastError_ = "Nothing below to merge into";
        return;
    }
    pushStructural("Merge down", [this]() { doc_.mergeDown(); });
}

void AppState::toggleVisibility(size_t index) {
    cancelActiveImageMove();
    if (index >= doc_.layerCount()) return;
    const bool before = doc_.layerAt(index).visible();
    const bool after = !before;
    pushHistory(
        "Toggle visibility",
        [this, index, before]() {
            doc_.layerAt(index).setVisible(before);
            notify();
            notifyLayers();
        },
        [this, index, after]() {
            doc_.layerAt(index).setVisible(after);
            notify();
            notifyLayers();
        });
    doc_.layerAt(index).setVisible(after);
    notify();
    notifyLayers();
}

void AppState::setLayerName(size_t index, std::string name) {
    cancelActiveImageMove();
    if (index >= doc_.layerCount()) return;
    name = name.substr(0, 64);
    // Trailing and leading blanks are almost always an accident, and a name that
    // renders as nothing makes the layer impossible to pick out of the list.
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    size_t start = 0;
    while (start < name.size() && (name[start] == ' ' || name[start] == '\t')) ++start;
    name = name.substr(start);
    if (name.empty()) return;
    if (name == doc_.layerAt(index).name()) return;

    const std::string before = doc_.layerAt(index).name();
    doc_.layerAt(index).setName(name);
    // The name only affects the panel's own label, so the canvas does not need to
    // redraw and the panel does not need rebuilding: notifyNameChanged carries the
    // one widget that has to update.
    notifyNameChanged(index, name);

    pushHistory(
        "Rename layer",
        [this, index, before]() {
            if (index >= doc_.layerCount()) return;
            doc_.layerAt(index).setName(before);
            notifyNameChanged(index, before);
        },
        [this, index, name]() {
            if (index >= doc_.layerCount()) return;
            doc_.layerAt(index).setName(name);
            notifyNameChanged(index, name);
        });
}

void AppState::setLayerOpacityLive(size_t index, double opacity) {
    if (index >= doc_.layerCount()) return;
    const double after = clampd(opacity, 0.0, 1.0);
    const double before = doc_.layerAt(index).opacity();
    if (std::fabs(before - after) < 0.004) return;

    if (!opacityDrag_.active || opacityDrag_.index != index) {
        opacityDrag_ = OpacityDrag{index, before, after, true};
    } else {
        opacityDrag_.after = after;
    }

    // Deliberately no notifyLayers(): this runs from a slider's value-changed
    // handler, and rebuilding the panel there destroys the slider and the row the
    // handler is holding. Only the canvas needs to know.
    doc_.layerAt(index).setOpacity(after);
    notify();
    notifyLayerPixels();
}

void AppState::commitLayerOpacity(size_t index) {
    if (!opacityDrag_.active || opacityDrag_.index != index) return;

    const size_t i = opacityDrag_.index;
    const double before = opacityDrag_.before;
    const double after = opacityDrag_.after;
    opacityDrag_ = OpacityDrag{};

    if (std::fabs(before - after) < 0.004) return;

    // One entry for the whole drag, so undo returns to where it started rather than
    // stepping back through every intermediate value.
    pushHistory(
        "Layer opacity",
        [this, i, before]() {
            if (i >= doc_.layerCount()) return;
            doc_.layerAt(i).setOpacity(before);
            notify();
            notifyLayerPixels();
        },
        [this, i, after]() {
            if (i >= doc_.layerCount()) return;
            doc_.layerAt(i).setOpacity(after);
            notify();
            notifyLayerPixels();
        });
}

void AppState::cancelLayerOpacity(size_t index) {
    if (!opacityDrag_.active || opacityDrag_.index != index) return;

    const size_t i = opacityDrag_.index;
    const double before = opacityDrag_.before;
    opacityDrag_ = OpacityDrag{};

    if (i >= doc_.layerCount()) return;
    doc_.layerAt(i).setOpacity(before);
    notify();
    notifyLayerPixels();
}

void AppState::clearLayer(size_t index) {
    cancelActiveImageMove();
    if (index >= doc_.layerCount()) return;
    if (doc_.layerAt(index).isEmpty()) return;

    SurfacePtr before = doc_.layerAt(index).snapshot();
    doc_.layerAt(index).clear();
    doc_.invalidateComposite();
    notify();
    notifyLayers();
    notifyLayerPixels();

    pushHistory(
        "Clear layer",
        [this, index, before]() {
            if (index >= doc_.layerCount()) return;
            doc_.layerAt(index).restore(before);
            doc_.invalidateComposite();
            notify();
            notifyLayers();
            notifyLayerPixels();
        },
        [this, index]() {
            if (index >= doc_.layerCount()) return;
            doc_.layerAt(index).clear();
            doc_.invalidateComposite();
            notify();
            notifyLayers();
            notifyLayerPixels();
        });
}

void AppState::clearActiveLayer() {
    cancelActiveImageMove();
    clearLayer(doc_.activeIndex());
}

void AppState::clearAllLayers() {
    cancelActiveImageMove();
    std::vector<SurfacePtr> before;
    for (size_t i = 0; i < doc_.layerCount(); ++i) before.push_back(doc_.layerAt(i).snapshot());
    doc_.clearAll();
    doc_.invalidateComposite();
    notify();
    notifyLayers();
    pushHistory("Clear all layers",
                  [this, before]() {
                      for (size_t i = 0; i < doc_.layerCount() && i < before.size(); ++i) {
                          doc_.layerAt(i).restore(before[i]);
                      }
                      notify();
                      notifyLayers();
                  },
                  [this]() {
                      doc_.clearAll();
                      notify();
                      notifyLayers();
                  });
}

bool AppState::setPaper(PaperKind kind, double spacing) {
    Paper next{kind, clampPaperSpacing(spacing)};
    if (next == doc_.paper()) return false;
    doc_.setPaper(next);
    notify();
    return true;
}

bool AppState::insertImage(SurfacePtr source, std::string name) {
    cancelActiveImageMove();
    if (!source) return false;

    const int pageW = doc_.width();
    const int pageH = doc_.height();
    const int srcW = cairo_image_surface_get_width(source.get());
    const int srcH = cairo_image_surface_get_height(source.get());
    if (srcW <= 0 || srcH <= 0) return false;

    // Only shrunk, never enlarged: a small icon dropped on a big page should arrive
    // at its own size, and anything that overflows the page is scaled to fit so it
    // can still be dragged into place.
    double scale = 1.0;
    if (srcW > pageW || srcH > pageH) {
        scale = std::min(static_cast<double>(pageW) / srcW, static_cast<double>(pageH) / srcH);
    }
    const double x = (pageW - srcW * scale) * 0.5;
    const double y = (pageH - srcH * scale) * 0.5;

    pushStructural("Insert image", [this, source, name, x, y, scale]() {
        doc_.addLayer();
        if (!name.empty()) doc_.active().setName(std::move(name));
        doc_.active().setImage(source, x, y, scale);
        doc_.invalidateComposite();
    });
    return true;
}

namespace {
SurfacePtr renderTextSurface(const Layer::TextInfo& info) {
    if (info.text.empty()) return nullptr;
    const double sz = clampd(info.fontSize, 8.0, 200.0);
    const char* family = info.fontFamily.empty() ? "Sans" : info.fontFamily.c_str();
    const cairo_font_slant_t slant = info.isItalic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL;
    const cairo_font_weight_t weight = info.isBold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL;

    // Split multi-line text by '\n'
    std::vector<std::string> lines;
    std::string current;
    for (char ch : info.text) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (ch != '\r') {
            current += ch;
        }
    }
    lines.push_back(current);

    SurfacePtr dummy = makeSurface(1, 1);
    cairo_t* crMeasure = cairo_create(dummy.get());
    cairo_select_font_face(crMeasure, family, slant, weight);
    cairo_set_font_size(crMeasure, sz);

    cairo_font_extents_t fextents;
    cairo_font_extents(crMeasure, &fextents);

    double maxW = 0.0;
    double minXBearing = 0.0;
    for (const auto& l : lines) {
        if (l.empty()) continue;
        cairo_text_extents_t ext;
        cairo_text_extents(crMeasure, l.c_str(), &ext);
        if (ext.width > maxW) maxW = ext.width;
        if (ext.x_bearing < minXBearing) minXBearing = ext.x_bearing;
    }
    cairo_destroy(crMeasure);

    const double pad = 12.0;
    const double lineH = (fextents.height > 0) ? fextents.height * 1.15 : sz * 1.25;
    const int w = std::max(1, static_cast<int>(std::ceil(maxW + pad * 2.0)));
    const int h = std::max(1, static_cast<int>(std::ceil(lines.size() * lineH + pad * 2.0)));

    SurfacePtr textSurf = makeSurface(w, h);
    cairo_t* cr = cairo_create(textSurf.get());
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_select_font_face(cr, family, slant, weight);
    cairo_set_font_size(cr, sz);
    cairo_set_source_rgb(cr, info.color.r, info.color.g, info.color.b);

    const double baselineY = pad + ((fextents.ascent > 0) ? fextents.ascent : sz * 0.8);
    for (size_t i = 0; i < lines.size(); ++i) {
        cairo_move_to(cr, pad - minXBearing, baselineY + i * lineH);
        cairo_show_text(cr, lines[i].c_str());
    }

    cairo_destroy(cr);
    cairo_surface_flush(textSurf.get());
    return textSurf;
}
}  // namespace


bool AppState::insertTextFormatted(const Layer::TextInfo& info, std::optional<double> customX, std::optional<double> customY) {
    if (info.text.empty()) return false;
    SurfacePtr surf = renderTextSurface(info);
    if (!surf) return false;

    std::string layerName = "Text: " + info.text;
    if (layerName.size() > 24) {
        layerName = layerName.substr(0, 21) + "...";
    }

    const double srcW = cairo_image_surface_get_width(surf.get());
    const double srcH = cairo_image_surface_get_height(surf.get());
    const double pageW = doc_.width();
    const double pageH = doc_.height();

    double scale = 1.0;
    if (srcW > pageW * 0.9 || srcH > pageH * 0.9) {
        scale = std::min(pageW * 0.85 / srcW, pageH * 0.85 / srcH);
    }

    const double x = customX.value_or((pageW - srcW * scale) * 0.5);
    const double y = customY.value_or((pageH - srcH * scale) * 0.5);

    Layer::TextInfo storedInfo = info;
    storedInfo.valid = true;

    pushStructural("Insert text", [this, surf, layerName, x, y, scale, storedInfo]() {
        doc_.addLayer();
        if (!layerName.empty()) doc_.active().setName(layerName);
        doc_.active().setImage(surf, x, y, scale);
        doc_.active().setTextInfo(storedInfo);
        doc_.invalidateComposite();
    });
    return true;
}

bool AppState::insertText(const std::string& text, double fontSize, const Color& color) {
    Layer::TextInfo info;
    info.text = text;
    info.fontSize = fontSize;
    info.color = color;
    info.isBold = true;
    info.isItalic = false;
    info.fontFamily = "Sans";
    info.valid = true;
    return insertTextFormatted(info);
}

bool AppState::insertTextWithCurrentBrush(const std::string& text, double fontSize) {
    Color c{brush_.r, brush_.g, brush_.b};
    return insertText(text, fontSize, c);
}

bool AppState::updateActiveTextLayer(const Layer::TextInfo& info) {
    if (info.text.empty() || !activeLayerHasText()) return false;
    const size_t idx = doc_.activeIndex();
    SurfacePtr newSurf = renderTextSurface(info);
    if (!newSurf) return false;

    std::string layerName = "Text: " + info.text;
    if (layerName.size() > 24) {
        layerName = layerName.substr(0, 21) + "...";
    }

    const double currentX = doc_.active().image().x;
    const double currentY = doc_.active().image().y;
    const double currentScale = doc_.active().image().scale;
    Layer::TextInfo updatedInfo = info;
    updatedInfo.valid = true;

    pushStructural("Update text format", [this, idx, newSurf, layerName, currentX, currentY, currentScale, updatedInfo]() {
        if (idx < doc_.layerCount()) {
            doc_.setActiveIndex(idx);
            doc_.active().setName(layerName);
            doc_.active().setImage(newSurf, currentX, currentY, currentScale);
            doc_.active().setTextInfo(updatedInfo);
            doc_.invalidateComposite();
        }
    });
    return true;
}

bool AppState::activeLayerHasText() const {
    return doc_.layerCount() > 0 && doc_.activeIndex() < doc_.layerCount() && doc_.active().hasText();
}

Layer::TextInfo AppState::activeLayerTextInfo() const {
    if (!activeLayerHasText()) return Layer::TextInfo{};
    return doc_.active().textInfo();
}

RecognizedText AppState::recognizeActiveLayerHandwriting() const {
    if (doc_.layerCount() == 0) return RecognizedText{};
    cairo_surface_t* s = doc_.active().surface();
    if (!s) return RecognizedText{};
    HandwritingRecognizer rec;
    return rec.recognizeSurface(s);
}

void AppState::learnHandwritingWord(const std::string& word) {
    HandwritingRecognizer::learnUserWord(word);
}


bool AppState::convertHandwritingToText(const std::string& text, const Rect& bounds,
                                        bool replaceOriginal, double fontSize) {
    if (text.empty()) return false;

    const size_t origIdx = doc_.activeIndex();
    const Color c{brush_.r, brush_.g, brush_.b};
    const double sz = clampd(fontSize, 8.0, 200.0);

    Layer::TextInfo storedInfo;
    storedInfo.text = text;
    storedInfo.fontSize = sz;
    storedInfo.color = c;
    storedInfo.isBold = true;
    storedInfo.isItalic = false;
    storedInfo.fontFamily = "Sans";
    storedInfo.valid = true;


    SurfacePtr textSurf = renderTextSurface(storedInfo);
    if (!textSurf) return false;

    std::string layerName = "Text: " + text;
    std::replace(layerName.begin(), layerName.end(), '\n', ' ');
    if (layerName.size() > 24) {
        layerName = layerName.substr(0, 21) + "...";
    }

    const double w = cairo_image_surface_get_width(textSurf.get());
    const double h = cairo_image_surface_get_height(textSurf.get());
    const double targetX = bounds.hasArea() ? bounds.x : (doc_.width() - w) * 0.5;
    const double targetY = bounds.hasArea() ? bounds.y : (doc_.height() - h) * 0.5;


    pushStructural("Convert handwriting to text", [this, textSurf, layerName, targetX, targetY, origIdx, replaceOriginal, storedInfo]() {
        if (replaceOriginal && origIdx < doc_.layerCount()) {
            doc_.setActiveIndex(origIdx);
            doc_.active().clear();
        }
        doc_.addLayer();
        if (!layerName.empty()) doc_.active().setName(layerName);
        doc_.active().setImage(textSurf, targetX, targetY, 1.0);
        doc_.active().setTextInfo(storedInfo);
        doc_.invalidateComposite();
    });
    return true;
}

bool AppState::activeLayerHasImage() const {
    return doc_.layerCount() > 0 && doc_.activeIndex() < doc_.layerCount() && doc_.active().hasImage();
}

bool AppState::canMoveActiveLayer() const {
    return doc_.layerCount() > 0 && doc_.activeIndex() < doc_.layerCount();
}

void AppState::applyImagePlacement(size_t index, double x, double y, double scale) {
    if (index >= doc_.layerCount()) return;
    Layer& layer = doc_.layerAt(index);
    if (!layer.hasImage()) return;
    layer.setImageScale(scale);
    layer.setImagePosition(x, y);
    doc_.invalidateComposite();
    notify();
    notifyLayerPixels();
}

bool AppState::commitImagePlacement(size_t index, const char* label, double fromX, double fromY,
                                    double fromScale, double toX, double toY, double toScale) {
    if (index >= doc_.layerCount()) return false;
    const Layer& layer = doc_.layerAt(index);
    if (!layer.hasImage()) return false;

    if (fromX == toX && fromY == toY && fromScale == toScale) return false;

    history_.push(
        label,
        [this, index, fromX, fromY, fromScale]() {
            applyImagePlacement(index, fromX, fromY, fromScale);
        },
        [this, index, toX, toY, toScale]() { applyImagePlacement(index, toX, toY, toScale); });
    return true;
}

bool AppState::beginMoveActiveImage() {
    if (imageDrag_.active) return false;
    if (!canMoveActiveLayer()) return false;

    Layer& layer = doc_.active();
    if (!layer.hasImage() && layer.isEmpty()) return false;

    imageDrag_.index = doc_.activeIndex();
    imageDrag_.hasImage = layer.hasImage();
    imageDrag_.moved = false;
    if (layer.hasImage()) {
        const Layer::ImagePlacement& image = layer.image();
        imageDrag_.startX = image.x;
        imageDrag_.startY = image.y;
        imageDrag_.startSnapshot = nullptr;
    } else {
        imageDrag_.startX = 0.0;
        imageDrag_.startY = 0.0;
        imageDrag_.startSnapshot = layer.snapshot();
    }
    imageDrag_.active = true;
    return true;
}

bool AppState::moveActiveImageBy(double dx, double dy) {
    if (!imageDrag_.active) return false;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return false;
    if (imageDrag_.index >= doc_.layerCount()) return false;
    if (doc_.activeIndex() != imageDrag_.index) return false;

    Layer& layer = doc_.layerAt(imageDrag_.index);
    if (!layer.moveContentBy(dx, dy)) return false;

    imageDrag_.moved = true;
    doc_.invalidateComposite();
    notify();
    notifyLayerPixels();
    return true;
}

bool AppState::commitActiveImageMove() {
    if (!imageDrag_.active) return false;
    const ImageMoveDrag drag = imageDrag_;
    imageDrag_ = ImageMoveDrag{};

    if (!drag.moved) return false;
    if (drag.index >= doc_.layerCount()) return false;
    Layer& layer = doc_.layerAt(drag.index);

    if (drag.hasImage) {
        if (!layer.hasImage()) return false;
        const bool committed = commitImagePlacement(drag.index, "Move layer", drag.startX,
                                                    drag.startY, layer.image().scale, layer.image().x,
                                                    layer.image().y, layer.image().scale);
        notify();
        return committed;
    } else {
        SurfacePtr endSnapshot = layer.snapshot();
        pushHistory(
            "Move layer",
            [this, index = drag.index, snap = drag.startSnapshot]() {
                if (index < doc_.layerCount()) {
                    doc_.layerAt(index).restore(snap);
                    doc_.invalidateComposite();
                    notify();
                    notifyLayers();
                    notifyLayerPixels();
                }
            },
            [this, index = drag.index, snap = endSnapshot]() {
                if (index < doc_.layerCount()) {
                    doc_.layerAt(index).restore(snap);
                    doc_.invalidateComposite();
                    notify();
                    notifyLayers();
                    notifyLayerPixels();
                }
            });
        notify();
        return true;
    }
}

void AppState::cancelActiveImageMove() {
    if (!imageDrag_.active) return;
    const ImageMoveDrag drag = imageDrag_;
    imageDrag_ = ImageMoveDrag{};

    if (drag.index < doc_.layerCount()) {
        Layer& layer = doc_.layerAt(drag.index);
        if (drag.hasImage && layer.hasImage()) {
            applyImagePlacement(drag.index, drag.startX, drag.startY, layer.image().scale);
        } else if (drag.startSnapshot) {
            layer.restore(drag.startSnapshot);
            doc_.invalidateComposite();
            notify();
            notifyLayerPixels();
        }
    }
}

bool AppState::setImageScaleLive(double scale) {
    if (doc_.activeIndex() >= doc_.layerCount()) return false;
    Layer& layer = doc_.active();
    if (!layer.hasImage()) return false;

    // The gesture opens on its first change. There is no separate "begin" for a
    // slider the way there is for a drag that starts with a button press, and
    // recording the placement here is what lets the whole sweep collapse into one undo
    // step that restores the exact starting position as well as the starting scale.
    if (!imageScaleDrag_.active) {
        imageScaleDrag_.index = doc_.activeIndex();
        imageScaleDrag_.startX = layer.image().x;
        imageScaleDrag_.startY = layer.image().y;
        imageScaleDrag_.startScale = layer.image().scale;
        imageScaleDrag_.active = true;
    } else if (imageScaleDrag_.index != doc_.activeIndex()) {
        // The active layer changed mid-sweep, so this is a different picture. Close
        // the old gesture rather than attributing its numbers to the new one.
        imageScaleDrag_ = ImageScaleDrag{};
    }

    if (!layer.setImageScale(scale)) return false;
    doc_.invalidateComposite();
    notify();
    return true;
}

bool AppState::commitImageScale() {
    if (!imageScaleDrag_.active) return false;
    const ImageScaleDrag drag = imageScaleDrag_;
    imageScaleDrag_ = ImageScaleDrag{};

    if (drag.index >= doc_.layerCount()) return false;
    const Layer& layer = doc_.layerAt(drag.index);
    if (!layer.hasImage()) return false;

    return commitImagePlacement(drag.index, "Resize image", drag.startX, drag.startY,
                                drag.startScale, layer.image().x, layer.image().y,
                                layer.image().scale);
}

void AppState::cancelImageScale() {
    if (!imageScaleDrag_.active) return;
    const ImageScaleDrag drag = imageScaleDrag_;
    imageScaleDrag_ = ImageScaleDrag{};
    if (drag.index >= doc_.layerCount()) return;
    Layer& layer = doc_.layerAt(drag.index);
    if (!layer.hasImage()) return;
    // Rescale first, then re-anchor on the same centre the resize used, so undoing
    // mid-sweep puts the picture back exactly where the sweep started.
    const Rect now = layer.imageBounds();
    const double centreX = now.x + now.w * 0.5;
    const double centreY = now.y + now.h * 0.5;
    const int sw = cairo_image_surface_get_width(layer.image().source.get());
    const int sh = cairo_image_surface_get_height(layer.image().source.get());
    layer.setImageScale(drag.startScale);
    layer.setImagePosition(centreX - sw * drag.startScale * 0.5,
                           centreY - sh * drag.startScale * 0.5);
    doc_.invalidateComposite();
    notify();
}

bool AppState::scaleActiveImageToPage() {
    if (!activeLayerHasImage()) return false;
    Layer& layer = doc_.active();
    const Rect current = layer.imageBounds();
    const double pageW = doc_.width();
    const double pageH = doc_.height();
    if (current.w <= 0.0 || current.h <= 0.0) return false;

    // The larger ratio wins, so a wide picture is not blown up past the page just
    // because it is narrow enough to fit on one axis.
    const double fit = std::min(pageW / current.w, pageH / current.h);
    return setImageScaleLive(layer.image().scale * fit);
}

void AppState::centreActiveImage() {
    if (!activeLayerHasImage()) return;
    Layer& layer = doc_.active();
    const Rect current = layer.imageBounds();
    layer.setImagePosition((doc_.width() - current.w) * 0.5, (doc_.height() - current.h) * 0.5);
    doc_.invalidateComposite();
    notify();
}

bool AppState::deleteActiveImage() {
    if (!activeLayerHasImage()) return false;
    if (doc_.layerCount() <= 1) {
        lastError_ = "Need at least one layer";
        return false;
    }
    deleteLayer();
    return true;
}

bool AppState::setBackgroundColor(const std::string& hex) {
    if (!doc_.setBackground(hex)) return false;
    // The page is not part of the composite cache -- layers composite over
    // transparency -- so only the widget needs repainting.
    notify();
    return true;
}

void AppState::newCanvas() {
    StackState before = captureStack();

    std::vector<std::unique_ptr<Layer>> fresh;
    fresh.push_back(std::make_unique<Layer>("Background", doc_.width(), doc_.height()));
    doc_.replaceLayers(std::move(fresh), 0);
    // A new sheet goes back to the default page colour and no ruling rather than
    // keeping the old ones, and captureStack below records both so Ctrl+Z restores
    // them.
    doc_.setBackground(Color{0.984, 0.984, 0.992});
    doc_.setPaper(Paper{});
    notify();
    notifyLayers();
    StackState after = captureStack();
    pushHistory("New canvas", [this, before]() { applyStack(before); },
                  [this, after]() { applyStack(after); });
}

std::optional<Project> AppState::serialize() const {
    return serializeProject(doc_, settings_);
}

bool AppState::load(const Project& project) {
    auto doc = deserializeProject(project);
    if (!doc) {
        lastError_ = "Could not read project data";
        return false;
    }
    doc_ = std::move(*doc);
    settings_ = project.settings;
    brush_.kind = settings_.tool;
    brush_.setHex(settings_.color);
    brush_.size = settings_.size;
    brush_.tiltShading = settings_.tiltShading;
    brush_.pressureWidth = settings_.pressureWidth;
    brush_.pressureSensitivity = settings_.pressureSensitivity;
    live_.clear();
    liveLayerIndex_ = 0;
    drawing_ = false;
    history_.clear();
    lastError_ = std::nullopt;
    notify();
    notifyLayers();
    return true;
}

std::optional<std::string> AppState::exportPng() const {
    SurfacePtr flat = doc_.flatten();
    std::string png = encodePng(flat.get());
    if (png.empty()) return std::nullopt;
    return png;
}

}  // namespace stylus