// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// A registered material -- a built-in E_MATERIAL_TYPE or a user shader from
// addHighLevelShaderMaterial() -- same role as CD3D11MaterialRenderer on the D3D11 side.
//
// Each material carries its OWN root signature (RootSignature/UserCBVTables below), built from
// reflection by CD3D12Driver::buildMaterialRootSignature() and shared between materials of an
// identical layout: the 7 fixed driver parameters plus one CBV table per (stage, register space)
// pair the shader actually declares a cbuffer at.
//
// OnSetMaterial()/OnRender() keep their empty inherited body -- D3D12 needs all state up front at
// PSO-creation time, so CD3D12Driver reads the fields below directly instead. isTransparent() IS
// wired up: generic scene code calls it to sort solid vs. transparent nodes.

#ifndef __C_D3D12_MATERIAL_RENDERER_H_INCLUDED__
#define __C_D3D12_MATERIAL_RENDERER_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>

#include "IVertexDescriptor.h" // IMaterialRenderer::OnRender() needs this, not included by IMaterialRenderer.h itself
#include "IMaterialRenderer.h"
#include "IShaderConstantSetCallBack.h"
#include "EShaderTypes.h"
#include "CD3D12PSOCache.h" // SPSOKey::EBlendMode

namespace irr
{
	namespace io
	{
		class IFileSystem;
	}

	namespace video
	{
		using Microsoft::WRL::ComPtr;

		//! Default register space for a user shader's cbuffers: engine shaders are written for
		//! D3D11 and declare "register(b0)", which is space0.
		static const UINT UserShaderRegisterSpace = 0;

		//! Highest user cbuffer register space (exclusive) a material root signature can carry a
		//! table for. Bounded by the width of the getOrCreateRootSignature() layout key, which packs
		//! ED3D12UCS_COUNT*MaxUserShaderRegisterSpaces table bits plus ED3D12UCS_COUNT stage bits
		//! into a u32. The compute root signature is not covered -- it hardcodes space0.
		static const UINT MaxUserShaderRegisterSpaces = 4;

		//! Register space of the driver's own cbuffers (b0..b4). Must be >=
		//! MaxUserShaderRegisterSpaces: a value inside that range overlaps two descriptor ranges at
		//! the same (register, space), failing root signature serialization for every shader.
		static const UINT DriverConstantRegisterSpace = MaxUserShaderRegisterSpaces;

		//! CBV registers (b0..b7) per user CBV descriptor table -- the per-(stage, space) width,
		//! not a per-material total. Shared with CD3D12Driver.h, which includes this.
		static const UINT MaxUserShaderCBVSlotsPerStage = 8;

		//! A reflected constant buffer variable, the same Name->{buffer, offset, size} table
		//! CD3D11MaterialRenderer::createResources() builds.
		struct SD3D12UserShaderVariable
		{
			core::stringc Name;
			s32 Buffer = 0; //!< Index into VSBuffers/PSBuffers, not the HLSL register.
			UINT Offset = 0;
			UINT Size = 0;
			//! True for a float4x4 reflected as D3D_SVC_MATRIX_COLUMNS (HLSL's default column-major
			//! packing). core::matrix4 is row-major, so these must be transposed on the way in or
			//! every matrix arrives transposed and the geometry is destroyed.
			bool TransposeOnSet = false;
		};

		//! A reflected cbuffer. BindPoint/Space decide which descriptor table covers it. Scratch is
		//! the CPU-side mirror, copied into the frame's constant ring on every draw.
		struct SD3D12UserShaderCBuffer
		{
			core::stringc Name;
			UINT BindPoint = 0;
			UINT Space = 0;
			std::vector<u8> Scratch;
		};

		//! The 5 programmable graphics stages, in the order buildMaterialRootSignature() assigns
		//! root parameter indices -- so materials declaring cbuffers at the same (stage, space)
		//! pairs get an identical layout and can share one root signature.
		enum E_D3D12_USER_CBV_STAGE
		{
			ED3D12UCS_VERTEX = 0,
			ED3D12UCS_PIXEL,
			ED3D12UCS_GEOMETRY,
			ED3D12UCS_HULL,
			ED3D12UCS_DOMAIN,
			ED3D12UCS_COUNT
		};

		//! One CBV b0..b7 table in a material's root signature. bindDrawState() issues one
		//! SetGraphicsRootDescriptorTable() per entry, so a built-in material issues none.
		struct SD3D12UserCBVTable
		{
			UINT RootSlot = 0; //!< Root parameter index, >= CD3D12Driver::FirstUserCBVRootSlot.
			E_D3D12_USER_CBV_STAGE Stage = ED3D12UCS_VERTEX;
			UINT Space = 0;
		};

