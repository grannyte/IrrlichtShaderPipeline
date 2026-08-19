// Copyright (C) 2012 Patryk Nadrowski
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Enabling the optional front ends -- both are off by default and EGSL_PCMP needs neither:
//
// _IRR_COMPILE_WITH_VULKAN_GLSLANG_ (GLSL)
//   Install glslang (Khronos, https://github.com/KhronosGroup/glslang), add its include directory
//   to the project and link glslang, MachineIndependent, GenericCodeGen, OSDependent, SPIRV,
//   SPIRV-Tools, SPIRV-Tools-opt and glslang-default-resource-limits.
//
// _IRR_COMPILE_WITH_VULKAN_DXC_ (HLSL)
//   Install the Khronos build of DXC (https://github.com/microsoft/DirectXShaderCompiler) and add
//   its inc/ directory to the project, for dxcapi.h. No .lib is needed: dxcompiler.dll is opened
//   with LoadLibrary at run time and must sit beside the executable or on PATH. The dxcompiler.dll
//   shipped in the Windows SDK has no SPIR-V back end and will fail on -spirv.

#include "CVulkanShaderCompiler.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include <string.h>
#include "os.h"

#ifdef _IRR_COMPILE_WITH_VULKAN_GLSLANG_
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#endif

#ifdef _IRR_COMPILE_WITH_VULKAN_DXC_
#include <windows.h>
#include <dxcapi.h>
#include <string>
#endif

namespace irr
{
	namespace video
	{
		namespace
		{
			//! Bytes in source, measuring a null-terminated one when the caller passed 0.
			u32 effectiveLength(const c8* source, u32 sourceLength)
			{
				if (sourceLength > 0)
					return sourceLength;

				return source ? (u32)strlen(source) : 0;
			}

			//! EGSL_PCMP: the blob is already a module, so the only work is to check it looks like
			//! one and copy it word for word. Deliberately dependency-free -- this is the path that
			//! has to keep working with both optional macros off.
			bool copyPrecompiledSpirv(const c8* source, u32 length,
				std::vector<u32>& outSpirv, core::stringc& outError)
			{
				if (length < SpirvHeaderSize)
				{
					outError = "Pre-compiled shader is shorter than a SPIR-V header (20 bytes).";
					return false;
				}

				// SPIR-V is a word stream, so a byte count that is not a multiple of 4 cannot be one.
				if ((length & 3) != 0)
				{
					outError = "Pre-compiled shader length is not a multiple of 4, so it is not SPIR-V.";
					return false;
				}

				u32 magic = 0;
				memcpy(&magic, source, sizeof(magic));

				if (magic != SpirvMagicWord)
				{
					// 0x03022307 is the same word byte-swapped: a module built for the other
					// endianness, which is worth naming separately because it looks like garbage.
					outError = (magic == 0x03022307u)
						? "Pre-compiled shader is a byte-swapped SPIR-V module, built for the opposite endianness."
						: "Pre-compiled shader does not start with the SPIR-V magic word 0x07230203.";
					return false;
				}

				outSpirv.resize(length / 4);
				memcpy(&outSpirv[0], source, length);
				return true;
			}

#if defined(_IRR_COMPILE_WITH_VULKAN_GLSLANG_) || defined(_IRR_COMPILE_WITH_VULKAN_DXC_)
			//! Both front ends default an empty entry point to "main", as the engine's own callers do.
			const c8* effectiveEntryPoint(const c8* entryPoint)
			{
				return (entryPoint && entryPoint[0]) ? entryPoint : "main";
			}
#endif

#ifdef _IRR_COMPILE_WITH_VULKAN_GLSLANG_
			//! glslang keeps per-process pools that must be set up once and torn down once, so the
			//! first compile initialises and CVulkanShaderCompiler::shutdown() finalises.
			bool GlslangInitialised = false;

