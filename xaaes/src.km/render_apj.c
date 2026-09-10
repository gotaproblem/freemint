/*
 * render_apj.c - APJ-OS object renderer for XaAES
 *
 * Forked from render_obj.c on branch apj-os-fluent. At this commit it is a
 * behavioural copy of the stock renderer: same drawers, same output. It exists
 * so that the Fluent work has somewhere to diverge without touching the path
 * legacy GEM apps draw through.
 *
 * XaAES supports one render module per client (xa_client.objcr_module, set up
 * by init_client_objcrend() in draw_obj.c), so both modules can be resident and
 * clients can be pointed at either one.
 *
 * Five services stay in render_obj.c rather than being duplicated here, because
 * other translation units call them and they carry global state:
 *   load_gradients(), is_gradient_installed(), do_bkg_img(),
 *   make_bkg_img_path(), and the imgpath/imgpath_file wallpaper path.
 * Consequence for now: the gradient table in this file keeps its compiled-in
 * defaults and is not reloaded from the GRADIENTS data files. That is deliberate
 * - the Fluent renderer is intended to take RGB directly rather than inherit the
 * indexed-pen gradient sets.
 */

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
#include "render_obj.h"
#include "render_apj.h"
#include "apj_atlas.h"
#include "global.h"
#include "mint/stat.h"
#include "xa_xtobj.h"
#undef NUM_STRINGS
#undef NUM_IMAGES
#undef NUM_UD
#undef NUM_BB
#undef NUM_IB
#undef NUM_CIB
#undef NUM_TI
#undef NUM_OBS
#undef NUM_TREE
#undef NUM_FRSTR
#undef NUM_FRIMG
#include "mscall.h"
#include "gradients.h"

#define RASTER_HDL	v->handle

#define MONO (screen->colours < 16)
#define done(x) (*wt->state_mask &= ~(x))
#ifndef max
#define max(x,y) (((x)>(y))?(x):(y))
#endif
#ifndef min
#define min(x,y) (((x)<(y))?(x):(y))
#endif

#define N0NE 0
#define IND 1
#define BKG 2
#define ACT 3

#define DRAW_BKG 	1
#define DRAW_BOX	(1<<1)
#define CALC_BOX	(1<<2)
#define DRAW_3D		(1<<2)
#define CALC_3D		(1<<4)
#define DRAW_TEXTURE	(1<<5)
#define ONLY_TEXTURE	(1<<6)
#define DRAW_ALL	(DRAW_BKG|DRAW_BOX|DRAW_3D)
#define REV_3D		(1<<7)
#define ANCH_PARENT	(1<<8)
#define CALC_ONLY	(1<<15)


static short _cdecl obj_thickness(struct widget_tree *wt, OBJECT *ob);
static int apj_gtext(struct xa_vdi_settings *v, short x, short y, short fg, const char *t);	/* AA text, defined below */

/* APJ-OS theme state - used by drawers throughout, defined early */
static struct rgb_1000 apj_rgb[APJ_R_N];
static short apj_active = 0;

/* AA text state (see apj_gtext below) */
static char *apj_tbuf = NULL;		/* read-back / blend buffer */
static long  apj_tbuf_size = 0;
static short apj_fgpen_cached = -1;	/* last foreground pen turned into pixel bytes */
static unsigned char apj_fgpx[4];
static void _cdecl obj_offsets(struct widget_tree *wt, OBJECT *ob, GRECT *c);

static bool use_gradients = true;
#if WITH_BKG_IMG || WITH_GRADIENTS
#endif
struct texture
{
	struct xa_data_hdr h;
	struct xa_wtexture t;
	XAMFDB xamfdb;
};
struct bcol
{
	short flags;
	short wr_mode;
	short c;
	short i;
	short f;
	short box_c;
	short box_th;
	short top;
	short bottom;
	short left;
	short right;
	struct xa_wtexture *texture;
#if WITH_GRADIENTS
	struct xa_gradient *gradient;
#endif
};

struct color_theme
{
	struct bcol col;
	struct xa_fnt_info fnt;
};

struct ob_theme
{
	struct color_theme n[4];	/* Normal */
	struct color_theme s[4];	/* Selected */
	struct color_theme h[4];	/* Highlighted */
	short thick_3d;
	short thick_3dact;
	short thick_def;
};

struct object_theme
{
	struct ob_theme norm;	/* Normal */
	struct ob_theme dis;	/* disabled */
};

struct theme
{
	struct xa_data_hdr h;

#ifdef REND_NAES3D
#define NAES3D			1
	unsigned long rflags;
#endif
	short	shadow_col;

	short	ad3dval_x;
	short	ad3dval_y;

	struct bcol outline;

	struct object_theme button;

	struct object_theme string;
	struct object_theme boxtext;
	struct object_theme ed_boxtext;
	struct object_theme box;
	struct object_theme text;
	struct object_theme ed_text;
	struct object_theme title;

	struct object_theme groupframe;
	struct object_theme extobj;

	struct object_theme pu_string;
	struct object_theme popupbkg;
	struct object_theme menubar;

	short end_obt[0];
};
#if WITH_GRADIENTS
static struct xa_gradient menu_gradient =
{
	NULL,
	 0, -1,
	16,  0,

	0, 0, {0},
	{{900,900,900},{600,600,600}},
};
static struct xa_gradient sel_title_gradient =
{
	NULL,
	0, -1,
	16, 0,
	0,0,{0},
	{{600,600,600},{950,950,950}},
};
static struct xa_gradient sel_popent_gradient =
{
	NULL,
	0, -1,
	16, 0,
	0,0,{0},
	{{950,950,1000},{800,800,1000}},
};

static struct xa_gradient indbutt_gradient =
{
	NULL,
	-1,   0,
	 0,  16,

	4, 1, { -35, 0, },
	{{700,700,700},{900,900,900},{700,700,700}},
};
static struct xa_gradient sel_indbutt_gradient =
{
	NULL,
	-1,   0,
	 0,  16,

	4, 1, { -35, 0, },
	{{700,700,700},{500,500,500},{700,700,700}},
};
static struct xa_gradient actbutt_gradient =
{
	NULL,
	0, -1,
	16, 0,

	0, 0, { 0 },
	{{900,900,900},{700,700,700}},
};

static struct xa_gradient popbkg_gradient =
{
	NULL,
	0, -1,
	16, 0,

	3, 1, {-40, 0},
	{{800,800,800}, {900,900,900}, {700,700,700}},
};
static struct xa_gradient box_gradient =
{
	NULL,
	-1,   0,
	 0,  16,

	4, -1, { -35, 0, },	/* n_steps < 0: disabled */
	{{700,700,700},{500,500,500},{700,700,700}},
};
static struct xa_gradient box_gradient2 =
{
	NULL,
	-1,   0,
	 0,  16,

	4, -1, { -35, 0, },	/* n_steps < 0: disabled */
	{{700,700,700},{500,500,500},{700,700,700}},
};
static struct xa_gradient text_gradient =
{
	NULL,
	-1,   0,
	 0,  16,

	4, -1, { -35, 0, },	/* n_steps < 0: disabled */
	{{800,800,800},{600,600,600},{600,600,600}},
};

/* the gradients[] dispatch table stays in render_obj.c: it exists to be filled
 * by load_gradients() from the GRADIENTS data files, and this renderer is
 * heading for direct RGB rather than the indexed gradient sets. */
#endif

static struct theme stdtheme =
{
	{ 0 },

#ifdef REND_NAES3D
	0,
#endif
	G_BLACK,
	2, 2,

/* ------------------------------------------------------------- */
/* -------------------- outline ------------------------ */
/* ------------------------------------------------------------- */
	{ WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1, G_WHITE, G_LBLACK, G_WHITE, G_LBLACK XA_GRADIENT(NULL) },
/* ------------------------------------------------------------- */
/* -------------------- Button - normal ------------------------ */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				/* flags   wr_mode   color   interior  fill frame_c frame_th top     bottom     left     right texture */
		/* no */	{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_BLACK, G_BLACK, G_BLACK, NULL XA_GRADIENT(NULL)},
                               /* id  pnts flags wr_mode  efx  fg       bg       banner  x_3dact y_3dact texture */
				 { 0, 0,   0,   MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0,      0,      NULL }},
		/* ind */	{{   WCOL_DRAWBKG|WCOL_BOXED|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&indbutt_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL  }},
		/* bkg */	{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
		/* act */	{{   WCOL_DRAWBKG|WCOL_BOXED|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&actbutt_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL }},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_BLACK, G_BLACK, G_BLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL }},
				{{   WCOL_DRAWBKG|WCOL_BOXED|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL XA_GRADIENT(&sel_indbutt_gradient)},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK,G_BLACK, 0, 0, NULL }},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_LWHITE, G_LBLACK,G_BLACK, 0, 0, NULL }},
				{{   WCOL_DRAWBKG|WCOL_BOXED|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&actbutt_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_WHITE, G_BLACK, 1, 1, NULL }},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,   G_BLACK, G_BLACK, G_BLACK, G_BLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL  }},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 0, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL)},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   -1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL)},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_PATTERN, 4, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_WHITE, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_WHITE, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_WHITE, G_LBLACK, G_WHITE, 1, 1, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 0, 2
		},
	},
/* ------------------------------------------------------------- */
/* ------------------------------------------------------------- */
/* -------------------- string -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- boxtext -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- editable boxtext -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE,  FIS_SOLID, 0, G_BLACK, 1, G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK, 1, G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK, 1, G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK, 1, G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- box -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&box_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- text -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{ WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{ WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, FAINT, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, FAINT, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&text_gradient) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- editable text -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{ WCOL_DRAWBKG|WCOL_DRAW3D|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_LBLACK, 1, G_LWHITE, G_WHITE, G_WHITE, G_LWHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{ WCOL_DRAWBKG,                        MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,  1, G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_DRAW3D|WCOL_BOXED|WCOL_REV3D|WCOL_BOXBF3D, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_LWHITE,   1, G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_LBLUE, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1, G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, FAINT, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG|WCOL_DRAW3D|WCOL_BOXED|WCOL_BOXBF3D|WCOL_REV3D, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_LBLACK,   1,    G_BLACK, G_LWHITE, G_BLACK, G_LWHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, FAINT, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- title -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&sel_title_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&sel_title_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&sel_title_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&sel_title_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 1, 1, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 1, 1, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 1, 1, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 1, 1, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 1, 1, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 1, 1, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 1, 1, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 1, 1, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- Group frame -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- extended objects ----------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- Popup strings ----------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&sel_popent_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&sel_popent_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&sel_popent_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&sel_popent_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* --------------- Popup background ---------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&popbkg_gradient) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* --------------- Menu Bar ---------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&menu_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&menu_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&menu_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_GRADIENT, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(&menu_gradient) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				 { 0, 0, 0, MD_TRANS, FAINT, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LBLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL XA_GRADIENT(NULL) },
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
};

static struct theme mono_stdtheme =
{
	{ 0 },

#ifdef REND_NAES3D
	0,
#endif
	G_BLACK,
	2, 2,

/* ------------------------------------------------------------- */
/* -------------------- outline ------------------------ */
/* ------------------------------------------------------------- */
	{ WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1, G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},

/* ------------------------------------------------------------- */
/* -------------------- Button - normal ------------------------ */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				/* flags                          wr_mode     color   interior  fill frame_c frame_th top     bottom     left     right texture */
		/* no */	{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_BLACK, G_BLACK, G_BLACK, NULL},
				 { 0, 0,   0,   MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0,      0,      NULL }},
		/* ind */	{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL  }},
		/* bkg */	{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
		/* act */	{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL }},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_BLACK, G_BLACK, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL }},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK,G_BLACK, 0, 0, NULL }},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK,G_BLACK, 0, 0, NULL }},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_LBLACK, G_WHITE, G_LBLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_WHITE, G_BLACK, 1, 1, NULL }},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,   G_BLACK, G_BLACK, G_BLACK, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL  }},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 0, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   -1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   -1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG|WCOL_BOXED, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 0, 2
		},
	},
/* ------------------------------------------------------------- */
/* ------------------------------------------------------------- */
/* -------------------- string -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- boxtext -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- editable boxtext -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- box -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- text -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{ WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{ WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{ WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{ WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- editable text -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{ WCOL_DRAWBKG|WCOL_DRAW3D|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_LBLACK,   1, G_WHITE, G_BLACK, G_WHITE, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{ WCOL_DRAWBKG|WCOL_DRAW3D|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_LBLACK,   1, G_WHITE, G_BLACK, G_WHITE, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{ WCOL_DRAWBKG|WCOL_DRAW3D|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_LBLACK,   1, G_WHITE, G_BLACK, G_WHITE, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{ WCOL_DRAWBKG|WCOL_DRAW3D|WCOL_BOXED, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_LBLACK,   1, G_WHITE, G_BLACK, G_WHITE, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{  WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				 { 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- title -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_BLACK, 1, 1, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_BLACK, 1, 1, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_BLACK, 1, 1, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_BLACK, 1, 1, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 1, 1, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 1, 1, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 1, 1, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 1, 1, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- Group frame -------------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- extended objects ----------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* -------------------- Popup strings ----------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LWHITE, G_BLACK, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* --------------- Popup background ---------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_LBLACK, G_BLACK, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
/* ------------------------------------------------------------- */
/* --------------- Menu Bar ---------------------------- */
/* ------------------------------------------------------------- */
	{
		{ /* normal */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_BLACK, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_BLACK, G_WHITE, G_BLACK, G_WHITE, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_WHITE, G_BLACK, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, 0, MD_TRANS, 0, G_BLACK, G_LBLACK, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
		{ /* disabled */
	/* normal */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_WHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_BLACK, G_WHITE, G_WHITE, 0, 0, NULL}},
			},
	/* selected */	{
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
				{{   WCOL_DRAWBKG, MD_REPLACE, G_BLACK, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_BLACK, G_WHITE, G_BLACK, NULL},
				 { 0, 0, 0, MD_TRANS, FAINT, G_WHITE, G_WHITE, G_BLACK, 0, 0, NULL}},
			},
	/* highlight */	{
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
				{{   0, MD_REPLACE, G_LWHITE, FIS_SOLID, 0, G_BLACK,   1,    G_WHITE, G_LBLACK, G_WHITE, G_LBLACK, NULL},
				{ 0, 0, WTXT_DRAW3D, MD_TRANS, 0, G_LBLACK, G_LWHITE, G_WHITE, 0, 0, NULL}},
			},
			2, 2, 2
		},
	},
};

static char xobj_name[] = "xa_xtobj.rsc";
static void *xobj_rshdr = NULL;
static void *xobj_rsc = NULL;
static struct object_render_api *current_render_api = NULL;
static struct theme *current_theme = NULL;
static struct xa_data_hdr *allocs;
static struct xa_data_hdr *pmaps;
static const struct xa_module_api *api = NULL;
static const struct xa_screen *screen = NULL;

#if SELECT_COLOR
static const short selected_colour[]   = {1, 0,13,15,14,10,12,11, 8, 9, 5, 7, 6, 2, 4, 3};

#endif
static const short selected3D_colour[] = {1, 0,13,15,14,10,12,11, 9, 8, 5, 7, 6, 2, 4, 3};

/*
 * APJ-OS: resource colour words name the standard 16 pens. Under a theme
 * the four greys - the ones every dialog's boxes, sliders and frames are
 * built from - are read as roles, so a themed client's dialogs follow the
 * theme without touching the resource; real colours pass through. Filled
 * in from the pens when a theme is loaded (apj_rsc_table()).
 */
static short apj_rsc_colour[16];
static short apj_rsc_sel[16];
static short apj_rsc_ready = 0;

static const short *
apj_rsc_table(int selected)
{
	if (!apj_rsc_ready)
	{
		int i;

		for (i = 0; i < 16; i++)
			apj_rsc_colour[i] = i;
		apj_rsc_colour[G_WHITE]  = APJ_PEN(APJ_R_PAPER);
		apj_rsc_colour[G_BLACK]  = APJ_PEN(APJ_R_TEXT);
		apj_rsc_colour[G_LWHITE] = APJ_PEN(APJ_R_PANEL);
		apj_rsc_colour[G_LBLACK] = APJ_PEN(APJ_R_DISABLED);
		for (i = 0; i < 16; i++)
			apj_rsc_sel[i] = apj_rsc_colour[selected3D_colour[i]];
		apj_rsc_ready = 1;
	}
	return selected ? apj_rsc_sel : apj_rsc_colour;
}
static const short efx3d_colour[] =      {8, 9,10,11,12,13,14,15, 0, 1, 2, 3, 4, 5, 6, 7};

/* ************************************************************ */
/*        Local helper functions				*/
/* ************************************************************ */

static void
chiseled_gbox(struct xa_vdi_settings *v, short d, short litecol, short shadowcol, const GRECT *r, const GRECT *t)
{
	GRECT rr;

	if (t)
	{
		(*v->api->wr_mode)(v, MD_REPLACE);
		if (shadowcol != -1)
		{
			(*v->api->left_line)(v, d, r, shadowcol);
			(*v->api->left_line)(v, d - 1, r, litecol);
			rr.g_x = r->g_x;
			rr.g_y = r->g_y;
			rr.g_w = t->g_x - r->g_x - 1;
			rr.g_h = r->g_h;
			(*v->api->top_line)(v, d,     &rr, shadowcol);
			(*v->api->top_line)(v, d - 1, &rr, litecol);
			rr.g_x = t->g_x + t->g_w;
			rr.g_w = r->g_x + r->g_w - rr.g_x;
			if (rr.g_w > 0)
			{
				(*v->api->top_line)(v, d,     &rr, shadowcol);
				(*v->api->top_line)(v, d - 1, &rr, litecol);
			}
			else
			{
				rr.g_y += t->g_h;
				rr.g_h -= t->g_h;
			}
			if (rr.g_h > 0)
			{
				rr.g_y += 1, rr.g_h -= 1;
				(*v->api->right_line)(v, d, &rr, litecol);
				(*v->api->right_line)(v, d - 1, &rr, shadowcol);
				rr = *r;
				rr.g_x += 1, rr.g_w -= 1;
				(*v->api->bottom_line)(v, d, &rr, litecol);
				(*v->api->bottom_line)(v, d-1, &rr, shadowcol);
			}
		}
		else
		{
			(*v->api->left_line)(v, d, r, litecol);
			rr.g_x = r->g_x;
			rr.g_y = r->g_y;
			rr.g_w = t->g_x - r->g_x;
			rr.g_h = r->g_h;
			(*v->api->top_line)(v, d, &rr, litecol);
			rr.g_x = t->g_x + t->g_w;
			rr.g_w = r->g_x + r->g_w - rr.g_x;
			if (rr.g_w > 0)
			{
				(*v->api->top_line)(v, d, &rr, litecol);
			}
			else
			{
				rr.g_y += t->g_h;
				rr.g_h -= t->g_h;
			}
			if (rr.g_h > 0)
			{
				(*v->api->right_line)(v, d, &rr, litecol);
				(*v->api->bottom_line)(v, d, r, litecol);
			}
		}
	}
	else
	{
		if (shadowcol != -1)
		{
			(*v->api->br_hook)(v, d,     r, litecol);
			(*v->api->tl_hook)(v, d,     r, shadowcol);
			(*v->api->br_hook)(v, d - 1, r, shadowcol);
			(*v->api->tl_hook)(v, d - 1, r, litecol);
		}
		else
		{
			(*v->api->l_color)(v, litecol);
			(*v->api->gbox)(v, d, r);
		}
	}
}

static void
d3_bottom_line(struct xa_vdi_settings *v, GRECT *r, bool l3d, short fg, short bg)
{
	short x, y, w;

	if (l3d)
	{
		x = r->g_x;
		y = r->g_y + r->g_h - 1;
		w = r->g_w - 1;
	}
	else
	{
		x = r->g_x;
		y = r->g_y + r->g_h;
		w = r->g_w;
	}
	(*v->api->line)(v, x, y, x + w, y, fg);
	if (l3d)
	{
		y++;
		(*v->api->line)(v, x + 1, y, x + w + 1, y, bg);
	}
}
static void
shadow_object(struct xa_vdi_settings *v, short d, short state, GRECT *rp, short colour, short thick)
{
	GRECT r = *rp;
	short offset, increase;

	/* Are we shadowing this object? (Borderless objects aren't shadowed!) */
	if (thick && (state & OS_SHADOWED))
	{
		short i;

		if (thick < -4) thick = -4;
		else
		if (thick >  4) thick =  4;

		offset = thick > 0 ? thick : 0;
		increase = -thick;

		r.g_x += offset;
		r.g_y += offset;
		r.g_w += increase;
		r.g_h += increase;

		for (i = 0; i < abs(thick)*2; i++)
		{
			r.g_w++, r.g_h++;
			(*v->api->br_hook)(v, d, &r, colour);
		}
	}
}
#if 0
static void
d3_pushbutton(struct xa_vdi_settings *v, struct theme *theme, short d, GRECT *r, BFOBSPEC *col, short state, short thick, short mode)
{
	const unsigned short selected = state & OS_SELECTED;
	short t, j, outline;

	thick = -thick;		/* make thick same direction as d (positive value --> LARGER!) */

	if (thick > 0)		/* outside thickness */
		d += thick;
	d += 2;

	if (mode & 1)		/* fill ? */
	{
		if (col == NULL)
			(*v->api->f_color)(v, theme->normal.c.c);
		/* otherwise set by set_colours() */
		/* inside bar */
		(*v->api->gbar)(v, d, r);
	}

	j = d;
	t = abs(thick);
	outline = j;

#ifdef REND_NAES3D
	if ((theme->rflags & NAES3D) && !(mode & 2))
	{
		(*v->api->l_color)(v, theme->normal.c.box_c);

		while (t > 0)
		{
			/* outside box */
			(*v->api->gbox)(v, j, r);
			t--, j--;
		}

		(*v->api->br_hook)(v, j, r, selected ? theme->normal.c.tlc : theme->normal.c.brc);
		(*v->api->tl_hook)(v, j, r, selected ? theme->normal.c.brc : theme->normal.c.tlc);
	}
	else
#endif
	{
		if (mode & 0x8000)
		{
			j--;
			do {
				(*v->api->br_hook)(v, j, r, selected ? theme->normal.c.tlc : theme->normal.c.brc);
				(*v->api->tl_hook)(v, j, r, selected ? theme->normal.c.brc : theme->normal.c.tlc);
				t--, j--;
			}
			while (t > 0);

			/* full outline ? */
			if (thick && !(mode & 2))
			{
				(*v->api->l_color)(v, theme->normal.c.box_c);
				/* outside box */

				if (thick > 2)
					(*v->api->gbox)(v, outline - 1, r);
				(*v->api->rgbox)(v, outline, 1, r);
			}
		}
		else
		{
			do {
				(*v->api->br_hook)(v, j, r, selected ? theme->normal.c.tlc : theme->normal.c.brc);
				(*v->api->tl_hook)(v, j, r, selected ? theme->normal.c.brc : theme->normal.c.tlc);
				t--, j--;
			}
			while (t >= 0);

			/* full outline ? */
			if (thick && !(mode & 2))
			{
				(*v->api->l_color)(v, theme->normal.c.box_c);
				/* outside box */
				(*v->api->gbox)(v, outline, r);
			}
		}
	}

	shadow_object(v, outline, state, r, theme->shadow_col, thick);
}
#endif
#if 0
/* This function doesnt change colourword anymore, but just sets the required color.
 * Neither does it affect writing mode for text (this is handled in ob_text() */
