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

#include "xa_types.h"
#include "xa_global.h"

#include "c_window.h"
#include "rectlist.h"
#include "widgets.h"
#include "win_draw.h"		/* apj_window_fluent */

#define max(x,y) (((x)>(y))?(x):(y))
#define min(x,y) (((x)<(y))?(x):(y))

bool inline
is_inside(const GRECT *r, const GRECT *o)
{
	if (   (r->g_x        < o->g_x       )
	    || (r->g_y        < o->g_y       )
	    || (r->g_x + r->g_w > o->g_x + o->g_w)
	    || (r->g_y + r->g_h > o->g_y + o->g_h)
	   )
		return false;

	return true;
}

static struct xa_rect_list *
build_rect_list(struct build_rl_parms *p)
{
	struct xa_rect_list *rl, *nrl, *rl_next, *rl_prev;
	GRECT r_ours, r_win;

	nrl = kmalloc(sizeof(*nrl));
	assert(nrl);
	nrl->next = NULL;
	nrl->r = *p->area;

	{
		short wx2, wy2, sx2, sy2;

		wx2 = nrl->r.g_x + nrl->r.g_w;
		wy2 = nrl->r.g_y + nrl->r.g_h;
		sx2 = screen.r.g_x + screen.r.g_w;
		sy2 = screen.r.g_y + screen.r.g_h;

		if (nrl->r.g_x < screen.r.g_x)
		{
			nrl->r.g_w -= screen.r.g_x - nrl->r.g_x;
			nrl->r.g_x = screen.r.g_x;
		}
		if (wx2 > sx2)
			nrl->r.g_w -= wx2 - sx2;

		if (nrl->r.g_y < screen.r.g_y)
		{
			nrl->r.g_h -= screen.r.g_y - nrl->r.g_y;
			nrl->r.g_y = screen.r.g_y;
		}
		if (wy2 > sy2)
			nrl->r.g_h -= wy2 - sy2;
	}

	/* APJ-OS: a rounded window's own list starts from its shape (top
	 * strips, body, bottom strips) clipped to the area, not from the area
	 * with the corners subtracted - subtracting cuts full-height columns
	 * and split the work area into several rects. p->shape holds the
	 * shape on entry; it is then free for nextwind_rect's occluders. */
	if (p->nshape > 0)
	{
		GRECT a = nrl->r;
		struct xa_rect_list *head = NULL, *tail = NULL;
		short i;

		for (i = 0; i < p->nshape; i++)
		{
			GRECT c;

			if (a.g_w <= 0 || a.g_h <= 0 || !xa_rect_clip(&a, &p->shape[i], &c))
				continue;
			if (!head)
				rl = nrl;			/* reuse the first node */
			else
			{
				rl = kmalloc(sizeof(*rl));
				assert(rl);
			}
			rl->next = NULL;
			rl->r = c;
			if (tail)
				tail->next = rl;
			else
				head = rl;
			tail = rl;
		}
		p->nshape = p->ishape = 0;

		if (!head)
		{
			kfree(nrl);
			return NULL;
		}
		nrl = head;
	}

	DIAGS(("build_rect_list: area=(%d/%d/%d/%d), nrl=(%d/%d/%d/%d)",
		p->area->g_x, p->area->g_y, p->area->g_w, p->area->g_h, nrl->r.g_x, nrl->r.g_y, nrl->r.g_w, nrl->r.g_h));

	if (nrl)
	{
		short flag, win_x2, win_y2, our_x2, our_y2;
		short w, h;

		while (p->getnxtrect(p))
		{
			r_win = *p->next_r;
			win_x2 = r_win.g_x + r_win.g_w;
			win_y2 = r_win.g_y + r_win.g_h;

			for (rl = nrl, rl_prev = NULL; rl; rl = rl_next)
			{
				r_ours = rl->r;

				flag = 0;

				h = r_win.g_y - r_ours.g_y;
				w = r_win.g_x - r_ours.g_x;

				rl_next = rl->next;

				if ( h < r_ours.g_h	&&
				     w < r_ours.g_w	&&
				     win_x2 > r_ours.g_x	&&
				     win_y2 > r_ours.g_y)
				{
					our_x2 = r_ours.g_x + r_ours.g_w;
					our_y2 = r_ours.g_y + r_ours.g_h;

					if (r_win.g_x > r_ours.g_x)
					{
						rl->r.g_x = r_ours.g_x;
						rl->r.g_y = r_ours.g_y;
						rl->r.g_h = r_ours.g_h;
						rl->r.g_w = w;

						r_ours.g_x += w;
						r_ours.g_w -= w;
						flag = 1;
					}
					if (r_win.g_y > r_ours.g_y)
					{
						if (flag)
						{
							rl_prev = rl;
							rl = kmalloc(sizeof(*rl));
							assert(rl);
							rl->next = rl_prev->next;
							rl_prev->next = rl;
						}
						rl->r.g_x = r_ours.g_x;
						rl->r.g_y = r_ours.g_y;
						rl->r.g_w = r_ours.g_w;
						rl->r.g_h = h;

						r_ours.g_y += h;
						r_ours.g_h -= h;

						flag = 1;
					}
					if (our_x2 > win_x2)
					{
						if (flag)
						{
							rl_prev = rl;
							rl = kmalloc(sizeof(*rl));
							assert(rl);
							rl->next = rl_prev->next;
							rl_prev->next = rl;
						}

						rl->r.g_x = win_x2;
						rl->r.g_y = r_ours.g_y;
						rl->r.g_w = our_x2 - win_x2;
						rl->r.g_h = r_ours.g_h;

						r_ours.g_w -= rl->r.g_w;
						flag = 1;
					}
					if (our_y2 > win_y2)
					{
						if (flag)
						{
							rl_prev = rl;
							rl = kmalloc(sizeof(*rl));
							assert(rl);
							rl->next = rl_prev->next;
							rl_prev->next = rl;
						}
						rl->r.g_x = r_ours.g_x;
						rl->r.g_y = win_y2;
						rl->r.g_w = r_ours.g_w;
						rl->r.g_h = our_y2 - win_y2;
						if( flag )
						{
#if 1
							/* a_avoid spltting workarea because of menu */
							if( cfg.menu_bar == 1 && menu_window && rl_prev->r.g_y < menu_window->r.g_h
								&& rl->r.g_x + rl->r.g_w == rl_prev->r.g_x && rl->r.g_y + rl->r.g_h == rl_prev->r.g_y + rl_prev->r.g_h )
							{
								rl->r.g_w += rl_prev->r.g_w;
								rl_prev->r.g_h = rl->r.g_y - rl_prev->r.g_y;
							}
#endif
						}

						r_ours.g_h -= rl->r.g_h;
						flag = 1;
					}
				}
				else
				{
					flag = 1;
				}

				if (!flag)
				{
					if (rl == nrl)
					{
						nrl = rl_next;
					}
					else if (nrl->next == rl)
						nrl->next = rl_next;

					if (rl_prev)
						rl_prev->next = rl_next;
					kfree(rl);
				}
				else
				{
					rl_prev = rl;
				}
			} /* for (rl = nrl; rl; rl = rl_next) */
		} /* while (wl) */
	} /* if (nrl && w->prev) */
	return nrl;
}

