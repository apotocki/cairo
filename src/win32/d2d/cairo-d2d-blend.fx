// Cairo - a vector graphics library with display and print output
//
// Copyright (C) 2010 Mozilla Foundation
// Copyright (C) 2026 Alexander A Pototskiy
//
// This library is free software; you can redistribute it and/or
// modify it either under the terms of the GNU Lesser General Public
// License version 2.1 as published by the Free Software Foundation
// (the "LGPL") or, at your option, under the terms of the Mozilla
// Public License Version 1.1 (the "MPL"). If you do not alter this
// notice, a recipient may use your version of this file under either
// the MPL or the LGPL.
//
// You should have received a copy of the LGPL along with this library
// in the file COPYING-LGPL-2.1; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA
// You should have received a copy of the MPL along with this library
// in the file COPYING-MPL-1.1
//
// The contents of this file are subject to the Mozilla Public License
// Version 1.1 (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://www.mozilla.org/MPL/
//
// This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
// OF ANY KIND, either express or implied. See the LGPL or the MPL for
// the specific language governing rights and limitations.
//
// The Original Code is the cairo graphics library.

// cairo-d2d-blend.fx
// Effect file for Cairo D2D backend
// Includes original SampleTexture, SampleTextTexture techniques
// and new BlendTexture technique for blend mode operators.
//
// Compile with: fxc /T fx_4_0 /O2 /Fh cairo-d2d-private-fx.h cairo-d2d-blend.fx

cbuffer cb0
{
    float4 QuadDesc;     // Offset: 0, size: 16
    float4 TexCoords;    // Offset: 16, size: 16
    float4 TextColor;    // Offset: 32, size: 16
    int    BlendMode;    // Offset: 48, size: 4
};

Texture2D tex;
Texture2D texDst;

SamplerState sSampler
{
    AddressU = CLAMP;
    AddressV = CLAMP;
};

BlendState bTextBlend
{
    AlphaToCoverageEnable = FALSE;
    BlendEnable[0] = TRUE;
    SrcBlend = SRC1_COLOR;
    DestBlend = INV_SRC1_COLOR;
    BlendOp = ADD;
    SrcBlendAlpha = SRC1_ALPHA;
    DestBlendAlpha = INV_SRC1_ALPHA;
    BlendOpAlpha = ADD;
    RenderTargetWriteMask[0] = 0x0f;
};

BlendState bNoBlend
{
    AlphaToCoverageEnable = FALSE;
    BlendEnable[0] = FALSE;
    RenderTargetWriteMask[0] = 0x0f;
};

// Shared vertex shader output
struct VS_OUTPUT
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

// Shared vertex shader
VS_OUTPUT VS_Quad(float2 pos : POSITION)
{
    VS_OUTPUT output;
    output.Position.xy = pos * QuadDesc.zw + QuadDesc.xy;
    output.Position.z = 0;
    output.Position.w = 1;
    output.TexCoord = pos * TexCoords.zw + TexCoords.xy;
    return output;
}

// Simple pass-through pixel shader (original SampleTexture)
float4 PS_Sample(VS_OUTPUT input) : SV_Target
{
    return tex.Sample(sSampler, input.TexCoord);
}

// Text pixel shader with dual-source blending (original SampleTextTexture)
struct PS_TEXT_OUTPUT
{
    float4 color : SV_Target0;
    float4 alpha : SV_Target1;
};

PS_TEXT_OUTPUT PS_SampleText(VS_OUTPUT input)
{
    PS_TEXT_OUTPUT output;
    output.color = TextColor;
    float4 texel = tex.Sample(sSampler, input.TexCoord);
    output.alpha = texel.zyxy * TextColor.w;
    return output;
}

