// Copyright (C) 2002-2009 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "CD3D11Driver.h"
#include "CD3D11Texture.h"
#include "os.h"

#include "CImage.h"
#include "CColorConverter.h"
#include "DDSTextureLoader.h"
#include <iostream>

namespace irr
{
	namespace video
	{
		//! rendertarget constructor
		CD3D11Texture::CD3D11Texture(CD3D11Driver* driver, const core::dimension2d<u32>& size,
			const io::path& name, const ECOLOR_FORMAT format, u32 arraySlices,
			u32 sampleCount, u32 sampleQuality, bool unorderedAccess)
			: ITexture(name), Texture(0), TextureBuffer(0),
			Device(0), Context(0), Driver(driver),
			RTView(0), SRView(0), UAView(0),
			TextureDimension(D3D11_RESOURCE_DIMENSION_TEXTURE2D),
			MipLevelLocked(0), NumberOfMipLevels(0), ArraySliceLocked(0), NumberOfArraySlices(arraySlices),
			SampleCount(sampleCount), SampleQuality(sampleQuality),
			LastMapDirection((D3D11_MAP)0), dsView(0),
			HardwareMipMaps(false)

		{
#ifdef _DEBUG
			setDebugName("CD3D11Texture");
#endif
			TextureType = arraySlices > 1 ? ETT_2D_ARRAY : ETT_2D;
			DriverType = EDT_DIRECT3D11;
			OriginalSize = size;
			Size = size;
			IsRenderTarget = true;
			IsUnorderedAccess = unorderedAccess;
			MipMaps = false;

			Device = driver->getExposedVideoData().D3D11.D3DDev11;
			if (Device)
			{
				Device->AddRef();
				// Stays the IMMEDIATE context: lock() maps a staging copy, which is only legal
				// there. The initial upload picks its own context -- see copyTexture().
				Device->GetImmediateContext(&Context);
			}

			createRenderTarget(format);
		}

		//! constructor
		CD3D11Texture::CD3D11Texture(IImage* image, CD3D11Driver* driver,
			u32 flags, const io::path& name, u32 arraySlices, void* mipmapData)
			: ITexture(name), Texture(0), TextureBuffer(0),
			Device(0), Context(0), Driver(driver),
			RTView(0), SRView(0), UAView(0),
			TextureDimension(D3D11_RESOURCE_DIMENSION_TEXTURE2D),
			LastMapDirection((D3D11_MAP)0), dsView(0), MipLevelLocked(0), NumberOfMipLevels(0),
			ArraySliceLocked(0), NumberOfArraySlices(arraySlices), SampleCount(1), SampleQuality(0),
			HardwareMipMaps(false)
		{
#ifdef _DEBUG
			setDebugName("CD3D11Texture");
#endif
			TextureType = ETT_2D;

			DriverType = EDT_DIRECT3D11;
			MipMaps = Driver->getTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS);
			OriginalSize = image->getDimension();
			IsRenderTarget = false;
			Device = driver->getExposedVideoData().D3D11.D3DDev11;
			if (Device)
			{
				Device->AddRef();
				// Stays the IMMEDIATE context: lock() maps a staging copy, which is only legal
				// there. The initial upload picks its own context -- see copyTexture().
				Device->GetImmediateContext(&Context);
			}

