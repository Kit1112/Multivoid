// harness/autotest/gt_task.h -- post one body to the game thread and wait for it, bounded, and
// say whether it LANDED.
//
// The drills all need this and nine of them hand-rolled it. The two properties the hand-rolled
// copies lack are the ones that bite:
//
//   1. The return value. A bounded wait that gives up silently is indistinguishable from a body
//      that ran and produced a false -- so a drill grades a negative assertion green on a game
//      thread that never ran its task. Every caller here must read the bool.
//   2. Outputs owned by the caller's FRAME are a use-after-free. The wait is bounded, the task is
//      not: the ProcessEvent detour's transparent bypass parks the whole task queue for up to 30 s
//      (net_pump's flee arms exactly that), so a task posted before it runs long after the waiter
//      returned. Capture a shared_ptr by VALUE, never `&local`.

#pragma once

#include "ue_wrap/core/game_thread.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <utility>

namespace harness::autotest {

// Run `body` on the game thread; true if it completed within `timeoutMs`. A false return means the
// game thread did not drain the queue in time and NOTHING the body was supposed to write was
// written -- treat every output as unread, not as false.
template <typename Fn>
bool RunOnGameThread(Fn&& body, int timeoutMs = 10000) {
    auto done = std::make_shared<std::atomic<int>>(0);
    ue_wrap::game_thread::Post([done, body]() mutable { body(); done->store(1); });
    const int spins = timeoutMs / 5;
    for (int i = 0; i < spins && done->load() == 0; ++i) ::Sleep(5);
    return done->load() != 0;
}

}  // namespace harness::autotest
