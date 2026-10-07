#pragma once

#include <string>
#include <vector>

namespace stylus {

// The platform bits the drawing code cannot avoid, in one place.
//
// Everything here had a POSIX-only spelling that appeared in exactly two files:
// main.cpp wanted to find its own executable to locate the icon theme, and ocr.cpp
// wanted to dlopen libtesseract and to find the user's config directory. Neither
// concern is drawing code, so both moved behind these three functions rather than
// being guarded with #ifdef at each call site -- a second #ifdef per call is how a
// portable path ends up tested on one platform only.
//
// Path separators are always '/' in what these return, on every platform. Win32
// accepts them, and it means a caller can concatenate without knowing where it is.

// Directory holding the running executable, or "" if it cannot be determined.
std::string executableDirectory();

// Where this user's settings for `appName` belong. On Linux that is
// $XDG_CONFIG_HOME or ~/.config; on Windows %APPDATA%. Created if absent, so a
// caller can write into it directly. Returns "" only when no such directory can be
// made, in which case the caller should fall back to a bundled path.
std::string userConfigDirectory(const std::string& appName);

// Creates `path` and any missing parents. Returns false when it could not, which
// the callers treat as "this location is unusable", never as a fatal error.
bool createDirectory(const std::string& path);

// Joins a directory and a leaf with a single separator, and normalises any
// duplicate separators at the join.
std::string joinPath(const std::string& dir, const std::string& leaf);

// A dynamically loaded shared library, opened by name at runtime.
//
// Tesseract is bound this way rather than linked so the app still runs, and the
// drawing still works, on a machine with no OCR engine installed. That makes "the
// library is not there" a normal state rather than a link error, so it has to be
// handled as one.
class SharedLibrary {
public:
    SharedLibrary() = default;
    ~SharedLibrary();

    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    // Tries each name in turn and keeps the first that opens. Also tries each name
    // resolved against the running executable's own directory, which is what makes a
    // portable install work when the DLL sits beside the exe and the installer's
    // directory is not on PATH.
    bool openAny(const std::vector<std::string>& names);

    void* symbol(const char* name) const;
    bool isOpen() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

}  // namespace stylus
