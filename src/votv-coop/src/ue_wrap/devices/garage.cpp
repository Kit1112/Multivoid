// ue_wrap/devices/garage.cpp -- see ue_wrap/devices/garage.h. Engine access for the base garage
// door (Agarage_C). Offsets resolved from the live class via reflection (version-portable); the
// Alpha 0.9.0-n values are logged fallbacks.

#include "ue_wrap/devices/garage.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::garage {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_garageCls    = nullptr;  // garage_C UClass
int32_t g_openOff      = -1;       // Agarage_C::Open     (0x02E8)
void*   g_acivaeFn     = nullptr;  // acivae() / acivate() / activate() -- animated swing
void*   g_settimeFn    = nullptr;  // settime() -- snap fallback
void*   g_runTriggerFn = nullptr;  // runTrigger() -- triggerBase_C verb fallback

constexpr int32_t kOpenOffFallback = 0x02E8;

void* FindFuncClimbing(void* startCls, const wchar_t* name) {
    if (!startCls || !name) return nullptr;
    for (void* cur = startCls; cur; cur = R::SuperStructOf(cur)) {
        if (void* fn = R::FindFunction(cur, name)) return fn;
    }
    return nullptr;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = R::FindClass(L"garage_C");
    if (!cls) cls = R::FindClass(L"garageDoor_C");
    if (!cls) cls = R::FindClass(L"door_garage_C");
    if (!cls) return false;

    int32_t openOff = R::FindPropertyOffset(cls, L"Open");
    if (openOff < 0) openOff = R::FindPropertyOffset(cls, L"open");
    if (openOff < 0) openOff = R::FindPropertyOffset(cls, L"bOpen");
    if (openOff < 0) openOff = R::FindPropertyOffset(cls, L"isOpened");
    if (openOff < 0) openOff = R::FindPropertyOffset(cls, L"isOpen");
    if (openOff < 0) {
        UE_LOGW("garage: reflected Open offset not found -- using fallback 0x%04X", kOpenOffFallback);
        openOff = kOpenOffFallback;
    }

    void* acivaeFn = FindFuncClimbing(cls, L"acivae");
    if (!acivaeFn) acivaeFn = FindFuncClimbing(cls, L"acivate");
    if (!acivaeFn) acivaeFn = FindFuncClimbing(cls, L"activate");

    void* settimeFn = FindFuncClimbing(cls, L"settime");
    void* runTriggerFn = FindFuncClimbing(cls, L"runTrigger");

    g_garageCls    = cls;
    g_openOff      = openOff;
    g_acivaeFn     = acivaeFn;
    g_settimeFn    = settimeFn;
    g_runTriggerFn = runTriggerFn;
    g_resolved.store(true, std::memory_order_release);

    UE_LOGI("garage: resolved garage_C=%p Open@0x%04X acivae=%p settime=%p runTrigger=%p (canonical identity)",
            cls, openOff, acivaeFn, settimeFn, runTriggerFn);
    return true;
}

bool IsGarage(void* obj) {
    if (!obj || !g_garageCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_garageCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetNameKey(void* g) {
    if (!g) return std::wstring();
    return L"garage";
}

bool TryReadOpen(void* g, bool& open) {
    if (!g || g_openOff < 0) return false;
    open = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(g) + g_openOff);
    return true;
}

bool ApplyOpen(void* g, bool open) {
    if (!g) return false;
    // Idempotent: if already in the target state, do nothing (skip the re-trigger + the echo).
    bool cur = false;
    if (TryReadOpen(g, cur) && cur == open) return true;

    // Direct write to the Open property first:
    if (g_openOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(g) + g_openOff) = open;

    // Preferred: animated swing through acivae() / activate()
    if (g_acivaeFn) {
        ParamFrame f(g_acivaeFn);
        if (f.valid() && Call(g, f)) return true;
    }

    // Fallback 1: settime()
    if (g_settimeFn) {
        ParamFrame f(g_settimeFn);
        if (f.valid() && Call(g, f)) return true;
    }

    // Fallback 2: runTrigger(owner=nullptr, index=0)
    if (g_runTriggerFn) {
        ParamFrame f(g_runTriggerFn);
        if (f.valid()) {
            f.Set<void*>(L"owner", nullptr);
            f.Set<int32_t>(L"index", 0);
            if (Call(g, f)) return true;
        }
    }

    return g_openOff >= 0;
}

}  // namespace ue_wrap::garage
