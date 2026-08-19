// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_LIGHTMAP_LIGHTING_M2.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ps_standard_in.glsl"

void main()
{
    applyClipPlanes(vWorldPos);
    vec4 tex1C = texture(BaseTexture, vUV);
    vec4 tex2C = texture(Layer1Texture, vUV);
    FragColor = applyFog((((tex1C * tex2C) * 2.0) * vColorD) + vColorS, vFogDist);
}
