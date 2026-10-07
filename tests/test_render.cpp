// Renders every brush mode to PNG files so the output can be inspected visually.
#include <cairo/cairo.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "brush.h"
#include "document.h"
#include "project.h"

using namespace stylus;

namespace {

// A single captured-looking pen line: gentle arc, smooth pressure ramp.
Stroke penLine(double x0, double y0, double x1, double y1, double pLo, double pHi, double tiltX,
               double tiltY, size_t samples = 200) {
    Stroke s;
    for (size_t i = 0; i < samples; ++i) {
        const double t = samples > 1 ? static_cast<double>(i) / (samples - 1) : 0.0;
        Point p;
        p.x = x0 + (x1 - x0) * t;
        p.y = y0 + (y1 - y0) * t - 14.0 * std::sin(t * M_PI);
        p.pressure = pLo + (pHi - pLo) * std::sin(t * M_PI);
        p.tiltX = tiltX;
        p.tiltY = tiltY;
        s.points.push_back(p);
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string outDir = argc > 1 ? argv[1] : ".";

    Document doc(1400, 1000);
    Layer& layer = doc.active();

    struct Row {
        BrushKind kind;
        const char* color;
        const char* label;
        double pLo;
        double pHi;
        double tiltX;
        double tiltY;
    };

    const Row rows[] = {
        {BrushKind::Pen, "#1b1b1f", "pen: pressure ramp 0.05 -> 1.0", 0.05, 1.0, 6, 3},
        {BrushKind::Pen, "#b91c1c", "pen: constant heavy pressure", 0.9, 0.95, 0, 0},
        {BrushKind::Pen, "#1b1b1f", "pen: tilt 58 deg (nib widens along lean)", 0.35, 0.95, 58, -50},
        {BrushKind::Pencil, "#4b5563", "pencil: grain + pressure", 0.1, 1.0, 0, 0},
        {BrushKind::Marker, "#ca8a04", "marker: wide, translucent, multiply", 0.6, 1.0, 0, 0},
    };

    double y = 120.0;
    for (const Row& row : rows) {
        Brush brush;
        brush.kind = row.kind;
        brush.setHex(row.color);
        brush.size = 14.0;
        brush.tiltShading = true;

        Stroke s = penLine(80.0, y, 1050.0, y, row.pLo, row.pHi, row.tiltX, row.tiltY);
        drawStrokeOn(layer.surface(), s, brush);
        y += 150.0;
    }

    // Second layer: a cyan line above the black one, with an eraser gap that
    // must cut only this layer and leave the black stroke underneath intact.
    doc.addLayer();
    const double cyanY = y + 60.0;
    Brush pen;
    pen.size = 12.0;
    pen.setHex("#0ea5e9");
    drawStrokeOn(doc.active().surface(), penLine(80.0, cyanY, 1050.0, cyanY, 0.2, 1.0, 0, 0), pen);

    Brush eraser;
    eraser.kind = BrushKind::Eraser;
    eraser.size = 30.0;
    Stroke eraseStroke;
    for (int i = 0; i <= 30; ++i) {
        eraseStroke.points.push_back(Point{620.0 + i * 8.0, cyanY, 0.9, 0, 0});
    }
    drawStrokeOn(doc.active().surface(), eraseStroke, eraser);

    // A black stroke on the base layer at the same height, to prove the eraser
    // did not reach it.
    Brush base;
    base.size = 12.0;
    base.setHex("#1b1b1f");
    drawStrokeOn(doc.layerAt(0).surface(), penLine(80.0, cyanY + 34.0, 1050.0, cyanY + 34.0,
                                                   0.2, 1.0, 0, 0),
                 base);

    // Labels are drawn onto the composite so the sheet is self-describing.
    SurfacePtr withLabels = doc.flatten();
    cairo_t* cr = cairo_create(withLabels.get());
    double labelY = 100.0;
    for (const Row& row : rows) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 19.0);
        cairo_set_source_rgb(cr, 0.40, 0.40, 0.44);
        cairo_move_to(cr, 80.0, labelY - 22.0);
        cairo_show_text(cr, row.label);
        labelY += 150.0;
    }
    cairo_set_source_rgb(cr, 0.40, 0.40, 0.44);
    cairo_move_to(cr, 80.0, labelY + 40.0);
    cairo_show_text(cr, "cyan on layer 2 with an eraser gap; black below is untouched");
    cairo_destroy(cr);
    cairo_surface_write_to_png(withLabels.get(), (outDir + "/render-all.png").c_str());

    // Also emit each brush in isolation on a white page.
    for (const Row& row : rows) {
        Document solo(1200, 200);
        Brush brush;
        brush.kind = row.kind;
        brush.setHex(row.color);
        brush.size = 16.0;
        brush.tiltShading = true;
        drawStrokeOn(solo.active().surface(),
                     penLine(60.0, 110.0, 1140.0, 110.0, row.pLo, row.pHi, row.tiltX, row.tiltY),
                     brush);
        SurfacePtr one = solo.flatten();

        std::string name;
        switch (row.kind) {
            case BrushKind::Pencil: name = "pencil.png"; break;
            case BrushKind::Marker: name = "marker.png"; break;
            default: name = "pen.png"; break;
        }
        cairo_surface_write_to_png(one.get(), (outDir + "/" + name).c_str());
    }

    std::printf("wrote render-all.png, pen.png, pencil.png, marker.png to %s\n", outDir.c_str());
    return 0;
}