// A guest JNIEnv that forwards to the real JVM.
//
// The reference bridge handed the guest a table of stubs returning magic numbers
// (FindClass -> 0x3000, GetMethodID -> 0x2000). That was enough for a game that
// made a handful of JNI calls; Unity 4 lives on JNI - it reaches the asset
// manager, the display metrics, the soft keyboard and every guest plugins soundbank
// through it, and Java calls back into natives that JNI_OnLoad registered.
//
// So each slot below marshals between the 32-bit guest and the 64-bit host:
//   * jobject/jclass/jstring/jarray become 32-bit handles over global refs
//   * jmethodID/jfieldID become synthetic ids that also carry the signature,
//     which is what makes the variadic Call<Type>Method forms possible
//   * strings and array regions are copied into the guest heap
//
// Variadic calls follow the guest's own ABI: AAPCS with softfp, so every
// argument travels in r3..r? and then on the stack, and 64-bit arguments are
// 8-byte aligned. The signature string says how wide each one is.

#include <android/log.h>
#include <jni.h>

#include <pthread.h>

#include <algorithm>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <dynarmic/interface/A32/a32.h>

#include "dynarmic_host.h"
#include "native_activity_bridge.h"
#include "thunk_manager.h"
#include "thunk_types.h"
#include "unity_jni.h"

#define LOG_TAG "CMR_GuestJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

JavaVM* GetHostJavaVM();
DynarmicHost* GetDynarmicHostForJni();

namespace {

using Jit = Dynarmic::A32::Jit;

// Synthetic id spaces, tagged so a stray value shows up in a log as what it is.
constexpr uint32_t kMethodIdTag = 0x4D000000;
constexpr uint32_t kFieldIdTag  = 0x46000000;

struct MethodEntry {
    jmethodID id;
    std::string signature;
};

struct FieldEntry {
    jfieldID id;
};

std::mutex g_registry;
std::vector<MethodEntry> g_methods;
std::vector<FieldEntry> g_fields;
std::unordered_map<jmethodID, uint32_t> g_method_ids;
std::unordered_map<jfieldID, uint32_t> g_field_ids;
// Guest buffer -> the host resource it was copied from, so Release* can find it.
std::unordered_map<uint32_t, jobject> g_pinned_objects;
// A direct ByteBuffer's storage is host memory with no guest address, so the guest
// gets a shadow copy inside its own arena and the contents are pushed back across
// once the call that filled it returns.
struct DirectShadow {
    uint32_t guest = 0;
    void* host = nullptr;
    jlong capacity = 0;
};
std::unordered_map<void*, DirectShadow> g_direct_shadows;

// Every guest thread that touches JNI needs its own attached JNIEnv.
thread_local JNIEnv* t_env = nullptr;

// FindClass on a thread the VM attached itself resolves against the *system* class
// loader, which cannot see anything in the APK. Unity's own threads hit this the
// moment they look up com/unity3d/player/UnityPlayer, so the activity's loader is
// captured up front and used as the fallback.
jobject g_class_loader = nullptr;
jmethodID g_load_class = nullptr;

// A thread that attaches itself and then exits still attached leaves the runtime's
// Thread object behind, and a checked build aborts the process over it outright.
// Mono and guest plugins both start and stop threads across a session, and none of them
// knows it is talking to a bridge, so the detach hangs off a TLS destructor
// instead of being left to whoever created the thread. Only threads this function
// attached get the key set - detaching one the VM owns would be far worse.
pthread_key_t g_detach_key;
void DetachOnThreadExit(void*) {
    if (JavaVM* vm = GetHostJavaVM()) vm->DetachCurrentThread();
    t_env = nullptr;
}

JNIEnv* HostEnv() {
    if (t_env) return t_env;
    JavaVM* vm = GetHostJavaVM();
    if (!vm) return nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&t_env), JNI_VERSION_1_6) == JNI_OK) return t_env;
    if (vm->AttachCurrentThread(&t_env, nullptr) != JNI_OK) {
        t_env = nullptr;
        return nullptr;
    }
    static std::once_flag once;
    std::call_once(once, [] { pthread_key_create(&g_detach_key, DetachOnThreadExit); });
    pthread_setspecific(g_detach_key, reinterpret_cast<void*>(1));
    return t_env;
}

// Guest-visible handles are global refs: a local ref would go stale the moment
// the guest stored it past the call that produced it, which Unity does constantly.
//
// One handle per object, not per request. Minting a fresh global ref each time
// leaks the JNI global reference table - Java calls into the guest about seventy
// times a second, each with the same receiver, and the runtime aborts the process
// at 51200 entries. It is also wrong: the guest compares handles to decide
// whether two references are the same object, so one object with two handles
// reads as two objects.
//
// The table is capped, and that cap is enforced by eviction rather than by giving
// up. It used to fill and then mint an untracked global ref per object forever:
// out/final_health.log logs the changeover three minutes into a run, and at the
// ~45 refs a second that run was producing the runtime's own limit is another
// quarter of an hour away. Entries the guest explicitly asked to keep - its own
// NewGlobalRef - are pinned and never evicted. The rest go least-recently-used,
// which is a weaker version of what JNI already promises about a local ref: it
// does not outlive the frame that made it. A handle evicted out from under a
// guest that hoarded it resolves to null, which is a wrong answer rather than a
// use-after-free of a deleted object.
//
// Lookup is by identity hash so the usual case is a single IsSameObject check.
struct RefEntry {
    jobject global = nullptr;
    uint32_t handle = 0;
    int32_t hash = 0;
    int pins = 0;                       // guest NewGlobalRef minus DeleteGlobalRef
    std::list<RefEntry*>::iterator lru;  // only linked while pins == 0
};
std::unordered_map<int32_t, std::vector<RefEntry*>> g_refs_by_hash;
std::unordered_map<uint32_t, RefEntry*> g_refs_by_handle;
std::list<RefEntry*> g_ref_lru;  // front = most recently used
constexpr size_t kMaxCachedRefs = 8192;

// All four take g_registry held by the caller.
void LinkRefLru(RefEntry* entry) {
    g_ref_lru.push_front(entry);
    entry->lru = g_ref_lru.begin();
}

void TouchRef(RefEntry* entry) {
    if (entry->pins == 0) g_ref_lru.splice(g_ref_lru.begin(), g_ref_lru, entry->lru);
}

void EvictOneRef(JNIEnv* env) {
    if (g_ref_lru.empty()) return;  // everything is pinned; the table grows past the cap
    RefEntry* victim = g_ref_lru.back();
    g_ref_lru.pop_back();

    auto bucket = g_refs_by_hash.find(victim->hash);
    if (bucket != g_refs_by_hash.end()) {
        auto& entries = bucket->second;
        entries.erase(std::remove(entries.begin(), entries.end(), victim), entries.end());
        if (entries.empty()) g_refs_by_hash.erase(bucket);
    }
    g_refs_by_handle.erase(victim->handle);
    ReleaseGuestHandle(victim->global);
    env->DeleteGlobalRef(victim->global);
    delete victim;

    // The check that this works at all: the table is meant to sit at its cap and
    // stay there. A pinned count that climbs towards kMaxCachedRefs is the failure
    // mode to watch for - it means the guest is asking to keep everything and
    // eviction has nothing left to take.
    static uint64_t evicted = 0;
    if (++evicted % 4096 == 0) {
        LOGI("ref table: %zu entries, %zu evictable, %llu evicted", g_refs_by_handle.size(),
             g_ref_lru.size(), static_cast<unsigned long long>(evicted));
    }
}

RefEntry* FindRefByHandle(uint32_t handle) {
    auto it = g_refs_by_handle.find(handle);
    return it == g_refs_by_handle.end() ? nullptr : it->second;
}

