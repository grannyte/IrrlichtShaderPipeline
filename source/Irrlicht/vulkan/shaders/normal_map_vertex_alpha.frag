// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_NORMAL_MAP_TRANSPARENT_VERTEX_ALPHA: same alpha split as vertex_alpha.frag, on top of
// the perturbed normal.
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

    vec4 result;
    result.rgb = (texColor.rgb * colorD.rgb) + colorS.rgb;
    result.a = colorD.a + colorS.a;
    FragColor = applyFog(result, vFogDist);
}
