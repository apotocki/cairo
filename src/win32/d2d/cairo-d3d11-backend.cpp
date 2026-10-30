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
 */

/*
 * D3D11 implementation of cairo_d3d_backend. D3D11 has no in-box Effects
 * runtime, so unlike the D3D10.1 backend, the shaders here are plain
 * HLSL (cairo-d3d11-shaders.hlsl / cairo-d3d11-shaders.h) and the render
 * states the fx techniques used to set (blend/sampler/rasterizer) are
 * explicit D3D11 state objects created once at device-creation time.
 */

#define CAIRO_HAS_D2D_SURFACE 1

#include "cairoint.h"

#include <cfloat>
#include <d3d11.h>
#include <dxgi.h>

#include "cairo-d3d-backend-private.h"
#include "cairo-d3d11-shaders.h"

#define CAIRO_INT_STATUS_SUCCESS (cairo_int_status_t)CAIRO_STATUS_SUCCESS

#define MAX_OPERATORS (CAIRO_OPERATOR_HSL_LUMINOSITY + 1)

typedef HRESULT (WINAPI*D3D11CreateDeviceFunc)(
    IDXGIAdapter *pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    CONST D3D_FEATURE_LEVEL *pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext
);

namespace {
struct Vertex
{
    float position[2];
};

/**
 * Layout matching cbuffer cb0 in cairo-d3d11-shaders.hlsl: three float4
 * registers followed by a fourth register holding BlendMode in its first
 * component (the rest is unused padding to keep the struct a whole
 * number of 16-byte constant registers).
 */
struct ConstantBufferData
{
    float QuadDesc[4];
    float TexCoords[4];
    float TextColor[4];
    int BlendMode;
    int pad[3];
};
}

struct cairo_d3d11_backend : public cairo_d3d_backend
{
    RefPtr<ID3D11Device> mDevice;
    RefPtr<ID3D11DeviceContext> mContext;

    RefPtr<ID3D11VertexShader> mVertexShader;
    RefPtr<ID3D11PixelShader> mSamplePS;
    RefPtr<ID3D11PixelShader> mSampleTextPS;
    RefPtr<ID3D11PixelShader> mBlendPS;
    RefPtr<ID3D11InputLayout> mInputLayout;
    RefPtr<ID3D11Buffer> mQuadBuffer;
    RefPtr<ID3D11Buffer> mConstantBuffer;
    RefPtr<ID3D11RasterizerState> mRasterizerState;
    RefPtr<ID3D11SamplerState> mSamplerState;
    RefPtr<ID3D11BlendState> mBlendStates[MAX_OPERATORS];
    /** Dual-source blend state used by the text (ClearType) shader */
    RefPtr<ID3D11BlendState> mTextBlendState;
    /** Blend state with blending disabled (blend mode compositing in PS) */
    RefPtr<ID3D11BlendState> mNoBlendState;

    /** Texture used for manual glyph rendering */
    RefPtr<ID3D11Texture2D> mTextTexture;
    RefPtr<ID3D11ShaderResourceView> mTextTextureView;
    /** Texture used for reading destination in blend mode pixel shader */
    RefPtr<ID3D11Texture2D> mDstCopyTexture;
    RefPtr<ID3D11ShaderResourceView> mDstCopySRV;

    cairo_d3d11_backend()
    {
    }

    virtual ~cairo_d3d11_backend()
    {
	/* Release all COM references. Note we deliberately never FreeLibrary
	 * the d3d11.dll module that cairo_d3d11_backend_create() may have
	 * LoadLibrary'd (see there): this device can end up shared with
	 * externally-created objects this backend has no visibility into
	 * (e.g. a DXGI swap chain / DirectComposition tree created directly
	 * on cairo_d2d_device_get_device11()'s device by the embedding
	 * application), whose lifetime can outlive this backend object. There
	 * is no point at which unloading the module is provably safe, so --
	 * like other long-lived driver DLLs -- it is intentionally leaked for
	 * the life of the process rather than risk unloading it out from
	 * under a still-live reference. */
	mVertexShader = NULL;
	mSamplePS = NULL;
	mSampleTextPS = NULL;
	mBlendPS = NULL;
	mInputLayout = NULL;
	mQuadBuffer = NULL;
	mConstantBuffer = NULL;
	mRasterizerState = NULL;
	mSamplerState = NULL;
	for (int i = 0; i < MAX_OPERATORS; i++) {
	    mBlendStates[i] = NULL;
	}
	mTextBlendState = NULL;
	mNoBlendState = NULL;
	mTextTexture = NULL;
	mTextTextureView = NULL;
	mDstCopyTexture = NULL;
	mDstCopySRV = NULL;
	mContext = NULL;
	mDevice = NULL;
    }

