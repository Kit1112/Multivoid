// harness/autotest/autotest_broomstroke.cpp -- see harness/autotest/broomstroke.h for what the two
// phases claim and why the host stands 30 m off during the first.
//
// What the field reported is a birth that never crosses: a stroke empties a dispenser pile and
// spawns real trash for it, and on a client only the emptying is seen by anyone else. So what this
// drill counts is the TRASH, on both peers, around the pile -- not the verb's return, and not a log
// line saying the verb ran.
//
// The verb is driven directly rather than through a held broom: the broom reaches `broomed` with an
// EX_LocalVirtualFunction from its own ubergraph, the same body entry a reflected call reaches, and
// a drill that first has to put a broom in a hand measures the hotbar instead.
#include "harness/autotest/broomstroke.h"

#include "harness/autotest.h"   // IsClientRole

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"      // the puppet whose distance the host's reach check reads
#include "coop/net/session.h"               // Session::connected -- the client half of the link
#include "harness/session_runtime.h"        // the link moment both peers time their phases from
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace P  = ue_wrap::profile;
namespace UP = ue_wrap::prop;

// The pile both peers must agree on is chosen by DISTANCE THEN KEY, and the key is what settles it.
// Distance alone picked two different piles on the two peers in one measured run -- a save holds
// piles stacked on two floors at the same X,Y whose distances to a fixed anchor differed by one
// centimetre, so the choice was a coin flip. Key alone picked a pile 60 km out and 171 m up, and
// the teleport there killed both players, which reloaded the host's world mid-run. So: take the
// piles nearest the base, then order THOSE by key. The keys are save-persisted and cross-peer
// identical (the counter mirror hashes its key set to prove exactly that), so rank N names the same
// actor on both peers with nothing to agree on, and it is a pile a player can stand at.
constexpr size_t kMaxPilesScanned = 4096;
constexpr size_t kNearestConsidered = 20;
// Trash counts as "out of the pile" within three metres of it: the stroke rolls its transform
// inside the pile's own bounds, and nothing else is spawning there during the run.
constexpr float kTrashRadiusCm = 300.f;
// How far the peer that is NOT striking has to be for phase 1 to mean anything: past the eight
// metres a depletion is judged by, so the host really is asked to believe in a death it cannot see.
// It is CHECKED, never arranged -- a blind 30 m offset dropped the host onto a sub-level travel
// trigger, which ended the session and made every phase read as a lane failure. The watcher stays
// where the game put it, which is hundreds of metres away, and the drill measures that instead.
constexpr float kFarEnoughCm = 900.f;

ue_wrap::FRotator LookAt(const ue_wrap::FVector& from, const ue_wrap::FVector& to) {
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
    const float kRad2Deg = 180.f / 3.14159265358979323846f;
    ue_wrap::FRotator r{};
    r.Yaw   = std::atan2(dy, dx) * kRad2Deg;
    r.Pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * kRad2Deg;
    r.Roll  = 0.f;
    return r;
}

// Run a game-thread closure and block until it stores into `done` (1 ok, 2 fail); engine state is
// game-thread only.
template <class Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

