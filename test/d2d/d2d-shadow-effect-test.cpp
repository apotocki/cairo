/* Cairo - a vector graphics library with display and print output
 *
 * Copyright (C) 2026 Alexander A Pototskiy
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
 */

/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/*
 * Standalone console tool: exercises cairo_d2d_surface_draw_shadow() on a
 * D3D11-backed device and verifies the result by pixel readback. Also
 * checks the error paths (D3D10-backed device, dst == source).
 *
 * Not part of the cairo library itself; compile it standalone against a
 * cairo build with CAIRO_HAS_D2D_SURFACE, mirroring
 * d3d-backend-compare-test.cpp, from the root of the source tree:
 *
 *   cl /EHsc /std:c++17 /I <cairo-build-dir>\src ^
 *      /I src /I src\win32 ^
 *      test\d2d\d2d-shadow-effect-test.cpp ^
 *      /link <path-to-cairo-lib>.lib d2d1.lib dwrite.lib dxgi.lib ^
 *      d3d10_1.lib d3d11.lib ole32.lib
 *
 * Build against a Debug cairo (DEBUG defined) to get the D2D debug layer:
 * a clean run has zero D2D runtime errors (two D2D targets over one
 * texture, strictly serialized).
 *
 * Exit code 0 means all checks passed.
 */

#define _USE_MATH_DEFINES

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <vector>

#include "cairo.h"
#include "cairo-win32.h"

namespace {

const int kSize = 300;

/* Opaque rounded rect drawn into the source surface. */
const double kRectX = 100;
const double kRectY = 100;
const double kRectW = 100;
const double kRectH = 100;
const double kRectR = 12;

/* Shadow parameters passed to cairo_d2d_surface_draw_shadow(). */
const double kOffsetX = 4;
const double kOffsetY = 4;
const double kSigma = 6;

int gFailures = 0;

void
check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
	gFailures++;
    }
}

struct PixelBuffer
{
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<unsigned char> data;

    unsigned int pixel(int x, int y) const
    {
	return *reinterpret_cast<const unsigned int *>(
	    &data[(size_t)y * stride + (size_t)x * 4]);
    }
};

/* Reads an ARGB32 d2d surface back into a plain pixel buffer via the
 * public cairo_surface_map_to_image() API. */
bool
read_back(cairo_surface_t *surface, PixelBuffer *out)
{
    cairo_surface_t *image = cairo_surface_map_to_image(surface, NULL);
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "map_to_image failed: %d\n", cairo_surface_status(image));
	cairo_surface_destroy(image);
	return false;
    }

    out->width = cairo_image_surface_get_width(image);
    out->height = cairo_image_surface_get_height(image);
    out->stride = cairo_image_surface_get_stride(image);
    unsigned char *data = cairo_image_surface_get_data(image);
    out->data.assign(data, data + (size_t)out->stride * out->height);

    cairo_surface_unmap_image(surface, image);
    return true;
}

void
rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI_2);
    cairo_close_path(cr);
}

/* Fills the source surface fully transparent with an opaque rounded rect
 * in the middle, through normal cairo drawing. */
void
draw_source_content(cairo_surface_t *source)
{
    cairo_t *cr = cairo_create(source);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 0.2, 0.4, 0.8, 1.0);
    rounded_rect_path(cr, kRectX, kRectY, kRectW, kRectH, kRectR);
    cairo_fill(cr);
    cairo_destroy(cr);
}

void
fill_white(cairo_surface_t *surface)
{
    cairo_t *cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_destroy(cr);
}

