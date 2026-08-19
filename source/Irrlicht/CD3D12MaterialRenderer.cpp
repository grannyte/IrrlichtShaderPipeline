// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CD3D12MaterialRenderer.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include <d3d12shader.h> // D3DReflect(), see reflectCBuffer()
#include <cstring>
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

			ComPtr<ID3D12ShaderReflection> reflector;
			HRESULT hr = D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
				IID_PPV_ARGS(&reflector));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12MaterialRenderer: D3DReflect failed on a user shader", ELL_ERROR);
				return false;
			}

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

			ComPtr<ID3DBlob> cs, errors;
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
