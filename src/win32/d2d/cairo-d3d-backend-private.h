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
 */
#ifndef CAIRO_D3D_BACKEND_PRIVATE_H
#define CAIRO_D3D_BACKEND_PRIVATE_H

#ifdef CAIRO_HAS_D2D_SURFACE

/* NOTE: cairoint.h must be included before this header (cairo_operator_t,
 * cairo_int_status_t). */

#include <windows.h>
#include <dxgi.h>

#include "cairo-win32-refptr.h"

/**
 * Internal abstraction over the raw-D3D operations of the cairo-d2d
 * backend. Everything D2D-level in cairo-d2d-surface.cpp is D3D-version
 * agnostic (D2D interops through DXGI); the operations below are the only
 * places where the D3D version matters. There is one implementation per
 * device kind: cairo-d3d10-backend.cpp (extracted original behavior) and
 * cairo-d3d11-backend.cpp.
 *
 * Textures, render-target views and shader-resource views cross this
 * interface as IUnknown pointers; each implementation casts back to its
 * own typed interfaces. DXGI-level access (IDXGISurface & co) is done by
 * the caller via QueryInterface, which works for both D3D versions.
 */

enum cairo_d3d_backend_kind_t {
    CAIRO_D3D_BACKEND_D3D10,
    CAIRO_D3D_BACKEND_D3D11
};

enum cairo_d3d_texture_usage_t {
    CAIRO_D3D_TEXTURE_USAGE_DEFAULT,
    CAIRO_D3D_TEXTURE_USAGE_IMMUTABLE,
    CAIRO_D3D_TEXTURE_USAGE_STAGING
};

/** Backend-neutral 2D texture description (single mip, single sample). */
struct cairo_d3d_texture_desc_t {
    DXGI_FORMAT format;
    UINT width;
    UINT height;
    cairo_d3d_texture_usage_t usage;
    bool bind_render_target;
    bool bind_shader_resource;
    bool cpu_read;
    bool cpu_write;
    /** D3D1x_RESOURCE_MISC_GDI_COMPATIBLE */
    bool gdi_compatible;

    cairo_d3d_texture_desc_t()
	: format(DXGI_FORMAT_B8G8R8A8_UNORM), width(0), height(0),
	  usage(CAIRO_D3D_TEXTURE_USAGE_DEFAULT),
	  bind_render_target(false), bind_shader_resource(false),
	  cpu_read(false), cpu_write(false), gdi_compatible(false)
    {
    }
};

struct cairo_d3d_mapped_texture_t {
    void *data;
    UINT row_pitch;
};

/** Texture used for manual glyph rendering, owned by the backend. */
const unsigned int TEXT_TEXTURE_WIDTH = 2048;
const unsigned int TEXT_TEXTURE_HEIGHT = 512;

struct cairo_d3d_backend
{
    virtual ~cairo_d3d_backend() {}

    virtual cairo_d3d_backend_kind_t kind() const = 0;

    /**
     * IUnknown of the underlying D3D device for DXGI-level QI
     * (IDXGIDevice, swap chain creation, ...). Not AddRef'ed.
     */
    virtual IUnknown *device_unknown() const = 0;

    /**
     * The typed device pointer (ID3D10Device1* resp. ID3D11Device*) for
     * the public getters. Not AddRef'ed.
     */
    virtual void *native_device() const = 0;

    /**
     * The typed texture interface (ID3D10Texture2D* resp. ID3D11Texture2D*)
     * of a resource of this backend's D3D version, NULL if the resource
     * does not belong to it. Not AddRef'ed.
     */
    virtual void *native_texture(IUnknown *resource) const = 0;

    /** Flush the device's command queue. */
    virtual void flush() = 0;

    /**
     * Block until the GPU is completely done with all submitted commands
     * (event-query spin, bails out on device removal).
     */
    virtual void finish() = 0;

    /**
     * Create a 2D texture. init_data/init_pitch may be NULL/0.
     */
    virtual HRESULT create_texture(const cairo_d3d_texture_desc_t &desc,
				   const void *init_data,
				   UINT init_pitch,
				   RefPtr<IUnknown> &texture_out) = 0;

    /** Open a shared resource handle as a resource of this D3D version. */
    virtual HRESULT open_shared_resource(HANDLE handle,
					 RefPtr<IUnknown> &resource_out) = 0;