int32_t IdentityHashOf(JNIEnv* env, jobject obj) {
    static jclass system = nullptr;
    static jmethodID identity_hash = nullptr;
    if (!identity_hash) {
        jclass local = env->FindClass("java/lang/System");
        if (!local) { env->ExceptionClear(); return 0; }
        if (!system) system = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        identity_hash = env->GetStaticMethodID(system, "identityHashCode", "(Ljava/lang/Object;)I");
        if (!identity_hash) { env->ExceptionClear(); return 0; }
    }
    int32_t hash = env->CallStaticIntMethod(system, identity_hash, obj);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    return hash;
}

uint32_t GuestRefForObject(JNIEnv* env, jobject obj) {
    if (!obj) return 0;
    int32_t hash = IdentityHashOf(env, obj);
    std::lock_guard<std::mutex> lock(g_registry);
    for (RefEntry* entry : g_refs_by_hash[hash]) {
        if (env->IsSameObject(entry->global, obj)) {
            TouchRef(entry);
            return entry->handle;
        }
    }
    // Eviction rehashes both maps, so nothing looked up above survives it.
    if (g_refs_by_handle.size() >= kMaxCachedRefs) EvictOneRef(env);

    jobject global = env->NewGlobalRef(obj);
    if (!global) return 0;
    uint32_t handle = GuestHandleFor(global);
    if (!handle) {
        env->DeleteGlobalRef(global);
        return 0;
    }
    auto* entry = new RefEntry{global, handle, hash, 0, {}};
    LinkRefLru(entry);
    g_refs_by_hash[hash].push_back(entry);
    g_refs_by_handle[handle] = entry;
    return handle;
}

uint32_t ToGuestRef(JNIEnv* env, jobject local) {
    if (!local) return 0;
    uint32_t handle = GuestRefForObject(env, local);
    env->DeleteLocalRef(local);
    return handle;
}

// A java.lang.Class turned into the JNI type descriptor for it. Class.getName()
// already returns array types in descriptor form ("[I", "[Ljava.lang.String;"), so
// only the primitives and the plain object case need translating.
std::string DescriptorOfClass(JNIEnv* env, jobject type) {
    if (!type) return "V";
    jclass class_cls = env->GetObjectClass(type);
    jmethodID get_name = env->GetMethodID(class_cls, "getName", "()Ljava/lang/String;");
    env->DeleteLocalRef(class_cls);
    if (!get_name) { env->ExceptionClear(); return "V"; }
    auto name = static_cast<jstring>(env->CallObjectMethod(type, get_name));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return "V"; }
    const char* chars = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
    std::string binary = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(name, chars);
    if (name) env->DeleteLocalRef(name);

    static const std::pair<const char*, const char*> kPrimitives[] = {
        {"boolean", "Z"}, {"byte", "B"}, {"char", "C"}, {"short", "S"},
        {"int", "I"},     {"long", "J"}, {"float", "F"}, {"double", "D"},
        {"void", "V"},
    };
    for (auto& [word, descriptor] : kPrimitives) {
        if (binary == word) return descriptor;
    }
    for (char& c : binary) {
        if (c == '.') c = '/';
    }
    if (!binary.empty() && binary[0] == '[') return binary;
    return "L" + binary + ";";
}

// The signature of a java.lang.reflect.Method or Constructor, read back out of the
// object itself. FromReflectedMethod hands over a jmethodID and nothing else, but
// every Call*Method in this bridge needs the signature to know how wide each
// argument is and what the return type is, so it is recovered here.
std::string SignatureOfReflected(JNIEnv* env, jobject member, bool* is_constructor) {
    jclass member_cls = env->GetObjectClass(member);
    jmethodID get_params = env->GetMethodID(member_cls, "getParameterTypes", "()[Ljava/lang/Class;");
    if (!get_params) { env->ExceptionClear(); env->DeleteLocalRef(member_cls); return ""; }
    // Constructors have no getReturnType, and their JNI signature returns void.
    jmethodID get_return = env->GetMethodID(member_cls, "getReturnType", "()Ljava/lang/Class;");
    if (!get_return) env->ExceptionClear();
    if (is_constructor) *is_constructor = (get_return == nullptr);
    env->DeleteLocalRef(member_cls);

    auto params = static_cast<jobjectArray>(env->CallObjectMethod(member, get_params));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return ""; }
    std::string signature = "(";
    if (params) {
        jsize count = env->GetArrayLength(params);
        for (jsize i = 0; i < count; ++i) {
            jobject type = env->GetObjectArrayElement(params, i);
            signature += DescriptorOfClass(env, type);
            env->DeleteLocalRef(type);
        }
        env->DeleteLocalRef(params);
    }
    signature += ")";

    if (!get_return) return signature + "V";
    jobject ret = env->CallObjectMethod(member, get_return);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return signature + "V"; }
    signature += DescriptorOfClass(env, ret);
    env->DeleteLocalRef(ret);
    return signature;
}

// Second chance for FindClass: ask the activity's own loader by binary name.
jclass LoadAppClass(JNIEnv* env, const char* jni_name) {
    if (!g_class_loader || !g_load_class) return nullptr;
    std::string binary(jni_name);
    for (char& c : binary) {
        if (c == '/') c = '.';
    }
    jstring arg = env->NewStringUTF(binary.c_str());
    auto cls = static_cast<jclass>(env->CallObjectMethod(g_class_loader, g_load_class, arg));
    env->DeleteLocalRef(arg);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return cls;
}

// Reading a handle counts as using it. An object a thunk is part way through
// working with has to sit at the front of the eviction order, or a
// GuestRefForObject on another thread could delete its global ref mid-call.
jobject FromGuestRef(uint32_t handle) {
    {
        std::lock_guard<std::mutex> lock(g_registry);
        if (RefEntry* entry = FindRefByHandle(handle)) {
            TouchRef(entry);
            return entry->global;
        }
    }
    // Not a JNI object: an ANativeWindow, an AAssetManager, an AInputEvent.
    return static_cast<jobject>(HostPointerFor(handle));
}

// The guest's own NewGlobalRef/DeleteGlobalRef, counted so two News and one
// Delete still leave the entry pinned.
void PinGuestRef(uint32_t handle, int delta) {
    std::lock_guard<std::mutex> lock(g_registry);
    RefEntry* entry = FindRefByHandle(handle);
    if (!entry) return;
    const bool was_pinned = entry->pins > 0;
    entry->pins += delta;
    if (entry->pins < 0) entry->pins = 0;
    const bool is_pinned = entry->pins > 0;
    if (is_pinned == was_pinned) return;
    if (is_pinned) g_ref_lru.erase(entry->lru);
    else LinkRefLru(entry);
}

// The guest says it is done with a local: send it to the back of the eviction
// order rather than the front.
void DemoteGuestRef(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_registry);
    RefEntry* entry = FindRefByHandle(handle);
    if (entry && entry->pins == 0) g_ref_lru.splice(g_ref_lru.end(), g_ref_lru, entry->lru);
}

const char* GuestStr(DynarmicHost& host, uint32_t addr) {
    return reinterpret_cast<const char*>(GuestToHost(host.GetGuestMemoryBase(), addr));
}

uint32_t GuestStrDup(DynarmicHost& host, const char* s, size_t n) {
    uint32_t addr = host.GuestAlloc(n + 1, 1);
    if (!addr) return 0;
    char* dst = reinterpret_cast<char*>(host.GetGuestMemoryBase() + addr);
    if (n) memcpy(dst, s, n);
    dst[n] = '\0';
    return addr;
}