/* ------------------------------------------------------------------ */
/* APJ-OS Fluent rounded corners                                        */
/* ------------------------------------------------------------------ */

static short apj_cr_radius = -1;		/* radius the table was built for */
static short apj_cr_n = 0;			/* rows with a non-zero inset */
static short apj_cr_inset[16];

static short
apj_isqrt_round(long v)
{
	long s = 0;

	while ((s + 1) * (s + 1) <= v)
		s++;
	if (v - s * s > s)		/* nearer the next integer */
		s++;
	return (short) s;
}

/* the corner radius for this window, 0 = square */
static short
apj_corner_radius(struct xa_window *w)
{
	short r;

	if (!w || w == root_window || w->frame < 0 || !(w->active_widgets & NAME)
	    || (w->dial & created_for_POPUP) || !apj_window_fluent(w))
		return 0;

	r = screen.r.g_h >= 1000 ? 8 : screen.r.g_h >= 700 ? 6 : 4;

	if (w->r.g_w < 4 * r || w->r.g_h < 2 * r + 2)
		return 0;

	/* maximised: square, as Windows does */
	if (w->r.g_x <= root_window->wa.g_x && w->r.g_y <= root_window->wa.g_y
	    && w->r.g_x + w->r.g_w >= root_window->wa.g_x + root_window->wa.g_w
	    && w->r.g_y + w->r.g_h >= root_window->wa.g_y + root_window->wa.g_h)
		return 0;

	return r;
}

