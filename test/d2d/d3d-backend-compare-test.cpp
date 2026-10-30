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
 * Standalone console tool: renders the same battery of primitives on a
 * cairo-d2d D3D10.1-backed device and a D3D11-backed device, then
 * compares the two offscreen surfaces pixel-by-pixel.
 *
 * Not part of the cairo library itself; link it against cairo (built
 * with CAIRO_HAS_D2D_SURFACE) and dwrite.lib, and run it standalone.
 * Exit code 0 means the two backends rendered within tolerance; nonzero
 * means a real difference (or that a battery step failed) was found.
 */

#include <windows.h>
#include <dwrite.h>
#include <stdio.h>
#include <vector>
#include <string>

#include "cairo.h"
#include "cairo-win32.h"
#include "cairo-dwrite.h"

namespace {

const int kCanvasWidth = 900;
const int kCanvasHeight = 700;
/** Per-channel tolerance; blend-mode/AA rounding differs slightly between
 * the two backends' shader compiles even though the math is identical. */
const int kChannelTolerance = 3;
/** Fraction of pixels allowed to exceed the tolerance (edge/AA noise). */
const double kMaxMismatchFraction = 0.002;

struct PixelBuffer
{
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<unsigned char> data;
};

struct Operator
{
    cairo_operator_t op;
    const char *name;
};

const Operator kOperators[] = {
    { CAIRO_OPERATOR_OVER,        "OVER" },
    { CAIRO_OPERATOR_ADD,         "ADD" },
    { CAIRO_OPERATOR_IN,          "IN" },
    { CAIRO_OPERATOR_OUT,         "OUT" },
    { CAIRO_OPERATOR_ATOP,        "ATOP" },
    { CAIRO_OPERATOR_DEST,        "DEST" },
    { CAIRO_OPERATOR_DEST_OVER,   "DEST_OVER" },
    { CAIRO_OPERATOR_DEST_IN,     "DEST_IN" },
    { CAIRO_OPERATOR_DEST_OUT,    "DEST_OUT" },
    { CAIRO_OPERATOR_DEST_ATOP,   "DEST_ATOP" },
    { CAIRO_OPERATOR_XOR,         "XOR" },
    { CAIRO_OPERATOR_MULTIPLY,    "MULTIPLY" },
    { CAIRO_OPERATOR_SCREEN,      "SCREEN" },
    { CAIRO_OPERATOR_OVERLAY,     "OVERLAY" },
    { CAIRO_OPERATOR_DARKEN,      "DARKEN" },
    { CAIRO_OPERATOR_LIGHTEN,     "LIGHTEN" },
    { CAIRO_OPERATOR_COLOR_DODGE, "COLOR_DODGE" },
    { CAIRO_OPERATOR_COLOR_BURN,  "COLOR_BURN" },
    { CAIRO_OPERATOR_HARD_LIGHT,  "HARD_LIGHT" },
    { CAIRO_OPERATOR_SOFT_LIGHT,  "SOFT_LIGHT" },
    { CAIRO_OPERATOR_DIFFERENCE,  "DIFFERENCE" },
    { CAIRO_OPERATOR_EXCLUSION,   "EXCLUSION" },
};
const int kNumOperators = sizeof(kOperators) / sizeof(kOperators[0]);
const int kGridCols = 6;
const int kCellSize = 110;
const int kGridTop = 10;

/**
 * Creates a cairo DWrite font face for a system font family, or NULL on
 * failure (e.g. the family isn't installed). Caller destroys with
 * cairo_font_face_destroy().
 */
cairo_font_face_t *
create_system_dwrite_font_face(const wchar_t *family_name)
{
    IDWriteFactory *factory = NULL;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
	__uuidof(IDWriteFactory),
	reinterpret_cast<IUnknown**>(&factory));
    if (FAILED(hr)) {
	return NULL;
    }

    IDWriteFontCollection *collection = NULL;
    hr = factory->GetSystemFontCollection(&collection);
    if (FAILED(hr)) {
	factory->Release();
	return NULL;
    }

    UINT32 index = 0;
    BOOL exists = FALSE;
    collection->FindFamilyName(family_name, &index, &exists);

    cairo_font_face_t *result = NULL;
    if (exists) {
	IDWriteFontFamily *fontFamily = NULL;
	if (SUCCEEDED(collection->GetFontFamily(index, &fontFamily))) {
	    IDWriteFont *font = NULL;
	    if (SUCCEEDED(fontFamily->GetFirstMatchingFont(
		    DWRITE_FONT_WEIGHT_NORMAL,
		    DWRITE_FONT_STRETCH_NORMAL,
		    DWRITE_FONT_STYLE_NORMAL,
		    &font))) {
		IDWriteFontFace *fontFace = NULL;
		if (SUCCEEDED(font->CreateFontFace(&fontFace))) {
		    result = cairo_dwrite_font_face_create_for_dwrite_fontface(fontFace);
		    fontFace->Release();
		}
		font->Release();
	    }
	    fontFamily->Release();
	}
    }

    collection->Release();
    factory->Release();
    return result;
}