static void
set_colours(OBJECT *ob, struct xa_vdi_settings *v, struct theme *t, BFOBSPEC *colourword)
{
	(*v->api->wr_mode)(v, MD_REPLACE);

	/* 2 */
	(*v->api->f_interior)(v, FIS_PATTERN);

	if (colourword->fillpattern == IP_SOLID)
	{
		/* 2,8  solid fill  colour */
		(*v->api->f_style)(v, 8);
		(*v->api->f_color)(v, colourword->interiorcol);
	}
	else
	{
		if (colourword->fillpattern == IP_HOLLOW)
		{
			short c;

			/* 2,8 solid fill  white */
			(v->api->f_style)(v, 8);

			/* Object inherits default dialog background colour? */
			if ((colourword->interiorcol == 0) && obj_is_3d(ob))
				c = t->button.norm.n[4].col.c;
			else
				c = G_WHITE;

			(*v->api->f_color)(v, c);
		}
		else
		{
			(*v->api->f_style)(v, colourword->fillpattern);
			(*v->api->f_color)(v, colourword->interiorcol);
		}
	}

#if SELECT_COLOR
	if (!MONO && (ob->ob_state & OS_SELECTED))
	{
		/* Allow a different colour set for 3d push  */
		if (obj_is_3d(ob))
			(*v->api->f_color)(v, selected3D_colour[colourword->interiorcol]);
		else
			(*v->api->f_color)(v, selected_colour[colourword->interiorcol]);
	}
#endif
	(*v->api->t_color)(v, colourword->textcol);
	(*v->api->l_color)(v, colourword->framecol);
}
#endif
static void
draw_3defx(struct xa_vdi_settings *v, struct color_theme *ct, bool selected, short o, short d3_thick, GRECT *r)
{
	if (d3_thick)
	{
		if (selected)
		{
			if (!(ct->col.flags & WCOL_REV3D))
			{
				while (d3_thick > 0)
				{
					(*v->api->right_line)	(v, o, r, ct->col.right);
					(*v->api->bottom_line)	(v, o, r, ct->col.bottom);
					(*v->api->left_line)	(v, o, r, ct->col.left);
					(*v->api->top_line)	(v, o, r, ct->col.top);
					o--, d3_thick--;
				}
			}
			else
			{
				while (d3_thick > 0)
				{
					(*v->api->left_line)	(v, o, r, ct->col.left);
					(*v->api->top_line)	(v, o, r, ct->col.top);
					(*v->api->right_line)	(v, o, r, ct->col.right);
					(*v->api->bottom_line)	(v, o, r, ct->col.bottom);
					o--, d3_thick--;
				}
			}
		}
		else
		{
			if (!(ct->col.flags & WCOL_REV3D))
			{
				while (d3_thick > 0)
				{
					(*v->api->left_line)	(v, o, r, ct->col.left);
					(*v->api->top_line)	(v, o, r, ct->col.top);
					(*v->api->right_line)	(v, o, r, ct->col.right);
					(*v->api->bottom_line)	(v, o, r, ct->col.bottom);
					o--, d3_thick--;
				}
			}
			else
			{
				while (d3_thick > 0)
				{
					(*v->api->right_line)	(v, o, r, ct->col.right);
					(*v->api->bottom_line)	(v, o, r, ct->col.bottom);
					(*v->api->left_line)	(v, o, r, ct->col.left);
					(*v->api->top_line)	(v, o, r, ct->col.top);
					o--, d3_thick--;
				}
			}
		}
	}
}
#if WITH_GRADIENTS
/* is_gradient_installed() lives in render_obj.c - shared service, see render_obj.h */

static void _cdecl
delete_pmap(void *_t)
{
	struct texture *t = _t;

	if (t->xamfdb.mfdb.fd_addr)
		(*api->kfree)(t->xamfdb.mfdb.fd_addr);
	(*api->kfree)(t);
}

static struct xa_wtexture *
find_gradient(struct xa_vdi_settings *v, struct color_theme *ct, short w, short h)
{
	struct texture *t;
	struct xa_wtexture *ret = NULL;
	struct xa_gradient *g = ct->col.gradient;

	if (g && use_gradients && w > 2 && h > 2 && g->n_steps >= 0 )
	{

		if( g == &box_gradient && w == root_window->wa.g_w && h == root_window->wa.g_h )
			g = &box_gradient2;
		w &= g->wmask;
		w |= g->w;
		h &= g->hmask;
		h |= g->h;

		t = (*api->lookup_xa_data_byid)(&g->allocs, (((long)w << 16) | h) );

		if (!t)
		{
			t = kmalloc(sizeof(*t));
			if (t)
			{
				(*api->bclear)(t, sizeof(*t));

				(*v->api->create_gradient)(&t->xamfdb, g->c, g->method, g->n_steps, g->steps, w, h);

				if (t->xamfdb.mfdb.fd_addr)
				{
					(*api->add_xa_data)(&g->allocs, t, (((long)w << 16) | h), NULL, delete_pmap);
					t->t.flags = 0;
					t->t.anchor = 0;
					t->t.left = t->t.right = t->t.top = t->t.bottom = NULL;
					t->t.body = &t->xamfdb;
					ret = &t->t;
				}
				else
				{
					(*api->kfree)(t);
					t = NULL;
				}
			}
		}
		else
			ret = &t->t;
	}
	return ret;
}
#endif

/* make_bkg_img_path() lives in render_obj.c - shared service, see render_obj.h */

#if WITH_BKG_IMG || WITH_GRADIENTS
#include "xa_rsrc.h"
#include "xaaes.h"
#endif

#if WITH_BKG_IMG
/*
 * md = 0: (load and) copy to screen
 *      1: write to disk
 *      2: free buffer
 *      3: load new
 *      4: get status
 */
/* do_bkg_img() lives in render_obj.c - shared service, see render_obj.h */
#endif

static void
draw_objc_bkg(struct widget_tree *wt, struct xa_vdi_settings *v, struct color_theme *ct, BFOBSPEC *cw, short flags, short fg, short box_col, short d, short d3_thick, short box_thick, short shadow_thick, GRECT *wr, GRECT *anch, GRECT *area)
{
#if WITH_GRADIENTS
	struct xa_wtexture *wgrad = NULL;
#endif
	struct xa_wtexture *wext = NULL;
	short f = ct->col.flags, o = 0, j = 0;
	GRECT r = *wr;
	bool selected = (wt->current.ob->ob_state & OS_SELECTED);
	const short *sc;
	bool ind = (selected && obj_is_indicator(wt->current.ob));
	bool use_cw = (cw && (cw->fillpattern != IP_HOLLOW || cw->interiorcol));

	(*v->api->wr_mode)(v, ct->col.wr_mode);

	if (ind)
		sc = selected3D_colour;
	else if (selected && !obj_is_activator(wt->current.ob))
		sc = selected3D_colour;
	else
		sc = NULL;

	/* APJ-OS: resource greys become theme roles (see apj_rsc_table) */
	if (apj_active && !MONO)
	{
		sc = apj_rsc_table(sc != NULL);
		if (box_col >= 0 && box_col < 16)
			box_col = apj_rsc_colour[box_col];
	}

	if (!use_cw)
	{
		if ((ct->col.flags & WCOL_BOXED) && ct->col.box_th > 0)
			j += ct->col.box_th;
	}
	if (box_thick < 0)
	{
		box_thick = -box_thick;
		j += box_thick;
	}
	if (d3_thick < 0)
	{
		d3_thick = -d3_thick;
		j += d3_thick;
	}
	if (d < 0)
	{
		d = -d;
		j += d;
	}
	if (j)
	{
		r.g_x -= j;
		r.g_y -= j;
		j += j;
		r.g_w += j;
		r.g_h += j;
	}

	if (use_cw)
	{
		/* Display a border? */
		if (box_thick)
		{
			if (wt->current.ob != wt->tree || !wt->zen)
			{
				if (flags & DRAW_BOX)
				{

					int i = box_thick;
					(*v->api->l_color)(v, cw->framecol);
					while (i > 0) {
						(*v->api->gbox)(v, o, &r);
						o--;
						i--;
					}
				}
				else
					o -= box_thick;
			}
		}
		if (d3_thick)
		{

			if (flags & DRAW_3D) {
				draw_3defx(v, ct, selected, o, d3_thick, &r);
			}
			o -= d3_thick;
		}

		r.g_x -= o;
		r.g_y -= o;
		r.g_w += o + o;
		r.g_h += o + o;

#if WITH_GRADIENTS
		if ( (ct->col.flags & WCOL_GRADIENT)
			&& (!cw || (r.g_w == root_window->wa.g_w && r.g_h == root_window->wa.g_h)
					|| (cw->interiorcol == G_WHITE || cw->interiorcol == G_BLACK || cw->interiorcol == G_LBLACK || cw->interiorcol == G_LWHITE)) )
		{
			if ((wgrad = find_gradient(v, ct, r.g_w, r.g_h)))
			{
				(*v->api->draw_texture)(v, wgrad->body, &r, wr == anch ? &r : anch);
				flags &= ~(DRAW_BKG|DRAW_TEXTURE);
			}
		}
#endif
		if (flags & DRAW_BKG)
		{
			if (cw->fillpattern == IP_SOLID)
			{
				/* 2,8  solid fill  colour */
				(*v->api->f_interior)(v, FIS_PATTERN);
				(*v->api->f_style)(v, 8);
				(*v->api->f_color)(v, sc ? sc[cw->interiorcol] : cw->interiorcol);
				(*v->api->gbar)(v, 0, &r);
			}
			else
			{
				if (cw->fillpattern == IP_HOLLOW)
				{
					/* Object inherits default dialog background colour? */
					if ((cw->interiorcol == 0) && obj_is_3d(wt->current.ob))
					{
						if (f & WCOL_DRAWBKG)
						{
							(*v->api->f_interior)(v, ct->col.i);
							if (ct->col.i > 1)
								(*v->api->f_style)(v, ct->col.f);
							(*v->api->f_color)(v, ct->col.c);
							(*v->api->gbar)(v, 0, &r);
						}
					}
					else
					{
						/* 2,8 solid fill  white */
						(*v->api->f_interior)(v, FIS_PATTERN);
						(*v->api->f_style)(v, 8);
						(*v->api->f_color)(v, sc  ? sc[G_WHITE] : G_WHITE);
						(*v->api->gbar)(v, 0, &r);
					}
				}
				else
				{
					(*v->api->f_interior)(v, FIS_PATTERN);
					(*v->api->f_style)(v, cw->fillpattern);
					(*v->api->f_color)(v, sc ? sc[cw->interiorcol] : cw->interiorcol);
					(*v->api->gbar)(v, 0, &r);
				}
			}
		}

		if (area)
		{
			*area = r;
		}
	}
	else
	{
		if (box_col == -1)
			box_col = ct->col.box_c;

		if (box_thick)
		{
			if (wt->current.ob != wt->tree || !wt->zen)
			{
				if (flags & DRAW_BOX)
				{
					int i = box_thick;
					(*v->api->l_color)(v, box_col);
					(*v->api->rgbox)(v, o, 1, &r);
					i--;
					o--;

					while (i > 0)
					{
						(*v->api->br_hook)(v, o, &r, box_col);
						(*v->api->tl_hook)(v, o, &r, box_col);
						i--;
						o--;
					}
				}
				else
					o -= box_thick;
			}
		}

		if (ct->col.flags & WCOL_BOXBF3D)
		{
			if ((flags & DRAW_3D))
				draw_3defx(v, ct, selected, o, d3_thick, &r);
			o -= d3_thick;
		}
		if (f & WCOL_BOXED)
		{
			int i = abs(ct->col.box_th);

			(*v->api->l_color)(v, ct->col.box_c);

			if (!(ct->col.flags & WCOL_BOXBF3D) && !box_thick && i)
			{
				(*v->api->rgbox)(v, o, 1, &r);
				o--, i--;
			}

			while (i > 0)
				(*v->api->gbox)(v, o, &r), o--, i--;
		}
		if (!(ct->col.flags & WCOL_BOXBF3D))
		{
			if ((flags & DRAW_3D))
				draw_3defx(v, ct, selected, o, d3_thick, &r);
			o -= d3_thick;
		}

		r.g_x -= o;
		r.g_y -= o;
		r.g_w += o + o;
		r.g_h += o + o;
#if WITH_GRADIENTS
		if ((ct->col.flags & WCOL_GRADIENT))
		{
			if ((wgrad = find_gradient(v, ct, r.g_w, r.g_h)))
			{
				short y = r.g_y;
				(*v->api->draw_texture)(v, wgrad->body, &r, wr == anch ? &r : anch);
				r.g_y = y;
				flags &= ~(DRAW_BKG|DRAW_TEXTURE);
			}
		}
#endif
		if ((flags & DRAW_BKG) && (f & WCOL_DRAWBKG))
		{
			if (fg == -1)
				fg = ct->col.c;
			else if (ind)
				fg = sc[fg];

			(*v->api->f_interior)(v, ct->col.i);
			if (ct->col.i > 1)
				(*v->api->f_style)(v, ct->col.f);
			(*v->api->f_color)(v, fg);
			(*v->api->gbar)(v, 0, &r);
		}

		if (area)
		{
			*area = r;
		}
	}

	if ((flags & DRAW_TEXTURE))
	{
		if ((wext = ct->col.texture)) {
			(*v->api->draw_texture)(v, wext->body, &r, wr == anch ? &r : anch);
		}
	}

	shadow_object(v, 0, wt->current.ob->ob_state, wr, ((struct theme *)wt->objcr_theme)->shadow_col, box_thick);
}

#if 0
static BFOBSPEC
button_colours(struct theme *theme)
{
	BFOBSPEC c;

	c.character = 0;
	c.framesize = 0;

	c.framecol = theme->normal.c.box_c;
	c.textcol  = theme->normal.t.fg;
	c.textmode = 1;
	c.fillpattern = IP_HOLLOW;
	c.interiorcol = theme->normal.c.c;
	return c;
}
#endif

static void
ob_text(XA_TREE *wt,
	struct xa_vdi_settings *v,
	struct objc_edit_info *ei,
	struct color_theme *ct,
	GRECT *r, GRECT *o,
	BFOBSPEC *c,
	short wr_mode,
	short fg,	/* foreground */
	short bg,	/* 3d effect */
	short bcol,	/* banner color */
	short mfgc,
	short mbgc,
	short markbg,
	char *t,
	short state,
	short flags,
	short und, short undcol)
{
	if (t && *t)
	{
		OBJECT *ob = wt->current.ob;
		bool fits = true;
		bool drw3d = (!MONO && ((ct->fnt.flags & WTXT_DRAW3D) || ((flags & OF_FL3DIND) && !(state & OS_DISABLED))));


		/* set according to circumstances. */
		if (c)
		{
		#if 0
			/* more restrictions	*/
			if (    c->textmode
			    && !MONO
			    && obj_is_3d(ob)
			    && (     c->fillpattern == IP_HOLLOW
			         || (c->fillpattern == IP_SOLID && c->interiorcol == G_WHITE)))
			{
				(*v->api->f_color)(v, ct->col.c);
				(*v->api->wr_mode)(v, MD_REPLACE);
				(*v->api->gbar)(v, 0, o ? o : r);
				(*v->api->wr_mode)(v, MD_TRANS);
			}
			else
		#endif
			if (wr_mode < 0)
				wr_mode = c->textmode ? MD_REPLACE : MD_TRANS;
		}
		else if (wr_mode < 0)
			wr_mode = ct->fnt.wr_mode;

		(*v->api->t_effects)(v, ct->fnt.effects);
		if (fg == -1)
		{
			fg = ct->fnt.fg;
			if (bg == -1)
				bg = ct->fnt.bg;
		}
		else if (bg == -1)
			bg = efx3d_colour[fg];

		if (wr_mode == MD_REPLACE)
		{
			GRECT br;

			if (bcol == -1)
				bcol = ct->fnt.bannercol;

			if (bcol == fg)
				bcol = selected3D_colour[fg];

			br.g_x = r->g_x, br.g_y = r->g_y - v->dists[5];
			(*v->api->t_extent)(v, t, &br.g_w, &br.g_h);
			(*v->api->wr_mode)(v, MD_REPLACE);
			(*v->api->f_color)(v, bcol);
			(*v->api->gbar)(v, 0, &br);
		}

		(*v->api->wr_mode)(v, MD_TRANS);

		if ((state & OS_DISABLED))
		{
			done(OS_DISABLED);
		}

		if (ei && !(state & OS_DISABLED) && edit_ob(ei) == ob && ei->m_end > ei->m_start)
		{
			int sl = strlen(t) + 1;
			short x = r->g_x, y = r->g_y - v->dists[5], w, h;
			short start, end;
			char s[256];
			GRECT br = o ? *o : *r;

			start = ei->m_start + ei->edstart;
			end = ei->m_end + ei->edstart;
			if (start)
			{
				strncpy(s, t, start);
				s[start] = '\0';
				if (drw3d)
				{
					(*v->api->t_color)(v, bg);
					v_gtext(v->handle, x+1, y+1, s);
				}
				(*v->api->t_color)(v, fg);
				if (!apj_gtext(v, x, r->g_y, fg, s))
					v_gtext(v->handle, x, y, s);
				(*v->api->t_extent)(v, s, &w, &h);
				x += w;
			}
			strncpy(s, t + start, end - start);
			s[end - start] = '\0';
			(*v->api->t_extent)(v, s, &w, &h);
			br.g_x = x, br.g_w = w;
			(*v->api->f_color)(v, markbg);
			(*v->api->wr_mode)(v, MD_REPLACE);
			(*v->api->gbar)(v, 0, &br);
			(*v->api->wr_mode)(v, MD_TRANS);

			if (drw3d)
			{
				(*v->api->t_color)(v, mbgc);
				v_gtext(v->handle, x, y, s);
			}
			(*v->api->t_color)(v, mfgc);
			if (!apj_gtext(v, x, r->g_y, mfgc, s))
				v_gtext(v->handle, x, y, s);

			if (sl > end)
			{
				x += w;
				strncpy(s, t + end, sl - end);
				s[sl - end] = '\0';
				if (drw3d)
				{
					(*v->api->t_color)(v, bg);
					v_gtext(v->handle, x+1, y+1, s);
				}
				(*v->api->t_color)(v, fg);
				if (!apj_gtext(v, x, r->g_y, fg, s))
					v_gtext(v->handle, x, y, s);
			}
		}
		else
		{
			if (fits)
			{
				if (!MONO && ((ct->fnt.flags & WTXT_DRAW3D) || ((flags & OF_FL3DIND) && !(state & OS_DISABLED))))
				{
					(*v->api->t_color)(v, bg);
					v_gtext(v->handle, r->g_x + 1, r->g_y + 1 - v->dists[5], t);
				}
				(*v->api->t_color)(v, fg);
				if (!apj_gtext(v, r->g_x, r->g_y, fg, t))
					v_gtext(v->handle, r->g_x, r->g_y - v->dists[5], t);
			}
		}
		/* Now underline the shortcut character, if any. */
		/* Ozk: Prepared for proportional fonts! */
		if (und >= 0)
		{
			short l = strlen(t);
			if (und < l)
			{
				char sc;
				short x, y, w, h;

				y = r->g_y;
				x = 0;
				if( und )
				{
					sc = t[und];
					t[und] = '\0';
					(*v->api->t_extent)(v, t, &w, &h);
					x = w;
					t[und] = sc;
				}
				sc = t[und + 1];
				t[und + 1] = '\0';
				(*v->api->t_extent)(v, t, &w, &h);
				if( h > ob->ob_height )
				{
					h = ob->ob_height;
					y += 1;
				}
				t[und + 1] = sc;
				w -= x;
				y += (h - v->dists[0] + 1);
				x += r->g_x;

				/* if selected invert underscore */
				if( (ob->ob_state & OS_SELECTED) )
				{
					if( undcol )
						undcol--;
					else
						undcol++;
				}
				(*v->api->line)(v, x, y, x + w, y, undcol);
			}
		}
		else if (und == -2)
		{
			short w, h;
			GRECT nr = *r;
			(*v->api->t_extent)(v, t, &w, &h);
			nr.g_w = w;

			d3_bottom_line(v, &nr, (flags & OF_FL3DBAK), G_BLACK, G_WHITE);
		}
		else if (und == -3)
		{
			short w;
			GRECT nr = *r;
			if (o)
			{
				nr.g_x = o->g_x;
				nr.g_w = o->g_w;
			}
			(*v->api->t_extent)(v, t, &w, &nr.g_h);
			d3_bottom_line(v, &nr, (flags & OF_FL3DBAK), G_BLACK, G_WHITE);
		}
		(*v->api->t_effects)(v, 0);
	}
}


/*
 * Format a G_FTEXT type text string from its template,
 * and return the real position of the text cursor.
 *
 * HR: WRONG_LEN
 * It is a very confusing here, edit_pos is always (=1) passed as tedinfo->te_tmplen.
 * which means that this only works if that field is wrongly used as the text corsor position.
 * A very bad thing, because it is supposed to be a constant describing the amount of memory
 * allocated to the template string.
 *
 * 28 jan 2001
 * OK, edit_pos is not anymore the te_tmplen field.
 */