		class CD3D12MaterialRenderer : public IMaterialRenderer
		{
		public:
			ComPtr<ID3DBlob> VS;
			ComPtr<ID3DBlob> PS;
			//! EVT_2TCOORDS variant, non-null only for the multi-texture built-in materials.
			ComPtr<ID3DBlob> VS2TCoords;
			ComPtr<ID3DBlob> PS2TCoords;
			ComPtr<ID3DBlob> GS;
			//! HS/DS only exist together; HS present means this material draws with PATCH topology.
			ComPtr<ID3DBlob> HS;
			ComPtr<ID3DBlob> DS;
			ComPtr<ID3DBlob> CS;
			std::vector<SD3D12UserShaderCBuffer> VSBuffers;
			std::vector<SD3D12UserShaderCBuffer> PSBuffers;
			std::vector<SD3D12UserShaderCBuffer> GSBuffers;
			std::vector<SD3D12UserShaderCBuffer> HSBuffers;
			std::vector<SD3D12UserShaderCBuffer> DSBuffers;
			std::vector<SD3D12UserShaderCBuffer> CSBuffers;
			std::vector<SD3D12UserShaderVariable> VSVariables;
			std::vector<SD3D12UserShaderVariable> PSVariables;
			std::vector<SD3D12UserShaderVariable> GSVariables;
			std::vector<SD3D12UserShaderVariable> HSVariables;
			std::vector<SD3D12UserShaderVariable> DSVariables;
			std::vector<SD3D12UserShaderVariable> CSVariables;
			//! Non-null means this GS writes stream-output rather than rasterizing. D3D12 has no
			//! equivalent of CreateGeometryShaderWithStreamOutput -- the same blob serves both, only
			//! the PSO differs. grab()/drop()'d.
			IVertexDescriptor* StreamOutputVertexType = nullptr;
			IShaderConstantSetCallBack* CallBack = nullptr;
			s32 UserData = 0;
			core::stringc Name;

#ifdef _DEBUG
			//! Diagnostic only: raw HLSL per stage, so a renderer's identity is readable in the
			//! debugger (an ID3DBlob* says nothing without decoding bytecode).
			core::stringc DebugVSSource, DebugVSEntryPoint;
			core::stringc DebugPSSource, DebugPSEntryPoint;
			core::stringc DebugGSSource, DebugGSEntryPoint;
			core::stringc DebugHSSource, DebugHSEntryPoint;
			core::stringc DebugDSSource, DebugDSEntryPoint;
			core::stringc DebugCSSource, DebugCSEntryPoint;
#endif

			//! Blend state baked at registration and read by buildPSOKeyFromMaterial() on every
			//! draw, except for EMT_ONETEXTURE_BLEND and the generic BlendOperation/BlendFactor
			//! override, both per-instance and decoded on the fly there.
			SPSOKey::EBlendMode BlendMode = SPSOKey::EBlendMode::None;
			D3D12_BLEND CustomSrcBlend = D3D12_BLEND_ONE;
			D3D12_BLEND CustomDestBlend = D3D12_BLEND_ZERO;
			D3D12_BLEND CustomSrcBlendAlpha = D3D12_BLEND_ONE;
			D3D12_BLEND CustomDestBlendAlpha = D3D12_BLEND_ZERO;
			D3D12_BLEND_OP CustomBlendOp = D3D12_BLEND_OP_ADD;

			//! Base E_MATERIAL_TYPE of a user shader: the blend fields above are copied from it at
			//! registration, as CD3D11MaterialRenderer delegates to its BaseRenderer. Also read so
			//! a shader based on EMT_ONETEXTURE_BLEND still decodes its per-instance factors.
			E_MATERIAL_TYPE BaseMaterialType = EMT_SOLID;

			//! Built by buildMaterialRootSignature() right after compilation, shared with every
			//! material of the same layout. A "foreign" renderer falls back to
			//! CD3D12Driver::RootSignature; a compute material uses ComputeRootSignature instead.
			ComPtr<ID3D12RootSignature> RootSignature;

			//! The (stage, space) CBV tables RootSignature declares, in root-parameter order --
			//! the per-draw bind plan. Empty for built-in types.
			std::vector<SD3D12UserCBVTable> UserCBVTables;

			//! Root parameter index of the pixel-stage UAV table (u0..u7) every graphics root
			//! signature ends with: the slot after the last user CBV table.
			UINT PixelUAVRootSlot = 0;

			//! Compile the user stages as Shader Model 6.0 through DXC (dxcompiler.dll + dxil.dll
			//! beside the executable) instead of FXC / SM 5.x. Set by the driver from
			//! SIrrlichtCreationParameters::PreferShaderModel6 once isShaderModel6Available() agreed.
			//! The requested SM 4/5 targets are ignored then; built-ins stay on FXC. The source sees
			//! IRR_SM6=1 and is parsed as HLSL 2018, the closest to what FXC accepts.
			bool UseShaderModel6 = false;
			//! Whether dxcompiler.dll and dxil.dll load (probed once per process, both kept loaded).
			static bool isShaderModel6Available();