			// A raw .dds file: block-compressed, a wide uncompressed format, a cube map, an array or
			// a volume. The loader hands over the whole file (see CImageLoaderDDS) and
			// DDSTextureLoader parses it, creating the resource with every mip, face and slice, plus
			// its shader resource view. A plain 2D uncompressed .dds arrives as pixels like any other
			// image and takes the createTexture() path below.
			if (image && image->isCompressed())
			{
				CreateDDSTextureFromMemory(
					Device,
					(byte*)image->lock(),
					((CImage*)image)->CompressedSize,
					&Texture,
					&SRView,
					0
				);
				image->unlock();

				// CreateDDSTextureFromMemory leaves Texture untouched on failure.
				if (!Texture)
				{
					os::Printer::log("Could not load .dds texture", name, ELL_ERROR);
					return;
				}

				Texture->GetType(&TextureDimension);
				DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
				if (TextureDimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
				{
					D3D11_TEXTURE3D_DESC desc;
					((ID3D11Texture3D*)Texture)->GetDesc(&desc);
					NumberOfMipLevels = desc.MipLevels;
					Size.Width = desc.Width;
					Size.Height = desc.Height;
					NumberOfArraySlices = 1;
					TextureType = ETT_3D;
					format = desc.Format;
				}
				else
				{
					D3D11_TEXTURE2D_DESC desc;
					((ID3D11Texture2D*)Texture)->GetDesc(&desc);
					NumberOfMipLevels = desc.MipLevels;
					Size.Width = desc.Width;
					Size.Height = desc.Height;
					NumberOfArraySlices = desc.ArraySize;
					const bool cube = (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
					TextureType = cube ? ((desc.ArraySize > 6) ? ETT_CUBE_ARRAY : ETT_CUBE) :
						((desc.ArraySize > 1) ? ETT_2D_ARRAY : ETT_2D);
					format = desc.Format;
				}
				OriginalSize = Size;
				MipMaps = NumberOfMipLevels > 1;
				HardwareMipMaps = false;

				// The loader's format wins where DXGI cannot tell: DXT2/DXT4 (premultiplied alpha)
				// share BC2/BC3 with DXT3/DXT5.
				ColorFormat = Driver->getColorFormatFromD3DFormat(format);
				const ECOLOR_FORMAT loaded = image->getColorFormat();
				if (loaded == ECF_DXT2 || loaded == ECF_DXT4)
					ColorFormat = loaded;
				HasAlpha = IImage::hasAlphaFormat(ColorFormat);

				// This path bypasses createTexture(), which is what normally sets Pitch.
				setPitch(format);
			}
			else if (image)
			{
				Size = image->getDimension();
				if (createTexture(flags, image))
				{
					if (!image->isCompressed() && copyTexture(image))
						regenerateMipMapLevels(mipmapData);
				}
				else
					os::Printer::log("Could not create Direct3D11 Texture.", ELL_WARNING);
			}
		}
		CD3D11Texture::CD3D11Texture(const core::array<ITexture*>* surfaces, CD3D11Driver* driver,
			u32 flags, const io::path& name, E_TEXTURE_TYPE Type, u32 arraySlices, void* mipmapData)
			: ITexture(name), Texture(0), TextureBuffer(0),
			Device(0), Context(0), Driver(driver),
			RTView(0), SRView(0), UAView(0),
			TextureDimension(D3D11_RESOURCE_DIMENSION_TEXTURE2D),
			LastMapDirection((D3D11_MAP)0), dsView(0), MipLevelLocked(0), NumberOfMipLevels(0),
			ArraySliceLocked(0), NumberOfArraySlices(arraySlices), SampleCount(1), SampleQuality(0), HardwareMipMaps(false)
		{
			DriverType = EDT_DIRECT3D11;
			OriginalSize = surfaces->operator[](0)->getOriginalSize();
			IsRenderTarget = false;
#ifdef _DEBUG
			setDebugName("CD3D11Texture");
#endif
			TextureType = Type;

			MipMaps = Driver->getTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS);

			Device = driver->getExposedVideoData().D3D11.D3DDev11;

			ColorFormat = (*surfaces)[0]->getColorFormat();
			if (!((CD3D11Texture*)(*surfaces)[0])->HardwareMipMaps)
				NumberOfMipLevels = ((CD3D11Texture*)(*surfaces)[0])->NumberOfMipLevels;

			if (Device)
			{
				Device->AddRef();
				// Stays the IMMEDIATE context: lock() maps a staging copy, which is only legal
				// there. The initial upload picks its own context -- see copyTexture().
				Device->GetImmediateContext(&Context);
			}

			// Load a dds file
			if (core::hasFileExtension(name, "dds"))
			{
			}

			if (surfaces)
			{
				Size = surfaces->operator[](0)->getSize();
				{
					core::stringc m = "CD3D11Texture[array]: begin ";
					m += core::stringc(Size.Width); m += "x"; m += core::stringc(Size.Height);
					m += " slices="; m += core::stringc((u32)surfaces->size());
					m += " mips="; m += core::stringc(NumberOfMipLevels);
					os::Printer::log(m.c_str(), ELL_DEBUG);
				}
				if (createTexture(flags, 0))
				{
					os::Printer::log("CD3D11Texture[array]: main texture created", ELL_DEBUG);
					D3D11_TEXTURE2D_DESC desc;
					((ID3D11Texture2D*)Texture)->GetDesc(&desc);
					desc.BindFlags = 0;
					desc.Usage = D3D11_USAGE_STAGING;
					desc.MiscFlags = 0;
					desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

					HRESULT hr = Device->CreateTexture2D(&desc, NULL, (ID3D11Texture2D**)&TextureBuffer);
					if (FAILED(hr))
					{
						logFormatError(hr, "Could not create texture buffer");

						return;
					}

					os::Printer::log("CD3D11Texture[array]: staging buffer created", ELL_DEBUG);

					// sync main texture contents with texture buffer
					Context->CopyResource(TextureBuffer, Texture);

					os::Printer::log("CD3D11Texture[array]: staging primed, begin slice copy", ELL_DEBUG);

					for (int i = 0; i < surfaces->size(); ++i)
					{
						if ((i % 25) == 0)
							os::Printer::log("CD3D11Texture[array]: slice", core::stringc(i), ELL_DEBUG);
						//copyTexture(surfaces->operator[](i), i);

						HRESULT hr = S_OK;
						D3D11_MAPPED_SUBRESOURCE mappedData;

						const u32 bpp = irr::video::IImage::getBitsPerPixelFromFormat(ColorFormat) / 8;

						u32 pitch = Size.Width * Size.Height * bpp;
						CD3D11Texture* stex = (CD3D11Texture*)surfaces->operator[](i);
						if ((*surfaces)[i]->hasMipMaps() && !stex->HardwareMipMaps)
						{
							for (int k = 0; k < stex->NumberOfMipLevels; ++k)
							{
								MapArraySlice(hr, k, i, mappedData, D3D11_MAP_WRITE, TextureBuffer);
								if (mappedData.pData)
								{
									auto lockedData = stex->lock(ETLM_READ_WRITE, k);
									memcpy(mappedData.pData, lockedData, min(mappedData.DepthPitch, stex->DPitch));
									stex->unlock();
								}

								if (!mappedData.pData)
								{
									os::Printer::log("Could not Copy Direct3D11 Texture.", ELL_WARNING);
									logFormatError(hr, "Could not map texture buffer");
								}
								Context->Unmap(TextureBuffer, D3D11CalcSubresource(k, i, NumberOfMipLevels));
							}
						}

						// copy texture buffer to main texture ONLY if buffer was write
					}
					os::Printer::log("CD3D11Texture[array]: slice copy done, copying back", ELL_DEBUG);
					Context->CopyResource(Texture, TextureBuffer);
					TextureBuffer->Release();
					TextureBuffer = NULL;

					os::Printer::log("CD3D11Texture[array]: regenerating mipmaps", ELL_DEBUG);
					regenerateMipMapLevels(mipmapData);
					os::Printer::log("CD3D11Texture[array]: done", ELL_DEBUG);
				}
				else
					os::Printer::log("Could not create Direct3D11 Texture.", ELL_WARNING);
			}
		}

