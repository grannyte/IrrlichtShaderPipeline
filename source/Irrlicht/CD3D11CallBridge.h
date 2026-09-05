#ifndef __C_DIRECTX11_CALLBRIDGE_H_INCLUDED__
#define __C_DIRECTX11_CALLBRIDGE_H_INCLUDED__

#include "IrrCompileConfig.h"

#ifdef _IRR_WINDOWS_

#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "CD3D11MaterialRenderer.h"
#include "irrMap.h"
#include <d3d11_3.h>

struct ID3D11DeviceContext;
struct ID3D11Device;

namespace irr
{
	namespace video
	{
		//! D3D11_BLEND_DESC plus the 11.1+ state that travels with it: a logic op (through
		//! ID3D11Device1::CreateBlendState1) and the OMSetBlendState() sample mask. The compare
		//! operators memcmp the whole struct, so every byte is set by reset().
		struct SD3D11_BLEND_DESC : public D3D11_BLEND_DESC
		{
			//! SMaterial::LogicOp, applied to every target; BlendEnable is forced off with it.
			BOOL LogicOpEnable;
			D3D11_LOGIC_OP LogicOp;
			//! SMaterial::SampleMask.
			UINT SampleMask;

			SD3D11_BLEND_DESC()
			{
				reset();
			}

			inline bool operator==(const SD3D11_BLEND_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_BLEND_DESC)) == 0;
			}