// One synthetic id per jmethodID, not per lookup. Unity's AndroidJavaObject goes
// through FromReflectedMethod on every call it makes, so appending unconditionally
// grew the table - and the string beside each entry - for the life of the process,
// with only 24 bits of index to run out of.
uint32_t RegisterMethod(jmethodID id, const char* signature) {
    if (!id) return 0;
    std::lock_guard<std::mutex> lock(g_registry);
    auto [it, fresh] =
        g_method_ids.emplace(id, kMethodIdTag | static_cast<uint32_t>(g_methods.size()));
    if (fresh) g_methods.push_back({id, signature ? signature : ""});
    return it->second;
}

const MethodEntry* LookupMethod(uint32_t guest_id) {
    if ((guest_id & 0xFF000000u) != kMethodIdTag) return nullptr;
    std::lock_guard<std::mutex> lock(g_registry);
    uint32_t index = guest_id & 0x00FFFFFFu;
    return index < g_methods.size() ? &g_methods[index] : nullptr;
}

uint32_t RegisterField(jfieldID id) {
    if (!id) return 0;
    std::lock_guard<std::mutex> lock(g_registry);
    auto [it, fresh] =
        g_field_ids.emplace(id, kFieldIdTag | static_cast<uint32_t>(g_fields.size()));
    if (fresh) g_fields.push_back({id});
    return it->second;
}

jfieldID LookupField(uint32_t guest_id) {
    if ((guest_id & 0xFF000000u) != kFieldIdTag) return nullptr;
    std::lock_guard<std::mutex> lock(g_registry);
    uint32_t index = guest_id & 0x00FFFFFFu;
    return index < g_fields.size() ? g_fields[index].id : nullptr;
}

// --- argument walkers -------------------------------------------------------
//
// Three shapes reach the same place. The plain Call<Type>Method form is a real
// variadic call, so its arguments follow AAPCS base (softfp): r0.. then the
// stack, floats in core registers, 64-bit values on an even boundary. The V form
// hands over a va_list, which on ARM32 is just a cursor into that same block. The
// A form hands over an array of 8-byte jvalues.
enum class ArgSource { Varargs, VaList, JValueArray };

class GuestArgs {
public:
    // Varargs: registers first, then the caller's stack.
    GuestArgs(Jit& jit, DynarmicHost& host, int first_reg)
        : m_jit(&jit), m_host(host), m_reg(first_reg), m_cursor(jit.Regs()[13]) {}
    // va_list: everything comes from memory.
    GuestArgs(DynarmicHost& host, uint32_t cursor)
        : m_jit(nullptr), m_host(host), m_reg(4), m_cursor(cursor) {}

    uint32_t Word() {
        if (m_jit && m_reg < 4) return m_jit->Regs()[m_reg++];
        uint32_t value = m_host.MemoryRead32(m_cursor);
        m_cursor += 4;
        return value;
    }

    uint64_t DoubleWord() {
        if (m_jit && m_reg < 4) {
            if (m_reg & 1) ++m_reg;
        } else {
            m_cursor = (m_cursor + 7u) & ~7u;
        }
        uint64_t lo = Word();
        uint64_t hi = Word();
        return lo | (hi << 32);
    }

private:
    Jit* m_jit;
    DynarmicHost& m_host;
    int m_reg;
    uint32_t m_cursor;
};

// Steps past the argument list of a JNI signature, yielding one jvalue per arg.
std::vector<jvalue> ReadArgs(const std::string& signature, GuestArgs& args) {
    std::vector<jvalue> out;
    size_t i = signature.find('(');
    if (i == std::string::npos) return out;
    ++i;
    while (i < signature.size() && signature[i] != ')') {
        char t = signature[i];
        jvalue v {};
        switch (t) {
            case 'J': v.j = static_cast<jlong>(args.DoubleWord()); ++i; break;
            case 'D': {
                uint64_t bits = args.DoubleWord();
                memcpy(&v.d, &bits, sizeof(v.d));
                ++i;
                break;
            }
            case 'F': {
                uint32_t bits = args.Word();
                memcpy(&v.f, &bits, sizeof(v.f));
                ++i;
                break;
            }
            case 'Z': v.z = args.Word() ? JNI_TRUE : JNI_FALSE; ++i; break;
            case 'B': v.b = static_cast<jbyte>(args.Word()); ++i; break;
            case 'C': v.c = static_cast<jchar>(args.Word()); ++i; break;
            case 'S': v.s = static_cast<jshort>(args.Word()); ++i; break;
            case 'I': v.i = static_cast<jint>(args.Word()); ++i; break;
            case 'L': {
                v.l = FromGuestRef(args.Word());
                i = signature.find(';', i);
                i = (i == std::string::npos) ? signature.size() : i + 1;
                break;
            }
            case '[': {
                // Skip the whole array type, then take one reference word.
                size_t start = i;
                while (i < signature.size() && signature[i] == '[') ++i;
                if (i < signature.size() && signature[i] == 'L') {
                    i = signature.find(';', i);
                    i = (i == std::string::npos) ? signature.size() : i + 1;
                } else if (i < signature.size()) {
                    ++i;
                }
                (void)start;
                v.l = FromGuestRef(args.Word());
                break;
            }
            default:
                LOGE("unhandled signature char '%c' in %s", t, signature.c_str());
                return out;
        }
        out.push_back(v);
    }
    return out;
}

// --- primitive arrays -------------------------------------------------------
//
// JNI lists the eight primitive element types in the same order five times over
// (New, GetElements, ReleaseElements, GetRegion, SetRegion), so everything here
// is indexed by that order rather than written out eight times.
constexpr int kPrimTypeCount = 8;
constexpr int kPrimElemSize[kPrimTypeCount] = {1, 1, 2, 2, 4, 8, 4, 8};

const char* const kNewArrayNames[kPrimTypeCount] = {
    "JNI_NewBooleanArray", "JNI_NewByteArray", "JNI_NewCharArray", "JNI_NewShortArray",
    "JNI_NewIntArray", "JNI_NewLongArray", "JNI_NewFloatArray", "JNI_NewDoubleArray"};
const char* const kGetElementsNames[kPrimTypeCount] = {
    "JNI_GetBooleanArrayElements", "JNI_GetByteArrayElements", "JNI_GetCharArrayElements",
    "JNI_GetShortArrayElements", "JNI_GetIntArrayElements", "JNI_GetLongArrayElements",
    "JNI_GetFloatArrayElements", "JNI_GetDoubleArrayElements"};
const char* const kReleaseElementsNames[kPrimTypeCount] = {
    "JNI_ReleaseBooleanArrayElements", "JNI_ReleaseByteArrayElements", "JNI_ReleaseCharArrayElements",
    "JNI_ReleaseShortArrayElements", "JNI_ReleaseIntArrayElements", "JNI_ReleaseLongArrayElements",
    "JNI_ReleaseFloatArrayElements", "JNI_ReleaseDoubleArrayElements"};
const char* const kGetRegionNames[kPrimTypeCount] = {
    "JNI_GetBooleanArrayRegion", "JNI_GetByteArrayRegion", "JNI_GetCharArrayRegion",
    "JNI_GetShortArrayRegion", "JNI_GetIntArrayRegion", "JNI_GetLongArrayRegion",
    "JNI_GetFloatArrayRegion", "JNI_GetDoubleArrayRegion"};
const char* const kSetRegionNames[kPrimTypeCount] = {
    "JNI_SetBooleanArrayRegion", "JNI_SetByteArrayRegion", "JNI_SetCharArrayRegion",
    "JNI_SetShortArrayRegion", "JNI_SetIntArrayRegion", "JNI_SetLongArrayRegion",
    "JNI_SetFloatArrayRegion", "JNI_SetDoubleArrayRegion"};

