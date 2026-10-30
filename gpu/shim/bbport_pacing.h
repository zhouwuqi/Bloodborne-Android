// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: display pacing (Bionic). When each frame really reached the screen, read from the release
// fences the app hands back (tools/android/present/bb_present_proto.h): the release fence of a buffer
// signals when the display stops reading it, which is when the frame after it went on screen. With
// the app in FIFO mode (bbport.ini vsync=1) every frame is shown in order, so the release times of
// two frames in a row are the start and the end of the second one on the screen.
//
// From those times:
//  - a "Pacing:" line every 10 s (BB_PACING_STATS=0: off): how long the frames stayed on screen
//    against the frame interval the game handed them over at (30 FPS on a 60 Hz panel: two refreshes,
//    33.3 ms each), how many left the game evenly but reached the screen unevenly, the time from the
//    hand-over to the screen, and the thermal state (BbAdpf's governor);
//  - a phase lock for the frame limiter (BB_PACE_LOCK=0: off; FIFO and no frame generation only):
//    the limiter's slots follow the display instead of the CPU clock alone. A proportional term keeps
//    the hand-over-to-screen time where it settled (no slow drift against the panel's own clock), and
//    when frames that left evenly reach the screen unevenly (the slots sit on a latch deadline) the
//    slots move by half a refresh, the farthest point from the deadlines on both sides.
#pragma once
#include "common/types.h"

namespace BbPacing {
/// The swap thread handed frame `frame` (Swapchain's counter) to the app.
void FrameSent(u64 frame);
/// The app released `frame`. `fence_fd`: its release fence, borrowed (duplicated here when kept);
/// -1: released at once. `fifo`: the connection's mode (newest wins gives the frames it replaced
/// unseen their own acquire fence, so the times are only approximate there).
void FrameReleased(u64 frame, int fence_fd, bool fifo);
/// The frame limiter's next slot (steady clock, ns) as it computed it with `period_ns`: the slot to
/// use. Unchanged while the lock is off, not settled, or the frames are not shown in FIFO order.
s64 AdjustSlot(s64 slot_ns, s64 period_ns);
/// The thermal state for the stats line: AThermal status, forecast headroom (NaN: unknown) and the
/// governor's level (bbport_adpf.cpp).
void NoteThermal(int status, float headroom, int level);
} // namespace BbPacing