			//! E_SHADER_TYPE -> EShLanguage. EST_STREAM_OUTPUT_SHADER has no counterpart: Vulkan
			//! expresses stream-out through transform feedback, not through a stage of its own.
			bool glslangStage(E_SHADER_TYPE stage, EShLanguage& outLanguage)
			{
				switch (stage)
				{
				case EST_VERTEX_SHADER: outLanguage = EShLangVertex; return true;
				case EST_GEOMETRY_SHADER: outLanguage = EShLangGeometry; return true;
				case EST_PIXEL_SHADER: outLanguage = EShLangFragment; return true;
				case EST_HULL_SHADER: outLanguage = EShLangTessControl; return true;
				case EST_DOMAIN_SHADER: outLanguage = EShLangTessEvaluation; return true;
				case EST_COMPUTE_SHADER: outLanguage = EShLangCompute; return true;
				default: return false;
				}
			}

			bool compileGlslWithGlslang(const c8* source, u32 length, const c8* entryPoint,
				E_SHADER_TYPE stage, std::vector<u32>& outSpirv, core::stringc& outError)
			{
				EShLanguage language = EShLangVertex;

				if (!glslangStage(stage, language))
				{
					outError = "No Vulkan shader stage corresponds to this E_SHADER_TYPE.";
					return false;
				}

				if (!GlslangInitialised)
				{
					if (!glslang::InitializeProcess())
					{
						outError = "glslang::InitializeProcess() failed.";
						return false;
					}

					GlslangInitialised = true;
				}

				glslang::TShader shader(language);
				const char* const sourceStrings[1] = { source };
				const int sourceLengths[1] = { (int)length };
				shader.setStringsWithLengths(sourceStrings, sourceLengths, 1);
				shader.setEntryPoint(entryPoint);
				shader.setSourceEntryPoint(entryPoint);

				// Vulkan 1.0 / SPIR-V 1.0 as the target: the floor every Vulkan implementation
				// accepts, and nothing the engine's own shaders declare needs more than that.
				shader.setEnvInput(glslang::EShSourceGlsl, language, glslang::EShClientVulkan, 100);
				shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
				shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

				const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

				// 450 is only the fallback version, used when the source omits its own #version.
				if (!shader.parse(glslang::GetDefaultResources(), 450, false, messages))
				{
					outError = shader.getInfoLog();
					outError += shader.getInfoDebugLog();
					return false;
				}

				glslang::TProgram program;
				program.addShader(&shader);

				if (!program.link(messages))
				{
					outError = program.getInfoLog();
					outError += program.getInfoDebugLog();
					return false;
				}

				glslang::SpvOptions options;
				options.disableOptimizer = true;
				options.stripDebugInfo = false;

				spv::SpvBuildLogger logger;
				glslang::GlslangToSpv(*program.getIntermediate(language), outSpirv, &logger, &options);

				if (outSpirv.empty())
				{
					outError = "glslang produced no SPIR-V: ";
					outError += logger.getAllMessages().c_str();
					return false;
				}

				return true;
			}
#endif // _IRR_COMPILE_WITH_VULKAN_GLSLANG_

#ifdef _IRR_COMPILE_WITH_VULKAN_DXC_
			//! dxcompiler.dll is resolved at run time rather than linked, so a build with this macro
			//! on still starts on a machine without DXC -- the shader compile fails, nothing else.
			HMODULE DxcLibrary = 0;
			DxcCreateInstanceProc DxcCreateInstanceFn = 0;

			bool loadDxcLibrary(core::stringc& outError)
			{
				if (DxcCreateInstanceFn)
					return true;

				DxcLibrary = LoadLibraryA("dxcompiler.dll");

				if (!DxcLibrary)
				{
					outError = "dxcompiler.dll could not be loaded; HLSL shaders cannot be compiled.";
					return false;
				}

				DxcCreateInstanceFn = (DxcCreateInstanceProc)GetProcAddress(DxcLibrary, "DxcCreateInstance");

				if (!DxcCreateInstanceFn)
				{
					FreeLibrary(DxcLibrary);
					DxcLibrary = 0;
					outError = "dxcompiler.dll exports no DxcCreateInstance; it is not a DXC library.";
					return false;
				}

				return true;
			}

