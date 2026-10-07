#include "color.h"

#include <cstdio>

#include "geometry.h"

namespace stylus {

namespace {

bool nibble(char c, int* out) {
    if (c >= '0' && c <= '9') {
        *out = c - '0';
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = c - 'a' + 10;
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = c - 'A' + 10;
        return true;
    }
    return false;
}

bool byte(char hi, char lo, double* out) {
    int h = 0;
    int l = 0;
    if (!nibble(hi, &h) || !nibble(lo, &l)) return false;
    *out = (h * 16 + l) / 255.0;
    return true;
}

}  // namespace

bool parseHexColor(const std::string& hex, Color* out) {
    if (!out) return false;

    std::string s = hex;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() == 3) {
        // "#abc" means "#aabbcc", so each digit is its own high and low nibble.
        s = std::string(1, s[0]) + s[0] + std::string(1, s[1]) + s[1] + std::string(1, s[2]) + s[2];
    }
    if (s.size() != 6) return false;

    Color parsed;
    if (!byte(s[0], s[1], &parsed.r) || !byte(s[2], s[3], &parsed.g) || !byte(s[4], s[5], &parsed.b)) {
        return false;
    }
    *out = parsed;
    return true;
}

std::string formatHexColor(const Color& color) {
    auto component = [](double v) {
        const int i = static_cast<int>(clampd(v, 0.0, 1.0) * 255.0 + 0.5);
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", i);
        return std::string(buf);
    };
    return "#" + component(color.r) + component(color.g) + component(color.b);
}

}  // namespace stylus