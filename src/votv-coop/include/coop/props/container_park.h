// coop/props/container_park.h -- the inbound holding pen for a container slice whose container is
// not resolvable on this peer yet: a birth skew, or a mid-activity join where the contents ride the
// normal lane while the PropSpawn rides bulk. One entry per eid, latest wins; the lane beside it
// (container_contents_sync) owns the parse and the apply and hands them back through the replay
// callback, so nothing here knows the blob's grammar.
//
// Park aging is EVENT-anchored, not wall clock from arrival: under backpressure a contents slice
// systematically precedes its PropSpawn, and a slow link can hold the bulk stream past any fixed
// TTL with no wire loss at all. While a join snapshot is in flight parks do not age; at Complete
// every park is re-stamped and the TTL runs from there as a leak guard (Complete is lane-ordered
// after every PropSpawn it brackets). Game thread throughout.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::props::container_park {

// Hold this blob until its eid resolves. Latest wins per eid.
void Admit(uint32_t eid, std::vector<uint8_t>&& blob);

// Re-offer each parked blob. The callback returns false only when the container STILL does not
// resolve; anything else (applied, refused, malformed) retires the entry.
using ReplayFn = bool (*)(const std::vector<uint8_t>& blob);

// Replay what can be replayed, and drop what has waited past the TTL outside a join bracket.
void Sweep(ReplayFn replay);

// The join snapshot's bracket, from the client-side SnapshotBegin / SnapshotComplete dispatch.
void NoteJoinBracket(bool open);

void Reset();

}  // namespace coop::props::container_park
