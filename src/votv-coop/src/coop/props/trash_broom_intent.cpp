// coop/props/trash_broom_intent.cpp -- see coop/props/trash_broom_intent.h.

#include "coop/props/trash_broom_intent.h"

#include "coop/element/intent_authority.h"   // may this sender name this pile
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/trash_pile_sync.h"      // the key -> pile index this lane names piles through

#include "ue_wrap/actors/prop.h"             // TrashBitsPileClass / GetInteractableKeyString
#include "ue_wrap/core/call.h"               // ParamFrame / Call
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"         // FindDispatchFunction / ClassOf / FindFunction
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"        // the verb name

#include <atomic>
#include <chrono>

namespace coop::trash_broom_intent {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace UP = ue_wrap::prop;
namespace sg = ue_wrap::script_gate;

// The session the role gate and the sender read. The verdict carries no session of its own, so it
// reads this cached pointer; Install re-stores it on every call and OnDisconnect clears it.
std::atomic<coop::net::Session*> g_session{nullptr};

// Registration is process-wide (a UFunction outlives a session), so the watch latches once it is
// on; the role gate below is what turns the refusal on and off.
bool g_installed = false;
std::chrono::steady_clock::time_point g_lastResolveAt{};
// The verb, kept from the resolve Install already paid for. The host executor used to look it up
// per packet, and that lookup is a walk of the whole object array with no cache and no climb to the
// declaring class -- so it cost a full walk per stroke AND would have missed a subclass the client's
// own watch covers, refusing on one peer and denying on the other.
void* g_broomedFn = nullptr;
// The class resolved but the verb is not on it: a definitive answer, not a not-yet. Latched so the
// retry stops paying for a walk, and warned once instead of every two seconds forever.
bool g_verbAbsent = false;

// This watch's own id, echoed back in the call record and the logs. It is not what keeps the lanes
// apart: the gate keys a registration on the UFunction, and this one watches a verb no other
// consumer does.
constexpr int kTagBroomed = 4;

// What the stroke did this session. A lane that never says it fired cannot be told apart from one
// whose verb never dispatches, and the two call for opposite fixes. Game-thread serial, so plain
// ints.
int g_seen = 0, g_refused = 0, g_sent = 0;

// The reach the host allows a client's stroke: the broom's own geometry. Its trace runs the
// player's CURRENT armLength -- 200 uu is the CDO default and the fallback, but the equip path
// overwrites it from the held item's weapons row, so this is a nominal figure, not the game's
// constant -- and the stroke then sweeps a 50 uu sphere at the hit point. Both halves are read off
// the broom's own ubergraph. The authorizer adds the target's bounds and a pose-staleness pad on
// top, which is what absorbs the difference between the nominal arm and the real one.
constexpr float kBroomReachUU = 250.f;

// True while this peer must not author a world change: a live session in which we are not the host.
// Single-player and the host both answer false, and the verb then runs exactly as the game wrote it.
bool MustNotAuthor() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() != coop::net::Role::Host;
}

// A client's stroke, refused and forwarded. The pile is named by the save key its counter mirror
// and its depletion destroy already use, which is a raw field read on the Aactor_save_C base -- no
// ProcessEvent dispatch, so it is safe to take here, inside the VM's own loop.
sg::Verdict OnBroomed(const sg::Call& call) {
    ++g_seen;
    if (!MustNotAuthor()) return sg::Verdict::Run;   // the host and single-player sweep natively
    ++g_refused;
    auto* s = g_session.load(std::memory_order_acquire);
    const std::wstring key = call.object ? UP::GetInteractableKeyString(call.object) : std::wstring();
    if (key.empty() || key == L"None") {
        // A pile with no key is in no peer's index, so no message can name it and the host could
        // not resolve one if we sent it. Refusing leaves the pile exactly as it stands on both
        // peers, which is the honest outcome; running would empty it here alone.
        if (g_refused <= 3)
            UE_LOGW("[BROOM-INTENT] pile %p has no save key -- stroke refused and NOT forwarded "
                    "(the pile is in no peer's index, so nothing can name it)", call.object);
        return sg::Verdict::Cancel;
    }
    if (s) {
        coop::net::BroomIntentPayload p{};
        p.key.len = 0;
        for (size_t i = 0; i < key.size() && i < 31; ++i)
            p.key.data[p.key.len++] = static_cast<char>(key[i]);
        if (s->SendReliable(coop::net::ReliableKind::BroomIntent, &p, sizeof(p))) ++g_sent;
        // The first three only, then the session tally. A swing calls this once per overlapped pile
        // and a held broom swings as fast as the player presses, so anything unconditional here is a
        // line a second for as long as someone is sweeping. The caller rides along because it is the
        // evidence for this lane's premise: null means the body was reached through ProcessEvent,
        // and the broom's own route must show its ubergraph instead.
        if (g_refused <= 3)
            UE_LOGI("[BROOM-INTENT] CLIENT SENT #%d key='%ls' -> host (own stroke refused; caller=%p)",
                    g_refused, key.c_str(), call.callerFunction);
    }
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)
    // This lane owns its own enable, as the morph gate does: the gate's switch is shared, and
    // riding another consumer's would leave this watch registered and its verdict silent the moment
    // that consumer retired. Scoped to a live session, because a refusal is scoped to one too.
    if (session && session->running()) sg::SetEnabled(true);
    if (g_installed) return;
    // FindClass does not memoise a miss, so an unresolved class walks the whole object array on
    // every attempt; this runs from the per-tick install pump, so the retry is throttled the way
    // the sibling trash lanes throttle theirs.
    const auto now = std::chrono::steady_clock::now();
    if (g_lastResolveAt.time_since_epoch().count() != 0 &&
        (now - g_lastResolveAt) < std::chrono::seconds(2))
        return;
    g_lastResolveAt = now;
    if (g_verbAbsent) return;   // asked and answered; the class is loaded and the verb is not on it
    void* cls = UP::TrashBitsPileClass();
    if (!cls) return;  // class not loaded yet -> retry on a later tick
    void* declarer = nullptr;
    void* fn = R::FindDispatchFunction(cls, P::name::PileBroomedFn, &declarer);
    if (!fn) {
        // Not a not-yet: the class IS resolved, so this build genuinely has no such verb. Each
        // attempt costs a walk of the object array per superclass hop, so it is asked once.
        g_verbAbsent = true;
        UE_LOGW("[BROOM-INTENT] '%ls' is not on the dispenser pile class in this build -- a client "
                "can still empty a pile locally, and this lane is inert", P::name::PileBroomedFn);
        return;
    }
    // An EXACT watch, not a watch by name: the answer is expected to be the dispenser pile's own
    // declaration, and one watch there covers every variant under it.
    if (!sg::Watch(fn, kTagBroomed, &OnBroomed, nullptr)) {
        UE_LOGW("[BROOM-INTENT] the script-body gate refused the watch on '%ls' (not installed, "
                "native, or the table is full)", P::name::PileBroomedFn);
        return;
    }
    g_broomedFn = fn;   // the host executor calls this, rather than re-walking per packet
    g_installed = true;
    UE_LOGI("[BROOM-INTENT] watching the broom verb at the script-body gate -- '%ls' declared on "
            "'%ls'; on a client every stroke is the host's", P::name::PileBroomedFn,
            R::ToString(R::NameOf(declarer)).c_str());
}

