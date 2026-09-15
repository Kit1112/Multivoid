# Trash piles

## Purpose

The ambient trash piles, the clump a pile becomes in a hand, and the trash-bits dispenser
piles: the whole collect loop of grab, carry, throw and re-pile across peers, and the identity
problem that makes this the hardest prop family. What is built, and how a mirror of the game's
own actor is kept from authoring its own transitions. Ordinary props are on [props.md](props.md).

## How it works

### Why piles are hard

A pile (`actorChipPile_C`) has no identity of any kind: no key, a key setter that writes nothing
anyone reads, and a save record of class, transform and chip type that the game loads by
destroying every pile and respawning them from their positions. Grabbing a pile runs its
Blueprint's to-clump, which spawns a separate clump actor (the carried ball) and destroys the
pile; landing re-piles, which spawns a fresh pile and destroys the clump. One logical thing
changes actor twice per carry, and nothing in the game names it across peers. The only handle
it can have is an element id the host mints and streams. A base save holds several hundred.

### Identity: the host's id and the sync context

The trash channel (`coop/props/trash_channel`) treats a trash entity as a host-minted id that
moves across pile, clump and pile again, rebound onto each successor at its birth: the id is the
logical entity, position is never identity, so a dense cluster cannot mis-bind. Every transition (grab, throw, land) bumps a
per-id sync-time context the host stamps on every convert and carry packet, and a receiver drops
a packet older than the id's known generation, so a carry packet still in flight when the entity
re-piles is never applied to the re-skinned entity. This is MTA's element sync-time context.

The pile-to-clump and clump-to-pile links are caught at the one seam that fires on every
dispatch route, the native function seam on the engine's deferred-spawn call. The grab direction
records a clump birth certificate that the held-object edge consumes; the land direction converts
the re-piled clump in place onto the exact spawned pile, the same tick, with no proximity search.

### The mirror on a client

The pile form on a client is the game's own `actorChipPile_C`. At a join it is the client's own
save-loaded pile, bound to the host's id where the host says the pile was at save time
(`coop/props/pile_spawn_bind`); a pile with no counterpart -- one derived during the join window
or born in play -- is a rooted runtime pile instead (`coop/props/trash_mirror`), spawned
with its tick and physics off and its root movable, skinned with the host's chip type, scale and
rotation. Either way it is bound and marked save-native, so it rides the same machinery: the pose
drive, the position correction, the grab route, the morph hand-off, the sweep exemption and
retire. A real pile is what the game's look-at trace accepts, so the hover prompt, collision,
occlusion and rotation are the game's. A rooted native stays live and inert; the earlier belief
that a runtime pile "dies on its own" was garbage collection of an unrooted actor.

A pile mirrors nothing of its own brain. A pile turns itself into a clump when any prop overlaps
its collision component and when something calls its grab event, and a clump re-piles itself on
its first level contact; each spawns the successor and destroys the actor it ran on. On a client
every trash transition is the host's, so those three are refused at the dispatch
(`coop/props/trash_morph_gate`) and the host's own convert performs the change.

The clump form, in a hand or in flight, is the game's own `prop_garbageClump_C`, made by the
same module on the same recipe (`coop/props/trash_mirror`), with one addition: its collision is
off while it is carried, because it renders in a puppet's hand with no holder to be attached to
and would otherwise block the player carrying it. It stays off for the flight too, which is a
host-driven pose stream rather than local physics; the pile it lands as is a fresh actor with the
game's own collision.

A form change is a class change, so there is no re-skin in place: the successor is made parked,
the one identity is rebound onto it at its birth, and only then is the predecessor destroyed. One
mirror implementation serves both forms.

### Grab, carry, throw, land

The host grabs natively. Its held-edge detector streams the clump's pose like any held prop, and
the release edge keeps streaming the clump's flight until it re-piles, because the clump's
release runs through neither of the verbs a hook could see.

A client's grab is an intent. The E press is intercepted before the native grab
(`coop/props/trash_use_intercept`) and the client sends the id of the pile it is looking at;
the host checks that the sender may name that pile (`coop/element/intent_authority`) and
performs the grab on the requester's puppet natively, which engages and holds on an unpossessed
pawn. The puppet's own tick is dead, so the host drives the held clump to the
puppet's hand each tick (`coop/player/puppet_carry_drive`) and streams the clump's pose as a
host-originated per-id batch to every client, the requester included
(`coop/props/trash_clump_pose_stream`), rendered with the same fixed-delay interpolation as a
held prop. A second E press is the release toggle and the left button a hard throw with a
direction; both are throw intents the host performs. Landing is an atomic convert from the host:
the pile materialises at the landing pose under the same id and the clump retires after it.
Pickup and landing sounds are synthesised on peers: the game plays them only for the actor.