static short
format_dialog_text(char *text_out, const char *template, const char *text_in, short edit_pos, short *ret_startpos)
{
	short index = 0, start_tpos = -1, tpos = 0, max = strlen(template), noned = 0;
	/* HR: In case a template ends with '_' and the text is completely
	 * filled, edit_index was indeterminate. :-)
	 */
	short edit_index = max;
	bool aap = *text_in == '@';

	DIAG((D_o, NULL, "format_dialog_text edit_pos %d", edit_pos));
	while (*template)
	{
		if (*template != '_')
		{
			*text_out++ = *template;
			if( start_tpos != -1 )
			{
				noned++;	/* inc. max-position for cursor */
			}
		}
		else
		{
			if (start_tpos == -1)
				start_tpos = index;

			/* Found text field */

			if (tpos == edit_pos)
				edit_index = index;

			if (*text_in)
			{
				if (aap)
					*text_out++ = '_';
				else
					*text_out++ = *text_in++;
			}
			else
				*text_out++ = '_';

			tpos++;
		}
		template++;
		index++;
	}

	*text_out = '\0';

	if (edit_index > (start_tpos + tpos + noned))
		edit_index = start_tpos + tpos;

	if (ret_startpos)
		*ret_startpos = start_tpos;

	/* keep visible at end */
	if (edit_index > max)
		edit_index--;

	return edit_index;
}

static void
get_tedinformation(OBJECT *ob, TEDINFO **ret_ted, BFOBSPEC *cw, struct objc_edit_info **ret_ei)
{
	union { short jc[2]; BFOBSPEC bfobspec;} col;
	TEDINFO *ted;
	XTEDINFO *xted = NULL;

	ted = (*api->object_get_tedinfo)(ob, &xted);

	if (ted)
	{
		if (cw)
		{
			col.jc[1] = ted->te_color;
			col.bfobspec.framesize = ted->te_thickness;
			col.bfobspec.character = 0;
			*cw = col.bfobspec;
		}
	}

	if (ret_ted)
		*ret_ted = ted;
	if (ret_ei)
		*ret_ei = xted;
}
#if 0
/* implement wt->x,y
 * Modifications: set_text(...  &gr, &cr, temp_text ...)
 * cursor alignment in centered text.
 * Justification now integrated in set_text (v_gtext isolated).
 * Preparations for positioning cursor in proportional fonts.
 * Moved a few calls.
 * pbox(hl,ext[0]+x,ext[1]+y,ext[4]+x-1,ext[5]+y-1);
 */
static void
set_text(OBJECT *ob,
	 struct xa_vdi_settings *v,
	 GRECT *gr,
	 GRECT *cr,
	 bool formatted,
	 short edit_pos,
	 char *temp_text,
	 BFOBSPEC *colours,
	 short *thick,
	 short *ret_edstart,
	 struct objc_edit_info **ret_ei,
	 GRECT r)
{
	union { short jc[2]; BFOBSPEC bfobspec;} col;
	TEDINFO *ted;
	XTEDINFO *xted = NULL;
	GRECT cur;
	short w, h, cur_x = 0, start_tpos = 0;

	ted = (TEDINFO *)(*api->object_get_spec)(ob)->index;

	if (ted->te_ptext == (char *)0xffffffffL)
	{
		xted = (XTEDINFO *)ted->te_ptmplt;
		ted = &xted->ti;
		if (ret_ei)
			*ret_ei = xted;
	}
	else if (ret_ei)
		*ret_ei = NULL;

	*thick = (char)ted->te_thickness;

	col.jc[0] = ted->te_just;
	col.jc[1] = ted->te_color;
	*colours = col.bfobspec;

    /*
     * FIXME: gemlib problem: hacked a bit need only "ted->te_color" word;
	 *	  -> cleaning the information that would not be taken if
	 *	     properly used:
	 */
	colours->character = 0;
	colours->framesize = 0;

	/* Set the correct text size & font */
	switch (ted->te_font)
	{
	case TE_GDOS_PROP:		/* Use a proportional SPEEDOGDOS font (AES4.1 style) */
	case TE_GDOS_MONO:		/* Use a monospaced SPEEDOGDOS font (AES4.1 style) */
	case TE_GDOS_BITM:		/* Use a GDOS bitmap font (AES4.1 style) */
	{
		(*v->api->t_font)(v, ted->te_fontsize, ted->te_fontid);
		cur.g_w = screen->c_max_w;
		cur.g_h = screen->c_max_h;
		break;
	}
	case TE_SMALL:			/* Use the small system font (probably 8 point) */
	{
		(*v->api->t_font)(v, screen->small_font_point, screen->small_font_id);
		cur.g_w = screen->c_min_w;
		cur.g_h = screen->c_min_h;
		break;
	}
	case TE_STANDARD:		/* Use the standard system font (probably 10 point) */
	default:
	{
		(*v->api->t_font)(v, screen->standard_font_point, screen->standard_font_id);
		cur.g_w = screen->c_max_w;
		cur.g_h = screen->c_max_h;
		break;
	}
	}
	(*v->api->t_effects)(v, 0);
	/* HR: use vqt_extent to obtain x, because it is almost impossible, or
	 *	at least very unnecessary tedious to position the cursor.
	 *	vst_alignment is not needed anymore.
	 */
	/* after vst_font & vst_point */

	if (formatted)
	{
		cur_x = format_dialog_text(temp_text, ted->te_ptmplt, ted->te_ptext, edit_pos, &start_tpos);
	}
	else
		strncpy(temp_text, ted->te_ptext, 255);

	(*v->api->t_extent)(v, temp_text, &w, &h);

	/* HR 290301 & 070202: Dont let a text violate its object space! (Twoinone packer shell!! :-) */
	if (w > r.g_w)
	{
		short	rw  = r.g_w / cur.g_w,
			dif = (w - r.g_w + cur.g_w - 1) / cur.g_w,
			h1dif, h2dif;

		switch (ted->te_just)
		{
			case TE_RIGHT:
			{
				strcpy (temp_text, temp_text + dif);
				break;
			}
			case TE_CNTR:
			{
				h1dif = dif/2;
				h2dif = (dif+1)/2;
				*(temp_text + strlen(temp_text) - h2dif) = 0;
				strcpy (temp_text, temp_text + h1dif);
				break;
			}
			default:
			case TE_LEFT:
			{
				*(temp_text + rw) = '\0';
				break;
			}
		}
		(*v->api->t_extent)(v, temp_text, &w, &h);
	}

	switch (ted->te_just)		/* Set text alignment - why on earth did */
	{					/* Atari use a different horizontal alignment */
		case TE_RIGHT:
		{
			cur.g_x = r.g_x + r.g_w - w;
			break;
		}
		case TE_CNTR:
		{
			cur.g_x = r.g_x + ((r.g_w - w) / 2);
			break;
		}
		default:
		case TE_LEFT:			/* code for GEM to the one the VDI uses? */
		{
			cur.g_x = r.g_x;
			break;
		}
	}

	cur.g_y = r.g_y + (r.g_h - h) / 2;

	if (cr)
	{
		short tw, th;
		char sc;

		sc = temp_text[cur_x];
		temp_text[cur_x] = '\0';
		(*v->api->t_extent)(v, temp_text, &tw, &th);
		temp_text[cur_x] = sc;

		*cr = cur;
		cr->g_x += tw;
		cr->g_w = 1;
	}

	cur.g_w = w;
	cur.g_h = h;
	*gr = cur;

	if (ret_edstart)
		*ret_edstart = start_tpos;
}
#endif

static void
set_obtext(TEDINFO *ted,
	 struct xa_vdi_settings *v,
	 GRECT *gr,
	 GRECT *cr,
	 bool formatted,
	 short edit_pos,
	 char *temp_text,
	 short *ret_edstart,
	 GRECT r)
{
	GRECT cur;
	short w, h, cur_x = 0, start_tpos = 0;

	/* Set the correct text size & font */
	switch (ted->te_font)
	{
	case TE_GDOS_PROP:		/* Use a proportional SPEEDOGDOS font (AES4.1 style) */
	case TE_GDOS_MONO:		/* Use a monospaced SPEEDOGDOS font (AES4.1 style) */
	case TE_GDOS_BITM:		/* Use a GDOS bitmap font (AES4.1 style) */
	{
		(*v->api->t_font)(v, ted->te_fontsize, ted->te_fontid);
		cur.g_w = screen->c_max_w;
		cur.g_h = screen->c_max_h;
		break;
	}
	case TE_SMALL:			/* Use the small system font (probably 8 point) */
	{
		(*v->api->t_font)(v, screen->small_font_point, screen->small_font_id);
		cur.g_w = screen->c_min_w;
		cur.g_h = screen->c_min_h;
		break;
	}
	case TE_STANDARD:		/* Use the standard system font (probably 10 point) */
	default:
	{
		(*v->api->t_font)(v, screen->standard_font_point, screen->standard_font_id);
		cur.g_w = screen->c_max_w;
		cur.g_h = screen->c_max_h;
		break;
	}
	}

	/* HR: use vqt_extent to obtain x, because it is almost impossible, or
	 *	at least very unnecessary tedious to position the cursor.
	 *	vst_alignment is not needed anymore.
	 */
	/* after vst_font & vst_point */

	if (formatted)
	{
		cur_x = format_dialog_text(temp_text, ted->te_ptmplt, ted->te_ptext, edit_pos, &start_tpos);
	}
	else
		strncpy(temp_text, ted->te_ptext, 255);

	(*v->api->t_effects)(v, 0);	/* use normal style */

	(*v->api->t_extent)(v, temp_text, &w, &h);

	/* HR 290301 & 070202: Dont let a text violate its object space! (Twoinone packer shell!! :-) */
	if (w > r.g_w)
	{
		short	rw  = r.g_w / cur.g_w,
			dif = (w - r.g_w + cur.g_w - 1) / cur.g_w,
			h1dif, h2dif;

		switch (ted->te_just)
		{
			case TE_RIGHT:
			{
				strcpy (temp_text, temp_text + dif);
				break;
			}
			case TE_CNTR:
			{
				h1dif = dif/2;
				h2dif = (dif+1)/2;
				*(temp_text + strlen(temp_text) - h2dif) = '\0';
				strcpy (temp_text, temp_text + h1dif);
				break;
			}
			default:
			case TE_LEFT:
			{
				*(temp_text + rw) = '\0';
				break;
			}
		}
		(*v->api->t_extent)(v, temp_text, &w, &h);
	}

	switch (ted->te_just)		/* Set text alignment - why on earth did */
	{
					/* Atari use a different horizontal alignment */
		case TE_RIGHT:
		{
			cur.g_x = r.g_x + r.g_w - w;
			break;
		}
		case TE_CNTR:
		{
			cur.g_x = r.g_x + ((r.g_w - w) / 2);
			break;
		}
		default:
		case TE_LEFT:			/* code for GEM to the one the VDI uses? */
		{
			cur.g_x = r.g_x;
			break;
		}
	}

	cur.g_y = r.g_y + ((r.g_h - h) / 2);

	if (cr)
	{
		short tw, th;
		char sc;

		sc = temp_text[cur_x];
		temp_text[cur_x] = '\0';
		(*v->api->t_extent)(v, temp_text, &tw, &th);
		temp_text[cur_x] = sc;

		*cr = cur;

		cr->g_x += tw;
		cr->g_w = 1;
	}

	cur.g_w = w;
	cur.g_h = h;
	*gr = cur;

	if (ret_edstart)
		*ret_edstart = start_tpos;
}
static void _cdecl g2d_box(struct xa_vdi_settings *v, short b, GRECT *r, short colour);
static void _cdecl write_selection(struct xa_vdi_settings *v, short d, GRECT *r);

static void
draw_outline(struct widget_tree *wt, struct xa_vdi_settings *v, short d, GRECT *r)
{
	struct bcol *col = &((struct theme *)wt->objcr_theme)->outline;

	/* special handling of root object. */
	if ((wt->current.ob->ob_state & OS_OUTLINED) && (!wt->zen || wt->current.ob != wt->current.tree))
	{
		if (!MONO && obj_is_background(wt->current.ob))
		{
			(*v->api->tl_hook)(v, d+1, r, col->top);
			(*v->api->br_hook)(v, d+1, r, col->bottom);
			(*v->api->tl_hook)(v, d+2, r, col->top);
			(*v->api->br_hook)(v, d+2, r, col->bottom);
			(*v->api->gbox)(v, d+3, r);
		}
		else
		{
			(*v->api->l_color)(v, G_WHITE);
			(*v->api->gbox)(v, d+1, r);
			(*v->api->gbox)(v, d+2, r);
			(*v->api->l_color)(v, G_BLACK);
			(*v->api->gbox)(v, d+3, r);
		}
	}

	done(OS_OUTLINED);
}

#include "desktop.h"

static void
draw_g_box(struct widget_tree *wt, struct xa_vdi_settings *v, struct color_theme *ct, BFOBSPEC *c, short flags, GRECT *area)
{
	GRECT r = wt->r;
	OBJECT *ob = wt->current.ob;
	short thick, d, d3t, out;

	thick = obj_thickness(wt, ob);


	/* before borders */
	done(OS_SELECTED);
	d3t = 0;
	out = 0;
	d = 0;
	if (obj_is_indicator(ob) || obj_is_activator(ob))
	{
		d3t--;
		d--;
		out += 2;
	}
	else if (!c && (ct->col.flags & WCOL_DRAW3D) && (flags & DRAW_3D))
		d3t--;

	draw_objc_bkg(wt, v, ct, c, flags, -1, c ? c->framecol : -1, d, d3t, thick, thick, &r, (flags & ANCH_PARENT) ? (GRECT *)&wt->current.tree->ob_x : &r, area);

	draw_outline(wt, v, out, &r);
}

/* ************************************************************ */
/*        Functions exported in objcr_theme			*/
/* ************************************************************ */
static void _cdecl
write_menu_line(struct xa_vdi_settings *v, GRECT *cl)
{
	/* lower */
	(*v->api->line)(v, cl->g_x, cl->g_y + cl->g_h - 1, cl->g_x + cl->g_w - 1, cl->g_y + cl->g_h - 1, G_BLACK);
	if( api->cfg->menu_layout )
	{
		/* right */
		(*v->api->line)(v, cl->g_x + cl->g_w - 1, cl->g_y + cl->g_h-1, cl->g_x + cl->g_w - 1, cl->g_y, G_BLACK);
		/* left */
		(*v->api->line)(v, cl->g_x + 8, cl->g_y + cl->g_h-1, cl->g_x + 8, cl->g_y, G_BLACK);
	}
}

static void _cdecl
g2d_box(struct xa_vdi_settings *v, short b, GRECT *r, short colour)
{
	/* inside runs from 3 to 0 */
	if (b > 0)
	{
		if (b >  4) b =  3;
		else        b--;
		(*v->api->l_color)(v, colour);
		while (b >= 0)
			(*v->api->gbox)(v, -b, r), b--;
	}
	/* outside runs from 4 to 1 */
	else if (b < 0)
	{
		if (b < -4) b = -4;
		(*v->api->l_color)(v, colour);
		while (b < 0)
			(*v->api->gbox)(v, -b, r), b++;
	}
}

static void _cdecl
draw_2d_box(struct xa_vdi_settings *v, short x, short y, short w, short h, short border_thick, short colour)
{
	GRECT r;

	r.g_x = x;
	r.g_y = y;
	r.g_w = w;
	r.g_h = h;

	g2d_box(v, border_thick, &r, colour);
}

static void _cdecl
write_selection(struct xa_vdi_settings *v, short d, GRECT *r)
{
	(*v->api->wr_mode)(v, MD_XOR);
	(*v->api->f_color)(v, G_BLACK);
	(*v->api->f_interior)(v, FIS_SOLID);
	(*v->api->gbar)(v, d, r);
	(*v->api->wr_mode)(v, MD_TRANS);
}

static void
write_selected(struct xa_vdi_settings *v, GRECT *r, short wr_mode, short colour)
{
	(*v->api->wr_mode)(v, wr_mode);
	(*v->api->f_color)(v, colour);
	(*v->api->f_interior)(v, FIS_SOLID);
	(*v->api->gbar)(v, 0, r);
	(*v->api->wr_mode)(v, MD_TRANS);
}

static void
write_disable(struct xa_vdi_settings *v, GRECT *r, short colour)
{
	static short pattern[16] =
	{
		0x5555, 0xaaaa, 0x5555, 0xaaaa,
		0x5555, 0xaaaa, 0x5555, 0xaaaa,
		0x5555, 0xaaaa, 0x5555, 0xaaaa,
		0x5555, 0xaaaa, 0x5555, 0xaaaa
	};
	(*v->api->wr_mode)(v, MD_TRANS);
	(*v->api->f_color)(v, colour);
	vsf_udpat(v->handle, pattern, 1);
	(*v->api->f_interior)(v, FIS_USER);
	(*v->api->gbar)(v, 0, r);
}


static void
rl_xor(struct xa_vdi_settings *v, GRECT *r, struct xa_rect_list *rl)
{
	if (rl)
	{
		short c[4];
		while (rl)
		{
			(*v->api->rtopxy)(c, &rl->r);
			vs_clip(v->handle, 1, c);
			write_selection(v, 0, r);
			rl = rl->next;
		}
		(*v->api->rtopxy)(c, &v->clip);
		vs_clip(v->handle, 1, c);
	}
	else
		write_selection(v, 0, r);
}

static void _cdecl
set_cursor(struct widget_tree *wt, struct xa_vdi_settings *v, struct objc_edit_info *ei)
{
	char temp_text[256];
	GRECT r;
	OBJECT *ob;
	GRECT gr;
	TEDINFO *ted;

 	XTEDINFO *xted;

	if (!ei)
		ei = &wt->e;

	if (!edit_set(ei))
		return;

	(*api->obj_rectangle)(wt, editfocus(ei), &r);
	ob = edit_ob(ei);

	get_tedinformation(ob, &ted, NULL/*&c*/, &xted);

	set_obtext(ted, v, &gr, &ei->cr, true, ei->pos, temp_text, &ei->edstart, r);

	ei->cr.g_x -= wt->tree->ob_x;
	ei->cr.g_y -= wt->tree->ob_y;
}

static void _cdecl
eor_cursor(struct widget_tree *wt, struct xa_vdi_settings *v, struct xa_rect_list *rl)
{
	struct objc_edit_info *ei = wt->ei;
	GRECT r;

	if (!ei)
		ei = &wt->e;

	if (edit_set(ei))
	{
		set_cursor(wt, v, ei);
		r = ei->cr;
		r.g_x += wt->tree->ob_x;
		r.g_y += wt->tree->ob_y;

		if (!(edit_ob(ei)->ob_flags & OF_HIDETREE))
		{
			rl_xor(v, &r, rl);
		}
	}
}

static void _cdecl
draw_cursor(struct widget_tree *wt, struct xa_vdi_settings *v, struct xa_rect_list *rl, short rdrw)
{
	struct objc_edit_info *ei = wt->ei;

	if (!ei)
		return;

	if ( !(ei->c_state & (OB_CURS_ENABLED)) )
	{
		if (edit_set(ei) && !(edit_ob(ei)->ob_flags & OF_HIDETREE))
		{
			if (rdrw)
			{
				GRECT r = ei->cr;

				r.g_x += wt->tree->ob_x;
				r.g_y += wt->tree->ob_y;
				rl_xor(v, &r, rl);
			}
			ei->c_state |= OB_CURS_ENABLED;
		}
	}
}

static void _cdecl
undraw_cursor(struct widget_tree *wt, struct xa_vdi_settings *v, struct xa_rect_list *rl, short rdrw)
{
	struct objc_edit_info *ei = wt->ei;

	if (!ei)
		return;

	if ( (ei->c_state & (OB_CURS_ENABLED)))
	{

		if (edit_set(ei) && !(edit_ob(ei)->ob_flags & OF_HIDETREE))
		{
			if (rdrw)
			{
				GRECT r = ei->cr;

				r.g_x += wt->tree->ob_x;
				r.g_y += wt->tree->ob_y;
				rl_xor(v, &r, rl);
			}
			ei->c_state &= ~OB_CURS_ENABLED;
		}
	}
}


static short _cdecl
obj_thickness(struct widget_tree *wt, OBJECT *ob)
{
	short t = 0, flags;
	TEDINFO *ted;

	switch (ob->ob_type & 0xff)
	{
		case G_BOX:
		case G_IBOX:
		case G_BOXCHAR:
		{
			t = (*api->object_get_spec)(ob)->obspec.framesize;
			break;
		}
		case G_BUTTON:
		{
			flags = ob->ob_flags;
			t = -1;
			if (flags & OF_EXIT)
				t--;
			if (flags & OF_DEFAULT)
				t--;
			break;
		}
		case G_BOXTEXT:
		case G_FBOXTEXT:
		{
			ted = (*api->object_get_tedinfo)(ob, NULL);
			t = (signed char)ted->te_thickness;
		}
	}
	return t;
}

/*
 * Return offsets to add to object dimensions to account for borders, etc.
 */
static void _cdecl
obj_offsets(struct widget_tree *wt, OBJECT *ob, GRECT *c)
{
	short dx = 0, dy = 0, dw = 0, dh = 0, db = 0;
	short thick;

	if (ob->ob_type != G_PROGDEF)
	{
		thick = obj_thickness(wt, ob);   /* type dependent */

		if (thick < 0)
			db = thick;

		/* HR 0080801: oef oef oef, if foreground any thickness has the 3d enlargement!! */
		if (obj_is_foreground(ob))
			db -= 2;

		dx = db;
		dy = db;
		dw = 2 * db;
		dh = 2 * db;

		if (ob->ob_state & OS_OUTLINED)
		{
			dx = min(dx, -3);
			dy = min(dy, -3);
			dw = min(dw, -6);
			dh = min(dh, -6);
		}

		/* Are we shadowing this object? (Borderless objects aren't shadowed!) */
		if (thick < 0 && ob->ob_state & OS_SHADOWED)
		{
			dw += 2 * thick;
			dh += 2 * thick;
		}
	}
	c->g_x = dx;
	c->g_y = dy;
	c->g_w = dw;
	c->g_h = dh;
}

