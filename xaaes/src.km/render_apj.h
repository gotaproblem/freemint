/*
 * render_apj.h - APJ-OS object renderer for XaAES
 *
 * Part of the APJ-OS Fluent GUI work. See render_apj.c.
 *
 * XaAES is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _render_apj_h_
#define _render_apj_h_

void main_object_render_apj(struct xa_module_object_render **);

/*
 * Point a client at the APJ renderer instead of the stock one.
 * Must be called before init_client_objcrend() opens the client's api.
 */
long client_use_apj_render(struct xa_client *client);

/*
 * The APJ theme: one RGB per role, pushed by the desktop through
 * appl_control opcode 111 as (role << 24) | 0xRRGGBB, cleared by 112.
 *
 * The role order MUST match BT_R_* in the Bespoke Desktop's btheme.h -
 * that is the contract between the two trees.
 *
 * Each role is loaded into a VDI colour register on XaAES's own
 * workstation at APJ_PEN_BASE + role. Truecolour palettes are per
 * workstation, and the stock renderer only ever names pens 0-15, so the
 * block is ours; the desktop uses the same 237..255 block on its own
 * workstation for the same roles, so in an 8-bit mode (global palette)
 * both write identical values and nothing fights.
 */

enum
{
	APJ_R_FACE, APJ_R_TEXT, APJ_R_LIGHT, APJ_R_DARK, APJ_R_SELBG, APJ_R_SELFG,
	APJ_R_ALBG, APJ_R_ALFG, APJ_R_PANEL, APJ_R_TITBG, APJ_R_TITFG, APJ_R_PAPER,
	APJ_R_BORDER, APJ_R_HOVER, APJ_R_PRESSED, APJ_R_FOCUS, APJ_R_DISABLED,
	APJ_R_ELEVATION, APJ_R_ACCENT,
	APJ_R_N
};

#define APJ_PEN_BASE	(256 - APJ_R_N)		/* 237..255 */
#define APJ_PEN(role)	((short) (APJ_PEN_BASE + (role)))

short apj_theme_set(long val);		/* opcode 111: (role<<24)|RGB; 1 ok, 0 bad */
short apj_theme_reset(void);		/* opcode 112: back to the stock look */

#endif /* _render_apj_h_ */
