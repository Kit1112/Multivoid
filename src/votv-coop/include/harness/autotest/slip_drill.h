// harness/autotest/slip_drill.h -- the slip drill's entry points.
//
// Its own header rather than a row in harness/autotest.h. That catalog is 349 lines declaring 45
// unrelated routines, and it sits at EXACTLY the prose gate's half-comment floor -- 210 comment
// lines against 90 of code, 300 counted, where the rule trips above 300. Measured: adding one
// comment and two declarations takes it to 303 and the gate refuses the commit. So a new drill
// cannot be declared there at all. Declaring it beside its own module is also the shape the
// tree is moving toward (death_state_probe.h); finishing that move for the other 45 is a lane of
// its own, and until it runs both homes exist.

#pragma once

#include <windows.h>

namespace harness::autotest {

// Solo host, or a client linked to one (`mp.py slip --client`). A banana peel slips the local
// player twice -- once with the pawn's `dead` false, the control, which must only ragdoll, and
// once with it true -- to settle whether a zero-damage ragdoll re-enters the death chain, and
// whether our run-ending seam refuses the menu travel that chain asks for. It needs a live
// session either way. Env VOTVCOOP_RUN_SLIP_DRILL=1; lines tagged [SLIP].
void RunSlipDrill();
DWORD WINAPI SlipDrillThread(LPVOID arg);

}  // namespace harness::autotest
