// coop/props/prop_drive_host.cpp -- see coop/props/prop_drive_host.h.

#include "coop/props/prop_drive_host.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/element/intent_authority.h"
#include "coop/player/hand_item.h"            // IsHandAxisActor: the hotbar hand and its mirrors
#include "coop/player/local_streams.h"        // LastHeldActor: this player's grab slot
#include "coop/player/players_registry.h"
#include "coop/props/active_drive.h"          // NowMs
#include "coop/props/prop_element_tracker.h"  // GetPropElementIdForActor
#include "coop/props/prop_lifecycle.h"        // IsWireSuppressedPropClass: a class no peer holds
#include "coop/props/prop_wire_parity.h"      // PhysFlagsOf: the flags the end edge carries
#include "coop/props/remote_prop.h"           // IsActorUnderAnyDrive: a peer's held-prop stream
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::prop_drive_host {
namespace {

namespace E  = ue_wrap::engine;
namespace P  = ue_wrap::profile;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;

// A pose goes out when it moved past these since the last one that went out, so a creeping body
// still steps and a resting one costs nothing. One threshold on every axis, where MTA's
// unoccupied syncer (reference/mtasa-blue/Client/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp)
// holds x/y to FLOAT_EPSILON in WriteVehicleInformation and z to 0.1 -- 1.2 for a boat riding
// waves -- because z is where a parked vehicle is noisy; a prop on a floor has no such axis.
constexpr float kSendEpsCm  = 0.5f;
constexpr float kSendEpsDeg = 0.5f;
// A released prop that has not moved past the epsilons for this long is eligible to settle. Its
// velocity still decides the end edge: a body can creep below the pose epsilon while PhysX keeps
// simulating it, and handing that body back early lets peers settle it independently.
constexpr uint64_t kRestMs = 500;
// A prop that has rested that long is read at 4 Hz, the hook lane's own resting cadence, instead
// of two dispatches per tick: an anchored tie holds a resting prop for as long as the hook stands.
constexpr uint64_t kRestProbeMs = 250;
// A body below the pose epsilon but still moving is checked at this cadence until its actual
// velocity falls below kRestVelCmS. This avoids two reflected velocity reads every game tick.
constexpr uint64_t kSettleProbeMs = 50;
// A velocity below this at the end goes out as zero: assigning a velocity wakes a body at rest.
constexpr float kRestVelCmS = 1.0f;
constexpr float kRestAngVelDegS = 1.0f;
constexpr float kNudgeReachUU = 180.f;
constexpr float kNudgeMinSpeedCmS = 70.f;
constexpr float kNudgeMaxSpeedCmS = 500.f;
constexpr uint64_t kNudgeIntervalMs = 100;
constexpr uint8_t kNudgeBurstPerInterval = 8;
// PhysX can resolve one final contact just after the rest probe reads a quiet body. Keep the
// final pose under a short host-only watch so late movement reopens the coast stream.
constexpr uint64_t kSettledWatchMs = 1500;

struct Driven {
    ue_wrap::CachedObjRef ref;
    uint32_t              eid = 0;
    coop::net::WireKey    key{};
    uint8_t               gen = 0;
    bool                  claimed  = true;   // false: coasting after the verb let go
    bool                  everSent = false;
    bool                  dead     = false;  // closed this tick; erased after the pass
    ue_wrap::FVector      sentLoc{};
    ue_wrap::FRotator     sentRot{};
    uint64_t              lastMoveMs  = 0;
    uint64_t              nextProbeMs = 0;   // 0 = every tick; set while resting
};

std::vector<Driven> g_driven;   // game thread only
struct SettledWatch {
    ue_wrap::CachedObjRef ref;
    ue_wrap::FVector      loc{};
    ue_wrap::FRotator     rot{};
    uint64_t              expiresMs = 0;
};
std::vector<SettledWatch> g_settled; // recently ended host bodies, game thread only
struct LocalHeldMotion {
    ue_wrap::CachedObjRef ref;
    ue_wrap::FVector      velocity{};
    ue_wrap::FVector      lastLoc{};
    uint64_t              lastMs = 0;
};
LocalHeldMotion g_localHeldMotion; // sampled while held, for client-only kinematic contacts
struct NudgeIngress { uint64_t windowMs = 0; uint8_t count = 0; };
std::array<NudgeIngress, coop::net::kMaxPeers> g_nudgeIngress;
std::unordered_map<uint64_t, uint64_t> g_lastNudgeByTarget;
size_t              g_turn = 0; // where the next tick's publishing starts, so no prop waits forever
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_impactObserverInstalled = false;
// The claim generation is PER PROP: the receiver's closed-generation gate is keyed by eid and
// compares in a signed 8-bit window, so a counter shared by every prop would read a re-claim as
// older than the closed one after enough claims of other props in between. Outlives the row.
std::unordered_map<uint32_t, uint8_t> g_genByEid;

Driven* Find(void* actor) {
    for (Driven& d : g_driven)
        if (!d.dead && d.ref.Raw() == actor) return &d;
    return nullptr;
}

int32_t g_offReceiveHitOther = -1;
int32_t g_offReceiveOverlapOther = -1;
bool    g_overlapObserverInstalled = false;

bool HeldBySomeone(void* actor);

void SampleLocalHeldMotion() {
    void* held = coop::local_streams::LastHeldActor();
    if (!held || !R::IsLive(held)) {
        g_localHeldMotion.ref.Reset();
        g_localHeldMotion.velocity = {};
        g_localHeldMotion.lastMs = 0;
        return;
    }
    const uint64_t now = coop::active_drive::NowMs();
    const ue_wrap::FVector loc = E::GetActorLocation(held);
    if (g_localHeldMotion.ref.Get() == held && g_localHeldMotion.lastMs != 0) {
        const uint64_t elapsed = now - g_localHeldMotion.lastMs;
        // A long pause is not motion: retain no stale direction after a load or frame hitch.
        if (elapsed > 0 && elapsed <= 250) {
            const float scale = 1000.0f / static_cast<float>(elapsed);
            g_localHeldMotion.velocity = ue_wrap::FVector{
                (loc.X - g_localHeldMotion.lastLoc.X) * scale,
                (loc.Y - g_localHeldMotion.lastLoc.Y) * scale,
                (loc.Z - g_localHeldMotion.lastLoc.Z) * scale};
        } else {
            g_localHeldMotion.velocity = {};
        }
    } else {
        g_localHeldMotion.ref.Set(held);
        g_localHeldMotion.velocity = {};
    }
    g_localHeldMotion.lastLoc = loc;
    g_localHeldMotion.lastMs = now;
}

void SendClientNudge(coop::net::Session& session, void* actor, void* other) {
    void* local = coop::players::Registry::Get().Local();
    if (!local || !R::IsLive(local)) return;
    // A local capsule can push a prop, and so can the prop currently moved in that player's hand.
    // The latter is normally kinematic on the host, so its contact exists only in the client's
    // physics scene unless we forward this bounded intent.
    void* held = coop::local_streams::LastHeldActor();
    void* prop = actor == local ? other : other == local ? actor : nullptr;
    if (!prop && held && R::IsLive(held))
        prop = actor == held ? other : other == held ? actor : nullptr;
    if (!prop || !R::IsLive(prop) || !PR::IsDescendantOfProp(prop)) return;
    const auto eid = coop::prop_element_tracker::GetPropElementIdForActor(prop);
    if (eid == coop::element::kInvalidId || eid == 0u) return;
    // The struck body's post-contact velocity is the best direction. A kinematic hand move may
    // leave it at zero, so then use the held item's velocity, followed by the player capsule.
    ue_wrap::FVector v = E::GetActorVelocity(prop);
    float flat = std::sqrt(v.X * v.X + v.Y * v.Y);
    if ((!std::isfinite(flat) || flat < 30.f) && held && R::IsLive(held)) {
        v = E::GetActorVelocity(held);
        flat = std::sqrt(v.X * v.X + v.Y * v.Y);
    }
    if ((!std::isfinite(flat) || flat < 30.f) && g_localHeldMotion.ref.Get() == held) {
        v = g_localHeldMotion.velocity;
        flat = std::sqrt(v.X * v.X + v.Y * v.Y);
    }
    if (!std::isfinite(flat) || flat < 30.f) {
        v = E::GetActorVelocity(local);
        flat = std::sqrt(v.X * v.X + v.Y * v.Y);
    }
    if (!std::isfinite(flat) || flat < 30.f) return;
    static std::unordered_map<uint32_t, uint64_t> s_lastNudge;
    const uint64_t now = coop::active_drive::NowMs();
    uint64_t& last = s_lastNudge[static_cast<uint32_t>(eid)];
    if (now - last < kNudgeIntervalMs) return;
    last = now;
    coop::net::PropNudgePayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    p.dirX = v.X / flat; p.dirY = v.Y / flat;
    p.speedCmS = std::clamp(flat, kNudgeMinSpeedCmS, kNudgeMaxSpeedCmS);
    session.SendReliable(coop::net::ReliableKind::PropNudge, &p, sizeof(p));
}

// ReceiveHit is emitted after UE has resolved a blocking physics contact. This is the missing
// verb for a prop a moving prop knocks: the impacted prop has already received its impulse when
// this observer runs, so coast streams its actual host trajectory. Both the hit actor and the
// contacting 'Other' actor are evaluated, capturing knock chains even if only one has hit events.
void OnActorReceiveHitPost(void* actor, void* /*function*/, void* params) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || !session->connected()) return;
    if (!ue_wrap::game_thread::IsGameThread()) return;

    if (session->role() != coop::net::Role::Host) {
        void* other = nullptr;
        if (params && g_offReceiveHitOther >= 0)
            other = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(params) + g_offReceiveHitOther);
        SendClientNudge(*session, actor, other);
        return;
    }

    if (actor && (PR::IsDescendantOfProp(actor) || PR::IsKeyedInteractable(actor))) {
        Coast(actor, "physics impact");
    }
    if (params && g_offReceiveHitOther >= 0) {
        void* other = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(params) + g_offReceiveHitOther);
        if (other && R::IsLive(other) && (PR::IsDescendantOfProp(other) || PR::IsKeyedInteractable(other))) {
            Coast(other, "chain impact");
        }
    }
}

