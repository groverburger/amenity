#include "platform.h"

#include <cstdlib>
#include <string>
#include <system_error>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace platform {

fs::path homeDir() {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return fs::path(home ? home : ".");
}

bool moveToTrash(const std::vector<fs::path>& paths) {
    if (paths.empty()) return true;
#if defined(_WIN32)
    // A list of names, each ending in a null, with an extra null at the end.
    std::wstring from;
    for (const fs::path& path : paths) {
        from += fs::absolute(path).wstring();
        from.push_back(L'\0');
    }
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
#else
    const fs::path trash = homeDir() / ".Trash";
    const fs::path& first = paths.front();
    fs::path stem = first.stem();
    auto taken = [&](const fs::path& name) {
        for (const fs::path& path : paths) {
            if (fs::exists(trash / (name.string() + path.extension().string()))) return true;
        }
        return false;
    };
    for (int n = 2; taken(stem); ++n) stem = first.stem().string() + " " + std::to_string(n);
    bool ok = true;
    for (const fs::path& path : paths) {
        std::error_code error;
        fs::rename(path, trash / (stem.string() + path.extension().string()), error);
        ok = ok && !error;
    }
    return ok;
#endif
}

fs::path uiFontPath() {
#if defined(__APPLE__)
    const fs::path font = "/System/Library/Fonts/Menlo.ttc";
#elif defined(_WIN32)
    const char* windows = std::getenv("WINDIR");
    const fs::path font = fs::path(windows ? windows : "C:\\Windows") / "Fonts" / "consola.ttf";
#else
    const fs::path font;
#endif
    std::error_code error;
    return !font.empty() && fs::exists(font, error) ? font : fs::path();
}

void init() {
#if defined(__APPLE__)
    // Stop macOS from showing the accent popup when a key is held down.
    CFPreferencesSetAppValue(CFSTR("ApplePressAndHoldEnabled"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
#endif
}

}  // namespace platform
