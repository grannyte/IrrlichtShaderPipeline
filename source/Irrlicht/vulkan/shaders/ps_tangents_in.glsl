// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Varyings written by tangents.vert. Port of PSInputTangents (CD3D12DefaultShaders.h).
// Lighting is evaluated per pixel here, so LightingCB is read on the fragment side too.

#ifndef IRR_VK_PS_TANGENTS_IN_GLSL
#define IRR_VK_PS_TANGENTS_IN_GLSL

#include "ps_common.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;
// World-space T/B/N, renormalized per pixel (interpolation does not preserve unit length).
layout(location = 3) in vec3 vWorldNormal;
layout(location = 4) in vec3 vWorldTangent;
layout(location = 5) in vec3 vWorldBinormal;
// Unnormalized surface-to-camera vector, world space. Parallax only.
layout(location = 6) in vec3 vWorldEyeDir;
// View-space position, feeds calcLighting()'s cameraPos parameter.
layout(location = 7) in vec3 vViewPos;
layout(location = 8) in float vFogDist;

layout(location = 0) out vec4 FragColor;

vec3 perturbNormalToWorld(vec3 worldNormal, vec3 worldTangent, vec3 worldBinormal, vec3 tangentSpaceNormal)
{
    vec3 N = normalize(worldNormal);
    vec3 T = normalize(worldTangent);
    vec3 B = normalize(worldBinormal);
    return normalize(tangentSpaceNormal.x * T + tangentSpaceNormal.y * B + tangentSpaceNormal.z * N);
}

// Shared tail of the 4 tangent shaders: per-pixel lighting on the perturbed normal.
void computeLitColors(vec3 worldNormal, out vec4 colorD, out vec4 colorS)
{
    if (EnableLighting != 0)
    {
        SLitColors lit = calcLighting(worldNormal, vWorldPos, vViewPos, vColor);
        colorD = lit.Diffuse;
        colorS = lit.Specular;
    }
    else
    {
        colorD = vColor;
        colorS = vec4(0.0);
    }
}

#endif
