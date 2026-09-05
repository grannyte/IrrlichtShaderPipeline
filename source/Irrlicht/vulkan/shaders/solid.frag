// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_SOLID / EMT_TRANSPARENT_ALPHA_CHANNEL / EMT_TRANSPARENT_ADD_COLOR /
// EMT_ONETEXTURE_BLEND. Same colour formula, blend state differs per pipeline.
// Reduces to tex * vertex colour when lighting is off (vColorD = colour, vColorS = 0).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ps_standard_in.glsl"

void main()
{
    applyClipPlanes(vWorldPos);
    vec4 texColor = texture(BaseTexture, vUV);
    FragColor = applyFog((texColor * vColorD) + vColorS, vFogDist);
}
