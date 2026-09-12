/*
 * apj_dragstat.h - APJ-OS: measure the live window-drag pipeline
 *
 * A live drag (title bar, left button) is not driven by mouse events.
 * XaAES samples the pointer once, posts the position to the window's
 * owner, and IGNORES further motion until that one step has gone all
 * the way round: client event -> WM_MOVED -> wind_set(WF_CURRXYWH) ->
 * WM_REDRAW to every window uncovered -> all of them serviced
 * (C.move_block 1 -> 2 -> 3 -> 0). The next sample is then taken from
 * wherever the pointer is NOW. So the window moves in steps whose size
 * is pointer speed x round-trip time, and the pointer itself (drawn
 * from the raw packets) stays smooth.
 *
 * This records, per drag, how long each phase took and how much motion
 * was skipped, and appends one report per drag to DRAGSTAT.LOG next to
 * the XaAES kernel module.
 *
 * XaAES is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _apj_dragstat_h_
#define _apj_dragstat_h_

/* k_mouse.c: XA_move_event posted the sampled position to the owner */
void apj_ds_sample(short x, short y);
/* k_mouse.c: adi_move - a pointer packet arrived; blocked = C.move_block */
void apj_ds_motion(short blocked);
/* c_window.c: send_moved queued WM_MOVED */
void apj_ds_moved(void);
/* c_window.c: move_window ran for the dragged window; nred = C.redraws */
void apj_ds_set(long nred);
/* every place C.move_block goes back to 0. why: 1 cevent only, 2 no
 * redraws, 3 all redraws serviced, 4 redraw timeout (lagging client) */
void apj_ds_unblock(short why);
/* k_mouse.c: button released while a widget was active */
void apj_ds_end(const char *owner);

#endif
