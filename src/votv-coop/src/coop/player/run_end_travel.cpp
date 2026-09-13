// coop/player/run_end_travel.cpp -- which trip to the main menu is the game ending the run.
// The shape is in the header; the census and the verdicts are here, beside the code that runs
// them.

// The nine "menu" sites, read off the cooked blueprints: `mainPlayer_C`'s uber (the death chain,
// the only one that sets `dead`); `ui_menu_C` (the player's own quit); `ui_disclaimer_C` twice
// (before any session); and five in-world run-endings that end a run without ever touching the
// flag -- `gameover_C` (spawned by `theEvil_C` and `ui_badSun_C`), `npc_angryErieFlesh_C`,
// `birch_C`, `SKEL_C` and `tutorWall_spikes_C`. The five are what the field reported as "a death
// sent everyone to the menu": to a player they are a black screen and ten seconds, exactly like
// dying.

#include "coop/player/run_end_travel.h"

#include "coop/net/session.h"
#include "coop/player/death_revive.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace coop::player::run_end_travel {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The single travel author in the game: lib_C::loadLevel. A NAME watch rather than an exact one,
// because `lib_C` is a BlueprintFunctionLibrary whose class loads with the first level and whose
// UFunction address is not stable across a level reload; the name matches whatever declares it.
constexpr const wchar_t* kLoadLevelName = L"loadLevel";
constexpr int            kLoadLevelTag  = 0x52554E45;  // 'RUNE'

// The level every run-ending travel names.
constexpr const wchar_t* kMenuLevel = L"menu";

// The class that owns the travel author, and the discriminator against its namesake.
constexpr const wchar_t* kTravelAuthorClass = L"lib_C";

// Gameplay ticks between class-resolve attempts (the pump runs at 125 Hz, so ~1 Hz).
constexpr uint32_t kResolveEveryNTicks = 125;

// The one author allowed to reach the menu from inside a session: the pause menu's own quit.
// The discrimination has to happen here because the author is a PARAMETER of `loadLevel` -- all
// 26 sites pass `this` as `__WorldContext` -- and it is gone one hop later, where `transition`
// calls `OpenLevel` with the gamemode.
constexpr const wchar_t* kQuitAuthorClass = L"ui_menu_C";

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_watchInstalled{false};

// Resolved on the game thread in Tick, read in the callback. A class lookup walks the object
// array, so it is kept off the VM's body.
std::atomic<void*> g_quitAuthorClass{nullptr};

std::atomic<unsigned long long> g_seen{0};
std::atomic<unsigned long long> g_menuSeen{0};
std::atomic<unsigned long long> g_cancelled{0};

// Rate latch for the sub-level line: a trigger volume can fire its travel more than once, and this
// seam does not act on those at all, so they are logged as a trail rather than per call.
unsigned long long g_subLevelLogged = 0;

// The class resolve runs at ~1 Hz of the gameplay tick, not every tick: a FindClass MISS walks
// every UObject slot and is never cached, so an unthrottled retry is a full-array scan per frame
// for as long as the widget class is not loaded. The player_damage / wisp_attack Install shape.
uint32_t g_resolveThrottle = 0;

// One-shot warnings, so a permanent shortfall says so once instead of once per travel.
bool g_saidNoQuitClass = false;
bool g_saidNoRevive = false;
bool g_saidNoParams = false;

// Does `obj`'s class chain reach `cls`? The quit menu is one exact class today; the walk costs
// nothing on a travel and survives a recook that subclasses it.
bool IsOrDerivesFrom(void* obj, void* cls) {
    if (!obj || !cls) return false;
    void* c = R::ClassOf(obj);
    for (int hops = 0; c && hops < 16; ++hops) {
        if (c == cls) return true;
        c = R::SuperStructOf(c);
    }
    return false;
}

