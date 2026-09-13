// harness/autotest/autotest_slip.cpp -- does a ZERO-DAMAGE ragdoll re-enter the death chain when
// the pawn's `dead` is already set? Field reports say a player who was walking around lands on
// the main menu and takes every peer with them; one slipped on a banana peel, another stood near
// an explosion. The bytecode says why, and the claim is one branch:
//
//   prop_bananaHusk_C::steppedOn -> player.punch(..., damage = 0, ...)   twice
//   mainPlayer::punch            -> if (!isRagdoll) ragdollMode(true, false, false)
//   mainPlayer::ragdollMode      -> ... -> fallen(death = false)
//   mainPlayer::fallen           -> @39848: if !(death) goto .L39685
//                                   @39685: if !(this.dead) goto .L39704  <- the only guard
//                                   @39699: .L37478 = deathEnd; dead := true; +5 s blackScreen;
//                                           +5 s lib_C::loadLevel("menu", ..., this)
//
// So the damage is irrelevant: it is a RAGDOLL bug, and a fall, a wisp, a punch and an explosion
// all enter that same fallen(false).

#include "harness/autotest.h"

#include "harness/autotest/death_state_probe.h"
#include "harness/autotest/slip_drill.h"
#include "harness/session_runtime.h"

#include "coop/net/session.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "coop/player/run_end_travel.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <memory>

namespace harness::autotest {
namespace {

namespace E   = ue_wrap::engine;
namespace GT  = ue_wrap::game_thread;
namespace R   = ue_wrap::reflection;
namespace RET = coop::player::run_end_travel;

constexpr const wchar_t* kPeelClass = L"prop_bananaHusk_C";

struct Tally {
    int pass = 0;
    int fail = 0;
};
Tally g_v;

void Check(bool ok, const char* name, const char* why) {
    (ok ? g_v.pass : g_v.fail)++;
    if (ok) UE_LOGI("[SLIP] PASS %s -- %s", name, why);
    else    UE_LOGE("[SLIP] FAIL %s -- %s", name, why);
}

// Post `body` to the game thread and wait for it (the runend drill's pattern).
template <typename Fn>
void RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(); done->store(1); });
    for (int i = 0; i < 2000 && done->load() == 0; ++i) ::Sleep(5);
}

DeathSnapshot Snap() {
    auto out = std::make_shared<DeathSnapshot>();
    RunGT([out] { *out = ReadDeathState(); });
    // sessionRunning is the caller's field by design (the probe reads UObject state and nothing
    // else), and this drill's every precondition rests on it -- the seam judges nothing without a
    // live session.
    out->sessionRunning = harness::session_runtime::Session().running();
    return *out;
}

// One line of what the game holds right now, so a run that decides nothing still says why.
void LogState(const char* when, const DeathSnapshot& s) {
    UE_LOGI("[SLIP] %s -- dead=%d ragdoll=%d(read=%d) canRagdoll=%d hp=%.1f black=%d "
            "inGameplay=%d session=%d", when, s.dead ? 1 : 0, s.isRagdoll ? 1 : 0,
            s.haveState ? 1 : 0, s.canRagdoll ? 1 : 0, s.health,
            s.blackScreenInViewport ? 1 : 0, s.inGameplay ? 1 : 0, s.sessionRunning ? 1 : 0);
}

// Set the pawn's `dead` bool. The mirror of death_revive::ClearDeadFlag, and it lives HERE and not
// there on purpose: the revive's module has no business owning a verb that kills. This is the one
// precondition the defect needs -- `dead` true on a player who is still walking. Read back rather
// than trusted: wrote is not holds.
bool SetDeadFlag(void* pawn, bool value) {
    if (!pawn || !R::IsLive(pawn)) return false;
    int32_t byteOff = -1; uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(pawn), L"dead", byteOff, mask)) return false;
    uint8_t* p = reinterpret_cast<uint8_t*>(pawn) + byteOff;
    if (value) *p |= mask; else *p &= static_cast<uint8_t>(~mask);
    return ((*p & mask) != 0) == value;
}

