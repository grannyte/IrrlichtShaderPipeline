// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Single embedded HLSL shader shared by all E_MATERIAL_TYPE values.
//
// PS entry point mapping:
//   - EMT_SOLID, EMT_TRANSPARENT_ALPHA_CHANNEL, EMT_TRANSPARENT_ADD_COLOR, EMT_ONETEXTURE_BLEND
//     -> PSMain (same color formula, blend state set per PSO, see buildPSOKeyFromMaterial()).
//   - EMT_TRANSPARENT_VERTEX_ALPHA -> PSMainVertexAlpha (alpha from vertex, not texture).
//   - EMT_TRANSPARENT_ALPHA_CHANNEL_REF -> PSMainAlphaTest (clip() at a fixed threshold, no blend).
//   - 12 multi-texture types (EMT_SOLID_2_LAYER, EMT_LIGHTMAP/_ADD/_M2/_M4/_LIGHTING/_LIGHTING_M2/
//     _LIGHTING_M4, EMT_DETAIL_MAP, EMT_SPHERE_MAP, EMT_REFLECTION_2_LAYER,
//     EMT_TRANSPARENT_REFLECTION_2_LAYER) each get their own PSMainXxx, matching
//     CD3D11FixedPipelineRenderer::standardPS()'s switch(material.type).
//   - EMT_SPHERE_MAP/EMT_TRANSPARENT_REFLECTION_2_LAYER have no real reflection implementation;
//     matches D3D11's "// TODO" placeholder.
//
// These 12 types sample Layer1Texture at the same UV as BaseTexture (input.UV, TEXCOORD0):
// only EVT_STANDARD is used for them. EVT_2TCOORDS meshes use VSMain2TCoords/PSMainXxxUV2
// instead, sampling Layer1Texture at its own UV (TEXCOORD1).
//
// Vertex layout, must match exactly:
//   - S3DVertex (include/S3DVertex.h): Pos(12b) + Normal(12b) + Color(4b, packed ARGB) +
//     TCoords(8b) = 36-byte stride. See kS3DVertexInputLayout in CD3D12Driver.cpp.
//
// Root signature (CD3D12Driver::createRootSignature()):
//   b0 = world (VS), b1 = view+proj (VS), t0/t1 = base texture + second layer (PS),
//   s0/s1 = per-layer sampler, b2 = user clip planes (PS), b3 = dynamic lighting (VS),
//   b4 = fog (PS).
//
// Dynamic lighting (SMaterial::Lighting/AmbientColor/DiffuseColor/SpecularColor/EmissiveColor/
// ColorMaterial/NormalizeNormals), up to MAX_LIGHTS=8. Per-vertex port of
// CD3D11FixedPipelineRenderer's calcLighting()/standardVS(), see calcLighting() below.
// Quirks kept for parity: specular exponent fixed at 64 (Shininess never read).
// SMaterial::ColorMaterial has no D3D11 equivalent; the semantics here are driver-specific,
// see ColorMaterialMode. When Lighting is false, ColorD=input.Color/ColorS=0, matching the
// pre-lighting behavior exactly.
//
// Fog (SMaterial::FogEnable), a port of calcFogFactor()/standardPS(), applied at the end of
// every PS entry point. FogDist reproduces a D3D11 quirk: distance(clipPos, viewPos) instead
// of a true world/view distance, see VSMain. FogCB::Mode stores E_FOG_TYPE directly with no
// conversion (matches bindFog()).
//
// Matrix convention not verified against core::matrix4: assumes mul(row_vector, matrix)
// (vector on the left). If core::matrix4 uses column-vector convention, matrices need
// transposing before upload, or mul() order needs reversing. To be verified at first render.

#ifndef __C_D3D12_DEFAULT_SHADERS_H_INCLUDED__
#define __C_D3D12_DEFAULT_SHADERS_H_INCLUDED__

