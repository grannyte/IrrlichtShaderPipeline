// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Stream output on Vulkan: the D3D drivers take a D3D11_SO_DECLARATION_ENTRY list built from the
// vertexTypeOut descriptor and hand it to the API; Vulkan's transform feedback has no such call --
// the layout lives in the shader, as XfbBuffer/XfbStride/Offset decorations on the output
// variables of the last pre-rasterization stage plus the Xfb execution mode. HLSL has no syntax for
// those, so this patches them into the SPIR-V DXC produced, matching each attribute of the
// descriptor to an output variable by semantic. A module already carrying Xfb decorations (GLSL
// with layout(xfb_*)) is left alone.

#ifndef __C_VULKAN_SPIRV_XFB_H_INCLUDED__
#define __C_VULKAN_SPIRV_XFB_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include <vector>
#include "irrTypes.h"
#include "irrString.h"

namespace irr
{
	namespace video
	{
		class IVertexDescriptor;

		//! Decorates `spirv` so transform feedback captures the descriptor's attributes.
		/** Each attribute names a D3D semantic (POSITION, NORMAL, COLOR, TEXCOORD, TANGENT, BINORMAL,
		BLENDWEIGHT, BLENDINDICES, CUSTOM) plus an index counted the way CD3D11VertexDescriptor
		builds its stream-output declaration (every TEXCOORDn shares one counter). An output variable
		matches through, in order: its UserSemantic decoration (DXC's -fspv-reflect, "SV_Position"
		counting as POSITION), its OpName (DXC's "out.var.<SEMANTIC>"), the Position built-in for
		POSITION, and finally location order for whatever is left, with a warning.

		The attribute's buffer id becomes the XfbBuffer, the descriptor's vertex size of that buffer
		the XfbStride, the attribute's offset the Offset. A variable wider or narrower than its
		attribute is reported in `outWarnings` -- SPIR-V captures whole variables, so the stride the
		descriptor promises would no longer hold.

		DXC's reflection extensions (SPV_GOOGLE_hlsl_functionality1 / user_type and their
		OpDecorateString instructions) are stripped on the way out, so the module stays free of
		extensions a driver may not accept.
		\return False (outError set) when an attribute matched nothing, or the module is malformed. */
		bool decorateSpirvForStreamOutput(std::vector<u32>& spirv, const IVertexDescriptor* layout,
			core::stringc& outError, core::stringc* outWarnings = 0);

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