// ReceiveActorBeginOverlap is emitted when any actor (player capsule, puppet, vehicle) overlaps
// with another actor. Captures body bumps/walk-throughs where blocking hit events did not fire.
void OnActorReceiveBeginOverlapPost(void* actor, void* /*function*/, void* params) {
    auto* session = g_session.load(std::memory_order_acquire);
    if (!session || !session->connected()) return;
    if (!ue_wrap::game_thread::IsGameThread()) return;

    if (session->role() != coop::net::Role::Host) {
        void* other = nullptr;
        if (params && g_offReceiveOverlapOther >= 0)
            other = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(params) + g_offReceiveOverlapOther);
        SendClientNudge(*session, actor, other);
        return;
    }

    if (actor && (PR::IsDescendantOfProp(actor) || PR::IsKeyedInteractable(actor))) {
        Coast(actor, "actor overlap");
    }
    if (params && g_offReceiveOverlapOther >= 0) {
        void* other = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(params) + g_offReceiveOverlapOther);
        if (other && R::IsLive(other) && (PR::IsDescendantOfProp(other) || PR::IsKeyedInteractable(other))) {
            Coast(other, "actor overlap other");
        }
    }
}

void InstallImpactObserver() {
    void* actorClass = R::FindClass(P::name::ActorClass);
    if (!actorClass) return;

    if (!g_impactObserverInstalled) {
        void* receiveHit = R::FindFunction(actorClass, P::name::ActorReceiveHitFn);
        if (receiveHit) {
            g_offReceiveHitOther = R::FindParamOffset(receiveHit, L"Other");
            if (ue_wrap::game_thread::RegisterPostObserver(receiveHit, &OnActorReceiveHitPost)) {
                g_impactObserverInstalled = true;
                UE_LOGI("[PROP-DRIVE] HOST observing AActor.ReceiveHit (Other offset=%d) for chain-impact coast handoff",
                        g_offReceiveHitOther);
            }
        }
    }

    if (!g_overlapObserverInstalled) {
        void* receiveOverlap = R::FindFunction(actorClass, L"ReceiveActorBeginOverlap");
        if (receiveOverlap) {
            g_offReceiveOverlapOther = R::FindParamOffset(receiveOverlap, L"OtherActor");
            if (ue_wrap::game_thread::RegisterPostObserver(receiveOverlap, &OnActorReceiveBeginOverlapPost)) {
                g_overlapObserverInstalled = true;
                UE_LOGI("[PROP-DRIVE] HOST observing AActor.ReceiveActorBeginOverlap (Other offset=%d) for body knock/overlap coast",
                        g_offReceiveOverlapOther);
            }
        }
    }
}

