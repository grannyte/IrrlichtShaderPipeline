// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_SPHERE_MAP: no real reflection, passthrough -- reproduces the D3D11/D3D12
// placeholder exactly. Does not sample the second layer, so the _uv2 variant is identical.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ps_standard_in.glsl"

void main()
{
    applyClipPlanes(vWorldPos);
    FragColor = applyFog(texture(BaseTexture, vUV), vFogDist);
}
