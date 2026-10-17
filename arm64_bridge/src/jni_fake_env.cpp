#include "jni_fake_env.h"
#include "dynarmic_host.h"
#include "thunk_manager.h"
#include "thunk_types.h"
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#define LOG_TAG "Arm32Bridge_FakeJni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// JNIEnv table slot count (through GetObjectRefType)
static thread_local JNIEnv* thread_local_env = nullptr;

static const uint32_t kEnvSlots = 233;

static const uint32_t kEnvTableBytes = kEnvSlots * 4;
// JavaVM (JNIInvokeInterface) slots: 0..7
static const uint32_t kVmSlots = 8;

FakeJniEnv& FakeJniEnv::Instance() {
    static FakeJniEnv inst;
    return inst;
}

void FakeJniEnv::Init(DynarmicHost& host) {
    if (m_host == &host) return;
    // Host was recreated (first load attempt failed before the Activity
    // existed) � old tables pointed into freed guest memory.
    m_host = &host;
    m_env_table = 0;
    m_vm_table = 0;
    m_next_obj_handle = 0x00020000;
    m_next_mid_handle = 0x00030000;
    m_next_fid_handle = 0x00040000;
    m_objects.clear();
    m_objects_by_hash.clear();
    m_lru.clear();
    m_methods.clear();
    m_fields.clear();
    m_method_ids.clear();
    m_field_ids.clear();
    m_direct_bufs.clear();
    m_jstr_utf.clear();
    ThunkManager& tm = host.GetThunkManager();

    // Guest memory for tables
    m_env_table = host.GuestAlloc(kEnvTableBytes, 4);
    m_vm_table = host.GuestAlloc(kVmSlots * 4, 4);

    uint8_t* mem = host.GetGuestMemoryBase();
    auto register_slot = [&](uint32_t table, uint32_t offset, bool is_vm, const char* name) {
        uint32_t thunk = tm.RegisterThunk(
            std::string("jni_") + (is_vm ? "vm" : "env") + "_" + std::to_string(offset),
            [this, offset, is_vm](Dynarmic::A32::Jit& jit, DynarmicHost&) {
                if (is_vm) DispatchVmSlot(offset, jit);
                else DispatchEnvSlot(offset, jit);
            });
        host.MemoryWrite32(table + offset, thunk);
    };

    // Register all env slots: known offsets get real handlers (via DispatchEnvSlot
    // switch), everything else logs-and-zeroes on first use.
    for (uint32_t off = 0; off < kEnvSlots; ++off) {
        register_slot(m_env_table, off * 4, false, nullptr);
    }
    for (uint32_t off = 0; off < kVmSlots; ++off) {
        register_slot(m_vm_table, off * 4, true, nullptr);
    }

    // JNI ABI: guest code does `ldr r, [env]; ldr r, [r, #off]; blx r` �
    // i.e. [env] must point to the function-pointer table itself. Slot 0 of
    // both tables is reserved by the spec, so make it a self-pointer.
    host.MemoryWrite32(m_env_table, m_env_table);
    host.MemoryWrite32(m_vm_table, m_vm_table);

    LOGI("Fake JNI tables: env=0x%08X (%u bytes), vm=0x%08X", m_env_table, kEnvTableBytes, m_vm_table);
}

void FakeJniEnv::AttachHostEnv(JNIEnv* env) {
    thread_local_env = env;
}

// A thread that attaches itself and then exits still attached leaves the
// runtime's Thread object behind, and a checked build aborts the process over
// it. Guest worker threads (audio, streaming, a JIT's own helpers) come and go
// and none of them knows it is talking to a bridge, so the detach hangs off a
// TLS destructor instead of being left to whoever created the thread. Only
// threads attached here get the key set - detaching one the VM owns is worse
// than leaking it.
static pthread_key_t s_detach_key;
static JavaVM* s_detach_vm = nullptr;

static void DetachOnThreadExit(void*) {
    if (s_detach_vm) s_detach_vm->DetachCurrentThread();
    thread_local_env = nullptr;
}

JNIEnv* FakeJniEnv::GetEnv() {
    if (thread_local_env) return thread_local_env;
    if (!m_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        thread_local_env = env;
        return env;
    }
    if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        thread_local_env = env;
        static std::once_flag once;
        std::call_once(once, [this] {
            s_detach_vm = m_vm;
            pthread_key_create(&s_detach_key, DetachOnThreadExit);
        });
        pthread_setspecific(s_detach_key, reinterpret_cast<void*>(1));
    }
    return env;
}

// -------------------------------------------------------------
// Handles
// -------------------------------------------------------------