namespace irr
{
	namespace video
	{
		// Driver's internal cbuffers live in space4, not space0: user shaders (written for
		// D3D11) declare their own constants at b0..b7/space0..space3, and PSO creation fails if the
		// root signature doesn't expose registers a shader reads. space0..space3 are reserved for
		// user shaders (see createRootSignature()); driver constants use space4 (out of that range).
		static const char* const D3D12DefaultShaderHLSL = R"(
cbuffer PerObject : register(b0, space4)
{
    float4x4 World;
};

cbuffer PerFrame : register(b1, space4)
{
    float4x4 View;
    float4x4 Proj;
    // World-space camera position, computed via the inverse view matrix (see
    // bindTransformsAndTexture()). Only VSMainTangents reads it.
    float4 CameraPosWorld;
};

// User clip planes (setClipPlane/enableClipPlane). A disabled plane is sent as (0,0,0,1),
// so dot(WorldPos1, plane) is always 1 and ApplyClipPlanes() never clips it.
cbuffer ClipPlanesCB : register(b2, space4)
{
    float4 ClipPlanes[3];
};

// Dynamic lighting. Layout must stay in sync with SD3D12LightingConstants/SD3D12ShaderLight/
// SD3D12ShaderLightMaterial (CD3D12Driver::bindLighting()).
#define MAX_LIGHTS 8

struct SLightGPU
{
    float4 Position; // xyz = world position, w unused (0)
    float4 Diffuse;
    float4 Specular;
    float4 Ambient;
    float4 Atten;    // x=constant, y=linear, z=quadratic, w unused (0)
};

struct SLightMaterialGPU
{
    float4 Ambient;
    float4 Diffuse;
    float4 Specular;
    float4 Emissive;
};

cbuffer LightingCB : register(b3, space4)
{
    SLightGPU Lights[MAX_LIGHTS];
    SLightMaterialGPU Mat;
    int LightCount;
    int EnableLighting;
    // E_COLOR_MATERIAL (SMaterial.h): ECM_NONE=0, ECM_DIFFUSE=1, ECM_AMBIENT=2,
    // ECM_EMISSIVE=3, ECM_SPECULAR=4, ECM_DIFFUSE_AND_AMBIENT=5. See calcLighting() below.
    int ColorMaterialMode;
    // SMaterial::NormalizeNormals -- worldNormal (VSMain) is always normalized regardless,
    // same as standardVS() on D3D11. Kept here to document the choice.
    int NormalizeNormalsFlag;
};

// Port of CD3D11FixedPipelineRenderer's calcLighting(): N.L diffuse + N.H specular, summed
// per active light. Specular exponent hardcoded to 64 (Shininess never read).
// cameraPos here is actually the view-space position, not world-space (matches
// standardVS()'s naming on the D3D11 side).
struct SLitColors
{
    float4 Diffuse;
    float4 Specular;
};

SLitColors calcLighting(float3 worldNormal, float3 worldPos, float3 cameraPos, float4 vertexColour)
{
    SLitColors output = (SLitColors)0;

    // SMaterial::ColorMaterial: vertex color modulates the corresponding material term(s)
    // before the light loop (glColorMaterial semantics).
    float4 matAmbient  = Mat.Ambient;
    float4 matDiffuse  = Mat.Diffuse;
    float4 matSpecular = Mat.Specular;
    float4 matEmissive = Mat.Emissive;
    if (ColorMaterialMode == 1 || ColorMaterialMode == 5) // ECM_DIFFUSE / ECM_DIFFUSE_AND_AMBIENT
        matDiffuse *= vertexColour;
    if (ColorMaterialMode == 2 || ColorMaterialMode == 5) // ECM_AMBIENT / ECM_DIFFUSE_AND_AMBIENT
        matAmbient *= vertexColour;
    if (ColorMaterialMode == 3) // ECM_EMISSIVE
        matEmissive *= vertexColour;
    if (ColorMaterialMode == 4) // ECM_SPECULAR
        matSpecular *= vertexColour;

    const int nLights = min(MAX_LIGHTS, LightCount);
    for (int i = 0; i < nLights; ++i)
    {
        float3 toLight = Lights[i].Position.xyz - worldPos;
        float lightDist = length(toLight);
        float fAtten = 1.0 / dot(Lights[i].Atten, float4(1, lightDist, lightDist * lightDist, 0));
        float3 lightDir = normalize(toLight);
        float3 halfAngle = normalize(normalize(-cameraPos) + lightDir);

        float4 _Ambient = Lights[i].Ambient * matAmbient;
        float4 _Diffuse = Lights[i].Diffuse * matDiffuse;
        float4 _Specular = Lights[i].Specular * matSpecular;
        float4 _Emissive = matEmissive;

        output.Diffuse += max(0, dot(lightDir, worldNormal) * _Diffuse * fAtten) + _Ambient + _Emissive;
        output.Specular += max(0, pow(abs(dot(halfAngle, worldNormal)), 64) * _Specular * fAtten);
    }

    return output;
}

// Fog (SMaterial::FogEnable). Layout must stay in sync with SD3D12FogConstants
// (CD3D12Driver::bindFog()). Mode stores E_FOG_TYPE directly, no conversion (see bindFog()).
#define FOGMODE_NONE   0
#define FOGMODE_LINEAR 1
#define FOGMODE_EXP    2
#define FOGMODE_EXP2   3
#define FOG_E 2.71828

cbuffer FogCB : register(b4, space4)
{
    float4 FogColor;
    int FogMode;
    float FogStart;
    float FogEnd;
    float FogDensity;
    int EnableFog;
    // Height-scale for parallax mapping (SMaterial::MaterialTypeParam; 0 defaults to 0.02f,
    // see EMT_PARALLAX_MAP_SOLID). Only PSMainParallaxMap*/PSMainParallaxMapVertexAlpha read it.
    float ParallaxHeightScale;
};

// Port of CD3D11FixedPipelineRenderer's calcFogFactor().
float calcFogFactor(float d)
{
    float fogCoeff = 1.0;

    if (!EnableFog)
        return fogCoeff;

    [branch]
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
    }

