// coop/props/container_park.h -- the inbound holding pen for a container slice whose container is
// not resolvable on this peer yet: a birth skew, or a mid-activity join where the contents ride the
// normal lane while the PropSpawn rides bulk. One entry per eid, latest wins; the lane beside it
// (container_contents_sync) owns the parse and the apply and hands them back through the replay
// callback, so nothing here knows the blob's grammar. Game thread throughout.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::props::container_park {

// How many entries one CLIENT author may hold at once. Reaching it evicts that author's oldest, so
// a newer slice is never refused in favour of a stale one. Host-authored parks are not capped, and
// the asymmetry is measured rather than assumed: a client's pen holds HOST-authored slices and a
// measured join filled it with 284 of them, while a host's pen holds client-authored ones, where a
// peer names WHAT it writes and never what holding it costs.
inline constexpr size_t kMaxParksPerClient = 8;

// How long an entry may wait outside a join bracket before it is dropped as a leak guard. Park
// aging is EVENT-anchored, not wall clock from arrival: under backpressure a contents slice
// systematically precedes its PropSpawn, so while a join snapshot is in flight parks do not age at
// all; at Complete every park is re-stamped and this clock runs from there.
inline constexpr uint64_t kTtlMs = 30'000;

// Hold this blob until its eid resolves, on behalf of the peer that authored it. Latest wins per
// eid; the caller's clock, like every other bounded table in this tree.
//
// MTA drops a packet naming an element it does not know, with no else branch
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp, Packet_CustomData). We hold
// instead, because here the element usually arrives a moment later: our props stream in behind
// their own spawn rows, so a slice naming an unknown eid is early far more often than it is wrong.
void Admit(uint32_t eid, uint8_t authorSlot, std::vector<uint8_t>&& blob, uint64_t nowMs);

// Re-offer each parked blob to its own author's arbitration. The callback returns false only when
// the container STILL does not resolve; anything else (applied, refused, malformed) retires the
// entry.
using ReplayFn = bool (*)(const std::vector<uint8_t>& blob, uint8_t authorSlot);

// Replay what can be replayed, and drop what has waited past the TTL outside a join bracket.
void Sweep(ReplayFn replay, uint64_t nowMs);

// The join snapshot's bracket, from the client-side SnapshotBegin / SnapshotComplete dispatch.
void NoteJoinBracket(bool open, uint64_t nowMs);

size_t Size();

void Reset();

// The arithmetic selftest, un-gated at session start: the cap that evicts this author's own oldest
// and nobody else's, the host author that is not capped at all, the re-park that does not grow the
// table, the TTL that drops an entry only outside a bracket, and a replay that retires what it
// dealt with.
bool RunSelftest();

}  // namespace coop::props::container_park
