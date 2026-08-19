// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EVT_STANDARD (S3DVertex, 36-byte stride). Port of VSMain (CD3D12DefaultShaders.h).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUV;

layout(location = 0) out vec4 vColorD;
layout(location = 1) out vec4 vColorS;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vWorldPos;
layout(location = 4) out float vFogDist;

void main()
{
    vec4 worldPos = vec4(aPos, 1.0) * World;
    vec4 viewPos  = worldPos * View;
    vec4 clipPos  = viewPos * Proj;

    vUV = aUV;
    vWorldPos = worldPos.xyz;

    // Matches "output.fogDist = distance(output.pos, cameraPos);" on the D3D11/D3D12 side,
    // where "cameraPos" is the view-space position. Computed before the y flip so the value
    // stays identical to the D3D12 backend.
    vFogDist = distance(clipPos, viewPos);

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

    // Vulkan NDC has +y down where D3D has +y up: flipped here so the driver can use a
    // plain positive-height VkViewport. Depth needs nothing: Proj is already the fork's
    // reversed-Z matrix (near -> ~1, far -> ~0) and Vulkan's z range [0,1] matches D3D's.
    clipPos.y = -clipPos.y;
    gl_Position = clipPos;
}
