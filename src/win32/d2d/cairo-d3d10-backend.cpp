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

/*
 * D3D10.1 implementation of cairo_d3d_backend. This is the raw-D3D device
 * code extracted unchanged from cairo-d2d-surface.cpp; the drawing
 * pipeline itself (D2D) lives there and is D3D-version agnostic.
 */

#define CAIRO_HAS_D2D_SURFACE 1

#include "cairoint.h"

#include <d3d10_1.h>
#include <d3d10effect.h>
#include <d3d10misc.h>
#include <dxgi.h>

#include "cairo-d3d-backend-private.h"
#include "cairo-d2d-private-fx.h"

#define CAIRO_INT_STATUS_SUCCESS (cairo_int_status_t)CAIRO_STATUS_SUCCESS

#define MAX_OPERATORS (CAIRO_OPERATOR_HSL_LUMINOSITY + 1)

typedef HRESULT (WINAPI*D3D10CreateDevice1Func)(
  IDXGIAdapter *pAdapter,
  D3D10_DRIVER_TYPE DriverType,
  HMODULE Software,
  UINT Flags,
  D3D10_FEATURE_LEVEL1 HardwareLevel,
  UINT SDKVersion,
  ID3D10Device1 **ppDevice
);

typedef HRESULT (WINAPI*D3D10CreateEffectFromMemoryFunc)(
    void *pData,
    SIZE_T DataLength,
    UINT FXFlags,
    ID3D10Device *pDevice,
    ID3D10EffectPool *pEffectPool,
    ID3D10Effect **ppEffect
);

namespace {
struct Vertex
{
    float position[2];
};
}

struct cairo_d3d10_backend : public cairo_d3d_backend
{
    HMODULE mD3D10_1;
    RefPtr<ID3D10Device1> mD3D10Device;
    RefPtr<ID3D10Effect> mSampleEffect;
    RefPtr<ID3D10InputLayout> mInputLayout;
    RefPtr<ID3D10Buffer> mQuadBuffer;
    RefPtr<ID3D10RasterizerState> mRasterizerState;
    RefPtr<ID3D10BlendState> mBlendStates[MAX_OPERATORS];
    /** Texture used for manual glyph rendering */
    RefPtr<ID3D10Texture2D> mTextTexture;
    RefPtr<ID3D10ShaderResourceView> mTextTextureView;
    /** Texture used for reading destination in blend mode pixel shader */
    RefPtr<ID3D10Texture2D> mDstCopyTexture;
    RefPtr<ID3D10ShaderResourceView> mDstCopySRV;
    /** Blend state with blending disabled (for blend mode compositing in PS) */
    RefPtr<ID3D10BlendState> mNoBlendState;

    cairo_d3d10_backend() : mD3D10_1(NULL)
    {
    }

    virtual ~cairo_d3d10_backend()
    {
	/* Release all COM references before the module they live in is
	 * unloaded. Member destructors would only run after the body of
	 * this destructor, so release explicitly first. */
	mSampleEffect = NULL;
	mInputLayout = NULL;
	mQuadBuffer = NULL;
	mRasterizerState = NULL;
	for (int i = 0; i < MAX_OPERATORS; i++) {
	    mBlendStates[i] = NULL;
	}
	mTextTexture = NULL;
	mTextTextureView = NULL;
	mDstCopyTexture = NULL;
	mDstCopySRV = NULL;
	mNoBlendState = NULL;
	mD3D10Device = NULL;
	if (mD3D10_1) {
	    FreeLibrary(mD3D10_1);
	}
    }

    virtual cairo_d3d_backend_kind_t kind() const
    {
	return CAIRO_D3D_BACKEND_D3D10;
    }

    virtual IUnknown *device_unknown() const
    {
	return static_cast<IUnknown*>(mD3D10Device.get());
    }

    virtual void *native_device() const
    {
	return mD3D10Device.get();
    }