static short _cdecl
objc_sysvar(void *_theme, short mode, short what, short *val1, short *val2, short *val3, short *val4)
{
	struct theme *theme = _theme;
	short ret = -1;

	switch (what)
	{
		case LK3DIND:	/* Indicator objects */
		{
			if (mode == SV_INQUIRE)
			{
				if (val1) *val1 = 0;	/* Text moves when selecting */
				if (val2) *val2 = 0;	/* Color changes when selecting */
				ret = 2;
			}
			break;
		}
		case LK3DACT:	/* Activator objects */
		{
			if (mode == SV_INQUIRE)
			{
				if (val1) *val1 = 1;	/* Text moves when selecting */
				if (val2) *val2 = 0;	/* Color changes when selected */
				ret = 2;
			}
			break;
		}
		case INDBUTCOL:
		{
			if (val1)
			{
				if (mode == SV_INQUIRE)
				{
					*val1 = theme->button.norm.n[IND].col.c;
					ret = 1;
				}
				else if (mode == SV_SET)
				{
					theme->button.norm.n[IND].col.c = *val1;
					ret = 0;
				}
			}
			break;
		}
		case ACTBUTCOL:
		{
			if (val1)
			{
				if (mode == SV_INQUIRE)
				{
					*val1 = theme->button.norm.n[ACT].col.c;
					ret = 1;
				}
				else
				{
					theme->button.norm.n[ACT].col.c = *val1;
					theme->button.norm.s[ACT].col.c = *val1;
					theme->button.norm.h[ACT].col.c = *val1;
					ret = 0;
				}
			}
			break;
		}
		case BACKGRCOL:
		{
			if (val1)
			{
				if (mode == SV_INQUIRE)
				{
					*val1 = theme->button.norm.n[BKG].col.c;
					ret = 1;
				}
				else if (mode == SV_SET)
				{
					theme->button.norm.n[BKG].col.c = *val1;
					ret = 0;
				}
			}
			break;
		}
		case AD3DVAL:
		{
			if (mode == SV_INQUIRE)
			{
				if (val1 && val2)
				{
					*val1 = theme->ad3dval_x;
					*val2 = theme->ad3dval_y;
					ret = 2;
				}
			}
			break;
		}
		default:
		{
			ret = -1;
			break;
		}
	}
	return ret;
}

static short _cdecl
obj_is_transparent(struct widget_tree *wt, OBJECT *ob, bool pdistrans)
{
	bool ret = false;

	if (ob->ob_flags & OF_RBUTTON)
		ret = true;
	else
	{
		switch (ob->ob_type & 0xff)
		{
#if 0
			case G_BOX:
			case G_BOXCHAR:
			{
				if (!(*(BFOBSPEC *)object_get_spec(ob)).fillpattern)
					ret = true;
				break;
			}
#endif
			case G_STRING:
			case G_SHORTCUT:
			case G_IBOX:
			case G_TITLE:
			{
				ret = true;
				break;
			}
			case G_PROGDEF:
			{
				ret = pdistrans ? true : false;
				break;
			}
			case G_TEXT:
			case G_FTEXT:
			case G_BOXTEXT:
			case G_FBOXTEXT:
			{
				union { short jc[2]; BFOBSPEC cw;} col;
				TEDINFO *ted;

				ted = (*api->object_get_tedinfo)(ob, NULL);
				col.jc[0] = ted->te_just;
				col.jc[1] = ted->te_color;
				if (col.cw.textmode)
					ret = true;
				break;
			}
			default:;
		}
	}
	/* opaque */
	return ret;
}
static struct object_render_api def_render_api =
{
	{ 0 },

	objc_sysvar,
	obj_offsets,
	obj_thickness,
	obj_is_transparent,

	write_menu_line,
	g2d_box,
	draw_2d_box,
	write_selection,

	set_cursor,
	eor_cursor,
	draw_cursor,
	undraw_cursor,

	{
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,

		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,

		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL
	},
};

static DrawObject
	d_g_box,
	d_g_ibox,
	d_g_boxtext,
	d_g_boxchar,
	d_g_boxtext,
	d_g_fboxtext,
	d_g_button,
	d_g_image,
	d_g_icon,
	d_g_cicon,
	d_g_text,
	d_g_ftext,
	d_g_string,
	d_g_title;

static void
setup_render_api(struct object_render_api *rapi)
{
	int i;
	DrawObject **objc_jump_table = rapi->drawers;

	*rapi = def_render_api;

 	for (i = 0; i < 256; i++)
		objc_jump_table[i] = NULL;

	objc_jump_table[G_BOX     ] = d_g_box;
	objc_jump_table[G_TEXT    ] = d_g_text;
	objc_jump_table[G_BOXTEXT ] = d_g_boxtext;
	objc_jump_table[G_IMAGE   ] = d_g_image;
/*	objc_jump_table[G_PROGDEF ] = d_g_progdef; */
	objc_jump_table[G_IBOX    ] = d_g_ibox;
	objc_jump_table[G_BUTTON  ] = d_g_button;
	objc_jump_table[G_BOXCHAR ] = d_g_boxchar;
	objc_jump_table[G_STRING  ] = d_g_string;
	objc_jump_table[G_FTEXT   ] = d_g_ftext;
	objc_jump_table[G_FBOXTEXT] = d_g_fboxtext;
	objc_jump_table[G_ICON    ] = d_g_icon;
	objc_jump_table[G_TITLE   ] = d_g_title;
	objc_jump_table[G_CICON   ] = d_g_cicon;

	objc_jump_table[G_SWBUTTON] = d_g_button;
	objc_jump_table[G_POPUP   ] = d_g_button;
/*	objc_jump_table[G_WINTITLE] = d_g_wintitle; */
/*	objc_jump_table[G_EDIT    ] = d_g_edit; */

	objc_jump_table[G_SHORTCUT] = d_g_string;
/*	objc_jump_table[G_SLIST   ] = d_g_slist; */
	objc_jump_table[G_EXTBOX  ] = d_g_box;
}
#if 0
static inline short
get_g_box_theme(struct widget_tree *wt, struct xa_aes_object obj, OBJECT **ob, bool *sel, bool *dis, struct object_theme **obt, struct color_theme **ct)
{
	struct theme *theme = wt->objcr_theme;
	short fl3d;

	*ob = aesobj_ob(&obj);
	*sel = (*ob)->ob_state & OS_SELECTED;
	*dis = (*ob)->ob_state & OS_DISABLED;

	fl3d = ((*ob)->ob_flags & OF_FL3DMASK) >> 9;

	if (wt->is_menu && wt->pop == aesobj_item(&obj))
		*obt = &theme->popupbkg;
	else
		*obt = &theme->box;

	if (*dis)
		*ct = *sel ? &(*obt)->dis.s[fl3d]  : &(*obt)->dis.n[fl3d];
	else
		*ct = *sel ? &(*obt)->norm.s[fl3d] : &(*obt)->norm.n[fl3d];

	return fl3d;
}
#endif
static void _cdecl
d_g_box(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	OBJECT *ob = wt->current.ob;
	struct theme *theme = wt->objcr_theme;
	struct object_theme *obt;
	struct color_theme *ct;
	BFOBSPEC c;
	short fl3d, bkg_flags;
	bool selected;

#if 0
	fl3d = get_g_box_theme(wt, wt->current, &ob, &selected, &disabled, &obt, &ct);

	c = (*api->object_get_spec)(ob)->obspec;

#else

#endif
#if WITH_BKG_IMG
	if( wt == get_desktop() && !memcmp( &wt->r, &root_window->wa, sizeof(GRECT)) )
	{

		if( !do_bkg_img(wt->owner, 0, 0) )
			return;
	}
#endif
	if ((ob->ob_type & 0xff) == G_EXTBOX)
	{
		union { BFOBSPEC c; unsigned long l; } conv;
		struct extbox_parms *p = (struct extbox_parms *)(*api->object_get_spec)(ob)->index;
		short ty = ob->ob_type;

		conv.l = p->obspec;
		c = conv.c;

		fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;
		selected = ob->ob_state & OS_SELECTED;

		obt = &theme->box;

		if (fl3d && (c.fillpattern == IP_HOLLOW && !c.interiorcol))
			bkg_flags = DRAW_ALL|DRAW_TEXTURE|ONLY_TEXTURE;
		else
			bkg_flags = DRAW_ALL;

		if (ob->ob_state & OS_DISABLED)
		{
			ct = selected ? &obt->dis.s[fl3d] : &obt->dis.n[fl3d];
		}
		else
		{
			ct = selected ? &obt->norm.s[fl3d] : &obt->norm.n[fl3d];
		}

		ob->ob_type = p->type;
		(*api->object_set_spec)(ob, (unsigned long)p->obspec);
		draw_g_box(wt, v, ct, &c, bkg_flags, NULL);
		(*api->object_set_spec)(ob, (unsigned long)p);
		ob->ob_type = ty;

		p->wt		= wt;
		p->index	= wt->current.item;
		p->r		= wt->r;
		if ((*api->rect_clip)(&wt->r, &v->clip, &p->clip))
		{
			p->clip.g_w = p->clip.g_x + p->clip.g_w - 1;
			p->clip.g_h = p->clip.g_y + p->clip.g_h - 1;

			(*p->callout)(p);
		}
	}
	else
	{
		c = (*api->object_get_spec)(ob)->obspec;
		fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;
		selected = ob->ob_state & OS_SELECTED;

		obt = NULL;
		bkg_flags = DRAW_ALL;
		if (wt->is_menu)
		{
			bkg_flags |= DRAW_TEXTURE|ONLY_TEXTURE;
			if (wt->pop == wt->current.item)
			{
				obt = &theme->popupbkg;
			}
			else if (wt->current.item == 1 && (wt->current.tree[3].ob_type & 0xff) == G_TITLE)
			{
				obt = &theme->menubar;
			}
		}
		if (!obt)
		{
			obt = &theme->box;

			if (fl3d && (c.fillpattern == IP_HOLLOW && !c.interiorcol))
				bkg_flags |= DRAW_TEXTURE|ONLY_TEXTURE;
		}
		if (ob->ob_state & OS_DISABLED)
		{
			ct = selected ? &obt->dis.s[fl3d] : &obt->dis.n[fl3d];
		}
		else
		{
			ct = selected ? &obt->norm.s[fl3d] : &obt->norm.n[fl3d];
		}
		/* APJ-OS: a menu bar / drop-down / popup surface is the theme's,
		 * not the resource's colour word - the stock look got the same
		 * effect from its gradient, which the flat theme has removed */
		draw_g_box(wt, v, ct, (apj_active && wt->is_menu && !MONO) ? NULL : &c, bkg_flags, NULL);
	}
	done(OS_DISABLED|OS_SELECTED);
}

/*
 * Draw a plain hollow ibox
 */
static void _cdecl
d_g_ibox(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	OBJECT *ob = wt->current.ob;
	struct theme *theme = wt->objcr_theme;
	struct object_theme *obt;
	struct color_theme *ct;
	BFOBSPEC c;
	short fl3d, bkg_flags;
	bool selected;

	c = (*api->object_get_spec)(ob)->obspec;
	fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;
	selected = ob->ob_state & OS_SELECTED;

	obt = NULL;
	bkg_flags = DRAW_BOX|DRAW_3D;
	/* do nothing if inside a menu */
	if (wt->is_menu)
	{
		done(OS_SELECTED|OS_DISABLED);
		return;
	}
	if (!obt)
	{
		obt = &theme->box;
		if (fl3d && (c.fillpattern == IP_HOLLOW && !c.interiorcol))
			bkg_flags |= DRAW_TEXTURE|ONLY_TEXTURE;
	}
	if (ob->ob_state & OS_DISABLED)
	{
		if (selected)
		{
			ct = &obt->dis.s[fl3d];
			write_selection(v, 0, &wt->r);
			write_disable(v, &wt->r, G_LBLACK);
		}
		else
		{
			ct = &obt->dis.n[fl3d];
			write_disable(v, &wt->r, (fl3d == BKG) ? G_LWHITE : G_WHITE);
		}
	}
	else
	{
		if (selected)
		{
			ct = &obt->norm.s[fl3d];
			write_selection(v, 0, &wt->r);
		}
		else
		{
			ct = &obt->norm.n[fl3d];
		}
	}
	draw_g_box(wt, v, ct, &c, bkg_flags, NULL);
	done(OS_SELECTED|OS_DISABLED);
}

/*
 * Display a boxchar (respecting 3d flags)
 */
static void _cdecl
d_g_boxchar(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	struct theme *theme = wt->objcr_theme;
	struct object_theme *obt = &theme->boxtext;
	struct color_theme *ct;
	GRECT r = wt->r, gr = r;
	OBJECT *ob = wt->current.ob;
	BFOBSPEC c;
	short w, h, fl3d, fcol;
	ushort selected = ob->ob_state & OS_SELECTED;
	char temp_text[2];
	bool disabled;

	selected = ob->ob_state & OS_SELECTED;
	fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;

	if ((disabled = (ob->ob_state & OS_DISABLED)))
	{
		ct = selected ? &obt->dis.s[fl3d] : &obt->dis.n[fl3d];
	}
	else
	{
		ct = selected ? &obt->norm.s[fl3d] : &obt->norm.n[fl3d];
	}
	c = (*api->object_get_spec)(ob)->obspec;
	temp_text[0] = c.character;
	temp_text[1] = '\0';


	draw_g_box(wt, v, ct, &c, DRAW_ALL, &gr);
	(*v->api->t_effects)(v, ct->fnt.effects);
	(*v->api->t_font)(v, screen->standard_font_point, screen->standard_font_id);
	(*v->api->t_extent)(v, temp_text, &w, &h);
	gr.g_x += (gr.g_w - w) >> 1;
	gr.g_y += (gr.g_h - h) >> 1;

	if (!disabled)
	{
		if (selected)
		{
			if (fl3d == ACT)
			{
				gr.g_x += PUSH3D_DISTANCE;
				gr.g_y += PUSH3D_DISTANCE;
				fcol = c.textcol;
			}
			else
				fcol = selected3D_colour[c.textcol];
		}
		else
			fcol = c.textcol;
	}
	else
		fcol = -1;

	ob_text(wt, v, NULL, ct, &gr, &r, &c, -1, fcol, -1, -1, 0, 0, 0, temp_text, 0, 0, -1, G_BLACK);
	done(OS_SELECTED);
}

/*
 * Draw a boxtext object
 */
static void _cdecl
d_g_boxtext(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	struct theme *theme = wt->objcr_theme;
	struct object_theme *obt = &theme->boxtext;
	struct color_theme *ct;
	bool selected, disabled;
	short fl3d, fcol;
	GRECT r = wt->r, gr;
	OBJECT *ob = wt->current.ob;
	BFOBSPEC c;
	TEDINFO *ted;
	struct objc_edit_info *ei;
	char temp_text[256];

	selected = (ob->ob_state & OS_SELECTED);

	fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;

	if ((disabled = (ob->ob_state & OS_DISABLED)))
	{
		ct = selected ? &obt->dis.s[fl3d] : &obt->dis.n[fl3d];
	}
	else
	{
		ct = selected ? &obt->norm.s[fl3d] : &obt->norm.n[fl3d];
	}

	get_tedinformation(ob, &ted, &c, &ei);
	draw_g_box(wt, v, ct, &c, DRAW_ALL|DRAW_TEXTURE|ONLY_TEXTURE, &gr);
	(*v->api->t_effects)(v, ct->fnt.effects);
	set_obtext(ted, v, &gr, NULL, false, -1, temp_text, NULL, gr);

	if (!disabled)
	{
		if (selected)
		{
			if (fl3d == ACT)
			{
				gr.g_x += PUSH3D_DISTANCE;
				gr.g_y += PUSH3D_DISTANCE;
				fcol = c.textcol;
			}
			else
				fcol = selected3D_colour[c.textcol];
		}
		else
			fcol = c.textcol;
	}
	else
		fcol = -1;

	c.textmode = 0;
	ob_text(wt, v, ei, ct, &gr, &r, disabled ? NULL : &c, -1, fcol, -1, -1, 0, 0, 0, temp_text, ob->ob_state, 0, -1, G_BLACK);
	done(OS_SELECTED);
}

static void _cdecl
d_g_fboxtext(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	struct theme *theme = wt->objcr_theme;
	struct object_theme *obt = &theme->boxtext;
	struct color_theme *ct;
	char temp_text[256];
	GRECT r = wt->r;
	OBJECT *ob = wt->current.ob;
	TEDINFO *ted;
	GRECT gr;
	BFOBSPEC c;
	struct objc_edit_info *ei;
	bool selected, disabled;
	short fl3d, f_fg, f_bg;

	selected = (ob->ob_state & OS_SELECTED);

	fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;

	if ((disabled = (ob->ob_state & OS_DISABLED)))
	{
		ct = selected ? &obt->dis.s[fl3d] : &obt->dis.n[fl3d];
	}
	else
	{
		ct = selected ? &obt->norm.s[fl3d] : &obt->norm.n[fl3d];
	}

	get_tedinformation(ob, &ted, &c, &ei);
	if (!ei)
		ei = &wt->e;
	if (edit_ob(ei) != wt->current.ob)
		ei = NULL;

	if (fl3d && (c.fillpattern == IP_HOLLOW && !c.interiorcol && c.textcol == G_BLACK))
	{
		draw_g_box(wt, v, ct, NULL, DRAW_ALL, &gr);
		(*v->api->t_effects)(v, ct->fnt.effects);
		set_obtext(ted, v, &gr, NULL, true, -1, temp_text, NULL, gr);
		ob_text(wt, v, ei, ct, &gr, &r, NULL, -1, -1, -1, -1, G_WHITE, G_LBLUE, G_BLUE, temp_text, ob->ob_state, 0, -1, G_BLACK);
		done(OS_SELECTED|OS_DISABLED);
	}
	else
	{
		draw_g_box(wt, v, ct, &c, DRAW_ALL, &gr);
		(*v->api->t_effects)(v, ct->fnt.effects);
		set_obtext(ted, v, &gr, NULL, true, -1, temp_text, NULL, gr);

		if (selected)
		{
			if (fl3d == ACT)
			{
				gr.g_x += PUSH3D_DISTANCE;
				gr.g_y += PUSH3D_DISTANCE;
				f_fg = c.textcol;
				f_bg = G_WHITE;
			}
			else
			{
				f_fg = selected3D_colour[c.textcol];
				f_bg = c.textcol;
			}
		}
		else
		{
			f_fg = c.textcol;
			f_bg = G_WHITE;
		}

		ob_text(wt, v, ei, ct, &gr, &r, &c, -1, f_fg, f_bg, -1, G_WHITE, G_LBLUE, G_BLUE, temp_text, ob->ob_state, 0, -1, G_BLACK);
		done(OS_SELECTED|OS_DISABLED);
	}
}

/*
 * Draw a button object
 */
/*
 * ---------------------------------------------------------------------
 * APJ-OS Fluent
 *
 * apj_theme_set()/reset() are the receiving end of appl_control 111/112.
 * While a theme is loaded (apj_active) the Fluent drawers below take
 * over from the stock ones for the object types they cover; with none
 * loaded this renderer draws exactly like render_obj.c.
 * ---------------------------------------------------------------------
 */


/*
 * The object theme (struct theme) is data too: one shared instance for
 * every client of this module (current_theme), holding per object type
 * a fill, box, 3D-edge and font colour for normal/selected/highlighted
 * x enabled/disabled x four 3D variants. The Fluent look for menus,
 * dialog boxes, text fields and labels is therefore a transform of that
 * data onto the APJ pens, applied to the live instance. apj_stock_theme
 * is the pristine copy (textures resolved) a theme drop restores from.
 */

static struct theme apj_stock_theme;
static short apj_have_stock = 0;

static void
apj_flat_ct(struct color_theme *ct, short fill, short fg, short border)
{
	ct->col.flags &= ~(WCOL_DRAW3D|WCOL_ACT3D|WCOL_GRADIENT|WCOL_DRAWTEXTURE|WCOL_ONLYTEXTURE|WCOL_REV3D|WCOL_BOXBF3D|WCOL_BOXRND);
	ct->col.flags |= WCOL_DRAWBKG;
	ct->col.wr_mode = MD_REPLACE;
	ct->col.c = fill;
	ct->col.i = FIS_SOLID;
	ct->col.f = 8;
	ct->col.box_c = border;
	ct->col.box_th = 1;
	ct->col.top = ct->col.bottom = ct->col.left = ct->col.right = border;
	/* texture pointer left alone - owned and refcounted by the module */
#if WITH_GRADIENTS
	ct->col.gradient = NULL;
#endif
	ct->fnt.flags &= ~(WTXT_DRAW3D|WTXT_ACT3D);
	ct->fnt.effects = 0;
	ct->fnt.fg = fg;
	ct->fnt.bg = fg;
	ct->fnt.bannercol = fill;
	ct->fnt.x_3dact = 0;
	ct->fnt.y_3dact = 0;
}

/* one object type: normal / selected / highlighted, enabled and disabled,
 * all four 3D variants get the same flat treatment */
static void
apj_flat_obt(struct object_theme *ot, short fill, short sel_fill, short fg, short sel_fg, short border)
{
	int i;

	for (i = 0; i < 4; i++)
	{
		apj_flat_ct(&ot->norm.n[i], fill,     fg,     border);
		apj_flat_ct(&ot->norm.s[i], sel_fill, sel_fg, border);
		apj_flat_ct(&ot->norm.h[i], sel_fill, sel_fg, border);
		apj_flat_ct(&ot->dis.n[i],  fill,     APJ_PEN(APJ_R_DISABLED), border);
		apj_flat_ct(&ot->dis.s[i],  sel_fill, APJ_PEN(APJ_R_DISABLED), border);
		apj_flat_ct(&ot->dis.h[i],  sel_fill, APJ_PEN(APJ_R_DISABLED), border);
	}
	ot->norm.thick_3d = ot->norm.thick_3dact = 0;
	ot->dis.thick_3d  = ot->dis.thick_3dact  = 0;
}

