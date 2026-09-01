#include "CD3D11CallBridge.h"

#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "os.h"
#include "CD3D11Driver.h"
#include "CD3D11Texture.h"
#include "CD3D11VertexDescriptor.h"

#include <d3d11.h>

namespace irr
{
	namespace video
	{
		CD3D11CallBridge::CD3D11CallBridge(ID3D11Device* device, CD3D11Driver* driver, ID3D11DeviceContext* explicitContext)
			: Context(NULL), Device(device), Driver(driver), InputLayout(NULL),
			Topology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED), VtxDescriptor(NULL), ShaderByteCode(NULL), ShaderByteCodeSize(0),
			samplersChanged(0), texturesChanged(0)
		{
			if (Device)
			{
				Device->AddRef();
				if (explicitContext)
				{
					// Used for a deferred recording context: target the
					// caller-supplied (deferred) ID3D11DeviceContext instead
					// of the immediate one, so this bridge's cached state
					// (blend/rasterizer/depthstencil/sampler/shaders/
					// input layout) tracks the deferred context's own
					// history, not the immediate context's. Sharing one
					// CD3D11CallBridge between an immediate and a deferred
					// context would cause it to wrongly skip real D3D11
					// calls it believes are already applied.
					Context = explicitContext;
					Context->AddRef();
				}
				else
				{
					Device->GetImmediateContext(&Context);
				}
			}

			for (int i = 0; i < EST_COUNT; ++i)
				shaders[i] = NULL;

			ZeroMemory(CurrentTextures, sizeof(CurrentTextures[0]) * MATERIAL_MAX_TEXTURES);
			ZeroMemory(SamplerStates, sizeof(SamplerStates[0]) * MATERIAL_MAX_TEXTURES);

			applyInitialStates();
		}

		//! Pousse une premiere fois les etats caches vers le device.
		//!
		//! Le cache de ce bridge (DepthStencilDesc/BlendDesc/RasterizerDesc) demarre sur les valeurs
		//! de reset(), alors que le DEVICE, lui, demarre sur les valeurs par defaut de D3D11
		//! (DepthFunc = LESS, DepthWriteMask = ALL...). Sans cette premiere application, le cache
		//! MENT : les setters comparent le desc demande a une valeur que le device n'a jamais recue,
		//! concluent "deja applique" et sautent l'appel D3D11. Tout draw dont le materiau tombe pile
		//! sur les valeurs de reset() s'execute alors avec l'etat PAR DEFAUT du device.
		//!
		//! C'est exactement ce qui cassait les occlusion queries : le materiau de requete
		//! (profondeur activee, GREATER_EQUAL, ecriture desactivee) produit EXACTEMENT le desc de
		//! reset(), donc OMSetDepthStencilState n'etait jamais appele et la requete tournait avec le
		//! test LESS par defaut de D3D11 -- qui, en Z inverse (depth efface a 0.0), rejette tous les
		//! fragments : 0 pixel visible, quelle que soit la geometrie.
		void CD3D11CallBridge::applyInitialStates()
		{
			if (!Device || !Context)
				return;

			const SD3D11_DEPTH_STENCIL_DESC depthStencil;	// valeurs de reset()
			const SD3D11_BLEND_DESC blend;
			const SD3D11_RASTERIZER_DESC rasterizer;

			// Les setters court-circuitent quand le desc demande est egal au cache. On rend donc le
			// cache different de tout desc legitime, le temps de ces trois appels : ils passeront et
			// laisseront cache et device d'accord.
			memset(&DepthStencilDesc, 0xFF, sizeof(DepthStencilDesc));
			memset(&BlendDesc, 0xFF, sizeof(BlendDesc));
			memset(&RasterizerDesc, 0xFF, sizeof(RasterizerDesc));

			setDepthStencilState(depthStencil);
			setBlendState(blend);
			setRasterizerState(rasterizer);
		}

