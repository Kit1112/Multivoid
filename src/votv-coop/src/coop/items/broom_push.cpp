// coop/items/broom_push.cpp -- see coop/items/broom_push.h.

#include "coop/items/broom_push.h"

#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/trash_sweep.h"
#include "ue_wrap/actors/broom.h"          // IsBroom
#include "ue_wrap/actors/prop.h"           // IsGarbageClump / IsDescendantOfProp
#include "ue_wrap/core/game_thread.h"      // IsGameThread
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace coop::broom_push {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_seamInstalled = false;
bool g_seamRefused   = false;

// The actors pushed since the last tick, each once, by pointer and the index it had when pushed.
// The seam fires inside the stroke, on the game thread; the streams open on the tick, outside it.
struct Pushed { void* actor; int32_t idx; };
std::vector<Pushed> g_pushed;
int g_seen = 0;

// The finish of a deferred spawn, armed while a dispenser pile's `broomed` body runs, the pile whose
// body it is, and the props that pile's own bytecode finished spawning there.
bool  g_dispenseInstalled = false;
bool  g_dispenseRefused   = false;
bool  g_dispenseArmed     = false;
void* g_finishFn          = nullptr;
void* g_dispensePile      = nullptr;
std::vector<Pushed> g_dispensed;
int g_dispenseSeen = 0;

// Every Blueprint call of the velocity setter arrives here; only the broom's is a push. A
// component's outer is the actor that owns it.
void OnSetLinearVelocityPost(void* component, void* sourceObject, void* /*result*/) {
    if (!component || !ue_wrap::broom::IsBroom(sourceObject)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    void* owner = R::OuterOf(component);
    if (!owner) return;
    for (const Pushed& p : g_pushed)
        if (p.actor == owner) return;
    g_pushed.push_back(Pushed{owner, R::InternalIndexOf(owner)});
    ++g_seen;
}

// A spawn the pile's own bytecode finished inside its `broomed` body is a prop the stroke knocked out
// of the pile. The source is the object whose bytecode made the call, so a spawn nested in a new
// prop's own BeginPlay, or one of ours through ProcessEvent, is never taken for it.
void OnDispensedPost(void* /*context*/, void* source, void* spawned) {
    if (!spawned || !source || source != g_dispensePile || !ue_wrap::game_thread::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!ue_wrap::prop::IsDescendantOfProp(spawned)) return;
    g_dispensed.push_back(Pushed{spawned, R::InternalIndexOf(spawned)});
    ++g_dispenseSeen;
}

void SetDispenseArmed(void* pile) {
    g_dispensePile = pile;
    const bool armed = pile != nullptr;
    if (!g_dispenseInstalled || g_dispenseArmed == armed) return;
    ue_wrap::ufunction_hook::SetArmed(g_finishFn, &OnDispensedPost, armed);
    g_dispenseArmed = armed;
}

void InstallDispenseSeam() {
    if (g_dispenseInstalled || g_dispenseRefused) return;
    void* cls = R::FindClass(P::name::GameplayStaticsClass);
    if (!cls) return;   // an engine class: present from boot, asked again next pass until then
    g_finishFn = R::FindFunction(cls, P::name::FinishSpawningActorFn);
    if (!g_finishFn || !ue_wrap::ufunction_hook::InstallPostHook(g_finishFn, &OnDispensedPost, /*armed=*/false)) {
        g_dispenseRefused = true;
        UE_LOGE("[BROOM-PUSH] the dispense seam did NOT install (%s) -- trash a host's broom knocks out of "
                "a dispenser pile falls unstreamed for the rest of this process",
                g_finishFn ? "native hook table full" : "the function is not on the class in this build");
        return;
    }
    g_dispenseInstalled = true;
    UE_LOGI("[BROOM-PUSH] installed on %ls, armed only inside a dispenser pile's `broomed` and for that pile's "
            "own spawns -- the trash a host's broom knocks out streams its fall", P::name::FinishSpawningActorFn);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    // A dispense is caught for a host alone, so a peer that never hosts never patches the finish of
    // every deferred spawn in the game.
    if (session && session->connected() && session->role() == coop::net::Role::Host) InstallDispenseSeam();
    if (g_seamInstalled || g_seamRefused) return;
    void* cls = R::FindClass(P::name::PrimitiveComponentClass);
    if (!cls) return;   // an engine class: present from boot, asked again next pass until then
    void* fn = R::FindFunction(cls, P::name::SetPhysicsLinearVelocityFn);
    const bool installed = fn && ue_wrap::ufunction_hook::InstallPostHook(fn, &OnSetLinearVelocityPost);
    if (!installed) {
        g_seamRefused = true;
        UE_LOGE("[BROOM-PUSH] the velocity-setter seam did NOT install (%s) -- what a broom pushes on "
                "the host slides there alone this session",
                fn ? "native hook table full" : "the function is not on the class in this build");
        return;
    }
    g_seamInstalled = true;
    UE_LOGI("[BROOM-PUSH] installed on %ls -- a host's broom push streams what it moves",
            P::name::SetPhysicsLinearVelocityFn);
}

void OnDispenseBegin(void* pile) { SetDispenseArmed(pile); }
void OnDispenseEnd()             { SetDispenseArmed(nullptr); }

void Tick() {
    // A window closes as its body returns. A body that faulted fires no end, and its window closes
    // here instead, having caught nothing but that pile's own spawns in between.
    SetDispenseArmed(nullptr);
    if (g_pushed.empty() && g_dispensed.empty()) return;
    for (const Pushed& p : g_pushed) {
        if (!R::IsLiveByIndex(p.actor, p.idx)) continue;
        // A clump is trash, not a prop: its roll rides the clump stream. A prop coasts on the
        // driven-prop channel until it rests; one a verb holds stays that verb's.
        if (ue_wrap::prop::IsGarbageClump(p.actor))
            coop::trash_sweep::NotePushed(p.actor);
        else
            coop::prop_drive_host::Coast(p.actor, "broom push");
    }
    g_pushed.clear();
    // What the same stroke both dispensed and pushed is coasting already; this starts its rest clock
    // again, and opens the fall of the rest.
    for (const Pushed& p : g_dispensed) {
        if (R::IsLiveByIndex(p.actor, p.idx)) coop::prop_drive_host::Coast(p.actor, "broom dispense");
    }
    g_dispensed.clear();
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    SetDispenseArmed(nullptr);
    if (g_seen > 0 || g_dispenseSeen > 0)
        UE_LOGI("[BROOM-PUSH] session tally -- %d push(es), %d dispensed prop(s) seen", g_seen, g_dispenseSeen);
    g_pushed.clear();
    g_dispensed.clear();
    g_seen = 0;
    g_dispenseSeen = 0;
}

}  // namespace coop::broom_push