    return clamp(fogCoeff, 0, 1);
}

// Applied to all 4 channels including alpha, matching the D3D11 reference. Called at the
// end of every PS entry point below.
float4 ApplyFog(float4 color, float fogDist)
{
    float fog = calcFogFactor(fogDist);
    return fog * color + (1.0 - fog) * FogColor;
}

Texture2D BaseTexture : register(t0);
SamplerState BaseSampler : register(s0);
// Second texture layer (SMaterial::TextureLayer[1]). Not read by single-texture materials;
// the driver still binds a default texture/sampler here on every draw so the descriptor
// table stays valid.
Texture2D Layer1Texture : register(t1);
SamplerState Layer1Sampler : register(s1);

struct VSInput
{
    float3 Pos    : POSITION;
    float3 Normal : NORMAL;
    float4 Color  : COLOR;
    float2 UV     : TEXCOORD0;
};

struct PSInput
{
    float4 Pos      : SV_POSITION;
    // ColorD/ColorS: diffuse (ambient+emissive folded in) and specular, same split as
    // colorD/colorS on the D3D11 side. When EnableLighting is 0, ColorD=input.Color, ColorS=0.
    float4 ColorD   : COLOR0;
    float4 ColorS   : COLOR1;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    // See VSMain: reproduces a D3D11 quirk (distance between clip space and view space).
    float FogDist   : TEXCOORD2;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    float4 worldPos = mul(float4(input.Pos, 1.0), World);
    float4 viewPos  = mul(worldPos, View);
    o.Pos      = mul(viewPos, Proj);
    o.UV       = input.UV;
    o.WorldPos = worldPos.xyz;

    // Matches "output.fogDist = distance(output.pos, cameraPos);" on the D3D11 side
    // (standardVS()); "cameraPos" there is the view-space position, not world-space.
    o.FogDist = distance(o.Pos, viewPos);

