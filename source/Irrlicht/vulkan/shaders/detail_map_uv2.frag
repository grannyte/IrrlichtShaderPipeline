// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_DETAIL_MAP: additive blend centred on 0.5 (detail texture neutral at mid-grey).
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
    FragColor = applyFog((tex1C + tex2C) - 0.5, vFogDist);
}