bool
run_d3d11_battery()
{
    cairo_device_t *device = cairo_d2d_create_device11();
    if (!device || cairo_device_status(device) != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "[d3d11] device creation failed\n");
	return false;
    }

    cairo_surface_t *source = cairo_d2d_surface_create(device, CAIRO_FORMAT_ARGB32, kSize, kSize);
    cairo_surface_t *dst = cairo_d2d_surface_create(device, CAIRO_FORMAT_ARGB32, kSize, kSize);
    if (cairo_surface_status(source) != CAIRO_STATUS_SUCCESS ||
	cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "[d3d11] surface creation failed\n");
	return false;
    }

    draw_source_content(source);
    fill_white(dst);

    check(cairo_d2d_surface_draw_shadow(dst, dst, kOffsetX, kOffsetY, kSigma,
					0, 0, 0, 0.5) == CAIRO_STATUS_SURFACE_TYPE_MISMATCH,
	  "dst == source returns SURFACE_TYPE_MISMATCH");

    cairo_status_t status = cairo_d2d_surface_draw_shadow(dst, source,
							  kOffsetX, kOffsetY, kSigma,
							  0, 0, 0, 0.5);
    check(status == CAIRO_STATUS_SUCCESS, "draw_shadow on D3D11 device succeeds");
    if (status != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "[d3d11] draw_shadow: %s\n", cairo_status_to_string(status));
	return false;
    }

    PixelBuffer buf;
    if (!read_back(dst, &buf)) {
	return false;
    }

    /* Pixels well outside the rect + blur radius (rect edge at 100, blur
     * reach ~3*sigma + offset = ~22px) must still be pure white: check the
     * outer 40px ring. */
    bool ring_white = true;
    for (int y = 0; y < kSize && ring_white; y++) {
	for (int x = 0; x < kSize; x++) {
	    if (x >= 40 && x < kSize - 40 && y >= 40 && y < kSize - 40) {
		continue;
	    }
	    if (buf.pixel(x, y) != 0xffffffffu) {
		fprintf(stderr, "[d3d11] non-white ring pixel at (%d,%d): %08x\n",
			x, y, buf.pixel(x, y));
		ring_white = false;
		break;
	    }
	}
    }
    check(ring_white, "outer ring (outside rect+blur) stays pure white");

    /* Pixels just outside the rect edge in the offset direction are
     * darkened: right of the (offset) right edge and below the (offset)
     * bottom edge. */
    check(buf.pixel((int)(kRectX + kRectW + kOffsetX + 4), (int)(kRectY + kRectH / 2)) != 0xffffffffu,
	  "pixel just right of the offset rect edge is darkened");
    check(buf.pixel((int)(kRectX + kRectW / 2), (int)(kRectY + kRectH + kOffsetY + 4)) != 0xffffffffu,
	  "pixel just below the offset rect edge is darkened");

    /* Centroid of the darkness must sit at the rect center shifted by
     * ~(offset_x, offset_y) - the blur is symmetric around it. */
    double sum = 0, sum_x = 0, sum_y = 0;
    for (int y = 0; y < kSize; y++) {
	for (int x = 0; x < kSize; x++) {
	    unsigned int p = buf.pixel(x, y);
	    /* dst is opaque white + black shadow: darkness of any one
	     * channel measures the shadow's coverage. */
	    double darkness = 255.0 - (p & 0xff);
	    sum += darkness;
	    sum_x += darkness * x;
	    sum_y += darkness * y;
	}
    }
    bool centroid_ok = false;
    if (sum > 0) {
	double cx = sum_x / sum;
	double cy = sum_y / sum;
	double ex = kRectX + kRectW / 2 + kOffsetX;
	double ey = kRectY + kRectH / 2 + kOffsetY;
	centroid_ok = fabs(cx - ex) < 1.5 && fabs(cy - ey) < 1.5;
	if (!centroid_ok) {
	    fprintf(stderr, "[d3d11] centroid (%.2f,%.2f), expected (%.2f,%.2f)\n",
		    cx, cy, ex, ey);
	}
    }
    check(centroid_ok, "shadow centroid is shifted by ~(offset_x, offset_y)");

    /* Cairo drawing onto dst AFTER the shadow pass must still work and
     * composite over the shadow (exercises the handoff back to cairo). */
    {
	cairo_t *cr = cairo_create(dst);
	cairo_set_source_rgb(cr, 1, 0, 0);
	cairo_rectangle(cr, 120, 120, 60, 60);
	cairo_fill(cr);
	cairo_destroy(cr);
    }
    PixelBuffer buf2;
    if (!read_back(dst, &buf2)) {
	return false;
    }
    check(buf2.pixel(150, 150) == 0xffff0000u,
	  "cairo drawing after the shadow pass lands on dst");
    check(buf2.pixel((int)(kRectX + kRectW + kOffsetX + 4), (int)(kRectY + kRectH / 2)) ==
	      buf.pixel((int)(kRectX + kRectW + kOffsetX + 4), (int)(kRectY + kRectH / 2)),
	  "shadow outside the new rect survives the later cairo drawing");

    cairo_surface_destroy(source);
    cairo_surface_destroy(dst);
    cairo_d2d_finish_device(device);
    cairo_release_device(device);
    return true;
}

bool
run_d3d10_battery()
{
    cairo_device_t *device = cairo_d2d_create_device();
    if (!device || cairo_device_status(device) != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "[d3d10] device creation failed\n");
	return false;
    }

    cairo_surface_t *source = cairo_d2d_surface_create(device, CAIRO_FORMAT_ARGB32, kSize, kSize);
    cairo_surface_t *dst = cairo_d2d_surface_create(device, CAIRO_FORMAT_ARGB32, kSize, kSize);
    if (cairo_surface_status(source) != CAIRO_STATUS_SUCCESS ||
	cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "[d3d10] surface creation failed\n");
	return false;
    }

    draw_source_content(source);
    fill_white(dst);

    check(cairo_d2d_surface_draw_shadow(dst, source, kOffsetX, kOffsetY, kSigma,
					0, 0, 0, 0.5) == CAIRO_STATUS_DEVICE_TYPE_MISMATCH,
	  "draw_shadow on D3D10 device returns DEVICE_TYPE_MISMATCH");

    /* The failed call must leave the surfaces usable. */
    {
	cairo_t *cr = cairo_create(dst);
	cairo_set_source_rgb(cr, 0, 1, 0);
	cairo_rectangle(cr, 10, 10, 50, 50);
	cairo_fill(cr);
	cairo_destroy(cr);
    }
    PixelBuffer buf;
    bool usable = read_back(dst, &buf) &&
	buf.pixel(30, 30) == 0xff00ff00u &&
	buf.pixel(200, 200) == 0xffffffffu;
    check(usable, "surfaces stay usable after the D3D10 mismatch");

    cairo_surface_destroy(source);
    cairo_surface_destroy(dst);
    cairo_d2d_finish_device(device);
    cairo_release_device(device);
    return true;
}

} // namespace

int
main()
{
    bool ok11 = run_d3d11_battery();
    bool ok10 = run_d3d10_battery();

    if (!ok11 || !ok10 || gFailures) {
	printf("\nFAILED (%d check(s) failed%s)\n", gFailures,
	       (!ok11 || !ok10) ? ", and a battery aborted early" : "");
	return 1;
    }
    printf("\nAll checks passed.\n");
    return 0;
}
