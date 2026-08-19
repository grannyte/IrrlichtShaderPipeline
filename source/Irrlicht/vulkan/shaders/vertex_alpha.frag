// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_TRANSPARENT_VERTEX_ALPHA: alpha comes from the vertex only, texture alpha ignored.
// vColorD.a + vColorS.a (not vColorS.a alone) so it still reduces to the vertex alpha with
// lighting disabled -- same deviation from D3D11 as the D3D12 backend.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ps_standard_in.glsl"

void main()
{
    applyClipPlanes(vWorldPos);
    vec4 texColor = texture(BaseTexture, vUV);
    vec4 result;
    result.rgb = (texColor.rgb * vColorD.rgb) + vColorS.rgb;
    result.a = vColorD.a + vColorS.a;
    FragColor = applyFog(result, vFogDist);
}