jarray NewPrimitiveArray(JNIEnv* env, int type, jsize length) {
    switch (type) {
        case 0: return env->NewBooleanArray(length);
        case 1: return env->NewByteArray(length);
        case 2: return env->NewCharArray(length);
        case 3: return env->NewShortArray(length);
        case 4: return env->NewIntArray(length);
        case 5: return env->NewLongArray(length);
        case 6: return env->NewFloatArray(length);
        case 7: return env->NewDoubleArray(length);
        default: return nullptr;
    }
}

// The critical pointer is held only for the memcpy, never across guest code, so
// this stays a copy in and out rather than a real pin.
void CopyArrayToHost(JNIEnv* env, jarray array, void* dst, size_t byte_offset, size_t bytes) {
    if (!bytes) return;
    void* src = env->GetPrimitiveArrayCritical(array, nullptr);
    if (!src) return;
    memcpy(dst, static_cast<const uint8_t*>(src) + byte_offset, bytes);
    env->ReleasePrimitiveArrayCritical(array, src, JNI_ABORT);
}

void CopyArrayFromHost(JNIEnv* env, jarray array, const void* src, size_t byte_offset, size_t bytes) {
    if (!bytes) return;
    void* dst = env->GetPrimitiveArrayCritical(array, nullptr);
    if (!dst) return;
    memcpy(static_cast<uint8_t*>(dst) + byte_offset, src, bytes);
    env->ReleasePrimitiveArrayCritical(array, dst, 0);
}

// GetPrimitiveArrayCritical does not say what it is pointing at, so the size per
// element is read off the array's class name: "[I", "[J", "[F" and so on.
size_t ElementSizeOf(JNIEnv* env, jarray array) {
    static jmethodID get_name = nullptr;
    jclass cls = env->GetObjectClass(array);
    if (!cls) return 1;
    if (!get_name) {
        jclass class_class = env->GetObjectClass(cls);
        get_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
        env->DeleteLocalRef(class_class);
    }
    size_t size = 1;
    if (get_name) {
        auto name = static_cast<jstring>(env->CallObjectMethod(cls, get_name));
        if (name) {
            const char* chars = env->GetStringUTFChars(name, nullptr);
            if (chars && chars[0] == '[') {
                switch (chars[1]) {
                    case 'Z': case 'B': size = 1; break;
                    case 'C': case 'S': size = 2; break;
                    case 'I': case 'F': size = 4; break;
                    case 'J': case 'D': size = 8; break;
                    default: size = 1; break;
                }
            }
            if (chars) env->ReleaseStringUTFChars(name, chars);
            env->DeleteLocalRef(name);
        }
    }
    env->DeleteLocalRef(cls);
    return size;
}

char ReturnType(const std::string& signature) {
    size_t close = signature.find(')');
    return (close == std::string::npos || close + 1 >= signature.size()) ? 'V'
                                                                         : signature[close + 1];
}

void SetGuestReturn64(Jit& jit, uint64_t value) {
    jit.Regs()[0] = static_cast<uint32_t>(value);
    jit.Regs()[1] = static_cast<uint32_t>(value >> 32);
}

// Shared body for the twelve Call*Method / CallStatic*Method entry points. The
// declared return type in the signature decides which host call is made, not the
// slot the guest went through - a guest that calls CallObjectMethod on a void
// method is already broken, and this way one implementation covers all of them.
// Reads the argument list in whichever of the three forms the guest used.
std::vector<jvalue> CollectArgs(Jit& jit, DynarmicHost& host, ArgSource source,
                                const std::string& signature) {
    if (source == ArgSource::JValueArray) {
        // A guest jvalue is a union containing a double, so 8 bytes wide each.
        std::vector<jvalue> out;
        uint32_t base = jit.Regs()[3];
        size_t i = signature.find('(');
        if (i == std::string::npos || !base) return out;
        for (++i; i < signature.size() && signature[i] != ')';) {
            GuestArgs one(host, base + static_cast<uint32_t>(out.size()) * 8);
            std::string single = "(";
            size_t start = i;
            while (i < signature.size() && signature[i] == '[') ++i;
            if (i < signature.size() && signature[i] == 'L') {
                i = signature.find(';', i);
                i = (i == std::string::npos) ? signature.size() : i + 1;
            } else if (i < signature.size()) {
                ++i;
            }
            single += signature.substr(start, i - start) + ")V";
            std::vector<jvalue> one_arg = ReadArgs(single, one);
            if (one_arg.empty()) break;
            out.push_back(one_arg.front());
        }
        return out;
    }

    if (source == ArgSource::VaList) {
        GuestArgs walker(host, jit.Regs()[3]);
        return ReadArgs(signature, walker);
    }

    GuestArgs walker(jit, host, 3);
    return ReadArgs(signature, walker);
}

void CallMethod(Jit& jit, DynarmicHost& host, bool is_static, ArgSource source) {
    JNIEnv* env = HostEnv();
    uint32_t target = jit.Regs()[1];
    const MethodEntry* method = LookupMethod(jit.Regs()[2]);
    if (!env || !method) {
        LOGE("Call%sMethod with bad method id 0x%08X", is_static ? "Static" : "", jit.Regs()[2]);
        jit.Regs()[0] = 0;
        return;
    }

    std::vector<jvalue> args = CollectArgs(jit, host, source, method->signature);
    const jvalue* argv = args.empty() ? nullptr : args.data();

    jobject obj = FromGuestRef(target);
    jclass cls = static_cast<jclass>(obj);

    switch (ReturnType(method->signature)) {
        case 'V':
            if (is_static) env->CallStaticVoidMethodA(cls, method->id, argv);
            else env->CallVoidMethodA(obj, method->id, argv);
            jit.Regs()[0] = 0;
            break;
        case 'Z':
            jit.Regs()[0] = (is_static ? env->CallStaticBooleanMethodA(cls, method->id, argv)
                                       : env->CallBooleanMethodA(obj, method->id, argv)) ? 1 : 0;
            break;
        case 'B':
            jit.Regs()[0] = static_cast<uint32_t>(is_static ? env->CallStaticByteMethodA(cls, method->id, argv)
                                                            : env->CallByteMethodA(obj, method->id, argv));
            break;
        case 'C':
            jit.Regs()[0] = static_cast<uint32_t>(is_static ? env->CallStaticCharMethodA(cls, method->id, argv)
                                                            : env->CallCharMethodA(obj, method->id, argv));
            break;
        case 'S':
            jit.Regs()[0] = static_cast<uint32_t>(is_static ? env->CallStaticShortMethodA(cls, method->id, argv)
                                                            : env->CallShortMethodA(obj, method->id, argv));
            break;
        case 'I':
            jit.Regs()[0] = static_cast<uint32_t>(is_static ? env->CallStaticIntMethodA(cls, method->id, argv)
                                                            : env->CallIntMethodA(obj, method->id, argv));
            break;
        case 'J':
            SetGuestReturn64(jit, static_cast<uint64_t>(is_static ? env->CallStaticLongMethodA(cls, method->id, argv)
                                                                  : env->CallLongMethodA(obj, method->id, argv)));
            break;
        case 'F': {
            jfloat f = is_static ? env->CallStaticFloatMethodA(cls, method->id, argv)
                                 : env->CallFloatMethodA(obj, method->id, argv);
            uint32_t bits;
            memcpy(&bits, &f, sizeof(bits));
            jit.Regs()[0] = bits;
            break;
        }
        case 'D': {
            jdouble d = is_static ? env->CallStaticDoubleMethodA(cls, method->id, argv)
                                  : env->CallDoubleMethodA(obj, method->id, argv);
            uint64_t bits;
            memcpy(&bits, &d, sizeof(bits));
            SetGuestReturn64(jit, bits);
            break;
        }
        default: {
            jobject result = is_static ? env->CallStaticObjectMethodA(cls, method->id, argv)
                                       : env->CallObjectMethodA(obj, method->id, argv);
            jit.Regs()[0] = ToGuestRef(env, result);
            break;
        }
    }
}

}  // namespace

