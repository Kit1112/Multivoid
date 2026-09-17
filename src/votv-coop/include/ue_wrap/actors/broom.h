// ue_wrap/actors/broom.h -- engine access for the broom (prop_broom_C). Principle-7 engine-wrapper
// layer: no network or coop state.
//
// One broom stroke is the ubergraph entry its "clean" montage notify reaches. It aims with the
// holder's `arm`, then runs three loops over what a 50 uu sphere at the hit point overlaps: each
// dispenser pile gets `broomed`, each chip pile is turned into a clump inline, and each physics body
// is pushed along the holder's heading, with the holder's velocity added. The chip-pile loop is the
// one no verb of the pile's announces, so the pile it is on can only be read out of the broom's own
// frame.
//
// Nothing here holds the broom's class or its functions, since a Blueprint class dies with the
// world that loaded it: a broom is known by its class's name, and each function is looked up on the
// class of the broom in hand. What is kept is layout -- the holder field, the notify's name
// parameter, the loop's pile local -- the same in every world that loads the same cooked class.

#pragma once

#include "ue_wrap/core/ufunction_hook.h"   // CallerFrame

#include <cstdint>

namespace ue_wrap::broom {

// Resolve the names a broom is known by: its class, its ubergraph and the notify name that starts
// a stroke. Dispatches the string-to-name conversion, so call it from a top-level game-thread pass,
// never from inside a hook. Idempotent; true once resolved.
bool ResolveNames();

// The dispatch function carrying the "clean" montage notify. Prefer the profile's measured name,
// but rediscover a recooked OnNotifyBegin_* function when its GUID changed. Empty until ResolveNames.
const wchar_t* StrokeNotifyFunctionName();

// True iff `obj` is a broom (prop_broom_C or a subclass). False before ResolveNames. Game thread.
bool IsBroom(void* obj);

// Read the stroke's layout off `broom`'s class: the holder field, the notify function's name
// parameter and the stroke loop's pile local. One walk of the class per process, so call it with a
// broom in hand at the entry of a stroke, before its loops run. True once resolved; a layout this
// build lacks is logged once and stays unresolved. Game thread.
bool ResolveLayout(void* broom);

// True when a call of the notify function carries the name that starts a stroke; every other name
// the montage sends reaches the same body and does nothing. `locals` is that call's frame. False
// before ResolveNames and ResolveLayout. Game thread.
bool IsStrokeNotify(const uint8_t* locals);

// The chip pile the stroke loop of `broom` is on, when `frame` is that loop's frame: a native call
// issued by the broom's ubergraph reports the ubergraph as its frame, and the loop keeps its
// current pile in a local the spawn and the destroy both read. Null for any other object or frame,
// and before ResolveLayout. Reads only. Game thread.
void* SweptChipPile(void* broom, const ue_wrap::ufunction_hook::CallerFrame& frame);

// prop_broom_C::player, the holder a stroke aims with and pushes along: the broom's own graph
// writes it when the right mouse button takes the broom up and clears it on release. Null before
// ResolveLayout. Game thread.
void* ReadHolder(void* broom);
bool WriteHolder(void* broom, void* player);

// Run a stroke on `broom` the way its montage notify does: the notify function with the stroke's
// name, through ProcessEvent. False when unresolved or the call did not dispatch. Game thread.
bool FireStroke(void* broom);

// The broom's right mouse button, as the player's input reaches the item in its hand: the press
// takes the broom up and plays the swing montage, whose notify runs each stroke; the release puts
// it down. False when the function does not resolve or the call did not dispatch. Game thread.
bool PressUse(void* broom, void* player);
bool ReleaseUse(void* broom, void* player);

}  // namespace ue_wrap::broom
