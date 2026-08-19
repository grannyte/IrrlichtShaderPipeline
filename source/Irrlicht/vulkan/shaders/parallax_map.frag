// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_PARALLAX_MAP_SOLID / EMT_PARALLAX_MAP_TRANSPARENT_ADD_COLOR. Like normal_map.frag but
// offsets the UV before both samples, using the height in the normal map's alpha channel and
// ParallaxHeightScale. The eye vector is reprojected into tangent space by dotting with the
// orthonormal T/B/N (its own inverse).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "ps_tangents_in.glsl"

void main()
{
    applyClipPlanes(vWorldPos);

    vec3 N = normalize(vWorldNormal);
    vec3 T = normalize(vWorldTangent);
    vec3 B = normalize(vWorldBinormal);
    vec3 eyeTangent = normalize(vec3(dot(vWorldEyeDir, T), dot(vWorldEyeDir, B), dot(vWorldEyeDir, N)));

    float height = texture(Layer1Texture, vUV).a;
    vec2 uv = vUV + eyeTangent.xy * (height * ParallaxHeightScale);

    vec4 texColor = texture(BaseTexture, uv);
    vec3 tangentNormal = normalize(texture(Layer1Texture, uv).rgb * 2.0 - 1.0);
    vec3 worldNormal = normalize(tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N);

    vec4 colorD, colorS;
    computeLitColors(worldNormal, colorD, colorS);

    FragColor = applyFog((texColor * colorD) + colorS, vFogDist);
}