		//! destructor
		CD3D11Texture::~CD3D11Texture()
		{
			if (dsView)
			{
				dsView->Release();
			}

			if (RTView)
				RTView->Release();

			for (u32 i = 0; i < SliceRTViews.size(); ++i)
				if (SliceRTViews[i])
					SliceRTViews[i]->Release();
			SliceRTViews.clear();

			if (SRView)
				SRView->Release();

			if (ResolvedSRView)
				ResolvedSRView->Release();

			if (ResolvedTexture)
				ResolvedTexture->Release();

			if (UAView)
				UAView->Release();

			if (Texture)
				Texture->Release();

			if (TextureBuffer)
				TextureBuffer->Release();

			if (Context)
				Context->Release();

			if (Device)
				Device->Release();
		}

		//! return render target view
		ID3D11RenderTargetView* CD3D11Texture::getRenderTargetView() const
		{
			return RTView;
		}

		// Rendering into a slice, rather than copying into it, makes the source format irrelevant.
		ID3D11RenderTargetView* CD3D11Texture::getRenderTargetView(u32 arraySlice)
		{
			if (arraySlice >= NumberOfArraySlices || !Texture || !Device)
				return 0;

			if (SliceRTViews.size() < NumberOfArraySlices)
			{
				const u32 had = SliceRTViews.size();
				SliceRTViews.set_used(NumberOfArraySlices);
				for (u32 i = had; i < NumberOfArraySlices; ++i)
					SliceRTViews[i] = 0;
			}

			if (!SliceRTViews[arraySlice])
			{
				D3D11_TEXTURE2D_DESC desc;
				((ID3D11Texture2D*)Texture)->GetDesc(&desc);

				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
				ZeroMemory(&rtvDesc, sizeof(rtvDesc));
				rtvDesc.Format = desc.Format;
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
				rtvDesc.Texture2DArray.MipSlice = 0;
				rtvDesc.Texture2DArray.FirstArraySlice = arraySlice;
				rtvDesc.Texture2DArray.ArraySize = 1;

				if (FAILED(Device->CreateRenderTargetView(Texture, &rtvDesc, &SliceRTViews[arraySlice])))
					return 0;
			}

			return SliceRTViews[arraySlice];
		}

		//! return shader resource view
		ID3D11ShaderResourceView* CD3D11Texture::getShaderResourceView() const
		{
			// A multisampled target is read through its resolved twin, refreshed on every bind:
			// there is no "changed since the last resolve" tracking, as on the D3D12 side.
			if (ResolvedSRView && ResolvedTexture)
			{
				Context->ResolveSubresource(ResolvedTexture, 0, Texture, 0, ResolveFormat);
				return ResolvedSRView;
			}

			// Emulate "auto" mipmap generation
			if (IsRenderTarget && SRView && MipMaps)
				Context->GenerateMips(SRView);

			return SRView;
		}

		//! return unordered access view (compute-writable textures only, see IsUnorderedAccess)
		ID3D11UnorderedAccessView* CD3D11Texture::getUnorderedAccessView() const
		{
			return UAView;
		}

		//! lock function
		void* CD3D11Texture::lock(E_TEXTURE_LOCK_MODE mode, u32 mipmapLevel)
		{
			bool ronly;
			if (mode == ETLM_READ_ONLY)
				ronly = true;
			else
				ronly = false;
			return lock(ronly, mipmapLevel, 0);
		}

		void* CD3D11Texture::lock(bool readOnly, u32 mipmapLevel, u32 arraySlice)
		{
			if (!Texture || !createTextureBuffer())
				return 0;

			HRESULT hr = S_OK;

			// Record mip level locked to use in unlock
			MipLevelLocked = mipmapLevel;
			ArraySliceLocked = arraySlice;

			// set map direction
			if (readOnly)
				LastMapDirection = D3D11_MAP_READ;
			else
				LastMapDirection = (D3D11_MAP)(D3D11_MAP_READ | D3D11_MAP_WRITE);

			// if read, and this is a render target texture (i.ex.: GPU will write data to texture)
			// shall sync data from main texture to texture buffer
			if ((IsRenderTarget == true) && (LastMapDirection & D3D11_MAP_READ))
			{
				Context->CopyResource(TextureBuffer, Texture);
			}
			if (TextureType == ETT_3D)
			{
				D3D11_MAPPED_SUBRESOURCE mappedData;
				ZeroMemory(&mappedData, sizeof(D3D11_MAPPED_SUBRESOURCE));

				hr = Context->Map(TextureBuffer,
					D3D11CalcSubresource(0,		// mip level to lock
						0,		// array slice (only 1 slice for now)
						NumberOfMipLevels),
					LastMapDirection, 							// direction to map
					0,
					&mappedData);								// mapped result

				Pitch = mappedData.RowPitch;
				return ((char*)mappedData.pData) + mappedData.DepthPitch * arraySlice;
			}
			// Map texture buffer
			D3D11_MAPPED_SUBRESOURCE mappedData;
			MapArraySlice(hr, mipmapLevel, arraySlice, mappedData, LastMapDirection, TextureBuffer);							// mapped result

			if (FAILED(hr))
			{
				// print every single variable to find out what is wrong

				logFormatError(hr, "Could not map texture buffer");

				if (hr == DXGI_ERROR_DEVICE_REMOVED)
				{

					HRESULT hremove = S_OK;
					hremove = Device->GetDeviceRemovedReason();
					logFormatError(hremove, "Device removed reason");
				}
				return NULL;
			}

			Pitch = mappedData.RowPitch;
			DPitch = mappedData.DepthPitch;
			return mappedData.pData;
		}