    virtual void *native_texture(IUnknown *resource) const
    {
	if (!resource) {
	    return NULL;
	}
	RefPtr<ID3D10Texture2D> texture;
	if (FAILED(resource->QueryInterface(&texture))) {
	    return NULL;
	}
	/* The caller holds a reference through the resource; hand out the
	 * interface pointer without an extra reference, like
	 * cairo_d2d_surface_get_texture always did. */
	return texture.get();
    }

    virtual void flush()
    {
	mD3D10Device->Flush();
    }

    virtual void finish()
    {
	// The device flush method may flush the device's command queue, but
	// it gives no guarantee that the device will actually be done with
	// those commands. The EVENT query will actually tell us when the GPU
	// is completely done with our commands.
	D3D10_QUERY_DESC queryDesc;
	queryDesc.MiscFlags = 0;
	queryDesc.Query = D3D10_QUERY_EVENT;
	RefPtr<ID3D10Query> query;

	mD3D10Device->CreateQuery(&queryDesc, &query);

	// QUERY_EVENT does not use Begin(). It's disabled.
	query->End();

	BOOL done = FALSE;
	while (!done) {
	    // This will return S_OK and done = FALSE when the GPU is not
	    // done, and S_OK and done = TRUE when the GPU is done. Any other
	    // return value means we need to break out or risk an infinite
	    // loop.
	    if (FAILED(query->GetData(&done, sizeof(BOOL), 0))) {
		break;
	    }
	    if (FAILED(mD3D10Device->GetDeviceRemovedReason())) {
		break;
	    }
	}
    }

    virtual HRESULT create_texture(const cairo_d3d_texture_desc_t &desc,
				   const void *init_data,
				   UINT init_pitch,
				   RefPtr<IUnknown> &texture_out)
    {
	D3D10_TEXTURE2D_DESC d;
	d.Width = desc.width;
	d.Height = desc.height;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = desc.format;
	d.SampleDesc.Count = 1;
	d.SampleDesc.Quality = 0;
	switch (desc.usage) {
	case CAIRO_D3D_TEXTURE_USAGE_IMMUTABLE:
	    d.Usage = D3D10_USAGE_IMMUTABLE;
	    break;
	case CAIRO_D3D_TEXTURE_USAGE_STAGING:
	    d.Usage = D3D10_USAGE_STAGING;
	    break;
	default:
	    d.Usage = D3D10_USAGE_DEFAULT;
	    break;
	}
	d.BindFlags = 0;
	if (desc.bind_render_target) {
	    d.BindFlags |= D3D10_BIND_RENDER_TARGET;
	}
	if (desc.bind_shader_resource) {
	    d.BindFlags |= D3D10_BIND_SHADER_RESOURCE;
	}
	d.CPUAccessFlags = 0;
	if (desc.cpu_read) {
	    d.CPUAccessFlags |= D3D10_CPU_ACCESS_READ;
	}
	if (desc.cpu_write) {
	    d.CPUAccessFlags |= D3D10_CPU_ACCESS_WRITE;
	}
	d.MiscFlags = desc.gdi_compatible ? D3D10_RESOURCE_MISC_GDI_COMPATIBLE : 0;

	D3D10_SUBRESOURCE_DATA data;
	data.pSysMem = init_data;
	data.SysMemPitch = init_pitch;
	data.SysMemSlicePitch = 0;

	RefPtr<ID3D10Texture2D> texture;
	HRESULT hr = mD3D10Device->CreateTexture2D(&d,
						   init_data ? &data : NULL,
						   &texture);
	if (FAILED(hr)) {
	    return hr;
	}
	texture_out = texture;
	return S_OK;
    }

    virtual HRESULT open_shared_resource(HANDLE handle,
					 RefPtr<IUnknown> &resource_out)
    {
	RefPtr<ID3D10Resource> resource;
	HRESULT hr = mD3D10Device->OpenSharedResource(handle,
						      __uuidof(ID3D10Resource),
						      (void**)&resource);
	if (FAILED(hr)) {
	    return hr;
	}
	resource_out = resource;
	return S_OK;
    }