void InstallGuestJNIEnv(ThunkManager& thunks, DynarmicHost& host, uint32_t table_addr) {
    auto slot = [&](int index, const char* name, ThunkHandler handler) {
        thunks.RegisterThunk(name, std::move(handler));
        host.MemoryWrite32(table_addr + index * 4, thunks.GetOrCreateThunkAddress(name));
    };
    auto reuse = [&](int index, const char* name) {
        host.MemoryWrite32(table_addr + index * 4, thunks.GetOrCreateThunkAddress(name));
    };

    slot(4, "JNI_GetVersion", [](Jit& jit, DynarmicHost&) {
        jit.Regs()[0] = JNI_VERSION_1_6;
    });

    slot(6, "JNI_FindClass", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        const char* name = GuestStr(host, jit.Regs()[1]);
        if (!env || !name) {
            // Returning a silent zero here reads, three layers up, as "Unity's
            // reflection helper does not exist" - and that is how a missing host
            // JavaVM stayed hidden through a whole boot.
            LOGE("FindClass('%s') has no host JNIEnv", name ? name : "(null)");
            jit.Regs()[0] = 0;
            return;
        }
        jclass cls = env->FindClass(name);
        if (!cls) {
            env->ExceptionClear();
            cls = LoadAppClass(env, name);
        }
        if (!cls) {
            LOGE("FindClass('%s') failed", name);
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = ToGuestRef(env, cls);
    });

    // Unity does not look methods up by name and signature. AndroidReflection asks
    // Java for a java.lang.reflect.Method and converts it here, so every call made
    // through AndroidJavaObject - which is all of them, including
    // new AndroidJavaClass("android.os.Build") - stops at this slot. Unbridged, it
    // answered "no such method", the call returned null, and Unity threw
    // "Init'd AndroidJavaObject with null ptr!" out of whatever was running.
    slot(7, "JNI_FromReflectedMethod", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject member = FromGuestRef(jit.Regs()[1]);
        if (!env || !member) { jit.Regs()[0] = 0; return; }
        bool is_constructor = false;
        std::string signature = SignatureOfReflected(env, member, &is_constructor);
        jmethodID id = env->FromReflectedMethod(member);
        if (!id) {
            env->ExceptionClear();
            LOGE("FromReflectedMethod failed");
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = RegisterMethod(id, signature.c_str());
    });

    slot(8, "JNI_FromReflectedField", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject member = FromGuestRef(jit.Regs()[1]);
        if (!env || !member) { jit.Regs()[0] = 0; return; }
        jfieldID id = env->FromReflectedField(member);
        if (!id) {
            env->ExceptionClear();
            LOGE("FromReflectedField failed");
            jit.Regs()[0] = 0;
            return;
        }
        jit.Regs()[0] = RegisterField(id);
    });

    slot(10, "JNI_GetSuperclass", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jclass cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        jit.Regs()[0] = (env && cls) ? ToGuestRef(env, env->GetSuperclass(cls)) : 0;
    });

    slot(11, "JNI_IsAssignableFrom", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto a = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        auto b = static_cast<jclass>(FromGuestRef(jit.Regs()[2]));
        jit.Regs()[0] = (env && a && b && env->IsAssignableFrom(a, b)) ? 1 : 0;
    });

    slot(14, "JNI_ThrowNew", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        const char* msg = GuestStr(host, jit.Regs()[2]);
        jit.Regs()[0] = (env && cls) ? static_cast<uint32_t>(env->ThrowNew(cls, msg)) : 0xFFFFFFFF;
    });

    slot(15, "JNI_ExceptionOccurred", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jit.Regs()[0] = env ? ToGuestRef(env, env->ExceptionOccurred()) : 0;
    });

    slot(16, "JNI_ExceptionDescribe", [](Jit& jit, DynarmicHost&) {
        if (JNIEnv* env = HostEnv()) env->ExceptionDescribe();
        jit.Regs()[0] = 0;
    });

    slot(17, "JNI_ExceptionClear", [](Jit& jit, DynarmicHost&) {
        if (JNIEnv* env = HostEnv()) env->ExceptionClear();
        jit.Regs()[0] = 0;
    });

    slot(21, "JNI_NewGlobalRef", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        // Already global on this side; hand back the same handle so the guest's
        // identity comparisons keep working. What the call actually buys is a pin:
        // an entry the guest says it is keeping is one the table may not evict.
        if (env && obj) PinGuestRef(jit.Regs()[1], +1);
        jit.Regs()[0] = (env && obj) ? jit.Regs()[1] : 0;
    });

    // Handles are shared, so dropping the host ref here would invalidate any copy
    // the guest still holds. Both forms therefore only adjust how willing the table
    // is to reclaim the entry: a released global loses its pin, and a released
    // local becomes the next thing evicted, which is as close to the real lifetime
    // as a shared handle can get.
    slot(22, "JNI_DeleteGlobalRef", [](Jit& jit, DynarmicHost&) {
        PinGuestRef(jit.Regs()[1], -1);
        jit.Regs()[0] = 0;
    });
    slot(23, "JNI_DeleteLocalRef", [](Jit& jit, DynarmicHost&) {
        DemoteGuestRef(jit.Regs()[1]);
        jit.Regs()[0] = 0;
    });
    slot(25, "JNI_NewLocalRef", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = jit.Regs()[1]; });
    slot(26, "JNI_EnsureLocalCapacity", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    slot(19, "JNI_PushLocalFrame", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });
    slot(20, "JNI_PopLocalFrame", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = jit.Regs()[1]; });

    slot(24, "JNI_IsSameObject", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject a = FromGuestRef(jit.Regs()[1]);
        jobject b = FromGuestRef(jit.Regs()[2]);
        jit.Regs()[0] = (env && env->IsSameObject(a, b)) ? 1 : 0;
    });

    slot(31, "JNI_GetObjectClass", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        jit.Regs()[0] = (env && obj) ? ToGuestRef(env, env->GetObjectClass(obj)) : 0;
    });

    slot(32, "JNI_IsInstanceOf", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[2]));
        jit.Regs()[0] = (env && cls && env->IsInstanceOf(obj, cls)) ? 1 : 0;
    });

    auto get_method_id = [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        const char* name = GuestStr(host, jit.Regs()[2]);
        const char* sig = GuestStr(host, jit.Regs()[3]);
        if (!env || !cls || !name || !sig) { jit.Regs()[0] = 0; return; }
        jmethodID id = env->GetMethodID(cls, name, sig);
        if (!id) { env->ExceptionClear(); LOGE("GetMethodID('%s','%s') failed", name, sig); }
        jit.Regs()[0] = RegisterMethod(id, sig);
    };
    slot(33, "JNI_GetMethodID", get_method_id);

    slot(113, "JNI_GetStaticMethodID", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        const char* name = GuestStr(host, jit.Regs()[2]);
        const char* sig = GuestStr(host, jit.Regs()[3]);
        if (!env || !cls || !name || !sig) { jit.Regs()[0] = 0; return; }
        jmethodID id = env->GetStaticMethodID(cls, name, sig);
        if (!id) { env->ExceptionClear(); LOGE("GetStaticMethodID('%s','%s') failed", name, sig); }
        jit.Regs()[0] = RegisterMethod(id, sig);
    });

    // NewObject: the constructor signature is stored with its id, so the same
    // walker handles it - only the receiver differs.
    auto new_object = [](Jit& jit, DynarmicHost& host, ArgSource source) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        const MethodEntry* method = LookupMethod(jit.Regs()[2]);
        if (!env || !cls || !method) { jit.Regs()[0] = 0; return; }
        std::vector<jvalue> args = CollectArgs(jit, host, source, method->signature);
        jobject obj = env->NewObjectA(cls, method->id, args.empty() ? nullptr : args.data());
        jit.Regs()[0] = ToGuestRef(env, obj);
    };
    slot(28, "JNI_NewObject", [new_object](Jit& j, DynarmicHost& h) {
        new_object(j, h, ArgSource::Varargs);
    });
    slot(29, "JNI_NewObjectV", [new_object](Jit& j, DynarmicHost& h) {
        new_object(j, h, ArgSource::VaList);
    });
    slot(30, "JNI_NewObjectA", [new_object](Jit& j, DynarmicHost& h) {
        new_object(j, h, ArgSource::JValueArray);
    });
    slot(27, "JNI_AllocObject", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        jit.Regs()[0] = (env && cls) ? ToGuestRef(env, env->AllocObject(cls)) : 0;
    });

    // Object, Boolean, Byte, Char, Short, Int, Long, Float, Double, Void - each with
    // a plain, a V and an A form, in that order. The declared return type in the
    // signature picks the host call, so all ten types share one handler and only the
    // argument shape needs its own.
    slot(34, "JNI_CallMethod",   [](Jit& j, DynarmicHost& h) { CallMethod(j, h, false, ArgSource::Varargs); });
    slot(35, "JNI_CallMethodV",  [](Jit& j, DynarmicHost& h) { CallMethod(j, h, false, ArgSource::VaList); });
    slot(36, "JNI_CallMethodA",  [](Jit& j, DynarmicHost& h) { CallMethod(j, h, false, ArgSource::JValueArray); });
    slot(114, "JNI_CallStaticMethod",  [](Jit& j, DynarmicHost& h) { CallMethod(j, h, true, ArgSource::Varargs); });
    slot(115, "JNI_CallStaticMethodV", [](Jit& j, DynarmicHost& h) { CallMethod(j, h, true, ArgSource::VaList); });
    slot(116, "JNI_CallStaticMethodA", [](Jit& j, DynarmicHost& h) { CallMethod(j, h, true, ArgSource::JValueArray); });
    static const char* const kCallNames[3] = {"JNI_CallMethod", "JNI_CallMethodV", "JNI_CallMethodA"};
    static const char* const kStaticNames[3] = {"JNI_CallStaticMethod", "JNI_CallStaticMethodV",
                                                "JNI_CallStaticMethodA"};
    for (int i = 3; i < 30; ++i) {
        reuse(34 + i, kCallNames[i % 3]);
        reuse(114 + i, kStaticNames[i % 3]);
    }
    // CallNonvirtual<Type>Method takes an extra jclass before the method id, which
    // shifts every argument by one register; nothing in Unity's guest code uses it,
    // so those slots keep the generic stub and will log if that ever changes.

    slot(94, "JNI_GetFieldID", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        const char* name = GuestStr(host, jit.Regs()[2]);
        const char* sig = GuestStr(host, jit.Regs()[3]);
        if (!env || !cls || !name || !sig) { jit.Regs()[0] = 0; return; }
        jfieldID id = env->GetFieldID(cls, name, sig);
        if (!id) { env->ExceptionClear(); LOGE("GetFieldID('%s','%s') failed", name, sig); }
        jit.Regs()[0] = RegisterField(id);
    });

    slot(144, "JNI_GetStaticFieldID", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        const char* name = GuestStr(host, jit.Regs()[2]);
        const char* sig = GuestStr(host, jit.Regs()[3]);
        if (!env || !cls || !name || !sig) { jit.Regs()[0] = 0; return; }
        jfieldID id = env->GetStaticFieldID(cls, name, sig);
        if (!id) { env->ExceptionClear(); LOGE("GetStaticFieldID('%s','%s') failed", name, sig); }
        jit.Regs()[0] = RegisterField(id);
    });

    slot(95, "JNI_GetObjectField", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        jfieldID f = LookupField(jit.Regs()[2]);
        jit.Regs()[0] = (env && obj && f) ? ToGuestRef(env, env->GetObjectField(obj, f)) : 0;
    });
    slot(96, "JNI_GetBooleanField", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        jfieldID f = LookupField(jit.Regs()[2]);
        jit.Regs()[0] = (env && obj && f && env->GetBooleanField(obj, f)) ? 1 : 0;
    });
    slot(100, "JNI_GetIntField", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        jfieldID f = LookupField(jit.Regs()[2]);
        jit.Regs()[0] = (env && obj && f) ? static_cast<uint32_t>(env->GetIntField(obj, f)) : 0;
    });
    slot(102, "JNI_GetFloatField", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        jfieldID f = LookupField(jit.Regs()[2]);
        jfloat v = (env && obj && f) ? env->GetFloatField(obj, f) : 0.0f;
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        jit.Regs()[0] = bits;
    });
    slot(109, "JNI_SetIntField", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        jfieldID f = LookupField(jit.Regs()[2]);
        if (env && obj && f) env->SetIntField(obj, f, static_cast<jint>(jit.Regs()[3]));
        jit.Regs()[0] = 0;
    });
    slot(145, "JNI_GetStaticObjectField", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        jfieldID f = LookupField(jit.Regs()[2]);
        jit.Regs()[0] = (env && cls && f) ? ToGuestRef(env, env->GetStaticObjectField(cls, f)) : 0;
    });
    slot(150, "JNI_GetStaticIntField", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        jfieldID f = LookupField(jit.Regs()[2]);
        jit.Regs()[0] = (env && cls && f) ? static_cast<uint32_t>(env->GetStaticIntField(cls, f)) : 0;
    });

    slot(163, "JNI_NewString", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        uint32_t chars = jit.Regs()[1];
        jsize len = static_cast<jsize>(jit.Regs()[2]);
        if (!env || !chars) { jit.Regs()[0] = 0; return; }
        auto* src = reinterpret_cast<const jchar*>(host.GetGuestMemoryBase() + chars);
        jit.Regs()[0] = ToGuestRef(env, env->NewString(src, len));
    });
    slot(164, "JNI_GetStringLength", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto s = static_cast<jstring>(FromGuestRef(jit.Regs()[1]));
        jit.Regs()[0] = (env && s) ? static_cast<uint32_t>(env->GetStringLength(s)) : 0;
    });

    slot(167, "JNI_NewStringUTF", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        const char* s = GuestStr(host, jit.Regs()[1]);
        jit.Regs()[0] = (env && s) ? ToGuestRef(env, env->NewStringUTF(s)) : 0;
    });
    slot(168, "JNI_GetStringUTFLength", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto s = static_cast<jstring>(FromGuestRef(jit.Regs()[1]));
        jit.Regs()[0] = (env && s) ? static_cast<uint32_t>(env->GetStringUTFLength(s)) : 0;
    });
    slot(169, "JNI_GetStringUTFChars", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        auto s = static_cast<jstring>(FromGuestRef(jit.Regs()[1]));
        if (!env || !s) { jit.Regs()[0] = 0; return; }
        const char* utf = env->GetStringUTFChars(s, nullptr);
        uint32_t guest = utf ? GuestStrDup(host, utf, strlen(utf)) : 0;
        if (utf) env->ReleaseStringUTFChars(s, utf);
        if (jit.Regs()[2]) host.MemoryWrite8(jit.Regs()[2], 1);  // isCopy
        jit.Regs()[0] = guest;
    });
    slot(170, "JNI_ReleaseStringUTFChars", [](Jit& jit, DynarmicHost& host) {
        // The copy lives in the guest heap, so there is nothing to hand back to the
        // JVM - but the block is ours and this is the only call that says when it
        // stops being needed. Mono asks for every assembly name and type name this
        // way, so skipping the free drained the arena steadily.
        host.GuestFree(jit.Regs()[2]);
        jit.Regs()[0] = 0;
    });

    slot(171, "JNI_GetArrayLength", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto a = static_cast<jarray>(FromGuestRef(jit.Regs()[1]));
        jit.Regs()[0] = (env && a) ? static_cast<uint32_t>(env->GetArrayLength(a)) : 0;
    });
    slot(172, "JNI_NewObjectArray", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[2]));
        jobject init = FromGuestRef(jit.Regs()[3]);
        if (!env || !cls) { jit.Regs()[0] = 0; return; }
        jit.Regs()[0] = ToGuestRef(env, env->NewObjectArray(static_cast<jsize>(jit.Regs()[1]), cls, init));
    });
    slot(173, "JNI_GetObjectArrayElement", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto a = static_cast<jobjectArray>(FromGuestRef(jit.Regs()[1]));
        if (!env || !a) { jit.Regs()[0] = 0; return; }
        jit.Regs()[0] = ToGuestRef(env, env->GetObjectArrayElement(a, static_cast<jsize>(jit.Regs()[2])));
    });
    slot(174, "JNI_SetObjectArrayElement", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto a = static_cast<jobjectArray>(FromGuestRef(jit.Regs()[1]));
        if (env && a) env->SetObjectArrayElement(a, static_cast<jsize>(jit.Regs()[2]),
                                                 FromGuestRef(jit.Regs()[3]));
        jit.Regs()[0] = 0;
    });
    // All eight primitive element types, not just byte. Unity hands touch events
    // to Java through UnityPlayer.forwardMotionEventToDalvik, which takes an
    // int[], a long[] and two float[] - with only NewByteArray bridged, the
    // others came back null and the first touch aborted the process inside ART.
    //
    // The five families are laid out in the same type order, so one loop covers
    // forty slots: New at 175, GetElements at 183, ReleaseElements at 191,
    // GetRegion at 199, SetRegion at 207.
    for (int type = 0; type < kPrimTypeCount; ++type) {
        const int elem_size = kPrimElemSize[type];

        slot(175 + type, kNewArrayNames[type], [type](Jit& jit, DynarmicHost&) {
            JNIEnv* env = HostEnv();
            jit.Regs()[0] = env ? ToGuestRef(env, NewPrimitiveArray(env, type,
                                                                    static_cast<jsize>(jit.Regs()[1])))
                                : 0;
        });

        // Get*ArrayElements has to hand back a guest address, so the contents are
        // copied into the guest heap and the array is remembered until Release.
        slot(183 + type, kGetElementsNames[type], [elem_size](Jit& jit, DynarmicHost& host) {
            JNIEnv* env = HostEnv();
            auto a = static_cast<jarray>(FromGuestRef(jit.Regs()[1]));
            if (!env || !a) { jit.Regs()[0] = 0; return; }
            size_t bytes = static_cast<size_t>(env->GetArrayLength(a)) * elem_size;
            uint32_t guest = host.GuestAlloc(bytes > 0 ? bytes : 1, 8);
            if (!guest) { jit.Regs()[0] = 0; return; }
            CopyArrayToHost(env, a, host.GetGuestMemoryBase() + guest, 0, bytes);
            {
                std::lock_guard<std::mutex> lock(g_registry);
                g_pinned_objects[guest] = a;
            }
            if (jit.Regs()[2]) host.MemoryWrite8(jit.Regs()[2], 1);
            jit.Regs()[0] = guest;
        });
        slot(191 + type, kReleaseElementsNames[type], [elem_size](Jit& jit, DynarmicHost& host) {
            JNIEnv* env = HostEnv();
            uint32_t guest = jit.Regs()[2];
            uint32_t mode = jit.Regs()[3];
            jarray a = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_registry);
                auto it = g_pinned_objects.find(guest);
                if (it != g_pinned_objects.end()) {
                    a = static_cast<jarray>(it->second);
                    if (mode != JNI_COMMIT) g_pinned_objects.erase(it);
                }
            }
            if (env && a && mode != JNI_ABORT) {
                size_t bytes = static_cast<size_t>(env->GetArrayLength(a)) * elem_size;
                CopyArrayFromHost(env, a, host.GetGuestMemoryBase() + guest, 0, bytes);
            }
            // JNI_COMMIT means "write it back but I am still holding the pointer";
            // any other mode ends the borrow, and the copy goes back to the arena.
            if (a && mode != JNI_COMMIT) host.GuestFree(guest);
            jit.Regs()[0] = 0;
        });

        // Get/Set*ArrayRegion(env, array, start, len, buf): the buffer is the
        // fifth argument, so it is the first one on the stack.
        slot(199 + type, kGetRegionNames[type], [elem_size](Jit& jit, DynarmicHost& host) {
            JNIEnv* env = HostEnv();
            auto a = static_cast<jarray>(FromGuestRef(jit.Regs()[1]));
            uint32_t buf = host.MemoryRead32(jit.Regs()[13]);
            if (env && a && buf) {
                CopyArrayToHost(env, a, host.GetGuestMemoryBase() + buf,
                                static_cast<size_t>(jit.Regs()[2]) * elem_size,
                                static_cast<size_t>(jit.Regs()[3]) * elem_size);
            }
            jit.Regs()[0] = 0;
        });
        slot(207 + type, kSetRegionNames[type], [elem_size](Jit& jit, DynarmicHost& host) {
            JNIEnv* env = HostEnv();
            auto a = static_cast<jarray>(FromGuestRef(jit.Regs()[1]));
            uint32_t buf = host.MemoryRead32(jit.Regs()[13]);
            if (env && a && buf) {
                CopyArrayFromHost(env, a, host.GetGuestMemoryBase() + buf,
                                  static_cast<size_t>(jit.Regs()[2]) * elem_size,
                                  static_cast<size_t>(jit.Regs()[3]) * elem_size);
            }
            jit.Regs()[0] = 0;
        });
    }

    // Get/ReleasePrimitiveArrayCritical: same copy-in/copy-out as the typed
    // Elements pair. The element size is not in the call, so it comes from the
    // array's own class name - "[I" is four bytes per element and so on.
    slot(222, "JNI_GetPrimitiveArrayCritical", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        auto a = static_cast<jarray>(FromGuestRef(jit.Regs()[1]));
        if (!env || !a) { jit.Regs()[0] = 0; return; }
        size_t bytes = static_cast<size_t>(env->GetArrayLength(a)) * ElementSizeOf(env, a);
        uint32_t guest = host.GuestAlloc(bytes > 0 ? bytes : 1, 8);
        if (!guest) { jit.Regs()[0] = 0; return; }
        CopyArrayToHost(env, a, host.GetGuestMemoryBase() + guest, 0, bytes);
        {
            std::lock_guard<std::mutex> lock(g_registry);
            g_pinned_objects[guest] = a;
        }
        if (jit.Regs()[2]) host.MemoryWrite8(jit.Regs()[2], 1);
        jit.Regs()[0] = guest;
    });
    slot(223, "JNI_ReleasePrimitiveArrayCritical", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        uint32_t guest = jit.Regs()[2];
        uint32_t mode = jit.Regs()[3];
        jarray a = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_registry);
            auto it = g_pinned_objects.find(guest);
            if (it != g_pinned_objects.end()) {
                a = static_cast<jarray>(it->second);
                if (mode != JNI_COMMIT) g_pinned_objects.erase(it);
            }
        }
        if (env && a && mode != JNI_ABORT) {
            size_t bytes = static_cast<size_t>(env->GetArrayLength(a)) * ElementSizeOf(env, a);
            CopyArrayFromHost(env, a, host.GetGuestMemoryBase() + guest, 0, bytes);
        }
        if (a && mode != JNI_COMMIT) host.GuestFree(guest);
        jit.Regs()[0] = 0;
    });

    slot(215, "JNI_RegisterNatives", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        std::string class_name = (env && cls) ? DescriptorOfClass(env, cls) : "";
        if (class_name.size() >= 2 && class_name.front() == 'L' && class_name.back() == ';') {
            class_name = class_name.substr(1, class_name.size() - 2);
        }
        if (class_name.empty()) {
            LOGE("RegisterNatives received an unknown class");
            jit.Regs()[0] = 0xFFFFFFFFu;
            return;
        }

        uint32_t methods = jit.Regs()[2];
        int32_t count = static_cast<int32_t>(jit.Regs()[3]);
        for (int32_t i = 0; i < count; ++i) {
            uint32_t rec = methods + static_cast<uint32_t>(i) * 12;
            const char* name = GuestStr(host, host.MemoryRead32(rec));
            const char* sig = GuestStr(host, host.MemoryRead32(rec + 4));
            uint32_t fn = host.MemoryRead32(rec + 8);
            if (!name || !sig || !fn) {
                LOGE("invalid RegisterNatives row %d for %s", i, class_name.c_str());
                jit.Regs()[0] = 0xFFFFFFFFu;
                return;
            }
            RecordGuestNative(class_name, name, sig, fn);
        }
        jit.Regs()[0] = GuestNativeRegistryValid() ? 0 : 0xFFFFFFFFu;
    });
    slot(216, "JNI_UnregisterNatives", [](Jit& jit, DynarmicHost&) { jit.Regs()[0] = 0; });

    slot(217, "JNI_MonitorEnter", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        jit.Regs()[0] = (env && obj) ? static_cast<uint32_t>(env->MonitorEnter(obj)) : 0;
    });
    slot(218, "JNI_MonitorExit", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject obj = FromGuestRef(jit.Regs()[1]);
        jit.Regs()[0] = (env && obj) ? static_cast<uint32_t>(env->MonitorExit(obj)) : 0;
    });

    slot(219, "JNI_GetJavaVM", [](Jit& jit, DynarmicHost& host) {
        if (jit.Regs()[1]) host.MemoryWrite32(jit.Regs()[1], GuestJavaVM());
        jit.Regs()[0] = 0;
    });

    slot(221, "JNI_GetStringUTFRegion", [](Jit& jit, DynarmicHost& host) {
        JNIEnv* env = HostEnv();
        auto s = static_cast<jstring>(FromGuestRef(jit.Regs()[1]));
        uint32_t buf = host.MemoryRead32(jit.Regs()[13]);
        if (env && s && buf) {
            env->GetStringUTFRegion(s, static_cast<jsize>(jit.Regs()[2]), static_cast<jsize>(jit.Regs()[3]),
                                    reinterpret_cast<char*>(host.GetGuestMemoryBase() + buf));
        }
        jit.Regs()[0] = 0;
    });

    slot(228, "JNI_ExceptionCheck", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jit.Regs()[0] = (env && env->ExceptionCheck()) ? 1 : 0;
    });

    slot(230, "JNI_GetDirectBufferAddress", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject buf = FromGuestRef(jit.Regs()[1]);
        jit.Regs()[0] = GuestDirectBufferShadow(env, buf);
    });
    slot(231, "JNI_GetDirectBufferCapacity", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        jobject buf = FromGuestRef(jit.Regs()[1]);
        jlong cap = (env && buf) ? env->GetDirectBufferCapacity(buf) : 0;
        jit.Regs()[0] = static_cast<uint32_t>(cap);
        jit.Regs()[1] = static_cast<uint32_t>(static_cast<uint64_t>(cap) >> 32);
    });

    slot(13, "JNI_Throw", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto ex = static_cast<jthrowable>(FromGuestRef(jit.Regs()[1]));
        jit.Regs()[0] = (env && ex) ? static_cast<uint32_t>(env->Throw(ex)) : 0xFFFFFFFF;
    });
    slot(18, "JNI_FatalError", [](Jit& jit, DynarmicHost& host) {
        const char* msg = GuestStr(host, jit.Regs()[1]);
        // Aborting here would take the process down inside the JIT with no usable
        // backtrace; the guest is left to unwind on its own instead.
        LOGE("guest called FatalError: %s", msg ? msg : "(null)");
        jit.Regs()[0] = 0;
    });
    slot(152, "JNI_GetStaticFloatField", [](Jit& jit, DynarmicHost&) {
        JNIEnv* env = HostEnv();
        auto cls = static_cast<jclass>(FromGuestRef(jit.Regs()[1]));
        jfieldID f = LookupField(jit.Regs()[2]);
        jfloat v = (env && cls && f) ? env->GetStaticFloatField(cls, f) : 0.0f;
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        jit.Regs()[0] = bits;
    });

    LOGI("guest JNIEnv forwarding installed");
}