// The faithful trigger: a real peel, dispatched through its own steppedOn, so the peel's own guard
// (upright and nearly still) and both of its zero-damage punches run. `hit` is left as the frame
// built it -- the ubergraph stores it and never reads it.
bool DispatchSteppedOn(void* peel, void* player) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(peel), L"steppedOn");
    if (!fn) { UE_LOGW("[SLIP] %ls::steppedOn did not resolve", kPeelClass); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (!f.SetRaw(L"player", &player, sizeof(player))) {
        UE_LOGW("[SLIP] steppedOn has no `player` parameter -- the game changed under this drill");
        return false;
    }
    return ue_wrap::Call(peel, f);
}

// The peel's own second call, with the peel taken out of the picture. The claim under test is
// about a zero-damage ragdoll, not about banana physics, so a peel that landed on its side must
// not be able to make the run inconclusive. Impulses are left zero: `punch` reaches ragdollMode
// before it uses any of them, and a drill that also throws the player makes its own reading harder.
bool DispatchPunchZeroDamage(void* pawn) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pawn), L"punch");
    if (!fn) { UE_LOGW("[SLIP] mainPlayer::punch did not resolve"); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<float>(L"damage", 0.f);
    return ue_wrap::Call(pawn, f);
}

// Slip the local player, and report WHICH trigger did it -- judged by the ragdoll the player is
// in afterwards, never by the dispatch returning true. steppedOn returns true whether or not the
// peel's guard let the punches run, so a call-return fallback would skip the punch exactly when
// it is needed.
enum class Trigger { None, Peel, Punch };
Trigger SlipLocalPlayer(void* peel) {
    bool ragdolledAfterPeel = false;
    if (peel) {
        RunGT([peel] {
            void* pawn = coop::players::Registry::Get().Local();
            if (pawn) DispatchSteppedOn(peel, pawn);
        });
        ::Sleep(500);
        const DeathSnapshot s = Snap();
        ragdolledAfterPeel = s.haveState && s.isRagdoll;
    }
    if (ragdolledAfterPeel) return Trigger::Peel;
    if (peel) UE_LOGI("[SLIP] the peel's guard did not let the punches run (it is not flat or "
                      "not yet still) -- driving its own punch instead");
    bool sent = false;
    RunGT([&sent] {
        void* pawn = coop::players::Registry::Get().Local();
        if (pawn) sent = DispatchPunchZeroDamage(pawn);
    });
    return sent ? Trigger::Punch : Trigger::None;
}

const char* TriggerName(Trigger t) {
    switch (t) {
        case Trigger::Peel:  return "the peel's own steppedOn";
        case Trigger::Punch: return "the direct zero-damage punch";
        case Trigger::None:  break;
    }
    return "NOTHING -- no trigger dispatched";
}

// Watch for `ms`, stamping when each thing the death chain does first happened. Every stamp is in
// milliseconds after the call; -1 means it never happened inside the window.
struct Watch {
    long long ragdoll = -1, dead = -1, black = -1, travel = -1;
    DeathSnapshot last;
};
Watch WatchFor(int ms) {
    Watch w;
    const uint64_t t0 = ::GetTickCount64();
    for (uint64_t now = t0; now - t0 < static_cast<uint64_t>(ms); now = ::GetTickCount64()) {
        const DeathSnapshot s = Snap();
        const long long dt = static_cast<long long>(::GetTickCount64() - t0);
        if (s.haveState && s.isRagdoll && w.ragdoll < 0) w.ragdoll = dt;
        if (s.haveState && s.dead && w.dead < 0) w.dead = dt;
        if (s.blackScreenInViewport && w.black < 0) w.black = dt;
        if (s.haveWorld && !s.inGameplay && w.travel < 0) w.travel = dt;
        w.last = s;
        ::Sleep(250);
    }
    return w;
}

}  // namespace

