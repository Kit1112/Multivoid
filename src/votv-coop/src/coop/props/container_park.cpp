// coop/props/container_park.cpp -- see coop/props/container_park.h.

#include "coop/props/container_park.h"

#include "ue_wrap/core/log.h"

#include <chrono>
#include <map>

namespace coop::props::container_park {
namespace {

using Clock = std::chrono::steady_clock;

struct Parked {
    std::vector<uint8_t> blob;
    Clock::time_point at;
    uint8_t authorSlot = 0;
};

std::map<uint32_t, Parked> g_parked;
constexpr int kTtlSec = 30;

bool g_joinBracketOpen = false;

// The oldest entry this author holds, and how many it holds. Bounded by the map, which is bounded
// by this cap times the peer count; a linear walk of that is cheaper than a second index.
size_t CountFor(uint8_t authorSlot, uint32_t& outOldestEid) {
    size_t n = 0;
    Clock::time_point oldest = Clock::time_point::max();
    outOldestEid = 0;
    for (const auto& kv : g_parked) {
        if (kv.second.authorSlot != authorSlot) continue;
        ++n;
        if (kv.second.at < oldest) { oldest = kv.second.at; outOldestEid = kv.first; }
    }
    return n;
}

}  // namespace

void Admit(uint32_t eid, uint8_t authorSlot, std::vector<uint8_t>&& blob) {
    // Only a NEW eid grows the table: replacing this author's own entry costs nothing.
    if (authorSlot != 0 && g_parked.find(eid) == g_parked.end()) {
        uint32_t oldestEid = 0;
        if (CountFor(authorSlot, oldestEid) >= kMaxParksPerClient && oldestEid != 0) {
            UE_LOGW("container_contents: slot %u holds %zu parked slices -- evicting its oldest "
                    "(eid=%u) to admit eid=%u", static_cast<unsigned>(authorSlot),
                    kMaxParksPerClient, oldestEid, eid);
            g_parked.erase(oldestEid);
        }
    }
    g_parked[eid] = Parked{std::move(blob), Clock::now(), authorSlot};
    UE_LOGI("container_contents: eid=%u not resolvable yet -- parked (TTL %ds, author slot %u)",
            eid, kTtlSec, static_cast<unsigned>(authorSlot));
}

void Sweep(ReplayFn replay) {
    if (g_parked.empty() || !replay) return;
    const auto now = Clock::now();
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (replay(it->second.blob, it->second.authorSlot)) {
            it = g_parked.erase(it);
        } else if (!g_joinBracketOpen && now - it->second.at > std::chrono::seconds(kTtlSec)) {
            UE_LOGW("container_contents: parked eid=%u expired after %ds unresolved -- dropped",
                    it->first, kTtlSec);
            it = g_parked.erase(it);
        } else {
            ++it;
        }
    }
}

void NoteJoinBracket(bool open) {
    if (g_joinBracketOpen == open) return;
    g_joinBracketOpen = open;
    if (open) return;
    const auto now = Clock::now();
    for (auto& kv : g_parked) kv.second.at = now;
    if (!g_parked.empty()) {
        UE_LOGI("container_contents: snapshot bracket closed -- %zu park(s) re-stamped, "
                "TTL runs from now (leak-guard)", g_parked.size());
    }
}

void Reset() {
    g_parked.clear();
    g_joinBracketOpen = false;  // session state must not survive the session
}

}  // namespace coop::props::container_park
