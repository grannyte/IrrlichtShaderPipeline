// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// A registered material (see CD3D12Driver::MaterialRenderers) -- whether one of the ~24 built-in
// types (CD3D12Driver::createBuiltInMaterialRenderers(), one per E_MATERIAL_TYPE value) or a user
// shader (IGPUProgrammingServices::addHighLevelShaderMaterial()) -- is a CD3D12MaterialRenderer,
// same role as CD3D11MaterialRenderer on the D3D11 side (CD3D11Driver::createMaterialRenderers()
// for built-ins, addHighLevelShaderMaterial() for user shaders, same class for both, its own
// dedicated file rather than inline in CD3D12Driver.h/.cpp).
//
// Shader content limitation: shares the driver's generic root signature instead of generating one
// per shader, so at most UserShaderCBVTableSlotCount (8) cbuffers per stage (VS/PS), each required
// to be declared at register "cbuffer Foo : register(bN)  // = space0" for N in 0..7 (see
// CD3D12Driver::UserShaderConstantSlotVS/PS, descriptor tables rather than single root CBVs -- one
// cbuffer per reflected register, not a single fixed cbuffer), and the same t0/s0 as other
// materials for any sampled texture. VSBuffers/PSBuffers/VSVariables/PSVariables stay empty for a
// built-in type (no reflected cbuffer needed, see setupBuiltIn()).
//
// This driver does NOT drive drawing through OnSetMaterial()/OnRender() (unlike
// CD3D11MaterialRenderer): D3D12's PSO model requires all state (shaders, blend, depth,
// rasterizer...) up front at PSO-creation time, which doesn't fit an incremental state-machine
// style of driving via virtual calls like D3D11. These two methods therefore keep their default
// (empty, inherited from IMaterialRenderer) body; CD3D12Driver::buildPSOKeyFromMaterial()/
// choosePixelShaderForMaterial() read the fields below directly (BlendMode and friends) to build
// the PSO, and CD3D12Driver::bindDrawState() calls CallBack->OnSetConstants() at the right time.
// isTransparent(), on the other hand, IS actually wired up, since it's the only IMaterialRenderer
// method that generic engine code calls independently of this driver's own draw pipeline (see
// CMeshSceneNode.cpp/CAnimatedMeshSceneNode.cpp/COctreeSceneNode.cpp/CSceneManager.cpp, which all
// call driver->getMaterialRenderer(material.MaterialType)->isTransparent() to sort solid vs.
// transparent scene nodes).

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

		//! Register space of a USER SHADER's cbuffers: space0.
		//!
		//! Forced by existing content, not a free choice: the engine's shaders are written for
		//! D3D11 and declare "cbuffer X : register(b0)", which is space0 (D3D11 has no register
		//! space). In D3D12 the root signature is a contract validated at PSO-creation time: if a
		//! shader reads b0/space0 and the root signature doesn't expose b0/space0 at that stage,
		//! CreateGraphicsPipelineState fails and nothing draws. The driver therefore keeps its OWN
		//! constants elsewhere (DriverConstantRegisterSpace below) and leaves space0 to user shaders.
		static const UINT UserShaderRegisterSpace = 0;

		//! Register space of the driver's INTERNAL cbuffers (World/ViewProj/ClipPlanes/Lighting/
		//! Fog, b0..b4) -- see CD3D12DefaultShaders.h, which declares them as "register(bN, space2)".
		//! Must stay distinct from UserShaderRegisterSpace.
		static const UINT DriverConstantRegisterSpace = 2;

		//! Number of CBV registers (b0..b7, in UserShaderRegisterSpace) the driver's root signature
		//! reserves per stage for a user shader's cbuffers (see
		//! CD3D12Driver::createRootSignature()/UserShaderConstantSlotVS/PS and
		//! CD3D12MaterialRenderer::reflectCBuffer()) -- single source of truth shared between the
		//! two files (CD3D12Driver.h includes this one).
		static const UINT MaxUserShaderCBVSlotsPerStage = 8;

		//! A reflected (D3DReflect) constant buffer variable for a user shader --
		//! Name->{buffer, offset, size}, the same table CD3D11MaterialRenderer::createResources()
		//! builds, carried here without the full IMaterialRenderer hierarchy (a flat record is
		//! enough, D3D12 doesn't need polymorphism per material type).
		struct SD3D12UserShaderVariable
		{
			core::stringc Name;
			s32 Buffer = 0; //!< Index into VSBuffers/PSBuffers (see reflectCBuffer()), not the HLSL register.
			UINT Offset = 0;
			UINT Size = 0;
			//! True for a float4x4 whose reflection reports D3D_SVC_MATRIX_COLUMNS -- i.e.
			//! column-major packing, HLSL's DEFAULT. core::matrix4 is row-major: written as-is into
			//! the cbuffer, the shader would read it transposed.
			//!
			//! CD3D11MaterialRenderer::setVariable() therefore always transposes such variables
			//! before copying them, and every engine shader is written assuming that. This driver
			//! used to memcpy raw: every matrix arrived transposed in user shaders, producing
			//! completely deformed geometry (garbage w -> points at infinity) anywhere a shader
			//! received a matrix.
			bool TransposeOnSet = false;
		};

		//! A reflected cbuffer. BindPoint is the HLSL register bN (0..7, space0 -- see
		//! UserShaderRegisterSpace and CD3D12Driver::UserShaderConstantSlotVS/PS): CPU-side mirror
		//! (Scratch) filled by CD3D12Driver::setVertexShaderConstant()/setPixelShaderConstant() and
		//! copied as-is into the frame's constant ring (CD3D12Driver::allocateConstant()) on every
		//! draw using this shader -- no D3D11-style Map/Unmap, the upload-heap ring already plays
		//! that role (see SD3D12FrameContext::ConstantRing).
		struct SD3D12UserShaderCBuffer
		{
			core::stringc Name;
			UINT BindPoint = 0;
			std::vector<u8> Scratch;
		};

		class CD3D12MaterialRenderer : public IMaterialRenderer
		{
		public:
			ComPtr<ID3DBlob> VS;
			ComPtr<ID3DBlob> PS;
			//! EVT_2TCOORDS variant (VSMain2TCoords/PSMainXxxUV2, CD3D12DefaultShaders.h) --
			//! non-null only for the multi-texture built-in materials (see
			//! CD3D12Driver::createBuiltInMaterialRenderers()), null for everything else
			//! (single-texture types, normal/parallax map, user shaders). Chosen by
			//! CD3D12Driver::chooseVertexShaderForMaterial()/choosePixelShaderForMaterial() in place
			//! of VS/PS when the mesh being drawn uses the "2tcoords" descriptor (see their
			//! comments), otherwise VS/PS are used as-is.
			ComPtr<ID3DBlob> VS2TCoords;
			ComPtr<ID3DBlob> PS2TCoords;
			//! Optional -- null for a built-in material and for most user shaders (see
			//! compileFromHLSL()).
			ComPtr<ID3DBlob> GS;
			//! HS/DS only exist together -- compileFromHLSL() only accepts/compiles one if the
			//! other is also supplied (D3D12 tessellation requires both stages, not just HS or DS
			//! alone). HS present <=> this material draws with
			//! D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH (see CD3D12Driver::buildPSOKeyFromMaterial()/
			//! drawMeshBuffer()).
			ComPtr<ID3DBlob> HS;
			ComPtr<ID3DBlob> DS;
			//! Compute renderer (see CD3D12MaterialRenderer::compileComputeFromHLSL(),
			//! CD3D12Driver::addComputeShader()) -- mutually exclusive with VS/PS/GS/HS/DS in
			//! practice (a material registered via addComputeShader() never has a VS/PS), though
			//! nothing structurally prevents it since the same CD3D12MaterialRenderer class is used
			//! for every case (see the file header comment).
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
			//! Non-null <=> this GS writes to a stream-output buffer rather than rasterizing
			//! (D3D12 has no equivalent of D3D11's CreateGeometryShaderWithStreamOutput -- the same
			//! GS blob serves both cases, it's the PSO that differs: see
			//! CD3D12Driver::getPSOForMaterial()/buildStreamOutputDeclaration(), which adds a
			//! D3D12_STREAM_OUTPUT_DESC + RasterizedStream=D3D12_SO_NO_RASTERIZED_STREAM to the PSO
			//! when this field is set, mirroring D3D11_SO_NO_RASTERIZED_STREAM on the
			//! CD3D11MaterialRenderer side). Pointer is grab()/drop()'d (see
			//! registerUserShaderMaterial()/~CD3D12MaterialRenderer()), set from the vertexTypeOut
			//! passed to addHighLevelShaderMaterial(). The actual GPU target of the stream-output is
			//! bound separately, at draw time, via CD3D12Driver::setStreamOutputBuffer() -- this
			//! field only tells the PSO "this shader writes stream-output" and describes the
			//! expected format.
			IVertexDescriptor* StreamOutputVertexType = nullptr;
			IShaderConstantSetCallBack* CallBack = nullptr;
			s32 UserData = 0;
			core::stringc Name;

#ifdef _DEBUG
			//! Diagnostic only: raw HLSL source + entry point for each compiled stage, kept so a
			//! renderer's identity can be inspected directly in the debugger (Locals/Watch) --
			//! unlike an ID3DBlob*, whose binary content says nothing at a glance without decoding
			//! bytecode. Absent in Release (no memory cost for keeping full HLSL sources around).
			//! Filled by compileFromHLSL()/compileBuiltIn()/compileComputeFromHLSL().
			core::stringc DebugVSSource, DebugVSEntryPoint;
			core::stringc DebugPSSource, DebugPSEntryPoint;
			core::stringc DebugGSSource, DebugGSEntryPoint;
			core::stringc DebugHSSource, DebugHSEntryPoint;
			core::stringc DebugDSSource, DebugDSEntryPoint;
			core::stringc DebugCSSource, DebugCSEntryPoint;
#endif

			//! Blend state baked once at registration (see SPSOKey::EBlendMode) and read by
			//! CD3D12Driver::buildPSOKeyFromMaterial() on every draw -- EXCEPT for
			//! EMT_ONETEXTURE_BLEND, whose factors vary per instance (SMaterial::MaterialTypeParam)
			//! and are therefore decoded on the fly instead (see buildPSOKeyFromMaterial()), and
			//! for the generic BlendOperation/BlendFactor override, also per-instance and applied
			//! on top regardless of what this renderer registered here.
			SPSOKey::EBlendMode BlendMode = SPSOKey::EBlendMode::None;
			D3D12_BLEND CustomSrcBlend = D3D12_BLEND_ONE;
			D3D12_BLEND CustomDestBlend = D3D12_BLEND_ZERO;
			D3D12_BLEND CustomSrcBlendAlpha = D3D12_BLEND_ONE;
			D3D12_BLEND CustomDestBlendAlpha = D3D12_BLEND_ZERO;
			D3D12_BLEND_OP CustomBlendOp = D3D12_BLEND_OP_ADD;

			//! Base E_MATERIAL_TYPE of a user shader (the baseMaterial parameter of
			//! addHighLevelShaderMaterial()): the blend fields above are copied from it at
			//! registration (see CD3D12Driver::registerUserShaderMaterial()), same as
			//! CD3D11MaterialRenderer delegating its blend state to its BaseRenderer. Stays
			//! EMT_SOLID for the ~24 built-in materials, which carry their own blend state.
			//! Also read by buildPSOKeyFromMaterial() so a user shader based on
			//! EMT_ONETEXTURE_BLEND correctly decodes its per-instance factors from
			//! SMaterial::MaterialTypeParam, same as the built-in material itself would.
			E_MATERIAL_TYPE BaseMaterialType = EMT_SOLID;

			CD3D12MaterialRenderer() = default;
			virtual ~CD3D12MaterialRenderer();

			//! See the file header comment: this is the field the generic solid/transparent scene
			//! node sort reads via driver->getMaterialRenderer(type)->isTransparent().
			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return BlendMode != SPSOKey::EBlendMode::None;
			}

			//! Renderer for one of the ~24 built-in materials: compiles hlslSource (the embedded
			//! default HLSL, D3D12DefaultShaderHLSL) at a fixed Shader Model 5.0, with no
			//! reflection (no user cbuffer for a built-in type -- b0/b1/b2 are handled separately by
			//! CD3D12Driver::bindTransformsAndTexture(), not by the generic VSBuffer/PSBuffer
			//! mechanism above). Used by CD3D12Driver::createBuiltInMaterialRenderers(), one
			//! renderer per E_MATERIAL_TYPE (compiled separately even when several types share the
			//! same entry points -- negligible compile cost, done once at init).
			//! vertexShaderEntryPoint2TCoords/pixelShaderEntryPointUV2, if both non-null, additionally
			//! compile VS2TCoords/PS2TCoords (same SM 5.0 target, same hlslSource) -- used only for
			//! the multi-texture built-in materials (see CD3D12Driver::createBuiltInMaterialRenderers()).
			bool compileBuiltIn(const c8* hlslSource, const c8* vertexShaderEntryPoint, const c8* pixelShaderEntryPoint,
				const c8* vertexShaderEntryPoint2TCoords = nullptr, const c8* pixelShaderEntryPointUV2 = nullptr);

			//! Interface parity with CD3D11MaterialRenderer::getShaderByteCode()/
			//! getShaderByteCodeSize() (CD3D11MaterialRenderer.h) -- returns VS, or GS if VS is
			//! absent (same fallback rule as on the D3D11 side: "the VS, or the GS if there's no
			//! VS"), nullptr/0 if neither exists. D3D12 does NOT need this internally: the PSO
			//! embeds the input layout directly (D3D12_GRAPHICS_PIPELINE_STATE_DESC::InputLayout),
			//! no separate CreateInputLayout() call fed with a bytecode signature like on the D3D11
			//! side (CD3D11CallBridge::setInputLayout()) -- exposed only so a generic caller
			//! querying an IMaterialRenderer-like object doesn't hit an API gap.
			void* getShaderByteCode() const;
			u32 getShaderByteCodeSize() const;

			//! Renderer for a user shader (addHighLevelShaderMaterial): compiles VS+PS (and, if
			//! geometryShaderProgram is non-null, a classic rasterizing GS -- not a "baked"
			//! stream-output one, see GS above; and, if hullShaderProgram AND domainShaderProgram
			//! are both non-null, the HS+DS tessellation pair -- see HS/DS above, one without the
			//! other is rejected) from their HLSL source via D3DCompile, then reflects each via
			//! D3DReflect (reflectCBuffer() below) to build VSBuffers/PSBuffers/GSBuffers/
			//! HSBuffers/DSBuffers and their variables. Returns false (already logged) if
			//! compilation or reflection fails -- the object is then unusable and should be
			//! drop()'d by the caller rather than registered. Clamps vsCompileTarget/psCompileTarget
			//! up to Shader Model 5.0 minimum (root signature 1.1) if needed, same fallback +
			//! D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY as CD3D11MaterialRenderer::init() for a
			//! target < 4.0 (gsCompileTarget/hsCompileTarget/dsCompileTarget are clamped to
			//! gs_5_0/hs_5_0/ds_5_0 if out of range rather than falling back -- there's no
			//! pre-SM4/SM5 geometry/hull/domain shader). fileSystem (may be null) resolves the
			//! HLSL source's "#include"s via a dedicated ID3DInclude (see
			//! reflectCBuffer.cpp/CD3D12ShaderInclude) -- same path convention
			//! ("media/shaders/<name>") as CD3D11MaterialRenderer::CShaderInclude, so the same HLSL
			//! content compiles on both drivers unmodified.
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

			//! Compute renderer (CD3D12Driver::addComputeShader()): compiles+reflects csProgram
			//! alone, filling CS/CSBuffers/CSVariables. Kept separate from compileFromHLSL() rather
			//! than a new set of optional parameters on it: a compute material never has a VS/PS
			//! (mandatory in compileFromHLSL()) -- the two compilation paths share nothing beyond
			//! reflectCBuffer(). fileSystem: see compileFromHLSL().
			bool compileComputeFromHLSL(const c8* computeShaderProgram,
				const c8* computeShaderEntryPointName, E_COMPUTE_SHADER_TYPE csCompileTarget,
				io::IFileSystem* fileSystem = nullptr);

			//! Looks up a buffer by name in VSBuffers/PSBuffers/GSBuffers/HSBuffers/DSBuffers/
			//! CSBuffers (per stage). Returns an id to pass to setConstantBuffer(); mirrors
			//! CD3D11MaterialRenderer::getConstantBufferID().
			s32 getConstantBufferID(const c8* name, E_SHADER_TYPE stage) const;

			//! Overwrites the whole buffer id (VSBuffers[id]/PSBuffers[id]/GSBuffers[id]/
			//! HSBuffers[id]/DSBuffers[id]/CSBuffers[id] depending on stage) with data, truncated
			//! to the buffer's size if needed (same care as
			//! CD3D12Driver::setVertexShaderConstant()) -- the "raw struct" shortcut that
			//! CD3D11MaterialRenderer::setConstantBuffer() offers in addition to writing
			//! variable-by-variable. Returns false if id/stage is invalid.
			bool setConstantBuffer(s32 id, const void* data, size_t dataSizeBytes, E_SHADER_TYPE stage);

		private:
			//! Reflects a compiled blob (D3DReflect) and fills outBuffers (one
			//! SD3D12UserShaderCBuffer per cbuffer found, up to
			//! CD3D12Driver::UserShaderCBVTableSlotCount) plus the list of SD3D12UserShaderVariable
			//! referencing them (SD3D12UserShaderVariable::Buffer = index into outBuffers). A
			//! cbuffer whose HLSL register (bindDesc.BindPoint) overflows the table
			//! (>= UserShaderCBVTableSlotCount) is skipped (warning logged) rather than failing the
			//! whole compilation; two cbuffers reflected at the same register are likewise skipped
			//! (the first one is kept).
			static bool reflectCBuffer(ID3DBlob* code, std::vector<SD3D12UserShaderCBuffer>& outBuffers,
				std::vector<SD3D12UserShaderVariable>& outVariables);
		};

		//! An entry in the CD3D12Driver::MaterialRenderers registry, indexed by
		//! SMaterial::MaterialType (same convention as CNullDriver::MaterialRenderers): Renderer is
		//! the generic handle (IVideoDriver::getMaterialRenderer() returns it as-is, including for
		//! a "foreign" IMaterialRenderer registered via addMaterialRenderer() by external engine
		//! code), Native is the same pointer re-cast to CD3D12MaterialRenderer if and only if this
		//! driver built it (nullptr otherwise) -- avoids a dynamic_cast on every draw in
		//! buildPSOKeyFromMaterial()/choosePixelShaderForMaterial(), which have nothing to read on a
		//! foreign renderer anyway (no PSO/blob to pull from it).
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