// Scans a sliced budget of tracked world props on the host to catch any props moving from physics
// (player capsule pushes, depenetration impulses, rolls, falls) that are not yet enrolled in g_driven.
void ScanAwakeProps() {
    static std::vector<coop::prop_element_tracker::KeyIndexEntry> s_entries;
    static uint64_t s_lastRefreshMs = 0;
    static uint64_t s_nextScanMs = 0;
    static size_t s_cursor = 0;

    const uint64_t nowMs = coop::active_drive::NowMs();
    // GetPhysicsVelocity dispatches two reflected UE calls per prop. ReceiveHit and overlap cover
    // the immediate collision edge; this is the fallback for missed wakeups, so 20 Hz is ample
    // and avoids multiplying the entire world's physics reads by the frame rate.
    if (nowMs < s_nextScanMs) return;
    s_nextScanMs = nowMs + 50;
    if (s_entries.empty() || (nowMs - s_lastRefreshMs >= 1500)) {
        s_entries.clear();
        coop::prop_element_tracker::CollectKeyIndexEntries(s_entries);
        s_lastRefreshMs = nowMs;
        if (s_entries.empty()) return;
    }

    const size_t n = s_entries.size();
    const size_t batchSize = std::min<size_t>(48, n);
    for (size_t i = 0; i < batchSize; ++i) {
        s_cursor = (s_cursor + 1) % n;
        const auto& entry = s_entries[s_cursor];
        void* actor = entry.actor;
        if (!actor || !R::IsLiveByIndex(actor, entry.internalIdx)) continue;
        if (Find(actor) || HeldBySomeone(actor)) continue;
        if (!PR::IsDescendantOfProp(actor)) continue;

        const PR::VelocityState v = PR::GetPhysicsVelocity(actor);
        if (!v.ok) continue;

        const float speedSq = v.linearCmS.X * v.linearCmS.X +
                              v.linearCmS.Y * v.linearCmS.Y +
                              v.linearCmS.Z * v.linearCmS.Z;
        const float angSpeedSq = v.angularDegS.X * v.angularDegS.X +
                                 v.angularDegS.Y * v.angularDegS.Y +
                                 v.angularDegS.Z * v.angularDegS.Z;

        // Linear speed >= 12 cm/s or angular speed >= 12 deg/s indicates active physics motion.
        constexpr float kActiveVelSq = 12.0f * 12.0f;
        if (speedSq >= kActiveVelSq || angSpeedSq >= kActiveVelSq) {
            Coast(actor, "host physics wake / motion scan");
        }
    }
}