    virtual cairo_d3d_backend_kind_t kind() const
    {
	return CAIRO_D3D_BACKEND_D3D11;
    }

    virtual IUnknown *device_unknown() const
    {
	return static_cast<IUnknown*>(mDevice.get());
    }

    virtual void *native_device() const
    {
	return mDevice.get();
    }

    virtual void *native_texture(IUnknown *resource) const
    {
	if (!resource) {
	    return NULL;
	}
	RefPtr<ID3D11Texture2D> texture;
	if (FAILED(resource->QueryInterface(&texture))) {
	    return NULL;
	}
	return texture.get();
    }

    virtual void flush()
    {
	mContext->Flush();
    }

    virtual void finish()
    {
	D3D11_QUERY_DESC queryDesc;
	queryDesc.Query = D3D11_QUERY_EVENT;
	queryDesc.MiscFlags = 0;
	RefPtr<ID3D11Query> query;

	mDevice->CreateQuery(&queryDesc, &query);

	// QUERY_EVENT does not use Begin(). It's disabled.
	mContext->End(query);

	BOOL done = FALSE;
	while (!done) {
	    if (FAILED(mContext->GetData(query, &done, sizeof(BOOL), 0))) {
		break;
	    }
	    if (FAILED(mDevice->GetDeviceRemovedReason())) {
		break;
	    }
	}
    }