    virtual bool get_texture_desc(IUnknown *texture,
				  cairo_d3d_texture_desc_t &desc_out)
    {
	RefPtr<ID3D10Texture2D> tex;
	if (!texture ||
	    FAILED(texture->QueryInterface(&tex))) {
	    return false;
	}
	D3D10_TEXTURE2D_DESC d;
	tex->GetDesc(&d);

	desc_out.format = d.Format;
	desc_out.width = d.Width;
	desc_out.height = d.Height;
	switch (d.Usage) {
	case D3D10_USAGE_IMMUTABLE:
	    desc_out.usage = CAIRO_D3D_TEXTURE_USAGE_IMMUTABLE;
	    break;
	case D3D10_USAGE_STAGING:
	    desc_out.usage = CAIRO_D3D_TEXTURE_USAGE_STAGING;
	    break;
	default:
	    desc_out.usage = CAIRO_D3D_TEXTURE_USAGE_DEFAULT;
	    break;
	}
	desc_out.bind_render_target = (d.BindFlags & D3D10_BIND_RENDER_TARGET) != 0;
	desc_out.bind_shader_resource = (d.BindFlags & D3D10_BIND_SHADER_RESOURCE) != 0;
	desc_out.cpu_read = (d.CPUAccessFlags & D3D10_CPU_ACCESS_READ) != 0;
	desc_out.cpu_write = (d.CPUAccessFlags & D3D10_CPU_ACCESS_WRITE) != 0;
	desc_out.gdi_compatible = (d.MiscFlags & D3D10_RESOURCE_MISC_GDI_COMPATIBLE) != 0;
	return true;
    }

    virtual HRESULT create_srv(IUnknown *texture,
			       RefPtr<IUnknown> &srv_out)
    {
	RefPtr<ID3D10Resource> resource;
	HRESULT hr = texture->QueryInterface(&resource);
	if (FAILED(hr)) {
	    return hr;
	}
	RefPtr<ID3D10ShaderResourceView> srv;
	hr = mD3D10Device->CreateShaderResourceView(resource, NULL, &srv);
	if (FAILED(hr)) {
	    return hr;
	}
	srv_out = srv;
	return S_OK;
    }

    virtual void copy_resource(IUnknown *dst, IUnknown *src)
    {
	RefPtr<ID3D10Resource> dstResource;
	RefPtr<ID3D10Resource> srcResource;
	if (FAILED(dst->QueryInterface(&dstResource)) ||
	    FAILED(src->QueryInterface(&srcResource))) {
	    return;
	}
	mD3D10Device->CopyResource(dstResource, srcResource);
    }

    virtual void copy_subresource_region(IUnknown *dst,
					 UINT dst_x, UINT dst_y,
					 IUnknown *src,
					 UINT left, UINT top,
					 UINT right, UINT bottom)
    {
	RefPtr<ID3D10Resource> dstResource;
	RefPtr<ID3D10Resource> srcResource;
	if (FAILED(dst->QueryInterface(&dstResource)) ||
	    FAILED(src->QueryInterface(&srcResource))) {
	    return;
	}
	D3D10_BOX box;
	box.front = 0;
	box.back = 1;
	box.left = left;
	box.top = top;
	box.right = right;
	box.bottom = bottom;
	mD3D10Device->CopySubresourceRegion(dstResource,
					    0,
					    dst_x,
					    dst_y,
					    0,
					    srcResource,
					    0,
					    &box);
    }

    virtual HRESULT map(IUnknown *texture, bool write_only,
			cairo_d3d_mapped_texture_t *mapped_out)
    {
	RefPtr<ID3D10Texture2D> tex;
	HRESULT hr = texture->QueryInterface(&tex);
	if (FAILED(hr)) {
	    return hr;
	}
	D3D10_MAPPED_TEXTURE2D data;
	hr = tex->Map(0,
		      write_only ? D3D10_MAP_WRITE : D3D10_MAP_READ_WRITE,
		      0,
		      &data);
	if (FAILED(hr)) {
	    return hr;
	}
	mapped_out->data = data.pData;
	mapped_out->row_pitch = data.RowPitch;
	return S_OK;
    }

