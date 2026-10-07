#include "platform.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace stylus {

namespace {

#ifdef _WIN32

// GetModuleFileNameW truncates rather than telling you it truncated, so it is
// called with a generous buffer and retried with a larger one if the result fills
// it. Returns a UTF-8 string, which is what every caller in this codebase works in.
std::string moduleFilePath() {
    std::wstring buffer(1024, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, &buffer[0],
                                             static_cast<DWORD>(buffer.size()));
        if (len == 0) return std::string();
        // On success the return value excludes the terminator, so len == size means
        // the path may have been cut. Anything less fits.
        if (len < buffer.size()) {
            buffer.resize(len);
            break;
        }
        if (buffer.size() >= 65536) return std::string();
        buffer.resize(buffer.size() * 2);
    }

    const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), -1, nullptr, 0, nullptr,
                                           nullptr);
    if (needed <= 1) return std::string();
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), -1, &out[0], needed, nullptr, nullptr);
    return out;
}

std::string parentDirectory(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

#else

std::string moduleFilePath() {
    // /proc/self/exe is a symlink to the running binary, and unlike argv[0] it
    // cannot be wrong: argv[0] is whatever the caller chose to call it, which for
    // a launcher or a PATH lookup is not the file's own location.
    std::string buffer(4096, '\0');
    const ssize_t len = readlink("/proc/self/exe", &buffer[0], buffer.size() - 1);
    if (len <= 0) return std::string();
    buffer.resize(static_cast<size_t>(len));
    return buffer;
}

std::string parentDirectory(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

#endif

// Strips trailing separators, so joining onto a directory that already ends in one
// does not produce a doubled separator. "/" is left alone: trimming it to the empty
// string would turn the root directory into a relative path.
std::string trimTrailingSeparators(const std::string& dir) {
    std::string out = dir;
    while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) out.pop_back();
    return out;
}

}  // namespace

std::string executableDirectory() {
    const std::string path = moduleFilePath();
    if (path.empty()) return std::string();
    return trimTrailingSeparators(parentDirectory(path));
}

std::string joinPath(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    const std::string trimmed = trimTrailingSeparators(dir);
    if (leaf.empty()) return trimmed;
    if (leaf.front() == '/' || leaf.front() == '\\') return leaf;
    return trimmed + "/" + leaf;
}

bool createDirectory(const std::string& path) {
    if (path.empty()) return false;

#ifdef _WIN32
    // Create one level at a time rather than relying on a recursive mkdir, which
    // MinGW does not have. Every existing parent is fine: CreateDirectory reports
    // "already exists" as a failure with ERROR_ALREADY_EXISTS, which is success here.
    std::string partial;
    size_t pos = 0;
    // A leading separator or a drive letter is not a level to create.
    if (path.size() >= 2 && path[1] == ':') {
        partial = path.substr(0, 2);
        pos = 2;
        if (pos < path.size() && (path[pos] == '/' || path[pos] == '\\')) {
            partial += '/';
            ++pos;
        }
    } else if (path[0] == '/' || path[0] == '\\') {
        partial = "/";
        pos = 1;
    }

    while (pos <= path.size()) {
        const size_t next = path.find_first_of("/\\", pos);
        const size_t end = next == std::string::npos ? path.size() : next;
        if (end > pos) {
            partial += path.substr(pos, end - pos);
            CreateDirectoryA(partial.c_str(), nullptr);
            partial += '/';
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return true;
#else
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
#endif
}

std::string userConfigDirectory(const std::string& appName) {
#ifdef _WIN32
    // APPDATA rather than USERPROFILE: it is the roaming profile, which is what a
    // user's own settings belong in, and it is the variable Windows itself uses for
    // per-application settings. USERPROFILE is the whole account folder and writing
    // app state there is what installers do when they go wrong.
    const char* base = std::getenv("APPDATA");
    if (base == nullptr || *base == '\0') return std::string();
    const std::string dir = joinPath(base, appName);
    if (!createDirectory(dir)) return std::string();
    return dir;
#else
    // $HOME/.config, not XDG_CONFIG_HOME, and that is deliberate: it is the path
    // this app has always used, so a user who has learned handwriting words already
    // has them in ~/.config/stylus-pen. Honouring XDG_CONFIG_HOME here would silently
    // move that file and start every existing user from an empty dictionary.
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return std::string();
    const std::string dir = joinPath(joinPath(home, ".config"), appName);
    if (!createDirectory(dir)) return std::string();
    return dir;
#endif
}

SharedLibrary::~SharedLibrary() {
    if (handle_ == nullptr) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
}

bool SharedLibrary::openAny(const std::vector<std::string>& names) {
    if (!handle_) return true;

    // Beside the executable first. A portable install drops its DLLs next to the
    // exe and ships without an installer, so nothing has put that directory on
    // PATH -- and the whole point of loading by name is that a missing engine
    // leaves the app working, which means it cannot also require a PATH edit.
    const std::string exeDir = executableDirectory();

    for (const std::string& name : names) {
#ifdef _WIN32
        if (!exeDir.empty()) {
            const std::string beside = joinPath(exeDir, name);
            if (HMODULE module = LoadLibraryA(beside.c_str())) {
                handle_ = module;
                return true;
            }
        }
        if (HMODULE module = LoadLibraryA(name.c_str())) {
            handle_ = module;
            return true;
        }
#else
        if (!exeDir.empty()) {
            const std::string beside = joinPath(exeDir, name);
            if (void* lib = dlopen(beside.c_str(), RTLD_LAZY)) {
                handle_ = lib;
                return true;
            }
        }
        if (void* lib = dlopen(name.c_str(), RTLD_LAZY)) {
            handle_ = lib;
            return true;
        }
#endif
    }
    return false;
}

void* SharedLibrary::symbol(const char* name) const {
    if (handle_ == nullptr) return nullptr;
#ifdef _WIN32
    // GetProcAddress returns a function pointer, not a data pointer. The round trip
    // through a uintptr_t is the standard way to hand that back as void* without
    // a warning or a strict-aliasing violation; it is only valid because the caller
    // casts it straight back to the matching function pointer type.
    return reinterpret_cast<void*>(
        static_cast<uintptr_t>(GetProcAddress(static_cast<HMODULE>(handle_), name)));
#else
    return dlsym(handle_, name);
#endif
}

}  // namespace stylus
