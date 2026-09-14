// harness/autotest/broomstroke.h -- the broom-stroke drill's own declaration.
//
// Beside its implementation rather than in harness/autotest.h: that catalogue carries a paragraph
// per drill and had reached the line at which a documented declaration file counts as an essay, so
// the next drill added to it fails the prose gate whatever its comment says. A drill is one
// feature; this is where its interface belongs.
//
// What it measures is a broom stroke counted as TRASH on both peers, in both directions. Phase 1:
// the client strikes a dispenser pile until it empties; phase 2: the host strikes the second pile.
// Both peers must GAIN trash around the pile in both phases. The peer that is not striking is left
// where the game put it and its distance is MEASURED, not arranged -- placing it was what dropped
// the host onto a sub-level travel trigger -- and phase 1 only proves its point while that distance
// is outside the radius a depletion is judged by, which the verdict carries.
// Env VOTVCOOP_RUN_BROOM_STROKE=1, on both peers.

#pragma once

#include <windows.h>

namespace harness::autotest {

void RunBroomStrokeProbe();
DWORD WINAPI BroomStrokeProbeThread(LPVOID arg);

}  // namespace harness::autotest
