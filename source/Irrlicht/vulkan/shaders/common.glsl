// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Shared uniform blocks + per-vertex/per-pixel lighting. Port of the LightingCB block and
// calcLighting() from CD3D12DefaultShaders.h. Driver constants live in descriptor set 4,
// mirroring DriverConstantRegisterSpace (space4) on the D3D12 side; sets 0..3 stay free.
//
// Matrix convention: the HLSL uses mul(row_vector, matrix). GLSL's `v * M` is the exact
// equivalent for the same bytes (HLSL column_major cbuffer packing == std140 column_major),
// so no transpose is needed on upload.

#ifndef IRR_VK_COMMON_GLSL
#define IRR_VK_COMMON_GLSL

#define MAX_LIGHTS 8

layout(std140, set = 4, binding = 0) uniform PerObject
{
    mat4 World;
};

layout(std140, set = 4, binding = 1) uniform PerFrame
{
    mat4 View;
    mat4 Proj;
    // World-space camera position (inverse view). Only the tangent VS reads it.
    vec4 CameraPosWorld;
};

// Layout must stay byte-compatible with SD3D12ShaderLight (CD3D12Driver.cpp): 5 * vec4 = 80 b.
struct SLightGPU
{
    vec4 Position; // xyz = world position, w unused (0)
    vec4 Diffuse;
    vec4 Specular;
    vec4 Ambient;
    vec4 Atten;    // x=constant, y=linear, z=quadratic, w unused (0)
};

// Mirrors SD3D12ShaderLightMaterial: 4 * vec4 = 64 b.
struct SLightMaterialGPU
{
    vec4 Ambient;
    vec4 Diffuse;
    vec4 Specular;
    vec4 Emissive;
};

// Mirrors SD3D12LightingConstants: Lights[8] (640 b) + Mat (64 b) + 4 ints (16 b) = 720 b.
layout(std140, set = 4, binding = 3) uniform LightingCB
{
    SLightGPU Lights[MAX_LIGHTS];
    SLightMaterialGPU Mat;
    int LightCount;
    int EnableLighting;
    // E_COLOR_MATERIAL: ECM_NONE=0, ECM_DIFFUSE=1, ECM_AMBIENT=2, ECM_EMISSIVE=3,
    // ECM_SPECULAR=4, ECM_DIFFUSE_AND_AMBIENT=5.
    int ColorMaterialMode;
    // SMaterial::NormalizeNormals -- normals are normalized unconditionally, same as D3D12.
    int NormalizeNormalsFlag;
};

struct SLitColors
{
    vec4 Diffuse;
    vec4 Specular;
};

// N.L diffuse + N.H specular summed over the active lights. Specular exponent hardcoded to
// 64 (Shininess never read), kept for parity with the D3D12/D3D11 path.
// cameraPos is the view-space position, matching the HLSL naming.
SLitColors calcLighting(vec3 worldNormal, vec3 worldPos, vec3 cameraPos, vec4 vertexColour)
{
    SLitColors result;
    result.Diffuse = vec4(0.0);
    result.Specular = vec4(0.0);

    vec4 matAmbient  = Mat.Ambient;
    vec4 matDiffuse  = Mat.Diffuse;
    vec4 matSpecular = Mat.Specular;
    vec4 matEmissive = Mat.Emissive;
    if (ColorMaterialMode == 1 || ColorMaterialMode == 5) // ECM_DIFFUSE / ECM_DIFFUSE_AND_AMBIENT
        matDiffuse *= vertexColour;
    if (ColorMaterialMode == 2 || ColorMaterialMode == 5) // ECM_AMBIENT / ECM_DIFFUSE_AND_AMBIENT
        matAmbient *= vertexColour;
    if (ColorMaterialMode == 3) // ECM_EMISSIVE
        matEmissive *= vertexColour;
    if (ColorMaterialMode == 4) // ECM_SPECULAR
        matSpecular *= vertexColour;

    int nLights = min(MAX_LIGHTS, LightCount);
    for (int i = 0; i < nLights; ++i)
    {
        vec3 toLight = Lights[i].Position.xyz - worldPos;
        float lightDist = length(toLight);
        float fAtten = 1.0 / dot(Lights[i].Atten, vec4(1.0, lightDist, lightDist * lightDist, 0.0));
        vec3 lightDir = normalize(toLight);
        vec3 halfAngle = normalize(normalize(-cameraPos) + lightDir);

        vec4 _Ambient  = Lights[i].Ambient * matAmbient;
        vec4 _Diffuse  = Lights[i].Diffuse * matDiffuse;
        vec4 _Specular = Lights[i].Specular * matSpecular;
        vec4 _Emissive = matEmissive;

        result.Diffuse += max(vec4(0.0), dot(lightDir, worldNormal) * _Diffuse * fAtten) + _Ambient + _Emissive;
        result.Specular += max(vec4(0.0), pow(abs(dot(halfAngle, worldNormal)), 64.0) * _Specular * fAtten);
    }

    return result;
}

#endif
