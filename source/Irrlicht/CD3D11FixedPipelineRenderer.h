#ifndef __C_D3D11_FIXED_FUNCTION_MATERIAL_RENDERER_H_INCLUDED__
#define __C_D3D11_FIXED_FUNCTION_MATERIAL_RENDERER_H_INCLUDED__

#include "IrrCompileConfig.h"

#ifdef _IRR_WINDOWS_

#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "CD3D11Driver.h"
#include "CD3D11MaterialRenderer.h"
#include "IShaderConstantSetCallBack.h"

namespace irr
{
	namespace video
	{

		namespace
		{

			struct SShaderMaterial
			{
				SColorf ambient;
				SColorf diffuse;
				SColorf specular;
				SColorf emissive;
				f32 shininess;
				s32	type;	// video::E_MATERIAL_TYPE
			};

			struct SShaderLight
			{
				SColorf position;
				SColorf diffuse;
				SColorf specular;
				SColorf ambient;
				SColorf atten;
			};

			core::matrix4 SphereMapMatrix;
			core::matrix4 UnitMapMatrix;
		}

		class CD3D11FixedPipelineRenderer : public CD3D11MaterialRenderer, IShaderConstantSetCallBack
		{
		public:
			//! Constructor for fixed function effect
			CD3D11FixedPipelineRenderer(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls);

			virtual ~CD3D11FixedPipelineRenderer();

			virtual bool OnRender(IMaterialRendererServices* service, IVertexDescriptor* vtxtype);

			virtual void OnSetMaterial(const SMaterial& material);

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services) {

				
			};

			virtual void OnSetConstants(IMaterialRendererServices* service, s32 userData);

		protected:
			s32 cbPerFrameId;
			s32	cbPerTechniqueId;
			s32	cbLightsId;

			SShader* vsStandardShader;
			SShader* psStandardShader;
			SShader* vsTangentsShader;
			SShader* psTangentsShader;
			SShader* vsCoords2TShader;
			SShader* psCoords2TShader;

			ID3DBlob* standardBuffer;
			ID3DBlob* coords2TBuffer;
			ID3DBlob* tangentsBuffer;

			struct SCbPerFrame
			{
				core::matrix4 mWorld;
				core::matrix4 mView;
				core::matrix4 mProj;
				core::matrix4 mTexture;
			} cbPerFrame;

			struct SCbPerTechnique
			{
				SColorf fogColor;
				s32 fogMode;
				f32 fogStart;
				f32 fogEnd;
				f32 fogDensity;
				s32 enableAlpha;
				s32 enableLighting;
				s32 enableClipping;
				s32 enableFog;
				s32 enablePointScale;
				f32 pointScaleA;
				f32 pointScaleB;
				f32 pointScaleC;
				f32 pointSize;
			} cbPerTechnique;

			struct SCbLights
			{
				SColorf clipPlanes[3];
				SShaderLight lights[8];
				SShaderMaterial material;
				s32 lightCount;
			} cbLights;
		};

