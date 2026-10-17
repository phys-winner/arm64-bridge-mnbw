#include "game_data_vfs.h"

#include "apk_reader.h"
#include "mod_texture_bridge.h"

#include <android/log.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <fcntl.h>
#include <mutex>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {
#define LOG_TAG "WarbandModuleVfs"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

std::mutex g_mutex;
std::string g_main_root;
std::string g_patch_root;
std::string g_selected_module_root;

bool ValidRoot(const std::string& root) {
    if (root.empty() || root.front() != '/' || root.back() == '/' || root.back() == '\\' || root.size() >= 512) {
        return false;
    }
    for (size_t i = 1; i < root.size();) {
        size_t end = root.find('/', i);
        if (end == std::string::npos) end = root.size();
        if (end == i || (end - i == 1 && root[i] == '.') ||
            (end - i == 2 && root.compare(i, 2, "..") == 0)) {
            return false;
        }
        for (size_t j = i; j < end; ++j) {
            unsigned char c = static_cast<unsigned char>(root[j]);
            if (c < 0x20 || c == 0x7f || c == '\\') return false;
        }
        i = end == root.size() ? end : end + 1;
    }
    return true;
}

bool RelativeForRoot(const char* path, const std::string& root, std::string& relative) {
    if (!path || root.empty()) return false;
    const size_t path_len = strnlen(path, 4096);
    if (path_len == 4096 || path_len < root.size() ||
        root.compare(0, root.size(), path, root.size()) != 0 ||
        (path_len != root.size() && path[root.size()] != '/')) {
        return false;
    }
    size_t start = path_len == root.size() ? path_len : root.size() + 1;
    // The guest texture scanner stores an absolute module search path and its
    // loader later prefixes the mounted root once more, producing
    // "<root>/<root>/Modules/..." (the separator is doubled because root is
    // absolute). Treat repeated, boundary-aligned copies as one root.
    while (start + root.size() <= path_len &&
           root.compare(0, root.size(), path + start, root.size()) == 0 &&
           (start + root.size() == path_len ||
            path[start + root.size()] == '/')) {
        start += root.size();
        if (start < path_len) ++start;
    }
    relative.assign(path + start, path_len - start);
    if (relative.empty() || relative.size() > 4095 || relative.front() == '/' || relative.back() == '/') {
        return false;
    }
    for (size_t i = 0; i < relative.size();) {
        size_t end = relative.find('/', i);
        if (end == std::string::npos) end = relative.size();
        if (end == i || (end - i == 1 && relative[i] == '.') ||
            (end - i == 2 && relative.compare(i, 2, "..") == 0)) {
            return false;
        }
        for (size_t j = i; j < end; ++j) {
            unsigned char c = static_cast<unsigned char>(relative[j]);
            if (c < 0x20 || c == 0x7f || c == '\\') return false;
        }
        i = end == relative.size() ? end : end + 1;
    }
    return true;
}

bool AssetNameFor(const char* path, std::string& asset_name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string relative;
    if (!RelativeForRoot(path, g_main_root, relative) &&
        !RelativeForRoot(path, g_patch_root, relative)) {
        return false;
    }
    asset_name = "assets/" + relative;
    return true;
}

bool ModulePathFor(const char* path, std::string& module_root, std::string& candidate) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_selected_module_root.empty()) return false;
    std::string relative;
    if (!RelativeForRoot(path, g_main_root, relative) &&
        !RelativeForRoot(path, g_patch_root, relative)) {
        return false;
    }
    constexpr std::string_view kNativePrefix = "Modules/Native";
    size_t suffix = 0u;
    if (relative == kNativePrefix ||
        (relative.size() > kNativePrefix.size() &&
         relative.compare(0, kNativePrefix.size(), kNativePrefix) == 0 &&
         relative[kNativePrefix.size()] == '/')) {
        suffix = relative.size() == kNativePrefix.size()
            ? relative.size() : kNativePrefix.size() + 1u;
    } else {
        // Warband resolves shared assets from top-level roots after discovering
        // them under Modules/<name>. Overlay only the data families a desktop
        // module is allowed to contribute; shaders and executable content stay
        // in the signed APK.
        constexpr std::string_view kOverlayRoots[] = {
            "Textures", "Resource", "Music", "music", "Sounds", "languages",
            "SceneObj", "Data",
        };
        bool overlay = false;
        for (const std::string_view prefix : kOverlayRoots) {
            if (relative == prefix ||
                (relative.size() > prefix.size() &&
                 relative.compare(0, prefix.size(), prefix) == 0 &&
                 relative[prefix.size()] == '/')) {
                overlay = true;
                break;
            }
        }
        if (!overlay) return false;
        suffix = 0u;
    }
    module_root = g_selected_module_root;
    candidate = module_root;
    if (suffix < relative.size()) candidate += "/" + relative.substr(suffix);
    return true;
}