		void CD3D11Texture::MapArraySlice(HRESULT& hr, const irr::u32& mipmapLevel, const irr::u32& arraySlice, D3D11_MAPPED_SUBRESOURCE& mappedData, D3D11_MAP MapDirection, ID3D11Resource* LocalTextureBuffer)
		{
				hr = Context->Map(LocalTextureBuffer,
				D3D11CalcSubresource(mipmapLevel,		// mip level to lock
					arraySlice,		// array slice (only 1 slice for now)
					NumberOfMipLevels), 	// number of mip levels
				MapDirection, 							// direction to map
				0,
				&mappedData);
		}

		//! unlock function
		void CD3D11Texture::unlock()
		{
			if (!Texture)
				return;

			// unlock texture buffer
			if (TextureType == ETT_3D)
			{
				Context->Unmap(TextureBuffer, 0);
			}
			else if (TextureBuffer != NULL)
			{
				Context->Unmap(TextureBuffer, D3D11CalcSubresource(MipLevelLocked, ArraySliceLocked, NumberOfMipLevels));
			}
			// copy texture buffer to main texture ONLY if buffer was write
			if (LastMapDirection & D3D11_MAP_WRITE)
			{
				Context->CopyResource(Texture, TextureBuffer);
			}
			TextureBuffer->Release();
			TextureBuffer = NULL;
		}

		u32 CD3D11Texture::getNumberOfArraySlices() const
		{
			return NumberOfArraySlices;
		}

		//! Regenerates the mip map levels of the texture. Useful after locking and
		//! modifying the texture
		void CD3D11Texture::regenerateMipMapLevels(void* mipmapData)
		{
			// Runs straight after the upload, so it follows the same rule: the creating driver's
			// context, not the immediate one. GenerateMips is legal on a deferred context, and
			// using the immediate one here would race it from the recording thread.
			ID3D11DeviceContext* mipContext = Driver ? Driver->getContext() : Context;
			if (SRView && HardwareMipMaps && mipContext)
				mipContext->GenerateMips(SRView);
		}

		void CD3D11Texture::createRenderTarget(const ECOLOR_FORMAT format)
		{
			HRESULT hr = S_OK;

			// are texture size restrictions there ?
			if (!Driver->queryFeature(EVDF_TEXTURE_NPOT))
			{
				if (Size != OriginalSize)
					os::Printer::log("RenderTarget size has to be a power of two", ELL_INFORMATION);
			}

			Size = Size.getOptimalSize(!Driver->queryFeature(EVDF_TEXTURE_NPOT), !Driver->queryFeature(EVDF_TEXTURE_NSQUARE), true, Driver->getMaxTextureSize().Width);

			DXGI_FORMAT d3dformat = Driver->getD3DColorFormat();

			if (ColorFormat == ECF_UNKNOWN)
			{
				// get irrlicht format from backbuffer
				// (This will get overwritten by the custom format if it is provided, else kept.)
				ColorFormat = Driver->getColorFormat();
				setPitch(d3dformat);

				// Use color format if provided.
				if (format != ECF_UNKNOWN)
				{
					ColorFormat = format;
					d3dformat = Driver->getD3DFormatFromColorFormat(format);
					setPitch(d3dformat); // This will likely set pitch to 0 for now.
				}
			}
			else
			{
				d3dformat = Driver->getD3DFormatFromColorFormat(ColorFormat);
			}

			if (d3dformat == DXGI_FORMAT_UNKNOWN)
			{
				// get irrlicht format from backbuffer
				// (This will get overwritten by the custom format if it is provided, else kept.)
				ColorFormat = Driver->getColorFormat();
				setPitch(d3dformat);

				// Use color format if provided.
				if (ColorFormat != ECF_UNKNOWN)
				{
					d3dformat = Driver->getD3DFormatFromColorFormat(ColorFormat);
					setPitch(d3dformat); // This will likely set pitch to 0 for now.
				}
				else
				{
					d3dformat = DXGI_FORMAT_R8G8B8A8_UNORM;
				}
			}
			irr::u32 bindflags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			if (IsUnorderedAccess)
				bindflags |= D3D11_BIND_UNORDERED_ACCESS;

			switch (ColorFormat)
			{
			case ECF_A8R8G8B8:
			case ECF_A1R5G5B5:
			case ECF_A16B16G16R16F:
			case ECF_A32B32G32R32F:
				HasAlpha = true;
				break;
			case ECF_D16:
			case ECF_D32:
			case ECF_D24S8:
			case ECF_DF32S8:
				bindflags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
				break;
			default:
				break;
			}

			// creating texture
			D3D11_TEXTURE2D_DESC desc;
			ZeroMemory(&desc, sizeof(D3D11_TEXTURE2D_DESC));
			desc.ArraySize = NumberOfArraySlices;
			desc.CPUAccessFlags = 0;
			desc.Format = d3dformat;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.SampleDesc.Count = SampleCount;
			desc.SampleDesc.Quality = SampleQuality;
			desc.BindFlags = bindflags;

			{
				desc.MiscFlags = 0;
				desc.MipLevels = 1;
				MipMaps = 0;
			}

			// If array size == 6, force cube texture
			if (desc.ArraySize == 6)
			{
				desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
			}

			// If multisampled, mip levels shall be 1
			//if (desc.SampleDesc.Count > 1)
			{
				desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
				desc.MipLevels = 1;
			}

			// Texture size
			desc.Width = Size.Width;
			desc.Height = Size.Height;

			// create texture
			hr = Device->CreateTexture2D(&desc, NULL, (ID3D11Texture2D**)&Texture);
			if (FAILED(hr))
			{
				logFormatError(hr, "Could not create render target texture");

				return;
			}

			// Get texture description to update some fields
			((ID3D11Texture2D*)Texture)->GetDesc(&desc);
			NumberOfMipLevels = desc.MipLevels;
			Size.Width = desc.Width;
			Size.Height = desc.Height;

			// create views
			createViews();
		}