    // Per-vertex dynamic lighting. When EnableLighting is 0 (the historical default),
    // ColorD=input.Color, ColorS=0.
    if (EnableLighting)
    {
        float3 worldNormal = normalize(mul(input.Normal, (float3x3)World));
        SLitColors lit = calcLighting(worldNormal, worldPos.xyz, viewPos.xyz, input.Color);
        o.ColorD = lit.Diffuse;
        o.ColorS = lit.Specular;
    }
    else
    {
        o.ColorD = input.Color;
        o.ColorS = float4(0, 0, 0, 0);
    }

    return o;
}

// Discards a pixel on the wrong side of an active clip plane. Same formula as
// CD3D11FixedPipelineRenderer (world-space clipping).
void ApplyClipPlanes(float3 worldPos)
{
    float4 wp = float4(worldPos, 1.0);
    clip(dot(wp, ClipPlanes[0]));
    clip(dot(wp, ClipPlanes[1]));
    clip(dot(wp, ClipPlanes[2]));
}

// tex*ColorD+ColorS. Reduces to tex*input.Color when Lighting is disabled (ColorD=Color, ColorS=0).
float4 PSMain(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 texColor = BaseTexture.Sample(BaseSampler, input.UV);
    return ApplyFog((texColor * input.ColorD) + input.ColorS, input.FogDist);
}

// EMT_TRANSPARENT_VERTEX_ALPHA: alpha comes only from the vertex, texture alpha is ignored.
// D3D11's standardPS() uses only colorS.a here, which would be permanently 0 (invisible)
// when Lighting is false; ColorD.a+ColorS.a is used instead so it still reduces to
// input.Color.a with Lighting disabled while allowing lighting to affect alpha when enabled.
float4 PSMainVertexAlpha(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 texColor = BaseTexture.Sample(BaseSampler, input.UV);
    float4 result;
    result.rgb = (texColor.rgb * input.ColorD.rgb) + input.ColorS.rgb;
    result.a = input.ColorD.a + input.ColorS.a;
    return ApplyFog(result, input.FogDist);
}

// EMT_TRANSPARENT_ALPHA_CHANNEL_REF: alpha test fixed at 127/255, no blend (matches
// D3DRS_ALPHAREF=127/D3DCMP_GREATEREQUAL). clip() discards when alpha < 127/255.
float4 PSMainAlphaTest(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 texColor = BaseTexture.Sample(BaseSampler, input.UV);
    clip(texColor.a - (127.0 / 255.0));
    return ApplyFog((texColor * input.ColorD) + input.ColorS, input.FogDist);
}

// The 12 entry points below sample BaseTexture and Layer1Texture at the same UV
// (EVT_STANDARD has no dedicated second UV). Direct translation of
// CD3D11FixedPipelineRenderer::standardPS()'s switch(material.type). EMT_LIGHTMAP/_ADD/_M2/_M4
// and EMT_DETAIL_MAP/EMT_SPHERE_MAP don't reference ColorD/ColorS, matching D3D11.

// EMT_SOLID_2_LAYER: blend based on the second texture's alpha.
float4 PSMainSolid2Layer(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog((lerp(tex1C, tex2C, tex1C.a) * input.ColorD) + input.ColorS, input.FogDist);
}

// EMT_LIGHTMAP: modulate (multiply); _ADD/_M2/_M4 are additive/x2/x4 variants.
float4 PSMainLightmap(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog(tex1C * tex2C, input.FogDist);
}

float4 PSMainLightmapAdd(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog(tex1C + tex2C, input.FogDist);
}

float4 PSMainLightmapM2(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog((tex1C * tex2C) * 2, input.FogDist);
}

float4 PSMainLightmapM4(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog((tex1C * tex2C) * 4, input.FogDist);
}

// _LIGHTING/_LIGHTING_M2/_LIGHTING_M4: same lightmap modulation, additionally modulated by
// ColorD with ColorS added.
float4 PSMainLightmapLighting(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog(((tex1C * tex2C) * input.ColorD) + input.ColorS, input.FogDist);
}

float4 PSMainLightmapLightingM2(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog((((tex1C * tex2C) * 2) * input.ColorD) + input.ColorS, input.FogDist);
}

float4 PSMainLightmapLightingM4(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog((((tex1C * tex2C) * 4) * input.ColorD) + input.ColorS, input.FogDist);
}

// EMT_DETAIL_MAP: additive blend centered on 0.5 (detail texture is neutral at mid-gray).
float4 PSMainDetailMap(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog((tex1C + tex2C) - 0.5, input.FogDist);
}

// EMT_SPHERE_MAP: D3D11 itself doesn't implement this ("// TODO", plain passthrough).
// Reproduced identically.
float4 PSMainSphereMap(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    return ApplyFog(BaseTexture.Sample(BaseSampler, input.UV), input.FogDist);
}

// EMT_REFLECTION_2_LAYER: same "TODO" note as EMT_SPHERE_MAP, but still applies
// tex1C*tex2C*colorD+colorS.
float4 PSMainReflection2Layer(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV);
    return ApplyFog(((tex1C * tex2C) * input.ColorD) + input.ColorS, input.FogDist);
}