		CD3D11CallBridge::~CD3D11CallBridge()
		{
			// release blend states
			core::map<SD3D11_BLEND_DESC, ID3D11BlendState*>::Iterator bldIt = BlendMap.getIterator();
			while (!bldIt.atEnd())
			{
				if (bldIt->getValue())
					bldIt->getValue()->Release();
				bldIt++;
			}
			BlendMap.clear();

			// release rasterizer states
			core::map<SD3D11_RASTERIZER_DESC, ID3D11RasterizerState*>::Iterator rasIt = RasterizerMap.getIterator();
			while (!rasIt.atEnd())
			{
				if (rasIt->getValue())
					rasIt->getValue()->Release();
				rasIt++;
			}
			RasterizerMap.clear();

			// release depth stencil states
			core::map<SD3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState*>::Iterator dsIt = DepthStencilMap.getIterator();
			while (!dsIt.atEnd())
			{
				if (dsIt->getValue())
					dsIt->getValue()->Release();
				dsIt++;
			}
			DepthStencilMap.clear();

			// release sampler states
			core::map<SD3D11_SAMPLER_DESC, ID3D11SamplerState*>::Iterator samIt = SamplerMap.getIterator();
			while (!samIt.atEnd())
			{
				if (samIt->getValue())
					samIt->getValue()->Release();
				samIt++;
			}
			SamplerMap.clear();

			// release input states
			core::map<size_t, ID3D11InputLayout*>::Iterator layIt = LayoutMap.getIterator();
			while (!layIt.atEnd())
			{
				if (layIt->getValue())
					layIt->getValue()->Release();
				layIt++;
			}
			LayoutMap.clear();

			if (Context)
				Context->Release();

			if (Device)
				Device->Release();
		}
		///FUCK UP DE TEXTURE probablement lie au fait que lors qu'on met le shader  NULL on skipp de retirer les texture
		void CD3D11CallBridge::setVertexShader(SShader* shader)
		{
			if (shaders[EST_VERTEX_SHADER] != shader)
			{
				shaders[EST_VERTEX_SHADER] = shader;

				if (shader)
				{
					//os::Printer::log((irr::core::stringc("Setting VertexShader : ") += shader->Name).c_str(), irr::ELL_DEBUG);
					Context->VSSetShader((ID3D11VertexShader*)shader->shader, NULL, 0);

					const u32 size = shader->bufferArray.size();

					if (size != 0)
					{
						shader->RealocateBufferPointers();
						Context->VSSetConstantBuffers(0, size, &shader->buffs[0]);
					}
				}
				else
				{
					Context->VSSetShader(NULL, NULL, 0);

					//os::Printer::log((irr::core::stringc("Setting VertexShader : NULL ")).c_str(), irr::ELL_DEBUG);
				}
			}
			if (shader)
			{
				// only set samplers and textures if a shader is set and if samplers / textures are used, setted and changed
				u32 samplersToSet = shader->samplersUsed & samplersChanged;
				u32 texturesToSet = texturesChanged;

				//irr::core::stringc texchange = "Vertex texture changed : ";
				//texchange += texturesChanged;
				//texchange += " texture used : ";
				//texchange += shader->texturesUsed;
				//texchange += " texture to Set : ";
				//texchange += texturesToSet;
				if (samplersToSet || texturesToSet)
				{
					for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
					{
						if (samplersToSet & (1 << i))
							Context->VSSetSamplers(i, 1, &SamplerStates[i]);

						if (texturesChanged & (1 << i))
						{
							ID3D11ShaderResourceView* views = NULL;

							if (CurrentTextures[i])
								views = ((CD3D11Texture*)CurrentTextures[i])->getShaderResourceView();

							Context->VSSetShaderResources(i, 1, &views);
							//texchange += "\n texture Set : ";
							//texchange += i;
						}
					}
				}
				//os::Printer::log(texchange.c_str());
			}
		}

		void CD3D11CallBridge::setPixelShader(SShader* shader)
		{
			if (shaders[EST_PIXEL_SHADER] != shader)
			{
				shaders[EST_PIXEL_SHADER] = shader;

				if (shader)
				{
					//os::Printer::log((irr::core::stringc("Setting PixelShader : ") += shader->Name).c_str(), irr::ELL_DEBUG);
					Context->PSSetShader((ID3D11PixelShader*)shader->shader, NULL, 0);

					const u32 size = shader->bufferArray.size();

					if (size != 0)
					{
						shader->RealocateBufferPointers();
						Context->PSSetConstantBuffers(0, size, &shader->buffs[0]);
					}
				}
				else {
					Context->PSSetShader(NULL, NULL, 0);
					//os::Printer::log((irr::core::stringc("Setting Pixel : NULL ")).c_str(), irr::ELL_DEBUG);
				}
			}

			if (shader)
			{
				// only set samplers and textures if a shader is set and if samplers / textures are used, setted and changed
				u32 samplersToSet = samplersChanged;
				u32 texturesToSet = texturesChanged;

				//irr::core::stringc texchange = "Pixel texture changed : ";
				//texchange += texturesChanged;
				//texchange += " texture used : ";
				//texchange += shader->texturesUsed;
				//texchange += " texture to Set : ";
				//texchange += texturesToSet;

				if (samplersToSet || texturesToSet)
				{
					ID3D11ShaderResourceView* views[MATERIAL_MAX_TEXTURES];
					if (samplersToSet)
						Context->PSSetSamplers(0, MATERIAL_MAX_TEXTURES, &SamplerStates[0]);

					for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
					{
						if ((shader->texturesUsed | texturesChanged) & (1 << i) && CurrentTextures[i])
						{
							views[i] = ((CD3D11Texture*)CurrentTextures[i])->getShaderResourceView();

							//texchange += "\n texture Set : ";
							//texchange += i;
						}
						else
							views[i] = 0;
					}
					if (texturesToSet)
						Context->PSSetShaderResources(0, MATERIAL_MAX_TEXTURES, &views[0]);
				}
				//os::Printer::log(texchange.c_str());
			}
		}