		//! creates the hardware texture
		bool CD3D11Texture::createTexture(u32 flags, IImage* image)
		{
			HRESULT hr = S_OK;
			if (image)
				OriginalSize = image->getDimension();

			core::dimension2d<u32> optSize = OriginalSize.getOptimalSize(!Driver->queryFeature(EVDF_TEXTURE_NPOT),
				!Driver->queryFeature(EVDF_TEXTURE_NSQUARE),
				true,
				Driver->getMaxTextureSize().Width);

			DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;

			// Color format for DX 10 driver shall be different that for DX 9
			// - B5G5R5A1 family is deprecated in DXGI, and doesn't exists in DX 10
			// - Irrlicht color format follows DX 9 (alpha first), and DX 10 is alpha last
			if (image)
			{
				format = Driver->getD3DFormatFromColorFormat(image->getColorFormat());
			}
			else if (ColorFormat != ECF_UNKNOWN)
				format = Driver->getD3DFormatFromColorFormat(ColorFormat);
			else
			{
				switch (getTextureFormatFromFlags(flags))
				{
				case ETCF_ALWAYS_16_BIT:
				case ETCF_ALWAYS_32_BIT:
					format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
					break;
				case ETCF_OPTIMIZED_FOR_SPEED:
					format = DXGI_FORMAT_R8G8B8A8_UNORM;
					break;
				default:
					break;
				}
			}

			if (((!isRenderTarget()) || (!isDeptStencil())) && (format == DXGI_FORMAT_R8G8B8A8_UNORM))
			{
				format = DXGI_FORMAT_B8G8R8A8_UNORM;
			}
			// Check hardware support for automatic mipmap support
			if (MipMaps && Driver->queryFeature(EVDF_MIP_MAP_AUTO_UPDATE))
			{
				UINT support = 0;
				Device->CheckFormatSupport(format, &support);

				if (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)
					HardwareMipMaps = !IImage::isCompressedFormat(Driver->getColorFormatFromD3DFormat(format));
			}

			if (TextureType == ETT_2D || TextureType == ETT_2D_ARRAY || TextureType == ETT_CUBE || TextureType == ETT_CUBE_ARRAY)
			{
				D3D11_TEXTURE2D_DESC desc;
				ZeroMemory(&desc, sizeof(D3D11_TEXTURE2D_DESC));
				desc.ArraySize = NumberOfArraySlices;
				desc.CPUAccessFlags = 0;
				desc.Format = format;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.SampleDesc.Count = SampleCount;
				desc.SampleDesc.Quality = SampleQuality;
				desc.Width = optSize.Width;
				desc.Height = optSize.Height;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

				// test if Direct3D support automatic mip map generation
				// AND creation flag is true
				if (MipMaps && image && image->hasMipMaps())
				{
					desc.MipLevels = image->getMipMapsCount();
				}
				else if (NumberOfMipLevels != 0)
				{
					desc.MipLevels = NumberOfMipLevels;
				}
				// Honour the creation flag: without it this auto-mipped every texture, forcing
				// MipLevels 0, which forbids initial data and puts the upload back on a context.
				else if (MipMaps && Driver->querySupportForColorFormat(format, D3D11_FORMAT_SUPPORT_MIP_AUTOGEN))
				{
					desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
					desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
					desc.MipLevels = 0;
				}
				else
				{
					desc.MipLevels = 1;		// Set only one mip level if do not support auto mip generation
				}

				// If array size == 6, force cube texture
				if (TextureType == ETT_CUBE)
				{
					desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
				}

				// If multisampled, mip levels shall be 1
				if (desc.SampleDesc.Count > 1)
				{
					desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
					desc.MipLevels = 1;
				}

				// Hand the pixels to CreateTexture2D when we can: it is a DEVICE call, free-threaded
				// and context-free, so a texture created while recording never touches the immediate
				// context. Only possible with an explicit mip count -- MipLevels 0 (auto-gen) forbids
				// initial data -- so anything auto-mipped still falls through to copyTexture().
				core::array<u8> initialPixels;
				D3D11_SUBRESOURCE_DATA initialData;
				D3D11_SUBRESOURCE_DATA* initialDataPtr = NULL;
				if (image && desc.MipLevels == 1 && !image->isCompressedFormat(image->getColorFormat()))
				{
					const ECOLOR_FORMAT dstFormat = Driver->getColorFormatFromD3DFormat(format);
					const u32 bpp = IImage::getBitsPerPixelFromFormat(dstFormat) / 8;
					if (bpp)
					{
						const u32 rowPitch = desc.Width * bpp;
						initialPixels.reallocate(rowPitch * desc.Height);
						initialPixels.set_used(rowPitch * desc.Height);
						image->copyToScaling(initialPixels.pointer(), desc.Width, desc.Height, dstFormat, rowPitch);

						initialData.pSysMem = initialPixels.pointer();
						initialData.SysMemPitch = rowPitch;
						initialData.SysMemSlicePitch = 0;
						initialDataPtr = &initialData;
						UploadedAtCreation = true;
					}
				}

				// create texture
				hr = Device->CreateTexture2D(&desc, initialDataPtr, (ID3D11Texture2D**)&Texture);
				if (FAILED(hr))
				{
					logFormatError(hr, "Could not create texture");

					return false;
				}

				((ID3D11Texture2D*)Texture)->GetDesc(&desc);
				NumberOfMipLevels = desc.MipLevels;
				Size.Width = desc.Width;
				Size.Height = desc.Height;
			}
			else if (TextureType == ETT_3D || TextureType == ETT_3D_ARRAY)
			{
				D3D11_TEXTURE3D_DESC desc;
				ZeroMemory(&desc, sizeof(D3D11_TEXTURE3D_DESC));
				desc.Depth = NumberOfArraySlices;
				desc.CPUAccessFlags = 0;
				desc.Format = format;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.Width = optSize.Width;
				desc.Height = optSize.Height;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

				// test if Direct3D support automatic mip map generation
				// AND creation flag is true
				if (MipMaps && Driver->querySupportForColorFormat(format, D3D11_FORMAT_SUPPORT_MIP_AUTOGEN))
				{
					desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
					desc.MipLevels = 0;
				}
				else
				{
					desc.MipLevels = 1;		// Set only one mip level if do not support auto mip generation
				}

				// create texture
				hr = Device->CreateTexture3D(&desc, NULL, (ID3D11Texture3D**)&Texture);
				if (FAILED(hr))
				{
					logFormatError(hr, "Could not create texture");

					return false;
				}

				((ID3D11Texture3D*)Texture)->GetDesc(&desc);
				NumberOfMipLevels = desc.MipLevels;
				Size.Width = desc.Width;
				Size.Height = desc.Height;
			}

			// get color format
			ColorFormat = Driver->getColorFormatFromD3DFormat(format);
			HasAlpha = IImage::hasAlphaFormat(ColorFormat);

			setPitch(format);

			// create views to bound texture to pipeline
			return createViews();
		}

