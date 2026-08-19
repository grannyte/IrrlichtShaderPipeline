// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// EVT_TANGENTS (S3DVertexTangents), used by the normal-map and parallax-map material types.
// Port of VSMainTangents (CD3D12DefaultShaders.h): lighting is deferred to the fragment
// stage, only the raw vertex colour and the world-space basis are forwarded.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUV;
layout(location = 4) in vec3 aTangent;
layout(location = 5) in vec3 aBinormal;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out vec3 vWorldNormal;
layout(location = 4) out vec3 vWorldTangent;
layout(location = 5) out vec3 vWorldBinormal;
layout(location = 6) out vec3 vWorldEyeDir;
layout(location = 7) out vec3 vViewPos;
layout(location = 8) out float vFogDist;

void main()
{
    vec4 worldPos = vec4(aPos, 1.0) * World;
    vec4 viewPos  = worldPos * View;
    vec4 clipPos  = viewPos * Proj;

    vUV = aUV;
    vWorldPos = worldPos.xyz;
    vViewPos = viewPos.xyz;
    vFogDist = distance(clipPos, viewPos); // same quirk as standard.vert

    vColor = aColor;
    // Not renormalized here; perturbNormalToWorld() does it per pixel.
    vWorldNormal   = aNormal   * mat3(World);
    vWorldTangent  = aTangent  * mat3(World);
    vWorldBinormal = aBinormal * mat3(World);
    vWorldEyeDir   = CameraPosWorld.xyz - worldPos.xyz;

    clipPos.y = -clipPos.y; // see standard.vert
    gl_Position = clipPos;
}