### The carry latch and the land settle

The game churns a held clump: about once a second it re-piles on contact with a cluster and
immediately re-grabs the result. Broadcasting every one of those would re-skin and teleport the
client's rendering each cycle, so the host holds a per-id carry latch from the real grab to the
real land, and suppresses the churn inside it -- no convert, no context bump. A churn re-grab
rebinds the id onto the new clump so the pose stream keeps tracking it.

Telling churn from a real landing needs one wait: a re-pile opens a settle window instead of
broadcasting. A re-grab inside the window cancels it as churn; the window expiring commits the
to-pile convert and closes the latch. The window is self-correcting either way -- too short
commits a churn re-pile that the re-grab then re-opens, a brief flicker; too long lags the land
by a few frames. Neither strands the id.

Every open carry must eventually close, so the host's tick also terminates lanes the normal path
would leave open: a clump destroyed mid-carry (consumed, or its holder gone) closes the lane and
broadcasts a destroy, so no client is stuck holding a dead mirror; a clump left lying un-held closes
the lane silently and leaves the clump world-tracked and re-grabbable, which is what single-player
does. A clump re-piles only on a hit its own gate passes: its re-pile has armed, a random 0.5 to 1 s
after its birth (1 to 2 s for a clump a pile kicks into being); its last holder's hand is empty; the
surface is within about 41 degrees of level (about 104 degrees, short of a ceiling, for one chip
type); and what it hits is not
a simulating body. So a throw whose thrower's hand is busy at the land leaves one, and so does a
roll that ends before the clump arms, or with no such hit after. When such a clump re-piles later,
its convert waits for the same settle, so every peer sets the pile down where it was placed and not
where the clump was, a radius above; a settle whose pile is gone before it commits is dropped, since
whatever took the pile reports itself. `[V]` a clump whose carry had closed at rest, knocked up by
the broom drill, landed as a pile 0.0 cm from the host's on the client.

### Trash-bits piles

The dispenser piles ("uses 6 of 7") are keyed save actors; a press, the vacuum or the broom
dispenses items and decrements a counter pair inside the Blueprint. Each peer polls its
indexed piles and broadcasts the pair on a decrease; receivers apply a per-component minimum,
so concurrent collects converge, and the host's connect snapshot is applied as sent
(`coop/props/trash_pile_sync`). Depletion destroys the pile inside the Blueprint, caught by a
death-watch in the poll and broadcast as the ordinary keyed destroy: outside a transition window,
an indexed pile that vanishes is a depletion if this peer has just run a destroying verb on it, or
if it vanished near the local camera, and any other disappearance is a sublevel stream-out. The
first rule exists because the host runs a client's broom stroke on its own pile, which can die
anywhere in the world; widening the camera test to every player's body instead would read a
stream-out beside a remote player as a depletion. The dispensed item is born keyless and grabbed the same frame; the held-edge broadcast mints it a
key and spawns the mirror on every peer (`coop/props/trash_collect_sync`).

The vacuum and the broom put their items on the floor: each pop spawns a real actor at a rolled
transform, one per vacuum call and up to three per broom stroke, and the pile is destroyed once
both counters reach zero. A client's broom
authors none of that: the verb is refused per call at the script-body gate, the pile's own key
goes up as a `BroomIntent`, and the host runs the game's own `broomed`, whose trash, counter
drop and depletion then leave the host on the three channels above
(`coop/props/trash_broom_intent`). Before that, the two halves that had channels crossed and
the spawn did not, so a client's stroke read to everyone else as the trash being deleted.

### Save-loaded piles at a join

A joiner's world comes from the host's save, so the client has its own copy of every pile. The
host expresses each with its id and the pile's save-time position; the client binds its own
save-loaded actor to the host's id by that position, at spawn time (`coop/props/pile_spawn_bind`)
or at quiescence (`coop/element/quiescence_drain`), and a pile the host moved during the window
is corrected by the host's position message, with the identity following the host's word rather
than the frozen save position. The index-to-id sidecar on [join.md](join.md) is the intended
replacement for the position key.

