#pragma once

#include <cairo/cairo.h>

#include <string>
#include <vector>

#include "geometry.h"

namespace stylus {

struct RecognizedText {
    std::string text;
    Rect bounds;
    double confidence = 0.0;
};

// Analyzes ink pixels on a Cairo surface and converts handwriting strokes to digital text.
// Supports both a native topological C++ recognizer and dynamic libtesseract integration.
class HandwritingRecognizer {
public:
    HandwritingRecognizer() = default;
    ~HandwritingRecognizer() = default;

    // Recognizes text from a layer surface. Returns recognized text string and bounding box.
    RecognizedText recognizeSurface(cairo_surface_t* surface) const;

    // Helper to preprocess surface into an upscaled, padded 8-bit grayscale image for OCR
    static std::vector<unsigned char> preprocessSurface(cairo_surface_t* surface,
                                                         const Rect& inkBounds,
                                                         int pad, double scale,
                                                         int* outW, int* outH);

    // Dynamic user handwriting learning: persists & remembers user confirmed words to adapt over time
    static void learnUserWord(const std::string& word);
    static std::vector<std::string> getUserWords();


private:
    std::string recognizeCharacterGlyph(const std::vector<unsigned char>& subImage,
                                         int width, int height, double strokeAspect) const;
    std::string recognizeWordFallback(const std::vector<unsigned char>& croppedImage,
                                       int width, int height) const;
};

}  // namespace stylus