// EMT_TRANSPARENT_REFLECTION_2_LAYER: same "TODO" placeholder, falls back to tex1C+colorS.
float4 PSMainTransparentReflection2Layer(PSInput input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    return ApplyFog(BaseTexture.Sample(BaseSampler, input.UV) + input.ColorS, input.FogDist);
}

// EVT_2TCOORDS (S3DVertex2TCoords): VSMain2TCoords propagates a second dedicated UV
// (TEXCOORD1) to the pixel shader, chosen by chooseVertexShaderForMaterial() in place of
// VSMain when the mesh uses this descriptor. The 12 PSMainXxxUV2 entry points below mirror
// PSMainXxx above, only Layer1Texture's UV changes (input.UV2 instead of input.UV).
struct VSInput2TCoords
{
    float3 Pos    : POSITION;
    float3 Normal : NORMAL;
    float4 Color  : COLOR;
    float2 UV     : TEXCOORD0;
    float2 UV2    : TEXCOORD1;
};

struct PSInput2TCoords
{
    float4 Pos      : SV_POSITION;
    float4 ColorD   : COLOR0;
    float4 ColorS   : COLOR1;
    float2 UV       : TEXCOORD0;
    float2 UV2      : TEXCOORD1;
    float3 WorldPos : TEXCOORD2;
    float FogDist   : TEXCOORD3;
};

PSInput2TCoords VSMain2TCoords(VSInput2TCoords input)
{
    PSInput2TCoords o;
    float4 worldPos = mul(float4(input.Pos, 1.0), World);
    float4 viewPos  = mul(worldPos, View);
    o.Pos      = mul(viewPos, Proj);
    o.UV       = input.UV;
    o.UV2      = input.UV2;
    o.WorldPos = worldPos.xyz;
    o.FogDist  = distance(o.Pos, viewPos); // same quirk as VSMain

    if (EnableLighting)
    {
        float3 worldNormal = normalize(mul(input.Normal, (float3x3)World));
        SLitColors lit = calcLighting(worldNormal, worldPos.xyz, viewPos.xyz, input.Color);
        o.ColorD = lit.Diffuse;
        o.ColorS = lit.Specular;
    }
    else
    {
        o.ColorD = input.Color;
        o.ColorS = float4(0, 0, 0, 0);
    }

    return o;
}

float4 PSMainSolid2LayerUV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog((lerp(tex1C, tex2C, tex1C.a) * input.ColorD) + input.ColorS, input.FogDist);
}

float4 PSMainLightmapUV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog(tex1C * tex2C, input.FogDist);
}

float4 PSMainLightmapAddUV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog(tex1C + tex2C, input.FogDist);
}