// A prop in a hand: this player's grab slot, a peer's held-prop stream, or the hotbar hand axis
// (the local hand actor and every peer's display mirror), which the hand lane owns outright.
bool HeldBySomeone(void* actor) {
    return actor == coop::local_streams::LastHeldActor() ||
           coop::remote_prop::IsActorUnderAnyDrive(actor) ||
           coop::hand_item::IsHandAxisActor(actor);
}

bool PoseDiffers(const ue_wrap::FVector& aLoc, const ue_wrap::FRotator& aRot,
                 const ue_wrap::FVector& bLoc, const ue_wrap::FRotator& bRot) {
    if (std::fabs(aLoc.X - bLoc.X) > kSendEpsCm) return true;
    if (std::fabs(aLoc.Y - bLoc.Y) > kSendEpsCm) return true;
    if (std::fabs(aLoc.Z - bLoc.Z) > kSendEpsCm) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(aRot.Pitch - bRot.Pitch)) > kSendEpsDeg) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(aRot.Yaw   - bRot.Yaw))   > kSendEpsDeg) return true;
    if (std::fabs(ue_wrap::NormalizeAxis(aRot.Roll  - bRot.Roll))  > kSendEpsDeg) return true;
    return false;
}

bool Moved(const Driven& d, const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot) {
    return !d.everSent || PoseDiffers(loc, rot, d.sentLoc, d.sentRot);
}