    virtual void unmap(IUnknown *texture)
    {
	RefPtr<ID3D10Texture2D> tex;
	if (FAILED(texture->QueryInterface(&tex))) {
	    return;
	}
	tex->Unmap(0);
    }

    virtual IUnknown *text_texture() const
    {
	return static_cast<IUnknown*>(mTextTexture.get());
    }

    virtual IUnknown *text_texture_srv() const
    {
	return static_cast<IUnknown*>(mTextTextureView.get());
    }

    /**
     * Set a blending mode for an operator. Blend states are created lazily
     * and cached per operator.
     */
    cairo_int_status_t set_operator(cairo_operator_t op)
    {
	assert(op < MAX_OPERATORS);
	if (op >= MAX_OPERATORS) {
	    // Eep! Someone forgot to update MAX_OPERATORS probably.
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}

	if (mBlendStates[static_cast<size_t>(op)]) {
	    mD3D10Device->OMSetBlendState(mBlendStates[op], NULL, 0xffffffff);
	    return CAIRO_INT_STATUS_SUCCESS;
	}

	D3D10_BLEND_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.BlendEnable[0] = TRUE;
	desc.AlphaToCoverageEnable = FALSE;
	desc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;

	switch (op) {
	case CAIRO_OPERATOR_OVER:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_ONE;
	    break;
	case CAIRO_OPERATOR_ADD:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_ONE;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_ONE;
	    break;
	case CAIRO_OPERATOR_IN:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_ZERO;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_OUT:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_ZERO;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_INV_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_ATOP:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_DEST:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_ONE;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_ZERO;
	    break;
	case CAIRO_OPERATOR_DEST_OVER:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_ONE;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_INV_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_DEST_IN:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_SRC_ALPHA;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_ZERO;
	    break;
	case CAIRO_OPERATOR_DEST_OUT:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_ZERO;
	    break;
	case CAIRO_OPERATOR_DEST_ATOP:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_SRC_ALPHA;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_INV_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_XOR:
	    desc.BlendOp = desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	    desc.DestBlend = desc.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
	    desc.SrcBlend = desc.SrcBlendAlpha = D3D10_BLEND_INV_DEST_ALPHA;
	    break;
	default:
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	};
	mD3D10Device->CreateBlendState(&desc, &mBlendStates[op]);

	mD3D10Device->OMSetBlendState(mBlendStates[op], NULL, 0xffffffff);
	return CAIRO_INT_STATUS_SUCCESS;
    }

    void setup_for_blend()
    {
	mD3D10Device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	mD3D10Device->IASetInputLayout(mInputLayout);

	UINT stride = sizeof(Vertex);
	UINT offset = 0;
	ID3D10Buffer *buff = mQuadBuffer;
	mD3D10Device->IASetVertexBuffers(0, 1, &buff, &stride, &offset);

	mD3D10Device->RSSetState(mRasterizerState);
    }

    HRESULT ensure_rtv(IUnknown *texture, RefPtr<IUnknown> &rtv_cache)
    {
	if (rtv_cache) {
	    return S_OK;
	}
	RefPtr<ID3D10Resource> resource;
	HRESULT hr = texture->QueryInterface(&resource);
	if (FAILED(hr)) {
	    return hr;
	}
	RefPtr<ID3D10RenderTargetView> rtv;
	hr = mD3D10Device->CreateRenderTargetView(resource, NULL, &rtv);
	if (FAILED(hr)) {
	    return hr;
	}
	rtv_cache = rtv;
	return S_OK;
    }

    HRESULT ensure_srv(IUnknown *texture, RefPtr<IUnknown> &srv_cache)
    {
	if (srv_cache) {
	    return S_OK;
	}
	return create_srv(texture, srv_cache);
    }

    void set_viewport(UINT width, UINT height)
    {
	D3D10_VIEWPORT vp;
	vp.Height = height;
	vp.MinDepth = 0;
	vp.MaxDepth = 1.0;
	vp.TopLeftX = 0;
	vp.TopLeftY = 0;
	vp.Width = width;
	mD3D10Device->RSSetViewports(1, &vp);
    }