// The verdict, at the body's entry, on the game thread (the gate skips and counts any off-thread
// match). Every early Run is the fail-closed direction the death arc chose: a player who reaches
// the menu is recoverable, a player stranded in a world we refused to leave is not.
sg::Verdict OnLoadLevelPre(const sg::Call& call) {
    g_seen.fetch_add(1, std::memory_order_relaxed);
    if (!call.locals || !call.function) return sg::Verdict::Run;

    // TWO blueprints declare a function called `loadLevel`: `lib_C`'s, the game's only travel
    // author, and `waterVolume_basementFlooder_C`'s, a 36-byte body with no parameters at all. A
    // name watch sees both -- measured on the first run of this seam, which logged the second one
    // as a resolve failure. So the owner decides, and the offsets are cached per function rather
    // than per process, since two of them alternate through here.
    static void*   sFn = nullptr;
    static bool    sIsTravelAuthor = false;
    static int32_t sLevelOff = -1;
    static int32_t sAuthorOff = -1;
    if (call.function != sFn) {
        sFn = call.function;
        sIsTravelAuthor = R::NameEquals(R::NameOf(R::OuterOf(call.function)), kTravelAuthorClass);
        sLevelOff  = sIsTravelAuthor ? R::FindParamOffset(call.function, L"level") : -1;
        sAuthorOff = sIsTravelAuthor ? R::FindParamOffset(call.function, L"__WorldContext") : -1;
        if (sIsTravelAuthor && (sLevelOff < 0 || sAuthorOff < 0) && !g_saidNoParams) {
            // Only for the real one: a namesake resolving nothing is expected, but the travel
            // author's own parameters going missing is a game update renaming them, and every
            // menu travel would pass unjudged -- the pre-fix behaviour, silently.
            g_saidNoParams = true;
            UE_LOGE("run_end_travel: %ls::loadLevel's parameters did not resolve (level=%d "
                    "__WorldContext=%d) -- every menu travel now passes through unjudged",
                    kTravelAuthorClass, sLevelOff, sAuthorOff);
        }
    }
    if (!sIsTravelAuthor || sLevelOff < 0 || sAuthorOff < 0) return sg::Verdict::Run;

    const R::FName level = *reinterpret_cast<const R::FName*>(call.locals + sLevelOff);
    void* author = *reinterpret_cast<void* const*>(call.locals + sAuthorOff);

    if (!R::NameEquals(level, kMenuLevel)) {
        // A sub-level travel (sl_*, the map transitions). It is not this seam's to refuse -- and
        // it will still end the session for the peer that takes it, which is a separate open
        // question, so the trail is left deliberately.
        const unsigned long long n = ++g_subLevelLogged;
        if (n <= 8 || (n % 50) == 0) {
            UE_LOGI("run_end_travel: lib_C::loadLevel(\"%ls\") authored by %ls (#%llu) -- not the "
                    "menu, so not judged here; a sub-level travel still ends this peer's session",
                    R::ToString(level).c_str(), R::ClassNameOf(author).c_str(), n);
        }
        return sg::Verdict::Run;
    }
    g_menuSeen.fetch_add(1, std::memory_order_relaxed);

    const Judgement v = JudgeMenuTravel(author);
    if (v != Judgement::Cancel) {
        // Only the two that say something about this seam are worth a line per travel: a solo
        // travel is the common case and says nothing, and the two shortfalls latch below.
        if (v == Judgement::RunPlayerAsked) {
            UE_LOGI("run_end_travel: allowed lib_C::loadLevel(\"menu\") authored by %ls -- the "
                    "player asked to leave", R::ClassNameOf(author).c_str());
        } else if (v == Judgement::RunNoQuitClass && !g_saidNoQuitClass) {
            g_saidNoQuitClass = true;
            UE_LOGW("run_end_travel: %ls has not resolved -- menu travels pass through unjudged "
                    "rather than risk refusing the player's own quit", kQuitAuthorClass);
        } else if (v == Judgement::RunNoRevive && !g_saidNoRevive) {
            g_saidNoRevive = true;
            UE_LOGW("run_end_travel: a run-ending travel authored by %ls is passing through -- the "
                    "revive is not available, and refusing a travel we cannot answer strands the "
                    "player. net_pump's flee handles a death; a catch ending ends the run as it "
                    "did before this seam.", R::ClassNameOf(author).c_str());
        }
        return sg::Verdict::Run;
    }

    const std::wstring cls = R::ClassNameOf(author);
    const bool isLocalPawn = author && author == coop::players::Registry::Get().Local();
    g_cancelled.fetch_add(1, std::memory_order_relaxed);
    UE_LOGW("run_end_travel: REFUSED lib_C::loadLevel(\"menu\") authored by %ls%s -- the game is "
            "ending the run and this is a coop session, so the world is kept and the player is "
            "revived in place", cls.c_str(),
            isLocalPawn ? " = THE LOCAL PLAYER (the death chain)"
                        : " (a run-ending that never touches `dead`)");
    coop::death_revive::NoteRunEndCancelled(author, cls.c_str(), isLocalPawn);
    return sg::Verdict::Cancel;
}

}  // namespace

