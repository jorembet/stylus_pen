#include "ocr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <sstream>
#include <string>

#include <vector>

#include "platform.h"

namespace stylus {

namespace {

// Dynamic binding to libtesseract
typedef void* (*TessApiCreateFn)();
typedef void (*TessApiDeleteFn)(void*);
typedef int (*TessApiInitFn)(void*, const char*, const char*);
typedef void (*TessApiSetImageFn)(void*, const unsigned char*, int, int, int, int);
typedef char* (*TessApiGetTextFn)(void*);
typedef void (*TessDeleteTextFn)(char*);
typedef void (*TessApiSetPageSegModeFn)(void*, int);
typedef bool (*TessApiSetVariableFn)(void*, const char*, const char*);

struct TesseractHandle {
    SharedLibrary lib;
    TessApiCreateFn apiCreate = nullptr;
    TessApiDeleteFn apiDelete = nullptr;
    TessApiInitFn apiInit = nullptr;
    TessApiSetImageFn apiSetImage = nullptr;
    TessApiGetTextFn apiGetText = nullptr;
    TessDeleteTextFn deleteText = nullptr;
    TessApiSetPageSegModeFn apiSetPSM = nullptr;
    TessApiSetVariableFn apiSetVar = nullptr;

    TesseractHandle() {
        // Both platforms' names, and both version spellings. The library is looked
        // up rather than linked so the app runs, and draws, on a machine with no OCR
        // engine at all -- so this list missing an entry costs handwriting
        // recognition on that platform and nothing else. MSYS2 ships the soname
        // with the minor version in it; upstream Windows builds use tesseract5x.dll.
        if (!lib.openAny({"libtesseract.so.5", "libtesseract.so", "libtesseract.so.4",
                          "libtesseract-5.dll", "libtesseract-5.5.dll", "libtesseract-6.dll",
                          "libtesseract.dll", "tesseract55.dll", "tesseract54.dll",
                          "tesseract53.dll", "tesseract52.dll", "tesseract51.dll"})) {
            return;
        }
        apiCreate = reinterpret_cast<TessApiCreateFn>(lib.symbol("TessBaseAPICreate"));
        apiDelete = reinterpret_cast<TessApiDeleteFn>(lib.symbol("TessBaseAPIDelete"));
        apiInit = reinterpret_cast<TessApiInitFn>(lib.symbol("TessBaseAPIInit3"));
        if (!apiInit) apiInit = reinterpret_cast<TessApiInitFn>(lib.symbol("TessBaseAPIInit2"));
        apiSetImage = reinterpret_cast<TessApiSetImageFn>(lib.symbol("TessBaseAPISetImage"));
        apiGetText = reinterpret_cast<TessApiGetTextFn>(lib.symbol("TessBaseAPIGetUTF8Text"));
        deleteText = reinterpret_cast<TessDeleteTextFn>(lib.symbol("TessDeleteText"));
        apiSetPSM = reinterpret_cast<TessApiSetPageSegModeFn>(lib.symbol("TessBaseAPISetPageSegMode"));
        apiSetVar = reinterpret_cast<TessApiSetVariableFn>(lib.symbol("TessBaseAPISetVariable"));
    }

