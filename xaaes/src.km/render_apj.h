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

#endif /* _render_apj_h_ */
