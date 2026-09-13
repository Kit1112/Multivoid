// coop/props/container_write_policy.cpp -- see coop/props/container_write_policy.h.

#include "coop/props/container_write_policy.h"

#include "ue_wrap/core/log.h"

#include <map>

namespace coop::props::container_write_policy {
namespace {

// Host: the content most recently published for an eid by any route, fan-out or a targeted
// connect seed. This is the compare-and-swap baseline ("what did I tell that peer the world looked
// like"); the lane's own g_sentHash answers a different question ("may I skip the next fan-out"),
// and a targeted send must not answer yes to that one.
std::map<uint32_t, uint64_t> g_publishedHash;

// This peer's own last verb edge per eid; the host uses it to detect a client write that raced a
// host-side change.
std::map<uint32_t, uint64_t> g_localChangeMs;

uint64_t g_refused = 0;

}  // namespace

Decision Judge(const Inputs& in) {
    // An up-to-date author edited from what the host last published; an author that never received
    // anything sends 0 and is refused rather than trusted.
    if (in.baseHash == 0 || in.baseHash != in.publishedHash) return Decision::StaleBase;
    // The author edited the published world; still refused if the host changed this container
    // inside the conflict window, a change in flight the author provably had not seen.
    if (in.lastLocalChangeMs != 0 && in.nowMs - in.lastLocalChangeMs <= kConflictWindowMs)
        return Decision::HostChangeInFlight;
    return Decision::Accept;
}

Decision Accept(uint32_t eid, uint64_t baseHash, uint8_t authorSlot, uint64_t nowMs) {
    Inputs in;
    in.baseHash = baseHash;
    in.nowMs    = nowMs;
    if (auto it = g_publishedHash.find(eid); it != g_publishedHash.end()) in.publishedHash = it->second;
    if (auto it = g_localChangeMs.find(eid); it != g_localChangeMs.end()) in.lastLocalChangeMs = it->second;

    const Decision d = Judge(in);
    if (d == Decision::Accept) return d;

    ++g_refused;
    // The failed condition is named: reported together, a never-published container once read as a
    // racing host change.
    UE_LOGW("container_contents: CONFLICT eid=%u slot %u -- %s (author base=%llu, host published=%llu). "
            "Write REFUSED; re-publishing host truth to the author. Total refused this session: %llu",
            eid, static_cast<unsigned>(authorSlot),
            d == Decision::HostChangeInFlight
                ? "a HOST-side change is in flight within the conflict window"
                : "the author edited a state the host has not published (STALE BASE)",
            static_cast<unsigned long long>(in.baseHash),
            static_cast<unsigned long long>(in.publishedHash),
            static_cast<unsigned long long>(g_refused));
    return d;
}

void NotePublished(uint32_t eid, uint64_t contentHash) { g_publishedHash[eid] = contentHash; }

void NoteLocalChange(uint32_t eid, uint64_t nowMs) { g_localChangeMs[eid] = nowMs; }

void Reset() {
    g_publishedHash.clear();
    g_localChangeMs.clear();
    g_refused = 0;
}

}  // namespace coop::props::container_write_policy
