// ue_wrap/actors/broom.cpp -- see ue_wrap/actors/broom.h.

#include "ue_wrap/actors/broom.h"

#include "ue_wrap/core/call.h"             // ParamFrame / Call
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace ue_wrap::broom {
namespace {

namespace R = reflection;
namespace P = profile;

// The names, resolved once per process on the game thread.
R::FName g_className{0, 0};
R::FName g_uberName{0, 0};
R::FName g_strokeName{0, 0};
std::wstring g_strokeNotifyFnName;
bool     g_namesResolved = false;

// The layout, read off the first broom a stroke is entered on.
int32_t g_holderOff     = -1;
int32_t g_notifyNameOff = -1;
int32_t g_sweptPileOff  = -1;
bool    g_layoutResolved = false;
bool    g_layoutAbsent   = false;   // a broom was in hand and the members were not: this build differs

bool SameName(const R::FName& a, const R::FName& b) {
    return a.ComparisonIndex == b.ComparisonIndex && a.Number == b.Number;
}

bool CallWithPlayer(void* broom, const wchar_t* fnName, void* player) {
    if (!broom || !R::IsLive(broom)) return false;
    // Revalidated on every hit, so a world unload cannot hand back a freed function.
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(broom), fnName);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(P::name::HandUsePlayerParam, player) && ue_wrap::Call(broom, f);
}

}  // namespace

bool ResolveNames() {
    if (g_namesResolved) return true;
    const R::FName cls = ue_wrap::fname_utils::StringToFName(P::name::BroomClass);
    const R::FName uber = ue_wrap::fname_utils::StringToFName(P::name::BroomUbergraphFn);
    const R::FName stroke = ue_wrap::fname_utils::StringToFName(P::name::BroomStrokeNotifyName);
    if (cls.ComparisonIndex == 0 || uber.ComparisonIndex == 0 || stroke.ComparisonIndex == 0) return false;
    void* broomClass = R::FindClass(P::name::BroomClass);
    if (!broomClass) return false;
    g_strokeNotifyFnName = P::name::BroomStrokeNotifyFn;
    if (!R::FindDispatchFunction(broomClass, g_strokeNotifyFnName.c_str(), nullptr)) {
        // Animation notify UFunctions include a cook-generated GUID. Keep the measured profile
        // name as the fast path, but recover after a recook by looking for the matching notify
        // signature on this class. IsStrokeNotify still checks that the runtime FName is "clean".
        g_strokeNotifyFnName.clear();
        for (const R::ObjectRef& child : R::ChildObjectsOf(broomClass)) {
            if (child.name.rfind(L"OnNotifyBegin_", 0) != 0) continue;
            if (R::FindParamOffset(child.object, P::name::BroomNotifyNameParam) >= 0) {
                g_strokeNotifyFnName = child.name;
                break;
            }
        }
        if (g_strokeNotifyFnName.empty()) return false;
        UE_LOGW("broom: profile notify '%ls' was recooked; using '%ls'", P::name::BroomStrokeNotifyFn,
                g_strokeNotifyFnName.c_str());
    }
    g_className = cls;
    g_uberName = uber;
    g_strokeName = stroke;
    g_namesResolved = true;
    return true;
}

const wchar_t* StrokeNotifyFunctionName() {
    return g_namesResolved ? g_strokeNotifyFnName.c_str() : L"";
}

bool IsBroom(void* obj) {
    if (!obj || !g_namesResolved) return false;
    void* cls = R::ClassOf(obj);
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (SameName(R::NameOf(cls), g_className)) return true;
        cls = R::SuperStructOf(cls);
    }
    return false;
}

bool ResolveLayout(void* broom) {
    if (g_layoutResolved) return true;
    if (g_layoutAbsent || !IsBroom(broom)) return false;
    void* cls = R::ClassOf(broom);
    const int32_t holderOff = R::FindPropertyOffset(cls, P::name::BroomHolderProp);
    void* notify = R::FindDispatchFunction(cls, g_strokeNotifyFnName.c_str(), nullptr);
    const int32_t nameOff = notify ? R::FindParamOffset(notify, P::name::BroomNotifyNameParam) : -1;
    // A function is a struct: its locals are properties of it like its parameters, at their
    // offsets into the frame the function runs in.
    void* uber = R::FindDispatchFunction(cls, P::name::BroomUbergraphFn, nullptr);
    const int32_t pileOff = uber ? R::FindPropertyOffset(uber, P::name::BroomSweptPileLocal) : -1;
    if (holderOff < 0 || nameOff < 0 || pileOff < 0) {
        g_layoutAbsent = true;
        UE_LOGW("broom: '%ls' lacks part of the stroke's layout in this build (holder@%d, %ls.%ls@%d, "
                "%ls.%ls@%d) -- a stroke cannot be refused, run for a remote player or named at its "
                "clumps' birth", R::ClassNameOf(broom).c_str(), holderOff, g_strokeNotifyFnName.c_str(),
                P::name::BroomNotifyNameParam, nameOff, P::name::BroomUbergraphFn,
                P::name::BroomSweptPileLocal, pileOff);
        return false;
    }
    g_holderOff = holderOff;
    g_notifyNameOff = nameOff;
    g_sweptPileOff = pileOff;
    g_layoutResolved = true;
    UE_LOGI("broom: the stroke's layout -- holder at +%d, notify name at +%d, the loop's pile at +%d "
            "of %ls", holderOff, nameOff, pileOff, P::name::BroomUbergraphFn);
    return true;
}

bool IsStrokeNotify(const uint8_t* locals) {
    if (!g_layoutResolved || !g_namesResolved || !locals) return false;
    R::FName n{};
    std::memcpy(&n, locals + g_notifyNameOff, sizeof(n));
    return SameName(n, g_strokeName);
}

void* SweptChipPile(void* broom, const ue_wrap::ufunction_hook::CallerFrame& frame) {
    if (!g_layoutResolved || !frame.function || !frame.locals) return nullptr;
    if (!SameName(R::NameOf(frame.function), g_uberName) || !IsBroom(broom)) return nullptr;
    void* pile = nullptr;
    std::memcpy(&pile, frame.locals + g_sweptPileOff, sizeof(pile));
    return pile;
}

void* ReadHolder(void* broom) {
    if (!broom || !g_layoutResolved) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, static_cast<uint8_t*>(broom) + g_holderOff, sizeof(p));
    return p;
}

bool WriteHolder(void* broom, void* player) {
    if (!broom || !g_layoutResolved) return false;
    std::memcpy(static_cast<uint8_t*>(broom) + g_holderOff, &player, sizeof(player));
    return true;
}

bool FireStroke(void* broom) {
    if (!broom || !g_namesResolved || !R::IsLive(broom)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(broom), g_strokeNotifyFnName.c_str());
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<R::FName>(P::name::BroomNotifyNameParam, g_strokeName) &&
           ue_wrap::Call(broom, f);
}

bool PressUse(void* broom, void* player) { return CallWithPlayer(broom, P::name::HandUseRmbFn, player); }

bool ReleaseUse(void* broom, void* player) {
    return CallWithPlayer(broom, P::name::HandReleaseRmbFn, player);
}

}  // namespace ue_wrap::broom
