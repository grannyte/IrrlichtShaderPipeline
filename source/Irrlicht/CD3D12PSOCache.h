// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Cache of Pipeline State Objects, keyed on everything that determines a PSO in D3D12
// (shader bytecode, blend, depth/stencil, rasterizer, input layout, render target formats).
// Independent of actual shader content - a generic cache, not tied to any particular
// material system.
//
// Uses a single shared ID3D12RootSignature (created by CD3D12Driver::createRootSignature())
// for every PSO in this cache, instead of a root signature per shader:
//   - CBV b0 (root descriptor): per-object world matrix.
//   - CBV b1 (root descriptor): per-frame view+projection matrices.
//   - Descriptor table (1 SRV t0): base texture, visible to the pixel shader.
//   - Descriptor table (1 sampler s0): filter/address mode per SMaterialLayer, visible to
//     the pixel shader.
//   - CBV b2 (root descriptor): user clip planes, visible to the pixel shader
//     (setClipPlane()/enableClipPlane()).

#ifndef __C_D3D12_PSO_CACHE_H_INCLUDED__
#define __C_D3D12_PSO_CACHE_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace irr
{
	namespace video
	{
		using Microsoft::WRL::ComPtr;

		//! Everything that determines an ID3D12PipelineState, aside from the shared root
		//! signature. Two draws with an equal SPSOKey can reuse the same PSO.
		struct SPSOKey
		{
			size_t VSHash = 0;
			size_t PSHash = 0;
			//! 0 if no geometry shader. StreamOutputHash is independent: 0 if this GS
			//! rasterizes normally, non-zero if it is a pure stream-output GS (see
			//! CD3D12Driver::getPSOForMaterial()/buildStreamOutputDeclaration()) - two
			//! materials sharing the same GS blob but differing in rasterize-vs-stream-output
			//! need distinct PSOs.
			size_t GSHash = 0;
			size_t StreamOutputHash = 0;
			//! 0 if no tessellation. HS/DS only ever exist together (see
			//! CD3D12MaterialRenderer::HS/DS), so these two fields are either both zero or
			//! both non-zero.
			size_t HSHash = 0;
			size_t DSHash = 0;
			//! Blend modes covered by the default shader (see
			//! CD3D12Driver::buildPSOKeyFromMaterial()):
			//!   None      : opaque, no blending (EMT_SOLID and any uncovered type).
			//!   AlphaBlend: src*srcAlpha + dst*(1-srcAlpha) (EMT_TRANSPARENT_ALPHA_CHANNEL,
			//!               EMT_TRANSPARENT_VERTEX_ALPHA - same blend state for both, only
			//!               the alpha source differs in the shader).
			//!   AddColor  : src*1 + dst*1, additive (EMT_TRANSPARENT_ADD_COLOR).
			//!   Custom    : arbitrary blend factors, see CustomSrcBlend/CustomDestBlend below
			//!               (EMT_ONETEXTURE_BLEND - factors decoded from
			//!               SMaterial::MaterialTypeParam via unpack_textureBlendFunc()).
			enum class EBlendMode { None, AlphaBlend, AddColor, Custom };
			EBlendMode BlendMode = EBlendMode::None;
			//! Only meaningful when BlendMode == Custom (EMT_ONETEXTURE_BLEND, same
			//! E_BLEND_FACTOR applied to RGB and alpha - pack_textureBlendFunc() does not
			//! distinguish the two).
			//! Separate RGB/alpha fields because D3D12_BLEND has "_COLOR" values
			//! (DEST_COLOR, SRC_COLOR...) that D3D12 validation rejects on
			//! SrcBlendAlpha/DestBlendAlpha - CustomSrcBlend/CustomDestBlend map to
			//! RenderTarget[0].SrcBlend/DestBlend (RGB), CustomSrcBlendAlpha/
			//! CustomDestBlendAlpha (same logical factor, "_COLOR" swapped for "_ALPHA", see
			//! CD3D12Driver::getD3D12BlendFactor()) map to SrcBlendAlpha/DestBlendAlpha.
			D3D12_BLEND CustomSrcBlend = D3D12_BLEND_ONE;
			D3D12_BLEND CustomDestBlend = D3D12_BLEND_ZERO;
			D3D12_BLEND CustomSrcBlendAlpha = D3D12_BLEND_ONE;
			D3D12_BLEND CustomDestBlendAlpha = D3D12_BLEND_ZERO;
			//! Blend operation for BlendMode::Custom. Reflects SMaterial::BlendOperation when
			//! CD3D12Driver::buildPSOKeyFromMaterial() reads the generic
			//! BlendOperation/BlendFactor path.
			D3D12_BLEND_OP CustomBlendOp = D3D12_BLEND_OP_ADD;
			bool DepthTestEnable = true;
			bool DepthWriteEnable = true;
			//! Actual depth comparison for the material (SMaterial::ZBuffer, see
			//! CD3D12Driver::getD3D12DepthFunc()) - defaults to GREATER to match SMaterial's
			//! default (ECFN_GREATER) and OuterSpace's inverted depth convention (see
			//! CD3D12Driver::clearZBuffer()/CD3D11Driver::getDepthFunction()).
			D3D12_COMPARISON_FUNC DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
			D3D12_CULL_MODE CullMode = D3D12_CULL_MODE_BACK;
			D3D12_FILL_MODE FillMode = D3D12_FILL_MODE_SOLID;
			D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			//! Up to 8 simultaneous render targets (D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT).
			//! NumRenderTargets==1/RTVFormats[0]==R8G8B8A8_UNORM (rest UNKNOWN) is the common
			//! single-render-target case - callers with one target only need to set RTVFormats[0].
			UINT NumRenderTargets = 1;
			DXGI_FORMAT RTVFormats[8] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
				DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
			DXGI_FORMAT DSVFormat = DXGI_FORMAT_D32_FLOAT;
			//! Sample count of the currently bound render target(s); 1 = no MSAA. Must match
			//! SampleDesc.Count of every RTV/DSV actually bound by OMSetRenderTargets() at draw
			//! time. Read by CD3D12Driver::buildPSOKeyFromMaterial()/
			//! buildShadowVolumeStencilKey()/drawStencilShadow() from CurrentRTVSampleCount.
			UINT SampleCount = 1;
			size_t InputLayoutHash = 0;

			// --- Stencil shadow volumes ---
			// Defaults (StencilEnable=false, RenderTargetWriteMask=ALL) leave "normal material"
			// PSOs built by buildPSOKeyFromMaterial() unaffected. Only the auxiliary PSOs from
			// CD3D12Driver::getOrCreateAuxPSO() (drawing the shadow volume into the stencil
			// buffer, then filling the shadow) set these fields away from their defaults - see
			// the zpass shadow technique comment at the top of CD3D12Driver.cpp.
			bool StencilEnable = false;
			UINT8 StencilReadMask = 0xFF;
			UINT8 StencilWriteMask = 0xFF;
			D3D12_STENCIL_OP StencilFailOp = D3D12_STENCIL_OP_KEEP;
			D3D12_STENCIL_OP StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
			D3D12_STENCIL_OP StencilPassOp = D3D12_STENCIL_OP_KEEP;
			D3D12_COMPARISON_FUNC StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			//! Same value applied to FrontFace and BackFace: PSOs that need this already cull
			//! one of the two faces (CullMode), so the other half of D3D12_DEPTH_STENCIL_DESC
			//! is never exercised.
			UINT8 RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

			//! Depth offset (SMaterial::PolygonOffsetFactor/PolygonOffsetDirection), same
			//! mapping as CD3D11Driver::setBasicRenderStates(): DepthBias/SlopeScaledDepthBias
			//! signed according to EPO_FRONT/EPO_BACK. Defaults (0/0.0f) leave PSOs that don't
			//! set these fields unaffected.
			INT DepthBias = 0;
			FLOAT SlopeScaledDepthBias = 0.0f;

			bool operator==(const SPSOKey& other) const
			{
				return VSHash == other.VSHash && PSHash == other.PSHash &&
					GSHash == other.GSHash && StreamOutputHash == other.StreamOutputHash &&
					HSHash == other.HSHash && DSHash == other.DSHash &&
					BlendMode == other.BlendMode &&
					CustomSrcBlend == other.CustomSrcBlend &&
					CustomDestBlend == other.CustomDestBlend &&
					CustomSrcBlendAlpha == other.CustomSrcBlendAlpha &&
					CustomDestBlendAlpha == other.CustomDestBlendAlpha &&
					CustomBlendOp == other.CustomBlendOp &&
					DepthTestEnable == other.DepthTestEnable &&
					DepthWriteEnable == other.DepthWriteEnable &&
					DepthFunc == other.DepthFunc &&
					CullMode == other.CullMode && FillMode == other.FillMode &&
					TopologyType == other.TopologyType &&
					NumRenderTargets == other.NumRenderTargets &&
					std::equal(std::begin(RTVFormats), std::end(RTVFormats), std::begin(other.RTVFormats)) &&
					DSVFormat == other.DSVFormat &&
					SampleCount == other.SampleCount &&
					InputLayoutHash == other.InputLayoutHash &&
					StencilEnable == other.StencilEnable &&
					StencilReadMask == other.StencilReadMask &&
					StencilWriteMask == other.StencilWriteMask &&
					StencilFailOp == other.StencilFailOp &&
					StencilDepthFailOp == other.StencilDepthFailOp &&
					StencilPassOp == other.StencilPassOp &&
					StencilFunc == other.StencilFunc &&
					RenderTargetWriteMask == other.RenderTargetWriteMask &&
					DepthBias == other.DepthBias &&
					SlopeScaledDepthBias == other.SlopeScaledDepthBias;
			}

			//! Combines all fields into one hash, used as the cache key. Two different input
			//! layouts that collided on this hash would incorrectly share a PSO; no collision
			//! handling beyond that (see CD3D12PSOCache::getOrCreate()).
			size_t computeHash() const
			{
				size_t h = VSHash;
				auto combine = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
				combine(PSHash);
				combine(GSHash);
				combine(StreamOutputHash);
				combine(HSHash);
				combine(DSHash);
				combine(static_cast<size_t>(BlendMode));
				combine(static_cast<size_t>(CustomSrcBlend));
				combine(static_cast<size_t>(CustomDestBlend));
				combine(static_cast<size_t>(CustomSrcBlendAlpha));
				combine(static_cast<size_t>(CustomDestBlendAlpha));
				combine(static_cast<size_t>(CustomBlendOp));
				combine(static_cast<size_t>(DepthTestEnable));
				combine(static_cast<size_t>(DepthWriteEnable));
				combine(static_cast<size_t>(DepthFunc));
				combine(static_cast<size_t>(CullMode));
				combine(static_cast<size_t>(FillMode));
				combine(static_cast<size_t>(TopologyType));
				combine(static_cast<size_t>(NumRenderTargets));
				for (UINT i = 0; i < 8; ++i)
					combine(static_cast<size_t>(RTVFormats[i]));
				combine(static_cast<size_t>(DSVFormat));
				combine(static_cast<size_t>(SampleCount));
				combine(InputLayoutHash);
				combine(static_cast<size_t>(StencilEnable));
				combine(static_cast<size_t>(StencilReadMask));
				combine(static_cast<size_t>(StencilWriteMask));
				combine(static_cast<size_t>(StencilFailOp));
				combine(static_cast<size_t>(StencilDepthFailOp));
				combine(static_cast<size_t>(StencilPassOp));
				combine(static_cast<size_t>(StencilFunc));
				combine(static_cast<size_t>(RenderTargetWriteMask));
				combine(static_cast<size_t>(DepthBias));
				// static_cast<size_t> directly on a negative FLOAT is UB - go through a
				// same-width signed integer, whose conversion to size_t is well defined.
				combine(static_cast<size_t>(*reinterpret_cast<const INT32*>(&SlopeScaledDepthBias)));
				return h;
			}
		};

		//! Hashes an array of D3D12_INPUT_ELEMENT_DESC for use as SPSOKey::InputLayoutHash.
		//! Hashes format/slot/offset/instancing of each element, not the text of SemanticName.
		size_t hashInputLayout(const D3D12_INPUT_ELEMENT_DESC* elements, UINT count);

		//! PSO cache. One per driver, not per frame - PSOs are immutable, expensive to
		//! create, and shared across all frames.
		class CD3D12PSOCache
		{
		public:
			//! Returns the existing PSO for this key, or creates a new one (blocking -
			//! CreateGraphicsPipelineState can take several milliseconds on first use).
			//! Returns nullptr if creation fails (error already logged).
			//! geometryShader/soEntries*/soStrides*/disableRasterization are optional
			//! (default to no GS, no stream-output). When soEntryCount > 0 the stream-output
			//! table is attached to the PSO (D3D12_STREAM_OUTPUT_DESC), and if
			//! disableRasterization is true RasterizedStream is set to
			//! D3D12_SO_NO_RASTERIZED_STREAM - a pure stream-output GS does not feed the
			//! rasterizer.
			//! hullShader/domainShader, like geometryShader, are optional (default nullptr).
			ID3D12PipelineState* getOrCreate(ID3D12Device* device, ID3D12RootSignature* rootSignature,
				const SPSOKey& key, ID3DBlob* vertexShader, ID3DBlob* pixelShader,
				const D3D12_INPUT_ELEMENT_DESC* inputElements, UINT inputElementCount,
				ID3DBlob* geometryShader = nullptr,
				const D3D12_SO_DECLARATION_ENTRY* soEntries = nullptr, UINT soEntryCount = 0,
				const UINT* soStrides = nullptr, UINT soStrideCount = 0,
				bool disableRasterization = false,
				ID3DBlob* hullShader = nullptr, ID3DBlob* domainShader = nullptr);

			void clear() { Cache.clear(); }
			size_t size() const { return Cache.size(); }

		private:
			std::unordered_map<size_t, ComPtr<ID3D12PipelineState>> Cache;
		};

	}
}

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
#endif
