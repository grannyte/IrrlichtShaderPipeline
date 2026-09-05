// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Cache of Pipeline State Objects, keyed on everything that determines a PSO in D3D12
// (shader bytecode, root signature, blend, depth/stencil, rasterizer, input layout, render
// target formats). Generic - not tied to any particular material system.

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

		//! Everything that determines an ID3D12PipelineState. Two draws with an equal SPSOKey
		//! can reuse the same PSO.
		struct SPSOKey
		{
			size_t VSHash = 0;
			size_t PSHash = 0;
			//! 0 if no geometry shader. StreamOutputHash is independent: non-zero only for a pure
			//! stream-output GS, which needs a distinct PSO from the same blob rasterizing normally.
			size_t GSHash = 0;
			size_t StreamOutputHash = 0;
			//! 0 if no tessellation. HS/DS only ever exist together, so both or neither.
			size_t HSHash = 0;
			size_t DSHash = 0;
			//! Hash of the ID3D12RootSignature* the PSO is created against. Part of the key because
			//! a PSO embeds its root signature and materials no longer share one; safe to compare by
			//! pointer since getOrCreateRootSignature() deduplicates by layout.
			size_t RootSignatureHash = 0;
			//! None: opaque. AlphaBlend: EMT_TRANSPARENT_ALPHA_CHANNEL/VERTEX_ALPHA. AddColor:
			//! EMT_TRANSPARENT_ADD_COLOR. Custom: factors from SMaterial::MaterialTypeParam, see
			//! CD3D12Driver::buildPSOKeyFromMaterial().
			enum class EBlendMode { None, AlphaBlend, AddColor, Custom };
			EBlendMode BlendMode = EBlendMode::None;
			//! Only meaningful when BlendMode == Custom. Separate RGB/alpha fields because D3D12
			//! validation rejects the "_COLOR" D3D12_BLEND values on SrcBlendAlpha/DestBlendAlpha,
			//! see CD3D12Driver::getD3D12BlendFactor().
			D3D12_BLEND CustomSrcBlend = D3D12_BLEND_ONE;
			D3D12_BLEND CustomDestBlend = D3D12_BLEND_ZERO;
			D3D12_BLEND CustomSrcBlendAlpha = D3D12_BLEND_ONE;
			D3D12_BLEND CustomDestBlendAlpha = D3D12_BLEND_ZERO;
			D3D12_BLEND_OP CustomBlendOp = D3D12_BLEND_OP_ADD;
			bool DepthTestEnable = true;
			bool DepthWriteEnable = true;
			//! Defaults to GREATER to match SMaterial's ECFN_GREATER and this fork's inverted depth
			//! convention (see CD3D12Driver::clearZBuffer()).
			D3D12_COMPARISON_FUNC DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
			D3D12_CULL_MODE CullMode = D3D12_CULL_MODE_BACK;
			D3D12_FILL_MODE FillMode = D3D12_FILL_MODE_SOLID;
			D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			//! Up to D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT targets; single-target callers only
			//! need RTVFormats[0].
			UINT NumRenderTargets = 1;
			DXGI_FORMAT RTVFormats[8] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
				DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
			DXGI_FORMAT DSVFormat = DXGI_FORMAT_D32_FLOAT;
			//! Must match SampleDesc.Count of every RTV/DSV bound by OMSetRenderTargets() at draw time.
			UINT SampleCount = 1;
			size_t InputLayoutHash = 0;

			// Stencil shadow volumes. The defaults leave normal material PSOs unaffected; only
			// CD3D12Driver::getOrCreateAuxPSO() sets these away from them.
			bool StencilEnable = false;
			UINT8 StencilReadMask = 0xFF;
			UINT8 StencilWriteMask = 0xFF;
			D3D12_STENCIL_OP StencilFailOp = D3D12_STENCIL_OP_KEEP;
			D3D12_STENCIL_OP StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
			D3D12_STENCIL_OP StencilPassOp = D3D12_STENCIL_OP_KEEP;
			D3D12_COMPARISON_FUNC StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			//! Applied to FrontFace and BackFace alike: PSOs needing this already cull one face.
			UINT8 RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

			//! SMaterial::PolygonOffsetFactor/PolygonOffsetDirection, same mapping as
			//! CD3D11Driver::setBasicRenderStates().
			INT DepthBias = 0;
			FLOAT SlopeScaledDepthBias = 0.0f;

			//! SMaterial::AntiAliasing & EAAM_ALPHA_TO_COVERAGE, as CD3D11Driver::setBasicRenderStates().
			bool AlphaToCoverage = false;

			//! SMaterial::LogicOp: replaces blending on every target when enabled.
			bool LogicOpEnable = false;
			D3D12_LOGIC_OP LogicOp = D3D12_LOGIC_OP_NOOP;
			//! SMaterial::ConservativeRaster.
			bool ConservativeRaster = false;
			//! SMaterial::SampleMask, D3D12_GRAPHICS_PIPELINE_STATE_DESC::SampleMask.
			UINT SampleMask = UINT_MAX;

			//! Per-target blend overrides of an MRT draw, from the IRenderTarget entries of
			//! setRenderTarget(array). Bit i of TargetOverrideMask set means RenderTarget[i] takes the
			//! fields below instead of the material blend (IndependentBlendEnable goes TRUE); bit 0
			//! is never set, the first target follows the material as on D3D11.
			UINT8 TargetOverrideMask = 0;
			BOOL TargetBlendEnable[8] = {};
			D3D12_BLEND TargetSrcBlend[8] = { D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE,
				D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE };
			D3D12_BLEND TargetDestBlend[8] = { D3D12_BLEND_ZERO, D3D12_BLEND_ZERO, D3D12_BLEND_ZERO, D3D12_BLEND_ZERO,
				D3D12_BLEND_ZERO, D3D12_BLEND_ZERO, D3D12_BLEND_ZERO, D3D12_BLEND_ZERO };
			UINT8 TargetWriteMask[8] = {};

			bool operator==(const SPSOKey& other) const
			{
				if (TargetOverrideMask != other.TargetOverrideMask)
					return false;
				for (UINT i = 0; i < 8; ++i)
				{
					if (!(TargetOverrideMask & (1u << i)))
						continue;
					if (TargetBlendEnable[i] != other.TargetBlendEnable[i] ||
						TargetSrcBlend[i] != other.TargetSrcBlend[i] ||
						TargetDestBlend[i] != other.TargetDestBlend[i] ||
						TargetWriteMask[i] != other.TargetWriteMask[i])
						return false;
				}
				return VSHash == other.VSHash && PSHash == other.PSHash &&
					AlphaToCoverage == other.AlphaToCoverage &&
					LogicOpEnable == other.LogicOpEnable && LogicOp == other.LogicOp &&
					ConservativeRaster == other.ConservativeRaster && SampleMask == other.SampleMask &&
					GSHash == other.GSHash && StreamOutputHash == other.StreamOutputHash &&
					HSHash == other.HSHash && DSHash == other.DSHash &&
					RootSignatureHash == other.RootSignatureHash &&
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

			//! Combines all fields into the cache key. Colliding keys would incorrectly share a
			//! PSO; there is no collision handling, see CD3D12PSOCache::getOrCreate().
			size_t computeHash() const
			{
				size_t h = VSHash;
				auto combine = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
				combine(PSHash);
				combine(GSHash);
				combine(StreamOutputHash);
				combine(HSHash);
				combine(DSHash);
				combine(RootSignatureHash);
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
				// static_cast<size_t> on a negative FLOAT is UB - go through a same-width signed int.
				combine(static_cast<size_t>(*reinterpret_cast<const INT32*>(&SlopeScaledDepthBias)));
				combine(static_cast<size_t>(AlphaToCoverage));
				combine(static_cast<size_t>(LogicOpEnable));
				combine(static_cast<size_t>(LogicOp));
				combine(static_cast<size_t>(ConservativeRaster));
				combine(static_cast<size_t>(SampleMask));
				combine(static_cast<size_t>(TargetOverrideMask));
				for (UINT i = 0; i < 8; ++i)
				{
					if (!(TargetOverrideMask & (1u << i)))
						continue;
					combine(static_cast<size_t>(TargetBlendEnable[i]));
					combine(static_cast<size_t>(TargetSrcBlend[i]));
					combine(static_cast<size_t>(TargetDestBlend[i]));
					combine(static_cast<size_t>(TargetWriteMask[i]));
				}
				return h;
			}
		};

		//! Hashes an input layout for SPSOKey::InputLayoutHash: format/slot/offset/instancing of
		//! each element, not the text of SemanticName.
		size_t hashInputLayout(const D3D12_INPUT_ELEMENT_DESC* elements, UINT count);

		//! PSO cache. One per driver, not per frame - PSOs are immutable and expensive to create.
		class CD3D12PSOCache
		{
		public:
			//! Returns the existing PSO for this key, or creates one (blocking - can take several
			//! milliseconds). Returns nullptr on failure (already logged). The geometry, stream
			//! output and tessellation parameters are optional.
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