// Live Aprop_C descendants within `radiusCm` of `at`. This is the measurement the whole drill turns
// on, so it counts world actors rather than asking any of our own registries: a mirror the client
// spawned for the host's trash and a prop the host spawned itself are both simply there.
int CountTrashNear(const ue_wrap::FVector& at, float radiusCm) {
    const float r2 = radiusCm * radiusCm;
    int n = 0;
    const int32_t total = R::NumObjects();
    for (int32_t i = 0; i < total; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!R::IsLive(obj)) continue;              // slot flags first: ClassOf dereferences
        if (!UP::IsDescendantOfProp(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const ue_wrap::FVector loc = E::GetActorLocation(obj);
        const float dx = loc.X - at.X, dy = loc.Y - at.Y, dz = loc.Z - at.Z;
        if (dx * dx + dy * dy + dz * dz <= r2) ++n;
    }
    return n;
}

struct Subject {
    void*            pile    = nullptr;
    int32_t          pileIdx = 0;
    bool             alive   = false;   // READ at the pick, never assumed: it is half of a verdict
    std::wstring     key;
    ue_wrap::FVector pos{};
    int32_t          a = 0, b = 0;
    int              trash = 0;
    void*            player  = nullptr;
};

// The dispenser pile at `rank` in the sorted key order, plus the local player. False when the
// world holds fewer piles than that, which is a fact about the save, not a failure of the lane.
bool PickSubject(const std::shared_ptr<Subject>& sb, const char* who, size_t rank,
                 const char* label) {
    return RunGT([sb, who, rank, label](std::atomic<int>& d) {
        void* p = coop::players::Registry::Get().Local();
        if (!p || !R::IsLive(p)) { UE_LOGW("broom_drill: %s has no local player", who); d.store(2); return; }
        sb->player = p;
        struct Found { std::wstring key; void* obj; float d2; };
        std::vector<Found> piles;
        const ue_wrap::FVector anchor{ P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ };
        const int32_t total = R::NumObjects();
        for (int32_t i = 0; i < total && piles.size() < kMaxPilesScanned; ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj || !R::IsLive(obj)) continue;
            if (!UP::IsTrashBitsPile(obj)) continue;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
            std::wstring k = UP::GetInteractableKeyString(obj);
            if (k.empty() || k == L"None") continue;
            const ue_wrap::FVector loc = E::GetActorLocation(obj);
            const float dx = loc.X - anchor.X, dy = loc.Y - anchor.Y, dz = loc.Z - anchor.Z;
            piles.push_back(Found{ std::move(k), obj, dx * dx + dy * dy + dz * dz });
        }
        const size_t seen = piles.size();
        // Nearest the base first, the key breaking every tie, so the cut is the same set on both
        // peers even where two piles sit one centimetre apart.
        std::sort(piles.begin(), piles.end(), [](const Found& x, const Found& y) {
            if (x.d2 != y.d2) return x.d2 < y.d2;
            return x.key < y.key;
        });
        if (piles.size() > kNearestConsidered) piles.resize(kNearestConsidered);
        std::sort(piles.begin(), piles.end(),
                  [](const Found& x, const Found& y) { return x.key < y.key; });
        UE_LOGI("broom_drill: %s sees %zu keyed dispenser pile(s), ranking the %zu nearest the base",
                who, seen, piles.size());
        if (piles.size() <= rank) {
            UE_LOGW("broom_drill: %s has no pile at key rank %zu -- this save cannot exercise the "
                    "%s phase", who, rank, label);
            d.store(2); return;
        }
        sb->key     = piles[rank].key;
        sb->pile    = piles[rank].obj;
        sb->pileIdx = R::InternalIndexOf(sb->pile);
        sb->alive   = R::IsLiveByIndex(sb->pile, sb->pileIdx);
        sb->pos     = E::GetActorLocation(sb->pile);
        UP::ReadTrashPileAmounts(sb->pile, sb->a, sb->b);
        sb->trash   = CountTrashNear(sb->pos, kTrashRadiusCm);
        UE_LOGI("broom_drill: %s %s SUBJECT rank=%zu key='%ls' pile=%p at(%.0f,%.0f,%.0f) -- "
                "A=%d B=%d trashNear=%d",
                who, label, rank, sb->key.c_str(), sb->pile, sb->pos.X, sb->pos.Y, sb->pos.Z,
                sb->a, sb->b, sb->trash);
        d.store(1);
    }) == 1;
}

// Distance from `a` to `b`.
float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Stand `standCm` from `at`, facing it when `face`. It REPORTS where the body actually ended up,
// and on the host the puppet's distance too: the reach check the host applies to an intent measures
// the PUPPET, and a teleport the puppet has not followed denies a stroke the striker believes is in
// range. A drill that only asks for a teleport cannot tell that apart from a broken intent path.
void StandAt(const ue_wrap::FVector& at, float standCm, bool face, const char* who,
             const char* phase) {
    RunGT([at, standCm, face, who, phase](std::atomic<int>& d) {
        // A FRESH read, never a pointer cached at the pick: a player who died in between has a new
        // pawn, and the old one answers (0,0,0) to every question instead of failing.
        void* player = coop::players::Registry::Get().Local();
        if (!player || !R::IsLive(player)) {
            UE_LOGW("broom_drill: %s %s STAND -- no live local pawn", who, phase);
            d.store(1); return;
        }
        const ue_wrap::FVector from = E::GetActorLocation(player);
        float ax = from.X - at.X, ay = from.Y - at.Y;
        const float h = std::sqrt(ax * ax + ay * ay);
        if (h < 1.f) { ax = 1.f; ay = 0.f; } else { ax /= h; ay /= h; }
        const ue_wrap::FVector stand{ at.X + ax * standCm, at.Y + ay * standCm, at.Z + 90.f };
        const bool ok = E::TeleportTo(player, stand, face ? LookAt(stand, at) : ue_wrap::FRotator{});
        const ue_wrap::FVector now = E::GetActorLocation(player);
        UE_LOGI("broom_drill: %s %s STAND asked=%.0fcm teleport=%d landed at(%.0f,%.0f,%.0f) "
                "= %.0fcm from the pile", who, phase, standCm, ok ? 1 : 0, now.X, now.Y, now.Z,
                Dist(now, at));
        d.store(1);
    });
}

