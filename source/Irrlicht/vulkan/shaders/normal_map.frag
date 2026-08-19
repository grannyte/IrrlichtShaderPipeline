// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_NORMAL_MAP_SOLID / EMT_NORMAL_MAP_TRANSPARENT_ADD_COLOR (blend differs per pipeline).
// Layer 1 carries the tangent-space normal map, decoded via *2-1. Lighting is per pixel.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ps_tangents_in.glsl"

void main()
{
    applyClipPlanes(vWorldPos);
    vec4 texColor = texture(BaseTexture, vUV);
    vec3 tangentNormal = normalize(texture(Layer1Texture, vUV).rgb * 2.0 - 1.0);
    vec3 worldNormal = perturbNormalToWorld(vWorldNormal, vWorldTangent, vWorldBinormal, tangentNormal);

    vec4 colorD, colorS;
    computeLitColors(worldNormal, colorD, colorS);

    FragColor = applyFog((texColor * colorD) + colorS, vFogDist);
}