		class CD3D11MaterialRenderer_SOLID : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_SOLID(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = false;
				
			}
		};

		class CD3D11MaterialRenderer_SOLID_2_LAYER : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_SOLID_2_LAYER(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = FALSE;
				
			}
		};

		class CD3D11MaterialRenderer_LIGHTMAP : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_LIGHTMAP(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = FALSE;
				
			}
		};

		class CD3D11MaterialRenderer_DETAIL_MAP : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_DETAIL_MAP(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();
				D3D11_SAMPLER_DESC* samplerDescs = static_cast<CD3D11Driver*>(Driver)->getSamplerDescs();

				blendDesc.RenderTarget[0].BlendEnable = FALSE;
				
			}
		};

		class CD3D11MaterialRenderer_SPHERE_MAP : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_SPHERE_MAP(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = FALSE;

				// set texture matrix
				cbPerFrame.mTexture = SphereMapMatrix;
				
			}

			virtual void OnUnsetMaterial()
			{
				cbPerFrame.mTexture = UnitMapMatrix;

				CD3D11FixedPipelineRenderer::OnUnsetMaterial();
			}
		};

		class CD3D11MaterialRenderer_REFLECTION_2_LAYER : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_REFLECTION_2_LAYER(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = false;

				// set texture matrix
				cbPerFrame.mTexture = SphereMapMatrix;
				
			}

			virtual void OnUnsetMaterial()
			{
				cbPerFrame.mTexture = UnitMapMatrix;

				CD3D11FixedPipelineRenderer::OnUnsetMaterial();
			}
		};

		class CD3D11MaterialRenderer_TRANSPARENT_ADD_COLOR : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_TRANSPARENT_ADD_COLOR(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = TRUE;
				blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
				blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
				blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
				blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
				
			}

			//! Returns if the material is transparent. The scene management needs to know this
			//! for being able to sort the materials by opaque and transparent.
			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return true;
			}
		};

		class CD3D11MaterialRenderer_TRANSPARENT_ALPHA_CHANNEL : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_TRANSPARENT_ALPHA_CHANNEL(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = TRUE;
				blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
				blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
				blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
				blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
				blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
				
			}

			//! Returns if the material is transparent. The scene managment needs to know this
			//! for being able to sort the materials by opaque and transparent.
			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return true;
			}
		};

		class CD3D11MaterialRenderer_TRANSPARENT_ALPHA_CHANNEL_REF : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_TRANSPARENT_ALPHA_CHANNEL_REF(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = true;
				
			}

			//! Returns if the material is transparent. The scene managment needs to know this
			//! for being able to sort the materials by opaque and transparent.
			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return false; // this material is not really transparent because it does no blending.
			}
		};

		class CD3D11MaterialRenderer_TRANSPARENT_VERTEX_ALPHA : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_TRANSPARENT_VERTEX_ALPHA(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = TRUE;
				blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
				blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
				blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
				blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
				
			}

			//! Returns if the material is transparent. The scene managment needs to know this
			//! for being able to sort the materials by opaque and transparent.
			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return true;
			}
		};

		class CD3D11MaterialRenderer_TRANSPARENT_REFLECTION_2_LAYER : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_TRANSPARENT_REFLECTION_2_LAYER(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates, IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				D3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();

				blendDesc.RenderTarget[0].BlendEnable = TRUE;
				blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
				blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
				blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
				blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
				blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
				
			}

			//! Returns if the material is transparent. The scene managment needs to know this
			//! for being able to sort the materials by opaque and transparent.
			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return true;
			}
		};

		class CD3D11MaterialRenderer_ONETEXTURE_BLEND : public CD3D11FixedPipelineRenderer
		{
		public:
			CD3D11MaterialRenderer_ONETEXTURE_BLEND(ID3D11Device* device, IVideoDriver* driver, CD3D11CallBridge* bridgeCalls)
				: CD3D11FixedPipelineRenderer(device, driver, bridgeCalls) {}

			virtual void OnSetMaterial(const video::SMaterial& material, const video::SMaterial& lastMaterial,
				bool resetAllRenderstates, video::IMaterialRendererServices* services)
			{
				CD3D11FixedPipelineRenderer::OnSetMaterial(material);
				//if (material.MaterialType != lastMaterial.MaterialType ||
				//	material.MaterialTypeParam != lastMaterial.MaterialTypeParam ||
				//	resetAllRenderstates)
				{
					services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
				}

				SD3D11_BLEND_DESC& blendDesc = static_cast<CD3D11Driver*>(Driver)->getBlendDesc();
				E_BLEND_FACTOR srcFact, dstFact;
				E_MODULATE_FUNC modulate;
				u32 alphaSource;
				unpack_textureBlendFunc(srcFact, dstFact, modulate, alphaSource, material.MaterialTypeParam);
				//	if (srcFact == EBF_SRC_COLOR && dstFact == EBF_ZERO)
				{
					//	blendDesc.RenderTarget[0].BlendEnable = FALSE;
				}
				//	else
				{
					blendDesc.RenderTarget[0].BlendEnable = TRUE;
					blendDesc.RenderTarget[0].SrcBlend = getD3DBlend(srcFact);
					blendDesc.RenderTarget[0].DestBlend = getD3DBlend(dstFact);

					blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
				}
				

			}

			//! Returns if the material is transparent.
			/** The scene management needs to know this for being able to sort the
			materials by opaque and transparent.
			The return value could be optimized, but we'd need to know the
			MaterialTypeParam for it. */
			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return true;
			}

		private:
			D3D11_BLEND getD3DBlend(E_BLEND_FACTOR factor) const
			{
				D3D11_BLEND r;
				switch (factor)
				{
				case EBF_ZERO:					r = D3D11_BLEND_ZERO; break;
				case EBF_ONE:					r = D3D11_BLEND_ONE; break;
				case EBF_DST_COLOR:				r = D3D11_BLEND_DEST_COLOR; break;
				case EBF_ONE_MINUS_DST_COLOR:	r = D3D11_BLEND_INV_DEST_COLOR; break;
				case EBF_SRC_COLOR:				r = D3D11_BLEND_SRC_COLOR; break;
				case EBF_ONE_MINUS_SRC_COLOR:	r = D3D11_BLEND_INV_SRC_COLOR; break;
				case EBF_SRC_ALPHA:				r = D3D11_BLEND_SRC_ALPHA; break;
				case EBF_ONE_MINUS_SRC_ALPHA:	r = D3D11_BLEND_INV_SRC_ALPHA; break;
				case EBF_DST_ALPHA:				r = D3D11_BLEND_DEST_ALPHA; break;
				case EBF_ONE_MINUS_DST_ALPHA:	r = D3D11_BLEND_INV_DEST_ALPHA; break;
				case EBF_SRC_ALPHA_SATURATE:	r = D3D11_BLEND_SRC_ALPHA_SAT; break;
				}
				return r;
			}
		};
	}
}

#endif
#endif
#endif