float4 PSMainLightmapM2UV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog((tex1C * tex2C) * 2, input.FogDist);
}

float4 PSMainLightmapM4UV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog((tex1C * tex2C) * 4, input.FogDist);
}

float4 PSMainLightmapLightingUV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog(((tex1C * tex2C) * input.ColorD) + input.ColorS, input.FogDist);
}

float4 PSMainLightmapLightingM2UV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog((((tex1C * tex2C) * 2) * input.ColorD) + input.ColorS, input.FogDist);
}

float4 PSMainLightmapLightingM4UV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog((((tex1C * tex2C) * 4) * input.ColorD) + input.ColorS, input.FogDist);
}

float4 PSMainDetailMapUV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog((tex1C + tex2C) - 0.5, input.FogDist);
}

// EMT_SPHERE_MAP: does not sample Layer1Texture (see PSMainSphereMap above); this variant
// exists only to accept PSInput2TCoords.
float4 PSMainSphereMapUV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    return ApplyFog(BaseTexture.Sample(BaseSampler, input.UV), input.FogDist);
}

float4 PSMainReflection2LayerUV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 tex1C = BaseTexture.Sample(BaseSampler, input.UV);
    float4 tex2C = Layer1Texture.Sample(Layer1Sampler, input.UV2);
    return ApplyFog(((tex1C * tex2C) * input.ColorD) + input.ColorS, input.FogDist);
}

// EMT_TRANSPARENT_REFLECTION_2_LAYER: same as PSMainSphereMapUV2, does not sample Layer1Texture.
float4 PSMainTransparentReflection2LayerUV2(PSInput2TCoords input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    return ApplyFog(BaseTexture.Sample(BaseSampler, input.UV) + input.ColorS, input.FogDist);
}

// Normal map / parallax map (EMT_NORMAL_MAP_*/EMT_PARALLAX_MAP_*, EVT_TANGENTS). D3D11's
// tangentsVS()/tangentsPS() are stubs (no normal map sampling), so there's nothing to port
// from them; the formula here is instead based on CD3D9NormalMapRenderer.cpp/
// CD3D9ParallaxMapRenderer.cpp, adapted to this file's multi-light world-space model
// (calcLighting() above). Lighting for these 6 types is evaluated per-pixel (not per-vertex)
// since the perturbed normal varies per texel. LightingCB (b3) is therefore also visible on
// the PS side (see createRootSignature()).

struct VSInputTangents
{
    float3 Pos      : POSITION;
    float3 Normal   : NORMAL;
    float4 Color    : COLOR;
    float2 UV       : TEXCOORD0;
    float3 Tangent  : TANGENT;
    float3 Binormal : BINORMAL;
};

struct PSInputTangents
{
    float4 Pos           : SV_POSITION;
    // Raw vertex color; PSMainNormalMap*/PSMainParallaxMap* compute colorD/colorS themselves
    // after perturbing the normal.
    float4 Color         : COLOR0;
    float2 UV             : TEXCOORD0;
    float3 WorldPos       : TEXCOORD1;
    // World-space T/B/N, not renormalized here (interpolation doesn't preserve unit length);
    // PerturbNormalToWorld() renormalizes per pixel.
    float3 WorldNormal    : TEXCOORD2;
    float3 WorldTangent   : TEXCOORD3;
    float3 WorldBinormal  : TEXCOORD4;
    // Unnormalized surface-to-camera vector, world space. Used only by PSMainParallaxMap*
    // for the tangent-space UV offset.
    float3 WorldEyeDir    : TEXCOORD5;
    // View-space position, feeds calcLighting()'s "cameraPos" parameter.
    float3 ViewPos        : TEXCOORD6;
    float FogDist         : TEXCOORD7;
};

