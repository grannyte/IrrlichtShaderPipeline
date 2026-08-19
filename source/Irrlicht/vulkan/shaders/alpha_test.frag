// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_TRANSPARENT_ALPHA_CHANNEL_REF: fixed alpha test at 127/255, no blend
// (D3DRS_ALPHAREF=127 / D3DCMP_GREATEREQUAL).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ps_standard_in.glsl"

void main()
{
    applyClipPlanes(vWorldPos);
    vec4 texColor = texture(BaseTexture, vUV);
    if (texColor.a < (127.0 / 255.0))
        discard;
    FragColor = applyFog((texColor * vColorD) + vColorS, vFogDist);
}