static void
apj_flatten_theme(struct theme *t)
{
	short panel  = APJ_PEN(APJ_R_PANEL);
	short paper  = APJ_PEN(APJ_R_PAPER);
	short text   = APJ_PEN(APJ_R_TEXT);
	short border = APJ_PEN(APJ_R_BORDER);
	short press  = APJ_PEN(APJ_R_PRESSED);
	short selbg  = APJ_PEN(APJ_R_SELBG);
	short selfg  = APJ_PEN(APJ_R_SELFG);

	t->shadow_col = APJ_PEN(APJ_R_ELEVATION);
	t->ad3dval_x = t->ad3dval_y = 0;

	t->outline.flags &= ~(WCOL_DRAW3D|WCOL_ACT3D|WCOL_GRADIENT);
	t->outline.c = border;
	t->outline.box_c = border;
	t->outline.top = t->outline.bottom = t->outline.left = t->outline.right = border;

	/* push buttons have their own drawer; this covers the data path */
	apj_flat_obt(&t->button,     APJ_PEN(APJ_R_FACE), press, text, text, border);

	/* dialog surface and labels */
	apj_flat_obt(&t->box,        panel, panel, text, text, border);
	apj_flat_obt(&t->string,     panel, selbg, text, selfg, border);
	apj_flat_obt(&t->groupframe, panel, panel, text, text, border);

	/* text and edit fields: paper white, thin border */
	apj_flat_obt(&t->text,       paper, selbg, text, selfg, border);
	apj_flat_obt(&t->ed_text,    paper, selbg, text, selfg, border);
	apj_flat_obt(&t->boxtext,    paper, selbg, text, selfg, border);
	apj_flat_obt(&t->ed_boxtext, paper, selbg, text, selfg, border);

	/* menus: bar and titles on the panel, a pressed-grey highlight;
	 * drop-down and popup entries on paper with the same grey highlight -
	 * Win11 does not paint menu selection in the accent colour */
	apj_flat_obt(&t->menubar,    panel, panel, text, text, border);
	apj_flat_obt(&t->title,      panel, press, text, text, border);
	apj_flat_obt(&t->pu_string,  paper, press, text, text, border);
	apj_flat_obt(&t->popupbkg,   paper, paper, text, text, border);

	/* extobj (checkbox / radio glyphs) come from the resource: untouched */
}

static void
apj_restore_theme(struct theme *t)
{
	struct xa_data_hdr h = t->h;

	if (!apj_have_stock)
		return;
	*t = apj_stock_theme;
	t->h = h;
}

/* opcode 113: every role is in - apply to the live object theme */
short
apj_theme_commit(void)
{
	if (!apj_active)
		return 0;
	if (current_theme)
		apj_flatten_theme(current_theme);
	return 1;
}

short
apj_theme_set(long val)
{
	short role = (short) ((val >> 24) & 0xff);
	struct xa_vdi_settings *vs = api->C->Aes->vdi_settings;
	short rgb[3];

	if (role < 0 || role >= APJ_R_N || screen->colours < 16 || !vs)
		return 0;

	rgb[0] = (short) (((val >> 16) & 0xff) * 1000L / 255L);
	rgb[1] = (short) (((val >>  8) & 0xff) * 1000L / 255L);
	rgb[2] = (short) (( val        & 0xff) * 1000L / 255L);

	apj_rgb[role].red   = rgb[0];
	apj_rgb[role].green = rgb[1];
	apj_rgb[role].blue  = rgb[2];

	vs_color(vs->handle, APJ_PEN(role), rgb);

	apj_fgpen_cached = -1;		/* pen colours changed: re-probe */
	apj_active = 1;
	return 1;
}

short
apj_theme_reset(void)
{
	apj_active = 0;
	if (current_theme)
		apj_restore_theme(current_theme);
	return 1;
}

short
apj_theme_active(void)
{
	return apj_active;
}

/* opcode 115: hand the theme's role RGBs to a client, 0xRRGGBB each */
short
apj_theme_query(long *rgb)
{
	int i;

	if (!rgb || !apj_active)
		return 0;
	for (i = 0; i < APJ_R_N; i++)
		rgb[i] = ((long) (apj_rgb[i].red   * 255L / 1000L) << 16) |
		         ((long) (apj_rgb[i].green * 255L / 1000L) << 8) |
		          (long) (apj_rgb[i].blue  * 255L / 1000L);
	return APJ_R_N;
}

/*
 * ---------------------------------------------------------------------
 * Antialiased text
 *
 * Route: build-time glyph atlas + software blend, entirely inside this
 * renderer. mkatlas.py renders DejaVu Sans Mono into 8-bit coverage
 * maps, one glyph per system-font cell (8x16, 10x20, 12x24, 16x32), so
 * the advance is exactly the bitmap font's and legacy dialog layout is
 * untouched ("same advance, or no AA"). At draw time the destination
 * rectangle is read back from the screen, the glyphs are blended over
 * it in the screen's own pixel format, and the result is blitted back.
 * Nothing in fVDI or the emulator is involved, and legacy clients on
 * render_obj never see it.
 *
 * Applies only at 32 bpp; anywhere it cannot apply (other depths, a
 * cell size with no atlas, non-ASCII, off-screen) it falls back to
 * v_gtext, so it is never the reason text goes missing.
 * ---------------------------------------------------------------------
 */


/*
 * The advance (cell width) must match exactly - that is the layout
 * contract. The height only has to be close: vqt_extent's box for a
 * bitmap font can differ from the nominal cell by a line or two, so
 * take the atlas whose height is nearest, and blend that many rows.
 */
static const struct apj_atlas *
apj_atlas_for(short cw, short ch)
{
	const struct apj_atlas *best = NULL;
	short bestd = 4;	/* tolerate up to 3 rows of difference */
	int i;

	for (i = 0; i < APJ_N_ATLASES; i++)
	{
		short d;

		if (apj_atlases[i].cw != cw)
			continue;
		d = apj_atlases[i].ch - ch;
		if (d < 0)
			d = -d;
		if (d < bestd)
		{
			bestd = d;
			best = &apj_atlases[i];
		}
	}
	return best;
}

/*
 * The screen-format pixel for a pen, found empirically: save one on-
 * screen pixel, draw a 1x1 bar in the pen, read it back, put the saved
 * pixel back. Done under a full-screen clip so a partial redraw cannot
 * fool it, and cached per pen. This works whatever the pixel layout -
 * XaAES's own detect_pixel_format() gives up (-1) on layouts it does
 * not know, and create_gradient() then produces nothing, which is why
 * the first version of this fell back silently.
 */
static int
apj_fg_pixel(struct xa_vdi_settings *v, short pen, short x, short y)
{
	unsigned long saved = 0, probe = 0;
	MFDB mone, mscr;
	GRECT clip;
	short pxy[8];

	if (pen == apj_fgpen_cached)
		return 1;

	mone.fd_addr = &probe;
	mone.fd_w = 16;
	mone.fd_h = 1;
	mone.fd_wdwidth = 1;
	mone.fd_stand = 0;
	mone.fd_nplanes = 32;
	mone.fd_r1 = mone.fd_r2 = mone.fd_r3 = 0;
	mscr.fd_addr = NULL;

	(*v->api->save_clip)(v, &clip);
	(*v->api->set_clip)(v, &screen->r);

	pxy[0] = x; pxy[1] = y; pxy[2] = x; pxy[3] = y;
	pxy[4] = 0; pxy[5] = 0; pxy[6] = 0; pxy[7] = 0;
	mone.fd_addr = &saved;
	vro_cpyfm(v->handle, S_ONLY, pxy, &mscr, &mone);	/* save */

	(*v->api->wr_mode)(v, MD_REPLACE);
	(*v->api->f_interior)(v, FIS_SOLID);
	(*v->api->f_color)(v, pen);
	(*v->api->bar)(v, 0, x, y, 1, 1);

	mone.fd_addr = &probe;
	vro_cpyfm(v->handle, S_ONLY, pxy, &mscr, &mone);	/* read the pen's pixel */

	pxy[0] = 0; pxy[1] = 0; pxy[2] = 0; pxy[3] = 0;
	pxy[4] = x; pxy[5] = y; pxy[6] = x; pxy[7] = y;
	mone.fd_addr = &saved;
	vro_cpyfm(v->handle, S_ONLY, pxy, &mone, &mscr);	/* restore */

	(*v->api->restore_clip)(v, &clip);

	apj_fgpx[0] = ((unsigned char *) &probe)[0];
	apj_fgpx[1] = ((unsigned char *) &probe)[1];
	apj_fgpx[2] = ((unsigned char *) &probe)[2];
	apj_fgpx[3] = ((unsigned char *) &probe)[3];
	apj_fgpen_cached = pen;
	return 1;
}

/*
 * Draw t with its top-left cell corner at (x, y) in pen fg. Returns 0
 * (nothing drawn) when the caller should use v_gtext instead.
 */
static int apj_gtext_cell(struct xa_vdi_settings *v, short x, short y, short fg, short cw, short ch, const char *t);

static int
apj_gtext(struct xa_vdi_settings *v, short x, short y, short fg, const char *t)
{
	short cw, ch;

	if (!apj_active || !t || !*t)
		return 0;
	(*v->api->t_extent)(v, "M", &cw, &ch);
	return apj_gtext_cell(v, x, y, fg, cw, ch, t);
}

static int
apj_gtext_cell(struct xa_vdi_settings *v, short x, short y, short fg, short cw, short ch, const char *t)
{
	const struct apj_atlas *at;
	short w, h, fdw, n, i, yy, xx;
	short pxy[8];
	long size;
	MFDB msrc, mscr;
	static short logged = 0;

	if (!apj_active || !t || !*t)
		return 0;

	if (screen->planes != 32)
	{
		if (!logged++)
			BLOG((0, "apj_gtext: no AA - screen is %d planes (need 32), pixel_fmt %d", screen->planes, screen->pixel_fmt));
		return 0;
	}

	/* strlen is the kernel-entry vector here; an open-coded loop gets
	 * pattern-matched by gcc into a call to a libc strlen that a -nostdlib
	 * module does not have */
	n = (short) strlen(t);

	if (!(at = apj_atlas_for(cw, ch)))
	{
		if (!logged++)
			BLOG((0, "apj_gtext: no AA - font cell %dx%d has no atlas (have widths 8-14,16 at 2:1); c_max %dx%d", cw, ch, screen->c_max_w, screen->c_max_h));
		return 0;
	}

	h = at->ch;
	ch = at->ch;

	/* Vertically the cell must be on screen. Horizontally, trim the
	 * string to the screen: a directory line is composed to its full
	 * width whatever the window shows, and windows hang off the right
	 * edge all the time. */
	if (y < screen->r.g_y || y + h > screen->r.g_y + screen->r.g_h)
	{
		if (!logged++)
			BLOG((0, "apj_gtext: no AA - text row %d h %d off screen", y, h));
		return 0;
	}
	if (x < screen->r.g_x)
	{
		short skip = (screen->r.g_x - x + cw - 1) / cw;

		if (skip >= n)
			return 1;			/* nothing visible: drawn, trivially */
		t += skip;
		n -= skip;
		x += skip * cw;
	}
	{
		short room = (screen->r.g_x + screen->r.g_w - x) / cw;

		if (room <= 0)
			return 1;
		if (n > room)
			n = room;
	}
	w = n * cw;

	if (!apj_fg_pixel(v, fg, x, y))
		return 0;

	fdw = (w + 15) & ~15;
	size = (long) fdw * h * 4;
	if (size > apj_tbuf_size)
	{
		if (apj_tbuf)
			(*api->kfree)(apj_tbuf);
		apj_tbuf = (*api->kmalloc)(size);
		apj_tbuf_size = apj_tbuf ? size : 0;
		if (!apj_tbuf)
			return 0;
	}

	msrc.fd_addr = apj_tbuf;
	msrc.fd_w = fdw;
	msrc.fd_h = h;
	msrc.fd_wdwidth = fdw >> 4;
	msrc.fd_stand = 0;
	msrc.fd_nplanes = 32;
	msrc.fd_r1 = msrc.fd_r2 = msrc.fd_r3 = 0;
	mscr.fd_addr = NULL;

	/* screen -> buffer */
	pxy[0] = x; pxy[1] = y; pxy[2] = x + w - 1; pxy[3] = y + h - 1;
	pxy[4] = 0; pxy[5] = 0; pxy[6] = w - 1;     pxy[7] = h - 1;
	vro_cpyfm(v->handle, S_ONLY, pxy, &mscr, &msrc);

	/* blend the glyphs in; a glyph the atlas lacks is left for v_gtext below */
	for (i = 0; i < n; i++)
	{
		short slot = apj_atlas_slot((unsigned char) t[i]);
		const unsigned char *cov;
		unsigned char *col = (unsigned char *) apj_tbuf + (long) i * cw * 4;

		if (slot < 0)
			continue;
		cov = at->cov + (long) slot * ch * cw;

		for (yy = 0; yy < ch; yy++)
		{
			unsigned char *p = col + (long) yy * fdw * 4;

			for (xx = 0; xx < cw; xx++, p += 4)
			{
				unsigned short a = *cov++;

				if (a == 0)
					continue;
				if (a == 255)
				{
					p[0] = apj_fgpx[0]; p[1] = apj_fgpx[1];
					p[2] = apj_fgpx[2]; p[3] = apj_fgpx[3];
					continue;
				}
				p[0] += (unsigned char) (((short) apj_fgpx[0] - p[0]) * a >> 8);
				p[1] += (unsigned char) (((short) apj_fgpx[1] - p[1]) * a >> 8);
				p[2] += (unsigned char) (((short) apj_fgpx[2] - p[2]) * a >> 8);
				p[3] += (unsigned char) (((short) apj_fgpx[3] - p[3]) * a >> 8);
			}
		}
	}

	/* buffer -> screen (the VDI clip applies here) */
	pxy[0] = 0; pxy[1] = 0; pxy[2] = w - 1;     pxy[3] = h - 1;
	pxy[4] = x; pxy[5] = y; pxy[6] = x + w - 1; pxy[7] = y + h - 1;
	vro_cpyfm(v->handle, S_ONLY, pxy, &msrc, &mscr);

	/* glyphs the atlas lacks: the bitmap font, one cell at a time */
	for (i = 0; i < n; i++)
	{
		if (apj_atlas_slot((unsigned char) t[i]) < 0)
		{
			char one[2];

			one[0] = t[i];
			one[1] = '\0';
			(*v->api->wr_mode)(v, MD_TRANS);
			(*v->api->t_color)(v, fg);
			v_gtext(v->handle, x + i * cw, y - v->dists[5], one);
		}
	}

	return 1;
}

/*
 * Window chrome text (title, info line) - the same placement rules as
 * xa_wtxt_output() in xa_vdi.c, drawn through the atlas. Returns 0 when
 * the caller should fall back to the stock output.
 */
int
apj_wtxt_output(struct xa_vdi_settings *v, struct xa_wtxt_inf *wtxti, char *txt, short state, const GRECT *r, short xoff, short yoff)
{
	struct xa_fnt_info *wtxt = (state & OS_SELECTED) ? &wtxti->selected : &wtxti->normal;
	short x, y, w, h, f = wtxti->flags;
	char t[200];

	if (!apj_active || !txt)
		return 0;

	(*v->api->wr_mode)(v, MD_TRANS);
	(*v->api->t_font)(v, wtxt->font_point, wtxt->font_id);
	(*v->api->t_effects)(v, 0);

	if (f & WTXT_NOCLIP)
	{
		strncpy(t, txt, sizeof(t) - 1);
		t[sizeof(t) - 1] = '\0';
	}
	else
		(*v->api->prop_clipped_name)(v, txt, t, r->g_w - (xoff << 1), &w, &h, 1);

	(*v->api->t_extent)(v, t, &w, &h);
	y = yoff + r->g_y + ((r->g_h - h) >> 1);

	if (f & WTXT_CENTER)
	{
		x = r->g_x + ((r->g_w - w) >> 1);
		if (x < (r->g_x + xoff))
			x = r->g_x + xoff;
	}
	else
		x = xoff + r->g_x;

	return apj_gtext(v, x, y, wtxt->fg, t);
}

/*
 * Opcode 114. Draws on the AES workstation under the caller's clip, with
 * the mouse hidden as any AES draw is. The caller holds the update lock
 * (it is inside its own redraw), so nothing else is painting.
 */
short
apj_text_request(struct xa_client *client, struct apj_textreq *rq)
{
	struct xa_vdi_settings *v = api->C->Aes->vdi_settings;
	GRECT clip, saved;
	short ret;

	if (!client || !rq || !rq->s || !apj_active || !v)
		return 0;
	if (rq->cw <= 0 || rq->ch <= 0 || rq->clip[2] < rq->clip[0] || rq->clip[3] < rq->clip[1])
		return 0;

	clip.g_x = rq->clip[0];
	clip.g_y = rq->clip[1];
	clip.g_w = rq->clip[2] - rq->clip[0] + 1;
	clip.g_h = rq->clip[3] - rq->clip[1] + 1;

	/* No hidem()/showm() here. The caller is inside its own redraw with
	 * the mouse already off (graf_mouse M_OFF), and an extra hide/show
	 * pair from the AES side moved the pointer as seen by the VDI's
	 * motion vector - the desktop's hover tooltips closed the instant
	 * they were drawn. The client owns the mouse state; we just draw. */
	(*v->api->save_clip)(v, &saved);
	(*v->api->set_clip)(v, &clip);
	ret = apj_gtext_cell(v, rq->x, rq->y, rq->pen, rq->cw, rq->ch, rq->s) ? 1 : 0;
	(*v->api->restore_clip)(v, &saved);

	return ret;
}

/*
 * ---------------------------------------------------------------------
 * Icon replacements
 *
 * apjicons-<size>.bin (built by apj-os-tools/icons/mkicons.py) holds a
 * 32-bit RGBA image per icon, keyed by an FNV-1a hash of the icon's mono
 * mask + data as stored in the resource - the one part of a CICONBLK
 * that survives TeraDesk copying icon blocks per desktop item. Loaded
 * from the AES home directory on first use while a theme is active.
 * Drawn through the same read-back / blend / blit path the text uses,
 * so alpha edges land correctly on any background; a selected icon gets
 * a translucent accent tint, a disabled one half alpha.
 *
 * File: "APJI", u16 version, u16 count, u16 size, u16 0; then count x
 * { u32 hash, u16 w, u16 h, u32 offset }, then RGBA data.
 * ---------------------------------------------------------------------
 */

struct apj_icon
{
	unsigned long hash;
	short w, h;
	unsigned char *rgba;		/* w*h*4, straight alpha, R G B A byte order */
};

static struct apj_icon *apj_icons = NULL;
static short apj_nicons = 0;
static char *apj_icon_blocks[3];		/* one data block per size file loaded */
static short apj_icon_nblocks = 0;
static short apj_icons_tried = 0;
static short apj_chan[4] = { -1, -1, -1, -1 };	/* screen byte index of R, G, B; [3] = the spare */

static unsigned long
apj_icon_hash(const unsigned char *mask, const unsigned char *data, long n)
{
	unsigned long h = 0x811C9DC5UL;
	long i;

	for (i = 0; i < n; i++)
		h = ((h ^ mask[i]) * 0x01000193UL) & 0xFFFFFFFFUL;
	for (i = 0; i < n; i++)
		h = ((h ^ data[i]) * 0x01000193UL) & 0xFFFFFFFFUL;
	return h;
}

/*
 * Which byte of a screen pixel carries which channel: set a scratch pen
 * to pure red, green, blue in turn and probe (apj_fg_pixel already knows
 * how to read a pen's pixel bytes back).
 */
static int
apj_icon_channels(struct xa_vdi_settings *v, short x, short y)
{
	static const short pure[3][3] = { {1000, 0, 0}, {0, 1000, 0}, {0, 0, 1000} };
	short scratch = APJ_PEN_BASE - 1;
	int c, i, used = 0;

	if (apj_chan[0] >= 0)
		return 1;
	for (c = 0; c < 3; c++)
	{
		vs_color(v->handle, scratch, (short *) pure[c]);
		apj_fgpen_cached = -1;
		if (!apj_fg_pixel(v, scratch, x, y))
			return 0;
		apj_chan[c] = -1;
		for (i = 0; i < 4; i++)
			if (apj_fgpx[i] >= 0xF0 && !(used & (1 << i)))
			{
				apj_chan[c] = i;
				used |= 1 << i;
				break;
			}
		if (apj_chan[c] < 0)
			return 0;
	}
	for (i = 0; i < 4; i++)
		if (!(used & (1 << i)))
			apj_chan[3] = i;
	apj_fgpen_cached = -1;
	return 1;
}

static void
apj_icons_load(void)
{
	char fn[200];
	struct file *fp;
	long err, size, got;
	unsigned char *d;
	unsigned short count, ver;
	int i;

	static const short sizes[] = { 32, 48, 64 };
	int sz;

	if (apj_icons_tried)
		return;
	apj_icons_tried = 1;

	/* every size present is loaded into one table: the hash is of the
	 * resource's mono mask+data, and a 48px resource icon can only ever
	 * match an entry made from the same 48px icon, so the right size
	 * picks itself */
	for (sz = 0; sz < 3; sz++)
	{
	sprintf(fn, sizeof(fn), "%sapjicons-%d.bin", api->C->Aes->home_path, sizes[sz]);
	fp = kernel_open(fn, O_RDONLY, &err, NULL);
	if (!fp)
	{
		BLOG((0, "apj icons: %s not found", fn));
		continue;
	}
	/* size: seek to the end */
	size = kernel_lseek(fp, 0, SEEK_END);
	kernel_lseek(fp, 0, SEEK_SET);
	if (size < 12 || size > 4000000L || !(d = (*api->kmalloc)(size)))
	{
		kernel_close(fp);
		continue;
	}
	for (got = 0; got < size; )
	{
		err = kernel_read(fp, d + got, size - got);
		if (err <= 0)
			break;
		got += err;
	}
	kernel_close(fp);
	if (got != size || d[0] != 'A' || d[1] != 'P' || d[2] != 'J' || d[3] != 'I')
	{
		(*api->kfree)(d);
		BLOG((0, "apj icons: bad file %s", fn));
		continue;
	}
	ver = (d[4] << 8) | d[5];
	count = (d[6] << 8) | d[7];
	if (ver != 1 || count == 0 || 12 + (long) count * 12 > size)
	{
		(*api->kfree)(d);
		continue;
	}
	{
		struct apj_icon *tab = (*api->kmalloc)((long) (apj_nicons + count) * sizeof(struct apj_icon));

		if (!tab)
		{
			(*api->kfree)(d);
			continue;
		}
		if (apj_icons)
		{
			for (i = 0; i < apj_nicons; i++)
				tab[i] = apj_icons[i];
			(*api->kfree)(apj_icons);
		}
		apj_icons = tab;
	}
	for (i = 0; i < count; i++)
	{
		const unsigned char *e = d + 12 + i * 12;
		unsigned long off = ((unsigned long) e[8] << 24) | ((unsigned long) e[9] << 16) | (e[10] << 8) | e[11];
		struct apj_icon *ic = &apj_icons[apj_nicons + i];

		ic->hash = ((unsigned long) e[0] << 24) | ((unsigned long) e[1] << 16) | (e[2] << 8) | e[3];
		ic->w = (e[4] << 8) | e[5];
		ic->h = (e[6] << 8) | e[7];
		ic->rgba = (off + (long) ic->w * ic->h * 4 <= size) ? d + off : NULL;
	}
	apj_icon_blocks[apj_icon_nblocks++] = (char *) d;
	apj_nicons += count;
	BLOG((0, "apj icons: %d loaded from %s (%d total)", count, fn, apj_nicons));
	}
}