PSInputTangents VSMainTangents(VSInputTangents input)
{
    PSInputTangents o;
    float4 worldPos = mul(float4(input.Pos, 1.0), World);
    float4 viewPos  = mul(worldPos, View);
    o.Pos      = mul(viewPos, Proj);
    o.UV       = input.UV;
    o.WorldPos = worldPos.xyz;
    o.ViewPos  = viewPos.xyz;
    o.FogDist  = distance(o.Pos, viewPos); // same quirk as VSMain

    o.Color = input.Color;
    o.WorldNormal   = mul(input.Normal,   (float3x3)World);
    o.WorldTangent  = mul(input.Tangent,  (float3x3)World);
    o.WorldBinormal = mul(input.Binormal, (float3x3)World);
    o.WorldEyeDir   = CameraPosWorld.xyz - worldPos.xyz;

    return o;
}

// Reconstructs the perturbed world-space normal from a decoded (-1..1) tangent-space normal
// map sample. T/B/N renormalized here (interpolation doesn't preserve length).
float3 PerturbNormalToWorld(float3 worldNormal, float3 worldTangent, float3 worldBinormal, float3 tangentSpaceNormal)
{
    float3 N = normalize(worldNormal);
    float3 T = normalize(worldTangent);
    float3 B = normalize(worldBinormal);
    return normalize(tangentSpaceNormal.x * T + tangentSpaceNormal.y * B + tangentSpaceNormal.z * N);
}

// EMT_NORMAL_MAP_SOLID (and EMT_NORMAL_MAP_TRANSPARENT_ADD_COLOR, same formula, blend mode
// differs per PSO). Layer1Texture carries the tangent-space normal map, decoded via *2-1.
float4 PSMainNormalMap(PSInputTangents input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 texColor = BaseTexture.Sample(BaseSampler, input.UV);
    float3 tangentNormal = normalize(Layer1Texture.Sample(Layer1Sampler, input.UV).rgb * 2.0 - 1.0);
    float3 worldNormal = PerturbNormalToWorld(input.WorldNormal, input.WorldTangent, input.WorldBinormal, tangentNormal);

    float4 colorD, colorS;
    if (EnableLighting)
    {
        SLitColors lit = calcLighting(worldNormal, input.WorldPos, input.ViewPos, input.Color);
        colorD = lit.Diffuse;
        colorS = lit.Specular;
    }
    else
    {
        colorD = input.Color;
        colorS = float4(0, 0, 0, 0);
    }

    return ApplyFog((texColor * colorD) + colorS, input.FogDist);
}

// EMT_NORMAL_MAP_TRANSPARENT_VERTEX_ALPHA: same alpha split as PSMainVertexAlpha, applied
// on top of the perturbed normal.
float4 PSMainNormalMapVertexAlpha(PSInputTangents input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);
    float4 texColor = BaseTexture.Sample(BaseSampler, input.UV);
    float3 tangentNormal = normalize(Layer1Texture.Sample(Layer1Sampler, input.UV).rgb * 2.0 - 1.0);
    float3 worldNormal = PerturbNormalToWorld(input.WorldNormal, input.WorldTangent, input.WorldBinormal, tangentNormal);

    float4 colorD, colorS;
    if (EnableLighting)
    {
        SLitColors lit = calcLighting(worldNormal, input.WorldPos, input.ViewPos, input.Color);
        colorD = lit.Diffuse;
        colorS = lit.Specular;
    }
    else
    {
        colorD = input.Color;
        colorS = float4(0, 0, 0, 0);
    }

    float4 result;
    result.rgb = (texColor.rgb * colorD.rgb) + colorS.rgb;
    result.a = colorD.a + colorS.a;
    return ApplyFog(result, input.FogDist);
}

