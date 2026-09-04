// Copyright (C) 2012 Patryk Nadrowski
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Source-to-SPIR-V front end for the Vulkan driver's user shader materials, the ones
// addHighLevelShaderMaterial() and friends create. Pure translation: no VkDevice, no vk::* call and
// no Vulkan header, so turning the words into a VkShaderModule stays the caller's job.
//
// E_GPU_SHADING_LANGUAGE picks the language, and every language but the pre-compiled one is
// optional and macro-gated, the way _IRR_COMPILE_WITH_CG_ gates Cg:
//   EGSL_PCMP    -- already SPIR-V, so only validated and copied. Always available, no dependency.
//   EGSL_DEFAULT -- GLSL under _IRR_COMPILE_WITH_VULKAN_GLSLANG_, and HLSL under
//                   _IRR_COMPILE_WITH_VULKAN_DXC_ when only that one is built in.
//   EGSL_CG      -- never: Cg has no SPIR-V back end.

#ifndef __C_VULKAN_SHADER_COMPILER_H_INCLUDED__
#define __C_VULKAN_SHADER_COMPILER_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include <vector>

#include "irrTypes.h"
#include "irrString.h"
#include "EShaderTypes.h"
#include "IGPUProgrammingServices.h" // E_GPU_SHADING_LANGUAGE

namespace irr
{
	namespace io
	{
		class IFileSystem;
	}

	namespace video
	{
		//! First word of every SPIR-V module, in host byte order.
		static const u32 SpirvMagicWord = 0x07230203u;

		//! Smallest module possible, in bytes: the five-word SPIR-V header and nothing else.
		static const u32 SpirvHeaderSize = 20;

		//! Translates shader source into SPIR-V. All static: the two optional back ends are
		//! process-wide, and there is no per-instance state worth keeping.
		class CVulkanShaderCompiler
		{
		public:
			//! Compiles the source of one pipeline stage.
			/** \param sourceLength Length of source in BYTES; 0 means "null-terminated, measure it".
			\param entryPoint 0 or "" is read as "main". EGSL_PCMP ignores it, having it baked in.
			\param stage EST_STREAM_OUTPUT_SHADER has no Vulkan equivalent and is rejected; the .cpp
			carries the full stage table.
			\param outSpirv One entry per SPIR-V word. Cleared on entry, left empty on failure.
			\param outError The front end's own diagnostics (glslang's info log, DXC's error blob) or
			the reason the language is unavailable, for the driver to log. Always set on failure.
			\param includeFileSystem Where an HLSL `#include "x"` is looked up: `includeDirectory`/x
			first (the including file's own directory), then x as given, then media/shaders/x -- the
			same convention the D3D11/D3D12 drivers' include handlers use. 0 falls back to plain
			fopen() on the same candidates. GLSL takes no includes.
			\param includeDirectory Directory of the source file, or 0 for an in-memory source. */
			static bool compileToSpirv(const c8* source, u32 sourceLength, const c8* entryPoint,
				E_SHADER_TYPE stage, E_GPU_SHADING_LANGUAGE lang,
				std::vector<u32>& outSpirv, core::stringc& outError,
				io::IFileSystem* includeFileSystem = 0, const c8* includeDirectory = 0);

			//! Whether this build handles that language at all, so a caller can refuse a material up
			//! front instead of compiling every stage only to find out on the first one.
			static bool isLanguageSupported(E_GPU_SHADING_LANGUAGE lang);

			//! What compileToSpirv() would parse this language as ("GLSL", "HLSL", "pre-compiled
			//! SPIR-V", ...), for log messages. Never 0.
			static const c8* getLanguageName(E_GPU_SHADING_LANGUAGE lang);

			//! Releases what the optional back ends hold process-wide: glslang's per-process pools
			//! and the DXC library handle. Call at driver shutdown; a later compile re-acquires both.
			static void shutdown();
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
