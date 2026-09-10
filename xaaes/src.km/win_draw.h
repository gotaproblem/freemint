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

#ifndef _wind_draw_h
#define _wind_draw_h

//void init_widget_theme(struct widget_theme *wd);
void main_xa_theme(struct xa_module_widget_theme **xmt);
void free_widg_grad(const struct xa_module_api *_api);

/*
 * APJ-OS Fluent window chrome. The chrome is pure data - a struct
 * window_colours per window - so the Fluent look is a rewrite of that
 * data in place, not a second widget module. on = 1 flattens the set
 * onto the APJ theme pens (render_apj.h); on = 0 restores the stock set
 * for the window class. Texture pointers are never touched, so the
 * module's texture refcounts stay balanced either way.
 */

void apj_chrome_colours(void *wcols, short on, short ontop, short win_class);

/* Re-order a client's widget layout (struct widget_theme *) for Fluent:
 * gadgets on the right, closer last, no scrollbar arrows. */
void apj_chrome_layout(void *theme, short on);


#endif /* _wind_draw_h */