// Blend mode pixel shader
float4 PS_Blend(VS_OUTPUT input) : SV_Target
{
    float4 src = tex.Sample(sSampler, input.TexCoord);
    float4 dst = texDst.Sample(sSampler, input.TexCoord);

    // Unpremultiply
    float3 srcC = src.a > 0.0f ? src.rgb / src.a : float3(0, 0, 0);
    float3 dstC = dst.a > 0.0f ? dst.rgb / dst.a : float3(0, 0, 0);

    float3 result;

    // BlendMode: 0=multiply, 1=screen, 2=overlay, 3=darken, 4=lighten,
    //            5=color_dodge, 6=color_burn, 7=hard_light, 8=soft_light,
    //            9=difference, 10=exclusion
    if (BlendMode == 0) {
        // MULTIPLY
        result = srcC * dstC;
    }
    else if (BlendMode == 1) {
        // SCREEN
        result = srcC + dstC - srcC * dstC;
    }
    else if (BlendMode == 2) {
        // OVERLAY: if dstC < 0.5 => 2*src*dst, else 1-2*(1-src)*(1-dst)
        result = (dstC < 0.5f)
            ? 2.0f * srcC * dstC
            : 1.0f - 2.0f * (1.0f - srcC) * (1.0f - dstC);
    }
    else if (BlendMode == 3) {
        // DARKEN
        result = min(srcC, dstC);
    }
    else if (BlendMode == 4) {
        // LIGHTEN
        result = max(srcC, dstC);
    }
    else if (BlendMode == 5) {
        // COLOR_DODGE
        result = dstC / max(1.0f - srcC, 0.001f);
    }
    else if (BlendMode == 6) {
        // COLOR_BURN
        result = 1.0f - (1.0f - dstC) / max(srcC, 0.001f);
    }
    else if (BlendMode == 7) {
        // HARD_LIGHT: if srcC < 0.5 => 2*src*dst, else 1-2*(1-src)*(1-dst)
        result = (srcC < 0.5f)
            ? 2.0f * srcC * dstC
            : 1.0f - 2.0f * (1.0f - srcC) * (1.0f - dstC);
    }
    else if (BlendMode == 8) {
        // SOFT_LIGHT (Pegtop variant)
        result = (1.0f - 2.0f * srcC) * dstC * dstC + 2.0f * srcC * dstC;
    }
    else if (BlendMode == 9) {
        // DIFFERENCE
        result = abs(srcC - dstC);
    }
    else if (BlendMode == 10) {
        // EXCLUSION
        result = srcC + dstC - 2.0f * srcC * dstC;
    }
    else {
        result = srcC;
    }

    result = saturate(result);

    // Porter-Duff compositing (OVER with blended colors):
    // Fa=1, Fb=1-srcA
    float outA = src.a + dst.a * (1.0f - src.a);
    float3 outRGB = result * src.a + dst.rgb * (1.0f - src.a);

    return float4(outRGB, outA); // premultiplied output
}

// Original technique: simple texture sampling with external BlendState
technique10 SampleTexture
{
    pass P0
    {
        SetVertexShader(CompileShader(vs_4_0, VS_Quad()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_4_0, PS_Sample()));
    }
}

// Original technique: text rendering with dual-source blending
technique10 SampleTextTexture
{
    pass P0
    {
        AB_BlendFactor = float4(0, 0, 0, 0);
        AB_SampleMask = uint(0xffffffff);
        SetBlendState(bTextBlend, float4(0, 0, 0, 0), 0xffffffff);
        SetVertexShader(CompileShader(vs_4_0, VS_Quad()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_4_0, PS_SampleText()));
    }
}

// New technique: blend mode compositing (no hardware blending - all in PS)
technique10 BlendTexture
{
    pass P0
    {
        SetBlendState(bNoBlend, float4(0, 0, 0, 0), 0xffffffff);
        SetVertexShader(CompileShader(vs_4_0, VS_Quad()));
        SetGeometryShader(NULL);
        SetPixelShader(CompileShader(ps_4_0, PS_Blend()));
    }
}
