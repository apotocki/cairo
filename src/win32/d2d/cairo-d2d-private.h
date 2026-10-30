/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/* Cairo - a vector graphics library with display and print output
 *
 * Copyright � 2010 Mozilla Foundation
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
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
 * The Initial Developer of the Original Code is the Mozilla Foundation
 *
 * Contributor(s):
 *	Bas Schouten <bschouten@mozilla.com>
 */
#ifndef CAIRO_D2D_PRIVATE_H
#define CAIRO_D2D_PRIVATE_H

#ifdef CAIRO_HAS_D2D_SURFACE

#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dxgi.h>

#include "cairoint.h"
#include "cairo-surface-clipper-private.h"

#include "cairo-win32-refptr.h"
#include "cairo-d3d-backend-private.h"
#include "cairo-d2d.h"
#include "cairo-list-private.h"
#include "cairo-list-inline.h"

/* describes the type of the currently applied clip so that we can pop it */
struct d2d_clip_t;

struct _cairo_d2d_device
{
    cairo_device_t base;

    /** Raw-D3D operations of this device's kind (D3D10.1 or D3D11) */
    cairo_d3d_backend *mBackend;
    int mVRAMUsage;

    /**
     * D2D1.1 objects for the shadow-effect side pass
     * (cairo_d2d_surface_draw_shadow). Lazily created on first use and only
     * ever populated for D3D11-kind devices; the D2D device is created over
     * the backend's own D3D11 device. Released by the implicit destructor
     * in reverse declaration order (effect -> context -> device), which in
     * cairo_release_device() happens before the backend is destroyed.
     * These must not be used by any other rendering path - cairo's own
     * rendering stays on the per-surface legacy render targets.
     */
    RefPtr<ID2D1Device> mD2DDevice;
    RefPtr<ID2D1DeviceContext> mD2DContext;
    RefPtr<ID2D1Effect> mShadowEffect;
};

typedef struct _cairo_d2d_device cairo_d2d_device_t;

struct _cairo_d2d_surface {
    _cairo_d2d_surface() : d2d_clip(NULL), clipping(false), isDrawing(false),
            textRenderingState(TEXT_RENDERING_UNINITIALIZED)
    {
        clip = _cairo_clip_create();
	    //_cairo_clip_init (&this->clip);
        cairo_list_init(&this->dependent_surfaces);
    }
    
    ~_cairo_d2d_surface();


    cairo_surface_t base;
    /* Device used by this surface
     * NOTE: In upstream cairo this is in the surface base class */
    cairo_d2d_device_t *device;

    /** Render target of the texture we render to */
    RefPtr<ID2D1RenderTarget> rt;
    /** Surface containing our backstore (texture of the device's D3D kind) */
    RefPtr<IUnknown> surface;
    /**
     * Surface used to temporarily store our surface if a bitmap isn't available
     * straight from our render target surface.
     */
    RefPtr<IUnknown> bufferTexture;
    /** Backbuffer surface hwndrt renders to (NULL if not a window surface) */
    RefPtr<IDXGISurface> backBuf;
    /** Bitmap shared with texture and rendered to by rt */
    RefPtr<ID2D1Bitmap> surfaceBitmap;
    /** Swap chain holding our backbuffer (NULL if not a window surface) */
    RefPtr<IDXGISwapChain> dxgiChain;
    /** Window handle of the window we belong to */
    HWND hwnd;
    /** Format of the surface */
    cairo_format_t format;

    cairo_clip_t* clip;
    d2d_clip_t *d2d_clip;


    /** Mask layer used by surface_mask to push opacity masks */
    RefPtr<ID2D1Layer> maskLayer;
    /**
     * Layer used for clipping when tiling, and also for clearing out geometries
     * - lazily initialized 
     */
    RefPtr<ID2D1Layer> helperLayer;
    /** If this layer currently is clipping, used to prevent excessive push/pops */
    bool clipping;
    /** Brush used for bitmaps */
    RefPtr<ID2D1BitmapBrush> bitmapBrush;
    /** Brush used for solid colors */
    RefPtr<ID2D1SolidColorBrush> solidColorBrush;
    /** Indicates if our render target is currently in drawing mode */
    bool isDrawing;
    /** Indicates if text rendering is initialized */
    enum TextRenderingState {
        TEXT_RENDERING_UNINITIALIZED,
        TEXT_RENDERING_NO_CLEARTYPE,
        TEXT_RENDERING_NORMAL,
        TEXT_RENDERING_GDI_CLASSIC
    };
    TextRenderingState textRenderingState;

    /** Backend render-target/shader-resource view caches (see
     * cairo_d3d_backend draw operations) */
    RefPtr<IUnknown> buffer_rt_view;
    RefPtr<IUnknown> buffer_sr_view;

    // Other d2d surfaces which depend on this one and need to be flushed if
    // it is drawn to. This is required for situations where this surface is
    // drawn to another surface, but may be modified before the other surface
    // has flushed. When the flush of the other surface then happens and the
    // drawing command is actually executed, the contents of this surface will
    // no longer be what it was when the drawing command was issued.
    cairo_list_t dependent_surfaces;
    //cairo_surface_clipper_t clipper;
};
typedef struct _cairo_d2d_surface cairo_d2d_surface_t;

struct _cairo_d2d_surface_entry
{
    cairo_list_t link;
    cairo_d2d_surface_t *surface;
};

typedef HRESULT (WINAPI*D2D1CreateFactoryFunc)(
    D2D1_FACTORY_TYPE factoryType,
    REFIID iid,
    CONST D2D1_FACTORY_OPTIONS *pFactoryOptions,
    void **factory
);

#endif /* CAIRO_HAS_D2D_SURFACE */
#endif /* CAIRO_D2D_PRIVATE_H */