/*
 * Inset of each carved row of a quarter circle of radius r, row 0 at the
 * outer edge: r - sqrt(r^2 - (r - k - 1/2)^2), in half-pixel integer
 * arithmetic. r = 8 gives 5 3 2 1 1.
 */
short
apj_corner_steps(struct xa_window *wind, const short **inset)
{
	short r = apj_corner_radius(wind), k;

	if (!r)
		return 0;
	if (r != apj_cr_radius)
	{
		apj_cr_radius = r;
		apj_cr_n = 0;
		for (k = 0; k < r && k < 16; k++)
		{
			long m = 2L * r - 2L * k - 1;
			short dx = apj_isqrt_round(4L * r * r - m * m);
			short in = (short) ((2 * r - dx) / 2);

			if (in <= 0)
				break;
			apj_cr_inset[k] = in;
			apj_cr_n = k + 1;
		}
	}
	if (inset)
		*inset = apj_cr_inset;
	return apj_cr_n;
}

/*
 * Carved rows at the top and bottom of a rounded window. Never into the
 * work area: a carved row splits the window's rectangle list, and a
 * program redraws its work area once per rectangle - three passes for a
 * window without a bottom scrollbar (GEMBench, TosWin2) instead of one.
 * The title bar always covers the top curve; the bottom gets the full
 * curve over a scrollbar or info row, else just the border rows.
 */
short
apj_corner_rows(struct xa_window *wind, const short **inset, short *nt, short *nb)
{
	short n = apj_corner_steps(wind, inset), room;

	*nt = *nb = 0;
	if (!n)
		return 0;

	room = wind->wa.g_y - wind->r.g_y;
	*nt = room < n ? (room > 0 ? room : 0) : n;
	room = (wind->r.g_y + wind->r.g_h) - (wind->wa.g_y + wind->wa.g_h);
	*nb = room < n ? (room > 0 ? room : 0) : n;
	return (*nt || *nb) ? n : 0;
}

/* merged runs of equal inset over rows [0, cnt) */
static short
apj_row_runs(const short *in, short cnt, short *start, short *len)
{
	short k = 0, c = 0;

	while (k < cnt)
	{
		short j = k;

		while (j + 1 < cnt && in[j + 1] == in[k])
			j++;
		start[c] = k;
		len[c] = j - k + 1;
		c++;
		k = j + 1;
	}
	return c;
}

/* The rounded shape as rects: merged top strips, body, bottom strips */
short
apj_shape_rects(struct xa_window *wind, GRECT *out)
{
	const short *in;
	short nt, nb, st[16], ln[16], runs, i, c = 0;
	GRECT r = wind->r;

	if (!apj_corner_rows(wind, &in, &nt, &nb))
		return 0;

	runs = apj_row_runs(in, nt, st, ln);
	for (i = 0; i < runs; i++)
	{
		out[c].g_x = r.g_x + in[st[i]];
		out[c].g_w = r.g_w - 2 * in[st[i]];
		out[c].g_y = r.g_y + st[i];
		out[c].g_h = ln[i];
		c++;
	}
	runs = apj_row_runs(in, nb, st, ln);
	for (i = 0; i < runs; i++)
	{
		out[c].g_x = r.g_x + in[st[i]];
		out[c].g_w = r.g_w - 2 * in[st[i]];
		out[c].g_y = r.g_y + r.g_h - st[i] - ln[i];
		out[c].g_h = ln[i];
		c++;
	}
	out[c].g_x = r.g_x;
	out[c].g_y = r.g_y + nt;
	out[c].g_w = r.g_w;
	out[c].g_h = r.g_h - nt - nb;
	c++;
	return c;
}