// Cap on live global refs. ART's own limit is 51200 and hitting it kills the
// process outright, so the table has to stay well under it and reclaim rather
// than stop caching - a "cache full, hand out a ref and forget it" branch is the
// original leak with a delay on it.
static const size_t kMaxObjectRefs = 8192;

int32_t FakeJniEnv::IdentityHash(JNIEnv* env, jobject o) {
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
    int32_t h = env->CallStaticIntMethod(system, identity_hash, o);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    return h;
}

void FakeJniEnv::TouchRef(uint32_t handle) const {
    auto it = m_objects.find(handle);
    if (it != m_objects.end() && it->second.pins == 0) {
        m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
    }
}

void FakeJniEnv::EvictOneRef(JNIEnv* env) {
    if (m_lru.empty()) return;  // everything pinned; the table grows past the cap
    uint32_t handle = m_lru.back();
    m_lru.pop_back();

    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return;

    auto bucket = m_objects_by_hash.find(it->second.hash);
    if (bucket != m_objects_by_hash.end()) {
        auto& v = bucket->second;
        v.erase(std::remove(v.begin(), v.end(), handle), v.end());
        if (v.empty()) m_objects_by_hash.erase(bucket);
    }
    // The UTF-8 copy of a jstring lives as long as the jstring's handle does;
    // leaving it behind strands the arena block and, worse, keeps a dead handle
    // mapped to it.
    auto utf = m_jstr_utf.find(handle);
    if (utf != m_jstr_utf.end()) {
        if (m_host) m_host->GuestFree(utf->second);
        m_jstr_utf.erase(utf);
    }
    env->DeleteGlobalRef(it->second.global);
    m_objects.erase(it);
}

uint32_t FakeJniEnv::AddObjectRef(jobject o) {
    if (!o) return 0;
    JNIEnv* env = GetEnv();
    if (!env) return 0;
    int32_t hash = IdentityHash(env, o);

    std::lock_guard<std::mutex> lk(m_mutex);
    for (uint32_t h : m_objects_by_hash[hash]) {
        auto it = m_objects.find(h);
        if (it != m_objects.end() && env->IsSameObject(it->second.global, o)) {
            TouchRef(h);
            return h;
        }
    }
    // Eviction rehashes both maps, so nothing looked up above survives it.
    if (m_objects.size() >= kMaxObjectRefs) EvictOneRef(env);

    jobject global = env->NewGlobalRef(o);
    if (!global) return 0;
    uint32_t h = m_next_obj_handle;
    m_next_obj_handle += 4;

    ObjRef& ref = m_objects[h];
    ref.global = global;
    ref.hash = hash;
    ref.pins = 0;
    m_lru.push_front(h);
    ref.lru = m_lru.begin();
    m_objects_by_hash[hash].push_back(h);
    return h;
}

jobject FakeJniEnv::ResolveObject(uint32_t handle) const {
    if (!handle) return nullptr;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return nullptr;
    // Resolving counts as using: an object a slot is part way through working
    // with must not be the one another thread picks to evict.
    TouchRef(handle);
    return it->second.global;
}

void FakeJniEnv::PinObjectRef(uint32_t handle, int delta) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return;
    ObjRef& ref = it->second;
    const bool was = ref.pins > 0;
    ref.pins += delta;
    if (ref.pins < 0) ref.pins = 0;
    const bool now = ref.pins > 0;
    if (now == was) return;
    if (now) {
        m_lru.erase(ref.lru);
    } else {
        m_lru.push_front(handle);
        ref.lru = m_lru.begin();
    }
}

void FakeJniEnv::DemoteObjectRef(uint32_t handle) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it != m_objects.end() && it->second.pins == 0) {
        m_lru.splice(m_lru.end(), m_lru, it->second.lru);
    }
}

// One id per jmethodID. Guests that reach Java through reflection ask for the
// same method on every call; appending unconditionally grew this table, and the
// signature string in each entry, for the life of the process.
//
// Dedup by mid alone is only correct because handles are interned: clsHandle for
// a given class is now stable, so the first registration's copy stays right.
uint32_t FakeJniEnv::AddMethod(jmethodID mid, bool isStatic, uint32_t clsHandle, const std::string& sig) {
    if (!mid) return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto known = m_method_ids.find(mid);
    if (known != m_method_ids.end()) return known->second;

    uint32_t h = m_next_mid_handle;
    m_next_mid_handle += 4;
    MethodInfo mi;
    mi.mid = mid;
    mi.isStatic = isStatic;
    mi.clsHandle = clsHandle;
    mi.sig = sig;
    m_methods[h] = mi;
    m_method_ids[mid] = h;
    return h;
}

