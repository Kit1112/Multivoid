// coop/props/container_park.cpp -- see coop/props/container_park.h.

#include "coop/props/container_park.h"

#include "ue_wrap/core/log.h"

#include <chrono>
#include <map>

namespace coop::props::container_park {
namespace {

struct Parked {
    std::vector<uint8_t> blob;
    std::chrono::steady_clock::time_point at;
};

std::map<uint32_t, Parked> g_parked;
constexpr int kTtlSec = 30;

bool g_joinBracketOpen = false;

}  // namespace

void Admit(uint32_t eid, std::vector<uint8_t>&& blob) {
    g_parked[eid] = Parked{std::move(blob), std::chrono::steady_clock::now()};
    UE_LOGI("container_contents: eid=%u not resolvable yet -- parked (TTL %ds)", eid, kTtlSec);
}

void Sweep(ReplayFn replay) {
    if (g_parked.empty() || !replay) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (replay(it->second.blob)) {
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
    const auto now = std::chrono::steady_clock::now();
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
