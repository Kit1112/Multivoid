// coop/dev/container_selftest.h -- an instrument for the container contents lane
// (`[dev] container_selftest=1`, or `VOTVCOOP_CONTAINER_SELFTEST=1` per run; it MUTATES the world).
//
// An idle two-peer run cannot exercise this lane: nobody opens a container, and a save load fills
// `saveSlot.GObjStack` wholesale rather than through the watched verbs, so the lane's central claim
// -- that the `addObject`/`takeObj` watch ENTERS on each peer -- stays invisible to it.
//
// It dispatches `prop_container_C::extract(0)` and nothing else, because extract's first act is
// `propInventory->takeObj(...)` dispatched blueprint-internally: the call WE make is the outer one
// and the mutation to be caught is the game's own inner dispatch. The host fires at +10 s on the
// nearest non-empty container to its own body, the client at +25 s on the second-nearest
// (`VOTVCOOP_CONTAINER_FIRE_MS` overrides that delay); both print a DIGEST every 5 s and one
// VERDICT fifteen seconds after their own fire. RED means the trigger changed nothing or the watch
// never fired -- the two failures that void every other line in the run.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_selftest {

// Cache the session pointer. Call once at boot (subsystems Install). No-op with the flag off.
void Install(coop::net::Session* session);

// WHAT IT DOES NOT MEASURE, so nobody reads it as a pass: this peer never walks, so a client firing
// from across the room authors a slice the host refuses on reach, and the verdict reports REVERTED
// as a legal outcome. The ACCEPTED client path belongs to `mp.py ctakerace`, whose peers walk to a
// shared container and whose orchestrator sums the item across them.

// Game thread, per tick. Drives the two scheduled dispatches and the 5s digest. No-op with the
// flag off, before the session connects, or once both dispatches have fired.
void Tick();

// Clear the schedule + the resolved containers so a reconnect re-runs the circle.
void OnDisconnect();

}  // namespace coop::dev::container_selftest