			inline bool operator!=(const SD3D11_BLEND_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_BLEND_DESC)) != 0;
			}

			inline bool operator<(const SD3D11_BLEND_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_BLEND_DESC)) < 0;
			}

			inline void reset()
			{
				LogicOpEnable = FALSE;
				LogicOp = D3D11_LOGIC_OP_NOOP;
				SampleMask = 0xffffffffu;
				AlphaToCoverageEnable = false;
				IndependentBlendEnable = false;
				RenderTarget[0].BlendEnable = false;
				RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
				RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
				RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
				RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
				RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
				RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
				RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
				memcpy(&RenderTarget[1], &RenderTarget[0], sizeof(D3D11_RENDER_TARGET_BLEND_DESC));
				memcpy(&RenderTarget[2], &RenderTarget[0], sizeof(D3D11_RENDER_TARGET_BLEND_DESC));
				memcpy(&RenderTarget[3], &RenderTarget[0], sizeof(D3D11_RENDER_TARGET_BLEND_DESC));
				memcpy(&RenderTarget[4], &RenderTarget[0], sizeof(D3D11_RENDER_TARGET_BLEND_DESC));
				memcpy(&RenderTarget[5], &RenderTarget[0], sizeof(D3D11_RENDER_TARGET_BLEND_DESC));
				memcpy(&RenderTarget[6], &RenderTarget[0], sizeof(D3D11_RENDER_TARGET_BLEND_DESC));
				memcpy(&RenderTarget[7], &RenderTarget[0], sizeof(D3D11_RENDER_TARGET_BLEND_DESC));
			}
		};

		// Rasterizer
		struct SD3D11_RASTERIZER_DESC : public D3D11_RASTERIZER_DESC
		{
			//! SMaterial::ConservativeRaster, through ID3D11Device3::CreateRasterizerState2.
			BOOL ConservativeRaster;

			SD3D11_RASTERIZER_DESC()
			{
				reset();
			}

			inline bool operator==(const SD3D11_RASTERIZER_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_RASTERIZER_DESC)) == 0;
			}

			inline bool operator!=(const SD3D11_RASTERIZER_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_RASTERIZER_DESC)) != 0;
			}

			inline bool operator<(const SD3D11_RASTERIZER_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_RASTERIZER_DESC)) < 0;
			}

			inline void reset()
			{
				ConservativeRaster = FALSE;
				FillMode = D3D11_FILL_SOLID;
				CullMode = D3D11_CULL_BACK;
				FrontCounterClockwise = false;
				DepthBias = 0;
				DepthBiasClamp = 0;
				SlopeScaledDepthBias = 0;
				DepthClipEnable = true;
				ScissorEnable = false;
				MultisampleEnable = false;
				AntialiasedLineEnable = false;
			}
		};

		// Depth stencil
		struct SD3D11_DEPTH_STENCIL_DESC : public D3D11_DEPTH_STENCIL_DESC
		{
			SD3D11_DEPTH_STENCIL_DESC()
			{
				reset();
			}

			inline bool operator==(const SD3D11_DEPTH_STENCIL_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_DEPTH_STENCIL_DESC)) == 0;
			}

			inline bool operator!=(const SD3D11_DEPTH_STENCIL_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_DEPTH_STENCIL_DESC)) != 0;
			}

			inline bool operator<(const SD3D11_DEPTH_STENCIL_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_DEPTH_STENCIL_DESC)) < 0;
			}

			inline bool operator>(const SD3D11_DEPTH_STENCIL_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_DEPTH_STENCIL_DESC)) > 0;
			}

			inline void reset()
			{
				DepthEnable = true;
				DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
				DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
				StencilEnable = false;
				StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
				StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
				FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
				FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
				FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
				FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
				BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
				BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
				BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
				BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
			}
		};

		// Samplers
		struct SD3D11_SAMPLER_DESC : public D3D11_SAMPLER_DESC
		{
			SD3D11_SAMPLER_DESC()
			{
				reset();
			}

			inline bool operator==(const SD3D11_SAMPLER_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_SAMPLER_DESC)) == 0;
			}

			inline bool operator!=(const SD3D11_SAMPLER_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_SAMPLER_DESC)) != 0;
			}

			inline bool operator<(const SD3D11_SAMPLER_DESC& other) const
			{
				return memcmp(this, &other, sizeof(SD3D11_SAMPLER_DESC)) < 0;
			}

			inline void reset()
			{
				Filter = D3D11_FILTER_ANISOTROPIC;
				AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
				AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
				AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
				MipLODBias = 0;
				MaxAnisotropy = 16;
				ComparisonFunc = D3D11_COMPARISON_NEVER;
				MinLOD = 0.0f;
				MaxLOD = D3D11_FLOAT32_MAX;
			}
		};

		class CD3D11VertexDeclaration;
		class CD3D11Driver;
		class IVertexDescriptor;

		//! This bridge between Irlicht pseudo DX calls and true DX calls.
		class CD3D11CallBridge
		{
		public:
			CD3D11CallBridge(ID3D11Device* device, CD3D11Driver* driver, ID3D11DeviceContext* explicitContext = 0);
			~CD3D11CallBridge();

			void setVertexShader(SShader* shader);
			void setPixelShader(SShader* shader);
			void setGeometryShader(SShader* shader);
			void setHullShader(SShader* shader);
			void setDomainShader(SShader* shader);
			void setComputeShader(SShader* shader);

			void setBlendState(const SD3D11_BLEND_DESC& BlendDesc);
			void setDepthStencilState(const SD3D11_DEPTH_STENCIL_DESC& depthStencilDesc);
			void setRasterizerState(const SD3D11_RASTERIZER_DESC& rasterizerDesc);

			void setShaderResources(SD3D11_SAMPLER_DESC SamplerDesc[MATERIAL_MAX_TEXTURES], ITexture* shaderViews[MATERIAL_MAX_TEXTURES]);
			//! Forget a cached texture binding, so the next material set rebinds it.
			void invalidateTextureBinding(ITexture* texture);
			void setPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY top);
			void setInputLayout(IVertexDescriptor* vtxDescriptor, IMaterialRenderer* r);

			//! True if a vertex shader is currently bound. Diagnostic seam: a draw with none is what
			//! the debug layer reports as DEVICE_DRAW_VERTEX_SHADER_NOT_SET, a call too late to trace.
			bool hasVertexShader() const { return shaders[EST_VERTEX_SHADER] != 0; }

			//! True if both tessellation stages are bound. D3D11 then requires a patch topology.
			bool hasTessellationStages() const
			{
				return shaders[EST_HULL_SHADER] != 0 && shaders[EST_DOMAIN_SHADER] != 0;
			}

			ID3D11SamplerState* getSamplerState(u32 idx);

			void setViewPort(const core::rect<s32>& vp);

			//! Drop every cached binding so the next setter re-issues its D3D call. Use after
			//! FinishCommandList/ExecuteCommandList, which reset real state behind this cache's back.
			void invalidateCache();

			//! Force depth-stencil/blend/rasterizer/viewport/topology/input-layout back onto the device from this bridge's own cache -- needed after ExecuteCommandList, which changes real state without going through these setters. Deliberately excludes shaders/textures/samplers -- see the .cpp.
			void forceReapplyAll();

		private:
			//! Synchronise le cache d'etat avec le device au demarrage (voir le .cpp : sans cela le
			//! cache pretend detenir des etats que le device n'a jamais recus).
			void applyInitialStates();

			ID3D11DeviceContext* Context;
			ID3D11Device* Device;
			//! Device1 creates the logic-op blend states, Device3 the conservative rasterizer
			//! states; null on an older runtime, and the flags are then ignored.
			ID3D11Device1* Device1;
			ID3D11Device3* Device3;
			CD3D11Driver* Driver;

			SShader* shaders[EST_COUNT];

			u32 samplersChanged;
			u32 texturesChanged;

			SD3D11_DEPTH_STENCIL_DESC DepthStencilDesc;
			core::map<SD3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState*> DepthStencilMap;

			SD3D11_BLEND_DESC BlendDesc;
			core::map<SD3D11_BLEND_DESC, ID3D11BlendState*> BlendMap;

			ITexture* CurrentTextures[MATERIAL_MAX_TEXTURES];
			ID3D11SamplerState* SamplerStates[MATERIAL_MAX_TEXTURES];
			SD3D11_SAMPLER_DESC SamplerDesc[MATERIAL_MAX_TEXTURES];
			core::map<SD3D11_SAMPLER_DESC, ID3D11SamplerState*> SamplerMap;

			SD3D11_RASTERIZER_DESC RasterizerDesc;
			core::map<SD3D11_RASTERIZER_DESC, ID3D11RasterizerState*> RasterizerMap;

			ID3D11InputLayout* InputLayout;

			D3D11_PRIMITIVE_TOPOLOGY Topology;

			IVertexDescriptor* VtxDescriptor;
			core::map<size_t, ID3D11InputLayout*> LayoutMap;
			void* ShaderByteCode;
			size_t ShaderByteCodeSize;

			core::rect<s32> ViewPort;
		};

	}
}

#endif
#endif
#endif