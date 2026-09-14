// coop/props/trash_broom_intent.h -- on a client, a broom stroke names the pile it struck instead
// of emptying it.
//
// A stroke calls `trashBitsPile_C::broomed` on every dispenser pile within reach: the body pops up
// to three props by a rolled name, spawns each as a real actor, drops one of the counter pair per
// pop and destroys the pile once both reach zero. On a client the counter drop and the depletion
// crossed while the spawn had no channel at all, the finish-spawn seam being host-only, so every
// other peer watched the pile empty with no trash coming out -- which two field reporters
// described as the trash being deleted outright.
//
// The remedy is the grab lane's, one file over: the client does not author the outcome, it names
// its subject. The verb is refused per call at the script-body gate -- the broom reaches it with an
// EX_LocalVirtualFunction, below any ProcessEvent detour -- the pile's own save key goes up as a
// BroomIntent, and the host runs the game's `broomed` on its own pile. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::trash_broom_intent {

// Resolve the verb, watch its body and assert the gate's enable while the session runs
// (idempotent; the session pointer is re-cached on every call so a reconnect keeps the refusal
// live). Safe before trashBitsPile_C loads: the class resolve is throttled and never latches on a
// miss, so a class that is not resident yet is picked up later. Game thread.
void Install(coop::net::Session* session);

// HOST receiver: the client in `senderSlot` struck the pile with this key. Resolves it against the
// counter mirror's index, asks whether that sender could have reached it, and runs the game's own
// `broomed`. The pile self-destructs inside the call when the stroke empties it, so nothing here
// touches it afterwards. Called from event_feed's reliable drain. Game thread.
void OnBroomIntent(coop::net::Session& session, const std::wstring& key, uint8_t senderSlot);

// Zero the tallies so a second session's numbers are its own. Game thread.
void OnSessionStart();

// Drop the cached session: with no session the verb is refused nowhere and a single-player broom
// behaves exactly as the game wrote it. Game thread.
void OnDisconnect();

}  // namespace coop::trash_broom_intent