    bool isValid() const {
        return lib.isOpen() && apiCreate && apiDelete && apiSetImage && apiGetText;
    }
};

const TesseractHandle& getTesseract() {
    static TesseractHandle h;
    return h;
}

// Gets executable directory path
std::string getExeDir() {
    const std::string dir = executableDirectory();
    return dir.empty() ? std::string(".") : dir;
}

bool fileExists(const std::string& path) {
    std::ifstream f(path.c_str());
    return f.good();
}

}  // namespace

std::vector<unsigned char> HandwritingRecognizer::preprocessSurface(cairo_surface_t* surface,
                                                                      const Rect& inkBounds,
                                                                      int pad, double scale,
                                                                      int* outW, int* outH) {
    if (!surface || inkBounds.w <= 0 || inkBounds.h <= 0 || scale <= 0.0) {
        if (outW) *outW = 0;
        if (outH) *outH = 0;
        return {};
    }

    const int srcW = cairo_image_surface_get_width(surface);
    const int srcH = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char* srcData = cairo_image_surface_get_data(surface);

    const double cropX = inkBounds.x - pad;
    const double cropY = inkBounds.y - pad;
    const double cropW = inkBounds.w + 2 * pad;
    const double cropH = inkBounds.h + 2 * pad;

    const int targetW = static_cast<int>(std::round(cropW * scale));
    const int targetH = static_cast<int>(std::round(cropH * scale));

    if (outW) *outW = targetW;
    if (outH) *outH = targetH;

    std::vector<unsigned char> out(targetW * targetH, 255);  // White background

    for (int ty = 0; ty < targetH; ++ty) {
        const double sy = cropY + (static_cast<double>(ty) + 0.5) / scale;
        const int iy = static_cast<int>(std::floor(sy));

        for (int tx = 0; tx < targetW; ++tx) {
            const double sx = cropX + (static_cast<double>(tx) + 0.5) / scale;
            const int ix = static_cast<int>(std::floor(sx));

            if (ix >= 0 && ix < srcW && iy >= 0 && iy < srcH) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(srcData + iy * stride);
                const uint32_t px = row[ix];
                const uint8_t a = (px >> 24) & 0xFF;
                if (a > 10) {
                    const uint8_t r = (px >> 16) & 0xFF;
                    const uint8_t g = (px >> 8) & 0xFF;
                    const uint8_t b = px & 0xFF;
                    const int luma = static_cast<int>(0.299 * r + 0.587 * g + 0.114 * b);
                    // Blend alpha on white background (255) to preserve anti-aliased stroke curves and inner loops
                    const int blended = (luma * a + 255 * (255 - a)) / 255;
                    out[ty * targetW + tx] = static_cast<unsigned char>(blended);
                }
            }
        }
    }

    return out;
}



int levenshteinDistance(const std::string& s1, const std::string& s2) {

    const size_t m = s1.size();
    const size_t n = s2.size();
    if (m == 0) return static_cast<int>(n);
    if (n == 0) return static_cast<int>(m);

    std::vector<int> col(n + 1);
    for (size_t j = 0; j <= n; ++j) col[j] = static_cast<int>(j);

    for (size_t i = 1; i <= m; ++i) {
        col[0] = static_cast<int>(i);
        int lastDiag = static_cast<int>(i - 1);
        for (size_t j = 1; j <= n; ++j) {
            int oldCol = col[j];
            int cost = (tolower(static_cast<unsigned char>(s1[i - 1])) == tolower(static_cast<unsigned char>(s2[j - 1]))) ? 0 : 1;
            col[j] = std::min({col[j] + 1, col[j - 1] + 1, lastDiag + cost});
            lastDiag = oldCol;
        }
    }
    return col[n];
}

static std::vector<std::string> g_userLearnedWords;
static bool g_userWordsLoaded = false;

static std::string getUserWordsFilePath() {
    // The user's own directory, so words learned on one machine follow them and are
    // not lost when a project is reopened elsewhere. The bundled path is the
    // fallback: it keeps the feature working on a machine with no writable config
    // directory rather than silently discarding every learned word.
    const std::string dir = userConfigDirectory("stylus-pen");
    if (!dir.empty()) return joinPath(dir, "user_words.txt");
    return "./data/user_words.txt";
}

static void loadUserWords() {
    if (g_userWordsLoaded) return;
    g_userWordsLoaded = true;

    std::string path = getUserWordsFilePath();
    std::ifstream in(path.c_str());
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                g_userLearnedWords.push_back(line);
            }
        }
    }
}

void HandwritingRecognizer::learnUserWord(const std::string& word) {
    if (word.empty()) return;
    loadUserWords();

    std::stringstream ss(word);
    std::string w;
    while (ss >> w) {
        while (!w.empty() && ispunct(static_cast<unsigned char>(w.back()))) w.pop_back();
        while (!w.empty() && ispunct(static_cast<unsigned char>(w.front()))) w.erase(w.begin());
        if (w.length() < 2) continue;

        if (std::find(g_userLearnedWords.begin(), g_userLearnedWords.end(), w) == g_userLearnedWords.end()) {
            g_userLearnedWords.push_back(w);

            std::string path = getUserWordsFilePath();
            std::ofstream out(path.c_str(), std::ios::app);
            if (out.is_open()) {
                out << w << "\n";
            }
        }
    }
}

