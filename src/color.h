#pragma once

#include <string>

namespace stylus {

// An sRGB triple in 0..1, which is also the form Cairo wants, so handing one to
// cairo_set_source_rgb is a straight copy.
struct Color {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

// Accepts "#rgb", "#rrggbb", and both of those without the leading '#'.
//
// A malformed or wrong-length string returns false and leaves `out` untouched.
// These strings come straight out of project files, which the user can edit, and
// the obvious shortcut -- std::stoi on a substring -- throws on anything that is
// not a number and takes the whole process down with it. Staying silent instead
// means a hand-edited file degrades to the default colour instead of crashing.
bool parseHexColor(const std::string& hex, Color* out);

// Lowercase "#rrggbb". Values are clamped, so a component outside 0..1 (only
// reachable from a raw Color) formats rather than emitting a byte out of range.
std::string formatHexColor(const Color& color);

}  // namespace stylus