// The drill asserts that branch rather than waiting for a log nobody will send: it slips the
// local player twice, once with `dead` false (the control, which must only ragdoll) and once with
// it true. Solo host WITH a session -- the second half of the question, whether our run-ending
// seam refuses the travel the chain asks for, does not exist without one.
void RunSlipDrill() {
    const bool isClient = IsClientRole();
    UE_LOGI("[SLIP] starting as the %s (waiting 60 s: world, possession, a live session)",
            isClient ? "CLIENT" : "HOST");
    ::Sleep(60000);

    // Preconditions. A player who cannot ragdoll cannot slip, and one already dead or already down
    // makes both arms meaningless. A client must also be LINKED, not merely running: the whole
    // point of the client arm is what happens to a session that exists.
    DeathSnapshot s;
    bool ready = false, linked = false;
    for (int i = 0; i < 60 && !ready; ++i) {
        s = Snap();
        linked = !isClient || harness::session_runtime::Session().connected();
        ready = s.havePawn && s.haveState && s.haveCanRagdoll && s.canRagdoll &&
                s.health > 0.f && !s.dead && !s.isRagdoll && s.inGameplay && s.sessionRunning &&
                linked;
        if (!ready) ::Sleep(1000);
    }
    LogState("pre-drill", s);
    if (isClient) UE_LOGI("[SLIP] client link: connected=%d", linked ? 1 : 0);
    if (!ready) {
        UE_LOGW("[SLIP] VERDICT INCONCLUSIVE -- preconditions never met (see the line above)");
        UE_LOGI("[SLIP] DONE");
        return;
    }
    Check(RET::WatchInstalled(), "A1 seam-watching",
          RET::WatchInstalled() ? "lib_C::loadLevel is watched, so arm B's travel will be judged"
                                : "the seam never installed -- arm B cannot tell a cancel from a "
                                  "chain that never fired");

    // One peel, spawned above the player's feet so it drops the last few centimetres and settles
    // flat. Both arms reuse it: the first slip kicks it away (the ubergraph's last statement), and
    // three seconds is enough for it to come to rest again.
    auto peel = std::make_shared<void*>(nullptr);
    RunGT([peel] {
        void* pawn = coop::players::Registry::Get().Local();
        void* cls = R::FindClass(kPeelClass);
        if (!pawn || !cls) return;
        ue_wrap::FVector at = E::GetActorLocation(pawn);
        at.Z += 40.f;
        *peel = E::SpawnActor(cls, at);
    });
    ::Sleep(3000);
    UE_LOGI("[SLIP] %ls %s", kPeelClass,
            *peel ? "spawned at the player's feet and settled"
                  : "is not in this cook or did not spawn -- the direct punch carries both arms");

    // ---- ARM A: the control. A slip with `dead` FALSE must ragdoll and do nothing else. ----
    UE_LOGI("[SLIP] ARM A (control): slipping a player whose `dead` is FALSE");
    const Trigger ta = SlipLocalPlayer(*peel);
    UE_LOGI("[SLIP] arm A trigger: %s", TriggerName(ta));
    const Watch a = WatchFor(9000);
    UE_LOGI("[SLIP] arm A TIMELINE (ms) -- ragdoll=%lld dead=%lld black=%lld travel=%lld",
            a.ragdoll, a.dead, a.black, a.travel);
    LogState("arm A end", a.last);

    Check(a.ragdoll >= 0, "B1 slip-ragdolled",
          a.ragdoll >= 0 ? "the zero-damage trigger put the player into a ragdoll -- punch reached "
                           "ragdollMode, so the hops above the branch are live"
                         : "the player never ragdolled: the trigger did not reach ragdollMode, and "
                           "arm B would prove nothing about the branch under it");
    Check(a.dead < 0 && a.black < 0 && a.travel < 0, "B2 control-quiet",
          (a.dead < 0 && a.black < 0 && a.travel < 0)
              ? "an ordinary slip set no `dead`, showed no black screen and travelled nowhere: the "
                "guard at @39685 holds while `dead` is false"
              : "an ordinary slip started the death chain -- the defect is NOT gated on `dead`, and "
                "this drill's whole model is wrong");

    // ---- ARM B: the defect. `dead` true on a player who is standing, then the same slip. ----
    // forceWakeup and not ragdollMode(false,...): it is unconditional, it reads no gate, and it
    // never calls fallen(), so the stand-up itself cannot arm the chain we are about to test.
    bool stoodUp = false, armed = false;
    RunGT([&stoodUp, &armed] {
        void* pawn = coop::players::Registry::Get().Local();
        if (!pawn) return;
        stoodUp = E::ForceMainPlayerWakeup(pawn);
        armed = SetDeadFlag(pawn, true);
    });
    ::Sleep(2000);
    const DeathSnapshot mid = Snap();
    LogState("arm B armed", mid);
    Check(armed && mid.dead, "C1 armed-dead-set",
          (armed && mid.dead) ? "`dead` reads TRUE on a player still on their feet -- the state the "
                                "field reports describe"
                              : "the `dead` write did not hold, so arm B tests nothing");
    Check(stoodUp && !mid.isRagdoll, "C2 standing",
          (stoodUp && !mid.isRagdoll)
              ? "the player is out of the ragdoll, so punch will call ragdollMode again"
              : "the player is STILL ragdolled -- punch returns early and the trigger cannot fire");

    UE_LOGI("[SLIP] ARM B (defect): slipping the same player with `dead` TRUE");
    const unsigned long long cancelledBefore = RET::TravelsCancelled();
    const Trigger tb = SlipLocalPlayer(*peel);
    UE_LOGI("[SLIP] arm B trigger: %s", TriggerName(tb));
    // Past the chain's own 10 s (deathEnd, +5 s black screen, +5 s travel), with room for the
    // revive that follows a cancel.
    const Watch b = WatchFor(22000);
    const unsigned long long cancelled = RET::TravelsCancelled() - cancelledBefore;
    UE_LOGI("[SLIP] arm B TIMELINE (ms) -- ragdoll=%lld dead=%lld black=%lld travel=%lld "
            "[the chain predicts black~5000, travel~10000]", b.ragdoll, b.dead, b.black, b.travel);
    LogState("arm B end", b.last);

    // THE ROW THIS DRILL EXISTS FOR. A black screen five seconds after a zero-damage slip is the
    // death chain; nothing else in this game does that.
    Check(b.black >= 0, "D1 chain-fired",
          b.black >= 0 ? "the death chain FIRED on a zero-damage ragdoll: `fallen(false)` re-enters "
                         "at @39685 when `dead` is already set, so the peel, the explosion and every other ragdoll cause are one defect"
                       : "no black screen -- the chain did NOT fire, and the @39685 reading this "
                         "drill rests on is wrong");
    Check(cancelled >= 1, "D2 travel-refused",
          cancelled >= 1 ? "our seam REFUSED the menu travel the chain asked for: inside a session "
                           "the defect is already covered, and the field reports are about a peer "
                           "where it is not"
                         : "the travel was NOT cancelled -- if D1 passed, this reaches a player");
    Check(b.travel < 0 && b.last.inGameplay, "D3 world-kept",
          (b.travel < 0 && b.last.inGameplay)
              ? "the world survived the chain: nobody went to the menu"
              : "the world CHANGED -- the run ended for every peer in the session");
    Check(b.last.haveState && !b.last.dead && b.last.health > 0.f, "D4 revived",
          (b.last.haveState && !b.last.dead && b.last.health > 0.f)
              ? "the player is alive with `dead` cleared: the revive disposed of the episode"
              : "the player is still dead or unreadable after the cancel");

    UE_LOGI("[SLIP] SEAM -- watch=%d travelsSeen=%llu menuTravels=%llu cancelled=%llu "
            "lastReviveOk=%d", RET::WatchInstalled() ? 1 : 0, RET::TravelsSeen(),
            RET::MenuTravelsSeen(), RET::TravelsCancelled(),
            coop::death_revive::LastReviveSucceeded() ? 1 : 0);
    UE_LOGI("[SLIP] VERDICT %s (%d checks passed, %d failed)",
            g_v.fail == 0 ? "PASS" : "FAIL", g_v.pass, g_v.fail);
    UE_LOGI("[SLIP] DONE");
}

DWORD WINAPI SlipDrillThread(LPVOID) {
    RunSlipDrill();
    return 0;
}

}  // namespace harness::autotest