		//! copies the image to the texture
		bool CD3D11Texture::copyTexture(IImage* image)
		{
			// Uncompressed uploads go through UpdateSubresource instead of the staging round-trip
			// below. lock() maps a STAGING copy, which a deferred context is not allowed to do (it
			// may only Map DYNAMIC resources), so the old path could never be recorded into a
			// command list. This texture is USAGE_DEFAULT, which UpdateSubresource accepts on the
			// immediate and deferred context alike -- letting a texture be uploaded while recording.
			// Already uploaded by CreateTexture2D, which needs no context at all.
			if (UploadedAtCreation)
				return true;

			// An UpdateSubresource path on the creating driver's context was tried here and
			// REVERTED: it recorded the upload into the command list, so pixels a caller wrote
			// straight afterwards via lock() were overwritten when the list replayed.
			void* ptr = lock();
			if (ptr && !image->isCompressedFormat(image->getColorFormat()))
				image->copyToScaling(ptr, Size.Width, Size.Height, ColorFormat, Pitch);
			else if (ptr && image->isCompressedFormat(image->getColorFormat()))
			{
				void* imgptr = image->lock();

				u32 lwidth = image->getDimension().Width;

				u32 lpitch = lwidth * lwidth;

				if (image->hasMipMaps())
				{
					for (int k = 0; k < image->getMipMapsCount(); ++k)
					{
						unlock();
						ptr = lock(ETLM_READ_WRITE, k);
						for (int j = 0; j < (DPitch / lpitch); ++j)
						{
							memcpy(ptr, imgptr, lpitch);
							ptr = &(((c8*)ptr)[lpitch]);
						}

						//imgptr = &((c8*)imgptr)[lpitch];

						irr::core::stringc sizewarning = "Current DPitch : ";
						sizewarning += DPitch;
						sizewarning += " Current pitch : ";
						sizewarning += Pitch;
						sizewarning += " Current Mip : ";
						sizewarning += k;
						sizewarning += " Calculated dpitch: ";
						sizewarning += lpitch;
						sizewarning += " Calculated pitch: ";
						sizewarning += lwidth;
						os::Printer::log(sizewarning.c_str());

						if (lwidth > 1)
							lwidth >>= 1;
						if ((lwidth * lwidth) < 16)
						{
							lpitch = 16;
							os::Printer::log(" this mip is probably broken");
						}
						else
							lpitch = lwidth * lwidth;

						imgptr = ((CImage*)image)->Mips[lwidth];
					}
				}

				image->unlock();
			}
			unlock();

			return true;
		}

		//! copies the texture to the texture layer
		bool CD3D11Texture::copyTexture(ITexture* image, int layer)
		{
			void* ptr = lock(0, 0, layer);
			const u32 bpp = irr::video::IImage::getBitsPerPixelFromFormat(ColorFormat) / 8;
			if (ptr)
			{
				memcpy(ptr, image->lock(), Size.Width * Size.Height * bpp);// image->copyToScaling(ptr, Size.Width, Size.Height, ColorFormat, Pitch);
				image->unlock();
			}

			unlock();

			return true;
		}