/* The corner areas - what a move must hand back to the windows beneath */
short
apj_corner_boxes(struct xa_window *wind, GRECT *out)
{
	const short *in;
	short nt, nb, w, c = 0;
	GRECT r = wind->r;

	if (!apj_corner_rows(wind, &in, &nt, &nb))
		return 0;
	w = in[0];
	if (nt)
	{
		out[c].g_x = r.g_x;             out[c].g_y = r.g_y; out[c].g_w = w; out[c].g_h = nt; c++;
		out[c].g_x = r.g_x + r.g_w - w; out[c].g_y = r.g_y; out[c].g_w = w; out[c].g_h = nt; c++;
	}
	if (nb)
	{
		out[c].g_x = r.g_x;             out[c].g_y = r.g_y + r.g_h - nb; out[c].g_w = w; out[c].g_h = nb; c++;
		out[c].g_x = r.g_x + r.g_w - w; out[c].g_y = r.g_y + r.g_h - nb; out[c].g_w = w; out[c].g_h = nb; c++;
	}
	return c;
}

static int
nextwind_rect(struct build_rl_parms *p)
{
	int ret = 0;
	struct xa_window *wind = p->ptr1;

	/* APJ-OS: the rest of a rounded occluder, or our own corner steps */
	if (p->ishape < p->nshape)
	{
		p->next_r = &p->shape[p->ishape++];
		return 1;
	}


	while (wind && !ret)
	{
		/*
		 * Lets skip windows whose owner is exiting or is hidden
		 */
		if (!(wind->owner->status & CS_EXITING) && !(wind->active_widgets & STORE_BACK) &&
		     (wind->window_status & (XAWS_HIDDEN|XAWS_OPEN)) == XAWS_OPEN)
		{
			short ns = apj_shape_rects(wind, p->shape);

			if (ns > 0)
			{
				p->nshape = ns;
				p->ishape = 1;
				p->next_r = &p->shape[0];
			}
			else
				p->next_r = &wind->r;
			//wind = wind->prev;
			ret = 1;
			//break;
		}
		if (!wind->prev && !wind->nolist)
			wind = S.open_nlwindows.last;
		else
			wind = wind->prev;
	}
	p->ptr1 = wind;
	return ret;
}

