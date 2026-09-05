// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Varyings written by standard.vert and (as a superset) standard_2tcoords.vert, so every
// fragment shader below pairs with either. Port of PSInput (CD3D12DefaultShaders.h).
// Location 5 (UV2) is declared only by the *_uv2 fragment shaders.

#ifndef IRR_VK_PS_STANDARD_IN_GLSL
#define IRR_VK_PS_STANDARD_IN_GLSL

#include "ps_common.glsl"

// Diffuse (ambient+emissive folded in) and specular. With EnableLighting == 0,
// vColorD = vertex colour and vColorS = 0.
layout(location = 0) in vec4 vColorD;
layout(location = 1) in vec4 vColorS;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vWorldPos;
// Reproduces the D3D11/D3D12 quirk: distance between clip space and view space.
layout(location = 4) in float vFogDist;

layout(location = 0) out vec4 FragColor;

#endif