// EMT_PARALLAX_MAP_SOLID (and EMT_PARALLAX_MAP_TRANSPARENT_ADD_COLOR, same formula). Like
// PSMainNormalMap, but offsets the UV before both samples using the height stored in the
// normal map's alpha channel and ParallaxHeightScale. Eye vector reprojected into
// tangent-space via dot product with T/B/N (its own inverse, being orthonormal).
float4 PSMainParallaxMap(PSInputTangents input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);

    float3 N = normalize(input.WorldNormal);
    float3 T = normalize(input.WorldTangent);
    float3 B = normalize(input.WorldBinormal);
    float3 eyeTangent = normalize(float3(dot(input.WorldEyeDir, T), dot(input.WorldEyeDir, B), dot(input.WorldEyeDir, N)));

    float height = Layer1Texture.Sample(Layer1Sampler, input.UV).a;
    float2 uv = input.UV + eyeTangent.xy * (height * ParallaxHeightScale);

    float4 texColor = BaseTexture.Sample(BaseSampler, uv);
    float3 tangentNormal = normalize(Layer1Texture.Sample(Layer1Sampler, uv).rgb * 2.0 - 1.0);
    float3 worldNormal = normalize(tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N);

    float4 colorD, colorS;
    if (EnableLighting)
    {
        SLitColors lit = calcLighting(worldNormal, input.WorldPos, input.ViewPos, input.Color);
        colorD = lit.Diffuse;
        colorS = lit.Specular;
    }
    else
    {
        colorD = input.Color;
        colorS = float4(0, 0, 0, 0);
    }

    return ApplyFog((texColor * colorD) + colorS, input.FogDist);
}

// EMT_PARALLAX_MAP_TRANSPARENT_VERTEX_ALPHA: same alpha split as PSMainNormalMapVertexAlpha,
// on top of PSMainParallaxMap's UV offset and perturbed normal.
float4 PSMainParallaxMapVertexAlpha(PSInputTangents input) : SV_TARGET
{
    ApplyClipPlanes(input.WorldPos);

    float3 N = normalize(input.WorldNormal);
    float3 T = normalize(input.WorldTangent);
    float3 B = normalize(input.WorldBinormal);
    float3 eyeTangent = normalize(float3(dot(input.WorldEyeDir, T), dot(input.WorldEyeDir, B), dot(input.WorldEyeDir, N)));

    float height = Layer1Texture.Sample(Layer1Sampler, input.UV).a;
    float2 uv = input.UV + eyeTangent.xy * (height * ParallaxHeightScale);

    float4 texColor = BaseTexture.Sample(BaseSampler, uv);
    float3 tangentNormal = normalize(Layer1Texture.Sample(Layer1Sampler, uv).rgb * 2.0 - 1.0);
    float3 worldNormal = normalize(tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N);

    float4 colorD, colorS;
    if (EnableLighting)
    {
        SLitColors lit = calcLighting(worldNormal, input.WorldPos, input.ViewPos, input.Color);
        colorD = lit.Diffuse;
        colorS = lit.Specular;
    }
    else
    {
        colorD = input.Color;
        colorS = float4(0, 0, 0, 0);
    }

    float4 result;
    result.rgb = (texColor.rgb * colorD.rgb) + colorS.rgb;
    result.a = colorD.a + colorS.a;
    return ApplyFog(result, input.FogDist);
}
)";

		// Mipmap generation shader. D3D12 has no equivalent to GenerateMips (see
		// CD3D12Texture.h): each mip is produced by a pixel-shader blit from the previous mip
		// (createMipGenPipeline()/generateMips()). Full-screen triangle from SV_VertexID, no
		// vertex buffer needed. Bilinear sampling at the destination texel center implicitly
		// averages the 4 corresponding source texels (a 2x2 box filter).
		static const char* const D3D12MipGenShaderHLSL = R"(
Texture2D SrcTexture : register(t0);
SamplerState LinearClampSampler : register(s0);

struct VSOut
{
    float4 Pos : SV_POSITION;
    float2 UV  : TEXCOORD0;
};

VSOut VSMipGen(uint vertexID : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.Pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    o.UV = uv;
    return o;
}

float4 PSMipGen(VSOut input) : SV_TARGET
{
    return SrcTexture.Sample(LinearClampSampler, input.UV);
}
)";

	}
}

#endif
