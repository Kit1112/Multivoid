// coop/props/trash_sweep.h -- HOST: the clumps a broom stroke sets rolling cross to every peer as
// clumps, under the ids they had, and roll there as they roll here.
//
// The broom copies the pile's to-clump body into its own graph, so the clump is born of the broom's
// bytecode and no verb of the pile's runs. The deferred-spawn seam reads which pile the stroke was
// on out of the broom's frame, moves that pile's id onto the clump at its birth (the husk then dies
// with no id) and notes the clump here. A tick later the finish has placed it and the stroke's push
// has launched it, and it is opened as a carry nobody holds: one to-clump convert, then its pose on
// the host-originated clump stream until the carry latch closes, which the clump's own re-pile, its
// rest or its death decide in trash_channel. A stroke's push also moves clumps lying at rest whose
// carry had closed; their latch opens again with no convert, since they are clumps everywhere
// already. A clump that enters a hand leaves this stream for the hand's. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::trash_sweep {

// Host, from the deferred-spawn seam: `clump` was born of a broom stroke and already carries the
// swept pile's certificate (trash_channel::NoteClumpBorn has run). Game thread.
void NoteSwept(void* clump);

// Host, from the broom's push: `clump` was pushed by a stroke. Game thread.
void NotePushed(void* clump);

// Host, per gameplay tick after trash_channel::TickCarry: open the carry of each clump swept since
// the last tick and of each resting clump pushed, and publish the pose of every clump still rolling
// whose turn it is. `localPlayer` is the local player, whose hand is read this tick.
// Game thread.
void Tick(coop::net::Session& s, void* localPlayer);

// Session end: forget the pending and rolling clumps and zero the tally. Game thread.
void OnDisconnect();

}  // namespace coop::trash_sweep
