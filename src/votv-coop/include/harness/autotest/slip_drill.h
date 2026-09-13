// harness/autotest/slip_drill.h -- the slip drill's entry points.
//
// Its own header rather than a row in harness/autotest.h: that catalog is a 350-line list of
// forty unrelated routines and sits exactly on the prose gate's comment ceiling, so every new
// drill that lands in it makes it worse. New drills declare themselves here-style, beside the
// module they belong to, the way death_state_probe.h does.

#pragma once

#include <windows.h>

namespace harness::autotest {

// Solo host with a live session. A banana peel slips the local player twice -- once with the
// pawn's `dead` false, the control, which must only ragdoll, and once with it true -- to settle
// whether a zero-damage ragdoll re-enters the death chain, and whether our run-ending seam
// refuses the menu travel that chain asks for. Env VOTVCOOP_RUN_SLIP_DRILL=1; lines tagged [SLIP].
void RunSlipDrill();
DWORD WINAPI SlipDrillThread(LPVOID arg);

}  // namespace harness::autotest
