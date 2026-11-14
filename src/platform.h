// The few things that differ between macOS and Windows.
#pragma once

#include <filesystem>
#include <vector>

#if defined(__APPLE__)
#define AMENITY_CMD "Cmd+"
#define AMENITY_ALT "Option"
#define AMENITY_TRASH "Trash"
#else
#define AMENITY_CMD "Ctrl+"
#define AMENITY_ALT "Alt"
#define AMENITY_TRASH "Recycle Bin"
#endif

namespace platform {

std::filesystem::path homeDir();

// Moves files to the Trash (the Recycle Bin on Windows). On macOS, files after the first
// are renamed to match it if a name is taken, so a sound and its arrangement stay together.
bool moveToTrash(const std::vector<std::filesystem::path>& paths);

// A monospace system font for the UI, or empty to use ImGui's built-in one.
std::filesystem::path uiFontPath();

// Run once at startup.
void init();

}  // namespace platform