    virtual cairo_int_status_t
    draw_sample_texture(IUnknown *dst_texture,
			RefPtr<IUnknown> &dst_rtv_cache,
			IUnknown *src_texture,
			RefPtr<IUnknown> &src_srv_cache,
			UINT viewport_width, UINT viewport_height,
			const float quad_desc[4],
			const float tex_coords[4],
			cairo_operator_t op)
    {
	if (FAILED(ensure_rtv(dst_texture, dst_rtv_cache))) {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}
	if (FAILED(ensure_srv(src_texture, src_srv_cache))) {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}

	cairo_int_status_t status = set_operator(op);
	if (unlikely(status)) {
	    return status;
	}

	set_viewport(viewport_width, viewport_height);

	ID3D10Effect *effect = mSampleEffect;

	RefPtr<ID3D10RenderTargetView> rtView;
	dst_rtv_cache->QueryInterface(&rtView);
	RefPtr<ID3D10ShaderResourceView> srView;
	src_srv_cache->QueryInterface(&srView);

	ID3D10RenderTargetView *rtViewPtr = rtView;
	mD3D10Device->OMSetRenderTargets(1, &rtViewPtr, 0);
	ID3D10EffectVectorVariable *quadDesc = effect->GetVariableByName("QuadDesc")->AsVector();
	ID3D10EffectVectorVariable *texCoords = effect->GetVariableByName("TexCoords")->AsVector();

	quadDesc->SetFloatVector(const_cast<float*>(quad_desc));
	texCoords->SetFloatVector(const_cast<float*>(tex_coords));

	setup_for_blend();
	ID3D10EffectTechnique *technique = effect->GetTechniqueByName("SampleTexture");
	technique->GetPassByIndex(0)->Apply(0);

	ID3D10ShaderResourceView *srViewPtr = srView;
	mD3D10Device->PSSetShaderResources(0, 1, &srViewPtr);

	mD3D10Device->Draw(4, 0);

#ifdef DEBUG
	// Quiet down some info messages from D3D10 debug layer
	srViewPtr = NULL;
	mD3D10Device->PSSetShaderResources(0, 1, &srViewPtr);
	rtViewPtr = NULL;
	mD3D10Device->OMSetRenderTargets(1, &rtViewPtr, 0);
#endif
	return CAIRO_INT_STATUS_SUCCESS;
    }