			//! E_SHADER_TYPE -> DXC target profile. Shader Model 6.0 throughout: the lowest model the
			//! DXIL-era compiler accepts, and the SPIR-V back end ignores the number anyway.
			const wchar_t* dxcTargetProfile(E_SHADER_TYPE stage)
			{
				switch (stage)
				{
				case EST_VERTEX_SHADER: return L"vs_6_0";
				case EST_GEOMETRY_SHADER: return L"gs_6_0";
				case EST_PIXEL_SHADER: return L"ps_6_0";
				case EST_HULL_SHADER: return L"hs_6_0";
				case EST_DOMAIN_SHADER: return L"ds_6_0";
				case EST_COMPUTE_SHADER: return L"cs_6_0";
				default: return 0;
				}
			}

			//! DXC takes wide strings only, and the entry point arrives as c8*.
			std::wstring widen(const c8* text)
			{
				const int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, 0, 0);

				if (needed <= 1)
					return std::wstring();

				std::wstring result((size_t)(needed - 1), L'\0');
				MultiByteToWideChar(CP_UTF8, 0, text, -1, &result[0], needed);
				return result;
			}

			bool compileHlslWithDxc(const c8* source, u32 length, const c8* entryPoint,
				E_SHADER_TYPE stage, std::vector<u32>& outSpirv, core::stringc& outError)
			{
				const wchar_t* const profile = dxcTargetProfile(stage);

				if (!profile)
				{
					outError = "No HLSL target profile corresponds to this E_SHADER_TYPE.";
					return false;
				}

				if (!loadDxcLibrary(outError))
					return false;

				IDxcCompiler3* compiler = 0;

				if (FAILED(DxcCreateInstanceFn(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || !compiler)
				{
					outError = "DxcCreateInstance(CLSID_DxcCompiler) failed.";
					return false;
				}

				DxcBuffer sourceBuffer;
				sourceBuffer.Ptr = source;
				sourceBuffer.Size = length;
				sourceBuffer.Encoding = DXC_CP_UTF8;

				const std::wstring entry = widen(entryPoint);

				// -spirv is what makes this a Vulkan compile at all; without it DXC emits DXIL.
				const wchar_t* arguments[] =
				{
					L"-E", entry.c_str(),
					L"-T", profile,
					L"-spirv",
					L"-fspv-target-env=vulkan1.0"
				};

				IDxcResult* result = 0;
				const HRESULT compiled = compiler->Compile(&sourceBuffer, arguments,
					(UINT32)(sizeof(arguments) / sizeof(arguments[0])), 0, IID_PPV_ARGS(&result));

				if (FAILED(compiled) || !result)
				{
					compiler->Release();
					outError = "IDxcCompiler3::Compile() failed before it could report diagnostics.";
					return false;
				}

				HRESULT status = S_OK;
				result->GetStatus(&status);

				if (FAILED(status))
				{
					// The error blob is the compiler's own message, so pass it through untouched.
					IDxcBlobUtf8* errors = 0;
					result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), 0);

					if (errors && errors->GetStringLength() > 0)
						outError = errors->GetStringPointer();
					else
						outError = "DXC rejected the shader but produced no error text.";

					if (errors)
						errors->Release();

					result->Release();
					compiler->Release();
					return false;
				}

				IDxcBlob* object = 0;
				result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), 0);

				const bool haveWords = object && object->GetBufferSize() >= SpirvHeaderSize
					&& (object->GetBufferSize() & 3) == 0;

				if (haveWords)
				{
					outSpirv.resize(object->GetBufferSize() / 4);
					memcpy(&outSpirv[0], object->GetBufferPointer(), object->GetBufferSize());
				}
				else
				{
					outError = "DXC reported success but returned no usable SPIR-V object.";
				}

				if (object)
					object->Release();

				result->Release();
				compiler->Release();
				return haveWords;
			}