struct xa_rect_list *
make_rect_list(struct xa_window *wind, bool swap, short which)
{
	struct xa_rect_list *nrl = NULL;
	struct xa_rectlist_entry *rle;
	struct build_rl_parms p;
	GRECT area;

	if ((wind->owner->status & CS_EXITING))
		return NULL;

	DIAGS(("Freeing old rect_list for %d", wind->handle));
	switch (which)
	{
		case RECT_SYS:
		{
			if (swap)
			{
				free_rectlist_entry(&wind->rect_list);
				free_rectlist_entry(&wind->rect_user);
				free_rectlist_entry(&wind->rect_opt);
			}
			area = wind->r;
			rle = &wind->rect_list;
			break;
		}
		case RECT_OPT:
		{
			if (swap)
				free_rectlist_entry(&wind->rect_opt);
			area = wind->rl_clip;
			rle = &wind->rect_opt;
			break;
		}
		case RECT_TOOLBAR:
		{
			if (swap)
				free_rectlist_entry(&wind->rect_toolbar);
			if (!usertoolbar_installed(wind) || !xa_rect_clip(&wind->r, &wind->widgets[XAW_TOOLBAR].ar, &area))
				return NULL;
			rle = &wind->rect_toolbar;
			break;
		}
		default:;
			return NULL;
	}

	DIAGS(("make_rect_list for wind %d", wind->handle));

	if ((wind->window_status & (XAWS_HIDDEN|XAWS_BELOWROOT)) ||
	    area.g_x > (screen.r.g_x + screen.r.g_w) ||
	    area.g_y > (screen.r.g_y + screen.r.g_h) ||
	   (area.g_x + area.g_w) < screen.r.g_x  ||
	   (area.g_y + area.g_h) < screen.r.g_y )
	{
		DIAGS(("make_rect_list: window is outside screen"));
		return NULL;
	}

	p.getnxtrect = nextwind_rect;
	p.area = &area;
	/* APJ-OS: a rounded window's own list is built from its shape - the
	 * corner steps are not its to draw (see build_rect_list) */
	p.nshape = apj_shape_rects(wind, p.shape);
	p.ishape = 0;
	if (!wind->prev && !wind->nolist)
		p.ptr1 = S.open_nlwindows.last;
	else
		p.ptr1 = wind->prev;

	nrl = build_rect_list(&p);
	if (swap)
		rle->start = rle->next = nrl;

	return nrl;
}

static struct xa_rect_list *
get_rect_first(struct xa_rectlist_entry *rle)
{
	rle->next = rle->start;
	return rle->next;
}
static struct xa_rect_list *
get_rect_next(struct xa_rectlist_entry *rle)
{
	if (rle->next)
		rle->next = rle->next->next;
	return rle->next;
}

/*
 * return 1 if found else 0
 */
int
get_rect(struct xa_rectlist_entry *rle, GRECT *clip, bool first, GRECT *ret)
{
	struct xa_rect_list *rl;
	int rtn = 0;
	GRECT r;

	if (first)
		rl = get_rect_first(rle);
	else
		rl = get_rect_next(rle);

	if (clip)
	{
		while (rl)
		{
			if (xa_rect_clip(&rl->r, clip, &r))
			{
				*ret = r;
				rtn = 1;
				break;
			}
			rl = get_rect_next(rle);
		}
	}
	else if (rl)
	{
		*ret = rl->r;
		rtn = 1;
	}

	return rtn;
}

#if INCLUDE_UNUSED
void
free_rect_list(struct xa_rect_list *first)
{
	struct xa_rect_list *next;

	DIAGS(("free_rect_list: start=%lx", first));
	while (first)
	{
		next = first->next;
		kfree(first);
		first = next;
	}
}
#endif
void
free_rectlist_entry(struct xa_rectlist_entry *rle)
{
	struct xa_rect_list *rl;

	while ((rl = rle->start))
	{
		rle->start = rl->next;
		kfree(rl);
	}
	rle->next = NULL;
}

#if INCLUDE_UNUSED
struct xa_rect_list *
rect_get_user_first(struct xa_window *w)
{
	w->rect_user.next = w->rect_user.start;
	return w->rect_user.next;
}
#endif
struct xa_rect_list *
rect_get_optimal_first(struct xa_window *w)
{
	w->rect_opt.next = w->rect_opt.start;
	return w->rect_opt.next;
}

#if INCLUDE_UNUSED
struct xa_rect_list *
rect_get_system_first(struct xa_window *w)
{
	w->rect_list.next = w->rect_list.start;
	return w->rect_list.next;
}
#endif
struct xa_rect_list *
rect_get_optimal_next(struct xa_window *w)
{
	if (w->rect_opt.next)
		w->rect_opt.next = w->rect_opt.next->next;
	return w->rect_opt.next;
}