		void CD3D11CallBridge::setGeometryShader(SShader* shader)
		{
			if (shaders[EST_GEOMETRY_SHADER] != shader)
			{
				shaders[EST_GEOMETRY_SHADER] = shader;

				if (shader)
				{
					//os::Printer::log((irr::core::stringc("Setting PixelShader : ") += shader->Name).c_str(), irr::ELL_DEBUG);
					Context->GSSetShader((ID3D11GeometryShader*)shader->shader, NULL, 0);

					const u32 size = shader->bufferArray.size();

					if (size != 0)
					{
						shader->RealocateBufferPointers();

						Context->GSSetConstantBuffers(0, size, &shader->buffs[0]);
					}
				}
				else
				{
					Context->GSSetShader(NULL, NULL, 0);

					//os::Printer::log((irr::core::stringc("Setting Pixel : NULL ")).c_str(), irr::ELL_DEBUG);
				}
			}

			if (shader)
			{
				// only set samplers and textures if a shader is set and if samplers / textures are used, setted and changed
				u32 samplersToSet = shader->samplersUsed & samplersChanged;
				u32 texturesToSet = shader->texturesUsed & texturesChanged;

				if (samplersToSet || texturesToSet)
				{
					for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
					{
						if (samplersToSet & (1 << i))
							Context->GSSetSamplers(i, 1, &SamplerStates[i]);

						if (texturesChanged & (1 << i))
						{
							ID3D11ShaderResourceView* views = NULL;

							if (CurrentTextures[i])
								views = ((CD3D11Texture*)CurrentTextures[i])->getShaderResourceView();

							Context->GSSetShaderResources(i, 1, &views);
						}
					}
				}
			}
		}

		void CD3D11CallBridge::setHullShader(SShader* shader)
		{
			const bool stageChanged = (shaders[EST_HULL_SHADER] != shader);
			if (stageChanged)
			{
				shaders[EST_HULL_SHADER] = shader;

				if (shader)
				{
					Context->HSSetShader((ID3D11HullShader*)shader->shader, NULL, 0);

					const u32 size = shader->bufferArray.size();

					if (size != 0)
					{
						shader->RealocateBufferPointers();
						Context->HSSetConstantBuffers(0, size, &shader->buffs[0]);
					}
				}
				else
					Context->HSSetShader(NULL, NULL, 0);
			}

			if (shader)
			{
				// Same newly-bound-stage rebind as setDomainShader -- see there.
				const u32 samplersToSet = stageChanged ? shader->samplersUsed : (shader->samplersUsed & samplersChanged);
				const u32 texturesToSet = stageChanged ? shader->texturesUsed : (shader->texturesUsed & texturesChanged);

				if (samplersToSet || texturesToSet)
				{
					for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
					{
						if (samplersToSet & (1 << i))
							Context->HSSetSamplers(i, 1, &SamplerStates[i]);

						if (texturesToSet & (1 << i))
						{
							ID3D11ShaderResourceView* views = NULL;

							if (CurrentTextures[i])
								views = ((CD3D11Texture*)CurrentTextures[i])->getShaderResourceView();

							Context->HSSetShaderResources(i, 1, &views);
						}
					}
				}
			}
		}