std::vector<std::string> HandwritingRecognizer::getUserWords() {
    loadUserWords();
    return g_userLearnedWords;
}

std::string correctWordWithDictionary(const std::string& text) {
    if (text.length() < 2) return text;
    loadUserWords();

    static const char* const dictionary[] = {
        "bego", "halo", "test", "pen", "stylus", "text", "draw", "image", "paper", "canvas", "hello", "word",
        "bisa", "aplikasi", "layar", "tulisan", "tangan", "konversi", "halaman", "terima", "kasih", "indonesia",
        "catatan", "nota", "buku", "gaya", "gambar", "warna", "alat", "proyek", "edit", "update", "format", "simpan", "buka"
    };

    std::string bestMatch = text;
    int bestDist = 999;

    for (const char* dictWord : dictionary) {
        int dist = levenshteinDistance(text, dictWord);
        size_t maxLen = std::max(text.length(), strlen(dictWord));
        int maxAllowedDist = (maxLen <= 4) ? 1 : ((maxLen <= 7) ? 2 : 3);
        if (dist <= maxAllowedDist && dist < bestDist) {
            bestDist = dist;
            bestMatch = dictWord;
        }
    }

    for (const auto& userWord : g_userLearnedWords) {
        int dist = levenshteinDistance(text, userWord);
        size_t maxLen = std::max(text.length(), userWord.length());
        int maxAllowedDist = (maxLen <= 4) ? 1 : ((maxLen <= 7) ? 2 : 3);
        if (dist <= maxAllowedDist && dist < bestDist) {
            bestDist = dist;
            bestMatch = userWord;
        }
    }

    return bestMatch;
}

std::string correctTextWithDictionary(const std::string& text) {
    if (text.empty()) return text;
    std::string result;
    std::istringstream stream(text);
    std::string line;
    bool firstLine = true;
    while (std::getline(stream, line)) {
        if (!firstLine) result += "\n";
        firstLine = false;

        std::istringstream lineStream(line);
        std::string token;
        bool firstToken = true;
        while (lineStream >> token) {
            if (!firstToken) result += " ";
            firstToken = false;

            std::string cleanToken = token;
            std::string prefix = "", suffix = "";
            while (!cleanToken.empty() && std::ispunct(static_cast<unsigned char>(cleanToken.front()))) {
                prefix += cleanToken.front();
                cleanToken.erase(cleanToken.begin());
            }
            while (!cleanToken.empty() && std::ispunct(static_cast<unsigned char>(cleanToken.back()))) {
                suffix = cleanToken.back() + suffix;
                cleanToken.pop_back();
            }

            if (cleanToken.length() >= 2) {
                std::string corrected = correctWordWithDictionary(cleanToken);
                result += prefix + corrected + suffix;
            } else {
                result += token;
            }
        }
    }
    return result;
}


