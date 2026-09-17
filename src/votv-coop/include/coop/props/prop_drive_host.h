// coop/props/prop_drive_host.h -- HOST-side: the props in motion under something other than a
// hand.
//
// The held-prop stream is sourced from the player's grab slot alone, so a prop a hook's constraint
// pulls -- or one still sliding after the hook let go -- had no channel: the host's copy moved and
// every other copy stood still. This is that channel. A verb that puts a host prop under an
// external drive CLAIMS it here (the hook lane is the first); the claim outlives the verb until
// the body rests; while claimed or coasting the prop's pose goes out on MsgType::PropDrivePose
// whenever it moved, and one reliable PropDriveEnd closes the stream with the final pose, the
// host's physics flags and the velocity. MTA's unoccupied-vehicle syncer, with the host as the
// only syncer since props are host-authoritative.
//
// HOST-ONLY, game thread. Verb-fed and never scanned: the set holds only what a caller claimed.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct PropNudgePayload; }

namespace coop::prop_drive_host {

// Put `actor`, a keyed Aprop_C descendant this host owns, under the drive. Idempotent while the
// claim holds, so a feeder may call it every pass for every prop it sees tied; a re-claim of a
// coasting prop cancels its end. A prop in a hand -- this player's grab slot, a peer's held-prop
// stream, or the hotbar hand axis -- is refused silently, since the hand lane owns it, and so is a
// class the wire never expresses. `reason` names the verb for the log. Game thread.
void Claim(void* actor, const char* reason);

// The verb let go of `actor`: the prop coasts under the stream until it rests, then the end edge
// fires. A no-op for an unclaimed actor. Game thread.
void Release(void* actor);

// An instant push moved `actor`: stream it, coasting, until it rests. A prop a verb has claimed
// stays claimed, and one already coasting starts its rest clock again; the refusals are Claim's.
// `reason` names the push for the log. Game thread.
void Coast(void* actor, const char* reason);

void OnNudge(coop::net::Session& session, const coop::net::PropNudgePayload& nudge,
             uint8_t senderSlot);

// A peer's world just came up: every driven prop's current pose is re-sent on the next tick, so
// the joiner parks the resting ones too, which the delta gate would otherwise never send it.
// Game thread.
void OnPeerWorldReady();

// Per gameplay tick: every role installs the body-contact observer; the host additionally reads
// driven props, publishes movement and closes streams that settled. Game thread.
void Tick(coop::net::Session& s);

// Session end. Game thread.
void OnDisconnect();

}  // namespace coop::prop_drive_host