static struct apj_icon *
apj_icon_find(ICONBLK *ib)
{
	long n = (long) ((ib->ib_wicon + 15) >> 4) * 2 * ib->ib_hicon;
	unsigned long h;
	int i;

	if (!apj_icons || !ib->ib_pmask || !ib->ib_pdata)
		return NULL;
	h = apj_icon_hash((unsigned char *) ib->ib_pmask, (unsigned char *) ib->ib_pdata, n);
	for (i = 0; i < apj_nicons; i++)
		if (apj_icons[i].hash == h && apj_icons[i].rgba)
			break;
	{
		static short logged = 0;

		if (logged < 12)
		{
			logged++;
			BLOG((0, "apj icon: %dx%d n=%ld mask=%lx data=%lx m0=%04x d0=%04x hash=%08lx -> %d (tab0=%08lx tab1=%08lx)",
				ib->ib_wicon, ib->ib_hicon, n, (unsigned long) ib->ib_pmask, (unsigned long) ib->ib_pdata,
				ib->ib_pmask ? *(unsigned short *) ib->ib_pmask : 0, ib->ib_pdata ? *(unsigned short *) ib->ib_pdata : 0,
				h, i < apj_nicons ? i : -1, apj_icons[0].hash, apj_nicons > 1 ? apj_icons[1].hash : 0L));
		}
	}
	/* A miss is written to u:\ram\apjicon.log (first 60), readable from the
	 * desktop with TosWin2. Crucially: scan the table again ignoring rgba,
	 * so we learn whether the hash was PRESENT (an rgba/size problem) or
	 * ABSENT (a load/hash problem), and print the matching entry's stored
	 * w/h/rgba so we can see exactly what the loader put there. */
	if (i >= apj_nicons)
	{
		static short misses = 0;

		if (misses < 60)
		{
			struct file *fp;
			long err;
			char line[200];
			int j, hit = -1;

			misses++;
			for (j = 0; j < apj_nicons; j++)
				if (apj_icons[j].hash == h)
				{
					hit = j;
					break;
				}
			fp = kernel_open("u:\\ram\\apjicon.log", O_WRONLY | O_CREAT | O_APPEND, &err, NULL);
			if (fp)
			{
				const char *nm = ib->ib_ptext ? ib->ib_ptext : "";

				if (hit >= 0)
					sprintf(line, sizeof(line), "miss %dx%d '%s' hash %08lx: in table at %d but SKIPPED - stored w=%d h=%d rgba=%lx (table %d)\r\n",
						ib->ib_wicon, ib->ib_hicon, nm, h, hit,
						apj_icons[hit].w, apj_icons[hit].h, (unsigned long) apj_icons[hit].rgba, apj_nicons);
				else
					sprintf(line, sizeof(line), "miss %dx%d '%s' hash %08lx: NOT in table at all (table %d)\r\n",
						ib->ib_wicon, ib->ib_hicon, nm, h, apj_nicons);
				kernel_write(fp, line, strlen(line));
				kernel_close(fp);
			}
		}
	}
	return i < apj_nicons ? &apj_icons[i] : NULL;
}

/*
 * Blend an icon at (x, y). sel = translucent accent tint over the icon's
 * own pixels, dis = half alpha. Returns 0 if it could not (caller draws
 * the resource icon instead).
 */
static int
apj_icon_draw(struct xa_vdi_settings *v, struct apj_icon *ic, short x, short y, short sel, short dis)
{
	short w = ic->w, h = ic->h, fdw, xx, yy;
	long size;
	short pxy[8];
	MFDB msrc, mscr;
	unsigned char selpx[4];
	const unsigned char *s;

	if (screen->planes != 32 || !apj_active)
		return 0;
	if (x < screen->r.g_x || y < screen->r.g_y ||
	    x + w > screen->r.g_x + screen->r.g_w || y + h > screen->r.g_y + screen->r.g_h)
		return 0;
	if (!apj_icon_channels(v, x, y))
		return 0;
	if (sel)
	{
		if (!apj_fg_pixel(v, APJ_PEN(APJ_R_SELBG), x, y))
			return 0;
		selpx[0] = apj_fgpx[0]; selpx[1] = apj_fgpx[1]; selpx[2] = apj_fgpx[2]; selpx[3] = apj_fgpx[3];
	}

	fdw = (w + 15) & ~15;
	size = (long) fdw * h * 4;
	if (size > apj_tbuf_size)
	{
		if (apj_tbuf)
			(*api->kfree)(apj_tbuf);
		apj_tbuf = (*api->kmalloc)(size);
		apj_tbuf_size = apj_tbuf ? size : 0;
		if (!apj_tbuf)
			return 0;
	}
	msrc.fd_addr = apj_tbuf;
	msrc.fd_w = fdw;
	msrc.fd_h = h;
	msrc.fd_wdwidth = fdw >> 4;
	msrc.fd_stand = 0;
	msrc.fd_nplanes = 32;
	msrc.fd_r1 = msrc.fd_r2 = msrc.fd_r3 = 0;
	mscr.fd_addr = NULL;

	pxy[0] = x; pxy[1] = y; pxy[2] = x + w - 1; pxy[3] = y + h - 1;
	pxy[4] = 0; pxy[5] = 0; pxy[6] = w - 1;     pxy[7] = h - 1;
	vro_cpyfm(v->handle, S_ONLY, pxy, &mscr, &msrc);

	s = ic->rgba;
	for (yy = 0; yy < h; yy++)
	{
		unsigned char *p = (unsigned char *) apj_tbuf + (long) yy * fdw * 4;

		for (xx = 0; xx < w; xx++, p += 4, s += 4)
		{
			unsigned short a = s[3];
			unsigned char src[4];

			if (dis)
				a >>= 1;
			if (a == 0)
				continue;
			src[apj_chan[0]] = s[0];
			src[apj_chan[1]] = s[1];
			src[apj_chan[2]] = s[2];
			src[apj_chan[3]] = p[apj_chan[3]];
			if (sel)
			{
				/* 40% towards the selection colour, on the icon's pixels */
				src[apj_chan[0]] += (unsigned char) (((short) selpx[apj_chan[0]] - src[apj_chan[0]]) * 102 >> 8);
				src[apj_chan[1]] += (unsigned char) (((short) selpx[apj_chan[1]] - src[apj_chan[1]]) * 102 >> 8);
				src[apj_chan[2]] += (unsigned char) (((short) selpx[apj_chan[2]] - src[apj_chan[2]]) * 102 >> 8);
			}
			if (a >= 255)
			{
				p[0] = src[0]; p[1] = src[1]; p[2] = src[2]; p[3] = src[3];
				continue;
			}
			p[0] += (unsigned char) (((short) src[0] - p[0]) * a >> 8);
			p[1] += (unsigned char) (((short) src[1] - p[1]) * a >> 8);
			p[2] += (unsigned char) (((short) src[2] - p[2]) * a >> 8);
			p[3] += (unsigned char) (((short) src[3] - p[3]) * a >> 8);
		}
	}

	pxy[0] = 0; pxy[1] = 0; pxy[2] = w - 1;     pxy[3] = h - 1;
	pxy[4] = x; pxy[5] = y; pxy[6] = x + w - 1; pxy[7] = y + h - 1;
	vro_cpyfm(v->handle, S_ONLY, pxy, &msrc, &mscr);
	return 1;
}

/*
 * A flat control: solid fill, 1px border. The border stops one pixel
 * short of each corner, which at this size reads as a rounded corner
 * without needing arcs - a real radius comes later with the RGB blit
 * path (the fill here is a plain v_bar in a theme pen, which is why it
 * is fast).
 */

static void
apj_flat_box(struct xa_vdi_settings *v, const GRECT *r, short fill, short border)
{
	short x1 = r->g_x;
	short y1 = r->g_y;
	short x2 = r->g_x + r->g_w - 1;
	short y2 = r->g_y + r->g_h - 1;
	short c = (r->g_w > 7 && r->g_h > 7) ? 2 : (r->g_w > 2 && r->g_h > 2) ? 1 : 0;

	(*v->api->wr_mode)(v, MD_REPLACE);
	(*v->api->f_interior)(v, FIS_SOLID);
	(*v->api->f_color)(v, fill);
	(*v->api->gbar)(v, 0, r);

	(*v->api->line)(v, x1 + c, y1, x2 - c, y1, border);
	(*v->api->line)(v, x1 + c, y2, x2 - c, y2, border);
	(*v->api->line)(v, x1, y1 + c, x1, y2 - c, border);
	(*v->api->line)(v, x2, y1 + c, x2, y2 - c, border);

	if (c == 2)
	{
		/* a 4px-radius look: one diagonal pixel in each corner */
		(*v->api->line)(v, x1 + 1, y1 + 1, x1 + 1, y1 + 1, border);
		(*v->api->line)(v, x2 - 1, y1 + 1, x2 - 1, y1 + 1, border);
		(*v->api->line)(v, x1 + 1, y2 - 1, x1 + 1, y2 - 1, border);
		(*v->api->line)(v, x2 - 1, y2 - 1, x2 - 1, y2 - 1, border);
	}
}

/*
 * Fluent checkbox: a square with a 1px border on paper; selected fills
 * it with the accent and draws a white tick. Sized to the resource glyph
 * it replaces so dialog layout is unchanged.
 */
static void
apj_checkbox(struct xa_vdi_settings *v, short x, short y, short w, short h, short selected, short disabled)
{
	GRECT r;
	short fill, border, mark;
	short sz = w < h ? w : h;

	r.g_x = x; r.g_y = y + ((h - sz) >> 1); r.g_w = sz; r.g_h = sz;

	if (disabled)
	{
		fill   = selected ? APJ_PEN(APJ_R_DISABLED) : APJ_PEN(APJ_R_PAPER);
		border = APJ_PEN(APJ_R_DISABLED);
		mark   = APJ_PEN(APJ_R_PAPER);
	}
	else if (selected)
	{
		fill = border = APJ_PEN(APJ_R_ACCENT);
		mark = APJ_PEN(APJ_R_SELFG);
	}
	else
	{
		fill   = APJ_PEN(APJ_R_PAPER);
		border = APJ_PEN(APJ_R_TEXT);
		mark   = fill;
	}

	apj_flat_box(v, &r, fill, border);

	if (selected)
	{
		/* tick: short stroke down-right, long stroke up-right, 2px wide */
		short x0 = r.g_x + (sz * 22) / 100;
		short y0 = r.g_y + (sz * 50) / 100;
		short x1 = r.g_x + (sz * 42) / 100;
		short y1 = r.g_y + (sz * 72) / 100;
		short x2 = r.g_x + (sz * 78) / 100;
		short y2 = r.g_y + (sz * 28) / 100;

		(*v->api->line)(v, x0, y0, x1, y1, mark);
		(*v->api->line)(v, x0, y0 + 1, x1, y1 + 1, mark);
		(*v->api->line)(v, x1, y1, x2, y2, mark);
		(*v->api->line)(v, x1, y1 + 1, x2, y2 + 1, mark);
	}
}

/*
 * Fluent radio: a 1px ring on paper; selected is an accent ring with a
 * white interior and an accent dot. Drawn with filled circles inside
 * one another - no arc primitives needed.
 */
static void
apj_circle(struct xa_vdi_settings *v, short cx, short cy, short rad, short col)
{
	if (rad < 0)
		return;
	(*v->api->f_interior)(v, FIS_SOLID);
	(*v->api->f_perimeter)(v, 0);
	(*v->api->f_color)(v, col);
	v_circle(v->handle, cx, cy, rad);
}

static void
apj_radio(struct xa_vdi_settings *v, short x, short y, short w, short h, short selected, short disabled)
{
	short sz = w < h ? w : h;
	short rad = (sz - 1) >> 1;
	short cx = x + rad;
	short cy = y + ((h - sz) >> 1) + rad;
	short ring, inner, dot;

	if (disabled)
	{
		ring  = APJ_PEN(APJ_R_DISABLED);
		inner = APJ_PEN(APJ_R_PAPER);
		dot   = APJ_PEN(APJ_R_DISABLED);
	}
	else if (selected)
	{
		ring  = APJ_PEN(APJ_R_ACCENT);
		inner = APJ_PEN(APJ_R_PAPER);
		dot   = APJ_PEN(APJ_R_ACCENT);
	}
	else
	{
		ring  = APJ_PEN(APJ_R_TEXT);
		inner = APJ_PEN(APJ_R_PAPER);
		dot   = inner;
	}

	(*v->api->wr_mode)(v, MD_REPLACE);
	apj_circle(v, cx, cy, rad, ring);
	apj_circle(v, cx, cy, rad - 1, inner);
	if (selected)
		apj_circle(v, cx, cy, rad - 4, dot);
}

/*
 * Fluent G_BUTTON. Replaces draw_objc_bkg() + the stock text pass for a
 * plain push button (the checkbox / radio / group-frame reinterpretations
 * still go the stock way for now).
 *
 *   normal    face fill, border, text
 *   selected  pressed fill (the button is down)
 *   default   accent fill, selection-text colour - Win11's accent button
 *   disabled  face fill, disabled text; no stipple
 *
 * ob_text() is given fg AND bg explicitly: with fg alone it looks bg up
 * in a 16-entry 3D table, and our pens are far past 15.
 */

static void
apj_button(struct widget_tree *wt, struct xa_vdi_settings *v, struct color_theme *ct,
	   GRECT *r, GRECT *gr, char *text, short und)
{
	OBJECT *ob = wt->current.ob;
	ushort state = ob->ob_state;
	short fill, fg;
	struct color_theme lct = *ct;

	if (state & OS_DISABLED)
	{
		fill = APJ_PEN(APJ_R_FACE);
		fg   = APJ_PEN(APJ_R_DISABLED);
	}
	else if (state & OS_SELECTED)
	{
		fill = (ob->ob_flags & OF_DEFAULT) ? APJ_PEN(APJ_R_SELBG) : APJ_PEN(APJ_R_PRESSED);
		fg   = (ob->ob_flags & OF_DEFAULT) ? APJ_PEN(APJ_R_SELFG) : APJ_PEN(APJ_R_TEXT);
	}
	else if (ob->ob_flags & OF_DEFAULT)
	{
		fill = APJ_PEN(APJ_R_ACCENT);
		fg   = APJ_PEN(APJ_R_SELFG);
	}
	else
	{
		fill = APJ_PEN(APJ_R_FACE);
		fg   = APJ_PEN(APJ_R_TEXT);
	}

	apj_flat_box(v, r, fill, APJ_PEN(APJ_R_BORDER));

	if (text)
	{
		/* flat text: no 3D relief, no engraving on disabled */
		lct.fnt.flags &= ~WTXT_DRAW3D;
		lct.fnt.effects = 0;

		(*v->api->wr_mode)(v, MD_TRANS);
		ob_text(wt, v, NULL, &lct, gr, r, NULL, -1, fg, fg, -1, 0,0,0, text,
			state & ~OS_DISABLED, 0, und, fg);
	}

	done(OS_SELECTED|OS_DISABLED);
}

static void _cdecl
d_g_button(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	struct theme *theme = wt->objcr_theme;
	struct object_theme *obt;
	struct color_theme *ct;
	GRECT r = wt->r, gr = r;
	OBJECT *ob = wt->current.ob;
	short thick = obj_thickness(wt, ob), d3t, d, fl3d;
	ushort selected = ob->ob_state & OS_SELECTED;
	char *text = NULL;
	char *textpatch = NULL;

	fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;

	if (ob->ob_type == G_POPUP)
	{
		POPINFO *pi = (*api->object_get_popinfo)(ob);
		if (pi->obnum > 0)
			text = (*api->object_get_spec)(pi->tree + pi->obnum)->free_string + 2;
	}
	else if (ob->ob_type == G_SWBUTTON)
	{
		SWINFO *si = (*api->object_get_swinfo)(ob);
		short pos = si->num;
		text = si->string;
		while (pos && *text)
		{
			if (*text == '|')
			{
				pos--;
			}
			text++;
		}
		textpatch = text;
		while (*textpatch != '|' && *textpatch != '\0')
			textpatch++;
		if (*textpatch == '|')
			*textpatch = '\0';
		else
			textpatch = NULL;
	}
	else
		text = (*api->object_get_spec)(ob)->free_string;

	/* OS_WHITEBAK + bit15 reinterprets a G_BUTTON as a checkbox, radio
	 * button or group frame. Suppress that only for extended type 18
	 * ("Exitbutton"), a real button that happens to carry these bits; the
	 * reinterpreted objects keep their shape even when OF_EXIT is set. */
	if ((ob->ob_state & OS_WHITEBAK) && (ob->ob_state & 0x8000) && ((ob->ob_type >> 8) & 0xff) != 18)
	{
		short und = (short)ob->ob_state >> 8;

		(*v->api->wr_mode)(v, MD_REPLACE);

		/* group frame */
		if (und == -2)
		{
			short tlc, brc;
			GRECT rr = r;

			obt = &theme->groupframe;
			ct = &obt->norm.n[fl3d];

			d = 0;
			if (fl3d == ACT || fl3d == IND)
				d += 2;

			if (text && *text)
			{
				(*v->api->t_color)(v, ct->fnt.fg);
				(*v->api->t_effects)(v, ct->fnt.effects);
				if (ob->ob_state & OS_SHADOWED)
					(*v->api->t_font)(v, screen->small_font_point, screen->small_font_id);
				else
					(*v->api->t_font)(v, screen->standard_font_point, screen->standard_font_id);

				(*v->api->t_extent)(v, text, &gr.g_w, &gr.g_h);
				rr.g_y += gr.g_h / 2;
				rr.g_h -= gr.g_h / 2;
			}
			if ((fl3d & 2)) /* BKG or ACT */
			{
				tlc = ct->col.left;
				brc = ct->col.right;
			}
			else
			{
				tlc = G_BLACK;
				brc = -1;
			}

			if (text && *text)
			{
				gr.g_x = r.g_x + screen->c_max_w;
				gr.g_y = r.g_y;
				chiseled_gbox(v, d, tlc, brc, &rr, &gr);
				ob_text(wt, v, NULL, ct, &gr, NULL, NULL, -1, -1, -1, -1, 0,0,0, text, ob->ob_state, ob->ob_flags, -1, G_BLACK);
			}
			else
				chiseled_gbox(v, d, tlc, brc, &rr, NULL);
			done(OS_CHECKED|OS_DISABLED|OS_SHADOWED);
		}
		else
		{
			short w, h;
			struct xa_aes_object xobj;
			struct widget_tree b;

			obt = &theme->extobj;
			if (ob->ob_state & OS_DISABLED)
			{
				ct = selected ? &obt->dis.s[fl3d] : &obt->dis.n[fl3d];
			}
			else
			{
				ct = selected ? &obt->norm.s[fl3d] : &obt->norm.n[fl3d];
			}

			b.owner = wt->owner;
			b.tree = xobj_rsc;
			b.objcr_api = wt->objcr_api;
			b.objcr_theme = theme;
			xobj = aesobj(xobj_rsc, (ob->ob_flags & OF_RBUTTON) ? (selected ? XOBJ_R_SEL : XOBJ_R_DSEL) : (selected ? XOBJ_B_SEL : XOBJ_B_DSEL));
			(*api->object_spec_wh)(aesobj_ob(&xobj), &w, &h);
			if (gr.g_h != h)
				gr.g_y += ((gr.g_h - h) >> 1);

			if (apj_active && !MONO)
			{
				short dis = (ob->ob_state & OS_DISABLED) ? 1 : 0;

				if (ob->ob_flags & OF_RBUTTON)
					apj_radio(v, gr.g_x, gr.g_y, w, h, selected ? 1 : 0, dis);
				else
					apj_checkbox(v, gr.g_x, gr.g_y, w, h, selected ? 1 : 0, dis);
			}
			else
				(*api->render_object)(&b, v, xobj, gr.g_x, gr.g_y);
			if (text)
			{
				short undcol;
				(*v->api->t_color)(v, ct->fnt.fg);
				r.g_x += (w + 2);
				(*v->api->wr_mode)(v, MD_TRANS);
				(*v->api->t_font)(v, screen->standard_font_point, screen->standard_font_id);
				(*v->api->t_effects)(v, ct->fnt.effects);

				switch ((ob->ob_flags & OF_FL3DMASK) >> 9)
				{
					case 1:	 undcol = G_GREEN; break;
					case 2:  undcol = G_BLUE; break;
					case 3:  undcol = G_RED;   break;
					default: undcol = G_BLACK; break;
				}
				ob_text(wt, v, NULL, ct, &r, &wt->r, NULL, -1, -1, -1, -1, 0,0,0, text, ob->ob_state, 0, und & 0x7f, undcol);
			}
		}
	}
	else
	{
		short und, tw, th;

		obt = &theme->button;

		if (ob->ob_state & OS_DISABLED)
			ct = selected ? &obt->dis.s[fl3d] : &obt->dis.n[fl3d];
		else
			ct = selected ? &obt->norm.s[fl3d] : &obt->norm.n[fl3d];

		und = (ob->ob_state & OS_WHITEBAK) ? ((ob->ob_state >> 8) & 0x7f) : -1;

		/* Use extent, NOT vst_alignment. Makes x and y to v_gtext
		 * real values that can be used for other stuff (like shortcut
		 * underlines :-)
		 */
		if (text)
		{
			short fi, fp;
			(*v->api->t_color)(v, ct->fnt.fg);
			(*v->api->t_effects)(v, ct->fnt.effects);
			fi = ct->fnt.font_id ? ct->fnt.font_id : screen->standard_font_id;
			fp = ct->fnt.font_point ? ct->fnt.font_point : screen->standard_font_point;
			(*v->api->t_font)(v, fp, fi);
			(*v->api->t_extent)(v, text, &tw, &th);
			gr.g_y += (r.g_h - th) / 2;
			gr.g_x += (r.g_w - tw) / 2;
		}

		d = 0;
		d3t = 0;
		if (fl3d)
			d3t--;
		if (fl3d & 1)
			d--;

		thick = 0;
		if (ob->ob_flags & OF_DEFAULT)
		{
			thick--;
			if (ob->ob_flags & OF_EXIT)
				thick--;
		}
		else if (ob->ob_flags & OF_EXIT)
			thick--;

		/* Use inward rendering for extended type 18 ("Exitbutton").
		 * Negative depths cause draw_objc_bkg to grow the bounding box
		 * outward before drawing the 3D border (AES 4.x behaviour) and
		 * makes the button bulge into neighbouring objects. Flip the depths
		 * positive so the border draws inside the resource-supplied
		 * rectangle (MagiC behaviour). Maybe to be extended to other objects? */
		if (((ob->ob_type >> 8) & 0xff) == 18)
		{
			d = -d;
			d3t = -d3t;
			thick = -thick;
		}

		if (apj_active && !MONO)
		{
			apj_button(wt, v, ct, &r, &gr, text, und);
		}
		else
		{
			draw_objc_bkg(wt, v, ct, NULL, DRAW_ALL | (fl3d ? DRAW_TEXTURE|ONLY_TEXTURE : 0), -1, -1, d, d3t, thick, 2, &r, &r, NULL);

			if (text)
			{
				if (selected)
				{
					gr.g_x += ct->fnt.x_3dact;
					gr.g_y += ct->fnt.y_3dact;
				}
				(*v->api->wr_mode)(v, MD_TRANS);
				ob_text(wt, v, NULL, ct, &gr, &r, NULL, -1, -1, -1, -1, 0,0,0, text, ob->ob_state, 0, und, G_BLACK);
			}
		}
	}

	if (textpatch)
	{
		*textpatch = '|';
		textpatch = NULL;
	}

	done(OS_SELECTED);
}

