// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CD3D12PSOCache.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_
#include "os.h"
#include "CD3D12Helpers.h"

namespace irr
{
	namespace video
	{
		size_t hashInputLayout(const D3D12_INPUT_ELEMENT_DESC* elements, UINT count)
		{
			size_t h = count;
			auto combine = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
			for (UINT i = 0; i < count; ++i)
			{
				combine(static_cast<size_t>(elements[i].Format));
				combine(static_cast<size_t>(elements[i].InputSlot));
				combine(static_cast<size_t>(elements[i].AlignedByteOffset));
				combine(static_cast<size_t>(elements[i].InputSlotClass));
				combine(static_cast<size_t>(elements[i].SemanticIndex));
				combine(static_cast<size_t>(elements[i].InstanceDataStepRate));
				// SemanticName (const char*) intentionally not hashed, see CD3D12PSOCache.h.
			}
			return h;
		}

		ID3D12PipelineState* CD3D12PSOCache::getOrCreate(ID3D12Device* device, ID3D12RootSignature* rootSignature,
			const SPSOKey& key, ID3DBlob* vertexShader, ID3DBlob* pixelShader,
			const D3D12_INPUT_ELEMENT_DESC* inputElements, UINT inputElementCount,
			ID3DBlob* geometryShader,
			const D3D12_SO_DECLARATION_ENTRY* soEntries, UINT soEntryCount,
			const UINT* soStrides, UINT soStrideCount,
			bool disableRasterization,
			ID3DBlob* hullShader, ID3DBlob* domainShader)
		{
			size_t hash = key.computeHash();
			auto it = Cache.find(hash);
			if (it != Cache.end())
				return it->second.Get();

			if (!device || !rootSignature || !vertexShader || !pixelShader)
			{
				os::Printer::log("CD3D12PSOCache::getOrCreate: null parameter, PSO not created", ELL_ERROR);
				return nullptr;
			}

			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
			desc.pRootSignature = rootSignature;
			desc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
			desc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
			desc.InputLayout = { inputElements, inputElementCount };
			if (geometryShader)
				desc.GS = { geometryShader->GetBufferPointer(), geometryShader->GetBufferSize() };
			if (hullShader)
				desc.HS = { hullShader->GetBufferPointer(), hullShader->GetBufferSize() };
			if (domainShader)
				desc.DS = { domainShader->GetBufferPointer(), domainShader->GetBufferSize() };
			if (soEntryCount > 0)
			{
				desc.StreamOutput.pSODeclaration = soEntries;
				desc.StreamOutput.NumEntries = soEntryCount;
				desc.StreamOutput.pBufferStrides = soStrides;
				desc.StreamOutput.NumStrides = soStrideCount;
				desc.StreamOutput.RasterizedStream = disableRasterization ?
					D3D12_SO_NO_RASTERIZED_STREAM : 0;
			}
			desc.PrimitiveTopologyType = key.TopologyType;
			// Up to 8 simultaneous render targets, see SPSOKey::NumRenderTargets/RTVFormats.
			desc.NumRenderTargets = key.NumRenderTargets;
			for (UINT i = 0; i < key.NumRenderTargets && i < 8; ++i)
				desc.RTVFormats[i] = key.RTVFormats[i];
			desc.DSVFormat = key.DSVFormat;
			// Quality stays 0 - a PSO only fixes the sample count, not the quality level
			// (that's a property of the RTV/DSV resource itself, see
			// CD3D12Texture::createResource()/CD3D12Driver::checkRTTDepthBuffer()).
			desc.SampleDesc = { key.SampleCount, 0 };
			desc.SampleMask = UINT_MAX;

			// Rasterizer state
			desc.RasterizerState.FillMode = key.FillMode;
			desc.RasterizerState.CullMode = key.CullMode;
			desc.RasterizerState.FrontCounterClockwise = FALSE; // Irrlicht convention: clockwise = front face
			desc.RasterizerState.DepthClipEnable = TRUE;
			desc.RasterizerState.DepthBias = key.DepthBias;
			desc.RasterizerState.DepthBiasClamp = 0.0f;
			desc.RasterizerState.SlopeScaledDepthBias = key.SlopeScaledDepthBias;
			desc.RasterizerState.MultisampleEnable = key.SampleCount > 1;
			desc.RasterizerState.AntialiasedLineEnable = FALSE;
			desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

			// Depth/stencil state. StencilEnable/ops support shadow volumes (see
			// CD3D12PSOCache.h); FrontFace==BackFace, see the RenderTargetWriteMask comment
			// in SPSOKey.
			desc.DepthStencilState.DepthEnable = key.DepthTestEnable;
			desc.DepthStencilState.DepthWriteMask = key.DepthWriteEnable ?
				D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
			desc.DepthStencilState.DepthFunc = key.DepthFunc;
			desc.DepthStencilState.StencilEnable = key.StencilEnable;
			desc.DepthStencilState.StencilReadMask = key.StencilReadMask;
			desc.DepthStencilState.StencilWriteMask = key.StencilWriteMask;
			desc.DepthStencilState.FrontFace.StencilFailOp = key.StencilFailOp;
			desc.DepthStencilState.FrontFace.StencilDepthFailOp = key.StencilDepthFailOp;
			desc.DepthStencilState.FrontFace.StencilPassOp = key.StencilPassOp;
			desc.DepthStencilState.FrontFace.StencilFunc = key.StencilFunc;
			desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;

			// Blend state - a single set of parameters, written to RenderTarget[0]. Four modes
			// are covered (see the SPSOKey::EBlendMode comment); the rest of the
			// E_MATERIAL_TYPE values fall back to None (opaque) via buildPSOKeyFromMaterial().
			// IndependentBlendEnable stays FALSE, so D3D12 applies RenderTarget[0] to every
			// active target (desc.NumRenderTargets above) - MRT draws automatically inherit
			// the same blend everywhere without duplicating rtBlend across slots 1..N-1. No
			// per-target independent blend in this path (unlike
			// CD3D11Driver::setRenderTarget(), which reads BlendFuncSrc/BlendFuncDst per
			// IRenderTarget).
			D3D12_RENDER_TARGET_BLEND_DESC& rtBlend = desc.BlendState.RenderTarget[0];
			switch (key.BlendMode)
			{
			case SPSOKey::EBlendMode::AlphaBlend:
				rtBlend.BlendEnable = TRUE;
				rtBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
				rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
				rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
				rtBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
				break;
			case SPSOKey::EBlendMode::AddColor:
				// EMT_TRANSPARENT_ADD_COLOR: pure additive blend, not alpha-weighted
				// (unlike AlphaBlend above).
				rtBlend.BlendEnable = TRUE;
				rtBlend.SrcBlend = D3D12_BLEND_ONE;
				rtBlend.DestBlend = D3D12_BLEND_ONE;
				rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
				rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
				rtBlend.DestBlendAlpha = D3D12_BLEND_ONE;
				rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
				break;
			case SPSOKey::EBlendMode::Custom:
				// EMT_ONETEXTURE_BLEND: BlendFunc = source * sourceFactor + dest * destFactor,
				// factors decoded from SMaterial::MaterialTypeParam by
				// CD3D12Driver::buildPSOKeyFromMaterial(). The alpha slot uses its own set of
				// factors (see SPSOKey::CustomSrcBlend/CustomDestBlend) since a "_COLOR"
				// factor on SrcBlendAlpha/DestBlendAlpha is rejected by D3D12 validation.
				rtBlend.BlendEnable = TRUE;
				rtBlend.SrcBlend = key.CustomSrcBlend;
				rtBlend.DestBlend = key.CustomDestBlend;
				rtBlend.BlendOp = key.CustomBlendOp;
				rtBlend.SrcBlendAlpha = key.CustomSrcBlendAlpha;
				rtBlend.DestBlendAlpha = key.CustomDestBlendAlpha;
				rtBlend.BlendOpAlpha = key.CustomBlendOp;
				break;
			case SPSOKey::EBlendMode::None:
			default:
				rtBlend.BlendEnable = FALSE;
				rtBlend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
				rtBlend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
				rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
				rtBlend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
				break;
			}
			rtBlend.LogicOpEnable = FALSE;
			rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
			rtBlend.RenderTargetWriteMask = key.RenderTargetWriteMask;

			ComPtr<ID3D12PipelineState> pso;
			HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
			if (FAILED(hr))
			{
				logD3D12Failure("CD3D12PSOCache::getOrCreate: CreateGraphicsPipelineState", hr, device);
				return nullptr;
			}

			ID3D12PipelineState* raw = pso.Get();
			Cache[hash] = std::move(pso);
			return raw;
		}

	}
}

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