uint32_t FakeJniEnv::AddField(jfieldID fid) {
    if (!fid) return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto known = m_field_ids.find(fid);
    if (known != m_field_ids.end()) return known->second;

    uint32_t h = m_next_fid_handle;
    m_next_fid_handle += 4;
    m_fields[h] = fid;
    m_field_ids[fid] = h;
    return h;
}

// -------------------------------------------------------------
// Signature parsing & guest arg reading
// -------------------------------------------------------------

std::vector<char> FakeJniEnv::ParseParams(const std::string& sig) const {
    std::vector<char> out;
    size_t i = sig.find('(');
    if (i == std::string::npos) return out;
    for (++i; i < sig.size() && sig[i] != ')'; ++i) {
        char c = sig[i];
        if (c == 'L') {
            while (sig[i] != ';') ++i;
            out.push_back('L');
        } else if (c == '[') {
            while (sig[i] == '[') ++i;
            if (sig[i] == 'L') while (sig[i] != ';') ++i;
            out.push_back('L');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// A-variant: r3 points to an array of jvalue (8 bytes each on ARM32)
std::vector<FakeJniEnv::GuestArg> FakeJniEnv::ReadArgsA(Dynarmic::A32::Jit& jit,
                                                        const std::vector<char>& params,
                                                        uint32_t jvalue_guest) {
    std::vector<GuestArg> args;
    uint8_t* mem = m_host->GetGuestMemoryBase();
    uint32_t p = jvalue_guest;
    for (char t : params) {
        GuestArg a;
        a.type = t;
        switch (t) {
            case 'L':
                a.obj = ResolveObject(*reinterpret_cast<uint32_t*>(mem + p));
                break;
            case 'J':
                a.v = *reinterpret_cast<uint64_t*>(mem + p);
                break;
            case 'D':
                a.v = *reinterpret_cast<uint64_t*>(mem + p);
                break;
            case 'F': {
                float f; memcpy(&f, mem + p, 4);
                a.v = *reinterpret_cast<uint32_t*>(mem + p);
                break;
            }
            default:
                a.v = static_cast<uint32_t>(*reinterpret_cast<uint32_t*>(mem + p));
                break;
        }
        args.push_back(a);
        p += 8;
    }
    return args;
}

// V-variant: r3 is a va_list (guest stack pointer); 64-bit slots aligned to 8
std::vector<FakeJniEnv::GuestArg> FakeJniEnv::ReadArgsV(Dynarmic::A32::Jit& jit,
                                                        const std::vector<char>& params,
                                                        uint32_t va_list_guest) {
    std::vector<GuestArg> args;
    uint8_t* mem = m_host->GetGuestMemoryBase();
    uint32_t p = va_list_guest;
    for (char t : params) {
        GuestArg a;
        a.type = t;
        if (t == 'J' || t == 'D') {
            p = (p + 7) & ~7u;
            a.v = *reinterpret_cast<uint64_t*>(mem + p);
            p += 8;
        } else {
            uint32_t raw = *reinterpret_cast<uint32_t*>(mem + p);
            if (t == 'L') a.obj = ResolveObject(raw);
            else a.v = raw;
            p += 4;
        }
        args.push_back(a);
    }
    return args;
}

// Plain vararg: first vararg in r3, then guest stack
std::vector<FakeJniEnv::GuestArg> FakeJniEnv::ReadArgsVar(Dynarmic::A32::Jit& jit,
                                                          const std::vector<char>& params) {
    std::vector<GuestArg> args;
    uint8_t* mem = m_host->GetGuestMemoryBase();
    uint32_t p = jit.Regs()[13];
    bool first = true;
    for (char t : params) {
        GuestArg a;
        a.type = t;
        uint32_t raw;
        if (first) { raw = jit.Regs()[3]; first = false; }
        else {
            raw = *reinterpret_cast<uint32_t*>(mem + p);
            p += 4;
        }
        if (t == 'J' || t == 'D') {
            uint32_t hi;
            if (first) { hi = *reinterpret_cast<uint32_t*>(mem + p); p += 4; first = false; }
            else { hi = *reinterpret_cast<uint32_t*>(mem + p); p += 4; }
            a.v = static_cast<uint64_t>(raw) | (static_cast<uint64_t>(hi) << 32);
        } else if (t == 'L') {
            a.obj = ResolveObject(raw);
        } else {
            a.v = raw;
        }
        args.push_back(a);
    }
    return args;
}

// -------------------------------------------------------------
// Host-side call marshaling
// -------------------------------------------------------------

void FakeJniEnv::CallMethod(Dynarmic::A32::Jit& jit, uint32_t mid_handle, uint32_t obj_handle,
                            char retType, const std::vector<GuestArg>& args, bool isStatic) {
    MethodInfo mi;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto mi_it = m_methods.find(mid_handle);
        if (mi_it == m_methods.end()) {
            LOGE("CallMethod: unknown method handle 0x%08X", mid_handle);
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
            return;
        }
        mi = mi_it->second; // copy; host JNI call below runs without the lock
    }
    jobject obj = ResolveObject(obj_handle);
    if (!obj) {
        jit.Regs()[0] = 0;
        jit.Regs()[1] = 0;
        return;
    }
    {
    }

    std::vector<jvalue> jvs(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        jvalue& jv = jvs[i];
        const GuestArg& a = args[i];
        switch (a.type) {
            case 'Z': jv.z = static_cast<jboolean>(a.v); break;
            case 'B': jv.b = static_cast<jbyte>(a.v); break;
            case 'C': jv.c = static_cast<jchar>(a.v); break;
            case 'S': jv.s = static_cast<jshort>(a.v); break;
            case 'I': jv.i = static_cast<jint>(a.v); break;
            case 'J': jv.j = static_cast<jlong>(a.v); break;
            case 'F': { float f; memcpy(&f, &a.v, 4); jv.f = f; break; }
            case 'D': { double d; memcpy(&d, &a.v, 8); jv.d = d; break; }
            case 'L': jv.l = a.obj; break;
        }
    }

    JNIEnv* env = GetEnv();
    bool pending = env->ExceptionCheck();
    if (pending) { env->ExceptionClear(); }

    switch (retType) {
        case 'V':
            if (isStatic) env->CallStaticVoidMethodA(static_cast<jclass>(obj), mi.mid, jvs.data());
            else env->CallVoidMethodA(obj, mi.mid, jvs.data());
            break;
        case 'Z': {
            jboolean r = isStatic ? env->CallStaticBooleanMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                  : env->CallBooleanMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = r ? 1 : 0;
            break;
        }
        case 'B': case 'C': case 'S': {
            jint r = isStatic ? env->CallStaticIntMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                              : env->CallIntMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(r);
            break;
        }
        case 'I': {
            jint r = isStatic ? env->CallStaticIntMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                              : env->CallIntMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(r);
            break;
        }
        case 'J': {
            jlong r = isStatic ? env->CallStaticLongMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                               : env->CallLongMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(r & 0xFFFFFFFFu);
            jit.Regs()[1] = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
            break;
        }
        case 'F': {
            jfloat r = isStatic ? env->CallStaticFloatMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                : env->CallFloatMethodA(obj, mi.mid, jvs.data());
            WriteSF(jit, 0, r);
            break;
        }
        case 'D': {
            jdouble r = isStatic ? env->CallStaticDoubleMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                 : env->CallDoubleMethodA(obj, mi.mid, jvs.data());
            WriteDF(jit, 0, r);
            break;
        }
        case 'L': {
            jobject r = isStatic ? env->CallStaticObjectMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                 : env->CallObjectMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = AddObjectRef(r);
            break;
        }
        default:
            jit.Regs()[0] = 0;
            break;
    }
}

// -------------------------------------------------------------
// Direct ByteBuffer bridging
// -------------------------------------------------------------

uint32_t FakeJniEnv::RegisterDirectBuffer(jobject buf) {
    JNIEnv* env = GetEnv();
    if (!env) return 0;
    // fmodProcess hands us the same ByteBuffer on every audio callback (~125/s).
    // A fresh entry per call burned a permanent JNI global ref and a
    // capacity-sized arena block each time. One entry per live buffer, so the
    // scan is over one or two elements.
    for (const auto& [h, db] : m_direct_bufs) {
        if (env->IsSameObject(db.jobj, buf)) return h;
    }
    void* p = env->GetDirectBufferAddress(buf);
    jlong cap = env->GetDirectBufferCapacity(buf);
    if (!p || cap <= 0) return 0;
    uint32_t handle = m_next_obj_handle;
    m_next_obj_handle += 4;
    DirectBuf db;
    db.jobj = env->NewGlobalRef(buf);
    db.hostPtr = p;
    db.capacity = cap;
    db.scratch = m_host->GuestAlloc(static_cast<size_t>(cap), 16);
    m_direct_bufs[handle] = db;
    return handle;
}

void FakeJniEnv::CommitDirectBuffer(uint32_t handle) {
    auto it = m_direct_bufs.find(handle);
    if (it == m_direct_bufs.end()) return;
    DirectBuf& db = it->second;
    memcpy(db.hostPtr, m_host->GetGuestMemoryBase() + db.scratch, static_cast<size_t>(db.capacity));
}

jlong FakeJniEnv::DirectBufferCapacity(uint32_t handle) const {
    auto it = m_direct_bufs.find(handle);
    return (it != m_direct_bufs.end()) ? it->second.capacity : 0;
}

// -------------------------------------------------------------
// Dispatch
// -------------------------------------------------------------

// Helpers
static const char* ReadGuestStr(DynarmicHost& host, uint32_t gaddr) {
    if (!gaddr) return nullptr;
    return reinterpret_cast<const char*>(host.GetGuestMemoryBase() + gaddr);
}

void FakeJniEnv::DispatchVmSlot(uint32_t offset, Dynarmic::A32::Jit& jit) {
    LOGI("[JNI-VM] slot %u (off 0x%X) r1=0x%08X", offset / 4, offset, jit.Regs()[1]);
    switch (offset) {
        case 16: // AttachCurrentThread(vm, penv, args)
        case 24: // GetEnv(vm, penv, version)
        case 28: // AttachCurrentThreadAsDaemon(vm, penv, args)
        {
            uint32_t out = jit.Regs()[1];
            if (out) m_host->MemoryWrite32(out, m_env_table);
            jit.Regs()[0] = 0; // JNI_OK
            break;
        }
        case 20: // DetachCurrentThread(vm)
        {
            jit.Regs()[0] = 0; // JNI_OK
            break;
        }
        default:
            static bool warned_vm[64];
            if (offset / 4 < 64 && !warned_vm[offset / 4]) {
                warned_vm[offset / 4] = true;
                LOGI("[JNI-VM] unhandled slot %u (off 0x%X) at PC 0x%08X", offset / 4, offset, jit.Regs()[15]);
            }
            jit.Regs()[0] = 0;
            break;
    }
}

void FakeJniEnv::DispatchEnvSlot(uint32_t offset, Dynarmic::A32::Jit& jit) {
    JNIEnv* e = GetEnv();
    uint8_t* mem = m_host->GetGuestMemoryBase();

    if (!e) {
        LOGE("[JNI] no host env attached (slot %u)", offset / 4);
        jit.Regs()[0] = 0;
        return;
    }

    switch (offset) {
        // ---- class / object basics ----
        case 24: { // FindClass
            const char* name = ReadGuestStr(*m_host, jit.Regs()[1]);
            jclass c = e->FindClass(name);
            if (!c) { e->ExceptionClear(); jit.Regs()[0] = 0; }
            else jit.Regs()[0] = AddObjectRef(c);
            break;
        }
        case 40: { // GetSuperclass
            jobject o = ResolveObject(jit.Regs()[1]);
            jclass c = e->GetSuperclass(static_cast<jclass>(o));
            jit.Regs()[0] = c ? AddObjectRef(c) : 0;
            break;
        }
        // Handles are shared, so dropping the host ref in DeleteGlobalRef/
        // DeleteLocalRef would invalidate any copy the guest still holds. What
        // the three calls do instead is set how willing the table is to reclaim
        // the entry - which is as close to the real lifetime as a shared handle
        // gets, and it is what keeps the table off ART's 51200 limit.
        case 84: { // NewGlobalRef -> same handle, plus a pin
            jobject o = ResolveObject(jit.Regs()[1]);
            if (!o) { jit.Regs()[0] = 0; break; }
            PinObjectRef(jit.Regs()[1], +1);
            jit.Regs()[0] = jit.Regs()[1];
            break;
        }
        case 88: // DeleteGlobalRef -> drop the pin
            PinObjectRef(jit.Regs()[1], -1);
            jit.Regs()[0] = 0;
            break;
        case 92: // DeleteLocalRef -> next in line to be evicted
            DemoteObjectRef(jit.Regs()[1]);
            jit.Regs()[0] = 0;
            break;
        case 100: { // NewLocalRef
            jobject o = ResolveObject(jit.Regs()[1]);
            jit.Regs()[0] = AddObjectRef(o);
            break;
        }
        case 124: { // GetObjectClass
            jobject o = ResolveObject(jit.Regs()[1]);
            if (!o) { jit.Regs()[0] = 0; break; } // guest expects null, ART would abort
            jclass c = e->GetObjectClass(o);
            jit.Regs()[0] = c ? AddObjectRef(c) : 0;
            break;
        }
        case 128: { // IsInstanceOf
            jobject o = ResolveObject(jit.Regs()[1]);
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[2]));
            jit.Regs()[0] = e->IsInstanceOf(o, c) ? 1 : 0;
            break;
        }

        // ---- method/field IDs ----
        case 132: { // GetMethodID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            if (!c) { jit.Regs()[0] = 0; break; }
            jmethodID mid = e->GetMethodID(c, name, sig);
            if (!mid) {
                e->ExceptionClear();
                LOGE("[JNI] GetMethodID failed: %s%s (cls=0x%08X)", name, sig, jit.Regs()[1]);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddMethod(mid, false, jit.Regs()[1], sig);
            }
            break;
        }
        case 376: { // GetFieldID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            jfieldID fid = e->GetFieldID(c, name, sig);
            if (!fid) {
                e->ExceptionClear();
                LOGE("[JNI] GetFieldID failed: %s%s", name, sig);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddField(fid);
            }
            break;
        }
        case 452: { // GetStaticMethodID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            if (!c) { jit.Regs()[0] = 0; break; }
            jmethodID mid = e->GetStaticMethodID(c, name, sig);
            if (!mid) {
                e->ExceptionClear();
                LOGE("[JNI] GetStaticMethodID failed: %s%s (cls=0x%08X)", name, sig, jit.Regs()[1]);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddMethod(mid, true, jit.Regs()[1], sig);
            }
            break;
        }

        // ---- object creation ----
        case 112: // NewObject (varargs)
        case 116: { // NewObjectV / NewObjectA region
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            uint32_t mid_handle = jit.Regs()[2];
            auto mi_it = m_methods.find(mid_handle);
            if (mi_it == m_methods.end()) { jit.Regs()[0] = 0; break; }
            MethodInfo& mi = mi_it->second;
            std::vector<char> params = ParseParams(mi.sig);
            std::vector<GuestArg> args = (offset == 112) ? ReadArgsVar(jit, params)
                                                         : ReadArgsV(jit, params, jit.Regs()[3]);
            // Build jvalues and call NewObjectA
            std::vector<jvalue> jvs(args.size());
            for (size_t i = 0; i < args.size(); ++i) {
                const GuestArg& a = args[i];
                switch (a.type) {
                    case 'Z': jvs[i].z = static_cast<jboolean>(a.v); break;
                    case 'I': jvs[i].i = static_cast<jint>(a.v); break;
                    case 'J': jvs[i].j = static_cast<jlong>(a.v); break;
                    case 'F': { float f; memcpy(&f, &a.v, 4); jvs[i].f = f; break; }
                    case 'L': jvs[i].l = a.obj; break;
                    default: jvs[i].i = static_cast<jint>(a.v); break;
                }
            }
            jobject o = e->NewObjectA(c, mi.mid, jvs.data());
            if (!o) { e->ExceptionClear(); LOGE("[JNI] NewObject failed"); jit.Regs()[0] = 0; }
            else jit.Regs()[0] = AddObjectRef(o);
            break;
        }

        // ---- instance method calls (V and A variants) ----
        case 136: { // CallObjectMethod
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', ReadArgsVar(jit, params), false);
            break;
        }
        case 140: { // CallObjectMethodV
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', ReadArgsV(jit, params, jit.Regs()[3]), false);
            break;
        }
        case 144: { // CallObjectMethodA
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', ReadArgsA(jit, params, jit.Regs()[3]), false);
            break;
        }
        case 148: case 152: case 156: { // CallBooleanMethod / V / A
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            std::vector<GuestArg> args = (offset == 148) ? ReadArgsVar(jit, params)
                                        : (offset == 152) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'Z', args, false);
            break;
        }
        case 196: case 200: case 204: { // CallIntMethod / V / A
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            std::vector<GuestArg> args = (offset == 196) ? ReadArgsVar(jit, params)
                                        : (offset == 200) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'I', args, false);
            break;
        }
        case 208: case 212: case 216: { // CallLongMethod / V / A
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            std::vector<GuestArg> args = (offset == 208) ? ReadArgsVar(jit, params)
                                        : (offset == 212) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'J', args, false);
            break;
        }
        case 244: case 248: case 252: { // CallVoidMethod / V / A
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            std::vector<GuestArg> args = (offset == 244) ? ReadArgsVar(jit, params)
                                        : (offset == 248) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'V', args, false);
            break;
        }

        // ---- static method calls ----
        case 456: case 460: case 464: { // CallStaticObjectMethod / V / A
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            std::vector<GuestArg> args = (offset == 456) ? ReadArgsVar(jit, params)
                                        : (offset == 460) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', args, true);
            break;
        }
        case 468: case 472: case 476: { // CallStaticBooleanMethod / V / A
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            std::vector<GuestArg> args = (offset == 468) ? ReadArgsVar(jit, params)
                                        : (offset == 472) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'Z', args, true);
            break;
        }
        case 516: case 520: case 524: { // CallStaticIntMethod / V / A
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            std::vector<GuestArg> args = (offset == 516) ? ReadArgsVar(jit, params)
                                        : (offset == 520) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'I', args, true);
            break;
        }
        case 528: case 532: case 536: { // CallStaticLongMethod / V / A
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            std::vector<GuestArg> args = (offset == 528) ? ReadArgsVar(jit, params)
                                        : (offset == 532) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'J', args, true);
            break;
        }
        case 564: case 568: case 572: { // CallStaticVoidMethod / V / A
            std::vector<char> params = ParseParams(m_methods[jit.Regs()[2]].sig);
            std::vector<GuestArg> args = (offset == 564) ? ReadArgsVar(jit, params)
                                        : (offset == 568) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'V', args, true);
            break;
        }

        // ---- field access ----
        case 400: { // GetIntField
            jobject o = ResolveObject(jit.Regs()[1]);
            auto it = m_fields.find(jit.Regs()[2]);
            if (!o || it == m_fields.end()) { jit.Regs()[0] = 0; break; }
            jit.Regs()[0] = static_cast<uint32_t>(e->GetIntField(o, it->second));
            break;
        }
        case 380: { // GetObjectField
            jobject o = ResolveObject(jit.Regs()[1]);
            auto it = m_fields.find(jit.Regs()[2]);
            if (!o || it == m_fields.end()) { jit.Regs()[0] = 0; break; }
            jobject r = e->GetObjectField(o, it->second);
            jit.Regs()[0] = r ? AddObjectRef(r) : 0;
            break;
        }

        // ---- strings ----
        case 668: { // NewStringUTF
            const char* s = ReadGuestStr(*m_host, jit.Regs()[1]);
            jstring js = e->NewStringUTF(s ? s : "");
            jit.Regs()[0] = AddObjectRef(js);
            break;
        }
        case 656: { // GetStringLength
            jstring s = static_cast<jstring>(ResolveObject(jit.Regs()[1]));
            jit.Regs()[0] = s ? static_cast<uint32_t>(e->GetStringLength(s)) : 0;
            break;
        }
        case 660: { // GetStringChars (UTF-16)
            jstring s = static_cast<jstring>(ResolveObject(jit.Regs()[1]));
            if (!s) { jit.Regs()[0] = 0; break; }
            const jchar* chars = e->GetStringChars(s, nullptr);
            jsize len = e->GetStringLength(s);
            uint32_t guest = m_host->GuestAlloc((len + 1) * 2, 4);
            memcpy(mem + guest, chars, len * 2);
            memset(mem + guest + len * 2, 0, 2);
            e->ReleaseStringChars(s, chars);
            jit.Regs()[0] = guest;
            break;
        }
        case 664: // ReleaseStringChars -> the copy is ours, and this is the only
                  // call that says when it stops being needed
            m_host->GuestFree(jit.Regs()[2]);
            jit.Regs()[0] = 0;
            break;
        case 676: { // GetStringUTFChars
            jstring s = static_cast<jstring>(ResolveObject(jit.Regs()[1]));
            uint32_t isCopyOut = jit.Regs()[2];
            if (!s) { jit.Regs()[0] = 0; break; }
            auto cached = m_jstr_utf.find(jit.Regs()[1]);
            if (cached != m_jstr_utf.end()) {
                if (isCopyOut) m_host->MemoryWrite8(isCopyOut, 0);
                jit.Regs()[0] = cached->second;
                break;
            }
            const char* utf = e->GetStringUTFChars(s, nullptr);
            size_t len = utf ? strlen(utf) + 1 : 1;
            uint32_t guest = m_host->GuestAlloc(len, 4);
            memcpy(mem + guest, utf ? utf : "", len);
            e->ReleaseStringUTFChars(s, utf);
            m_jstr_utf[jit.Regs()[1]] = guest;
            if (isCopyOut) m_host->MemoryWrite8(isCopyOut, 0); // JNI_FALSE
            jit.Regs()[0] = guest;
            break;
        }
        case 680: // ReleaseStringUTFChars -> no-op on purpose: unlike
                  // GetStringChars this copy is cached in m_jstr_utf and handed
                  // back on the next call, so it lives exactly as long as the
                  // jstring's handle and is freed when that handle is evicted
            jit.Regs()[0] = 0;
            break;

        // ---- exceptions ----
        case 60: { // ExceptionOccurred
            jthrowable t = e->ExceptionOccurred();
            jit.Regs()[0] = t ? AddObjectRef(t) : 0;
            break;
        }
        case 64: // ExceptionDescribe
            e->ExceptionDescribe();
            jit.Regs()[0] = 0;
            break;
        case 68: // ExceptionClear
            e->ExceptionClear();
            jit.Regs()[0] = 0;
            break;
        case 912: { // ExceptionCheck
            jit.Regs()[0] = e->ExceptionCheck() ? 1 : 0;
            break;
        }
        case 56: { // ThrowNew
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* msg = ReadGuestStr(*m_host, jit.Regs()[2]);
            if (c) e->ThrowNew(c, msg);
            jit.Regs()[0] = 0;
            break;
        }
        case 72: { // FatalError
            const char* msg = ReadGuestStr(*m_host, jit.Regs()[1]);
            LOGE("[JNI] FatalError: %s", msg ? msg : "");
            jit.Regs()[0] = 0;
            break;
        }

        // ---- direct buffers ----
        case 920: { // GetDirectBufferAddress
            uint32_t handle = jit.Regs()[1];
            auto it = m_direct_bufs.find(handle);
            if (it != m_direct_bufs.end()) {
                jit.Regs()[0] = it->second.scratch;
                break;
            }
            jobject buf = ResolveObject(handle);
            if (!buf) { jit.Regs()[0] = 0; break; }
            void* p = e->GetDirectBufferAddress(buf);
            if (!p) { jit.Regs()[0] = 0; break; }
            // Unknown buffer: mirror host content into guest scratch (read-only view)
            uint32_t newHandle = RegisterDirectBuffer(buf);
            jit.Regs()[0] = newHandle ? m_direct_bufs[newHandle].scratch : 0;
            break;
        }
        case 924: { // GetDirectBufferCapacity
            uint32_t handle = jit.Regs()[1];
            auto it = m_direct_bufs.find(handle);
            if (it != m_direct_bufs.end()) {
                jit.Regs()[0] = static_cast<uint32_t>(it->second.capacity);
                break;
            }
            jobject buf = ResolveObject(handle);
            jit.Regs()[0] = buf ? static_cast<uint32_t>(e->GetDirectBufferCapacity(buf)) : 0;
            break;
        }

        // ---- misc ----
        case 876: { // GetJavaVM
            uint32_t out = jit.Regs()[1];
            if (out) m_host->MemoryWrite32(out, m_vm_table);
            jit.Regs()[0] = 0;
            break;
        }
        case 96: { // IsSameObject
            jobject a = ResolveObject(jit.Regs()[1]);
            jobject b = ResolveObject(jit.Regs()[2]);
            jit.Regs()[0] = e->IsSameObject(a, b) ? 1 : 0;
            break;
        }
        case 52: { // Throw
            jobject t = ResolveObject(jit.Regs()[1]);
            if (t) e->Throw(static_cast<jthrowable>(t));
            jit.Regs()[0] = 0;
            break;
        }
        case 436: case 440: { // SetObjectField / SetIntField (unlikely)
            jobject o = ResolveObject(jit.Regs()[1]);
            auto it = m_fields.find(jit.Regs()[2]);
            if (o && it != m_fields.end()) {
                if (offset == 436) e->SetObjectField(o, it->second, ResolveObject(jit.Regs()[3]));
                else e->SetIntField(o, it->second, static_cast<jint>(jit.Regs()[3]));
            }
            jit.Regs()[0] = 0;
            break;
        }
        case 104: { // GetLongField
            jobject o = ResolveObject(jit.Regs()[1]);
            auto it = m_fields.find(jit.Regs()[2]);
            jlong v = (o && it != m_fields.end()) ? e->GetLongField(o, it->second) : 0;
            jit.Regs()[0] = static_cast<uint32_t>(v & 0xFFFFFFFFu);
            jit.Regs()[1] = static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32);
            break;
        }

        default: {
            static bool warned[240];
            uint32_t slot = offset / 4;
            if (slot < 240 && !warned[slot]) {
                warned[slot] = true;
                LOGE("[JNI] unhandled env slot %u (off 0x%X) at PC 0x%08X, r1=0x%08X r2=0x%08X r3=0x%08X",
                     slot, offset, jit.Regs()[15], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
            }
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
            break;
        }
    }
}