/* set by the icon drawers: 1 when the icon sits on the desktop
 * background (the owner's desktop tree) rather than in a window */
static short apj_icon_on_desktop = 0;

static void
icon_characters(struct xa_vdi_settings *v, struct theme *theme, ICONBLK *iconblk, short state, short obx, short oby, short icx, short icy)
{
	char lc = iconblk->ib_char;
	short tx,ty,pnt[4];

	if( !lc && !*iconblk->ib_ptext )
		return;

	(*v->api->wr_mode)(v, MD_REPLACE);
	(*v->api->ritopxy)(pnt, obx + iconblk->ib_xtext, oby + iconblk->ib_ytext,
		     iconblk->ib_wtext, iconblk->ib_htext);

	/* APJ-OS: a label box tall enough for it (48px icon sets) gets the
	 * standard font, antialiased; the classic 8px box keeps the small font */
	if (apj_active && !MONO && iconblk->ib_htext >= screen->c_max_h)
		(*v->api->t_font)(v, screen->standard_font_point, screen->standard_font_id);
	else
		(*v->api->t_font)(v, screen->small_font_point, screen->small_font_id);

	/* center the text in a bar given by iconblk->tx, relative to object */
	(*v->api->t_color)(v, G_BLACK);
	if (   iconblk->ib_ptext
	    && *iconblk->ib_ptext
	    && iconblk->ib_wtext
	    && iconblk->ib_htext)
	{
		short tw, th;

		(*v->api->t_extent)(v, iconblk->ib_ptext, &tw, &th);
		if (!(apj_active && !MONO))
		{
			tw = strlen(iconblk->ib_ptext) * 6;		/* the stock small-font maths */
			th = 6;
		}
		tx = obx + iconblk->ib_xtext + ((iconblk->ib_wtext - tw) / 2);
		ty = oby + iconblk->ib_ytext + ((iconblk->ib_htext - th) / 2);

		if (state & OS_STATE08)
		{
			short col;
			if (state & OS_SELECTED)
				col = G_WHITE;
			else
				col = G_BLACK;
			(*v->api->wr_mode)(v, MD_TRANS);
			(*v->api->t_color)(v, col);
		}
		else if (apj_active && !MONO)
		{
			/* APJ-OS: no opaque label box. Unselected labels sit
			 * transparently on whatever is behind them in the theme's
			 * text colour; a selected icon gets a flat selection pill
			 * behind its label with the selection text colour - the
			 * Windows desktop convention, and the ib_char colours (which
			 * assume a white box) are ignored. */
			if (state & OS_SELECTED)
			{
				GRECT lb;

				lb.g_x = tx - 3;
				lb.g_y = oby + iconblk->ib_ytext;
				lb.g_w = tw + 6;
				lb.g_h = iconblk->ib_htext;
				apj_flat_box(v, &lb, APJ_PEN(APJ_R_SELBG), APJ_PEN(APJ_R_SELBG));
				(*v->api->t_color)(v, APJ_PEN(APJ_R_SELFG));
			}
			else if (apj_icon_on_desktop)
			{
				/* on the wallpaper, which can be anything: white with a
				 * one-pixel dark shadow, as the Windows desktop does */
				(*v->api->wr_mode)(v, MD_TRANS);
				(*v->api->t_color)(v, G_BLACK);
				if (!apj_gtext(v, tx + 1, ty + 1, G_BLACK, iconblk->ib_ptext))
					v_gtext(v->handle, tx + 1, ty + 1, iconblk->ib_ptext);
				(*v->api->t_color)(v, G_WHITE);
			}
			else
				(*v->api->t_color)(v, APJ_PEN(APJ_R_TEXT));

			(*v->api->wr_mode)(v, MD_TRANS);
			if (state & OS_DISABLED)
				(*v->api->t_effects)(v, FAINT);
		}
		else
		{
			short text_fg, text_bg;

			/* Text foreground comes from ib_char bits 12-15 and the
			 * background from bits 8-11; swap them when the icon is
			 * selected, mirroring d_g_icon. */
			if (state & OS_SELECTED)
			{
				text_fg = (iconblk->ib_char >> 8) & 0x0f;
				text_bg = (iconblk->ib_char >> 12) & 0x0f;
			}
			else
			{
				text_fg = (iconblk->ib_char >> 12) & 0x0f;
				text_bg = (iconblk->ib_char >> 8) & 0x0f;
			}

			(*v->api->f_color)(v, text_bg);
			(*v->api->f_interior)(v, FIS_SOLID);
			v_bar(v->handle, pnt);

			(*v->api->t_color)(v, text_fg);
			/* The strip was just filled with text_bg by v_bar above;
			 * draw the glyphs transparently so v_gtext's MD_REPLACE
			 * does not repaint the character cells with G_WHITE. */
			if (state & OS_SELECTED)
				(*v->api->wr_mode)(v, MD_XOR);
			else
				(*v->api->wr_mode)(v, MD_TRANS);
			if (state & OS_DISABLED)
				(*v->api->t_effects)(v, FAINT);
		}
		{
			short lfg = -1;

			if (apj_active && !MONO && !(state & OS_STATE08))
				lfg = (state & OS_SELECTED) ? APJ_PEN(APJ_R_SELFG) : apj_icon_on_desktop ? G_WHITE : APJ_PEN(APJ_R_TEXT);
			if (lfg < 0 || !apj_gtext(v, tx, ty, lfg, iconblk->ib_ptext))
				v_gtext(v->handle, tx, ty, iconblk->ib_ptext);
		}
	}

	if (lc != 0 && lc != ' ')
	{
		char ch[2];
		short char_fg;

		if (state & OS_SELECTED)
			char_fg = (iconblk->ib_char >> 8) & 0x0f;
		else
			char_fg = (iconblk->ib_char >> 12) & 0x0f;

		ch[0] = lc;
		ch[1] = 0;
		/* Draw the icon char over the existing background; MD_REPLACE
		 * would clear a white rectangle behind it. */
		(*v->api->wr_mode)(v, MD_TRANS);
		(*v->api->t_color)(v, char_fg);
		v_gtext(v->handle, icx + iconblk->ib_xchar, icy + iconblk->ib_ychar, ch);
		/* Seemingly the ch is supposed to be relative to the image */
	}

	if (state & OS_SELECTED)
		(*v->api->f_color)(v, G_WHITE);

	(*v->api->t_effects)(v, 0);
	(*v->api->wr_mode)(v, MD_TRANS);
}

/*
 * Draw a image
 */
static void _cdecl
d_g_image(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	OBJECT *ob = wt->current.ob;
	BITBLK *bitblk;
	MFDB Mscreen;
	MFDB Micon;
	short fl3d, pxy[8], cols[2], icx, icy;

	fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;

	bitblk = (*api->object_get_spec)(ob)->bitblk;

	icx = wt->r.g_x;
	icy = wt->r.g_y;

	Mscreen.fd_addr = NULL;

	Micon.fd_w = (bitblk->bi_wb & -2) << 3;
	Micon.fd_h = bitblk->bi_hl;
	Micon.fd_wdwidth = (Micon.fd_w + 15) >> 4;
	Micon.fd_nplanes = 1;
	Micon.fd_stand = 0;
	Micon.fd_addr = bitblk->bi_pdata;

	cols[0] = bitblk->bi_color;	/* HR 100202: simply 1 foreground color index. */
	cols[1] = 0;			/* background color; not used in mode MD_TRANS. */

	pxy[0] = bitblk->bi_x;
	pxy[1] = bitblk->bi_y;
	pxy[2] = Micon.fd_w - 1 - pxy[0];
	pxy[3] = Micon.fd_h - 1 - pxy[1];
	pxy[4] = icx;
	pxy[5] = icy;
	pxy[6] = icx + pxy[2];
	pxy[7] = icy + pxy[3];

	vrt_cpyfm(v->handle, MD_TRANS, pxy, &Micon, &Mscreen, cols);

	if (ob->ob_state & OS_DISABLED)
		write_disable(v, &wt->r, (fl3d == BKG) ? G_LWHITE : G_WHITE);
	if (ob->ob_state & OS_SELECTED)
		write_selected(v, &wt->r, MD_XOR, G_BLACK);

	done(OS_SELECTED|OS_DISABLED);
}

/* HR 060202: all icons: handle disabled: make icon rectangle and text faint in stead
 *                                        of object rectangle.
 */

static unsigned short dismskb[10 * 160];
static MFDB Mddm =
{
	dismskb,
	160, 160,
	10,
	1,
	1,
	0,0,0
};
/*
 * Draw a mono icon
 */
static void _cdecl
d_g_icon(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	struct theme *theme = wt->objcr_theme;
	OBJECT *ob = wt->current.ob;
	ICONBLK *iconblk;
	MFDB Mscreen;
	MFDB Micon;
	GRECT ic;
	short pxy[8], cols[2], obx, oby, msk_col, icn_col, blitmode;

	iconblk = (*api->object_get_spec)(ob)->iconblk;
	obx = wt->r.g_x;
	oby = wt->r.g_y;

	ic = *(GRECT*)&iconblk->ib_xicon;
	ic.g_x += obx;
	ic.g_y += oby;

	(*v->api->ritopxy)(pxy,     0, 0, ic.g_w, ic.g_h);
	(*v->api->rtopxy) (pxy + 4, &ic);

	Micon.fd_w = ic.g_w;
	Micon.fd_h = ic.g_h;
	Micon.fd_wdwidth = (ic.g_w + 15) >> 4;
	Micon.fd_nplanes = 1;
	Micon.fd_stand = 0;
	Mscreen.fd_addr = NULL;

	if (ob->ob_state & OS_SELECTED)
	{
		icn_col = ((iconblk->ib_char) >> 8) & 0xf;
		msk_col = ((iconblk->ib_char) >> 12) & 0xf;
	}
	else
	{
		icn_col = ((iconblk->ib_char) >> 12) & 0xf;
		msk_col = ((iconblk->ib_char) >> 8) & 0xf;
	}
	/* Ozk:
	 * Dont draw mask when WHITEBAK set
	 */
	if (!(ob->ob_state & OS_WHITEBAK))
	{
		Micon.fd_addr = iconblk->ib_pmask;
		cols[0] = msk_col;
		cols[1] = icn_col;
		vrt_cpyfm(v->handle, MD_TRANS, pxy, &Micon, &Mscreen, cols);
		blitmode = MD_TRANS;
	}
	else
		blitmode = MD_REPLACE;

	Micon.fd_addr = iconblk->ib_pdata;
	cols[0] = icn_col;
	cols[1] = msk_col;
	vrt_cpyfm(v->handle, blitmode, pxy, &Micon, &Mscreen, cols);

	if (ob->ob_state & OS_DISABLED)
	{
		int i, j, sc;
		unsigned short *s, *d, m;
		unsigned long sm = 0xAAAA5555L;

		s = (unsigned short *)iconblk->ib_pmask;
		d = (unsigned short *)Mddm.fd_addr;
		sc = Mddm.fd_wdwidth - Micon.fd_wdwidth;

		for (i = 0; i < Micon.fd_h; i++)
		{
			m = sm;
			for (j = 0; j < Micon.fd_wdwidth; j++)
			{
				*d++ = *s++ & m;
			}
			sm = (sm << 16) | (sm >> 16);
			d += sc;
		}

		if (screen->planes > 3)
			cols[0] = G_LWHITE;
		else
			cols[0] = G_WHITE;

		cols[1] = G_WHITE;
		vrt_cpyfm(v->handle, MD_TRANS, pxy, &Mddm, &Mscreen, cols);
	}

	/* should be the same for color & mono */
	apj_icon_on_desktop = (wt->owner && wt->owner->desktop && wt->tree == wt->owner->desktop->tree) ? 1 : 0;
	icon_characters(v, theme, iconblk, ob->ob_state & (OS_SELECTED|OS_DISABLED), obx, oby, ic.g_x, ic.g_y);

	done(OS_SELECTED|OS_DISABLED);
}

/*
 * A clean flat stand-in for an icon that has no replacement in the set.
 * A rounded document tile: face fill, 1px border, a folded top-right
 * corner. Uses theme pens and solid fills only - no colour-plane blit,
 * so it never produces the stock 32-bit noise. Centred in the icon rect,
 * capped so a big cell still gets a sensibly sized tile.
 */
static void
apj_icon_placeholder(struct xa_vdi_settings *v, const GRECT *ic, short sel)
{
	struct xa_vdi_api *vapi = v->api;
	short face   = APJ_PEN(sel ? APJ_R_SELBG : APJ_R_PAPER);
	short border = APJ_PEN(APJ_R_BORDER);
	short fold   = APJ_PEN(APJ_R_DISABLED);
	short w = ic->g_w, h = ic->g_h, fw, tx, ty, cut, i;
	GRECT r;

	if (w <= 0 || h <= 0)
		return;

	/* a portrait document tile, ~70% of the cell, centred */
	fw = (w * 7) / 10;
	if (fw < 12) fw = w < 12 ? w : 12;
	r.g_h = (h * 8) / 10;
	if (r.g_h < 14) r.g_h = h < 14 ? h : 14;
	r.g_w = fw;
	r.g_x = ic->g_x + ((w - r.g_w) >> 1);
	r.g_y = ic->g_y + ((h - r.g_h) >> 1);

	cut = r.g_w / 3;
	if (cut > r.g_h / 3) cut = r.g_h / 3;
	if (cut < 3) cut = 3;

	(*vapi->wr_mode)(v, MD_REPLACE);

	/* body */
	(*vapi->f_interior)(v, FIS_SOLID);
	(*vapi->f_color)(v, face);
	(*vapi->gbar)(v, 0, &r);

	/* border (leave the corners one pixel short for a soft-corner look) */
	(*vapi->l_color)(v, border);
	(*vapi->line)(v, r.g_x + 1, r.g_y, r.g_x + r.g_w - cut - 1, r.g_y, border);          /* top, up to the fold */
	(*vapi->line)(v, r.g_x + 1, r.g_y + r.g_h - 1, r.g_x + r.g_w - 2, r.g_y + r.g_h - 1, border); /* bottom */
	(*vapi->line)(v, r.g_x, r.g_y + 1, r.g_x, r.g_y + r.g_h - 2, border);                /* left */
	(*vapi->line)(v, r.g_x + r.g_w - 1, r.g_y + cut + 1, r.g_x + r.g_w - 1, r.g_y + r.g_h - 2, border); /* right, below the fold */

	/* folded top-right corner */
	tx = r.g_x + r.g_w - cut;
	ty = r.g_y;
	(*vapi->line)(v, tx, ty, r.g_x + r.g_w - 1, ty + cut, border);   /* diagonal */
	(*vapi->line)(v, tx, ty, tx, ty + cut, border);                 /* fold left edge */
	(*vapi->line)(v, tx, ty + cut, r.g_x + r.g_w - 1, ty + cut, border); /* fold bottom */

	/* a couple of faint "text" lines so it reads as a document */
	for (i = 1; i <= 2; i++)
	{
		short ly = r.g_y + cut + 4 + i * 5;

		if (ly < r.g_y + r.g_h - 4)
			(*vapi->line)(v, r.g_x + 4, ly, r.g_x + r.g_w - 5, ly, fold);
	}
}

/*
 * Draw a colour icon
 */
static void _cdecl
d_g_cicon(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	struct theme *theme = wt->objcr_theme;
	OBJECT *ob = wt->current.ob;
	ICONBLK *iconblk;
	CICON	*best_cicon;
	MFDB Mscreen;
	MFDB Micon, Mmask;
	bool have_sel;
	GRECT ic;
	short pxy[8], cols[2] = {0,1}, obx, oby, blitmode;

	best_cicon = (*api->getbest_cicon)((*api->object_get_spec)(ob)->ciconblk);
	/* No matching icon, so use the mono one instead */
	if (!best_cicon)
	{
		d_g_icon(wt, v);
		return;
	}

	iconblk = (*api->object_get_spec)(ob)->iconblk;
	obx = wt->r.g_x;
	oby = wt->r.g_y;

	ic = *(GRECT *)&iconblk->ib_xicon;

	ic.g_x += obx;
	ic.g_y += oby;

	/* APJ-OS: a replacement icon under a theme */
	if (apj_active && !MONO)
	{
		struct apj_icon *ai;

		apj_icons_load();
		if ((ai = apj_icon_find(iconblk)) && ai->w == ic.g_w && ai->h == ic.g_h
		    && apj_icon_draw(v, ai, ic.g_x, ic.g_y, (ob->ob_state & OS_SELECTED) ? 1 : 0, (ob->ob_state & OS_DISABLED) ? 1 : 0))
		{
			if (iconblk->ib_char || *iconblk->ib_ptext)
				apj_icon_on_desktop = (wt->owner && wt->owner->desktop && wt->tree == wt->owner->desktop->tree) ? 1 : 0;
			icon_characters(v, theme, iconblk, ob->ob_state & (OS_SELECTED|OS_DISABLED), obx, oby, ic.g_x, ic.g_y);
			done(OS_SELECTED|OS_DISABLED);
			return;
		}

		/* No replacement for this icon. The stock CICON path paints the
		 * colour planes wrong on this 32-bit framebuffer (the noisy blob),
		 * so draw a clean flat placeholder instead - a rounded panel tile
		 * with a folded corner - and keep the label. Deliberate, not
		 * broken, until the icon is added to the set. */
		apj_icon_placeholder(v, &ic, ob->ob_state & OS_SELECTED);
		if (iconblk->ib_char || *iconblk->ib_ptext)
			apj_icon_on_desktop = (wt->owner && wt->owner->desktop && wt->tree == wt->owner->desktop->tree) ? 1 : 0;
		icon_characters(v, theme, iconblk, ob->ob_state & (OS_SELECTED|OS_DISABLED), obx, oby, ic.g_x, ic.g_y);
		done(OS_SELECTED|OS_DISABLED);
		return;
	}

	(*v->api->ritopxy)(pxy,     0, 0, ic.g_w, ic.g_h);
	(*v->api->rtopxy) (pxy + 4, &ic);

	Micon.fd_w = ic.g_w;
	Micon.fd_h = ic.g_h;
	Micon.fd_wdwidth = (ic.g_w + 15) >> 4;
	Micon.fd_nplanes = 1;
	Micon.fd_stand = 0;
	Mscreen.fd_addr = NULL;

	Mmask = Micon;
	have_sel = best_cicon->sel_data != NULL;

	/* check existence of selection. */
	if ((ob->ob_state & OS_SELECTED) && have_sel)
	{
		Micon.fd_addr = best_cicon->sel_data;
		Mmask.fd_addr = best_cicon->sel_mask;
	}
	else
	{
		Micon.fd_addr = best_cicon->col_data;
		Mmask.fd_addr = best_cicon->col_mask;
	}

	if (!(ob->ob_state & OS_WHITEBAK))
	{
		vrt_cpyfm(v->handle, MD_TRANS, pxy, &Mmask, &Mscreen, cols);
		blitmode = screen->planes > 8 ? S_AND_D : S_OR_D;
	}
	else
		blitmode = S_ONLY;

	Micon.fd_nplanes = screen->planes;

	vro_cpyfm(RASTER_HDL, blitmode, pxy, &Micon, &Mscreen);
	if( iconblk->ib_char || *iconblk->ib_ptext )
		apj_icon_on_desktop = (wt->owner && wt->owner->desktop && wt->tree == wt->owner->desktop->tree) ? 1 : 0;
	icon_characters(v, theme, iconblk, ob->ob_state & (OS_SELECTED|OS_DISABLED), obx, oby, ic.g_x, ic.g_y);

	if ((ob->ob_state & OS_DISABLED) || ((ob->ob_state & OS_SELECTED) && !have_sel))
	{
		int i, j, sc;
		unsigned short *s, *d, m;
		unsigned long sm = 0xAAAA5555L;

		s = Mmask.fd_addr;
		d = Mddm.fd_addr;
		sc = Mddm.fd_wdwidth - Mmask.fd_wdwidth;

		for (i = 0; i < Micon.fd_h; i++)
		{
			m = sm;
			for (j = 0; j < Micon.fd_wdwidth; j++)
			{
				*d++ = *s++ & m;
			}
			sm = (sm << 16) | (sm >> 16);
			d += sc;
		}

		if (ob->ob_state & OS_DISABLED)
			cols[0] = G_LWHITE;
		else
			cols[0] = G_BLACK;

		cols[1] = G_WHITE;
		vrt_cpyfm(v->handle, MD_TRANS, pxy, &Mddm, &Mscreen, cols);
	}
	done(OS_SELECTED|OS_DISABLED);
}

