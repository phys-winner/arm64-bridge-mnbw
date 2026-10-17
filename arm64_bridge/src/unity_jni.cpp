// Registry of guest JNI natives learned from RegisterNatives.

#include "unity_jni.h"

#include <android/log.h>

#include <map>
#include <mutex>

#include "dynarmic_host.h"
#include "elf_loader.h"

#define LOG_TAG "CMR_UnityJNI"
#define LOGI(...) ((void)0)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::mutex g_mutex;
std::map<std::string, uint32_t> g_natives;
std::map<std::string, std::string> g_name_to_key;
bool g_registry_valid = true;

std::string NativeKey(const std::string& class_name, const std::string& name,
                      const std::string& signature) {
    return class_name + "\n" + name + "\n" + signature;
}

}  // namespace

void RecordGuestNative(const std::string& class_name, const std::string& name,
                       const std::string& signature, uint32_t guest_fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string key = NativeKey(class_name, name, signature);
    auto existing = g_natives.find(key);
    if (existing != g_natives.end()) {
        if (existing->second != guest_fn) {
            LOGE("duplicate guest native %s.%s%s: 0x%08x vs 0x%08x", class_name.c_str(),
                 name.c_str(), signature.c_str(), existing->second, guest_fn);
            g_registry_valid = false;
        }
        return;
    }

    auto by_name = g_name_to_key.find(name);
    if (by_name != g_name_to_key.end() && by_name->second != key) {
        LOGE("ambiguous guest native name '%s'", name.c_str());
        g_registry_valid = false;
    } else {
        g_name_to_key[name] = key;
    }

    g_natives[key] = guest_fn;
    LOGI("registered %s.%s%s -> 0x%08x", class_name.c_str(), name.c_str(), signature.c_str(),
         guest_fn);
}

uint32_t GuestNative(const std::string& class_name, const std::string& name,
                     const std::string& signature) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_natives.find(NativeKey(class_name, name, signature));
    return it == g_natives.end() ? 0 : it->second;
}

uint32_t GuestNative(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto by_name = g_name_to_key.find(name);
    if (by_name == g_name_to_key.end()) return 0;
    auto native = g_natives.find(by_name->second);
    return native == g_natives.end() ? 0 : native->second;
}

bool GuestNativeRegistryValid() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_registry_valid;
}

void RunGuestJniOnLoad(DynarmicHost& host, uint32_t guest_vm) {
    for (const char* soname : {"libmain.so", "libunity.so"}) {
        ElfLoader* lib = host.GetLibrary(soname);
        if (!lib) continue;
        uint32_t fn = lib->GetSymbolAddress("JNI_OnLoad");
        if (!fn) continue;
        LOGI("calling %s JNI_OnLoad at 0x%08x", soname, fn);
        uint32_t version = host.RunFunction(fn, {guest_vm, 0});
        LOGI("%s JNI_OnLoad returned 0x%08x", soname, version);
        if (version == 0) g_registry_valid = false;
    }
}