		void CD3D11CallBridge::setDomainShader(SShader* shader)
		{
			const bool stageChanged = (shaders[EST_DOMAIN_SHADER] != shader);
			if (stageChanged)
			{
				shaders[EST_DOMAIN_SHADER] = shader;

				if (shader)
				{
					Context->DSSetShader((ID3D11DomainShader*)shader->shader, NULL, 0);

					const u32 size = shader->bufferArray.size();

					if (size != 0)
					{
						shader->RealocateBufferPointers();
						Context->DSSetConstantBuffers(0, size, &shader->buffs[0]);
					}
				}
				else
					Context->DSSetShader(NULL, NULL, 0);
			}

			if (shader)
			{
				// texturesChanged tracks texture IDENTITY changes, not per-stage binding state, so a
				// newly bound stage would inherit nothing and sample zeros. Bind all it uses instead.
				const u32 samplersToSet = stageChanged ? shader->samplersUsed : (shader->samplersUsed & samplersChanged);
				const u32 texturesToSet = stageChanged ? shader->texturesUsed : (shader->texturesUsed & texturesChanged);

				if (samplersToSet || texturesToSet)
				{
					for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
					{
						if (samplersToSet & (1 << i))
							Context->DSSetSamplers(i, 1, &SamplerStates[i]);

						if (texturesToSet & (1 << i))
						{
							ID3D11ShaderResourceView* views = NULL;

							if (CurrentTextures[i])
								views = ((CD3D11Texture*)CurrentTextures[i])->getShaderResourceView();

							Context->DSSetShaderResources(i, 1, &views);
						}
					}
				}
			}
		}

		void CD3D11CallBridge::setComputeShader(SShader* shader)
		{
			if (shaders[EST_COMPUTE_SHADER] != shader)
			{
				shaders[EST_COMPUTE_SHADER] = shader;

				if (shader)
				{
					Context->CSSetShader((ID3D11ComputeShader*)shader->shader, NULL, 0);

					const u32 size = shader->bufferArray.size();

					if (size != 0)
					{
						shader->RealocateBufferPointers();

						Context->CSSetConstantBuffers(0, size, &shader->buffs[0]);
					}
				}
				else
					Context->CSSetShader(NULL, NULL, 0);
			}

			if (shader)
			{
				// only set samplers and textures if a shader is set and if samplers / textures are used, setted and changed
				u32 samplersToSet = shader->samplersUsed & samplersChanged;
				u32 texturesToSet = shader->texturesUsed & texturesChanged;

				if (samplersToSet || texturesToSet)
				{
					for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
					{
						if (samplersToSet & (1 << i))
							Context->CSSetSamplers(i, 1, &SamplerStates[i]);

						if (texturesChanged & (1 << i))
						{
							ID3D11ShaderResourceView* views = NULL;

							if (CurrentTextures[i])
								views = ((CD3D11Texture*)CurrentTextures[i])->getShaderResourceView();

							Context->CSSetShaderResources(i, 1, &views);
						}
					}
				}
			}
		}

		void CD3D11CallBridge::setDepthStencilState(const SD3D11_DEPTH_STENCIL_DESC& depthStencilDesc)
		{
			if (DepthStencilDesc != depthStencilDesc)
			{
				DepthStencilDesc = depthStencilDesc;
				ID3D11DepthStencilState* state = NULL;
				core::map<SD3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState*>::Node* dsIt = DepthStencilMap.find(DepthStencilDesc);

				if (dsIt)
				{
					state = dsIt->getValue();
				}
				else	// if not found, create and insert into map
				{
					HRESULT hr;
					if (SUCCEEDED(hr = Device->CreateDepthStencilState(&DepthStencilDesc, &state)))
					{
						DepthStencilMap.insert(DepthStencilDesc, state);
					}
					else
					{
						logFormatError(hr, "Could not create depth stencil state");

						return;
					}
				}
				Context->OMSetDepthStencilState(state, 1);
			}
		}

		void CD3D11CallBridge::setRasterizerState(const SD3D11_RASTERIZER_DESC& rasterizerDesc)
		{
			if (RasterizerDesc != rasterizerDesc)
			{
				RasterizerDesc = rasterizerDesc;

				// Rasterizer state
				ID3D11RasterizerState* state = 0;
				core::map<SD3D11_RASTERIZER_DESC, ID3D11RasterizerState*>::Node* rasIt = RasterizerMap.find(RasterizerDesc);
				if (rasIt)
				{
					state = rasIt->getValue();
				}
				else	// if not found, create and insert into map
				{
					HRESULT hr;
					if (SUCCEEDED(hr = Device->CreateRasterizerState(&RasterizerDesc, &state)))
					{
						RasterizerMap.insert(RasterizerDesc, state);
					}
					else
					{
						logFormatError(hr, "Could not create rasterizer state");

						return;
					}
				}

				Context->RSSetState(state);
			}
		}