			//! The reflected cbuffers of one stage, so callers can walk all 5 in a loop.
			std::vector<SD3D12UserShaderCBuffer>* getStageBuffers(E_D3D12_USER_CBV_STAGE stage);
			const std::vector<SD3D12UserShaderCBuffer>* getStageBuffers(E_D3D12_USER_CBV_STAGE stage) const;

			CD3D12MaterialRenderer() = default;
			virtual ~CD3D12MaterialRenderer();

			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return BlendMode != SPSOKey::EBlendMode::None;
			}

			//! Renderer for a built-in material: compiles the embedded default HLSL at Shader Model
			//! 5.0 with no reflection (built-ins have no user cbuffer). The 2TCoords entry points,
			//! if both given, additionally compile VS2TCoords/PS2TCoords.
			bool compileBuiltIn(const c8* hlslSource, const c8* vertexShaderEntryPoint, const c8* pixelShaderEntryPoint,
				const c8* vertexShaderEntryPoint2TCoords = nullptr, const c8* pixelShaderEntryPointUV2 = nullptr);

			//! Interface parity with CD3D11MaterialRenderer: returns VS, or GS if there is no VS.
			//! D3D12 does not need it internally -- the PSO embeds the input layout.
			void* getShaderByteCode() const;
			u32 getShaderByteCodeSize() const;

			//! Renderer for a user shader: compiles VS+PS (plus an optional GS, and the HS+DS pair
			//! if both are supplied -- one without the other is rejected), then reflects each to
			//! build the per-stage buffers and variables. Returns false (already logged) on
			//! failure, leaving the object to be drop()'d rather than registered. Targets below
			//! Shader Model 5.0 are clamped up. fileSystem resolves "#include"s under
			//! "media/shaders/<name>", the same convention as the D3D11 side.
			bool compileFromHLSL(
				const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
				const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
				const c8* geometryShaderProgram = nullptr, const c8* geometryShaderEntryPointName = nullptr,
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_COUNT,
				const c8* hullShaderProgram = nullptr, const c8* hullShaderEntryPointName = nullptr,
				E_HULL_SHADER_TYPE hsCompileTarget = EHST_COUNT,
				const c8* domainShaderProgram = nullptr, const c8* domainShaderEntryPointName = nullptr,
				E_DOMAIN_SHADER_TYPE dsCompileTarget = EDST_COUNT,
				io::IFileSystem* fileSystem = nullptr);

			//! Compute renderer (addComputeShader()): compiles and reflects csProgram alone. Kept
			//! separate from compileFromHLSL(), which requires a VS+PS a compute material lacks.
			bool compileComputeFromHLSL(const c8* computeShaderProgram,
				const c8* computeShaderEntryPointName, E_COMPUTE_SHADER_TYPE csCompileTarget,
				io::IFileSystem* fileSystem = nullptr);

			//! Mirrors CD3D11MaterialRenderer::getConstantBufferID().
			s32 getConstantBufferID(const c8* name, E_SHADER_TYPE stage) const;

			//! Overwrites the whole buffer `id` of `stage`, truncated to its size -- the "raw
			//! struct" shortcut alongside writing variable by variable.
			bool setConstantBuffer(s32 id, const void* data, size_t dataSizeBytes, E_SHADER_TYPE stage);

		private:
			//! Reflects a compiled blob into outBuffers/outVariables. A cbuffer whose register
			//! overflows the table, or a duplicate at an already-used register, is skipped with a
			//! warning rather than failing the whole compilation.
			static bool reflectCBuffer(ID3DBlob* code, std::vector<SD3D12UserShaderCBuffer>& outBuffers,
				std::vector<SD3D12UserShaderVariable>& outVariables);
			//! The walk itself, over a reflector from D3DReflect (FXC) or IDxcUtils::CreateReflection (DXC).
			static bool reflectFromReflector(ID3D12ShaderReflection* reflector, std::vector<SD3D12UserShaderCBuffer>& outBuffers,
				std::vector<SD3D12UserShaderVariable>& outVariables);

			//! One stage through DXC: compiles `source` at `profile` (e.g. L"vs_6_0"), reflects its
			//! cbuffers, hands the DXIL back as an ID3DBlob. False (logged, with stageName) on failure.
			static bool compileStageShaderModel6(const c8* source, const c8* entryPoint, const wchar_t* profile,
				const c8* stageName, io::IFileSystem* fileSystem, ComPtr<ID3DBlob>& outCode,
				std::vector<SD3D12UserShaderCBuffer>& outBuffers, std::vector<SD3D12UserShaderVariable>& outVariables);
		};

		//! An entry in the CD3D12Driver::MaterialRenderers registry, indexed by
		//! SMaterial::MaterialType. Native is the same pointer re-cast if and only if this driver
		//! built it, which avoids a per-draw dynamic_cast.
		struct SD3D12MaterialRendererEntry
		{
			IMaterialRenderer* Renderer = nullptr;
			CD3D12MaterialRenderer* Native = nullptr;
			core::stringc Name;
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
#endif