// MTA answers a client's request to act -- VEHICLE_ATTEMPT_FAILED and its siblings
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3042) -- and the sibling grab lane
// in this folder follows it, because a denied grab leaves the client holding a wrong belief that
// only a reply can correct. This lane deliberately diverges and stays silent: the client cancelled
// its own body before sending, so a denial leaves nothing diverged to heal, only a swing that did
// nothing.
void OnBroomIntent(coop::net::Session& session, const std::wstring& key, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (key.empty()) return;
    void* pile = coop::trash_pile_sync::ResolveByKey(key);
    if (!pile) {
        // The key names no live pile here: the pile already depleted under an earlier stroke, the
        // host's index belongs to a world it is reloading, or the pile is one of the event-born
        // dispensers whose keys are per-process and never resolve across peers. The swing is
        // DROPPED, and a drop is safe because the client authored nothing -- both peers still hold
        // the pile exactly as it stood. It is not parked: a stroke applied seconds after the swing
        // is its own defect, and the next swing asks again.
        UE_LOGW("[BROOM-INTENT] DENIED key='%ls' slot=%u -- no live pile under that key on the host",
                key.c_str(), senderSlot);
        return;
    }
    // Could this sender have reached it? The identity question is already answered -- the key
    // resolved a live pile of the right class through the counter mirror's own index -- so this is
    // the reach primitive rather than the eid resolve the grab lane takes.
    const auto tok = coop::element::IntentTarget::ForClientIntent(session, senderSlot, kBroomReachUU);
    const coop::element::IntentSubject sub = tok.Authorize(pile);
    if (!sub) {
        UE_LOGW("[BROOM-INTENT] DENIED key='%ls' slot=%u -- REASON=%s (dist=%.0f allowed=%.0f); the "
                "pile is real and untouched, the sender is just not near it",
                key.c_str(), senderSlot, coop::element::OutcomeName(sub.outcome),
                sub.distUU, sub.reachUU);
        return;
    }
    if (!g_broomedFn) {
        UE_LOGW("[BROOM-INTENT] DENIED key='%ls' slot=%u -- the broom verb never resolved here",
                key.c_str(), senderSlot);
        return;
    }
    UE_LOGI("[BROOM-INTENT] EXEC key='%ls' slot=%u pile=%p (dist=%.0f allowed=%.0f)",
            key.c_str(), senderSlot, pile, sub.distUU, sub.reachUU);
    // The stroke may be this pile's last. Say so BEFORE running it: the depletion watch cannot see
    // a Blueprint-internal destroy and would otherwise have to judge this death by how near the
    // HOST's camera is, which for a client's stroke is a question about the wrong player.
    coop::trash_pile_sync::NoteAuthoredDeath(key);
    {
        // The frame is allocated zeroed, and the verb's one parameter is dead -- the thunk writes
        // `location` to the persistent frame and no instruction reads it back, the trash's
        // transform being rolled from the pile's own component bounds -- so a zeroed frame IS the
        // faithful call, the hit result the grab lane leaves zeroed for the same reason.
        ue_wrap::ParamFrame pf(g_broomedFn);
        ue_wrap::Call(pile, pf);   // the pile self-destructs HERE when the stroke empties it
    }
    // `pile` may be dangling now. Everything the stroke produced leaves the host on its own
    // channel: each spawned prop through the finish-spawn seam, the counters through the pile
    // state poll, and the death through the depletion watch.
}

void OnSessionStart() { g_seen = g_refused = g_sent = 0; }

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    // The session's tally, once. Zero SEEN means the verb never dispatched in this session, which
    // is a fact about the game, not about the watch.
    UE_LOGI("[BROOM-INTENT] session tally -- %d stroke(s) seen, %d refused, %d forwarded",
            g_seen, g_refused, g_sent);
}

}  // namespace coop::trash_broom_intent
