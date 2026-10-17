#pragma once

#include <sys/types.h>
#include <string>

// The guest still names the old external cache root. These helpers first let the
// real file system satisfy a mod override, then map a packaged game-data path to
// the flat assets/ tree in the APK.
bool SetGameDataRoots(const std::string& main_root, const std::string& patch_root,
                      const std::string& selected_module_root = {});
int GameDataOpenEntry(const char* guest_path, off_t* out_size);
bool GameDataStatEntry(const char* guest_path, off_t* out_size, bool* out_is_dir);

// The mounted external root game paths are built from, without a trailing
// slash. Empty until SetGameDataRoots succeeds.
std::string GameDataMainRoot();