// Wraps an object arriving from Java in a global ref and gives the guest a handle
// for it. A per-frame caller such as nativeInjectEvent hands over a fresh object
// every time; those entries are unpinned, so the table reclaims them in arrival
// order once it is full rather than growing.
uint32_t GuestRefForHostObject(JNIEnv* env, jobject obj) {
    if (!env || !obj) return 0;
    return GuestRefForObject(env, obj);
}

// Direct ByteBuffers: the guest cannot address host memory, so it is handed a
// shadow inside its own arena. GuestDirectBufferShadow both creates it and is what
// the GetDirectBufferAddress slot returns, so the guest sees one stable address.
uint32_t GuestDirectBufferShadow(JNIEnv* env, jobject buffer) {
    DynarmicHost* host = GetDynarmicHostForJni();
    if (!env || !buffer || !host) return 0;
    void* addr = env->GetDirectBufferAddress(buffer);
    if (!addr) return 0;

    std::lock_guard<std::mutex> lock(g_registry);
    auto it = g_direct_shadows.find(addr);
    if (it != g_direct_shadows.end()) return it->second.guest;

    jlong capacity = env->GetDirectBufferCapacity(buffer);
    if (capacity <= 0) return 0;
    uint32_t guest = host->GuestAlloc(static_cast<size_t>(capacity), 16);
    if (!guest) return 0;
    memcpy(host->GetGuestMemoryBase() + guest, addr, static_cast<size_t>(capacity));
    g_direct_shadows[addr] = {guest, addr, capacity};
    LOGI("direct buffer %p (%lld bytes) shadowed at guest 0x%08X", addr, (long long)capacity, guest);
    return guest;
}