void WatchSettled(void* actor, const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot) {
    for (SettledWatch& watch : g_settled) {
        if (watch.ref.Raw() == actor) {
            watch.loc = loc;
            watch.rot = rot;
            watch.expiresMs = coop::active_drive::NowMs() + kSettledWatchMs;
            return;
        }
    }
    SettledWatch watch;
    watch.ref.Set(actor);
    watch.loc = loc;
    watch.rot = rot;
    watch.expiresMs = coop::active_drive::NowMs() + kSettledWatchMs;
    g_settled.push_back(std::move(watch));
}

void SweepSettled(uint64_t now) {
    for (size_t i = 0; i < g_settled.size();) {
        SettledWatch& watch = g_settled[i];
        void* actor = watch.ref.Get();
        if (!actor || now >= watch.expiresMs || HeldBySomeone(actor)) {
            g_settled[i] = std::move(g_settled.back());
            g_settled.pop_back();
            continue;
        }
        const ue_wrap::FVector loc = E::GetActorLocation(actor);
        const ue_wrap::FRotator rot = E::GetActorRotation(actor);
        if (PoseDiffers(loc, rot, watch.loc, watch.rot)) {
            UE_LOGI("[PROP-DRIVE] HOST late post-settle movement -- reopening coast");
            Coast(actor, "late post-settle movement");
            g_settled[i] = std::move(g_settled.back());
            g_settled.pop_back();
            continue;
        }
        ++i;
    }
}

void SendEnd(coop::net::Session& s, const Driven& d, void* actor, const ue_wrap::FVector& loc,
             const ue_wrap::FRotator& rot, const char* why) {
    coop::net::PropDriveEndPayload p{};
    p.eid = d.eid;
    p.gen = d.gen;
    // The host's own flags at this instant: the receiver restores the same parity the join's
    // converge applies, rather than guessing that a simulating prop is what it took off.
    p.physFlags = coop::prop_wire_parity::PhysFlagsOf(actor);
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.roll  = ue_wrap::NormalizeAxis(rot.Roll);
    // The body's velocity at this instant. At rest it is noise below kRestVelCmS and goes out as
    // zero, so no receiver wakes a resting body by writing a velocity into it. MTA's syncer
    // (CUnoccupiedVehicleSync.cpp, bSyncVelocity) streams a velocity in every packet it moved in;
    // ours rides the end edge only, since the receiver interpolates a parked body and has no use
    // for one before that.
    const PR::VelocityState v = PR::GetPhysicsVelocity(actor);
    const float lin = std::sqrt(v.linearCmS.X * v.linearCmS.X + v.linearCmS.Y * v.linearCmS.Y +
                                v.linearCmS.Z * v.linearCmS.Z);
    const float ang = std::sqrt(v.angularDegS.X * v.angularDegS.X + v.angularDegS.Y * v.angularDegS.Y +
                                v.angularDegS.Z * v.angularDegS.Z);
    if (v.ok && (lin >= kRestVelCmS || ang >= kRestAngVelDegS)) {
        p.linVelX = v.linearCmS.X;   p.linVelY = v.linearCmS.Y;   p.linVelZ = v.linearCmS.Z;
        p.angVelX = v.angularDegS.X; p.angVelY = v.angularDegS.Y; p.angVelZ = v.angularDegS.Z;
    }
    s.SendReliable(coop::net::ReliableKind::PropDriveEnd, &p, sizeof(p));
    UE_LOGI("[PROP-DRIVE] HOST END eid=%u gen=%u %s -> (%.1f,%.1f,%.1f) |v|=%.1f cm/s flags=0x%02x",
            d.eid, static_cast<unsigned>(d.gen), why, loc.X, loc.Y, loc.Z, v.ok ? lin : 0.f,
            static_cast<unsigned>(p.physFlags));
}