bool ResolveConfined(const std::string& root, const std::string& candidate,
                     std::string& resolved) {
    char buffer[PATH_MAX] = {};
    if (!realpath(candidate.c_str(), buffer)) return false;
    resolved.assign(buffer);
    if (resolved == root) return true;
    return resolved.size() > root.size() &&
           resolved.compare(0, root.size(), root) == 0 &&
           resolved[root.size()] == '/';
}
}  // namespace

bool SetGameDataRoots(const std::string& main_root, const std::string& patch_root,
                      const std::string& selected_module_root) {
    if (!ValidRoot(main_root) || !ValidRoot(patch_root) ||
        (!selected_module_root.empty() && !ValidRoot(selected_module_root))) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_main_root = main_root;
    g_patch_root = patch_root;
    g_selected_module_root = selected_module_root;
    LOGI("roots main='%s' patch='%s' module='%s'", g_main_root.c_str(),
         g_patch_root.c_str(), g_selected_module_root.c_str());
    return true;
}

std::string GameDataMainRoot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_main_root;
}

int GameDataOpenEntry(const char* guest_path, off_t* out_size) {
    std::string module_root;
    std::string candidate;
    std::string resolved;
    if (ModulePathFor(guest_path, module_root, candidate) &&
        ResolveConfined(module_root, candidate, resolved)) {
        LOGI("open override '%s' -> '%s'", guest_path, resolved.c_str());
        bool texture_handled = false;
        int texture_fd = OpenModDdsVirtualized(resolved.c_str(), out_size, &texture_handled);
        if (texture_handled) return texture_fd;
        int fd = open(resolved.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            struct stat st {};
            if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
                if (out_size) *out_size = st.st_size;
                return fd;
            }
            close(fd);
        }
    } else if (!module_root.empty()) {
        LOGI("open override missing '%s' -> '%s'", guest_path,
             candidate.c_str());
    }
    std::string asset_name;
    if (!AssetNameFor(guest_path, asset_name)) return -1;
    return ApkOpenEntry(asset_name.c_str(), out_size);
}

bool GameDataStatEntry(const char* guest_path, off_t* out_size, bool* out_is_dir) {
    std::string module_root;
    std::string candidate;
    std::string resolved;
    if (ModulePathFor(guest_path, module_root, candidate) &&
        ResolveConfined(module_root, candidate, resolved)) {
        LOGI("stat override '%s' -> '%s'", guest_path, resolved.c_str());
        bool texture_handled = false;
        off_t texture_size = 0;
        if (StatModDdsVirtualized(resolved.c_str(), &texture_size,
                                  &texture_handled)) {
            if (out_size) *out_size = texture_size;
            if (out_is_dir) *out_is_dir = false;
            return true;
        }
        if (texture_handled) return false;
        struct stat st {};
        if (stat(resolved.c_str(), &st) == 0) {
            if (out_size) *out_size = st.st_size;
            if (out_is_dir) *out_is_dir = S_ISDIR(st.st_mode);
            return S_ISREG(st.st_mode) || S_ISDIR(st.st_mode);
        }
    } else if (!module_root.empty()) {
        LOGI("stat override missing '%s' -> '%s'", guest_path,
             candidate.c_str());
    }
    std::string asset_name;
    if (!AssetNameFor(guest_path, asset_name)) return false;
    return ApkStatEntry(asset_name.c_str(), out_size, out_is_dir);
}