    virtual cairo_int_status_t
    draw_blend_texture(IUnknown *dst_texture,
		       RefPtr<IUnknown> &dst_rtv_cache,
		       IUnknown *src_texture,
		       RefPtr<IUnknown> &src_srv_cache,
		       UINT viewport_width, UINT viewport_height,
		       const float quad_desc[4],
		       const float tex_coords[4],
		       int blend_mode)
    {
	HRESULT hr;

	if (FAILED(ensure_rtv(dst_texture, dst_rtv_cache))) {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}
	if (FAILED(ensure_srv(src_texture, src_srv_cache))) {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}

	RefPtr<ID3D10Texture2D> dstTexture;
	if (FAILED(dst_texture->QueryInterface(&dstTexture))) {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}

	D3D10_TEXTURE2D_DESC tDesc;
	dstTexture->GetDesc(&tDesc);

	// Create or recreate the destination copy texture if needed
	bool needRecreate = false;
	if (mDstCopyTexture) {
	    D3D10_TEXTURE2D_DESC existDesc;
	    mDstCopyTexture->GetDesc(&existDesc);
	    if (existDesc.Width != tDesc.Width || existDesc.Height != tDesc.Height || existDesc.Format != tDesc.Format) {
		needRecreate = true;
	    }
	} else {
	    needRecreate = true;
	}

	if (needRecreate) {
	    mDstCopySRV = NULL;
	    mDstCopyTexture = NULL;

	    CD3D10_TEXTURE2D_DESC dstCopyDesc(tDesc.Format, tDesc.Width, tDesc.Height);
	    dstCopyDesc.MipLevels = 1;
	    dstCopyDesc.Usage = D3D10_USAGE_DEFAULT;
	    dstCopyDesc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
	    hr = mD3D10Device->CreateTexture2D(&dstCopyDesc, NULL, &mDstCopyTexture);
	    if (FAILED(hr)) {
		return CAIRO_INT_STATUS_UNSUPPORTED;
	    }
	    hr = mD3D10Device->CreateShaderResourceView(mDstCopyTexture, NULL, &mDstCopySRV);
	    if (FAILED(hr)) {
		mDstCopyTexture = NULL;
		return CAIRO_INT_STATUS_UNSUPPORTED;
	    }
	}

	// Copy the current destination to the copy texture
	mD3D10Device->CopyResource(mDstCopyTexture, dstTexture);

	// Create the no-blend state if needed
	if (!mNoBlendState) {
	    D3D10_BLEND_DESC blendDesc;
	    memset(&blendDesc, 0, sizeof(blendDesc));
	    blendDesc.BlendEnable[0] = FALSE;
	    blendDesc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
	    mD3D10Device->CreateBlendState(&blendDesc, &mNoBlendState);
	}

	// Set no-blend state (blending is done entirely in the pixel shader)
	mD3D10Device->OMSetBlendState(mNoBlendState, NULL, 0xffffffff);

	set_viewport(viewport_width, viewport_height);

	ID3D10Effect *effect = mSampleEffect;

	RefPtr<ID3D10RenderTargetView> rtView;
	dst_rtv_cache->QueryInterface(&rtView);
	RefPtr<ID3D10ShaderResourceView> srView;
	src_srv_cache->QueryInterface(&srView);

	ID3D10RenderTargetView *rtViewPtr = rtView;
	mD3D10Device->OMSetRenderTargets(1, &rtViewPtr, 0);
	ID3D10EffectVectorVariable *quadDesc = effect->GetVariableByName("QuadDesc")->AsVector();
	ID3D10EffectVectorVariable *texCoords = effect->GetVariableByName("TexCoords")->AsVector();
	ID3D10EffectScalarVariable *blendModeVar = effect->GetVariableByName("BlendMode")->AsScalar();

	quadDesc->SetFloatVector(const_cast<float*>(quad_desc));
	texCoords->SetFloatVector(const_cast<float*>(tex_coords));
	blendModeVar->SetInt(blend_mode);

	setup_for_blend();
	ID3D10EffectTechnique *technique = effect->GetTechniqueByName("BlendTexture");
	technique->GetPassByIndex(0)->Apply(0);

	// Bind src (temp surface) to slot 0, dst copy to slot 1
	ID3D10ShaderResourceView *srViews[2];
	srViews[0] = srView;
	srViews[1] = mDstCopySRV;
	mD3D10Device->PSSetShaderResources(0, 2, srViews);

	mD3D10Device->Draw(4, 0);

#ifdef DEBUG
	srViews[0] = NULL;
	srViews[1] = NULL;
	mD3D10Device->PSSetShaderResources(0, 2, srViews);
	rtViewPtr = NULL;
	mD3D10Device->OMSetRenderTargets(1, &rtViewPtr, 0);
#endif
	return CAIRO_INT_STATUS_SUCCESS;
    }