// A new row for `actor` when the channel can carry it: a keyed prop with a host element id, in no
// hand, of a class peers hold. Null otherwise. `claimed` false opens it coasting.
Driven* Open(void* actor, bool claimed, const char* why) {
    if (!PR::IsDescendantOfProp(actor)) return nullptr;
    if (HeldBySomeone(actor)) return nullptr;   // the held-prop lane owns a prop in a hand
    coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(actor);
    if (eid == coop::element::kInvalidId || eid == 0u) {
        // A live placed or spawned prop may not be enrolled yet; enroll it now so coasting is admitted.
        const std::wstring key = PR::GetInteractableKeyString(actor);
        const std::wstring cls = R::ClassNameOf(actor);
        if (!key.empty() && key != L"None" && !cls.empty()) {
            coop::prop_element_tracker::MarkPropElement(
                actor, key, cls, coop::prop_element_tracker::EnrollSource::kExpressSeam);
            eid = coop::prop_element_tracker::GetPropElementIdForActor(actor);
        }
    }
    if (eid == coop::element::kInvalidId || eid == 0u) {
        // A feeder asks again every pass while the tie holds, so the line is per actor, not per pass.
        static void* sRefusedSaid = nullptr;
        if (actor != sRefusedSaid) {
            sRefusedSaid = actor;
            // A class the wire never expresses has no copy on any peer to move, so its motion is
            // parity and its refusal no news.
            if (!coop::prop_lifecycle::IsWireSuppressedPropClass(R::ClassNameOf(actor)))
                UE_LOGW("[PROP-DRIVE] HOST refused: actor %p has no host element id (%s)", actor, why);
        }
        return nullptr;
    }
    if (coop::prop_lifecycle::IsWireSuppressedPropClass(R::ClassNameOf(actor))) return nullptr;
    Driven d;
    d.ref.Set(actor);
    d.eid = static_cast<uint32_t>(eid);
    d.claimed = claimed;
    const std::wstring keyW = PR::GetInteractableKeyString(actor);
    d.key.len = 0;
    for (size_t i = 0; i < keyW.size() && i < sizeof(d.key.data); ++i)
        d.key.data[d.key.len++] = static_cast<char>(keyW[i]);
    uint8_t& gen = g_genByEid[d.eid];
    if (++gen == 0) ++gen;   // 0 is the stream's "no generation"
    d.gen = gen;
    d.lastMoveMs = coop::active_drive::NowMs();
    g_driven.push_back(std::move(d));
    UE_LOGI("[PROP-DRIVE] HOST %s eid=%u gen=%u key='%ls' (%s) -- %zu driven",
            claimed ? "CLAIM" : "COAST", g_driven.back().eid, static_cast<unsigned>(g_driven.back().gen),
            keyW.c_str(), why, g_driven.size());
    return &g_driven.back();
}

}  // namespace

void Claim(void* actor, const char* reason) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::Claim");
    if (!actor) return;
    const char* why = reason ? reason : "";
    if (Driven* d = Find(actor)) {
        if (!d->claimed) {
            d->claimed     = true;   // a re-claim while coasting: the stream simply continues
            d->nextProbeMs = 0;
            UE_LOGI("[PROP-DRIVE] HOST re-claim eid=%u (%s)", d->eid, why);
        }
        return;
    }
    Open(actor, /*claimed=*/true, why);
}

void Coast(void* actor, const char* reason) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::Coast");
    if (!actor) return;
    if (Driven* d = Find(actor)) {
        // A claimed prop stays its verb's; either way the push starts its rest clock again and it is
        // read every tick.
        d->nextProbeMs = 0;
        d->lastMoveMs = coop::active_drive::NowMs();
        return;
    }
    Open(actor, /*claimed=*/false, reason ? reason : "");
}