    virtual HRESULT create_texture(const cairo_d3d_texture_desc_t &desc,
				   const void *init_data,
				   UINT init_pitch,
				   RefPtr<IUnknown> &texture_out)
    {
	D3D11_TEXTURE2D_DESC d;
	d.Width = desc.width;
	d.Height = desc.height;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = desc.format;
	d.SampleDesc.Count = 1;
	d.SampleDesc.Quality = 0;
	switch (desc.usage) {
	case CAIRO_D3D_TEXTURE_USAGE_IMMUTABLE:
	    d.Usage = D3D11_USAGE_IMMUTABLE;
	    break;
	case CAIRO_D3D_TEXTURE_USAGE_STAGING:
	    d.Usage = D3D11_USAGE_STAGING;
	    break;
	default:
	    d.Usage = D3D11_USAGE_DEFAULT;
	    break;
	}
	d.BindFlags = 0;
	if (desc.bind_render_target) {
	    d.BindFlags |= D3D11_BIND_RENDER_TARGET;
	}
	if (desc.bind_shader_resource) {
	    d.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
	}
	d.CPUAccessFlags = 0;
	if (desc.cpu_read) {
	    d.CPUAccessFlags |= D3D11_CPU_ACCESS_READ;
	}
	if (desc.cpu_write) {
	    d.CPUAccessFlags |= D3D11_CPU_ACCESS_WRITE;
	}
	d.MiscFlags = desc.gdi_compatible ? D3D11_RESOURCE_MISC_GDI_COMPATIBLE : 0;

	D3D11_SUBRESOURCE_DATA data;
	data.pSysMem = init_data;
	data.SysMemPitch = init_pitch;
	data.SysMemSlicePitch = 0;

	RefPtr<ID3D11Texture2D> texture;
	HRESULT hr = mDevice->CreateTexture2D(&d,
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
	RefPtr<ID3D11Resource> resource;
	HRESULT hr = mDevice->OpenSharedResource(handle,
						 __uuidof(ID3D11Resource),
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
	RefPtr<ID3D11Texture2D> tex;
	if (!texture ||
	    FAILED(texture->QueryInterface(&tex))) {
	    return false;
	}
	D3D11_TEXTURE2D_DESC d;
	tex->GetDesc(&d);

	desc_out.format = d.Format;
	desc_out.width = d.Width;
	desc_out.height = d.Height;
	switch (d.Usage) {
	case D3D11_USAGE_IMMUTABLE:
	    desc_out.usage = CAIRO_D3D_TEXTURE_USAGE_IMMUTABLE;
	    break;
	case D3D11_USAGE_STAGING:
	    desc_out.usage = CAIRO_D3D_TEXTURE_USAGE_STAGING;
	    break;
	default:
	    desc_out.usage = CAIRO_D3D_TEXTURE_USAGE_DEFAULT;
	    break;
	}
	desc_out.bind_render_target = (d.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
	desc_out.bind_shader_resource = (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
	desc_out.cpu_read = (d.CPUAccessFlags & D3D11_CPU_ACCESS_READ) != 0;
	desc_out.cpu_write = (d.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) != 0;
	desc_out.gdi_compatible = (d.MiscFlags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE) != 0;
	return true;
    }

    virtual HRESULT create_srv(IUnknown *texture,
			       RefPtr<IUnknown> &srv_out)
    {
	RefPtr<ID3D11Resource> resource;
	HRESULT hr = texture->QueryInterface(&resource);
	if (FAILED(hr)) {
	    return hr;
	}
	RefPtr<ID3D11ShaderResourceView> srv;
	hr = mDevice->CreateShaderResourceView(resource, NULL, &srv);
	if (FAILED(hr)) {
	    return hr;
	}
	srv_out = srv;
	return S_OK;
    }

    virtual void copy_resource(IUnknown *dst, IUnknown *src)
    {
	RefPtr<ID3D11Resource> dstResource;
	RefPtr<ID3D11Resource> srcResource;
	if (FAILED(dst->QueryInterface(&dstResource)) ||
	    FAILED(src->QueryInterface(&srcResource))) {
	    return;
	}
	mContext->CopyResource(dstResource, srcResource);
    }

    virtual void copy_subresource_region(IUnknown *dst,
					 UINT dst_x, UINT dst_y,
					 IUnknown *src,
					 UINT left, UINT top,
					 UINT right, UINT bottom)
    {
	RefPtr<ID3D11Resource> dstResource;
	RefPtr<ID3D11Resource> srcResource;
	if (FAILED(dst->QueryInterface(&dstResource)) ||
	    FAILED(src->QueryInterface(&srcResource))) {
	    return;
	}
	D3D11_BOX box;
	box.front = 0;
	box.back = 1;
	box.left = left;
	box.top = top;
	box.right = right;
	box.bottom = bottom;
	mContext->CopySubresourceRegion(dstResource,
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
	RefPtr<ID3D11Resource> resource;
	HRESULT hr = texture->QueryInterface(&resource);
	if (FAILED(hr)) {
	    return hr;
	}
	D3D11_MAPPED_SUBRESOURCE data;
	hr = mContext->Map(resource, 0,
			   write_only ? D3D11_MAP_WRITE : D3D11_MAP_READ_WRITE,
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
	RefPtr<ID3D11Resource> resource;
	if (FAILED(texture->QueryInterface(&resource))) {
	    return;
	}
	mContext->Unmap(resource, 0);
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
     * Create (and cache) the blend state for a cairo operator. Mirrors
     * the operator -> D3D10_BLEND_DESC mapping in the D3D10 backend; the
     * D3D11_BLEND enum values are numerically compatible so the mapping
     * is unchanged.
     */
    HRESULT ensure_operator_blend_state(cairo_operator_t op, ID3D11BlendState **out)
    {
	assert(op < MAX_OPERATORS);
	if (op >= MAX_OPERATORS) {
	    return E_INVALIDARG;
	}

	if (mBlendStates[static_cast<size_t>(op)]) {
	    *out = mBlendStates[op];
	    return S_OK;
	}

	D3D11_BLEND_DESC desc;
	memset(&desc, 0, sizeof(desc));
	desc.AlphaToCoverageEnable = FALSE;
	desc.IndependentBlendEnable = FALSE;
	desc.RenderTarget[0].BlendEnable = TRUE;
	desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	switch (op) {
	case CAIRO_OPERATOR_OVER:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	    break;
	case CAIRO_OPERATOR_ADD:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	    break;
	case CAIRO_OPERATOR_IN:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_OUT:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_ATOP:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_DEST:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
	    break;
	case CAIRO_OPERATOR_DEST_OVER:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_DEST_IN:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
	    break;
	case CAIRO_OPERATOR_DEST_OUT:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
	    break;
	case CAIRO_OPERATOR_DEST_ATOP:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
	    break;
	case CAIRO_OPERATOR_XOR:
	    desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	    desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	    desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
	    break;
	default:
	    return E_INVALIDARG;
	};

	HRESULT hr = mDevice->CreateBlendState(&desc, &mBlendStates[op]);
	if (FAILED(hr)) {
	    return hr;
	}
	*out = mBlendStates[op];
	return S_OK;
    }

    void setup_for_blend()
    {
	mContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	mContext->IASetInputLayout(mInputLayout);

	UINT stride = sizeof(Vertex);
	UINT offset = 0;
	ID3D11Buffer *buff = mQuadBuffer;
	mContext->IASetVertexBuffers(0, 1, &buff, &stride, &offset);

	mContext->RSSetState(mRasterizerState);

	ID3D11SamplerState *sampler = mSamplerState;
	mContext->PSSetSamplers(0, 1, &sampler);

	mContext->VSSetShader(mVertexShader, NULL, 0);
    }

    void update_constants(const float quad_desc[4], const float tex_coords[4],
			  const float text_color[4], int blend_mode)
    {
	ConstantBufferData data;
	memcpy(data.QuadDesc, quad_desc, sizeof(data.QuadDesc));
	memcpy(data.TexCoords, tex_coords, sizeof(data.TexCoords));
	if (text_color) {
	    memcpy(data.TextColor, text_color, sizeof(data.TextColor));
	} else {
	    memset(data.TextColor, 0, sizeof(data.TextColor));
	}
	data.BlendMode = blend_mode;
	data.pad[0] = data.pad[1] = data.pad[2] = 0;

	mContext->UpdateSubresource(mConstantBuffer, 0, NULL, &data, 0, 0);

	ID3D11Buffer *cb = mConstantBuffer;
	mContext->VSSetConstantBuffers(0, 1, &cb);
	mContext->PSSetConstantBuffers(0, 1, &cb);
    }

    HRESULT ensure_rtv(IUnknown *texture, RefPtr<IUnknown> &rtv_cache)
    {
	if (rtv_cache) {
	    return S_OK;
	}
	RefPtr<ID3D11Resource> resource;
	HRESULT hr = texture->QueryInterface(&resource);
	if (FAILED(hr)) {
	    return hr;
	}
	RefPtr<ID3D11RenderTargetView> rtv;
	hr = mDevice->CreateRenderTargetView(resource, NULL, &rtv);
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
	D3D11_VIEWPORT vp;
	vp.Height = (FLOAT)height;
	vp.MinDepth = 0;
	vp.MaxDepth = 1.0;
	vp.TopLeftX = 0;
	vp.TopLeftY = 0;
	vp.Width = (FLOAT)width;
	mContext->RSSetViewports(1, &vp);
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

	ID3D11BlendState *blendState;
	if (FAILED(ensure_operator_blend_state(op, &blendState))) {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}

	set_viewport(viewport_width, viewport_height);
	update_constants(quad_desc, tex_coords, NULL, 0);

	RefPtr<ID3D11RenderTargetView> rtView;
	dst_rtv_cache->QueryInterface(&rtView);
	RefPtr<ID3D11ShaderResourceView> srView;
	src_srv_cache->QueryInterface(&srView);

	ID3D11RenderTargetView *rtViewPtr = rtView;
	mContext->OMSetRenderTargets(1, &rtViewPtr, NULL);
	mContext->OMSetBlendState(blendState, NULL, 0xffffffff);

	setup_for_blend();
	mContext->PSSetShader(mSamplePS, NULL, 0);

	ID3D11ShaderResourceView *srViewPtr = srView;
	mContext->PSSetShaderResources(0, 1, &srViewPtr);

	mContext->Draw(4, 0);

#ifdef DEBUG
	srViewPtr = NULL;
	mContext->PSSetShaderResources(0, 1, &srViewPtr);
	rtViewPtr = NULL;
	mContext->OMSetRenderTargets(1, &rtViewPtr, NULL);
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

	RefPtr<ID3D11Texture2D> dstTexture;
	if (FAILED(dst_texture->QueryInterface(&dstTexture))) {
	    return CAIRO_INT_STATUS_UNSUPPORTED;
	}

	D3D11_TEXTURE2D_DESC tDesc;
	dstTexture->GetDesc(&tDesc);

	bool needRecreate = false;
	if (mDstCopyTexture) {
	    D3D11_TEXTURE2D_DESC existDesc;
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

	    D3D11_TEXTURE2D_DESC dstCopyDesc;
	    memset(&dstCopyDesc, 0, sizeof(dstCopyDesc));
	    dstCopyDesc.Width = tDesc.Width;
	    dstCopyDesc.Height = tDesc.Height;
	    dstCopyDesc.MipLevels = 1;
	    dstCopyDesc.ArraySize = 1;
	    dstCopyDesc.Format = tDesc.Format;
	    dstCopyDesc.SampleDesc.Count = 1;
	    dstCopyDesc.Usage = D3D11_USAGE_DEFAULT;
	    dstCopyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	    hr = mDevice->CreateTexture2D(&dstCopyDesc, NULL, &mDstCopyTexture);
	    if (FAILED(hr)) {
		return CAIRO_INT_STATUS_UNSUPPORTED;
	    }
	    hr = mDevice->CreateShaderResourceView(mDstCopyTexture, NULL, &mDstCopySRV);
	    if (FAILED(hr)) {
		mDstCopyTexture = NULL;
		return CAIRO_INT_STATUS_UNSUPPORTED;
	    }
	}

	// Copy the current destination to the copy texture
	mContext->CopyResource(mDstCopyTexture, dstTexture);

	// Create the no-blend state if needed
	if (!mNoBlendState) {
	    D3D11_BLEND_DESC blendDesc;
	    memset(&blendDesc, 0, sizeof(blendDesc));
	    blendDesc.RenderTarget[0].BlendEnable = FALSE;
	    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	    hr = mDevice->CreateBlendState(&blendDesc, &mNoBlendState);
	    if (FAILED(hr)) {
		return CAIRO_INT_STATUS_UNSUPPORTED;
	    }
	}

	set_viewport(viewport_width, viewport_height);
	update_constants(quad_desc, tex_coords, NULL, blend_mode);

	RefPtr<ID3D11RenderTargetView> rtView;
	dst_rtv_cache->QueryInterface(&rtView);
	RefPtr<ID3D11ShaderResourceView> srView;
	src_srv_cache->QueryInterface(&srView);

	ID3D11RenderTargetView *rtViewPtr = rtView;
	mContext->OMSetRenderTargets(1, &rtViewPtr, NULL);
	// Blending is done entirely in the pixel shader.
	mContext->OMSetBlendState(mNoBlendState, NULL, 0xffffffff);

	setup_for_blend();
	mContext->PSSetShader(mBlendPS, NULL, 0);

	// Bind src (temp surface) to slot 0, dst copy to slot 1
	ID3D11ShaderResourceView *srViews[2];
	srViews[0] = srView;
	srViews[1] = mDstCopySRV;
	mContext->PSSetShaderResources(0, 2, srViews);

	mContext->Draw(4, 0);

#ifdef DEBUG
	srViews[0] = NULL;
	srViews[1] = NULL;
	mContext->PSSetShaderResources(0, 2, srViews);
	rtViewPtr = NULL;
	mContext->OMSetRenderTargets(1, &rtViewPtr, NULL);
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

	RefPtr<ID3D11RenderTargetView> rtView;
	dst_rtv_cache->QueryInterface(&rtView);
	RefPtr<ID3D11ShaderResourceView> srView;
	glyph_srv->QueryInterface(&srView);

	set_viewport(viewport_width, viewport_height);
	update_constants(quad_desc, tex_coords, text_color, 0);

	ID3D11RenderTargetView *rtViewPtr = rtView;
	mContext->OMSetRenderTargets(1, &rtViewPtr, NULL);
	mContext->OMSetBlendState(mTextBlendState, NULL, 0xffffffff);

	setup_for_blend();
	mContext->PSSetShader(mSampleTextPS, NULL, 0);

	ID3D11ShaderResourceView *srViewPtr = srView;
	mContext->PSSetShaderResources(0, 1, &srViewPtr);

	mContext->Draw(4, 0);

	return CAIRO_INT_STATUS_SUCCESS;
    }
};

static cairo_d3d_backend *
_cairo_d3d11_backend_init(ID3D11Device *d3d11device)
{
    HRESULT hr;

    cairo_d3d11_backend *backend = new cairo_d3d11_backend;
    backend->mDevice = d3d11device;
    backend->mDevice->GetImmediateContext(&backend->mContext);

    hr = backend->mDevice->CreateVertexShader(g_d3d11_VS_Quad, sizeof(g_d3d11_VS_Quad), NULL, &backend->mVertexShader);
    if (FAILED(hr)) {
	goto FAILED;
    }

    hr = backend->mDevice->CreatePixelShader(g_d3d11_PS_Sample, sizeof(g_d3d11_PS_Sample), NULL, &backend->mSamplePS);
    if (FAILED(hr)) {
	goto FAILED;
    }

    hr = backend->mDevice->CreatePixelShader(g_d3d11_PS_SampleText, sizeof(g_d3d11_PS_SampleText), NULL, &backend->mSampleTextPS);
    if (FAILED(hr)) {
	goto FAILED;
    }

    hr = backend->mDevice->CreatePixelShader(g_d3d11_PS_Blend, sizeof(g_d3d11_PS_Blend), NULL, &backend->mBlendPS);
    if (FAILED(hr)) {
	goto FAILED;
    }

    {
	D3D11_INPUT_ELEMENT_DESC layout[] =
	{
	    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	hr = backend->mDevice->CreateInputLayout(layout,
	    sizeof(layout) / sizeof(D3D11_INPUT_ELEMENT_DESC),
	    g_d3d11_VS_Quad,
	    sizeof(g_d3d11_VS_Quad),
	    &backend->mInputLayout);
	if (FAILED(hr)) {
	    goto FAILED;
	}
    }

    {
	Vertex vertices[] = { {{0.0, 0.0}}, {{1.0, 0.0}}, {{0.0, 1.0}}, {{1.0, 1.0}} };
	D3D11_BUFFER_DESC bufferDesc;
	memset(&bufferDesc, 0, sizeof(bufferDesc));
	bufferDesc.ByteWidth = sizeof(vertices);
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	D3D11_SUBRESOURCE_DATA data;
	data.pSysMem = vertices;
	data.SysMemPitch = 0;
	data.SysMemSlicePitch = 0;
	hr = backend->mDevice->CreateBuffer(&bufferDesc, &data, &backend->mQuadBuffer);
	if (FAILED(hr)) {
	    goto FAILED;
	}
    }

    {
	D3D11_BUFFER_DESC cbDesc;
	memset(&cbDesc, 0, sizeof(cbDesc));
	cbDesc.ByteWidth = sizeof(ConstantBufferData);
	cbDesc.Usage = D3D11_USAGE_DEFAULT;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = backend->mDevice->CreateBuffer(&cbDesc, NULL, &backend->mConstantBuffer);
	if (FAILED(hr)) {
	    goto FAILED;
	}
    }

    {
	D3D11_RASTERIZER_DESC rastDesc;
	memset(&rastDesc, 0, sizeof(rastDesc));
	rastDesc.CullMode = D3D11_CULL_NONE;
	rastDesc.FillMode = D3D11_FILL_SOLID;
	rastDesc.DepthClipEnable = TRUE;
	hr = backend->mDevice->CreateRasterizerState(&rastDesc, &backend->mRasterizerState);
	if (FAILED(hr)) {
	    goto FAILED;
	}
    }

    {
	// Matches the fx sSampler block (AddressU/V = CLAMP, all other
	// SamplerState fields left at their fx defaults: bilinear filter,
	// no comparison, full LOD range).
	D3D11_SAMPLER_DESC sampDesc;
	memset(&sampDesc, 0, sizeof(sampDesc));
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampDesc.MaxAnisotropy = 1;
	sampDesc.MinLOD = -FLT_MAX;
	sampDesc.MaxLOD = FLT_MAX;
	hr = backend->mDevice->CreateSamplerState(&sampDesc, &backend->mSamplerState);
	if (FAILED(hr)) {
	    goto FAILED;
	}
    }

    {
	// Matches the fx bTextBlend block (dual-source blending).
	D3D11_BLEND_DESC blendDesc;
	memset(&blendDesc, 0, sizeof(blendDesc));
	blendDesc.AlphaToCoverageEnable = FALSE;
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC1_COLOR;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC1_COLOR;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC1_ALPHA;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC1_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = backend->mDevice->CreateBlendState(&blendDesc, &backend->mTextBlendState);
	if (FAILED(hr)) {
	    goto FAILED;
	}
    }

    {
	D3D11_TEXTURE2D_DESC textDesc;
	memset(&textDesc, 0, sizeof(textDesc));
	textDesc.Width = TEXT_TEXTURE_WIDTH;
	textDesc.Height = TEXT_TEXTURE_HEIGHT;
	textDesc.MipLevels = 1;
	textDesc.ArraySize = 1;
	textDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	textDesc.SampleDesc.Count = 1;
	// We create this with USAGE_DEFAULT, our intention is to have VRAM
	// reserved for text usage. We actually store glyph data in STAGING
	// textures for the rendering pipeline to read and copy it to this
	// VRAM texture.
	textDesc.Usage = D3D11_USAGE_DEFAULT;
	textDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	hr = backend->mDevice->CreateTexture2D(&textDesc, NULL, &backend->mTextTexture);
	if (FAILED(hr)) {
	    goto FAILED;
	}

	hr = backend->mDevice->CreateShaderResourceView(backend->mTextTexture, NULL, &backend->mTextTextureView);
	if (FAILED(hr)) {
	    goto FAILED;
	}
    }

    return backend;
FAILED:
    delete backend;
    return NULL;
}

cairo_d3d_backend *
cairo_d3d11_backend_create(ID3D11Device *d3d11device)
{
    if (d3d11device) {
	return _cairo_d3d11_backend_init(d3d11device);
    }

    HMODULE d3d11module = LoadLibraryA("d3d11.dll");
    if (!d3d11module) {
	return NULL;
    }
    D3D11CreateDeviceFunc createD3DDevice = (D3D11CreateDeviceFunc)
	GetProcAddress(d3d11module, "D3D11CreateDevice");

    if (!createD3DDevice) {
	FreeLibrary(d3d11module);
	return NULL;
    }

    static const D3D_FEATURE_LEVEL featureLevels[] = {
	D3D_FEATURE_LEVEL_11_1,
	D3D_FEATURE_LEVEL_11_0,
	D3D_FEATURE_LEVEL_10_1,
	D3D_FEATURE_LEVEL_10_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
#ifdef DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    RefPtr<ID3D11Device> d3ddevice;
    D3D_FEATURE_LEVEL obtainedLevel;

    // ppImmediateContext is intentionally NULL: _cairo_d3d11_backend_init
    // fetches its own context reference via GetImmediateContext right after
    // (see above). Requesting one here too would mean releasing a second,
    // separately-refcounted RefPtr<ID3D11DeviceContext> at this function's
    // scope exit -- which happens after FreeLibrary(d3d11module) below, i.e.
    // Release() would run against a device/context whose vtable code may
    // already have been unloaded from the process. Not asking for it removes
    // the dangling-reference-after-FreeLibrary hazard entirely.
    HRESULT hr = createD3DDevice(
	NULL,
	D3D_DRIVER_TYPE_HARDWARE,
	NULL,
	flags,
	featureLevels,
	sizeof(featureLevels) / sizeof(featureLevels[0]),
	D3D11_SDK_VERSION,
	&d3ddevice,
	&obtainedLevel,
	NULL);
    if (FAILED(hr)) {
	FreeLibrary(d3d11module);
	return NULL;
    }

    cairo_d3d_backend *backend = _cairo_d3d11_backend_init(d3ddevice);

    // Deliberately NOT calling FreeLibrary(d3d11module) on success: see the
    // note in cairo_d3d11_backend's destructor. d3ddevice (released when this
    // function returns) and backend->mDevice both still reference the module,
    // and -- unlike a plain COM refcount -- there is no reliable point at
    // which every external consumer built on top of this device (e.g. a
    // DXGI/DirectComposition swap chain the embedding app created directly on
    // cairo_d2d_device_get_device11()) is guaranteed to be done with it. On
    // failure nothing was created from the module, so it's safe to free here.
    if (!backend) {
	FreeLibrary(d3d11module);
    }
    return backend;
}