		//! Bytes per 4x4 block, or 0 if the format is not block compressed.
		static u32 getBlockBytes(DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_BC1_TYPELESS:
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC4_TYPELESS:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC4_SNORM:
				return 8;

			case DXGI_FORMAT_BC2_TYPELESS:
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_TYPELESS:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC5_TYPELESS:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC5_SNORM:
			case DXGI_FORMAT_BC6H_TYPELESS:
			case DXGI_FORMAT_BC6H_UF16:
			case DXGI_FORMAT_BC6H_SF16:
			case DXGI_FORMAT_BC7_TYPELESS:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				return 16;

			default:
				return 0;
			}
		}

		void CD3D11Texture::setPitch(DXGI_FORMAT d3dformat)
		{
			// getBitsPerPixel returns BITS; a pitch is bytes per row. Block-compressed
			// formats are one pitch per 4-row block, not per pixel row.
			const u32 blockBytes = getBlockBytes(d3dformat);
			if (blockBytes)
				Pitch = ((Size.Width + 3) / 4) * blockBytes;
			else
				Pitch = (Driver->getBitsPerPixel(d3dformat) * Size.Width) / 8;
		}

		bool CD3D11Texture::createTextureBuffer()
		{
			if (!Texture)
			{
				os::Printer::log("Error creating texture buffer: main texture is null", ELL_ERROR);
				return false;
			}

			if (TextureBuffer == NULL && (TextureType == ETT_2D || TextureType == ETT_2D_ARRAY || TextureType == ETT_CUBE || TextureType == ETT_CUBE_ARRAY))
			{
				D3D11_TEXTURE2D_DESC desc;
				((ID3D11Texture2D*)Texture)->GetDesc(&desc);

				desc.BindFlags = 0;
				desc.Usage = D3D11_USAGE_STAGING;
				desc.MiscFlags = 0;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

				HRESULT hr = Device->CreateTexture2D(&desc, NULL, (ID3D11Texture2D**)&TextureBuffer);
				if (FAILED(hr))
				{
					logFormatError(hr, "Could not create texture buffer");

					return false;
				}

				// sync main texture contents with texture buffer
				Context->CopyResource(TextureBuffer, Texture);
			}
			else if (TextureBuffer == NULL && (TextureType == ETT_3D || TextureType == ETT_3D_ARRAY))
			{
				D3D11_TEXTURE3D_DESC desc;
				((ID3D11Texture3D*)Texture)->GetDesc(&desc);

				desc.BindFlags = 0;
				desc.Usage = D3D11_USAGE_STAGING;
				desc.MiscFlags = 0;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

				HRESULT hr = Device->CreateTexture3D(&desc, NULL, (ID3D11Texture3D**)&TextureBuffer);
				if (FAILED(hr))
				{
					logFormatError(hr, "Could not create texture buffer");

					return false;
				}

				// sync main texture contents with texture buffer
				Context->CopyResource(TextureBuffer, Texture);
			}

			return true;
		}