void
draw_operator_grid(cairo_t *cr)
{
    for (int i = 0; i < kNumOperators; i++) {
	int col = i % kGridCols;
	int row = i / kGridCols;
	double x = 10 + col * kCellSize;
	double y = kGridTop + row * kCellSize;

	cairo_save(cr);
	cairo_rectangle(cr, x, y, kCellSize - 10, kCellSize - 10);
	cairo_clip(cr);

	// Background: an opaque gradient-ish pattern (two solid rects) so
	// DEST_* operators have non-trivial content to preserve/remove.
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.2, 0.5, 0.9, 1.0);
	cairo_rectangle(cr, x, y, kCellSize - 10, (kCellSize - 10) / 2.0);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 0.9, 0.7, 0.1, 1.0);
	cairo_rectangle(cr, x, y + (kCellSize - 10) / 2.0, kCellSize - 10, (kCellSize - 10) / 2.0);
	cairo_fill(cr);

	// Foreground: semi-transparent circle drawn with the operator under
	// test. Hits _cairo_d2d_set_operator (hardware blend state) for
	// Porter-Duff operators and _cairo_d2d_blend_temp_surface_blendmode
	// (BlendTexture pixel shader) for the CSS blend-mode operators.
	cairo_set_operator(cr, kOperators[i].op);
	cairo_set_source_rgba(cr, 0.8, 0.1, 0.3, 0.6);
	cairo_arc(cr, x + (kCellSize - 10) / 2.0, y + (kCellSize - 10) / 2.0,
	    (kCellSize - 10) / 2.5, 0, 2 * 3.14159265358979);
	cairo_fill(cr);

	cairo_restore(cr);
    }
}

void
draw_text_battery(cairo_t *cr, cairo_font_face_t *font_face)
{
    int rows = (kNumOperators + kGridCols - 1) / kGridCols;
    double text_top = kGridTop + rows * kCellSize + 20;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_rectangle(cr, 0, text_top, kCanvasWidth, 90);
    cairo_fill(cr);

    if (font_face) {
	cairo_set_font_face(cr, font_face);
    }
    cairo_set_font_size(cr, 28);

    // ClearType text on an opaque background.
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_move_to(cr, 20, text_top + 40);
    cairo_show_text(cr, "The quick brown fox jumps 0123456789");

    // ClearType/grayscale text on a transparent background (this surface
    // is ARGB32, so text drawn without an opaque backing takes the
    // permit_subpixel_antialiasing==false / grayscale fallback path).
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.6);
    cairo_move_to(cr, 20, text_top + 75);
    cairo_show_text(cr, "Transparent background text sample");

    cairo_restore(cr);
}

void
draw_surface_pattern_battery(cairo_device_t *device, cairo_t *cr)
{
    int rows = (kNumOperators + kGridCols - 1) / kGridCols;
    double top = kGridTop + rows * kCellSize + 120;

    // A small source surface with its own content, used as a surface
    // pattern below (hits the D2D-bitmap / fastblit / SampleTexture
    // composite paths depending on operator and transform).
    cairo_surface_t *small = cairo_d2d_surface_create(device, CAIRO_FORMAT_ARGB32, 80, 80);
    cairo_t *scr = cairo_create(small);
    cairo_set_source_rgba(scr, 1, 1, 1, 0);
    cairo_paint(scr);
    cairo_set_source_rgba(scr, 0.9, 0.2, 0.2, 1.0);
    cairo_rectangle(scr, 0, 0, 40, 80);
    cairo_fill(scr);
    cairo_set_source_rgba(scr, 0.2, 0.2, 0.9, 1.0);
    cairo_rectangle(scr, 40, 0, 40, 80);
    cairo_fill(scr);
    cairo_destroy(scr);

    // OVER + integer translation: hits _cairo_d2d_try_fastblit's
    // CopySubresourceRegion path (_cairo_d2d_copy_surface).
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_surface(cr, small, 20, top);
    cairo_rectangle(cr, 20, top, 80, 80);
    cairo_fill(cr);
    cairo_restore(cr);

    // Non-OVER operator with a surface pattern: falls through fastblit
    // (unsupported for non-OVER) to the general temp-rt path, hitting
    // draw_sample_texture with a D2D bitmap-brush-filled temp surface.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
    cairo_set_source_surface(cr, small, 140, top);
    cairo_rectangle(cr, 140, top, 80, 80);
    cairo_fill(cr);
    cairo_restore(cr);

    // Non-integer transform (scale): also excluded from fastblit, forces
    // the brush + temp-rt composite path with a non-trivial matrix.
    cairo_save(cr);
    cairo_translate(cr, 260, top);
    cairo_scale(cr, 1.5, 1.5);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_surface(cr, small, 0, 0);
    cairo_rectangle(cr, 0, 0, 80, 80);
    cairo_fill(cr);
    cairo_restore(cr);

    cairo_surface_destroy(small);
}

/**
 * Renders the whole battery for one device kind into an offscreen d2d
 * surface and reads it back into a plain pixel buffer via the public
 * cairo_surface_map_to_image() API (works even though this backend
 * doesn't implement map_to_image directly: cairo core falls back to
 * acquire_source_image, which is implemented).
 */