		void CD3D11CallBridge::setBlendState(const SD3D11_BLEND_DESC& blendDesc)
		{
			if (BlendDesc != blendDesc)
			{
				BlendDesc = blendDesc;

				ID3D11BlendState* state = 0;
				core::map<SD3D11_BLEND_DESC, ID3D11BlendState*>::Node* bldIt = BlendMap.find(BlendDesc);

				if (bldIt)
				{
					state = bldIt->getValue();
				}
				else	// if not found, create and insert into map
				{
					HRESULT hr;
					if (SUCCEEDED(hr = Device->CreateBlendState(&BlendDesc, &state)))
					{
						BlendMap.insert(BlendDesc, state);
					}
					else
					{
						logFormatError(hr, "Could not create blend state");

						return;
					}
				}

				Context->OMSetBlendState(state, 0, 0xffffffff);
			}
		}

		void CD3D11CallBridge::setShaderResources(SD3D11_SAMPLER_DESC samplerDesc[MATERIAL_MAX_TEXTURES], ITexture* currentTextures[MATERIAL_MAX_TEXTURES])
		{
			texturesChanged = 0;
			samplersChanged = 0;

			for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
			{
				if (SamplerDesc[i] != samplerDesc[i])
				{
					SamplerDesc[i] = samplerDesc[i];

					SamplerStates[i] = getSamplerState(i);

					samplersChanged |= (1 << i);
				}

				if (CurrentTextures[i] != currentTextures[i])
				{
					CurrentTextures[i] = currentTextures[i];

					texturesChanged |= (1 << i);
				}
			}

			//irr::core::stringc texchange = "texture changed : ";
			//texchange += texturesChanged;
			//os::Printer::log(texchange.c_str());
		}

		void CD3D11CallBridge::invalidateTextureBinding(ITexture* texture)
		{
			if (!texture)
				return;

			// Binding a texture as a UAV makes D3D11 unbind it from every SRV slot, so the cached
			// binding here no longer matches the context and would suppress the next rebind.
			for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
			{
				if (CurrentTextures[i] == texture)
					CurrentTextures[i] = NULL;
			}
		}

		void CD3D11CallBridge::setPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
		{
			if (Topology != topology)
			{
				Topology = topology;

				Context->IASetPrimitiveTopology(Topology);
			}
		}

		void CD3D11CallBridge::setInputLayout(IVertexDescriptor* vtxDescriptor, IMaterialRenderer* r)
		{
			CD3D11MaterialRenderer* renderer = (CD3D11MaterialRenderer*)r;

			if (VtxDescriptor != vtxDescriptor || renderer->getShaderByteCode() != ShaderByteCode || renderer->getShaderByteCodeSize() != ShaderByteCodeSize)
			{
				VtxDescriptor = vtxDescriptor;
				ShaderByteCode = renderer->getShaderByteCode();
				ShaderByteCodeSize = renderer->getShaderByteCodeSize();

				ID3D11InputLayout* state = NULL;

				size_t signature = reinterpret_cast<size_t>(renderer->getShaderByteCode());

				core::map<size_t, ID3D11InputLayout*>::Node* layIt = LayoutMap.find(signature);
				if (layIt)
				{
					state = layIt->getValue();
				}
				else	// if not found, create and insert into layout
				{
					core::array<D3D11_INPUT_ELEMENT_DESC>& inputLayoutDesc = ((CD3D11VertexDescriptor*)VtxDescriptor)->getInputLayoutDescription();

					HRESULT hr;
					if (SUCCEEDED(hr = Device->CreateInputLayout(inputLayoutDesc.pointer(), inputLayoutDesc.size(),
						renderer->getShaderByteCode(), renderer->getShaderByteCodeSize(), &state)))
					{
						LayoutMap.insert(signature, state);
					}
					else
					{
						logFormatError(hr, "Could not create input layout");

						return;
					}
				}

				Context->IASetInputLayout(state);
			}
		}

