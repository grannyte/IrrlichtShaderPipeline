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

namespace irr
{
	namespace video
	{
		//! rendertarget constructor
		CD3D11Texture::CD3D11Texture(CD3D11Driver* driver, const core::dimension2d<u32>& size,
			const io::path& name, const ECOLOR_FORMAT format, u32 arraySlices,
			u32 sampleCount, u32 sampleQuality)
			: ITexture(name), Texture(0), TextureBuffer(0),
			Device(0), Context(0), Driver(driver),
			RTView(0), SRView(0),
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
			MipMaps = false;

			Device = driver->getExposedVideoData().D3D11.D3DDev11;
			if (Device)
			{
				Device->AddRef();
				Device->GetImmediateContext(&Context);
			}

			createRenderTarget(format);
		}

		//! constructor
		CD3D11Texture::CD3D11Texture(IImage* image, CD3D11Driver* driver,
			u32 flags, const io::path& name, u32 arraySlices, void* mipmapData)
			: ITexture(name), Texture(0), TextureBuffer(0),
			Device(0), Context(0), Driver(driver),
			RTView(0), SRView(0),
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
				Device->GetImmediateContext(&Context);
			}

			// Load a dds file
			if (core::hasFileExtension(name, "dds") && image)
			{
				CreateDDSTextureFromMemory(
					Device,
					(byte*)image->lock(),
					((CImage*)image)->CompressedSize,
					&Texture,
					&SRView,
					0
				);

				D3D11_TEXTURE2D_DESC desc;
				((ID3D11Texture2D*)Texture)->GetDesc(&desc);
				NumberOfMipLevels = desc.MipLevels;
				Size.Width = desc.Width;
				Size.Height = desc.Height;
				NumberOfMipLevels = desc.MipLevels;
				MipMaps = NumberOfMipLevels > 1;
				HardwareMipMaps = false;

				// get color format
				ColorFormat = Driver->getColorFormatFromD3DFormat(desc.Format);
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
			RTView(0), SRView(0),
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
				Device->GetImmediateContext(&Context);
			}

			// Load a dds file
			if (core::hasFileExtension(name, "dds"))
			{
			}

			if (surfaces)
			{
				Size = surfaces->operator[](0)->getSize();
				if (createTexture(flags, 0))
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

						return;
					}

					// sync main texture contents with texture buffer
					Context->CopyResource(TextureBuffer, Texture);

					for (int i = 0; i < surfaces->size(); ++i)
					{
						//copyTexture(surfaces->operator[](i), i);

						HRESULT hr = S_OK;
						D3D11_MAPPED_SUBRESOURCE mappedData;

						const u32 bpp = irr::video::IImage::getBitsPerPixelFromFormat(ColorFormat) / 8;

						u32 pitch = Size.Width * Size.Height * bpp;

						u32 lwidth = Size.Width;
						u32 lheight = Size.Height;

						if ((*surfaces)[i]->hasMipMaps() && !((CD3D11Texture*)(*surfaces)[i])->HardwareMipMaps)
						{
							for (int k = 0; k < ((CD3D11Texture*)(*surfaces)[i])->NumberOfMipLevels; ++k)
							{
								MapArraySlice(hr, k, i, mappedData, D3D11_MAP_WRITE, TextureBuffer);
								if (mappedData.pData)
									memcpy(mappedData.pData, surfaces->operator[](i)->lock(ETLM_READ_WRITE, k), mappedData.DepthPitch);// image->copyToScaling(ptr, Size.Width, Size.Height, ColorFormat, Pitch);
								surfaces->operator[](i)->unlock();
								lwidth /= 2;
								lheight /= 2;
								pitch = lwidth * lwidth * bpp;

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
					Context->CopyResource(Texture, TextureBuffer);
					TextureBuffer->Release();
					TextureBuffer = NULL;

					regenerateMipMapLevels(mipmapData);
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

			if (SRView)
				SRView->Release();

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

		//! return shader resource view
		ID3D11ShaderResourceView* CD3D11Texture::getShaderResourceView() const
		{
			// Emulate "auto" mipmap generation
			if (IsRenderTarget && SRView && MipMaps)
				Context->GenerateMips(SRView);

			return SRView;
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
				logFormatError(hr, "Could not map texture buffer");

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
			if (SRView && HardwareMipMaps)
				Context->GenerateMips(SRView);
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
				else if (Driver->querySupportForColorFormat(format, D3D11_FORMAT_SUPPORT_MIP_AUTOGEN))
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

				// create texture
				hr = Device->CreateTexture2D(&desc, NULL, (ID3D11Texture2D**)&Texture);
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

			switch (ColorFormat)
			{
			case ECF_A8R8G8B8:
			case ECF_A1R5G5B5:
			case ECF_DXT1:
			case ECF_DXT2:
			case ECF_DXT3:
			case ECF_DXT4:
			case ECF_DXT5:
			case ECF_BC7_U:
			case ECF_BC7_S:
			case ECF_A16B16G16R16F:
			case ECF_A32B32G32R32F:
				HasAlpha = true;
				break;
			default:
				break;
			}

			setPitch(format);

			// create views to bound texture to pipeline
			return createViews();
		}

		//! copies the image to the texture
		bool CD3D11Texture::copyTexture(IImage* image)
		{
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

		void CD3D11Texture::setPitch(DXGI_FORMAT d3dformat)
		{
			Pitch = Driver->getBitsPerPixel(d3dformat) * Size.Width;
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

			return true;
		}
	}
}

#endif;