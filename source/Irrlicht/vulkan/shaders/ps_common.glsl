// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Fragment-side shared code: user clip planes, fog, material textures.
// Port of ClipPlanesCB/FogCB/ApplyClipPlanes()/calcFogFactor()/ApplyFog() from
// CD3D12DefaultShaders.h.

#ifndef IRR_VK_PS_COMMON_GLSL
#define IRR_VK_PS_COMMON_GLSL

#include "common.glsl"

// setClipPlane/enableClipPlane. A disabled plane is uploaded as (0,0,0,1) so the dot product
// is always 1 and nothing is ever discarded.
layout(std140, set = 4, binding = 2) uniform ClipPlanesCB
{
    vec4 ClipPlanes[3];
};

#define FOGMODE_NONE   0
#define FOGMODE_LINEAR 1
#define FOGMODE_EXP    2
#define FOGMODE_EXP2   3
#define FOG_E 2.71828

// Mirrors SD3D12FogConstants (CD3D12Driver.cpp): vec4 + 4 words + 2 words.
layout(std140, set = 4, binding = 4) uniform FogCB
{
    vec4 FogColor;
    int FogMode;
    float FogStart;
    float FogEnd;
    float FogDensity;
    int EnableFog;
    // SMaterial::MaterialTypeParam, 0 replaced by 0.02 driver-side. Parallax shaders only.
    float ParallaxHeightScale;
};

// Material textures (SMaterial::TextureLayer[0..1]). The driver binds a default texture on
// layer 1 for single-texture materials so the set stays complete.
layout(set = 0, binding = 0) uniform sampler2D BaseTexture;
layout(set = 0, binding = 1) uniform sampler2D Layer1Texture;

// HLSL clip(x) discards for x < 0.
void applyClipPlanes(vec3 worldPos)
{
    vec4 wp = vec4(worldPos, 1.0);
    if (dot(wp, ClipPlanes[0]) < 0.0 || dot(wp, ClipPlanes[1]) < 0.0 || dot(wp, ClipPlanes[2]) < 0.0)
        discard;
}

float calcFogFactor(float d)
{
    float fogCoeff = 1.0;

    if (EnableFog == 0)
        return fogCoeff;

    switch (FogMode)
    {
    case FOGMODE_LINEAR:
        fogCoeff = (FogEnd - d) / (FogEnd - FogStart);
        break;
    case FOGMODE_EXP:
        fogCoeff = 1.0 / pow(FOG_E, d * FogDensity);
        break;
    case FOGMODE_EXP2:
        fogCoeff = 1.0 / pow(FOG_E, d * d * FogDensity * FogDensity);
        break;
    default:
        break;
    }

    return clamp(fogCoeff, 0.0, 1.0);
}

// Applied to all 4 channels including alpha, matching the reference.
vec4 applyFog(vec4 color, float fogDist)
{
    float fog = calcFogFactor(fogDist);
    return fog * color + (1.0 - fog) * FogColor;
}

#endif
