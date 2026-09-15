// coop/items/broom_push.h -- HOST: what a broom stroke sets moving moves the same on every peer.
//
// The stroke's last loop sets a velocity on the root body of every physics actor in its sphere,
// along the holder's heading with the holder's velocity added. A pushed prop is in nobody's hand,
// so no held-prop stream carries it, and every other peer saw it stand still. The push is caught at
// the native seam on the velocity setter -- from the broom's graph only -- and each pushed prop
// coasts on the driven-prop channel (coop/props/prop_drive_host), streaming while it slides and
// closing when it rests; a prop a verb already holds stays that verb's. A pushed clump is trash,
// not a prop, and goes to the trash lane (coop/props/trash_sweep), which streams its roll.
//
// The trash the stroke knocks out of a dispenser pile falls from where the pile's bounds put it,
// and every peer let its own copy tumble to wherever its physics left it. Its spawn is caught at the
// finish of the deferred spawn, at a seam armed only while a `broomed` body runs and only for the
// spawns that pile's own bytecode makes, and it coasts the same way. Game thread; the velocity seam
// is installed on every peer, the spawn seam on a host, and both act on the host alone.

#pragma once

namespace coop::net { class Session; }

namespace coop::broom_push {

// Install the seam once and re-cache the session; a seam that cannot install is said once and not
// retried. Game thread.
void Install(coop::net::Session* session);

// Host: dispenser pile `pile`'s `broomed` body begins, and ends -- the window in which a spawn that
// pile's bytecode finishes is trash the stroke knocked out of it. Game thread.
void OnDispenseBegin(void* pile);
void OnDispenseEnd();

// Host, per gameplay tick after the host spawn watcher's drain: hand what the strokes since the last
// tick pushed and dispensed to the lane that streams it. The order is the point: a prop a stroke
// dispensed has no element id until the drain names it, so a stream opened before the drain is
// refused. Game thread.
void Tick();

// Session end: forget pushes not yet handed on and report the tally. Game thread.
void OnDisconnect();

}  // namespace coop::broom_push
