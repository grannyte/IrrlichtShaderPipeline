// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EVT_2TCOORDS (S3DVertex2TCoords). Port of VSMain2TCoords (CD3D12DefaultShaders.h).
// Outputs are a superset of standard.vert's, so the non-*_uv2 fragment shaders pair with
// this one too (an unconsumed vertex output is legal).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUV;
layout(location = 4) in vec2 aUV2;

layout(location = 0) out vec4 vColorD;
layout(location = 1) out vec4 vColorS;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vWorldPos;
layout(location = 4) out float vFogDist;
layout(location = 5) out vec2 vUV2;

void main()
{
    vec4 worldPos = vec4(aPos, 1.0) * World;
    vec4 viewPos  = worldPos * View;
    vec4 clipPos  = viewPos * Proj;

    vUV = aUV;
    vUV2 = aUV2;
    vWorldPos = worldPos.xyz;
    vFogDist = distance(clipPos, viewPos); // same quirk as standard.vert

    if (EnableLighting != 0)
    {
        vec3 worldNormal = normalize(aNormal * mat3(World));
        SLitColors lit = calcLighting(worldNormal, worldPos.xyz, viewPos.xyz, aColor);
        vColorD = lit.Diffuse;
        vColorS = lit.Specular;
    }
    else
    {
        vColorD = aColor;
        vColorS = vec4(0.0);
    }

    clipPos.y = -clipPos.y; // see standard.vert
    gl_Position = clipPos;
}