// An unknown author is REFUSED rather than allowed, and the direction is deliberate: being wrong
// here means the game did not end a run it wanted to end, and the player keeps playing with the
// pause menu still available; being wrong the other way is the session ending for everyone.
Judgement JudgeMenuTravel(void* author) {
    // Single player is untouched by this test, not by the watch's absence: the watch is registered
    // whenever the lane installs, and it refuses nothing without a live session.
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return Judgement::RunNoSession;
    // Without the quit author's class every menu travel would read as a run-ending, including the
    // player's own quit, and refusing THAT traps them in the world.
    void* quitCls = g_quitAuthorClass.load(std::memory_order_acquire);
    if (!quitCls) return Judgement::RunNoQuitClass;
    if (IsOrDerivesFrom(author, quitCls)) return Judgement::RunPlayerAsked;
    // Refusing a travel we cannot answer is the one outcome worse than the menu.
    if (!coop::death_revive::ReviveAvailable()) return Judgement::RunNoRevive;
    return Judgement::Cancel;
}

// Our own leave never reaches this seam: `net_pump`'s flee calls `mainGamemode_C::transition`
// directly (`ue_wrap::engine::ReturnToMainMenu`), one hop past `loadLevel`. A future lane that
// wants to leave through `loadLevel` needs an allow path here, not a special case at its call
// site.
void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watchInstalled.load(std::memory_order_acquire)) return;
    // A name watch registers immediately and the gate resolves the name itself on the game
    // thread, so there is nothing to wait for and no retry throttle to own.
    if (sg::WatchName(kLoadLevelName, kLoadLevelTag, &OnLoadLevelPre, nullptr)) {
        g_watchInstalled.store(true, std::memory_order_release);
        UE_LOGI("run_end_travel: watching lib_C::%ls at the script-body gate -- the game's only "
                "travel author, and the only place a travel still carries its own author",
                kLoadLevelName);
    }
}

void Tick() {
    // The gate's enable is a per-script-call cost on the VM's hottest loop, so it is scoped to a
    // live session -- which is also what the verdict is scoped to, so an enabled gate with nothing
    // to judge is pure waste. Measured: the first build enabled it unconditionally and the
    // sessionless death control caught it, reporting a solo player paying for a seam that can
    // never fire.
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) {
        coop::death_revive::NoteRunEndSeamReady(false);
        return;
    }
    sg::ResolvePendingNames();
    // This lane owns its own enable: the gate's switch is shared, and riding another consumer's
    // would leave this watch green and its callback silent the moment that consumer retired.
    sg::SetEnabled(true);
    if (!g_quitAuthorClass.load(std::memory_order_relaxed) &&
        (g_resolveThrottle++ % kResolveEveryNTicks) == 0) {
        if (void* c = R::FindClass(kQuitAuthorClass)) {
            g_quitAuthorClass.store(c, std::memory_order_release);
            UE_LOGI("run_end_travel: %ls resolved (%p) -- the player's own quit-to-menu is now "
                    "told apart from the game ending the run", kQuitAuthorClass, c);
        }
    }
    // The revive's arm asks whether this seam can answer a death at all: published from here so
    // `death_revive` never has to name this module back.
    coop::death_revive::NoteRunEndSeamReady(g_watchInstalled.load(std::memory_order_acquire) &&
                                            g_quitAuthorClass.load(std::memory_order_relaxed) !=
                                                nullptr);
}

void OnSessionStart() {
    g_seen.store(0, std::memory_order_relaxed);
    g_menuSeen.store(0, std::memory_order_relaxed);
    g_cancelled.store(0, std::memory_order_relaxed);
    g_subLevelLogged = 0;
    g_saidNoQuitClass = false;
    g_saidNoRevive = false;
    g_saidNoParams = false;
}

bool WatchInstalled() { return g_watchInstalled.load(std::memory_order_acquire); }
unsigned long long TravelsSeen() { return g_seen.load(std::memory_order_relaxed); }
unsigned long long MenuTravelsSeen() { return g_menuSeen.load(std::memory_order_relaxed); }
unsigned long long TravelsCancelled() { return g_cancelled.load(std::memory_order_relaxed); }

}  // namespace coop::player::run_end_travel