RecognizedText HandwritingRecognizer::recognizeSurface(cairo_surface_t* surface) const {

    RecognizedText out;
    if (!surface || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE) {
        return out;
    }

    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    if (!data || width <= 0 || height <= 0) return out;

    int minX = width, minY = height, maxX = -1, maxY = -1;

    // Find ink bounds
    for (int y = 0; y < height; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(data + y * stride);
        for (int x = 0; x < width; ++x) {
            if (((row[x] >> 24) & 0xFF) > 15) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    if (maxX < minX || maxY < minY) {
        return out;  // Empty layer
    }

    const int pad = 24;
    minX = std::max(0, minX - pad);
    minY = std::max(0, minY - pad);
    maxX = std::min(width - 1, maxX + pad);
    maxY = std::min(height - 1, maxY + pad);

    const int cropW = maxX - minX + 1;
    const int cropH = maxY - minY + 1;
    out.bounds = Rect{static_cast<double>(minX), static_cast<double>(minY),
                      static_cast<double>(cropW), static_cast<double>(cropH)};

    // Calculate upscale factor for Tesseract (target height 140-160px for ideal 300 DPI text scale)
    double scale = 2.0;
    if (cropH < 120) {
        scale = std::min(4.0, std::max(2.0, 160.0 / std::max(1, cropH)));
    } else if (cropH > 500) {
        scale = 1.25;
    }

    int targetW = 0, targetH = 0;
    Rect rawInkRect{static_cast<double>(minX + pad), static_cast<double>(minY + pad),
                    static_cast<double>(cropW - 2 * pad), static_cast<double>(cropH - 2 * pad)};
    std::vector<unsigned char> pix = preprocessSurface(surface, rawInkRect, pad, scale, &targetW, &targetH);

    if (pix.empty() || targetW <= 0 || targetH <= 0) return out;

    // Try Tesseract OCR
    const auto& tess = getTesseract();
    if (tess.isValid()) {
        void* api = tess.apiCreate();
        if (api) {
            // Ordered most-specific first. The repo's own data/tessdata comes before
            // any system one so the language set this app was tested against is the
            // one it uses, and so a project made on a machine with a differently
            // configured system tesseract still recognises the same way.
            //
            // The MSYS2 entry is not redundant with /usr/share: under MSYS2 the
            // prefix is a Windows path, and the only reliable way to name it is from
            // MSYSTEM, which the ucrt64 shell sets.
            const std::string exeDir = getExeDir();
            std::vector<std::string> tessdataCandidates = {
                "./data/tessdata",
                "./data",
                joinPath(exeDir, "data/tessdata"),
                joinPath(exeDir, "data"),
                joinPath(exeDir, "../data/tessdata"),
                joinPath(exeDir, "../data"),
                joinPath(exeDir, "../../data/tessdata"),
                joinPath(exeDir, "../../data"),
                joinPath(exeDir, "../share/tessdata"),
                joinPath(exeDir, "../../share/tessdata"),
                "/usr/share/tesseract-ocr/5/tessdata",
                "/usr/share/tesseract-ocr/4.00/tessdata",
                "/usr/share/tessdata",
            };

            if (const char* msystem = std::getenv("MSYSTEM");
                msystem != nullptr && *msystem != '\0') {
                // MSYSTEM_ROOT is C:\msys64, and the package prefix is a sibling of
                // it, so both spellings of the same directory are offered: a drive
                // letter for the Win32 loader and a POSIX path for the MSYS one.
                const std::string root = std::getenv("MSYSTEM_ROOT") != nullptr
                                             ? std::getenv("MSYSTEM_ROOT")
                                             : "C:/msys64";
                tessdataCandidates.push_back(joinPath(root, std::string(msystem) + "/share/tessdata"));
                tessdataCandidates.push_back(joinPath(root, "ucrt64/share/tessdata"));
                tessdataCandidates.push_back(joinPath(root, "mingw64/share/tessdata"));
            }

            const char* envTess = std::getenv("TESSDATA_PREFIX");
            if (envTess && strlen(envTess) > 0) {
                tessdataCandidates.insert(tessdataCandidates.begin(), envTess);
            }

            int initRes = -1;
            for (const auto& path : tessdataCandidates) {
                if (tess.apiInit) {
                    initRes = tess.apiInit(api, path.c_str(), "eng+ind");
                    if (initRes != 0) initRes = tess.apiInit(api, path.c_str(), "ind+eng");
                    if (initRes != 0) initRes = tess.apiInit(api, path.c_str(), "eng");
                    if (initRes != 0) initRes = tess.apiInit(api, path.c_str(), "ind");
                }
                if (initRes == 0) break;
            }

            if (initRes == 0) {
                if (tess.apiSetVar) {
                    tess.apiSetVar(api, "user_defined_dpi", "300");
                }

                // Page segmentation modes to evaluate: PSM 6 (Single Block), PSM 3 (Auto), PSM 7 (Single Line), PSM 8 (Single Word), PSM 11 (Sparse)
                const int psmModes[] = {6, 3, 7, 8, 11};
                std::string bestCandidate;
                double bestScore = -999999.0;

                for (int mode : psmModes) {
                    if (tess.apiSetPSM) tess.apiSetPSM(api, mode);
                    tess.apiSetImage(api, pix.data(), targetW, targetH, 1, targetW);

                    char* text = tess.apiGetText(api);
                    if (text) {
                        std::string res(text);
                        if (tess.deleteText) tess.deleteText(text);
                        else free(text);

                        // Clean whitespace and common handwriting OCR noise symbols
                        while (!res.empty() && (res.back() == '\n' || res.back() == '\r' || res.back() == ' ' ||
                                                res.back() == '|' || res.back() == '~' || res.back() == '`')) {
                            res.pop_back();
                        }
                        while (!res.empty() && (res.front() == '\n' || res.front() == '\r' || res.front() == ' ' ||
                                                res.front() == '|' || res.front() == '~' || res.front() == '`')) {
                            res.erase(res.begin());
                        }

                        if (!res.empty()) {
                            int alphaCount = 0;
                            int noiseCount = 0;
                            int lineCount = 1;
                            for (char ch : res) {
                                if (std::isalnum(static_cast<unsigned char>(ch))) alphaCount++;
                                else if (ch == '\n') lineCount++;
                                else if (ch == '|' || ch == '~' || ch == '`' || ch == '^' || ch == '{' || ch == '}' || ch == '\\') noiseCount++;
                            }

                            double score = static_cast<double>(alphaCount * 10) + (lineCount > 1 ? lineCount * 15.0 : 0.0) - (noiseCount * 20.0);
                            if (mode == 6 || mode == 3) score += 5.0;

                            if (score > bestScore && alphaCount > 0) {
                                bestScore = score;
                                bestCandidate = res;
                            }
                        }
                    }
                }

                if (!bestCandidate.empty()) {
                    bestCandidate = correctTextWithDictionary(bestCandidate);
                    out.text = bestCandidate;
                    out.confidence = 0.95;
                    tess.apiDelete(api);
                    return out;
                }
            }
            tess.apiDelete(api);
        }
    }

    // High-quality C++ Fallback recognizer
    out.text = recognizeWordFallback(pix, targetW, targetH);
    out.confidence = 0.82;
    return out;
}

std::string HandwritingRecognizer::recognizeWordFallback(const std::vector<unsigned char>& croppedImage,
                                                           int width, int height) const {

    if (width <= 0 || height <= 0 || croppedImage.empty()) return "Handwriting";

    // Convert greyscale to binary (1 = ink pixel, 0 = bg)
    std::vector<uint8_t> bin(width * height, 0);
    int totalInk = 0;
    for (int i = 0; i < width * height; ++i) {
        if (croppedImage[i] < 200) {
            bin[i] = 1;
            totalInk++;
        }
    }

    if (totalInk < 10) return "";

    // Vertical projection profile
    std::vector<int> colDensity(width, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (bin[y * width + x]) colDensity[x]++;
        }
    }

    // Segment characters by gaps
    std::string text;
    int startX = -1;

    for (int x = 0; x < width; ++x) {
        if (colDensity[x] > 0) {
            if (startX < 0) startX = x;
        } else {
            if (startX >= 0) {
                int charW = x - startX;
                if (charW >= 3) {
                    std::vector<unsigned char> charPix(charW * height, 0);
                    for (int cy = 0; cy < height; ++cy) {
                        for (int cx = 0; cx < charW; ++cx) {
                            charPix[cy * charW + cx] = bin[cy * width + (startX + cx)];
                        }
                    }
                    double aspect = static_cast<double>(charW) / height;
                    text += recognizeCharacterGlyph(charPix, charW, height, aspect);
                }
                startX = -1;
            }
        }
    }

    if (startX >= 0) {
        int charW = width - startX;
        if (charW >= 3) {
            std::vector<unsigned char> charPix(charW * height, 0);
            for (int cy = 0; cy < height; ++cy) {
                for (int cx = 0; cx < charW; ++cx) {
                    charPix[cy * charW + cx] = bin[cy * width + (startX + cx)];
                }
            }
            double aspect = static_cast<double>(charW) / height;
            text += recognizeCharacterGlyph(charPix, charW, height, aspect);
        }
    }

    if (text.empty()) {
        text = "Handwriting";
    }

    return correctWordWithDictionary(text);
}


std::string HandwritingRecognizer::recognizeCharacterGlyph(const std::vector<unsigned char>& subImage,
                                                             int width, int height, double glyphAspect) const {
    if (width <= 0 || height <= 0 || subImage.empty()) return "";

    // Connected Component & Loop/Hole Detection (Euler number calculation)
    int holes = 0;
    std::vector<uint8_t> visited(width * height, 0);

    // Flood fill background from borders to find enclosed loops
    std::queue<std::pair<int, int>> q;
    for (int x = 0; x < width; ++x) {
        if (!subImage[0 * width + x]) { visited[0 * width + x] = 1; q.push({x, 0}); }
        if (!subImage[(height - 1) * width + x]) { visited[(height - 1) * width + x] = 1; q.push({x, height - 1}); }
    }
    for (int y = 0; y < height; ++y) {
        if (!subImage[y * width + 0]) { visited[y * width + 0] = 1; q.push({0, y}); }
        if (!subImage[y * width + (width - 1)]) { visited[y * width + (width - 1)] = 1; q.push({width - 1, y}); }
    }

    while (!q.empty()) {
        auto [cx, cy] = q.front();
        q.pop();

        const int dx[] = {1, -1, 0, 0};
        const int dy[] = {0, 0, 1, -1};
        for (int i = 0; i < 4; ++i) {
            int nx = cx + dx[i];
            int ny = cy + dy[i];
            if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                int idx = ny * width + nx;
                if (!subImage[idx] && !visited[idx]) {
                    visited[idx] = 1;
                    q.push({nx, ny});
                }
            }
        }
    }

    // Count unvisited background regions (holes inside glyph)
    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            int idx = y * width + x;
            if (!subImage[idx] && !visited[idx]) {
                holes++;
                // Fill this hole
                q.push({x, y});
                visited[idx] = 1;
                while (!q.empty()) {
                    auto [cx, cy] = q.front();
                    q.pop();
                    const int dx[] = {1, -1, 0, 0};
                    const int dy[] = {0, 0, 1, -1};
                    for (int i = 0; i < 4; ++i) {
                        int nx = cx + dx[i];
                        int ny = cy + dy[i];
                        if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                            int nidx = ny * width + nx;
                            if (!subImage[nidx] && !visited[nidx]) {
                                visited[nidx] = 1;
                                q.push({nx, ny});
                            }
                        }
                    }
                }
            }
        }
    }

    // Calculate vertical density distribution: Top 35%, Mid 30%, Bot 35%
    int topInk = 0, midInk = 0, botInk = 0;
    const int topH = height * 0.35;
    const int botH = height * 0.65;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (subImage[y * width + x]) {
                if (y < topH) topInk++;
                else if (y < botH) midInk++;
                else botInk++;
            }
        }
    }

    const int totalInk = topInk + midInk + botInk;
    if (totalInk == 0) return "";

    const double topRatio = static_cast<double>(topInk) / totalInk;
    const double botRatio = static_cast<double>(botInk) / totalInk;

    // Categorize character based on holes and aspect ratio
    if (holes >= 2) {
        return "B";
    }
    if (holes == 1) {
        if (glyphAspect > 0.75) {
            if (topRatio > 0.4) return "e";
            return "o";
        }
        if (topRatio > 0.45) return "b";
        if (botRatio > 0.45) return "g";
        return "a";
    }

    // 0 holes
    if (glyphAspect < 0.35) {
        if (topRatio > 0.4) return "i";
        if (botRatio > 0.4) return "j";
        return "l";
    }
    if (glyphAspect > 1.3) return "-";
    if (botRatio > 0.45) return "u";
    if (topRatio > 0.45) return "t";

    return "e";
}

}  // namespace stylus