#if INCLUDE_UNUSED
struct xa_rect_list *
rect_get_user_next(struct xa_window *w)
{
	if (w->rect_user.next)
		w->rect_user.next = w->rect_user.next->next;
	return w->rect_user.next;
}
#endif
#if INCLUDE_UNUSED
struct xa_rect_list *
rect_get_system_next(struct xa_window *w)
{
	if (w->rect_list.next)
		w->rect_list.next = w->rect_list.next->next;
	return w->rect_list.next;
}
#endif
struct xa_rect_list *
rect_get_toolbar_first(struct xa_window *w)
{
	w->rect_toolbar.next = w->rect_toolbar.start;
	return w->rect_toolbar.next;
}
struct xa_rect_list *
rect_get_toolbar_next(struct xa_window *w)
{
	if (w->rect_toolbar.next)
		w->rect_toolbar.next = w->rect_toolbar.next->next;
	return w->rect_toolbar.next;
}

/*
 * Compute intersection of two rectangles; put result rectangle
 * into *d; return true if intersection is nonzero.
 *
 * (Original version of this function taken from Digital Research's
 * GEM sample application `DEMO' [aka `DOODLE'],  Version 1.1,
 * March 22, 1985)
 */
bool
xa_rc_intersect(const GRECT s, GRECT *d)
{
	if (s.g_w > 0 && s.g_h > 0 && d->g_w > 0 && d->g_h > 0)
	{
		const short w1 = s.g_x + s.g_w;
		const short w2 = d->g_x + d->g_w;
		const short h1 = s.g_y + s.g_h;
		const short h2 = d->g_y + d->g_h;

		d->g_x = max(s.g_x, d->g_x);
		d->g_y = max(s.g_y, d->g_y);
		d->g_w = min(w1, w2) - d->g_x;
		d->g_h = min(h1, h2) - d->g_y;

		return (d->g_w > 0) && (d->g_h > 0);
	}
	else
		return false;
}
/* Ozk:
 * This is my (ozk) version of the xa_rc_intersect.
 * Takes pointers to source, destination and result
 * rectangle structures.
 */
bool
xa_rect_clip(const GRECT *s, const GRECT *d, GRECT *r)
{
	if (s->g_w > 0 && s->g_h > 0 && d->g_w > 0 && d->g_h > 0)
	{
		const short w1 = s->g_x + s->g_w;
		const short w2 = d->g_x + d->g_w;
		const short h1 = s->g_y + s->g_h;
		const short h2 = d->g_y + d->g_h;

		r->g_x = s->g_x > d->g_x ? s->g_x : d->g_x;	//max(s->x, d->g_x);
		r->g_y = s->g_y > d->g_y ? s->g_y : d->g_y;	//max(s->y, d->g_y);
		r->g_w = (w1 < w2 ? w1 : w2) - r->g_x; 	//min(w1, w2) - d->g_x;
		r->g_h = (h1 < h2 ? h1 : h2) - r->g_y;	//min(h1, h2) - d->g_y;

		return ((r->g_w > 0) && (r->g_h > 0));
	}
	else
		return false;
}
/*
 * return
 *
 * 0	s or d has w=0 or h=0 or no intersection
 * 1	d not inside s
 * 2	d inside s
 *
 */
int
xa_rect_chk(const GRECT *s, const GRECT *d, GRECT *r)
{
	int ret = 0;

	if (s->g_w > 0 && s->g_h > 0 && d->g_w > 0 && d->g_h > 0)
	{
		const short sw = s->g_x + s->g_w;
		const short dw = d->g_x + d->g_w;
		const short sh = s->g_y + s->g_h;
		const short dh = d->g_y + d->g_h;

		r->g_x = s->g_x < d->g_x ? d->g_x : s->g_x;
		r->g_y = s->g_y < d->g_y ? d->g_y : s->g_y;
		r->g_w = (sw < dw ? sw : dw) - r->g_x;
		r->g_h = (sh < dh ? sh : dh) - r->g_y;

		if (r->g_x == d->g_x && r->g_y == d->g_y && r->g_w == d->g_w && r->g_h == d->g_h)
			ret = 2;
		else if ((r->g_w > 0) && (r->g_h > 0))
			ret = 1;
	}
	return ret;
}