		bool CD3D11Texture::createViews()
		{
			if (!Texture)
				return false;

			HRESULT hr = S_OK;
			DXGI_FORMAT format = Driver->getD3DFormatFromColorFormat(ColorFormat);

			if (ColorFormat == ECF_DF32S8 || ColorFormat == ECF_D16 || ColorFormat == ECF_D32 || ColorFormat == ECF_D24S8)
			{
				IsDepthStencil = true;

				DXGI_FORMAT dformat = format;

				switch (ColorFormat)
				{
				case ECF_DF32S8:
					dformat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
					break;
				case ECF_D32:
					dformat = DXGI_FORMAT_D32_FLOAT;
					break;
				case ECF_D24S8:
					dformat = DXGI_FORMAT_D24_UNORM_S8_UINT;
					break;
				case ECF_D16:
					dformat = DXGI_FORMAT_D16_UNORM;
					break;
				default:
					break;
				}

				D3D11_DEPTH_STENCIL_VIEW_DESC dsDesc;
				::ZeroMemory(&dsDesc, sizeof(dsDesc));
				dsDesc.Format = dformat;
				dsDesc.Flags = 0;
				dsDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
				dsDesc.Texture2D.MipSlice = 0;
				if (TextureType == ETT_2D_ARRAY)
				{
					dsDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
					dsDesc.Texture2DArray.ArraySize = this->NumberOfArraySlices;
				}
				// A multisampled depth buffer (the pooled partner of a multisampled render target)
				// needs the MS view dimension, or the view is refused and the null view crashes
				// the next ClearDepthStencilView().
				if (SampleCount > 1)
				{
					if (TextureType == ETT_2D_ARRAY)
					{
						dsDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
						dsDesc.Texture2DMSArray.FirstArraySlice = 0;
						dsDesc.Texture2DMSArray.ArraySize = this->NumberOfArraySlices;
					}
					else
						dsDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
				}
				hr = Device->CreateDepthStencilView(Texture, &dsDesc, &dsView);

				if (FAILED(hr))
				{
					logFormatError(hr, "Could not create depth stencil view");

					return NULL;
				}
				switch (ColorFormat)
				{
				case ECF_DF32S8:
					format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
					break;
				case ECF_D32:
					format = DXGI_FORMAT_R32_FLOAT;
					break;
				case ECF_D24S8:
					format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
					break;
				case ECF_D16:
					format = DXGI_FORMAT_R16_FLOAT;
					break;
				default:
					break;
				}
				IsRenderTarget = false;
			}
			else if (IsRenderTarget)
			{
				if (RTView)
					RTView->Release();

				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc;
				::ZeroMemory(&rtvDesc, sizeof(rtvDesc));
				rtvDesc.Format = format;

				// check if texture is array and/or multisampled
				if (SampleCount > 1 && NumberOfArraySlices > 1)		// multisampled array
				{
					rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
					rtvDesc.Texture2DMSArray.ArraySize = NumberOfArraySlices;
					rtvDesc.Texture2DMSArray.FirstArraySlice = 0;
				}
				else if (SampleCount > 1)	// only multisampled
				{
					rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
				}
				else if (NumberOfArraySlices > 1)	// only array
				{
					rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
					rtvDesc.Texture2DArray.ArraySize = NumberOfArraySlices;
					rtvDesc.Texture2DArray.FirstArraySlice = 0;
					rtvDesc.Texture2DArray.MipSlice = 0;
				}
				else	// simple texture
				{
					rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
					rtvDesc.Texture2D.MipSlice = 0;
				}

				hr = Device->CreateRenderTargetView(Texture, &rtvDesc, &RTView);
				if (FAILED(hr))
				{
					logFormatError(hr, "Could not create render target view");

					return false;
				}
			}
			else if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
			{
				format = DXGI_FORMAT_B8G8R8A8_UNORM;
			}

			// A multisampled colour target cannot be sampled as a Texture2D: it gets a single-sample
			// twin that getShaderResourceView() resolves into on every shader bind, the explicit
			// resolve CD3D12Texture::resolveIfNeeded() and the Vulkan resolve attachment perform.
			if (ResolvedSRView)
			{
				ResolvedSRView->Release();
				ResolvedSRView = 0;
			}
			if (ResolvedTexture)
			{
				ResolvedTexture->Release();
				ResolvedTexture = 0;
			}
			if (IsRenderTarget && SampleCount > 1 && NumberOfArraySlices == 1 && TextureType == ETT_2D)
			{
				D3D11_TEXTURE2D_DESC msDesc;
				((ID3D11Texture2D*)Texture)->GetDesc(&msDesc);
				D3D11_TEXTURE2D_DESC resolvedDesc = msDesc;
				resolvedDesc.SampleDesc.Count = 1;
				resolvedDesc.SampleDesc.Quality = 0;
				resolvedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				resolvedDesc.MiscFlags = 0;
				resolvedDesc.MipLevels = 1;
				ResolveFormat = msDesc.Format;

				hr = Device->CreateTexture2D(&resolvedDesc, NULL, &ResolvedTexture);
				if (SUCCEEDED(hr))
				{
					D3D11_SHADER_RESOURCE_VIEW_DESC resolvedSrv;
					::ZeroMemory(&resolvedSrv, sizeof(resolvedSrv));
					resolvedSrv.Format = format;
					resolvedSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					resolvedSrv.Texture2D.MipLevels = 1;
					hr = Device->CreateShaderResourceView(ResolvedTexture, &resolvedSrv, &ResolvedSRView);
				}
				if (FAILED(hr))
				{
					logFormatError(hr, "Could not create the resolve texture of a multisampled render target");
					if (ResolvedSRView)
						ResolvedSRView->Release();
					if (ResolvedTexture)
						ResolvedTexture->Release();
					ResolvedSRView = 0;
					ResolvedTexture = 0;
				}
			}

			// create shader resource view
			if (SRView)
				SRView->Release();

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
			::ZeroMemory(&srvDesc, sizeof(srvDesc));
			srvDesc.Format = format;
			if (TextureType == ETT_2D || TextureType == ETT_2D_ARRAY || TextureType == ETT_CUBE || TextureType == ETT_CUBE_ARRAY)
			{
				// check if texture is array and/or multisampled
				if (SampleCount > 1 && NumberOfArraySlices > 1)		// multisampled array
				{
					srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
					srvDesc.Texture2DMSArray.ArraySize = NumberOfArraySlices;
				}
				else if (SampleCount > 1)	// only multisampled
				{
					srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
				}
				else if (NumberOfArraySlices > 1)	// only array
				{
					srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
					srvDesc.Texture2DArray.ArraySize = NumberOfArraySlices;
					srvDesc.Texture2DArray.FirstArraySlice = 0;
					srvDesc.Texture2DArray.MipLevels = NumberOfMipLevels;
					srvDesc.Texture2DArray.MostDetailedMip = 0;
				}
				else	// simple texture
				{
					srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					srvDesc.Texture2D.MipLevels = NumberOfMipLevels;
					srvDesc.Texture2D.MostDetailedMip = 0;
				}
			}
			else if (TextureType == ETT_3D || TextureType == ETT_3D_ARRAY)
			{
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
				srvDesc.Texture3D.MipLevels = NumberOfMipLevels;
				srvDesc.Texture3D.MostDetailedMip = 0;
			}
			hr = Device->CreateShaderResourceView(Texture, &srvDesc, &SRView);
			if (FAILED(hr))
			{
				logFormatError(hr, "Could not create shader resource view : " + NumberOfMipLevels);

				return false;
			}

			// create unordered access view (compute-writable textures only, e.g. FFT displacement)
			if (IsUnorderedAccess)
			{
				if (UAView)
					UAView->Release();

				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc;
				::ZeroMemory(&uavDesc, sizeof(uavDesc));
				uavDesc.Format = format;
				if (NumberOfArraySlices > 1)
				{
					uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
					uavDesc.Texture2DArray.ArraySize = NumberOfArraySlices;
					uavDesc.Texture2DArray.FirstArraySlice = 0;
					uavDesc.Texture2DArray.MipSlice = 0;
				}
				else
				{
					uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
					uavDesc.Texture2D.MipSlice = 0;
				}

				hr = Device->CreateUnorderedAccessView(Texture, &uavDesc, &UAView);
				if (FAILED(hr))
				{
					logFormatError(hr, "Could not create unordered access view");

					return false;
				}
			}

			return true;
		}
	}
}

#endif;