// Pushes whatever the guest wrote into the shadow back to the real buffer.
void FlushGuestDirectBuffer(JNIEnv* env, jobject buffer) {
    DynarmicHost* host = GetDynarmicHostForJni();
    if (!env || !buffer || !host) return;
    void* addr = env->GetDirectBufferAddress(buffer);
    if (!addr) return;
    std::lock_guard<std::mutex> lock(g_registry);
    auto it = g_direct_shadows.find(addr);
    if (it == g_direct_shadows.end()) return;
    memcpy(addr, host->GetGuestMemoryBase() + it->second.guest,
           static_cast<size_t>(it->second.capacity));
}

// Captured from the UI thread, where FindClass still resolves normally.
void BindGuestClassLoader(JNIEnv* env, jobject activity) {
    if (!env || !activity || g_class_loader) return;
    jclass activity_cls = env->GetObjectClass(activity);
    jmethodID get_loader =
        env->GetMethodID(activity_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = get_loader ? env->CallObjectMethod(activity, get_loader) : nullptr;
    if (!loader) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("could not capture the activity class loader - guest threads will not find app classes");
        return;
    }
    jclass loader_cls = env->GetObjectClass(loader);
    g_load_class = env->GetMethodID(loader_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    g_class_loader = env->NewGlobalRef(loader);
    LOGI("app class loader captured");
}