// Where this peer's own body is, relative to the pile. The watcher never moves, so this is the
// phase's premise rather than its setup: phase 1 only means something while the watcher is outside
// the radius a depletion is judged by.
bool ReportStandoff(const ue_wrap::FVector& at, const char* who, const char* phase) {
    auto outside = std::make_shared<std::atomic<int>>(0);
    RunGT([at, who, phase, outside](std::atomic<int>& d) {
        void* p = coop::players::Registry::Get().Local();
        if (!p || !R::IsLive(p)) {
            UE_LOGW("broom_drill: %s %s WATCH -- no live local pawn", who, phase);
            d.store(1); return;
        }
        const float dist = Dist(E::GetActorLocation(p), at);
        outside->store(dist > kFarEnoughCm ? 1 : 0);
        UE_LOGI("broom_drill: %s %s WATCH standing %.0fcm from the pile -- %s", who, phase, dist,
                dist > kFarEnoughCm ? "outside the depletion radius, so the phase means something"
                                    : "INSIDE the depletion radius: this peer could witness the "
                                      "death itself, so the phase proves less than it claims");
        d.store(1);
    });
    return outside->load() == 1;
}

// The host's view of a client's body, which is what its reach check measures. Logged beside the
// client's own reading so the two can be compared directly.
void LogPuppetDistance(const ue_wrap::FVector& at, const char* phase) {
    RunGT([at, phase](std::atomic<int>& d) {
        for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
            void* body = (rp && rp->valid()) ? rp->GetActor() : nullptr;
            if (!body) continue;
            const ue_wrap::FVector loc = E::GetActorLocation(body);
            UE_LOGI("broom_drill: HOST %s PUPPET slot=%u at(%.0f,%.0f,%.0f) = %.0fcm from the pile",
                    phase, slot, loc.X, loc.Y, loc.Z, Dist(loc, at));
        }
        d.store(1);
    });
}

// One sample of everything the verdict reads.
struct Sample { bool alive = false; int32_t a = 0, b = 0; int trash = 0; };

Sample Read(const std::shared_ptr<Subject>& sb) {
    auto out = std::make_shared<Sample>();
    RunGT([sb, out](std::atomic<int>& d) {
        // IsLiveByIndex, never IsLive: a destroyed actor's slot can be recycled, and a raw liveness
        // read of freed memory misreads.
        out->alive = R::IsLiveByIndex(sb->pile, sb->pileIdx);
        if (out->alive) UP::ReadTrashPileAmounts(sb->pile, out->a, out->b);
        out->trash = CountTrashNear(sb->pos, kTrashRadiusCm);
        d.store(1);
    });
    return *out;
}

