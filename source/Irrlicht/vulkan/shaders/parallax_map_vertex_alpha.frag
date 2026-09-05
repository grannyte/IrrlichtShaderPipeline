// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EMT_PARALLAX_MAP_TRANSPARENT_VERTEX_ALPHA: parallax_map.frag with the vertex-alpha split.
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

    vec4 result;
    result.rgb = (texColor.rgb * colorD.rgb) + colorS.rgb;
    result.a = colorD.a + colorS.a;
    FragColor = applyFog(result, vFogDist);
}
