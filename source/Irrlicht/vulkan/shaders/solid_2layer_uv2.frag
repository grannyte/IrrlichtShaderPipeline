// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_SOLID_2_LAYER: blend driven by the first texture's alpha.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ps_standard_in.glsl"

// Second layer gets its own UV (EVT_2TCOORDS), written by standard_2tcoords.vert.
layout(location = 5) in vec2 vUV2;

void main()
{
    applyClipPlanes(vWorldPos);
    vec4 tex1C = texture(BaseTexture, vUV);
    vec4 tex2C = texture(Layer1Texture, vUV2);
    FragColor = applyFog((mix(tex1C, tex2C, tex1C.a) * vColorD) + vColorS, vFogDist);
}
