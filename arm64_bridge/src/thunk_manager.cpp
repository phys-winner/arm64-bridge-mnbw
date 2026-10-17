#include "thunk_manager.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "bridge_config.h"
#include "guest_signals.h"
#include "thunks_opensles.h"
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>
#include <unistd.h>
#include "thunk_types.h"
#include "unity_jni.h"
#include <android/log.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>

#define LOG_TAG "CMR_ThunkManager"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

void RegisterGles2CoreThunks(ThunkManager& manager);

uint32_t ThunkManager::s_screenWidth = 1213;
uint32_t ThunkManager::s_screenHeight = 540;
uint32_t ThunkManager::s_forced_dpi = 0;
uint32_t ThunkManager::s_ui_inset = 0;

ThunkManager::ThunkManager(DynarmicHost& host) : m_host(host) {}

ThunkManager::~ThunkManager() = default;


void ThunkManager::RegisterAllThunks() {
    RegisterGLThunks();
    RegisterLibcThunks();
    RegisterExtraLibcThunks();
    RegisterNetworkThunks();
    RegisterZlibThunks();
    RegisterFmodHostDecodeThunks();
    RegisterEglThunks();
    RegisterAndroidThunks();
    RegisterWarbandExtraThunks();
    // This table intentionally supplies only missing GLES2 entry points. Run it
    // after every canonical GL/EGL provider so a later specialised wrapper can
    // never become a duplicate merely because the fallback registered first.
    RegisterGles2CoreThunks(*this);
    RegisterOpenSLESThunks(*this, m_host);
    RegisterDataSymbols();
    InstallFastMemcpy();
    SetupGuestJNIEnv();
}

#include <jni.h>
#include <sys/system_properties.h>
#include <cmath>
#include <cctype>
#include <cstring>

JavaVM* GetHostJavaVM();