void OnNudge(coop::net::Session& session, const coop::net::PropNudgePayload& p,
             uint8_t senderSlot) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::OnNudge");
    if (session.role() != coop::net::Role::Host || senderSlot == 0 ||
        senderSlot >= coop::net::kMaxPeers || p.elementId == 0 ||
        !std::isfinite(p.dirX) || !std::isfinite(p.dirY) || !std::isfinite(p.dirZ) ||
        !std::isfinite(p.speedCmS)) return;
    // Keep a small global ingress gate, but rate-limit the actual impulse by (sender, prop).
    // A held box clipping two loose boxes is two real contacts, not a reason to discard the
    // second one because the first message arrived in the same tenth of a second.
    const uint64_t now = coop::active_drive::NowMs();
    NudgeIngress& ingress = g_nudgeIngress[senderSlot];
    if (now - ingress.windowMs >= kNudgeIntervalMs) {
        ingress.windowMs = now;
        ingress.count = 0;
    }
    if (ingress.count >= kNudgeBurstPerInterval) return;
    ++ingress.count;
    const float flat = std::sqrt(p.dirX * p.dirX + p.dirY * p.dirY);
    if (flat < 0.9f || flat > 1.1f || std::fabs(p.dirZ) > 0.1f) return;
    const auto target = coop::element::IntentTarget::ForClientIntent(session, senderSlot, kNudgeReachUU)
        .Resolve(static_cast<coop::element::ElementId>(p.elementId), coop::element::ElementType::Prop);
    if (!target || HeldBySomeone(target.actor)) return;
    const uint64_t targetKey = (static_cast<uint64_t>(senderSlot) << 32) | p.elementId;
    uint64_t& lastTarget = g_lastNudgeByTarget[targetKey];
    if (now - lastTarget < kNudgeIntervalMs) return;
    lastTarget = now;
    void* mesh = PR::GetStaticMesh(target.actor);
    if (!mesh) return;
    const float speed = std::clamp(p.speedCmS, kNudgeMinSpeedCmS, kNudgeMaxSpeedCmS);
    coop::remote_prop::DriveSimulate(mesh, true);
    const PR::VelocityState old = PR::GetPhysicsVelocity(target.actor);
    coop::remote_prop::DriveSetLinearVelocity(mesh, p.dirX * speed,
                                               p.dirY * speed,
                                               old.ok ? old.linearCmS.Z : 0.f);
    Coast(target.actor, "client body nudge");
}

void Release(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::Release");
    Driven* d = Find(actor);
    if (!d || !d->claimed) return;
    d->claimed     = false;
    d->lastMoveMs  = coop::active_drive::NowMs();   // the rest clock starts now
    d->nextProbeMs = 0;                              // and the probe goes back to per tick
    UE_LOGI("[PROP-DRIVE] HOST release eid=%u -- coasting until it rests", d->eid);
}

void OnPeerWorldReady() {
    UE_ASSERT_GAME_THREAD("prop_drive_host::OnPeerWorldReady");
    if (g_driven.empty()) return;
    // Every driven prop's pose goes out again on the next tick, resting ones included, so the
    // joiner parks them where the other peers already have them. MTA's ResyncForPlayer
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp) sends the
    // entering player alone;
    // ours rides the broadcast stream, so the others receive a pose they already hold -- one
    // datagram per join, not worth a targeted lane.
    for (Driven& d : g_driven) {
        d.everSent    = false;
        d.nextProbeMs = 0;
    }
    UE_LOGI("[PROP-DRIVE] HOST world-ready re-send armed for %zu driven prop(s)", g_driven.size());
}