    virtual cairo_int_status_t
    draw_text_texture(IUnknown *dst_texture,
		      RefPtr<IUnknown> &dst_rtv_cache,
		      IUnknown *glyph_srv,
		      UINT viewport_width, UINT viewport_height,
		      const float quad_desc[4],
		      const float tex_coords[4],
		      const float text_color[4])
    {
	if (FAILED(ensure_rtv(dst_texture, dst_rtv_cache))) {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}

	RefPtr<ID3D10RenderTargetView> rtView;
	dst_rtv_cache->QueryInterface(&rtView);
	RefPtr<ID3D10ShaderResourceView> srView;
	glyph_srv->QueryInterface(&srView);

	set_viewport(viewport_width, viewport_height);

	ID3D10Effect *effect = mSampleEffect;

	ID3D10RenderTargetView *rtViewPtr = rtView;
	mD3D10Device->OMSetRenderTargets(1, &rtViewPtr, 0);

	ID3D10EffectTechnique *technique = effect->GetTechniqueByName("SampleTextTexture");

	ID3D10EffectVectorVariable *quadDesc = effect->GetVariableByName("QuadDesc")->AsVector();
	ID3D10EffectVectorVariable *texCoords = effect->GetVariableByName("TexCoords")->AsVector();
	ID3D10EffectVectorVariable *textColor = effect->GetVariableByName("TextColor")->AsVector();

	textColor->SetFloatVector(const_cast<float*>(text_color));
	quadDesc->SetFloatVector(const_cast<float*>(quad_desc));
	texCoords->SetFloatVector(const_cast<float*>(tex_coords));

	setup_for_blend();
	technique->GetPassByIndex(0)->Apply(0);

	ID3D10ShaderResourceView *srViewPtr = srView;
	mD3D10Device->PSSetShaderResources(0, 1, &srViewPtr);

	mD3D10Device->Draw(4, 0);

	return CAIRO_INT_STATUS_SUCCESS;
    }
};

/**
 * Initialize the backend around an existing device: load the effects
 * runtime, create the effect, input layout, quad vertex buffer, rasterizer
 * state and the glyph text texture. Extracted unchanged from
 * cairo_d2d_create_device_from_d3d10device().
 */
static cairo_d3d_backend *
_cairo_d3d10_backend_init(ID3D10Device1 *d3d10device)
{
    HRESULT hr;
    D3D10_RASTERIZER_DESC rastDesc;
    D3D10_INPUT_ELEMENT_DESC layout[] =
    {
    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D10_PASS_DESC passDesc;
    ID3D10EffectTechnique *technique;
    Vertex vertices[] = { {{0.0, 0.0}}, {{1.0, 0.0}}, {{0.0, 1.0}}, {{1.0, 1.0}} };
    CD3D10_BUFFER_DESC bufferDesc(sizeof(vertices), D3D10_BIND_VERTEX_BUFFER);
    D3D10_SUBRESOURCE_DATA data;
    CD3D10_TEXTURE2D_DESC textDesc(DXGI_FORMAT_B8G8R8A8_UNORM,
	TEXT_TEXTURE_WIDTH,
	TEXT_TEXTURE_HEIGHT,
	1, 1);

    cairo_d3d10_backend *backend = new cairo_d3d10_backend;

    backend->mD3D10Device = d3d10device;

    backend->mD3D10_1 = LoadLibraryA("d3d10_1.dll");
    D3D10CreateEffectFromMemoryFunc createEffect = (D3D10CreateEffectFromMemoryFunc)
	GetProcAddress(backend->mD3D10_1, "D3D10CreateEffectFromMemory");

    if (!createEffect) {
	goto FAILED;
    }

    backend->mD3D10Device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_LINESTRIP);
    createEffect((void*)g_main, sizeof(g_main), 0, backend->mD3D10Device, NULL, &backend->mSampleEffect);

    if (!backend->mSampleEffect) {
	goto FAILED;
    }

    technique = backend->mSampleEffect->GetTechniqueByName("SampleTexture");
    technique->GetPassByIndex(0)->GetDesc(&passDesc);

    hr = backend->mD3D10Device->CreateInputLayout(layout,
	sizeof(layout) / sizeof(D3D10_INPUT_ELEMENT_DESC),
	passDesc.pIAInputSignature,
	passDesc.IAInputSignatureSize,
	&backend->mInputLayout);
    if (FAILED(hr)) {
	goto FAILED;
    }

    data.pSysMem = (void*)vertices;
    hr = backend->mD3D10Device->CreateBuffer(&bufferDesc, &data, &backend->mQuadBuffer);
    if (FAILED(hr)) {
	goto FAILED;
    }

    memset(&rastDesc, 0, sizeof(rastDesc));
    rastDesc.CullMode = D3D10_CULL_NONE;
    rastDesc.FillMode = D3D10_FILL_SOLID;
    rastDesc.DepthClipEnable = TRUE;
    hr = backend->mD3D10Device->CreateRasterizerState(&rastDesc, &backend->mRasterizerState);
    if (FAILED(hr)) {
	goto FAILED;
    }

    // We create this with USAGE_DEFAULT, our intention is to have VRAM
    // reserved for text usage. We actually store glyph data in STAGING
    // textures for the rendering pipeline to read and copy it to this VRAM
    // texture.
    textDesc.Usage = D3D10_USAGE_DEFAULT;
    hr = backend->mD3D10Device->CreateTexture2D(&textDesc, NULL, &backend->mTextTexture);
    if (FAILED(hr)) {
	goto FAILED;
    }

    hr = backend->mD3D10Device->CreateShaderResourceView(backend->mTextTexture,
	NULL,
	&backend->mTextTextureView);
    if (FAILED(hr)) {
	goto FAILED;
    }

    return backend;
FAILED:
    delete backend;
    return NULL;
}