// Call the game's own `broomed` on the pile. The script-body gate sits at the body's entry on every
// route, so this reaches it exactly as the broom's own ubergraph does -- which is the point on a
// client, where the refusal and the intent are what this drill is measuring.
bool Strike(const std::shared_ptr<Subject>& sb) {
    auto ok = std::make_shared<std::atomic<int>>(0);
    RunGT([sb, ok](std::atomic<int>& d) {
        if (!R::IsLiveByIndex(sb->pile, sb->pileIdx)) { d.store(2); return; }
        void* declarer = nullptr;
        void* fn = R::FindDispatchFunction(R::ClassOf(sb->pile), P::name::PileBroomedFn, &declarer);
        if (!fn) { UE_LOGW("broom_drill: '%ls' did not resolve on the pile", P::name::PileBroomedFn);
                   d.store(2); return; }
        const int32_t frameSize = R::FunctionFrameSize(fn);
        std::vector<uint8_t> frame(frameSize > 0 ? static_cast<size_t>(frameSize) : 0, 0u);
        // The verb's one parameter is dead -- the thunk writes `location` to the persistent frame
        // and nothing reads it back -- so a zeroed frame is the faithful call.
        ok->store(R::CallFunction(sb->pile, fn, frame.empty() ? nullptr : frame.data()) ? 1 : 0);
        d.store(1);
    });
    return ok->load() == 1;
}

void LogPhase(const char* who, const char* phase, const Sample& s) {
    UE_LOGI("broom_drill: %s %s -- pileAlive=%d A=%d B=%d trashNear=%d",
            who, phase, s.alive ? 1 : 0, s.a, s.b, s.trash);
}

}  // namespace