		ID3D11SamplerState* CD3D11CallBridge::getSamplerState(u32 idx)
		{
			// Depth stencil state
			ID3D11SamplerState* state = NULL;
			core::map<SD3D11_SAMPLER_DESC, ID3D11SamplerState*>::Node* samIt = SamplerMap.find(SamplerDesc[idx]);
			if (samIt)
			{
				state = samIt->getValue();
			}
			else	// if not found, create and insert into map
			{
				HRESULT hr;
				if (SUCCEEDED(hr = Device->CreateSamplerState(&SamplerDesc[idx], &state)))
				{
					SamplerMap.insert(SamplerDesc[idx], state);
				}
				else
				{
					logFormatError(hr, "Could not create sampler state");

					return NULL;
				}
			}

			return state;
		}

		void CD3D11CallBridge::setViewPort(const core::rect<s32>& vp)
		{
			if (ViewPort != vp)
			{
				D3D11_VIEWPORT viewPort;
				viewPort.TopLeftX = (FLOAT)vp.UpperLeftCorner.X;
				viewPort.TopLeftY = (FLOAT)vp.UpperLeftCorner.Y;
				viewPort.Width = (FLOAT)vp.getWidth();
				viewPort.Height = (FLOAT)vp.getHeight();
				viewPort.MinDepth = 0.0f;
				viewPort.MaxDepth = 1.0f;

				Context->RSSetViewports(1, &viewPort);
				ViewPort = vp;
			}
		}

		void CD3D11CallBridge::invalidateCache()
		{
			// Forget everything, so the next setter for each category actually issues its D3D call.
			// Required after FinishCommandList/ExecuteCommandList, which change the real context
			// state without going through any setter here -- a cache that still claims those values
			// makes the next set a no-op and the draw runs with nothing bound.
			for (int i = 0; i < EST_COUNT; ++i)
				shaders[i] = NULL;

			memset(&DepthStencilDesc, 0xFF, sizeof(DepthStencilDesc));
			memset(&BlendDesc, 0xFF, sizeof(BlendDesc));
			memset(&RasterizerDesc, 0xFF, sizeof(RasterizerDesc));
			memset(&SamplerDesc, 0xFF, sizeof(SamplerDesc));

			ZeroMemory(CurrentTextures, sizeof(CurrentTextures[0]) * MATERIAL_MAX_TEXTURES);
			ZeroMemory(SamplerStates, sizeof(SamplerStates[0]) * MATERIAL_MAX_TEXTURES);
			texturesChanged = 0;
			samplersChanged = 0;

			InputLayout = NULL;
			VtxDescriptor = NULL;
			ShaderByteCode = NULL;
			ShaderByteCodeSize = 0;
			Topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
			ViewPort = core::rect<s32>(0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF);
		}

		void CD3D11CallBridge::forceReapplyAll()
		{
			if (!Device || !Context)
				return;

			// Same invalidate-then-set trick as applyInitialStates(), reapplying our own cache instead of defaults.
			{
				const SD3D11_DEPTH_STENCIL_DESC depthStencil = DepthStencilDesc;
				const SD3D11_BLEND_DESC blend = BlendDesc;
				const SD3D11_RASTERIZER_DESC rasterizer = RasterizerDesc;

				memset(&DepthStencilDesc, 0xFF, sizeof(DepthStencilDesc));
				memset(&BlendDesc, 0xFF, sizeof(BlendDesc));
				memset(&RasterizerDesc, 0xFF, sizeof(RasterizerDesc));

				setDepthStencilState(depthStencil);
				setBlendState(blend);
				setRasterizerState(rasterizer);
			}

			// Same trick via an out-of-range sentinel -- ViewPort has no bit pattern guaranteed invalid.
			{
				const core::rect<s32> vp = ViewPort;
				ViewPort = core::rect<s32>(0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF);
				setViewPort(vp);
			}

			// Topology -- unconditional, no cached device call to skip re-issuing.
			Context->IASetPrimitiveTopology(Topology);

			// Re-derive setInputLayout()'s signature (the bytecode pointer) -- no IMaterialRenderer* here.
			if (ShaderByteCode)
			{
				size_t signature = reinterpret_cast<size_t>(ShaderByteCode);
				core::map<size_t, ID3D11InputLayout*>::Node* layIt = LayoutMap.find(signature);
				if (layIt)
					Context->IASetInputLayout(layIt->getValue());
			}

			// Shaders/textures/samplers deliberately NOT force-reapplied here (tried, reverted --
			// caused a real device-removal regression, root cause not yet pinned down). Every real
			// draw already calls setXShader(theShaderItWants) explicitly, and the existing cache
			// check only wrongly skips when a NEW draw wants the exact same pointer already
			// cached -- a much narrower, non-crashing gap than what force-reapplying introduced.
		}
	}
}

#endif