cairo_d3d_backend *
cairo_d3d10_backend_create(ID3D10Device1 *d3d10device)
{
    if (d3d10device) {
	return _cairo_d3d10_backend_init(d3d10device);
    }

    HMODULE d3d10module = LoadLibraryA("d3d10_1.dll");
    D3D10CreateDevice1Func createD3DDevice = (D3D10CreateDevice1Func)
	GetProcAddress(d3d10module, "D3D10CreateDevice1");

    if (!createD3DDevice) {
	return NULL;
    }

    RefPtr<ID3D10Device1> d3ddevice;

    /**
     * On usage of D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS:
     * documentation on D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS
     * can be misleading. In fact, that flag gives no such indication. I pointed this
     * out to Bas in my email. However, Microsoft is in fact using this flag to
     * indicate "light weight" DX applications. By light weight they are essentially
     * referring to applications that are not games. The idea is that when you create
     * a DX game, the driver assumes that you will pretty much have a single instance
     * and therefore it doesn't try to hold back when it comes to GPU resource
     * allocation as long as it can crank out performance. In other words, the
     * priority in regular DX applications is to make that one application run as fast
     * as you can. For "light weight" applications, including D2D applications, the
     * priorities are a bit different. Now you are no longer going to have a single
     * (or very few) instances. You can have a lot of them (say, for example, a
     * separate DX context/device per browser tab). In such cases, the GPU resource
     * allocation scheme changes.
     */
    HRESULT hr = createD3DDevice(
	NULL,
	D3D10_DRIVER_TYPE_HARDWARE,
	NULL,
	D3D10_CREATE_DEVICE_DEBUG | D3D10_CREATE_DEVICE_BGRA_SUPPORT |
	D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
	D3D10_FEATURE_LEVEL_10_1,
	D3D10_1_SDK_VERSION,
	&d3ddevice);
    if (FAILED(hr)) {
	hr = createD3DDevice(
	    NULL,
	    D3D10_DRIVER_TYPE_HARDWARE,
	    NULL,
	    D3D10_CREATE_DEVICE_DEBUG | D3D10_CREATE_DEVICE_BGRA_SUPPORT |
	    D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
	    D3D10_FEATURE_LEVEL_10_0,
	    D3D10_1_SDK_VERSION,
	    &d3ddevice);
	if (FAILED(hr)) {
	    /* This is not guaranteed to be too fast! */
	    hr = createD3DDevice(
		NULL,
		D3D10_DRIVER_TYPE_HARDWARE,
		NULL,
		D3D10_CREATE_DEVICE_DEBUG | D3D10_CREATE_DEVICE_BGRA_SUPPORT |
		D3D10_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
		D3D10_FEATURE_LEVEL_9_3,
		D3D10_1_SDK_VERSION,
		&d3ddevice);

	}
    }
    if (FAILED(hr)) {
	FreeLibrary(d3d10module);
	return NULL;
    }

    cairo_d3d_backend *backend = _cairo_d3d10_backend_init(d3ddevice);

    // Free our reference to the module. The created backend has its own.
    FreeLibrary(d3d10module);
    return backend;
}