static std::string QueryDeviceLanguage() {
    std::string lang = "en";
    JavaVM* jvm = GetHostJavaVM();
    if (jvm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
        if (env) {
            jclass locCls = env->FindClass("java/util/Locale");
            if (locCls) {
                jmethodID getDefault = env->GetStaticMethodID(locCls, "getDefault", "()Ljava/util/Locale;");
                if (getDefault) {
                    jobject locObj = env->CallStaticObjectMethod(locCls, getDefault);
                    if (locObj) {
                        jmethodID getLang = env->GetMethodID(locCls, "getLanguage", "()Ljava/lang/String;");
                        if (getLang) {
                            jstring jStr = (jstring)env->CallObjectMethod(locObj, getLang);
                            if (jStr) {
                                const char* utf = env->GetStringUTFChars(jStr, nullptr);
                                if (utf && strlen(utf) > 0) {
                                    lang = utf;
                                }
                                if (utf) env->ReleaseStringUTFChars(jStr, utf);
                            }
                        }
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (attached) jvm->DetachCurrentThread();
        }
    }
    if (lang.empty() || lang == "en") {
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("persist.sys.locale", prop) > 0 ||
            __system_property_get("ro.product.locale", prop) > 0 ||
            __system_property_get("persist.sys.language", prop) > 0) {
            char* dash = strchr(prop, '-');
            if (dash) *dash = '\0';
            char* under = strchr(prop, '_');
            if (under) *under = '\0';
            if (strlen(prop) > 0) lang = prop;
        }
    }
    for (char& c : lang) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return lang;
}

static std::string QueryDeviceCountryCode() {
    std::string country = "US";
    JavaVM* jvm = GetHostJavaVM();
    if (jvm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
        if (env) {
            jclass locCls = env->FindClass("java/util/Locale");
            if (locCls) {
                jmethodID getDefault = env->GetStaticMethodID(locCls, "getDefault", "()Ljava/util/Locale;");
                if (getDefault) {
                    jobject locObj = env->CallStaticObjectMethod(locCls, getDefault);
                    if (locObj) {
                        jmethodID getCountry = env->GetMethodID(locCls, "getCountry", "()Ljava/lang/String;");
                        if (getCountry) {
                            jstring jStr = (jstring)env->CallObjectMethod(locObj, getCountry);
                            if (jStr) {
                                const char* utf = env->GetStringUTFChars(jStr, nullptr);
                                if (utf && strlen(utf) > 0) {
                                    country = utf;
                                }
                                if (utf) env->ReleaseStringUTFChars(jStr, utf);
                            }
                        }
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (attached) jvm->DetachCurrentThread();
        }
    }
    if (country.empty() || country == "US") {
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("persist.sys.country", prop) > 0 ||
            __system_property_get("ro.product.locale.region", prop) > 0) {
            if (strlen(prop) > 0) country = prop;
        } else if (__system_property_get("persist.sys.locale", prop) > 0 ||
                   __system_property_get("ro.product.locale", prop) > 0) {
            char* dash = strchr(prop, '-');
            if (dash && strlen(dash + 1) >= 2) {
                country = std::string(dash + 1, 2);
            } else {
                char* under = strchr(prop, '_');
                if (under && strlen(under + 1) >= 2) {
                    country = std::string(under + 1, 2);
                }
            }
        }
    }
    for (char& c : country) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return country;
}

static uint32_t QueryDeviceDensity() {
    if (ThunkManager::s_forced_dpi > 0) {
        return ThunkManager::s_forced_dpi;
    }
    BridgeConfig& ext = GetBridgeConfig();
    if (ext.forced_dpi > 0) {
        return ext.forced_dpi;
    }
    uint32_t density = 0;
    JavaVM* jvm = GetHostJavaVM();
    if (jvm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
        if (env) {
            jclass resCls = env->FindClass("android/content/res/Resources");
            if (resCls) {
                jmethodID getSys = env->GetStaticMethodID(resCls, "getSystem", "()Landroid/content/res/Resources;");
                if (getSys) {
                    jobject resObj = env->CallStaticObjectMethod(resCls, getSys);
                    if (resObj) {
                        jmethodID getDM = env->GetMethodID(resCls, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
                        if (getDM) {
                            jobject dmObj = env->CallObjectMethod(resObj, getDM);
                            if (dmObj) {
                                jclass dmCls = env->FindClass("android/util/DisplayMetrics");
                                jfieldID fid = env->GetFieldID(dmCls, "densityDpi", "I");
                                if (fid) {
                                    density = static_cast<uint32_t>(env->GetIntField(dmObj, fid));
                                }
                            }
                        }
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (attached) jvm->DetachCurrentThread();
        }
    }
    if (density == 0) {
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("ro.sf.lcd_density", prop) > 0 ||
            __system_property_get("qemu.sf.lcd_density", prop) > 0) {
            density = static_cast<uint32_t>(atoi(prop));
        }
    }
    if (density == 0) {
        density = 320;
    }
    return density;
}

static uint32_t QueryDeviceIsPad() {
    JavaVM* jvm = GetHostJavaVM();
    if (jvm) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
        }
        if (env) {
            jclass resCls = env->FindClass("android/content/res/Resources");
            if (resCls) {
                jmethodID getSys = env->GetStaticMethodID(resCls, "getSystem", "()Landroid/content/res/Resources;");
                if (getSys) {
                    jobject resObj = env->CallStaticObjectMethod(resCls, getSys);
                    if (resObj) {
                        jmethodID getCfg = env->GetMethodID(resCls, "getConfiguration", "()Landroid/content/res/Configuration;");
                        if (getCfg) {
                            jobject cfgObj = env->CallObjectMethod(resObj, getCfg);
                            if (cfgObj) {
                                jclass cfgCls = env->FindClass("android/content/res/Configuration");
                                jfieldID slFid = env->GetFieldID(cfgCls, "screenLayout", "I");
                                if (slFid) {
                                    jint screenLayout = env->GetIntField(cfgObj, slFid);
                                    if ((screenLayout & 0x0F) >= 3) { // SCREENLAYOUT_SIZE_LARGE or XLARGE
                                        if (env->ExceptionCheck()) env->ExceptionClear();
                                        if (attached) jvm->DetachCurrentThread();
                                        return 1;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (attached) jvm->DetachCurrentThread();
        }
    }

    uint32_t w = ThunkManager::s_screenWidth;
    uint32_t h = ThunkManager::s_screenHeight;
    uint32_t dpi = QueryDeviceDensity();
    if (w > 0 && h > 0 && dpi > 0) {
        double w_in = static_cast<double>(w) / static_cast<double>(dpi);
        double h_in = static_cast<double>(h) / static_cast<double>(dpi);
        double diag = sqrt(w_in * w_in + h_in * h_in);
        if (diag >= 6.5) return 1;
    }
    return 0;
}




namespace {
// Both live with the rest of the diagnostic state further down; declared here so
// registration can publish into them.
void PublishFastThunk(uint32_t id, const ThunkHandler* handler);
constexpr size_t kFastThunkSlots = 8192;
std::atomic<const ThunkHandler*> g_fast_handler[kFastThunkSlots];
}  // namespace

void ThunkManager::RegisterThunk(const std::string& name, ThunkHandler handler) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_symbol_to_address.count(name)) {
        LOGE("duplicate provider for '%s'", name.c_str());
        m_registration_error = true;
        return;
    }
    uint32_t thunk_id = static_cast<uint32_t>(m_id_to_handler.size() + 1);
    uint32_t thunk_addr = m_next_thunk_addr;
    m_next_thunk_addr += 8; // 2 instructions per trampoline

    // Write ARM32 trampoline instructions into guest memory:
    // 0: SVC #thunk_id (0xEF000000 | thunk_id)
    // 4: BX LR (0xE12FFF1E)
    uint32_t svc_instr = 0xEF000000 | (thunk_id & 0x00FFFFFF);
    uint32_t bx_lr_instr = 0xE12FFF1E;

    m_host.MemoryWrite32(thunk_addr, svc_instr);
    m_host.MemoryWrite32(thunk_addr + 4, bx_lr_instr);
    // Trampolines are also created at runtime (dlsym, OpenSL ES vtables), so the JIT may
    // already hold a translation of this address range.
    m_host.InvalidateGuestCode(thunk_addr, 8);

    m_symbol_to_address[name] = thunk_addr;
    m_id_to_handler[thunk_id] = std::move(handler);
    m_id_to_name[thunk_id] = name;
    // unordered_map nodes never move and nothing is erased, so both the handler
    // and the name string keep these addresses for the life of the process.
    PublishFastThunk(thunk_id, &m_id_to_handler[thunk_id]);
}

void ThunkManager::RegisterSymbol(const std::string& name, uint32_t address) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_symbol_to_address.count(name)) {
        LOGE("duplicate provider for '%s'", name.c_str());
        m_registration_error = true;
        return;
    }
    m_symbol_to_address[name] = address;
}

bool ThunkManager::IsRegistered(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_symbol_to_address.find(name) != m_symbol_to_address.end();
}

uint32_t ThunkManager::GetOrCreateThunkAddress(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_symbol_to_address.find(name);
    if (it != m_symbol_to_address.end()) {
        return it->second;
    }
    RegisterThunk(name, [this, name](Dynarmic::A32::Jit& jit, DynarmicHost&) {
        m_diagnostic_trap_executed = true;
        LOGE("diagnostic trap executed for unresolved function '%s'", name.c_str());
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
    });
    return m_symbol_to_address[name];
}

namespace {

bool LooksLikeReturnAddress(DynarmicHost& host, uint32_t addr) {
    if (addr < 0x40000000u || addr >= 0x47000000u) return false;
    uint32_t arm = host.MemoryRead32(addr - 4);
    if ((arm & 0x0F000000u) == 0x0B000000u && (arm >> 28) != 0xF) return true;
    if ((arm & 0xFE000000u) == 0xFA000000u) return true;
    if ((arm & 0x0FFFFFF0u) == 0x012FFF30u) return true;
    uint16_t first = static_cast<uint16_t>(arm & 0xFFFF);
    uint16_t second = static_cast<uint16_t>(arm >> 16);
    return (first >= 0xF000 && first <= 0xF7FF && second >= 0xE800) ||
           ((second & 0xFF80) == 0x4780);
}

void PublishFastThunk(uint32_t id, const ThunkHandler* handler) {
    if (id < kFastThunkSlots)
        g_fast_handler[id].store(handler, std::memory_order_release);
}

}  // namespace

int GuestBacktrace(DynarmicHost& host, uint32_t sp, uint32_t lr, uint32_t* out, int max_out) {
    int found = 0;
    if (max_out <= 0) return 0;
    out[found++] = lr;
    for (int i = 0; i < 256 && found < max_out; ++i) {
        uint32_t word = host.MemoryRead32(sp + i * 4);
        if (LooksLikeReturnAddress(host, word)) out[found++] = word;
    }
    for (int i = found; i < max_out; ++i) out[i] = 0;
    return found;
}

const ThunkHandler* ThunkManager::ResolveThunkHandler(uint32_t thunk_id) {
    const ThunkHandler* handler = nullptr;
    if (thunk_id < kFastThunkSlots)
        handler = g_fast_handler[thunk_id].load(std::memory_order_acquire);
    if (!handler) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_id_to_handler.find(thunk_id);
        if (it != m_id_to_handler.end()) handler = &it->second;
    }
    return handler;
}

void ThunkManager::DispatchThunk(uint32_t thunk_id, Dynarmic::A32::Jit& jit) {
    if (const ThunkHandler* handler = ResolveThunkHandler(thunk_id)) {
        (*handler)(jit, m_host);
        if (GuestSignalPending()) GuestSignalDeliverPending(m_host);
        return;
    }
    LOGE("unknown thunk ID %u at PC 0x%08X", thunk_id, jit.Regs()[15]);
    jit.Regs()[0] = 0;
}

void ThunkManager::SetupGuestJNIEnv() {
    // 232 entries in JNINativeInterface; 256 leaves room and keeps the maths simple.
    uint32_t table_addr = m_host.GuestAlloc(256 * 4, 8);
    m_guest_jni_env = m_host.GuestAlloc(4, 4);
    m_host.MemoryWrite32(m_guest_jni_env, table_addr);

    // Anything not wired below lands on a stub that names its own slot index, so a
    // gap shows up as "JNI slot 123 is not bridged" instead of a silent zero.
    for (int i = 0; i < 256; ++i) {
        std::string name = "JNI_slot_" + std::to_string(i);
        RegisterThunk(name, [i](Dynarmic::A32::Jit& jit, DynarmicHost&) {
            LOGE("JNI slot %d is not bridged", i);
            jit.Regs()[0] = 0;
        });
        m_host.MemoryWrite32(table_addr + i * 4, GetOrCreateThunkAddress(name));
    }

    InstallGuestJNIEnv(*this, m_host, table_addr);
    LOGI("Guest JNIEnv configured at 0x%08X (table at 0x%08X)", m_guest_jni_env, table_addr);
}