    /**
     * Get the description of a texture of this backend's D3D version.
     * Returns false if the texture does not belong to this D3D version.
     */
    virtual bool get_texture_desc(IUnknown *texture,
				  cairo_d3d_texture_desc_t &desc_out) = 0;

    /** Create a shader resource view for a texture (whole resource). */
    virtual HRESULT create_srv(IUnknown *texture,
			       RefPtr<IUnknown> &srv_out) = 0;

    virtual void copy_resource(IUnknown *dst, IUnknown *src) = 0;

    /** Copy [left,top,right,bottom) of subresource 0 of src to (dst_x,dst_y). */
    virtual void copy_subresource_region(IUnknown *dst,
					 UINT dst_x, UINT dst_y,
					 IUnknown *src,
					 UINT left, UINT top,
					 UINT right, UINT bottom) = 0;

    /**
     * Map subresource 0 of a staging texture. write_only maps for write
     * access only, otherwise read/write.
     */
    virtual HRESULT map(IUnknown *texture, bool write_only,
			cairo_d3d_mapped_texture_t *mapped_out) = 0;
    virtual void unmap(IUnknown *texture) = 0;

    /**
     * Device-owned VRAM texture used for manual glyph rendering
     * (TEXT_TEXTURE_WIDTH x TEXT_TEXTURE_HEIGHT, B8G8R8A8) and its SRV.
     * Not AddRef'ed.
     */
    virtual IUnknown *text_texture() const = 0;
    virtual IUnknown *text_texture_srv() const = 0;

    /*
     * Whole-draw operations. Each implementation owns its shaders, input
     * layout, quad vertex buffer and rasterizer/blend/sampler states.
     * quad_desc/tex_coords/text_color follow the semantics of the fx
     * variables QuadDesc/TexCoords/TextColor in cairo-d2d-blend.fx.
     * dst_rtv_cache/src_srv_cache are per-surface view caches, created on
     * first use.
     */

    /**
     * Draw src_texture to dst_texture with the SampleTexture shader and
     * the hardware blend state for the given cairo operator.
     */
    virtual cairo_int_status_t
    draw_sample_texture(IUnknown *dst_texture,
			RefPtr<IUnknown> &dst_rtv_cache,
			IUnknown *src_texture,
			RefPtr<IUnknown> &src_srv_cache,
			UINT viewport_width, UINT viewport_height,
			const float quad_desc[4],
			const float tex_coords[4],
			cairo_operator_t op) = 0;

    /**
     * Composite src_texture onto dst_texture with the BlendTexture pixel
     * shader (blend mode operators; blend_mode as in the fx BlendMode
     * variable). The implementation snapshots the destination into an
     * internal copy texture read by the pixel shader; hardware blending
     * is disabled.
     */
    virtual cairo_int_status_t
    draw_blend_texture(IUnknown *dst_texture,
		       RefPtr<IUnknown> &dst_rtv_cache,
		       IUnknown *src_texture,
		       RefPtr<IUnknown> &src_srv_cache,
		       UINT viewport_width, UINT viewport_height,
		       const float quad_desc[4],
		       const float tex_coords[4],
		       int blend_mode) = 0;

    /**
     * Draw a glyph-texture quad with the SampleTextTexture shader
     * (dual-source ClearType blend state). glyph_srv is either
     * text_texture_srv() or a one-off SRV created with create_srv().
     */
    virtual cairo_int_status_t
    draw_text_texture(IUnknown *dst_texture,
		      RefPtr<IUnknown> &dst_rtv_cache,
		      IUnknown *glyph_srv,
		      UINT viewport_width, UINT viewport_height,
		      const float quad_desc[4],
		      const float tex_coords[4],
		      const float text_color[4]) = 0;
};

/**
 * Create the D3D10.1 backend around an existing device, or around a new
 * hardware device when d3d10device is NULL. Returns NULL on failure.
 */
cairo_d3d_backend *
cairo_d3d10_backend_create(struct ID3D10Device1 *d3d10device);

/**
 * Create the D3D11 backend around an existing device (must have been
 * created with D3D11_CREATE_DEVICE_BGRA_SUPPORT), or around a new
 * hardware device when d3d11device is NULL. Returns NULL on failure.
 */
cairo_d3d_backend *
cairo_d3d11_backend_create(struct ID3D11Device *d3d11device);

#endif /* CAIRO_HAS_D2D_SURFACE */
#endif /* CAIRO_D3D_BACKEND_PRIVATE_H */
