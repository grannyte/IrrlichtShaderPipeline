// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CD3D12MaterialRenderer.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include <d3d12shader.h> // D3DReflect(), see reflectCBuffer()
#include <dxcapi.h>      // Shader Model 6 through DXC, see compileStageShaderModel6()
#include <windows.h>
#include <cstring>
#include <string>
#include <algorithm> // std::min(), see setConstantBuffer()
#include "os.h"
#include "IFileSystem.h"
#include "IReadFile.h"

namespace irr
{
	namespace video
	{
		// Mirrors CD3D11MaterialRenderer.cpp's CShaderInclude (same class, duplicated rather than
		// shared: CD3D11MaterialRenderer.cpp doesn't export it outside its own file). Resolves an
		// HLSL "#include \"X\"" to "media/shaders/X" via Irrlicht's IFileSystem -- without this,
		// D3DCompile(..., pInclude=nullptr, ...) simply fails whenever a shader includes a shared
		// .hlsl/.hlsli file instead of inlining everything into one string.
		class CD3D12ShaderInclude : public ID3DInclude
		{
		public:
			CD3D12ShaderInclude(io::IFileSystem* fileSystem) : FileSystem(fileSystem) {}

			STDMETHOD(Close(LPCVOID pData))
			{
				delete[] static_cast<const c8*>(pData);
				return S_OK;
			}

			STDMETHOD(Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes))
			{
				core::stringc path = "media//shaders//";
				path.append(pFileName);
				io::IReadFile* file = FileSystem->createAndOpenFile(path);
				if (!file)
				{
					os::Printer::log("CD3D12ShaderInclude: could not open included file", path.c_str(), ELL_ERROR);
					return S_FALSE;
				}

				u32 size = file->getSize();
				*pBytes = size;
				c8* data = new c8[size + 1];
				file->read(data, size);
				data[size] = '\0';
				*ppData = data;
				file->drop();
				return S_OK;
			}

		private:
			io::IFileSystem* FileSystem;
		};

		// ---------------- Shader Model 6 through DXC ----------------
		// dxcompiler.dll is resolved at run time (no import library), so a build always starts on a
		// machine without it and PreferShaderModel6 simply falls back to FXC there.
		namespace
		{
			HMODULE DxcModule = 0;
			HMODULE DxilModule = 0;
			DxcCreateInstanceProc DxcCreateInstanceFn = 0;
			bool DxcProbed = false;

			std::wstring widenUtf8(const c8* text)
			{
				const int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, 0, 0);
				if (needed <= 1)
					return std::wstring();
				std::wstring result((size_t)(needed - 1), L'\0');
				MultiByteToWideChar(CP_UTF8, 0, text, -1, &result[0], needed);
				return result;
			}

