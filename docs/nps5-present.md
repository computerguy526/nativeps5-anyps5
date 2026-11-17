# nps5: presentation pacing and frames in flight (branch nps5-present)

Everything here is behind environment flags and off by default.

## What the presentation path does (a00d5ff82f)

- **vblank beat**: `VideoOutDriver::vblankLoop` is a free-running 119.88 Hz timer on the host steady clock
  (every second tick is a 59.94 Hz vblank). It has no relation to the screen. A late wake-up is caught up with a
  burst of vblank events, each stamped "now".
- **flip**: the guest's flip (a packet in a DCB, or `sceVideoOutSubmitFlip`) is *reserved* when the guest submits
  it (`flipPendingNum`, `FlipQueue::reservations`), becomes *GPU-ready* when the driver's queue worker reaches it
  (`FlipRequest::GpuReady`), and is then taken by the presenter thread (`processFlip`): wait until the vblank
  counter is at least `lastFlipVblank + flipRate + 1`, blit to the swapchain image, `vkQueuePresentKHR`, complete
  (flip event, `flipStatus`, `flipPendingNum--`).
- **swapchain**: 3 images, `VK_PRESENT_MODE_FIFO_KHR` (KosmicKrisp offers FIFO and IMMEDIATE only;
  `ANYPS5_PRESENT_MODE` selects). The acquire has a 1 ms timeout: a frame that finds no free image is dropped, so
  the swapchain itself never holds more than two frames.
- **how far the guest can run ahead**: `VIDEO_OUT_FLIP_QUEUE_CAPACITY` = 16 pending flips. A submission that holds
  a flip waits (`WaitForFlipRoom`) only when 16 are pending. Nothing else ties the guest to the picture.

## Flags

| flag | effect |
|---|---|
| `APS5_PRESENT_LINK=1` | The vblank beat follows the display the window is on (CVDisplayLink, `DisplayLinkBeat`). While that display refreshes at 60 or 120 Hz (30 Hz too) each refresh advances the guest's counter by 2 or 1 ticks of 119.88 Hz, so a flip that waits for a vblank is released at the screen's own refresh and the guest's vblank stamps are the screen's. A display at another rate (75, 144, 50 Hz), a silent link (hidden window, sleeping display: no tick for 3 refreshes) and a missing link use the host-clock beat; the link takes over again at its next tick. In both modes a hitch gives one vblank event that advances the counter by the ticks missed, never a burst, and the host-clock beat goes on from the last refresh (nxnative b010f2b8: the beat restarts at a frame the link paced, instead of running ahead or behind real time). The window is followed when it moves to another display. Log: `[videoout] vblank beat: ...`. |
| `APS5_PRESENT_DEPTH=<n>` | The guest's submission of a flip is held while `n` flips are pending, so the title is at most `n` frames ahead of the picture instead of 16 (nxnative 34ada028, `DM_PRESENT_DEPTH`: the acquire waits until the present of n frames back is on screen). The hold is at most 250 ms (a flip that could only complete after the title's next submission costs that wait, never a hang). Nothing is dropped. |
| `APS5_PRESENT_DRAIN=1` | A flip taken while another GPU-ready flip already stands behind it waits one vblank, not `flipRate + 1`. At a 30 fps cap (`sceVideoOutSetFlipRate(1)`, `rhi.SyncInterval=2`) frames queued by a hitch otherwise each stay one frame late for as long as the title holds the cap (nxnative 34ada028: "no frames left queued after a hitch"). Nothing is dropped or shown twice. No effect at flip rate 0. |
| `APS5_TRACE_PRESENT=1` | One `[pflip]` line per completed flip: reserve time, delay to GPU-ready, to the presenter taking it, to completion; `pend` = flips pending at the reserve (frames in flight); `behind` = ready flips queued behind it; `holds=a/b` = depth holds / holds that timed out. |
| `APS5_DUMP_SIG=1` (with `APS5_DUMP_FRAMES=<n>`) | No BMPs: every presented frame is read back and reduced to a 64x36 grid; `[sig] idx t diff` gives the mean difference to the previous frame. With `APS5_TRACE_LAT=1` (`[lat] inject`, from nps5-inputlag; `APS5_TRACE_LAT_AXIS=1` also counts scripted stick moves) the first frame whose `diff` jumps after an injection is the input's first changed picture. The readback is a full display buffer per frame (4K on PPSA30552): use it for latency runs, not for fps numbers. |

## Merge notes

- nps5-qos registers the vblank thread at the top of `vblankLoop` (`ThreadQos::Register(Pacer, "vblank")`): on a
  merge that line has to stay above the `presentLinkRequested()` branch, and `ThreadQos::NoteFrame()` belongs in
  the `vblankLoopLinked` loop too. The CVDisplayLink callback thread is the system's own (it only bumps a counter).
- The branch contains nps5-inputlag 76ec3c07d6 (cherry-pick) for `APS5_TRACE_LAT`.