## Who owns what

| State | Owner | Shape |
|---|---|---|
| a pile at rest | the host | an id and a rooted native mirror; the client's save-loaded actor is bound to it |
| a carried clump | the host | a client's grab is performed on the host; the pose is host-originated |
| grab, throw, land | the host, by intent | a client's press is an intent, reach-checked |
| the pile-to-clump identity | the host | one id, rebound onto each successor at its birth, guarded by the sync context |
| a dispenser's counters | each peer, minimum wins | the host as sent at join |
| a broom stroke | the host | a client's stroke is an intent naming the pile by key, reach-checked |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `GrabIntent`, `ThrowIntent` | a client to the host | the pile id; the release or a hard throw with its direction |
| `BroomIntent` | a client to the host | the key of the dispenser pile its broom stroke struck |
| `PropConvert` | the host to all | the atomic pile-clump convert: id, form, pose, scale, chip type, context |
| `TrashCarryPose` (stream) | the host to all | per-id pose batches for client-grabbed clumps |
| `PropPose` (stream) | the host to all | the host's own held clump and its flight |
| `PropSnapPos` | the host to one joiner | a position correction for a pile moved in the window |
| `TrashPileState` | each peer, relayed | a dispenser's counter pair |
| `PropDestroy` | either role | a depleted dispenser |

## Late join

The snapshot carries one spawn per pile with its id and save-time position, the client binds its
own actors by that position, the membership sweep removes what the host never claimed, and a
clump held by someone at the moment of the join binds without a duplicate. A pile the host moved
during the window arrives as a position correction after the snapshot.

The client's own piles do not all exist when the snapshot arrives -- its save load is still
draining -- so an expression that finds no actor at its save-time position is HELD rather than
answered: the id is recorded and the quiescence sweep binds the actor when it appears. The hold is
bounded. An id whose actor never appears is given up after the sweep's retry budget and that pile
is absent on that client until a later join expresses it again; nothing is invented at the stale
save position, because the host may have moved or removed the pile since.

## Known limits

| Limit | Evidence |
|---|---|
| A clump has its collision off for its whole life -- carried and in flight -- so a player walks through the ball someone else is holding or has thrown. The pile it lands as has the game's own collision | `[V]` `coop/props/trash_mirror` |
| Trash dropped into a garbage container updates the container on the host only: every client's container has its brain cancelled, so none of them -- not even the one whose player dropped the trash -- ever learns what is inside it, and the two pickup flags the game writes from those contents stay frozen | `[V]` `coop/interactables/garbage_sync`, and the cancelled Blueprint body read from the cook |
| Dispenser piles born by an event carry per-process keys and never resolve across peers | `[V]` `coop/props/trash_pile_sync` |
| A client's vacuum on a dispenser pile spawns its item on that client only: the broom's intent does not cover the vacuum's own spawn | `[V]` the vacuum verb's bytecode; `coop/props/trash_broom_intent` watches the broom verb alone |
| Sweeping CHIP piles with the broom does not cross in either direction: a client's stroke leaves the host's piles untouched, and a host's stroke makes them vanish on clients until they re-pile. The broom copies the pile's morph into its own graph, so the host's birth seam never sees a clump born | `[V]` `coop/props/trash_collect_sync` (the birth seam's source test), and a hands-on |
| The join-window bind is by save-time position; the sidecar that replaces it is off by default | `[V]` see [join.md](join.md) |

## Code map

| Concept | Files |
|---|---|
| identity and the transitions | `coop/props/trash_channel`, `coop/props/trash_grab_intent.cpp` |
| the mirrors, in both forms | `coop/props/trash_mirror`, and `coop/props/trash_morph_gate` for the verbs a client refuses |
| the client's grab and throw | `coop/props/trash_use_intercept`, `coop/player/puppet_carry_drive`, `coop/props/trash_clump_pose_stream`, `coop/props/active_drive` |
| the dispenser piles | `coop/props/trash_pile_sync`, `coop/props/trash_collect_sync` |
| garbage containers | `coop/interactables/garbage_sync` |
| the join | `coop/props/pile_spawn_bind`, `coop/element/quiescence_drain`, `coop/props/save_time_retire_util.h`, `coop/props/save_identity_map`, `coop/props/save_identity_bind` |
| tests | `harness/autotest/autotest_chippile.cpp`, `harness/autotest/autotest_clump.cpp` |
