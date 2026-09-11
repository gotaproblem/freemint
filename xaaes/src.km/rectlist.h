/*
 * XaAES - XaAES Ain't the AES (c) 1992 - 1998 C.Graham
 *                                 1999 - 2003 H.Robbers
 *                                        2004 F.Naumann & O.Skancke
 *
 * A multitasking AES replacement for FreeMiNT
 *
 * This file is part of XaAES.
 *
 * XaAES is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * XaAES is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with XaAES; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _rectlist_h
#define _rectlist_h

#include "global.h"

#define RECT_SYS	0
#define RECT_OPT	1
#define RECT_TOOLBAR	2

bool is_inside(const GRECT *r, const GRECT *o);

/*
 * APJ-OS Fluent rounded window corners (phase 5). A rounded window's
 * shape is carved into the rectangle lists: windows above occlude with
 * their rounded shape (a few row strips + the body), a window's own list
 * leaves out its corner steps, so corner pixels belong to - and are drawn
 * by - whatever lies beneath. No compositing, no stale pixels.
 */
#define APJ_SHAPE_MAX	40		/* >= 4 x steps (radius <= 8) + body */

struct xa_window;
short apj_corner_steps(struct xa_window *wind, const short **inset);	/* rows carved per corner, 0 = square */
short apj_shape_rects(struct xa_window *wind, GRECT *out);		/* rounded shape as rects; 0 = square */
short apj_corner_boxes(struct xa_window *wind, GRECT *out);		/* 4 corner bounding boxes; 0 = square */

struct build_rl_parms;
struct build_rl_parms
{
	int	(*getnxtrect)(struct build_rl_parms *p);
	GRECT	*area;
	GRECT	*next_r;

	void *ptr1;

	/* APJ-OS: occluder rects still to hand out before the next window */
	short	nshape, ishape;
	GRECT	shape[APJ_SHAPE_MAX];
};

// bool was_visible(struct xa_window *w);
bool xa_rc_intersect(const GRECT s, GRECT *d);
bool xa_rect_clip(const GRECT *s, const GRECT *d, GRECT *r);
int xa_rect_chk(const GRECT *s, const GRECT *d, GRECT *r);

//struct xa_rect_list *build_rect_list(struct build_rl_parms *p);
struct xa_rect_list *make_rect_list(struct xa_window *w, bool swap, short which);
int get_rect(struct xa_rectlist_entry *rle, GRECT *clip, bool first, GRECT *ret);
void free_rect_list(struct xa_rect_list *first);
void free_rectlist_entry(struct xa_rectlist_entry *rlent);

struct xa_rect_list *rect_get_optimal_first(struct xa_window *w);
struct xa_rect_list *rect_get_user_first(struct xa_window *w);
struct xa_rect_list *rect_get_optimal_next(struct xa_window *w);
struct xa_rect_list *rect_get_user_next(struct xa_window *w);
struct xa_rect_list *rect_get_system_first(struct xa_window *w);
struct xa_rect_list *rect_get_system_next(struct xa_window *w);
struct xa_rect_list *rect_get_toolbar_first(struct xa_window *w);
struct xa_rect_list *rect_get_toolbar_next(struct xa_window *w);

#endif /* _rectlist_h */