#endif // _IRR_COMPILE_WITH_VULKAN_DXC_
		}

		bool CVulkanShaderCompiler::compileToSpirv(const c8* source, u32 sourceLength,
			const c8* entryPoint, E_SHADER_TYPE stage, E_GPU_SHADING_LANGUAGE lang,
			std::vector<u32>& outSpirv, core::stringc& outError)
		{
			outSpirv.clear();
			outError = "";

			const u32 length = effectiveLength(source, sourceLength);

			if (!source || length == 0)
			{
				outError = "Shader source is empty.";
				os::Printer::log(outError.c_str(), ELL_ERROR);
				return false;
			}

			switch (lang)
			{
			case EGSL_PCMP:
				if (copyPrecompiledSpirv(source, length, outSpirv, outError))
					return true;
				break;

			case EGSL_DEFAULT:
#if defined(_IRR_COMPILE_WITH_VULKAN_GLSLANG_)
				// GLSL wins when glslang is built in: EGSL_DEFAULT means "the driver's own
				// language", and the Vulkan backend's built-in shaders are GLSL.
				if (compileGlslWithGlslang(source, length, effectiveEntryPoint(entryPoint), stage,
						outSpirv, outError))
					return true;
#elif defined(_IRR_COMPILE_WITH_VULKAN_DXC_)
				// Only DXC is built in, so EGSL_DEFAULT falls back to meaning HLSL -- the same
				// sources the Direct3D backends take, which is the useful reading of "default" here.
				if (compileHlslWithDxc(source, length, effectiveEntryPoint(entryPoint), stage,
						outSpirv, outError))
					return true;
#else
				outError = "The Vulkan driver was built without a shader source compiler. Define "
					"_IRR_COMPILE_WITH_VULKAN_GLSLANG_ for GLSL or _IRR_COMPILE_WITH_VULKAN_DXC_ for "
					"HLSL in IrrCompileConfig.h, or supply pre-compiled SPIR-V with EGSL_PCMP.";
#endif
				break;

			case EGSL_CG:
				outError = "Cg is not supported by the Vulkan driver: it has no SPIR-V back end. Use "
					"GLSL, HLSL or pre-compiled SPIR-V (EGSL_PCMP) instead.";
				break;

			default:
				outError = "Unknown E_GPU_SHADING_LANGUAGE value passed to the Vulkan driver.";
				break;
			}

			outSpirv.clear();
			os::Printer::log(outError.c_str(), ELL_ERROR);
			return false;
		}

		bool CVulkanShaderCompiler::isLanguageSupported(E_GPU_SHADING_LANGUAGE lang)
		{
			switch (lang)
			{
			case EGSL_PCMP:
				return true;

			case EGSL_DEFAULT:
#if defined(_IRR_COMPILE_WITH_VULKAN_GLSLANG_) || defined(_IRR_COMPILE_WITH_VULKAN_DXC_)
				return true;
#else
				return false;
#endif

			default: // EGSL_CG, and anything added later
				return false;
			}
		}

		const c8* CVulkanShaderCompiler::getLanguageName(E_GPU_SHADING_LANGUAGE lang)
		{
			switch (lang)
			{
			case EGSL_PCMP:
				return "pre-compiled SPIR-V";

			case EGSL_CG:
				return "Cg (unsupported)";

			case EGSL_DEFAULT:
#if defined(_IRR_COMPILE_WITH_VULKAN_GLSLANG_)
				return "GLSL";
#elif defined(_IRR_COMPILE_WITH_VULKAN_DXC_)
				return "HLSL";
#else
				return "none (no shader source compiler built in)";
#endif
			}

			return "unknown";
		}

		void CVulkanShaderCompiler::shutdown()
		{
#ifdef _IRR_COMPILE_WITH_VULKAN_GLSLANG_
			if (GlslangInitialised)
			{
				glslang::FinalizeProcess();
				GlslangInitialised = false;
			}
#endif

#ifdef _IRR_COMPILE_WITH_VULKAN_DXC_
			if (DxcLibrary)
			{
				FreeLibrary(DxcLibrary);
				DxcLibrary = 0;
				DxcCreateInstanceFn = 0;
			}
#endif
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