static void
drw_g_text(struct widget_tree *wt, struct xa_vdi_settings *v, bool ftext)
{
	struct theme *theme = wt->objcr_theme;
	struct object_theme *obt;
	struct color_theme *ct;
	short fl3d, f_fg, boxflags, fwr_mode;
	OBJECT *ob = wt->current.ob;
	TEDINFO *ted;
	GRECT r = wt->r, gr;
	BFOBSPEC c;
	struct objc_edit_info *ei;
	bool selected, disabled, writedis = false, writesel = false;
	char temp_text[256];

	obt = ((ob->ob_flags & OF_EDITABLE) && 0) ? &theme->ed_text : &theme->text;

	selected = (ob->ob_state & OS_SELECTED);

	fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;

	if ((disabled = (ob->ob_state & OS_DISABLED)))
	{
		ct = selected ? &obt->dis.s[fl3d] : &obt->dis.n[fl3d];
	}
	else
	{
		ct = selected ? &obt->norm.s[fl3d] : &obt->norm.n[fl3d];
	}

	get_tedinformation(ob, &ted, &c, &ei);
	if (!ei)
		ei = &wt->e;
	if (edit_ob(ei) != wt->current.ob)
		ei = NULL;

	boxflags = (c.textmode ? (DRAW_BKG|DRAW_TEXTURE|ONLY_TEXTURE) : 0) | (ANCH_PARENT|DRAW_3D);
	if (fl3d && c.fillpattern == IP_HOLLOW && !c.interiorcol)
	{
		/* CONFIG: Draw background or not here */
		draw_g_box(wt, v, ct, NULL, boxflags, &gr);
		c.textmode = 0;
		fwr_mode = -1;
	}
	else
	{
		if (selected || disabled)
			draw_g_box(wt, v, ct, NULL, boxflags | DRAW_BKG, &gr);
		else
		{
			draw_g_box(wt, v, ct, &c, boxflags & ~(DRAW_BKG|DRAW_TEXTURE), &gr);
			writedis = disabled;
			writesel = selected;
		}
	}
	fwr_mode = disabled ? -1 : (c.textmode ? MD_REPLACE : MD_TRANS);

	(*v->api->t_effects)(v, ct->fnt.effects);
	set_obtext(ted, v, &gr, NULL, ftext, -1, temp_text, NULL, r);

	if (disabled)
	{
		f_fg = -1;
	}
	else if (selected)
	{
		if (fl3d == ACT)
		{
			gr.g_x += PUSH3D_DISTANCE;
			gr.g_y += PUSH3D_DISTANCE;
			f_fg = -1;
		}
		else
		{
			f_fg = -1;
		}
	}
	else
	{
		f_fg = c.textcol;
	}

	ob_text(wt, v, ei, ct, &gr, &r, disabled ? NULL : &c, fwr_mode, f_fg, -1, -1, G_WHITE, G_LBLUE, G_BLUE, temp_text, ob->ob_state, 0, -1, G_BLACK);

	if (writedis)
		write_disable(v, &r, G_WHITE);
	if (writesel)
		write_selected(v, &r, MD_XOR, G_BLACK);

	done(OS_SELECTED|OS_DISABLED);
}

/*
 * Draw a text object
 */
static void _cdecl
d_g_text(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	drw_g_text(wt, v, false);
}
static void _cdecl
d_g_ftext(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	drw_g_text(wt, v, true);
}

static void _cdecl
d_g_string(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	struct theme *theme = wt->objcr_theme;
	struct object_theme *obt;
	struct color_theme *ct;
	GRECT r = wt->r;
	OBJECT *ob = wt->current.ob;
	ushort state = ob->ob_state;
	bool selected, disabled;
	short fl3d;
	char *t, text[256];

	t = (*api->object_get_spec)(ob)->free_string;

	/* most AES's allow null string */
	if (t)
	{
		fl3d = (ob->ob_flags & OF_FL3DMASK) >> 9;
		strncpy(text, t, 254);
		text[255] = '\0';

		selected = ob->ob_state & OS_SELECTED;

		if (wt->is_menu)
			obt = &theme->pu_string;
		else
			obt = &theme->string;

		if ((disabled = (ob->ob_state & OS_DISABLED)))
		{
			ct = selected ? &obt->dis.s[fl3d] : &obt->dis.n[fl3d];
		}
		else
		{
			ct = selected ? &obt->norm.s[fl3d] : &obt->norm.n[fl3d];
		}

		if (wt->is_menu)
		{
			short d = 0, d3t, thick;
			if (selected)
			{
				d3t = 1;
			}
			else
			{
				d3t = 0;
			}

			thick = 0;
			draw_objc_bkg(wt, v, ct, NULL, DRAW_ALL|DRAW_TEXTURE|ONLY_TEXTURE, -1, -1, d, d3t, thick, 2, &r, &r, NULL);
			done(OS_SELECTED);
		}

		if (wt->is_menu
		    && (ob->ob_state & OS_DISABLED)
		    && *text == '-')
		{
			/* If optional text is found, remove leading and trailing '-' */
			short start_pos = 0;
			char *end_pos;
			short old_rx, old_rw;

			while (text[start_pos] == '-')
				start_pos++;
			if (text[start_pos] != '\0')
			{
				end_pos = text + strlen(text) - 1;
				while (end_pos > text && *end_pos == '-')
					end_pos--;
				end_pos[1] = '\0';
				strcpy(text, text + start_pos);
			} else
			{
				strcpy(text, "");
			}
			/* Draw separator line (or line on the left if text is present) */
			old_rx = r.g_x;
			old_rw = r.g_w;
			if (*text)
			{				
				r.g_x += 1;
				r.g_w = start_pos * screen->c_max_w - 1;
			} else
			{
				r.g_x += 1;
				r.g_w -= 3;				
			}
			r.g_y += (r.g_h - 2)/2;
			if (MONO)
			{
				(*v->api->l_type)(v, 7);
				(*v->api->l_udsty)(v, 0xaaaa);
				(*v->api->line)(v, r.g_x, r.g_y, r.g_x + r.g_w, r.g_y, G_BLACK);
				(*v->api->l_udsty)(v, 0x5555);
				(*v->api->line)(v, r.g_x, r.g_y + 1, r.g_x + r.g_w, r.g_y + 1, G_BLACK);
				(*v->api->l_type)(v, 0);
			}
			else
			{
				r.g_x += 2, r.g_w -= 4;
				(*v->api->line)(v, r.g_x, r.g_y,     r.g_x + r.g_w, r.g_y,     ct->col.bottom);
				(*v->api->line)(v, r.g_x, r.g_y + 1, r.g_x + r.g_w, r.g_y + 1, ct->col.top);
			}
			/* Draw text if any, ignore underscore/OS_WHITEBAK */
			if (*text) 
			{				   
				/* Draw separator line on the right if there is enough space */
				r.g_x = old_rx + (start_pos + strlen(text)) * screen->c_max_w; 
				r.g_w = old_rw - (start_pos + strlen(text)) * screen->c_max_w - 2;
				if (MONO && (r.g_w > 0))
				{
					(*v->api->l_type)(v, 7);
					(*v->api->l_udsty)(v, 0xaaaa);
					(*v->api->line)(v, r.g_x, r.g_y, r.g_x + r.g_w, r.g_y, G_BLACK);
					(*v->api->l_udsty)(v, 0x5555);
					(*v->api->line)(v, r.g_x, r.g_y + 1, r.g_x + r.g_w, r.g_y + 1, G_BLACK);
					(*v->api->l_type)(v, 0);
				}
				else if (!MONO && (r.g_w > 4))
				{
					r.g_x += 2, r.g_w -= 4;
					(*v->api->line)(v, r.g_x, r.g_y,     r.g_x + r.g_w, r.g_y,     ct->col.bottom);
					(*v->api->line)(v, r.g_x, r.g_y + 1, r.g_x + r.g_w, r.g_y + 1, ct->col.top);
				}
				/* Draw text */
				(*v->api->t_font)(v, screen->standard_font_point, screen->standard_font_id);
				(*v->api->t_color)(v, ct->fnt.fg);
				(*v->api->t_effects)(v, ct->fnt.effects);
				r.g_y -= (r.g_h - 2)/2; /* fixes y-coordinate to draw text */
				r.g_x = old_rx + start_pos * screen->c_max_w;
				ob_text(wt, v, NULL, ct, &r, &wt->r, NULL, -1, -1, -1, -1, 0,0,0, text, state, 0, -1, G_BLACK);				
			}
			done(OS_DISABLED);
		}
		else
		{
			short und = -1, flags = 0;

			if ((state & OS_WHITEBAK))
			{
				short ns = state >> 8;
				if (!(ns & 0x80))
				{
					und = ns & 0x7f;
				}
				else if (ns == 0xfe)
				{
					und = -2;
					flags = ob->ob_flags;
				}
				else if (ns == 0xff)
				{
					flags = ob->ob_flags;
					und = -3;
				}
			}
			(*v->api->t_font)(v, screen->standard_font_point, screen->standard_font_id);
			(*v->api->t_color)(v, ct->fnt.fg);
			(*v->api->t_effects)(v, ct->fnt.effects);
			ob_text(wt, v, NULL, ct, &r, &wt->r, NULL, -1, -1, -1, -1, 0,0,0, t, state, flags, und, G_BLACK);
		}
	}
}

static void _cdecl
d_g_title(struct widget_tree *wt, struct xa_vdi_settings *v)
{
	struct theme *theme = wt->objcr_theme;
	struct object_theme *obt = &theme->title;
	struct color_theme *ct;
	short d, d3t, thick;
	OBJECT *ob = wt->current.ob;
	char *t = (*api->object_get_spec)(ob)->free_string;
	bool selected;

	if (t)
	{
		short und = -1, state = ob->ob_state;
		GRECT r = wt->r;
		char text[256];

		selected = ob->ob_state & OS_SELECTED;

		if (ob->ob_state & OS_DISABLED)
		{
			ct = selected ? &obt->dis.s[0] : &obt->dis.n[0];
		}
		else
		{
			ct = selected ? &obt->norm.s[0] : &obt->norm.n[0];
		}

		strncpy(text, t, 254);
		text[255] = '\0';
		if (selected)
		{
			d3t = 1;
		}
		else
		{
			d3t = 0;
		}
		thick = d = 0;
		draw_objc_bkg(wt, v, ct, NULL, DRAW_ALL|DRAW_TEXTURE|ONLY_TEXTURE, -1, -1, d, d3t, thick, 1, &r, &r, NULL);
		/* most AES's allow null string */
		(*v->api->t_font)(v, screen->standard_font_point, screen->standard_font_id);
		(*v->api->t_color)(v, ct->fnt.fg);
		(*v->api->t_effects)(v, ct->fnt.effects);

		if (state & 0x8000)
			und = (state >> 8) & 0x7f;

		ob_text(wt, v, NULL, ct, &r, &wt->r, NULL, -1, -1, -1, -1, 0,0,0, text, ob->ob_state, OF_FL3DBAK, und, G_BLACK);
	}
	done(OS_SELECTED);
}

/* wallpaper path state is owned by render_obj.c and shared by both renderers */
extern int imgpath_file;
extern char imgpath[128];

static void _cdecl
delete_texture(void *_t)
{
	struct texture *t = _t;

	if (t->xamfdb.mfdb.fd_addr)
		(*api->kfree)(t->xamfdb.mfdb.fd_addr);
	(*api->kfree)(t);
}

static struct texture *
load_texture(char *fn)
{
	struct texture *t = NULL;

	if( imgpath_file == -1 )
		return NULL;

	imgpath[imgpath_file] = '\0';
	strcat(imgpath, fn);
	if ((fn = (*api->sysfile)(imgpath)))
	{
		if ((t = (*api->kmalloc)(sizeof(*t))))
		{
			(*api->load_img)(fn, &t->xamfdb);
			if (t->xamfdb.mfdb.fd_addr)
			{
				(*api->add_xa_data)(&allocs, t, 0, NULL, delete_texture);
				t->t.flags = 0;
				t->t.anchor = 0;
				t->t.left = t->t.right = t->t.top = t->t.bottom = NULL;
				t->t.body = &t->xamfdb;
			}
			else
			{
				(*api->kfree)(t);
				t = NULL;
			}
		}
		(*api->kfree)(fn);
	}
	return t;
}

#define IT_NORMAL	1
#define IT_DISABLED	2

#define ST_NONE		1
#define ST_IND		2
#define ST_BKG		4
#define ST_ACT		8
#define ST_ALL		(ST_NONE|ST_IND|ST_BKG|ST_ACT)
#define ST_ALL3D	(ST_IND|ST_BKG|ST_ACT)

static void
set_texture(struct texture *t, struct ob_theme *ot, short n, short s, short h)
{
	int i;
	struct color_theme *ct;

	if (n)
	{
		ct = &ot->n[0];
		for (i = 0; i < 4; i++, ct++)
		{
			if (n & 1)
			{
				ct->col.texture = &t->t;
			}
			n >>= 1;
		}
	}
	if (s)
	{
		ct = &ot->s[0];
		for (i = 0; i < 4; i++, ct++)
		{
			if (s & 1)
			{
				ct->col.texture = &t->t;
			}
			s >>= 1;
		}
	}
	if (h)
	{
		ct = &ot->h[0];
		for (i = 0; i < 4; i++, ct++)
		{
			if (h & 1)
			{
				ct->col.texture = &t->t;
			}
			h >>= 1;
		}
	}
}

static struct texture *
install_texture(char *fn, struct texture *texture, struct object_theme *obt, short norm_n, short norm_s, short norm_h, short dis_n, short dis_s, short dis_h)
{
	struct texture *t = NULL;

	if (screen->planes >= 8)
	{
		if ((t = texture) || (fn && (t = load_texture(fn))) )
		{
			set_texture(t, &obt->norm, norm_n, norm_s, norm_h);
			set_texture(t, &obt->dis, dis_n, dis_s, dis_h);
		}
	}
	return t;
}
static void
load_textures(struct theme *theme)
{
	if (screen->planes >= 8)
	{
		install_texture("dbox.img"/*steel3.img"*/, NULL, &theme->box, ST_ALL, ST_ALL, 0, 0, 0, 0);
		install_texture("popbkg.img", NULL, &theme->popupbkg, ST_ALL, 0, 0, 0,0,0);
		install_texture("dbutton.img", NULL, &theme->button, ST_ALL, ST_ALL, 0, ST_ALL, ST_ALL,0);
		install_texture("dtext.img", NULL, &theme->text, ST_ALL, ST_ALL, 0, ST_ALL, ST_ALL, 0);
	}
}

#if WITH_GRADIENTS
#include "win_draw.h"

/* load_gradients() lives in render_obj.c - shared service, see render_obj.h */
#endif
static void * _cdecl
init_module(const struct xa_module_api *xmapi, const struct xa_screen *xa_screen, bool grad)
{
	char *resource_name;

	use_gradients = grad;
	api = xmapi;
	screen = xa_screen;

	allocs = NULL;
	pmaps = NULL;

	/*
	 * Now load and fix the resource containing the extended AES object icons
	 * This will be done by the object renderer later on...
	 */
	if (!(resource_name = (*api->sysfile)(xobj_name)))
	{
		DIAGS(("ERROR: Can't find extended AES objects resource file '%s'", xobj_name));
		return NULL;
	}
	else
	{
		xobj_rshdr = (*api->load_resource)(NULL, resource_name, NULL, DU_RSX_CONV, DU_RSY_CONV, false);
		(*api->kfree)(resource_name);
	}
	if (!xobj_rshdr)
	{
		DIAGS(("ERROR: Can't load extended AES objects resource file '%s'", xobj_name));
		return NULL;
	}

	/* get widget object parameters. */
	{
		int i;
		GRECT c;
		OBJECT *tree = (*api->resource_tree)(xobj_rshdr, EXT_AESOBJ);

		(*api->ob_spec_xywh)(tree, 1, &c);

		for (i = 1; i < EXTOBJ_NAME; i++)
			tree[i].ob_x = tree[i].ob_y = 0;

		xobj_rsc = tree;
	}
	/*
	 * APJ-OS: the stock renderer's init_module() has already run by the time
	 * we get here (k_init.c brings it up for AESSYS; this module is opened
	 * lazily for the first client that asks for it). Gradient loading and the
	 * texture/imgpath setup write state that lives in render_obj.c and is
	 * shared by both renderers, so repeating them here would reload the
	 * gradient files and re-scan the texture directory for no gain - and
	 * would reset imgpath_file underneath the renderer already using it.
	 *
	 * (void) the unused parameter so -Wall -Werror stays happy.
	 */
	(void) grad;

	return (void *)1L;
}


static long _cdecl
exit_module(void)
{
	DIAGS(("exit_module:"));
	if (apj_tbuf)
	{
		(*api->kfree)(apj_tbuf);
		apj_tbuf = NULL;
		apj_tbuf_size = 0;
	}
	/* APJ-OS: the icon table is loaded ONCE and kept for the life of
	 * XaAES. It is NOT tied to this module's init/exit lifecycle - the
	 * module is torn down and re-inited many times (theme commits, client
	 * swaps), and freeing + reloading the table on every cycle raced with
	 * in-flight icon draws and dropped entries (RAMDISK etc. went missing
	 * from a fully-loaded 132-entry table). Leaving it allocated costs a
	 * few hundred KB until reboot and makes lookups deterministic. */
	(*api->free_xa_data_list)(&allocs);
	(*api->free_xa_data_list)(&pmaps);
	current_render_api = NULL;
	current_theme = NULL;
	return 0;
}

static void
free_ob_theme_resources(struct ob_theme *obt)
{
#if WITH_GRADIENTS
	int j;
	{
		struct xa_gradient *g;

		for (j = 0; j < 4; j++)
		{
			if ((g = obt->n[j].col.gradient))
				(*api->free_xa_data_list)(&g->allocs);
		}
		for (j = 0; j < 4; j++)
		{
			if ((g = obt->s[j].col.gradient))
				(*api->free_xa_data_list)(&g->allocs);
		}
		for (j = 0; j < 4; j++)
		{
			if ((g = obt->h[j].col.gradient))
				(*api->free_xa_data_list)(&g->allocs);
		}
	}
#endif
}

static void
free_theme_resources(struct theme *theme)
{
	int i, n_obt;
	struct object_theme *obt;
#if WITH_GRADIENTS
	struct xa_gradient *g = theme->outline.gradient;

	if (g)
		(*api->free_xa_data_list)(&g->allocs);
#endif
	obt = &theme->button;
	n_obt = ((long)&theme->end_obt - (long)obt) / sizeof(*obt);

	for (i = 0; i < n_obt; i++, obt++)
	{
		free_ob_theme_resources(&obt->norm);
		free_ob_theme_resources(&obt->dis);
	}
}

static void _cdecl
delete_rendapi(void *_render)
{
	DIAGS(("deleting rapi %lx", (unsigned long)_render));
	(*api->kfree)(_render);
}

static long _cdecl
open(struct object_render_api **rapi)
{
	long ret = E_OK;

	if (rapi)
	{
		if (!current_render_api)
		{
			current_render_api = (*api->kmalloc)(sizeof(struct object_render_api));
			if (current_render_api)
			{
				setup_render_api(current_render_api);
				(*api->add_xa_data)(&allocs, current_render_api, 0, NULL, delete_rendapi);
			}
		}

		if (!current_render_api)
			ret = ENOMEM;
		else
		{
			*rapi = current_render_api;
			(*rapi)->h.links++;
		}
	}
	else
		ret = EBADARG;

	return ret;
}

static long _cdecl
close(struct object_render_api *rapi)
{
	long ret = E_OK;
	DIAGS(("close: %lx", (unsigned long)rapi));
	if (rapi)
	{
		struct object_render_api *r = (*api->lookup_xa_data)(&allocs, rapi);

		if (r)
		{
			r->h.links--;
			if (!r->h.links)
			{

				DIAGS((" --- free api %lx", (unsigned long)r));
			#if 1
				if (r == current_render_api)
					current_render_api = NULL;
				(*api->delete_xa_data)(&allocs, r);
			#endif
			}
		}
		else
			ret = ENXIO;
	}
	else
		ret = EBADARG;

	return ret;
}

static void _cdecl
delete_theme(void *_theme)
{
	struct theme *theme = _theme;

	DIAGS(("deleting theme %lx", (unsigned long)_theme));

	free_theme_resources(theme);

	(*api->kfree)(_theme);
}

static long _cdecl
new_theme(void **themeptr)
{
	long ret = E_OK;

	if (themeptr)
	{
		if (!current_theme)
		{
			current_theme = (*api->kmalloc)(sizeof(struct theme));
			if (current_theme)
			{
				if (screen->planes < 4)
					*current_theme = mono_stdtheme;
				else
					*current_theme = stdtheme;
				(*api->add_xa_data)(&allocs, current_theme, 0, NULL, delete_theme);
				load_textures(current_theme);

				/* APJ-OS: keep the stock look (textures resolved) so a
				 * theme drop can restore it exactly, and apply the Fluent
				 * look now if a theme is already loaded */
				apj_stock_theme = *current_theme;
				apj_have_stock = 1;
				if (apj_active)
					apj_flatten_theme(current_theme);
			}
		}

		if (!current_theme)
			ret = ENOMEM;
		else
		{
			*themeptr = current_theme;
			current_theme->h.links++;
		}
	}
	else
		ret = EBADARG;

	return ret;
}

static long _cdecl
free_theme(void *theme)
{
	long ret = E_OK;
	DIAGS(("free_theme: %lx", (unsigned long)theme));
	if (theme)
	{
		struct theme *t = (*api->lookup_xa_data)(&allocs, theme);

		if (t)
		{
			t->h.links--;
			DIAGS((" --- links = %ld", t->h.links));
			if (!t->h.links)
			{
				DIAGS((" --- links 0, free theme %lx", (unsigned long)t));
			#if 1
				if (t == current_theme)
					current_theme = NULL;
				(*api->delete_xa_data)(&allocs, t);
			#endif
			}
		}
		else
			ret = ENXIO;
	}
	else
		ret = EBADARG;

	return ret;
}

struct xa_module_object_render module_objrend_apj =
{
	"APJ objrender",
	"APJ-OS Fluent object renderer",

	init_module,
	exit_module,

	open,
	close,

	new_theme,
	free_theme,
};

void
main_object_render_apj(struct xa_module_object_render **ret)
{
	if (ret)
		*ret = &module_objrend_apj;
}