void RunBroomStrokeProbe() {
    const bool isClient = IsClientRole();
    const char* who = isClient ? "CLIENT" : "HOST";
    // Both peers time their phases from the LINK, not from their own start: the two processes are
    // launched seconds apart, so a fixed wait on each side put the client's phase 1 after the host's
    // phase 2 and the run measured neither. The link is the same wall-clock instant on both peers,
    // within the join's own round trip, and it is the first moment either has anything to measure.
    UE_LOGI("broom_drill: %s -- waiting for the link, then a settle, then the two phases", who);
    // The same event on both sides: THE OTHER PEER'S BODY EXISTS HERE. A connected socket is not it
    // -- measured, the client's socket came up 22 s before the host had a puppet for it, and phases
    // timed from those two moments overlapped. Both peers spawn each other's puppet off the same
    // roster, so this fires within a round trip of itself on both.
    bool linked = false;
    for (int i = 0; i < 180 && !linked; ++i) {
        if (isClient && !harness::session_runtime::Session().connected()) { ::Sleep(1000); continue; }
        auto* rp = coop::players::Registry::Get().Puppet(isClient ? 0 : 1);
        linked = rp && rp->valid() && rp->GetActor() != nullptr;
        if (!linked) ::Sleep(1000);
    }
    if (!linked) { UE_LOGW("broom_drill: %s never saw the link -- aborting", who); return; }
    UE_LOGI("broom_drill: %s LINKED -- settling 25s for the pile index and the save binds", who);
    ::Sleep(25000);
    // Every wait after this is against ONE clock started here, not a chain of relative sleeps: a
    // peer whose strike loop ends early must still reach phase 2 at the same moment as the other,
    // and relative sleeps drift apart exactly when a phase does something interesting.
    const DWORD t0 = ::GetTickCount();
    auto waitUntil = [t0](DWORD ms) {
        for (;;) {
            const DWORD elapsed = ::GetTickCount() - t0;
            if (elapsed >= ms) return;
            ::Sleep(ms - elapsed > 500 ? 500 : ms - elapsed);
        }
    };

    // BOTH subjects are chosen here, before anything is struck: phase 2's pile must be picked while
    // the key set still holds phase 1's, or the two peers rank a different set and name different
    // piles again.
    // Where the game put this body, so the run can hand it back.
    auto spawn = std::make_shared<ue_wrap::FVector>();
    RunGT([spawn](std::atomic<int>& d) {
        void* p = coop::players::Registry::Get().Local();
        if (p && R::IsLive(p)) *spawn = E::GetActorLocation(p);
        d.store(1);
    });
    auto sb  = std::make_shared<Subject>();
    auto sb2 = std::make_shared<Subject>();
    if (!PickSubject(sb, who, 0, "P1")) { UE_LOGW("broom_drill: %s could not pick a subject -- aborting", who); return; }
    const bool haveP2 = PickSubject(sb2, who, 1, "P2");

    // ---- phase 1: the CLIENT strikes; the host watches from wherever the game put it ----
    bool watcherOutside = true;   // the striker is not a witness to its own phase's premise
    if (isClient) StandAt(sb->pos, 120.f, true, who, "P1");
    else          watcherOutside = ReportStandoff(sb->pos, who, "P1");
    // The puppet has to follow the client's teleport before the host's reach check can accept a
    // stroke, and that is a stream, not an instant.
    ::Sleep(5000);
    if (!isClient) LogPuppetDistance(sb->pos, "P1");
    // The baseline is the one taken AT THE PICK, before either peer could act -- not a fresh read
    // here. Measured: the client's first strokes landed inside the settle above, so a sample taken
    // at this line already held the trash it was supposed to precede, and the host scored its own
    // gain as zero while holding the client's four.
    const Sample before1{ sb->alive, sb->a, sb->b, sb->trash };
    LogPhase(who, "P1 BEFORE (at the pick)", before1);

    if (isClient) {
        // Strike until the pile is gone. One stroke pops up to three props, so a pile of six needs
        // two or three strokes; the cap is a runaway backstop, not an expectation.
        for (int i = 0; i < 12 && (::GetTickCount() - t0) < 28000; ++i) {
            if (!Strike(sb)) break;
            UE_LOGI("broom_drill: CLIENT stroke #%d sent", i + 1);
            ::Sleep(1200);
            if (!Read(sb).alive) break;
        }
    }
    waitUntil(35000);   // phase 1 closes here on both peers, whatever the strike loop did
    const Sample after1 = Read(sb);
    LogPhase(who, "P1 AFTER", after1);

    const int  gained1 = after1.trash - before1.trash;
    const bool emptied = before1.alive && !after1.alive;
    UE_LOGI("broom_drill: VERDICT role=%s phase=1 key='%ls' trash %d->%d (gained %d) pileAlive "
            "%d->%d -- %s",
            who, sb->key.c_str(), before1.trash, after1.trash, gained1,
            before1.alive ? 1 : 0, after1.alive ? 1 : 0,
            !watcherOutside
                ? "INCONCLUSIVE: this peer was inside the depletion radius, so the death proves "
                  "nothing about whether a far peer believes in it"
                : ((gained1 > 0 && emptied)
                   ? "PASS: a client's stroke put trash here and took the pile with it"
                   : (gained1 > 0 ? "PARTIAL: trash arrived but the pile is still standing"
                                  : "FAIL: nothing came out of the pile on this peer")));

    // ---- phase 2: the HOST strikes the second pile; the client watches from 30 m ----
    if (!haveP2) {
        UE_LOGW("broom_drill: %s skipping phase 2 (the save holds one pile) -- phase 1 stands alone", who);
        UE_LOGI("broom_drill: done");
        return;
    }

    if (!isClient) StandAt(sb2->pos, 120.f, true, who, "P2");
    else           ReportStandoff(sb2->pos, who, "P2");
    waitUntil(43000);
    if (!isClient) LogPuppetDistance(sb2->pos, "P2");
    const Sample before2 = Read(sb2);
    LogPhase(who, "P2 BEFORE", before2);
    if (!isClient) {
        if (Strike(sb2)) UE_LOGI("broom_drill: HOST stroke sent");
    }
    waitUntil(52000);
    const Sample after2 = Read(sb2);
    LogPhase(who, "P2 AFTER", after2);
    const int gained2 = after2.trash - before2.trash;
    UE_LOGI("broom_drill: VERDICT role=%s phase=2 key='%ls' trash %d->%d (gained %d) -- %s",
            who, sb2->key.c_str(), before2.trash, after2.trash, gained2,
            gained2 > 0 ? "PASS: a host's stroke put trash on this peer"
                        : "FAIL: the host's own stroke never reached this peer");
    // Put the body back where the run found it. A drill that teleports a player and walks away
    // leaves the next scenario measuring a world this one moved.
    RunGT([spawn](std::atomic<int>& d) {
        void* p = coop::players::Registry::Get().Local();
        if (p && R::IsLive(p)) {
            E::TeleportTo(p, *spawn, ue_wrap::FRotator{});
            UE_LOGI("broom_drill: body returned to (%.0f,%.0f,%.0f)", spawn->X, spawn->Y, spawn->Z);
        }
        d.store(1);
    });
    UE_LOGI("broom_drill: done");
}

DWORD WINAPI BroomStrokeProbeThread(LPVOID) {
    RunBroomStrokeProbe();
    return 0;
}

}  // namespace harness::autotest