bool
render_battery(cairo_device_t *device, const char *label, PixelBuffer *out)
{
    if (!device) {
	fprintf(stderr, "[%s] device creation failed (feature/driver not available?)\n", label);
	return false;
    }
    if (cairo_device_status(device) != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "[%s] device status error: %d\n", label, cairo_device_status(device));
	cairo_release_device(device);
	return false;
    }

    cairo_surface_t *surface = cairo_d2d_surface_create(device, CAIRO_FORMAT_ARGB32,
	kCanvasWidth, kCanvasHeight);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "[%s] surface creation failed: %d\n", label, cairo_surface_status(surface));
	cairo_surface_destroy(surface);
	cairo_release_device(device);
	return false;
    }

    cairo_font_face_t *font_face = create_system_dwrite_font_face(L"Segoe UI");
    if (!font_face) {
	fprintf(stderr, "[%s] warning: could not create a DWrite font face; "
	    "text battery will use whatever the default toy font resolves to "
	    "(may not exercise the ClearType fast path)\n", label);
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    draw_operator_grid(cr);
    draw_text_battery(cr, font_face);
    draw_surface_pattern_battery(device, cr);

    cairo_status_t status = cairo_status(cr);
    cairo_destroy(cr);
    if (font_face) {
	cairo_font_face_destroy(font_face);
    }
    if (status != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "[%s] drawing failed: %s\n", label, cairo_status_to_string(status));
	cairo_surface_destroy(surface);
	cairo_release_device(device);
	return false;
    }

    cairo_surface_t *image = cairo_surface_map_to_image(surface, NULL);
    if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
	fprintf(stderr, "[%s] map_to_image failed: %d\n", label, cairo_surface_status(image));
	cairo_surface_destroy(image);
	cairo_surface_destroy(surface);
	cairo_release_device(device);
	return false;
    }

    out->width = cairo_image_surface_get_width(image);
    out->height = cairo_image_surface_get_height(image);
    out->stride = cairo_image_surface_get_stride(image);
    unsigned char *data = cairo_image_surface_get_data(image);
    out->data.assign(data, data + (size_t)out->stride * out->height);

    cairo_surface_unmap_image(surface, image);
    cairo_surface_destroy(surface);

    // Wait for all GPU work (including the readback copy) to fully
    // retire before tearing the device down, mirroring how an external
    // consumer synchronizing across devices would use this entry point.
    cairo_d2d_finish_device(device);
    cairo_release_device(device);
    return true;
}

/**
 * Compares two same-size ARGB32 buffers with a per-channel tolerance,
 * printing a summary. Returns true if the mismatch fraction is within
 * kMaxMismatchFraction.
 */
bool
compare_buffers(const PixelBuffer &a, const PixelBuffer &b)
{
    if (a.width != b.width || a.height != b.height) {
	fprintf(stderr, "size mismatch: %dx%d vs %dx%d\n", a.width, a.height, b.width, b.height);
	return false;
    }

    long long mismatched = 0;
    long long total = (long long)a.width * a.height;
    int worst_diff = 0;

    for (int y = 0; y < a.height; y++) {
	const unsigned char *rowA = a.data.data() + (size_t)y * a.stride;
	const unsigned char *rowB = b.data.data() + (size_t)y * b.stride;
	for (int x = 0; x < a.width; x++) {
	    const unsigned char *pa = rowA + x * 4;
	    const unsigned char *pb = rowB + x * 4;
	    int diff = 0;
	    for (int c = 0; c < 4; c++) {
		int d = abs((int)pa[c] - (int)pb[c]);
		if (d > diff) diff = d;
	    }
	    if (diff > worst_diff) worst_diff = diff;
	    if (diff > kChannelTolerance) {
		mismatched++;
	    }
	}
    }

    double fraction = total ? (double)mismatched / (double)total : 0.0;
    printf("compared %lldx%d pixels: %lld mismatched (%.4f%%), worst per-channel diff %d\n",
	(long long)a.width, a.height, mismatched, fraction * 100.0, worst_diff);

    return fraction <= kMaxMismatchFraction;
}

} // namespace

int
main()
{
    printf("cairo-d2d D3D10 vs D3D11 backend comparison test\n");

    PixelBuffer d3d10_image, d3d11_image;

    printf("-- rendering on D3D10.1 backend --\n");
    bool ok10 = render_battery(cairo_d2d_create_device(), "d3d10", &d3d10_image);

    printf("-- rendering on D3D11 backend --\n");
    bool ok11 = render_battery(cairo_d2d_create_device11(), "d3d11", &d3d11_image);

    if (!ok10 || !ok11) {
	fprintf(stderr, "FAIL: one or both backends failed to render "
	    "(see messages above)\n");
	return 1;
    }

    bool match = compare_buffers(d3d10_image, d3d11_image);
    if (!match) {
	fprintf(stderr, "FAIL: D3D10 and D3D11 output differs beyond tolerance\n");
	return 1;
    }

    printf("PASS: D3D10 and D3D11 output matches within tolerance\n");
    return 0;
}