			core::stringc narrowUtf8(const wchar_t* text)
			{
				const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, 0, 0, 0, 0);
				if (needed <= 1)
					return core::stringc();
				std::string result((size_t)(needed - 1), '\0');
				WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], needed, 0, 0);
				return core::stringc(result.c_str());
			}

			//! The DXC counterpart of CD3D12ShaderInclude: media/shaders/<name> through the engine's
			//! file system, then the name as written. Stack-allocated by its one caller, which holds
			//! the initial reference, so Release() never deletes.
			class CD3D12DxcInclude : public IDxcIncludeHandler
			{
			public:
				CD3D12DxcInclude(IDxcUtils* utils, io::IFileSystem* fileSystem)
					: Utils(utils), FileSystem(fileSystem), RefCount(1) {}

				HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
				{
					if (!ppvObject)
						return E_POINTER;
					if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcIncludeHandler))
					{
						*ppvObject = static_cast<IDxcIncludeHandler*>(this);
						AddRef();
						return S_OK;
					}
					*ppvObject = 0;
					return E_NOINTERFACE;
				}
				ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&RefCount); }
				ULONG STDMETHODCALLTYPE Release() override { return (ULONG)InterlockedDecrement(&RefCount); }

				HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override
				{
					if (!ppIncludeSource)
						return E_POINTER;
					*ppIncludeSource = 0;
					if (!FileSystem || !Utils)
						return E_FAIL;

					// DXC hands over "./name" for a quoted include; only the name the shader wrote matters.
					core::stringc name = narrowUtf8(pFilename);
					name.replace('\\', '/');
					while (name.size() > 2 && name[0] == '.' && name[1] == '/')
						name = name.subString(2, (s32)name.size() - 2);
					core::stringc baseName = name;
					const s32 slash = name.findLast('/');
					if (slash >= 0)
						baseName = name.subString(slash + 1, (s32)name.size() - slash - 1);

					const core::stringc candidates[2] = { core::stringc("media/shaders/") + baseName, name };
					for (u32 i = 0; i < 2; ++i)
					{
						const io::path path(candidates[i].c_str());
						if (!FileSystem->existFile(path))
							continue;
						io::IReadFile* file = FileSystem->createAndOpenFile(path);
						if (!file)
							continue;
						std::vector<c8> bytes((size_t)core::max_(file->getSize(), 0L));
						if (!bytes.empty() && file->read(bytes.data(), (u32)bytes.size()) != (s32)bytes.size())
							bytes.clear();
						file->drop();

						IDxcBlobEncoding* blob = 0;
						if (FAILED(Utils->CreateBlob(bytes.data(), (UINT32)bytes.size(), DXC_CP_UTF8, &blob)) || !blob)
							return E_OUTOFMEMORY;
						*ppIncludeSource = blob;
						return S_OK;
					}
					os::Printer::log("CD3D12MaterialRenderer: could not open included shader file", name.c_str(), ELL_ERROR);
					return E_FAIL;
				}

			private:
				IDxcUtils* Utils;
				io::IFileSystem* FileSystem;
				LONG RefCount;
			};
		}

		bool CD3D12MaterialRenderer::isShaderModel6Available()
		{
			if (DxcCreateInstanceFn)
				return true;
			if (DxcProbed)
				return false;
			DxcProbed = true;
			// dxil.dll first: DXC signs its DXIL through the validator it finds loaded, and an unsigned
			// blob is refused by the runtime outside developer mode.
			DxilModule = LoadLibraryA("dxil.dll");
			DxcModule = LoadLibraryA("dxcompiler.dll");
			if (!DxilModule || !DxcModule)
			{
				os::Printer::log("CD3D12MaterialRenderer: dxcompiler.dll / dxil.dll not beside the executable, "
					"Shader Model 6 unavailable", ELL_INFORMATION);
				return false;
			}
			DxcCreateInstanceFn = (DxcCreateInstanceProc)GetProcAddress(DxcModule, "DxcCreateInstance");
			return DxcCreateInstanceFn != 0;
		}

		bool CD3D12MaterialRenderer::compileStageShaderModel6(const c8* source, const c8* entryPoint, const wchar_t* profile,
			const c8* stageName, io::IFileSystem* fileSystem, ComPtr<ID3DBlob>& outCode,
			std::vector<SD3D12UserShaderCBuffer>& outBuffers, std::vector<SD3D12UserShaderVariable>& outVariables)
		{
			outCode.Reset();
			outBuffers.clear();
			outVariables.clear();
			const core::stringc prefix = core::stringc("CD3D12MaterialRenderer: user ") + stageName + " shader (SM 6): ";
			if (!isShaderModel6Available())
			{
				os::Printer::log(prefix.c_str(), "DXC unavailable", ELL_ERROR);
				return false;
			}

			ComPtr<IDxcCompiler3> compiler;
			ComPtr<IDxcUtils> utils;
			if (FAILED(DxcCreateInstanceFn(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
				FAILED(DxcCreateInstanceFn(CLSID_DxcUtils, IID_PPV_ARGS(&utils))))
			{
				os::Printer::log(prefix.c_str(), "DxcCreateInstance failed", ELL_ERROR);
				return false;
			}

			DxcBuffer buffer = {};
			buffer.Ptr = source;
			buffer.Size = strlen(source);
			buffer.Encoding = DXC_CP_UTF8;
			const std::wstring entry = widenUtf8(entryPoint && entryPoint[0] ? entryPoint : "main");

			// HLSL 2018: the closest dialect to what FXC parsed, so a shader written for SM 5 compiles
			// unchanged (2021's stricter overload rules reject some of that code). IRR_SM6 lets a
			// source opt into wave intrinsics and friends behind an #ifdef.
			std::vector<LPCWSTR> arguments;
			arguments.push_back(L"-E");
			arguments.push_back(entry.c_str());
			arguments.push_back(L"-T");
			arguments.push_back(profile);
			arguments.push_back(L"-HV");
			arguments.push_back(L"2018");
			arguments.push_back(L"-D");
			arguments.push_back(L"IRR_SM6=1");
#ifdef _DEBUG
			arguments.push_back(L"-Zi");
			arguments.push_back(L"-Od");
			arguments.push_back(L"-Qembed_debug");
#else
			arguments.push_back(L"-O3");
#endif

			CD3D12DxcInclude include(utils.Get(), fileSystem);
			ComPtr<IDxcResult> result;
			if (FAILED(compiler->Compile(&buffer, arguments.data(), (UINT32)arguments.size(),
				fileSystem ? &include : nullptr, IID_PPV_ARGS(&result))) || !result)
			{
				os::Printer::log(prefix.c_str(), "IDxcCompiler3::Compile failed before reporting diagnostics", ELL_ERROR);
				return false;
			}

			HRESULT status = S_OK;
			result->GetStatus(&status);
			ComPtr<IDxcBlobUtf8> errors;
			result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
			if (FAILED(status))
			{
				os::Printer::log(prefix.c_str(), errors && errors->GetStringLength() ? errors->GetStringPointer() :
					"rejected without diagnostics", ELL_ERROR);
				return false;
			}

			ComPtr<IDxcBlob> object;
			result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
			if (!object || object->GetBufferSize() == 0)
			{
				os::Printer::log(prefix.c_str(), "no DXIL object produced", ELL_ERROR);
				return false;
			}
			if (FAILED(D3DCreateBlob(object->GetBufferSize(), &outCode)))
				return false;
			memcpy(outCode->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());

			// D3DReflect() cannot read DXIL; the reflection part DXC emits goes through its own utils.
			ComPtr<IDxcBlob> reflectionBlob;
			result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflectionBlob), nullptr);
			if (!reflectionBlob)
			{
				os::Printer::log(prefix.c_str(), "no reflection data produced", ELL_ERROR);
				return false;
			}
			DxcBuffer reflectionBuffer = {};
			reflectionBuffer.Ptr = reflectionBlob->GetBufferPointer();
			reflectionBuffer.Size = reflectionBlob->GetBufferSize();
			ComPtr<ID3D12ShaderReflection> reflector;
			if (FAILED(utils->CreateReflection(&reflectionBuffer, IID_PPV_ARGS(&reflector))))
			{
				os::Printer::log(prefix.c_str(), "IDxcUtils::CreateReflection failed", ELL_ERROR);
				return false;
			}
			return reflectFromReflector(reflector.Get(), outBuffers, outVariables);
		}

		CD3D12MaterialRenderer::~CD3D12MaterialRenderer()
		{
			if (CallBack)
				CallBack->drop();
			if (StreamOutputVertexType)
				StreamOutputVertexType->drop();
		}

		void* CD3D12MaterialRenderer::getShaderByteCode() const
		{
			if (VS)
				return VS->GetBufferPointer();
			if (GS)
				return GS->GetBufferPointer();
			return nullptr;
		}

		u32 CD3D12MaterialRenderer::getShaderByteCodeSize() const
		{
			if (VS)
				return static_cast<u32>(VS->GetBufferSize());
			if (GS)
				return static_cast<u32>(GS->GetBufferSize());
			return 0;
		}

		// BUILT-IN shaders compile at Shader Model 5.1, not 5.0: they declare their cbuffers in a
		// dedicated register space ("register(b0, space4)", see CD3D12DefaultShaders.h /
		// DriverConstantRegisterSpace), and the "spaceN" syntax only exists from SM 5.1 onward --
		// it's a compile error under 5_0. SM 5.1 is available on all D3D12 hardware, so there's no
		// compatibility cost.
		//
		// USER shaders, on the other hand, still compile at whatever target the caller requested
		// (compileFromHLSL(), typically 5_0): they use no register space, only the default space0,
		// which 5_0 can express just fine.
		bool CD3D12MaterialRenderer::compileBuiltIn(const c8* hlslSource, const c8* vertexShaderEntryPoint,
			const c8* pixelShaderEntryPoint, const c8* vertexShaderEntryPoint2TCoords, const c8* pixelShaderEntryPointUV2)
		{
			UINT compileFlags = 0;
#ifdef _DEBUG
			compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
			//compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
			//compileFlags |= D3DCOMPILE_AVOID_FLOW_CONTROL;
#endif
			ComPtr<ID3DBlob> vs, ps, errors;

			HRESULT hr = D3DCompile(hlslSource, strlen(hlslSource), "D3D12DefaultShaderHLSL",
				nullptr, nullptr, vertexShaderEntryPoint, "vs_5_1", compileFlags, 0, &vs, &errors);
			if (FAILED(hr))
			{
				if (errors)
					os::Printer::log("CD3D12MaterialRenderer: built-in vertex shader compilation: ",
						static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
				return false;
			}

			errors.Reset();
			hr = D3DCompile(hlslSource, strlen(hlslSource), "D3D12DefaultShaderHLSL",
				nullptr, nullptr, pixelShaderEntryPoint, "ps_5_1", compileFlags, 0, &ps, &errors);
			if (FAILED(hr))
			{
				if (errors)
					os::Printer::log("CD3D12MaterialRenderer: built-in pixel shader compilation: ",
						static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
				return false;
			}

			VS = vs;
			PS = ps;

			// EVT_2TCOORDS variant, optional (see the file header comment).
			if (vertexShaderEntryPoint2TCoords && pixelShaderEntryPointUV2)
			{
				ComPtr<ID3DBlob> vs2t, ps2t;
				errors.Reset();
				hr = D3DCompile(hlslSource, strlen(hlslSource), "D3D12DefaultShaderHLSL",
					nullptr, nullptr, vertexShaderEntryPoint2TCoords, "vs_5_1", compileFlags, 0, &vs2t, &errors);
				if (FAILED(hr))
				{
					if (errors)
						os::Printer::log("CD3D12MaterialRenderer: built-in vertex shader compilation (2tcoords): ",
							static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
					return false;
				}

				errors.Reset();
				hr = D3DCompile(hlslSource, strlen(hlslSource), "D3D12DefaultShaderHLSL",
					nullptr, nullptr, pixelShaderEntryPointUV2, "ps_5_1", compileFlags, 0, &ps2t, &errors);
				if (FAILED(hr))
				{
					if (errors)
						os::Printer::log("CD3D12MaterialRenderer: built-in pixel shader compilation (2tcoords): ",
							static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
					return false;
				}

				VS2TCoords = vs2t;
				PS2TCoords = ps2t;
			}

#ifdef _DEBUG
			DebugVSSource = hlslSource;
			DebugVSEntryPoint = vertexShaderEntryPoint;
			DebugPSSource = hlslSource;
			DebugPSEntryPoint = pixelShaderEntryPoint;
#endif

			return true;
		}

		bool CD3D12MaterialRenderer::reflectCBuffer(ID3DBlob* code, std::vector<SD3D12UserShaderCBuffer>& outBuffers,
			std::vector<SD3D12UserShaderVariable>& outVariables)
		{
			outBuffers.clear();
			outVariables.clear();
			if (!code)
				return true; // stage absent (PS/VS optional in the generic API) -- nothing to reflect

			// FXC bytecode only: DXIL is reflected through IDxcUtils, see compileStageShaderModel6().
			ComPtr<ID3D12ShaderReflection> reflector;
			HRESULT hr = D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
				IID_PPV_ARGS(&reflector));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12MaterialRenderer: D3DReflect failed on a user shader", ELL_ERROR);
				return false;
			}
			return reflectFromReflector(reflector.Get(), outBuffers, outVariables);
		}

		bool CD3D12MaterialRenderer::reflectFromReflector(ID3D12ShaderReflection* reflector,
			std::vector<SD3D12UserShaderCBuffer>& outBuffers, std::vector<SD3D12UserShaderVariable>& outVariables)
		{
			outBuffers.clear();
			outVariables.clear();
			if (!reflector)
				return false;

			D3D12_SHADER_DESC shaderDesc = {};
			reflector->GetDesc(&shaderDesc);

			for (UINT i = 0; i < shaderDesc.BoundResources; ++i)
			{
				D3D12_SHADER_INPUT_BIND_DESC bindDesc = {};
				reflector->GetResourceBindingDesc(i, &bindDesc);
				if (bindDesc.Type != D3D_SIT_CBUFFER)
					continue;

				if (bindDesc.BindPoint >= MaxUserShaderCBVSlotsPerStage)
				{
					os::Printer::log("CD3D12MaterialRenderer: cbuffer register outside the b0..b7 table, "
						"skipped -- see MaxUserShaderCBVSlotsPerStage: ", bindDesc.Name, ELL_WARNING);
					continue;
				}

				// The root signature only reserves tables for a bounded set of spaces (see
				// MaxUserShaderRegisterSpaces) -- it's built once at device creation, not per-shader,
				// so a shader can't just declare any space it likes. Rejecting here with a clear
				// message is far better than letting it reach CreateGraphicsPipelineState: under the
				// D3D12 debug layer that mismatch has been observed to crash the whole process (a
				// fail-fast inside Microsoft's own D3D12SDKLayers.dll while it reports the error),
				// not just fail this one shader.
				if (bindDesc.Space >= MaxUserShaderRegisterSpaces)
				{
					os::Printer::log("CD3D12MaterialRenderer: cbuffer register space not reserved by the "
						"root signature (raise MaxUserShaderRegisterSpaces and mirror into "
						"CD3D12Driver::createRootSignature()/bindDrawState() if this space is genuinely "
						"needed), skipped: ", bindDesc.Name, ELL_ERROR);
					continue;
				}

				bool duplicate = false;
				for (const SD3D12UserShaderCBuffer& existing : outBuffers)
				{
					if (existing.BindPoint == bindDesc.BindPoint && existing.Space == bindDesc.Space)
					{
						os::Printer::log("CD3D12MaterialRenderer: two cbuffers reflected at the same "
							"register/space, keeping the first, skipped: ", bindDesc.Name, ELL_WARNING);
						duplicate = true;
						break;
					}
				}
				if (duplicate)
					continue;

				ID3D12ShaderReflectionConstantBuffer* cbuffer = reflector->GetConstantBufferByName(bindDesc.Name);
				D3D12_SHADER_BUFFER_DESC bufferDesc = {};
				cbuffer->GetDesc(&bufferDesc);

				SD3D12UserShaderCBuffer newBuffer;
				newBuffer.Name = bindDesc.Name;
				newBuffer.BindPoint = bindDesc.BindPoint;
				newBuffer.Space = bindDesc.Space;
				newBuffer.Scratch.assign(bufferDesc.Size, 0);
				outBuffers.push_back(newBuffer);
				const s32 bufferIndex = static_cast<s32>(outBuffers.size() - 1);

				for (UINT v = 0; v < bufferDesc.Variables; ++v)
				{
					ID3D12ShaderReflectionVariable* var = cbuffer->GetVariableByIndex(v);
					D3D12_SHADER_VARIABLE_DESC varDesc = {};
					var->GetDesc(&varDesc);

					SD3D12UserShaderVariable sv;
					sv.Name = varDesc.Name;
					sv.Buffer = bufferIndex;
					sv.Offset = varDesc.StartOffset;
					sv.Size = varDesc.Size;

					// See SD3D12UserShaderVariable::TransposeOnSet: this is where column-major
					// float4x4 (HLSL's default) is detected, so it can be transposed on write like
					// CD3D11MaterialRenderer::setVariable() does.
					if (ID3D12ShaderReflectionType* varType = var->GetType())
					{
						D3D12_SHADER_TYPE_DESC typeDesc = {};
						if (SUCCEEDED(varType->GetDesc(&typeDesc)))
						{
							// Restricted to float4x4: the only shape core::matrix4 knows how to
							// transpose. A column-major float3x3 would still end up misoriented, but
							// no engine shader passes one (and CD3D11MaterialRenderer, which casts to
							// core::matrix4 without checking size, would do worse).
							sv.TransposeOnSet = (typeDesc.Class == D3D_SVC_MATRIX_COLUMNS) &&
								typeDesc.Type == D3D_SVT_FLOAT &&
								typeDesc.Rows == 4 && typeDesc.Columns == 4;
						}
					}

					outVariables.push_back(sv);
				}
			}

			return true;
		}

		bool CD3D12MaterialRenderer::compileFromHLSL(
			const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
			const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			const c8* hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
			const c8* domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
			io::IFileSystem* fileSystem)
		{
			if (!vertexShaderProgram || !vertexShaderProgram[0])
			{
				os::Printer::log("CD3D12MaterialRenderer::compileFromHLSL: vertex shader is required", ELL_ERROR);
				return false;
			}
			// Null if fileSystem is null (backward compat: a caller that doesn't supply a
			// filesystem just loses #include resolution, as before this fix).
			CD3D12ShaderInclude includeHandler(fileSystem);
			ID3DInclude* pInclude = fileSystem ? &includeHandler : nullptr;
			// pixelShaderProgram is optional: a pure stream-output GS (see
			// StreamOutputVertexType/RasterizedStream=D3D12_SO_NO_RASTERIZED_STREAM on the
			// CD3D12Driver::getPSOForMaterial() side) legitimately has no pixel shader -- nothing
			// ever reaches the rasterizer. choosePixelShaderForMaterial() already falls back to the
			// built-in "solid" PS when PS is empty (see its comment), so no change is needed on the
			// PSO-building side.
			bool hasPixel = pixelShaderProgram && pixelShaderProgram[0];

			// Shader Model 6 (SIrrlichtCreationParameters::PreferShaderModel6): every stage through
			// DXC at xs_6_0, the requested SM 4/5 targets ignored. Same stage rules as the FXC path.
			if (UseShaderModel6)
			{
				ComPtr<ID3DBlob> vs6, ps6, gs6, hs6, ds6;
				if (!compileStageShaderModel6(vertexShaderProgram, vertexShaderEntryPointName, L"vs_6_0", "vertex",
					fileSystem, vs6, VSBuffers, VSVariables))
					return false;
				if (hasPixel && !compileStageShaderModel6(pixelShaderProgram, pixelShaderEntryPointName, L"ps_6_0", "pixel",
					fileSystem, ps6, PSBuffers, PSVariables))
					return false;
				if (!hasPixel)
				{
					PSBuffers.clear();
					PSVariables.clear();
				}
				if (geometryShaderProgram && geometryShaderProgram[0] &&
					!compileStageShaderModel6(geometryShaderProgram, geometryShaderEntryPointName, L"gs_6_0", "geometry",
						fileSystem, gs6, GSBuffers, GSVariables))
					return false;
				const bool hasHull6 = hullShaderProgram && hullShaderProgram[0];
				const bool hasDomain6 = domainShaderProgram && domainShaderProgram[0];
				if (hasHull6 != hasDomain6)
				{
					os::Printer::log("CD3D12MaterialRenderer::compileFromHLSL: hull and domain shader "
						"must be supplied together (D3D12 tessellation requires both stages)", ELL_ERROR);
					return false;
				}
				if (hasHull6 && (!compileStageShaderModel6(hullShaderProgram, hullShaderEntryPointName, L"hs_6_0", "hull",
					fileSystem, hs6, HSBuffers, HSVariables) ||
					!compileStageShaderModel6(domainShaderProgram, domainShaderEntryPointName, L"ds_6_0", "domain",
						fileSystem, ds6, DSBuffers, DSVariables)))
					return false;
				VS = vs6;
				PS = ps6;
				GS = gs6;
				HS = hs6;
				DS = ds6;
				BlendMode = SPSOKey::EBlendMode::None;
				return true;
			}

			// D3D12 requires Shader Model 5.0 minimum (root signature 1.1) -- same fallback as
			// CD3D11MaterialRenderer::init() for a target < 4.0, including the
			// D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY flag (without it, HLSL written in the
			// pre-SM4 style, e.g. vs_1_1..vs_3_0 syntax/intrinsics, can fail to compile even once
			// the target is bumped to vs_5_0/ps_5_0).
			UINT compileFlags = 0;
			if (vsCompileTarget < EVST_VS_4_0 || vsCompileTarget >= EVST_COUNT ||
				(hasPixel && (psCompileTarget < EPST_PS_4_0 || psCompileTarget >= EPST_COUNT)))
			{
				compileFlags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
				vsCompileTarget = EVST_VS_5_0;
				psCompileTarget = EPST_PS_5_0;
			}

#ifdef _DEBUG
			compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
			// These flags allow maximum performance
			/*compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
			if (strstr(pixelShaderEntryPointName, "pixelNoiseMain") == NULL)
			{
				compileFlags |= D3DCOMPILE_AVOID_FLOW_CONTROL;
			}
			else
			{
				compileFlags |= D3DCOMPILE_PREFER_FLOW_CONTROL;
			}*/
#endif

			ComPtr<ID3DBlob> vs, ps, errors;
			HRESULT hr = D3DCompile(vertexShaderProgram, strlen(vertexShaderProgram), "user_vertex_shader",
				nullptr, pInclude, vertexShaderEntryPointName && vertexShaderEntryPointName[0] ? vertexShaderEntryPointName : "main",
				VERTEX_SHADER_TYPE_NAMES[vsCompileTarget], compileFlags, 0, &vs, &errors);
			if (FAILED(hr))
			{
				if (errors)
					os::Printer::log("CD3D12MaterialRenderer: user vertex shader compilation: ",
						static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
				return false;
			}

			if (hasPixel)
			{
				errors.Reset();
				hr = D3DCompile(pixelShaderProgram, strlen(pixelShaderProgram), "user_pixel_shader",
					nullptr, pInclude, pixelShaderEntryPointName && pixelShaderEntryPointName[0] ? pixelShaderEntryPointName : "main",
					PIXEL_SHADER_TYPE_NAMES[psCompileTarget], compileFlags, 0, &ps, &errors);
				if (FAILED(hr))
				{
					if (errors)
						os::Printer::log("CD3D12MaterialRenderer: user pixel shader compilation: ",
							static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
					return false;
				}
			}

			if (!reflectCBuffer(vs.Get(), VSBuffers, VSVariables) ||
				!reflectCBuffer(ps.Get(), PSBuffers, PSVariables))
				return false;

			ComPtr<ID3DBlob> gs;
			if (geometryShaderProgram && geometryShaderProgram[0])
			{
				if (gsCompileTarget < EGST_GS_4_0 || gsCompileTarget >= EGST_COUNT)
					gsCompileTarget = EGST_GS_5_0; // no pre-SM4 geometry shader, nothing to fall back to

				errors.Reset();
				hr = D3DCompile(geometryShaderProgram, strlen(geometryShaderProgram), "user_geometry_shader",
					nullptr, pInclude, geometryShaderEntryPointName && geometryShaderEntryPointName[0] ? geometryShaderEntryPointName : "main",
					GEOMETRY_SHADER_TYPE_NAMES[gsCompileTarget], compileFlags, 0, &gs, &errors);
				if (FAILED(hr))
				{
					if (errors)
						os::Printer::log("CD3D12MaterialRenderer: user geometry shader compilation: ",
							static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
					return false;
				}

				if (!reflectCBuffer(gs.Get(), GSBuffers, GSVariables))
					return false;
			}

			// HS/DS only exist together (see the HS/DS comment in CD3D12MaterialRenderer.h) -- only
			// one of the two supplied is treated as an invalid user shader rather than silently
			// ignored, so the caller isn't left believing tessellation works while a stage is
			// missing.
			ComPtr<ID3DBlob> hs, ds;
			bool hasHull = hullShaderProgram && hullShaderProgram[0];
			bool hasDomain = domainShaderProgram && domainShaderProgram[0];
			if (hasHull != hasDomain)
			{
				os::Printer::log("CD3D12MaterialRenderer::compileFromHLSL: hull and domain shader "
					"must be supplied together (D3D12 tessellation requires both stages)", ELL_ERROR);
				return false;
			}
			if (hasHull && hasDomain)
			{
				if (hsCompileTarget < EHST_HS_5_0 || hsCompileTarget >= EHST_COUNT)
					hsCompileTarget = EHST_HS_5_0;
				if (dsCompileTarget < EDST_DS_5_0 || dsCompileTarget >= EDST_COUNT)
					dsCompileTarget = EDST_DS_5_0;

				errors.Reset();
				hr = D3DCompile(hullShaderProgram, strlen(hullShaderProgram), "user_hull_shader",
					nullptr, pInclude, hullShaderEntryPointName && hullShaderEntryPointName[0] ? hullShaderEntryPointName : "main",
					HULL_SHADER_TYPE_NAMES[hsCompileTarget], compileFlags, 0, &hs, &errors);
				if (FAILED(hr))
				{
					if (errors)
						os::Printer::log("CD3D12MaterialRenderer: user hull shader compilation: ",
							static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
					return false;
				}

				errors.Reset();
				hr = D3DCompile(domainShaderProgram, strlen(domainShaderProgram), "user_domain_shader",
					nullptr, pInclude, domainShaderEntryPointName && domainShaderEntryPointName[0] ? domainShaderEntryPointName : "main",
					DOMAIN_SHADER_TYPE_NAMES[dsCompileTarget], compileFlags, 0, &ds, &errors);
				if (FAILED(hr))
				{
					if (errors)
						os::Printer::log("CD3D12MaterialRenderer: user domain shader compilation: ",
							static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
					return false;
				}

				if (!reflectCBuffer(hs.Get(), HSBuffers, HSVariables) ||
					!reflectCBuffer(ds.Get(), DSBuffers, DSVariables))
					return false;
			}

			VS = vs;
			PS = ps;
			GS = gs;
			HS = hs;
			DS = ds;

#ifdef _DEBUG
			DebugVSSource = vertexShaderProgram;
			DebugVSEntryPoint = vertexShaderEntryPointName && vertexShaderEntryPointName[0] ? vertexShaderEntryPointName : "main";
			if (hasPixel)
			{
				DebugPSSource = pixelShaderProgram;
				DebugPSEntryPoint = pixelShaderEntryPointName && pixelShaderEntryPointName[0] ? pixelShaderEntryPointName : "main";
			}
			if (gs)
			{
				DebugGSSource = geometryShaderProgram;
				DebugGSEntryPoint = geometryShaderEntryPointName && geometryShaderEntryPointName[0] ? geometryShaderEntryPointName : "main";
			}
			if (hs)
			{
				DebugHSSource = hullShaderProgram;
				DebugHSEntryPoint = hullShaderEntryPointName && hullShaderEntryPointName[0] ? hullShaderEntryPointName : "main";
			}
			if (ds)
			{
				DebugDSSource = domainShaderProgram;
				DebugDSEntryPoint = domainShaderEntryPointName && domainShaderEntryPointName[0] ? domainShaderEntryPointName : "main";
			}
#endif

			// Opaque by default: a user shader has no blend state "baked" of its own -- see
			// SMaterial::BlendOperation/BlendFactor to customize, applied generically by
			// CD3D12Driver::buildPSOKeyFromMaterial() regardless of MaterialType.
			BlendMode = SPSOKey::EBlendMode::None;
			return true;
		}

		bool CD3D12MaterialRenderer::compileComputeFromHLSL(const c8* computeShaderProgram,
			const c8* computeShaderEntryPointName, E_COMPUTE_SHADER_TYPE csCompileTarget,
			io::IFileSystem* fileSystem)
		{
			if (!computeShaderProgram || !computeShaderProgram[0])
			{
				os::Printer::log("CD3D12MaterialRenderer::compileComputeFromHLSL: compute shader "
					"is required", ELL_ERROR);
				return false;
			}
			if (csCompileTarget < ECST_CS_5_0 || csCompileTarget >= ECST_COUNT)
				csCompileTarget = ECST_CS_5_0; // D3D12 requires Shader Model 5.0 minimum, same reason as compileFromHLSL()

			CD3D12ShaderInclude includeHandler(fileSystem);
			ID3DInclude* pInclude = fileSystem ? &includeHandler : nullptr;

			UINT compileFlags = 0;
#ifdef _DEBUG
			compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
			// Same opt-in the D3D11 driver honours: a kernel whose branches must survive (per-body
			// selects, traversal loops) says so with this marker in its top-level source.
			if (strstr(computeShaderProgram, "PREFER_FLOW_CONTROL") != nullptr)
				compileFlags |= D3DCOMPILE_PREFER_FLOW_CONTROL;

			ComPtr<ID3DBlob> cs, errors;
			if (UseShaderModel6)
			{
				if (!compileStageShaderModel6(computeShaderProgram, computeShaderEntryPointName, L"cs_6_0", "compute",
					fileSystem, cs, CSBuffers, CSVariables))
					return false;
			}
			else
			{
				HRESULT hr = D3DCompile(computeShaderProgram, strlen(computeShaderProgram), "user_compute_shader",
					nullptr, pInclude, computeShaderEntryPointName && computeShaderEntryPointName[0] ? computeShaderEntryPointName : "main",
					COMPUTE_SHADER_TYPE_NAMES[csCompileTarget], compileFlags, 0, &cs, &errors);
				if (FAILED(hr))
				{
					if (errors)
						os::Printer::log("CD3D12MaterialRenderer: user compute shader compilation: ",
							static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
					return false;
				}

				if (!reflectCBuffer(cs.Get(), CSBuffers, CSVariables))
					return false;
			}

			// Unlike the graphics stages (VS/PS/GS/HS/DS), createComputeRootSignature() still only
			// reserves ONE user-cbuffer table, hardcoded to UserShaderRegisterSpace (space0) -- it
			// hasn't been extended to MaxUserShaderRegisterSpaces tables the way the graphics root
			// signature has (see the "COMPUTE root signature" paragraph in
			// MaxUserShaderRegisterSpaces' own comment). reflectCBuffer() alone can't catch this: it
			// accepts any space < MaxUserShaderRegisterSpaces since that check is shared with the
			// graphics stages. Enforce space0-only here specifically, so a compute shader using
			// space1..3 fails compilation with a clear message instead of reflecting "successfully"
			// and then silently getting a null CBV at dispatch time (allocateUserCBVTable() filters
			// by space; only space0 has a real table for compute).
			for (const SD3D12UserShaderCBuffer& buf : CSBuffers)
			{
				if (buf.Space != UserShaderRegisterSpace)
				{
					os::Printer::log("CD3D12MaterialRenderer::compileComputeFromHLSL: compute shader cbuffer "
						"must use space0 (UserShaderRegisterSpace) -- the compute root signature doesn't "
						"support other spaces yet, unlike the graphics stages: ", buf.Name.c_str(), ELL_ERROR);
					return false;
				}
			}

			CS = cs;

#ifdef _DEBUG
			DebugCSSource = computeShaderProgram;
			DebugCSEntryPoint = computeShaderEntryPointName && computeShaderEntryPointName[0] ? computeShaderEntryPointName : "main";
#endif

			return true;
		}

		std::vector<SD3D12UserShaderCBuffer>* CD3D12MaterialRenderer::getStageBuffers(E_D3D12_USER_CBV_STAGE stage)
		{
			// const_cast on the const overload rather than duplicating the switch -- the object is
			// non-const here by construction, so this hands back a legitimately mutable reference.
			return const_cast<std::vector<SD3D12UserShaderCBuffer>*>(
				static_cast<const CD3D12MaterialRenderer*>(this)->getStageBuffers(stage));
		}

		const std::vector<SD3D12UserShaderCBuffer>* CD3D12MaterialRenderer::getStageBuffers(E_D3D12_USER_CBV_STAGE stage) const
		{
			switch (stage)
			{
			case ED3D12UCS_VERTEX:   return &VSBuffers;
			case ED3D12UCS_PIXEL:    return &PSBuffers;
			case ED3D12UCS_GEOMETRY: return &GSBuffers;
			case ED3D12UCS_HULL:     return &HSBuffers;
			case ED3D12UCS_DOMAIN:   return &DSBuffers;
			default:                 return nullptr;
			}
		}

		s32 CD3D12MaterialRenderer::getConstantBufferID(const c8* name, E_SHADER_TYPE stage) const
		{
			const std::vector<SD3D12UserShaderCBuffer>* buffers = nullptr;
			if (stage == EST_VERTEX_SHADER)
				buffers = &VSBuffers;
			else if (stage == EST_PIXEL_SHADER)
				buffers = &PSBuffers;
			else if (stage == EST_GEOMETRY_SHADER)
				buffers = &GSBuffers;
			else if (stage == EST_HULL_SHADER)
				buffers = &HSBuffers;
			else if (stage == EST_DOMAIN_SHADER)
				buffers = &DSBuffers;
			else if (stage == EST_COMPUTE_SHADER)
				buffers = &CSBuffers;
			if (!buffers || !name)
				return -1;

			for (size_t i = 0; i < buffers->size(); ++i)
				if ((*buffers)[i].Name == name)
					return static_cast<s32>(i);
			return -1;
		}

		bool CD3D12MaterialRenderer::setConstantBuffer(s32 id, const void* data, size_t dataSizeBytes, E_SHADER_TYPE stage)
		{
			std::vector<SD3D12UserShaderCBuffer>* buffers = nullptr;
			if (stage == EST_VERTEX_SHADER)
				buffers = &VSBuffers;
			else if (stage == EST_PIXEL_SHADER)
				buffers = &PSBuffers;
			else if (stage == EST_GEOMETRY_SHADER)
				buffers = &GSBuffers;
			else if (stage == EST_HULL_SHADER)
				buffers = &HSBuffers;
			else if (stage == EST_DOMAIN_SHADER)
				buffers = &DSBuffers;
			else if (stage == EST_COMPUTE_SHADER)
				buffers = &CSBuffers;
			if (!buffers || !data || id < 0 || static_cast<size_t>(id) >= buffers->size())
				return false;

			SD3D12UserShaderCBuffer& buffer = (*buffers)[static_cast<size_t>(id)];
			const size_t bytes = std::min<size_t>(dataSizeBytes, buffer.Scratch.size());
			memcpy(buffer.Scratch.data(), data, bytes);
			return true;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