void Tick(coop::net::Session& s) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::Tick");
    g_session.store(&s, std::memory_order_release);
    InstallImpactObserver();
    SampleLocalHeldMotion();
    if (s.role() != coop::net::Role::Host) return;
    ScanAwakeProps();
    SweepSettled(coop::active_drive::NowMs());
    if (g_driven.empty()) return;
    const uint64_t now = coop::active_drive::NowMs();
    static uint32_t sPublished = 0;
    // Publishing starts where the last tick's joins left off, so with more driven props than one send
    // carries, none waits forever for its turn.
    const size_t n = g_driven.size();
    size_t joined = 0;
    for (size_t k = 0; k < n; ++k) {
        Driven& d = g_driven[(g_turn + k) % n];
        void* actor = d.ref.Get();
        if (!actor) {
            // The destroy crossed on its own seam; a receiver drops a drive whose actor died.
            UE_LOGI("[PROP-DRIVE] HOST eid=%u died -- dropped", d.eid);
            d.dead = true;
            continue;
        }
        if (HeldBySomeone(actor)) {
            // A hand took it: the held-prop lane streams it from here and its release hands the
            // velocity back. The end edge closes this stream's generation first, whatever the
            // prop's turn in the queue.
            SendEnd(s, d, actor, E::GetActorLocation(actor), E::GetActorRotation(actor), "taken by a hand");
            d.dead = true;
            continue;
        }
        if (now < d.nextProbeMs) continue;   // resting: read at the slow cadence
        // A pose read before its turn would be overwritten before any send.
        const coop::net::PoseTurn turn = s.PropDrivePoseTurn(d.eid);
        if (turn == coop::net::PoseTurn::Wait) continue;
        const ue_wrap::FVector  loc = E::GetActorLocation(actor);
        const ue_wrap::FRotator rot = E::GetActorRotation(actor);
        if (Moved(d, loc, rot)) {
            coop::net::PropPoseSnapshot pp{};
            pp.key       = d.key;
            pp.elementId = d.eid;
            pp.ctx       = d.gen;
            pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
            pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
            pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
            pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
            s.PublishPropDrivePose(pp);   // queued: it goes out in its turn, refreshed until then
            if (turn == coop::net::PoseTurn::Join) ++joined;
            d.everSent    = true;
            d.sentLoc     = loc;
            d.sentRot     = rot;
            d.lastMoveMs  = now;
            d.nextProbeMs = 0;
            if ((++sPublished % 120) == 1)
                UE_LOGI("[PROP-DRIVE] HOST publish eid=%u gen=%u %s -> (%.1f,%.1f,%.1f)",
                        d.eid, static_cast<unsigned>(d.gen), d.claimed ? "claimed" : "coasting",
                        loc.X, loc.Y, loc.Z);
            continue;
        }
        if (now - d.lastMoveMs >= kRestMs) {
            const PR::VelocityState v = PR::GetPhysicsVelocity(actor);
            const float speed = std::sqrt(v.linearCmS.X * v.linearCmS.X +
                                          v.linearCmS.Y * v.linearCmS.Y +
                                          v.linearCmS.Z * v.linearCmS.Z);
            const float angularSpeed = std::sqrt(v.angularDegS.X * v.angularDegS.X +
                                                 v.angularDegS.Y * v.angularDegS.Y +
                                                 v.angularDegS.Z * v.angularDegS.Z);
            if (v.ok && (speed >= kRestVelCmS || angularSpeed >= kRestAngVelDegS)) {
                // The actor is still physically live even though its pose has moved less than one
                // outbound delta. Keep it host-driven until the real body settles.
                d.nextProbeMs = now + kSettleProbeMs;
                continue;
            }
            if (!d.claimed) {
                SendEnd(s, d, actor, loc, rot, "rested");
                WatchSettled(actor, loc, rot);
                d.dead = true;
                continue;
            }
            d.nextProbeMs = now + kRestProbeMs;   // claimed and resting: the slow cadence
        }
    }
    g_turn = n ? (g_turn + joined) % n : 0;
    g_driven.erase(std::remove_if(g_driven.begin(), g_driven.end(),
                                  [](const Driven& d) { return d.dead; }),
                   g_driven.end());
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    g_driven.clear();
    g_settled.clear();
    g_localHeldMotion.ref.Reset();
    g_localHeldMotion.velocity = {};
    g_localHeldMotion.lastMs = 0;
    g_nudgeIngress = {};
    g_lastNudgeByTarget.clear();
    g_turn = 0;
    g_genByEid.clear();
}

}  // namespace coop::prop_drive_host
