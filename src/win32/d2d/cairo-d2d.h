/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2005 Red Hat, Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is Red Hat, Inc.
 *
 * Contributor(s):
 *	Owen Taylor <otaylor@redhat.com>
 */

#ifndef _CAIRO_D2D_H_
#define _CAIRO_D2D_H_

#include "cairo-win32.h"

#if CAIRO_HAS_WIN32_SURFACE

CAIRO_BEGIN_DECLS

cairo_public cairo_surface_t *
cairo_win32_surface_create_with_alpha (HDC hdc);

cairo_public HDC
cairo_win32_get_dc_with_clip (cairo_t *cr);

cairo_public cairo_surface_t *
cairo_win32_surface_get_image (cairo_surface_t *surface);

cairo_public cairo_status_t
cairo_win32_surface_set_can_convert_to_dib (cairo_surface_t *surface, cairo_bool_t can_convert);

cairo_public cairo_status_t
cairo_win32_surface_get_can_convert_to_dib (cairo_surface_t *surface, cairo_bool_t *can_convert);

BYTE cairo_win32_get_system_text_quality (void);

cairo_public int
cairo_win32_surface_get_width (cairo_surface_t *surface);

cairo_public int
cairo_win32_surface_get_height (cairo_surface_t *surface);


#if 0
#if CAIRO_HAS_DWRITE_FONT

/*
 * Win32 DirectWrite font support
 */

void
cairo_dwrite_scaled_font_allow_manual_show_glyphs(cairo_scaled_font_t *dwrite_scaled_font, cairo_bool_t allowed);

void
cairo_dwrite_scaled_font_set_force_GDI_classic(cairo_scaled_font_t *dwrite_scaled_font, cairo_bool_t force);

cairo_bool_t
cairo_dwrite_scaled_font_get_force_GDI_classic(cairo_scaled_font_t *dwrite_scaled_font);

void
cairo_dwrite_set_cleartype_params(FLOAT gamma, FLOAT contrast, FLOAT level, int geometry, int mode);

int
cairo_dwrite_get_cleartype_rendering_mode();

#endif /* CAIRO_HAS_DWRITE_FONT */
#endif

struct IDirect3DSurface9;
cairo_public cairo_surface_t *
cairo_win32_surface_create_with_d3dsurface9 (struct IDirect3DSurface9 *surface);



CAIRO_END_DECLS

#else  /* CAIRO_HAS_WIN32_SURFACE */
# error Cairo was not compiled with support for the win32 backend
#endif /* CAIRO_HAS_WIN32_SURFACE */

#endif /* _CAIRO_D2D_H_ */
