// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CD3D12Texture.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_
#include "CD3D12Driver.h"
#include "DDSTextureLoader12.h"
#include "CImage.h"
#include "coreutil.h"
#include <vector>

namespace irr
{
	namespace video
	{
		DXGI_FORMAT getD3D12ColorFormat(ECOLOR_FORMAT format)
		{
			switch (format)
			{
				// Returns R8G8B8A8_UNORM for ECF_A8R8G8B8; not final for an ordinary
				// texture, applyNonRenderTargetChannelOrder() below swaps it to BGRA.
				case ECF_A8R8G8B8:       return DXGI_FORMAT_R8G8B8A8_UNORM;
				case ECF_A1R5G5B5:       return DXGI_FORMAT_B5G5R5A1_UNORM;
				case ECF_R5G6B5:         return DXGI_FORMAT_B5G6R5_UNORM;
				case ECF_R16F:           return DXGI_FORMAT_R16_FLOAT;
				case ECF_G16R16F:        return DXGI_FORMAT_R16G16_FLOAT;
				case ECF_A16B16G16R16F:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
				case ECF_R32F:           return DXGI_FORMAT_R32_FLOAT;
				case ECF_G32R32F:        return DXGI_FORMAT_R32G32_FLOAT;
				case ECF_A32B32G32R32F:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
				// R,G,B memory order matches DXGI_FORMAT_R32G32B32_FLOAT directly, no
				// conversion needed on upload (unlike ECF_R8G8B8 below).
				case ECF_B32G32R32F:     return DXGI_FORMAT_R32G32B32_FLOAT;
				// No 24-bit DXGI equivalent: promoted to R8G8B8A8_UNORM (alpha=0xFF on
				// upload). Caller must update ColorFormat to ECF_A8R8G8B8 accordingly.
				case ECF_R8G8B8:         return DXGI_FORMAT_R8G8B8A8_UNORM;
				// Block-compressed formats. ECF_DXT2/4 (premultiplied alpha) share DXT3/5's
				// DXGI format; there's no dedicated one for them.
				case ECF_DXT1:           return DXGI_FORMAT_BC1_UNORM;
				case ECF_DXT1_SRGB:      return DXGI_FORMAT_BC1_UNORM_SRGB;
				case ECF_DXT2:
				case ECF_DXT3:           return DXGI_FORMAT_BC2_UNORM;
				case ECF_DXT3_SRGB:      return DXGI_FORMAT_BC2_UNORM_SRGB;
				case ECF_DXT4:
				case ECF_DXT5:           return DXGI_FORMAT_BC3_UNORM;
				case ECF_DXT5_SRGB:      return DXGI_FORMAT_BC3_UNORM_SRGB;
				case ECF_BC4_U:          return DXGI_FORMAT_BC4_UNORM;
				case ECF_BC4_S:          return DXGI_FORMAT_BC4_SNORM;
				case ECF_BC5_U:          return DXGI_FORMAT_BC5_UNORM;
				case ECF_BC5_S:          return DXGI_FORMAT_BC5_SNORM;
				case ECF_BC6_U:          return DXGI_FORMAT_BC6H_UF16;
				case ECF_BC6_S:          return DXGI_FORMAT_BC6H_SF16;
				case ECF_BC7_U:          return DXGI_FORMAT_BC7_UNORM;
				case ECF_BC7_S:          return DXGI_FORMAT_BC7_UNORM_SRGB;
				// Extended engine formats used by the deferred renderer's G-buffer.
				case ECF_A8R8G8B8S:      return DXGI_FORMAT_R8G8B8A8_SNORM;
				case ECF_R8:             return DXGI_FORMAT_R8_UNORM;
				case ECF_R8S:            return DXGI_FORMAT_R8_SNORM;
				case ECF_R8G8:           return DXGI_FORMAT_R8G8_UNORM;
				case ECF_R16:            return DXGI_FORMAT_R16_UNORM;
				case ECF_R16G16:         return DXGI_FORMAT_R16G16_UNORM;
				// Depth formats. A depth/stencil resource is created typeless and viewed via
				// a DSV (typed) + SRV (read format); this table returns the view's typed format.
				case ECF_D16:            return DXGI_FORMAT_D16_UNORM;
				case ECF_D32:            return DXGI_FORMAT_D32_FLOAT;
				case ECF_D24S8:          return DXGI_FORMAT_D24_UNORM_S8_UINT;
				case ECF_DF32S8:         return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
				default:
					// R10G10B10A2/R11G11B10_FLOAT/R9G9B9E5_SHAREDEXP have no corresponding
					// ECOLOR_FORMAT value in SColor.h.
					os::Printer::log("getD3D12ColorFormat: unsupported ECOLOR_FORMAT", ELL_ERROR);
				return DXGI_FORMAT_UNKNOWN;
			}
		}

		//! A non-render-target texture goes from R8G8B8A8_UNORM to B8G8R8A8_UNORM.
		//! A render target texture keeps R8G8B8A8_UNORM, like the back buffer.
		//!
		//! This is the channel-order convention engine content is written against:
		//!   - an ordinary texture arrives at the shader in true RGB order (ECF_A8R8G8B8
		//!     is 0xAARRGGBB, i.e. B,G,R,A in memory, and B8G8R8A8_UNORM reads byte 0 as
		//!     blue); no shader swizzles a texture.
		//!   - the vertex color attribute stays R8G8B8A8_UNORM, so it arrives at the
		//!     shader with R and B swapped - hence the ".bgra" every engine shader that
		//!     reads vertex color carries (IrrRocketRenderer, Star, noise2...). The
		//!     built-in shaders (CD3D12DefaultShaders.h) don't need it.
		DXGI_FORMAT applyNonRenderTargetChannelOrder(DXGI_FORMAT format)
		{
			return (format == DXGI_FORMAT_R8G8B8A8_UNORM) ? DXGI_FORMAT_B8G8R8A8_UNORM : format;
		}

		//! True if `format` is a depth/stencil format. Such textures need
		//! ALLOW_DEPTH_STENCIL, not ALLOW_RENDER_TARGET (D3D12 rejects that
		//! combination), a DepthStencil clear value, and a DSV instead of an RTV.
		bool isDepthFormat(DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				return true;
			default:
				return false;
			}
		}

		//! Typeless format for a resource that needs both a DSV (typed) and an SRV
		//! (read format) - a resource created directly as e.g. D32_FLOAT can't carry an SRV.
		DXGI_FORMAT getTypelessDepthFormat(DXGI_FORMAT depthFormat)
		{
			switch (depthFormat)
			{
			case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_TYPELESS;
			case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_TYPELESS;
			case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24G8_TYPELESS;
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
			default:                               return depthFormat;
			}
		}

		//! SRV read format matching a depth format; stencil, if present, isn't exposed.
		DXGI_FORMAT getDepthSRVFormat(DXGI_FORMAT depthFormat)
		{
			switch (depthFormat)
			{
			case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_UNORM;
			case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			default:                               return depthFormat;
			}
		}

		//! True if the device can use `format` as a render target. Queries the device
		//! instead of hard-coding a list, since support varies by hardware. Used to
		//! decide whether a mip chain is possible, and to avoid requesting
		//! ALLOW_RENDER_TARGET on a format that doesn't support it (e.g. R32G32B32_FLOAT,
		//! which would fail CreateCommittedResource).
		static bool supportsRenderTarget(CD3D12Driver* driver, DXGI_FORMAT format)
		{
			if (!driver || format == DXGI_FORMAT_UNKNOWN)
				return false;
			ID3D12Device2* device = driver->getDevice();
			if (!device)
				return false;

			D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
			support.Format = format;
			if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
				return false;

			return (support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0;
		}

		//! Reverse mapping restricted to formats DDSTextureLoader12 can produce, used
		//! only to populate ITexture::ColorFormat for the .dds loading path. An
		//! "exotic" uncompressed .dds format falls back to ECF_UNKNOWN.
		static ECOLOR_FORMAT getColorFormatFromD3D12Format(DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_BC1_TYPELESS:
			case DXGI_FORMAT_BC1_UNORM:       return ECF_DXT1;
			case DXGI_FORMAT_BC1_UNORM_SRGB:  return ECF_DXT1_SRGB;
			case DXGI_FORMAT_BC2_TYPELESS:
			case DXGI_FORMAT_BC2_UNORM:       return ECF_DXT3;
			case DXGI_FORMAT_BC2_UNORM_SRGB:  return ECF_DXT3_SRGB;
			case DXGI_FORMAT_BC3_TYPELESS:
			case DXGI_FORMAT_BC3_UNORM:       return ECF_DXT5;
			case DXGI_FORMAT_BC3_UNORM_SRGB:  return ECF_DXT5_SRGB;
			case DXGI_FORMAT_BC4_TYPELESS:
			case DXGI_FORMAT_BC4_UNORM:       return ECF_BC4_U;
			case DXGI_FORMAT_BC4_SNORM:       return ECF_BC4_S;
			case DXGI_FORMAT_BC5_TYPELESS:
			case DXGI_FORMAT_BC5_UNORM:       return ECF_BC5_U;
			case DXGI_FORMAT_BC5_SNORM:       return ECF_BC5_S;
			case DXGI_FORMAT_BC6H_TYPELESS:
			case DXGI_FORMAT_BC6H_UF16:       return ECF_BC6_U;
			case DXGI_FORMAT_BC6H_SF16:       return ECF_BC6_S;
			case DXGI_FORMAT_BC7_TYPELESS:
			case DXGI_FORMAT_BC7_UNORM:       return ECF_BC7_U;
			case DXGI_FORMAT_BC7_UNORM_SRGB:  return ECF_BC7_S;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM:  return ECF_A8R8G8B8;
			case DXGI_FORMAT_R8G8B8A8_SNORM:  return ECF_A8R8G8B8S;
			case DXGI_FORMAT_B5G5R5A1_UNORM:  return ECF_A1R5G5B5;
			case DXGI_FORMAT_B5G6R5_UNORM:    return ECF_R5G6B5;
			case DXGI_FORMAT_R8_UNORM:        return ECF_R8;
			case DXGI_FORMAT_R8_SNORM:        return ECF_R8S;
			case DXGI_FORMAT_R8G8_UNORM:      return ECF_R8G8;
			case DXGI_FORMAT_R16_UNORM:       return ECF_R16;
			case DXGI_FORMAT_R16G16_UNORM:    return ECF_R16G16;
			case DXGI_FORMAT_R16_FLOAT:       return ECF_R16F;
			case DXGI_FORMAT_R16G16_FLOAT:    return ECF_G16R16F;
			case DXGI_FORMAT_R16G16B16A16_FLOAT: return ECF_A16B16G16R16F;
			case DXGI_FORMAT_R32_FLOAT:       return ECF_R32F;
			case DXGI_FORMAT_R32G32_FLOAT:    return ECF_G32R32F;
			case DXGI_FORMAT_R32G32B32A32_FLOAT: return ECF_A32B32G32R32F;
			default:
				return ECF_UNKNOWN;
			}
		}

		// ------------------------------------------------------------------------------

		CD3D12Texture::CD3D12Texture(IImage* image, CD3D12Driver* driver, u32 flags, const io::path& name)
			: ITexture(name), Driver(driver)
		{
			DriverType = EDT_DIRECT3D12;
			TextureType = ETT_2D;
			Source = ETS_UNKNOWN;

			if (!image)
			{
				os::Printer::log("CD3D12Texture: image nulle", ELL_ERROR);
				return;
			}

			// A raw .dds file (block-compressed, wide format, cube map, array or volume) bypasses the
			// ECOLOR_FORMAT/uploadInitialData() path below: the IImage holds the raw .dds bytes
			// (header included, see CImageLoaderDDS), and DDSTextureLoader12 parses the header and
			// loads mips/slices/faces itself. A plain 2D uncompressed .dds arrives as pixels.
			if (image->isCompressed())
			{
				void* rawDdsBytes = image->lock();
				SDDSTexture12Result ddsResult;
				const bool ddsOk = rawDdsBytes && CreateDDSTextureFromMemory12(driver,
					static_cast<const unsigned char*>(rawDdsBytes),
					static_cast<CImage*>(image)->CompressedSize, ddsResult);
				image->unlock();

				if (!ddsOk)
				{
					os::Printer::log("CD3D12Texture: echec du chargement .dds", ELL_ERROR);
					return;
				}

				Resource = ddsResult.Resource;
				DxgiFormat = ddsResult.Format;
				OriginalSize = Size = core::dimension2d<u32>(ddsResult.Width, ddsResult.Height);
				MipLevelCount = ddsResult.MipLevels;
				MipMaps = MipLevelCount > 1;
				NumberOfArraySlices = ddsResult.ArraySize;
				CurrentState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				TextureType = ddsResult.IsCubeMap ?
					((ddsResult.ArraySize > 6) ? ETT_CUBE_ARRAY : ETT_CUBE) :
					(ddsResult.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? ETT_3D :
					((ddsResult.ArraySize > 1) ? ETT_2D_ARRAY : ETT_2D);
				// ColorFormat becomes ECF_UNKNOWN for an exotic DXGI .dds format with no
				// matching ECOLOR_FORMAT; the GPU texture stays valid and sampleable.
				// Pitch is left unset here: meaningless for a block-compressed format
				// and recomputed by lock() anyway. The loader's format wins where DXGI
				// cannot tell: DXT2/DXT4 (premultiplied alpha) share BC2/BC3 with DXT3/DXT5.
				ColorFormat = getColorFormatFromD3D12Format(DxgiFormat);
				const ECOLOR_FORMAT loaded = image->getColorFormat();
				if (loaded == ECF_DXT2 || loaded == ECF_DXT4)
					ColorFormat = loaded;
				HasAlpha = IImage::hasAlphaFormat(ColorFormat);

				createShaderResourceView();
				return;
			}

			OriginalSize = Size = image->getDimension();
			const ECOLOR_FORMAT sourceFormat = image->getColorFormat();
			DxgiFormat = getD3D12ColorFormat(sourceFormat);
			HasAlpha = (sourceFormat == ECF_A8R8G8B8 || sourceFormat == ECF_A1R5G5B5 ||
				sourceFormat == ECF_A16B16G16R16F || sourceFormat == ECF_A32B32G32R32F);

			if (DxgiFormat == DXGI_FORMAT_UNKNOWN)
				return; // error already logged by getD3D12ColorFormat

			// ECF_R8G8B8 is promoted to DXGI_FORMAT_R8G8B8A8_UNORM (no 24-bit DXGI
			// equivalent): the public ColorFormat reflects this actual 4-bytes-per-pixel storage, same as
			// CD3D11Texture::createTexture() does by re-reading ColorFormat from the chosen DXGI format.
			const bool expandR8G8B8 = (sourceFormat == ECF_R8G8B8 && DxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM);
			ColorFormat = expandR8G8B8 ? ECF_A8R8G8B8 : sourceFormat;

			// Non-render-target texture: applied after expandR8G8B8, which tests the
			// pre-correction format.
			DxgiFormat = applyNonRenderTargetChannelOrder(DxgiFormat);

			// Reads Driver->getTextureCreationFlag(), not this constructor's flags
			// parameter (unused).
			//
			// Only enabled if the format can actually carry a mip chain: generateMips()
			// blits via an RTV per mip, requiring ALLOW_RENDER_TARGET. Not all formats
			// support that (e.g. R32G32B32_FLOAT, on any hardware), and requesting it
			// anyway fails CreateCommittedResource. Query the device instead of assuming.
			MipMaps = Driver->getTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS)
				&& supportsRenderTarget(Driver, DxgiFormat);
			MipLevelCount = MipMaps ? computeMipLevels(Size.Width, Size.Height) : 1;

			if (!createResource(false))
				return;

			void* pixels = image->lock();
			if (pixels)
			{
				uploadInitialData(pixels, image->getPitch(), expandR8G8B8);
				image->unlock();
			}

			Pitch = expandR8G8B8 ? Size.Width * 4 : image->getPitch();

			if (MipMaps && MipLevelCount > 1)
				generateMips();
		}

		CD3D12Texture::CD3D12Texture(CD3D12Driver* driver, const core::dimension2d<u32>& size,
			const io::path& name, ECOLOR_FORMAT format, bool renderTarget,
			u32 sampleCount, u32 sampleQuality, u32 arraySlices, bool unorderedAccess)
			: ITexture(name), Driver(driver)
		{
			if (unorderedAccess && renderTarget)
				os::Printer::log("CD3D12Texture: unorderedAccess ignore (renderTarget=true)", ELL_WARNING);
			IsUnorderedAccess = unorderedAccess && !renderTarget;
			DriverType = EDT_DIRECT3D12;
			// arraySlices > 1 only applies to a render-target-texture; a non-RT texture
			// stays ETT_2D. arraySlices > 1 combined with sampleCount > 1 is already
			// rejected upstream by CD3D12Driver::addRenderTargetTexture().
			NumberOfArraySlices = (renderTarget && arraySlices > 1) ? arraySlices : 1;
			TextureType = (NumberOfArraySlices > 1) ? ETT_2D_ARRAY : ETT_2D;
			if (arraySlices > 1 && !renderTarget)
				os::Printer::log("CD3D12Texture: arraySlices > 1 ignore (texture non render-target)", ELL_WARNING);
			Source = ETS_UNKNOWN;

			OriginalSize = Size = size;
			const ECOLOR_FORMAT requestedFormat = (format == ECF_UNKNOWN) ? ECF_A8R8G8B8 : format;
			DxgiFormat = getD3D12ColorFormat(requestedFormat);
			// Same promotion as the IImage* constructor: ECF_R8G8B8 has no 24-bit DXGI
			// equivalent and is stored as 4 bytes/pixel, so ColorFormat/Pitch must
			// reflect that even for an empty texture.
			ColorFormat = (requestedFormat == ECF_R8G8B8 && DxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM)
				? ECF_A8R8G8B8 : requestedFormat;
			IsRenderTarget = renderTarget;
			// Only a non-render-target texture reorders its channels; an RTT keeps
			// R8G8B8A8_UNORM, like the back buffer.
			if (!renderTarget)
				DxgiFormat = applyNonRenderTargetChannelOrder(DxgiFormat);
			Pitch = size.Width * (video::IImage::getBitsPerPixelFromFormat(ColorFormat) / 8);

			// Multisampling only applies to a render-target-texture; an empty texture
			// (e.g. a compute-shader write target) stays single-sample.
			SampleCount = (renderTarget && sampleCount > 1) ? sampleCount : 1;
			SampleQuality = (SampleCount > 1) ? sampleQuality : 0;
			if (sampleCount > 1 && !renderTarget)
				os::Printer::log("CD3D12Texture: sampleCount > 1 ignore (texture non render-target)", ELL_WARNING);

			if (DxgiFormat == DXGI_FORMAT_UNKNOWN)
				return;

			createResource(renderTarget);
		}

		CD3D12Texture::CD3D12Texture(CD3D12Driver* driver, const core::dimension2d<u32>& size,
			const io::path& name, ECOLOR_FORMAT format, u32 mipLevels, u32 arraySlices, bool renderTarget,
			STiledTextureTag)
			: ITexture(name), Driver(driver)
		{
			DriverType = EDT_DIRECT3D12;
			Tiled = true;
			NumberOfArraySlices = arraySlices ? arraySlices : 1;
			TextureType = (NumberOfArraySlices > 1) ? ETT_2D_ARRAY : ETT_2D;
			Source = ETS_UNKNOWN;
			OriginalSize = Size = size;
			MipLevelCount = mipLevels ? mipLevels : computeMipLevels(size.Width, size.Height);

			const ECOLOR_FORMAT requestedFormat = (format == ECF_UNKNOWN) ? ECF_A8R8G8B8 : format;
			DxgiFormat = getD3D12ColorFormat(requestedFormat);
			ColorFormat = (requestedFormat == ECF_R8G8B8 && DxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM)
				? ECF_A8R8G8B8 : requestedFormat;
			IsRenderTarget = renderTarget;
			// The same channel-order rule as the empty-texture constructor above.
			if (!renderTarget)
				DxgiFormat = applyNonRenderTargetChannelOrder(DxgiFormat);
			Pitch = size.Width * (video::IImage::getBitsPerPixelFromFormat(ColorFormat) / 8);
			MipMaps = MipLevelCount > 1;

			if (DxgiFormat == DXGI_FORMAT_UNKNOWN)
				return;
			createResource(renderTarget);
		}

		// Every texture release passes through this destructor, regardless of origin
		// (removeTexture(), a replaced render target, a caller's drop()...), so this
		// is where GPU-side lifetime is decided.
		//
		// Nothing is released immediately: a command list may still reference this
		// resource, and D3D12 takes no reference on it, unlike D3D11. Releasing here
		// caused OBJECT_DELETED_WHILE_STILL_IN_USE / DEVICE_REMOVED. Resources are
		// handed to the driver instead, freed once the GPU passes the fence (see
		// CD3D12Driver::retireResource()).
		CD3D12Texture::~CD3D12Texture()
		{
			if (!Driver)
				return;

			if (HasRTV)
				Driver->retireDescriptor(Driver->getRTVHeap(), RTVHeapIndex);
			for (size_t i = 0; i < SliceRTVs.size(); ++i)
				if (SliceRTVs[i].Valid)
					Driver->retireDescriptor(Driver->getRTVHeap(), SliceRTVs[i].HeapIndex);
			if (HasSRV)
				Driver->retireDescriptor(Driver->getSRVHeap(), SRVHeapIndex);
			if (HasUAV)
				Driver->retireDescriptor(Driver->getSRVHeap(), UAVHeapIndex);
			if (HasDSV)
				Driver->retireDescriptor(Driver->getDSVHeap(), DSVHeapIndex);

			// StagingResource (lock()'s upload/readback) and ResolvedResource (MSAA companion)
			// may also be referenced by a copy still in flight.
			Driver->retireResource(std::move(Resource));
			Driver->retireResource(std::move(ResolvedResource));
			Driver->retireResource(std::move(StagingResource));
		}

		bool CD3D12Texture::createResource(bool asRenderTarget)
		{
			ID3D12Device2* device = Driver->getDevice();
			if (!device)
				return false;

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC desc = {};
			// ETT_3D (alias ETT_3D_ARRAY) is the only dimension distinct from TEXTURE2D.
			// DepthOrArraySize means volume depth for TEXTURE3D, slice/face count for
			// TEXTURE2D; array/cube/cube-array aren't separate D3D12_RESOURCE_DIMENSION
			// values, only the SRV view distinguishes them (see createShaderResourceView()).
			desc.Dimension = (TextureType == ETT_3D || TextureType == ETT_3D_ARRAY) ?
				D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Alignment = 0;
			desc.Width = Size.Width;
			desc.Height = Size.Height;
			desc.DepthOrArraySize = static_cast<UINT16>(NumberOfArraySlices);
			desc.MipLevels = static_cast<UINT16>(MipLevelCount);
			// A depth texture is created typeless so it can carry both a DSV (write) and
			// an SRV (read, for deferred passes). DxgiFormat keeps the typed format for the views.
			IsDepth = isDepthFormat(DxgiFormat);
			desc.Format = IsDepth ? getTypelessDepthFormat(DxgiFormat) : DxgiFormat;
			// SampleCount > 1 only for a render-target-texture (see the
			// constructor's comment) -- SampleQuality stays 0 for any other resource.
			desc.SampleDesc = { SampleCount, SampleQuality };
			// A reserved (tiled) resource has to declare the 64 KB tile layout; anything else lets
			// the driver choose.
			desc.Layout = Tiled ? D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE : D3D12_TEXTURE_LAYOUT_UNKNOWN;

			// ALLOW_RENDER_TARGET is also needed when MipLevelCount > 1, even for a
			// normal loaded texture: generateMips() creates a transient RTV per mip.
			// Never for a block-compressed format (D3D12 rejects it there, and
			// generateMips() isn't called on compressed textures anyway) or for a format
			// the device can't use as a render target (e.g. R32G32B32_FLOAT), which would
			// fail CreateCommittedResource.
			//
			// A depth format wants ALLOW_DEPTH_STENCIL and specifically not
			// ALLOW_RENDER_TARGET; combining them is an E_INVALIDARG.
			const bool mipBlitCapable = !IsDepth && !Tiled
				&& !IImage::isCompressedFormat(ColorFormat)
				&& supportsRenderTarget(Driver, DxgiFormat);
			if (IsDepth)
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			else
				desc.Flags = (asRenderTarget || (MipLevelCount > 1 && mipBlitCapable)) ?
					D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
			if (IsUnorderedAccess)
				desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

			D3D12_CLEAR_VALUE clearValue = {};
			D3D12_CLEAR_VALUE* pClearValue = nullptr;
			if (IsDepth)
			{
				// DepthStencil clear value, not Color (a color clear value is also an
				// E_INVALIDARG here). 1.0 = far plane.
				clearValue.Format = DxgiFormat;
				clearValue.DepthStencil.Depth = 1.0f;
				clearValue.DepthStencil.Stencil = 0;
				pClearValue = &clearValue;
			}
			else if (asRenderTarget)
			{
				clearValue.Format = DxgiFormat;
				clearValue.Color[0] = clearValue.Color[1] = clearValue.Color[2] = 0.0f;
				clearValue.Color[3] = 1.0f;
				pClearValue = &clearValue;
				// This "optimized" clear value must match future ClearRenderTargetView()
				// calls to benefit from hardware-accelerated clearing; a mismatch is
				// only a debug-layer warning, not an error.
			}

			CurrentState = IsDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE :
				(asRenderTarget ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST);

			// A tiled texture reserves address space only (CreateReservedResource); its tiles get
			// memory from a tile pool heap through CD3D12Driver::updateTileMappings().
			HRESULT hr = Tiled ?
				device->CreateReservedResource(&desc, CurrentState, pClearValue, IID_PPV_ARGS(&Resource)) :
				device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
					CurrentState, pClearValue, IID_PPV_ARGS(&Resource));
			if (FAILED(hr))
			{
				os::Printer::log(Tiled ? "CD3D12Texture: CreateReservedResource failed" :
					"CD3D12Texture: CreateCommittedResource a echoue", ELL_ERROR);
				return false;
			}

			// A depth texture always gets a DSV + SRV, never an RTV, regardless of
			// `asRenderTarget`.
			if (IsDepth)
			{
				if (!createDepthStencilView())
					return false;
				return createShaderResourceView();
			}

			if (asRenderTarget)
			{
				if (!createRenderTargetView())
					return false;
				// A multisampled resource has no SRV usable by PSMain (Texture2D, not
				// Texture2DMS): createResolveResource() creates a single-sample companion
				// resource to carry the SRV, filled by resolveIfNeeded().
				if (SampleCount > 1)
				{
					if (!createResolveResource())
						return false;
				}
				else if (!createShaderResourceView())
					return false;
				// Initial state already RENDER_TARGET, consistent with CurrentState above.
			}
			else
			{
				if (!createShaderResourceView())
					return false;
				if (IsUnorderedAccess && !createUnorderedAccessView())
					return false;
			}

			return true;
		}

		bool CD3D12Texture::createShaderResourceView()
		{
			ID3D12Device2* device = Driver->getDevice();
			CD3DX12_CPU_DESCRIPTOR_HANDLE handle;
			if (!Driver->getSRVHeap().allocate(SRVHeapIndex, handle))
			{
				os::Printer::log("CD3D12Texture: heap SRV plein", ELL_ERROR);
				return false;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			// On a depth resource (typeless), the SRV must use the matching read format;
			// a D32_FLOAT SRV on an R32_TYPELESS resource is invalid.
			srvDesc.Format = IsDepth ? getDepthSRVFormat(DxgiFormat) : DxgiFormat;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			// Exposes the full mip chain (MipLevelCount, always 1 outside
			// ETCF_CREATE_MIP_MAPS) rather than a hard-coded mip 0, so an LOD-aware
			// sampler can read past mip 0. MipLevelCount stays 1 for non-ETT_2D types.
			switch (TextureType)
			{
			case ETT_2D_ARRAY:
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				srvDesc.Texture2DArray.MipLevels = MipLevelCount;
				srvDesc.Texture2DArray.ArraySize = NumberOfArraySlices;
				srvDesc.Texture2DArray.FirstArraySlice = 0;
				srvDesc.Texture2DArray.MostDetailedMip = 0;
				break;
			case ETT_CUBE:
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
				srvDesc.TextureCube.MipLevels = MipLevelCount;
				srvDesc.TextureCube.MostDetailedMip = 0;
				break;
			case ETT_CUBE_ARRAY:
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
				srvDesc.TextureCubeArray.MipLevels = MipLevelCount;
				srvDesc.TextureCubeArray.First2DArrayFace = 0;
				// NumCubes is the number of 6-face cubes; NumberOfArraySlices is the total
				// face count, not the cube count.
				srvDesc.TextureCubeArray.NumCubes = NumberOfArraySlices / 6;
				srvDesc.TextureCubeArray.MostDetailedMip = 0;
				break;
			case ETT_3D:
			case ETT_3D_ARRAY: // alias of ETT_3D, see the array<IImage*>& constructor
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
				srvDesc.Texture3D.MipLevels = MipLevelCount;
				srvDesc.Texture3D.MostDetailedMip = 0;
				break;
			default: // ETT_2D
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = MipLevelCount;
				srvDesc.Texture2D.MostDetailedMip = 0;
				break;
			}

			device->CreateShaderResourceView(Resource.Get(), &srvDesc, handle);
			SRVHandle = handle;
			HasSRV = true;
			return true;
		}

		bool CD3D12Texture::createUnorderedAccessView()
		{
			ID3D12Device2* device = Driver->getDevice();
			CD3DX12_CPU_DESCRIPTOR_HANDLE handle;
			// Same heap type as SRV/CBV (D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) - see
			// CD3D12HardwareBuffer::createComputeViews() for the identical precedent on
			// the structured-buffer UAV path.
			if (!Driver->getSRVHeap().allocate(UAVHeapIndex, handle))
			{
				os::Printer::log("CD3D12Texture: heap UAV plein", ELL_ERROR);
				return false;
			}

			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DxgiFormat;
			if (TextureType == ETT_2D_ARRAY)
			{
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
				uavDesc.Texture2DArray.MipSlice = 0;
				uavDesc.Texture2DArray.FirstArraySlice = 0;
				uavDesc.Texture2DArray.ArraySize = NumberOfArraySlices;
			}
			else
			{
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				uavDesc.Texture2D.MipSlice = 0;
			}

			device->CreateUnorderedAccessView(Resource.Get(), nullptr, &uavDesc, handle);
			UAVHandle = handle;
			HasUAV = true;
			return true;
		}

		// DSV of a depth texture created via addRenderTargetTexture(ECF_D32/D24S8/...).
		// The resource is TYPELESS (see createResource()): the view is what carries the typed format.
		bool CD3D12Texture::createDepthStencilView()
		{
			ID3D12Device2* device = Driver->getDevice();
			CD3DX12_CPU_DESCRIPTOR_HANDLE handle;
			if (!Driver->getDSVHeap().allocate(DSVHeapIndex, handle))
			{
				os::Printer::log("CD3D12Texture: heap DSV plein", ELL_ERROR);
				return false;
			}

			D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
			dsvDesc.Format = DxgiFormat;
			dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
			if (TextureType == ETT_2D_ARRAY)
			{
				dsvDesc.ViewDimension = (SampleCount > 1) ?
					D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
				dsvDesc.Texture2DArray.MipSlice = 0;
				dsvDesc.Texture2DArray.FirstArraySlice = 0;
				dsvDesc.Texture2DArray.ArraySize = NumberOfArraySlices;
			}
			else
			{
				dsvDesc.ViewDimension = (SampleCount > 1) ?
					D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
				dsvDesc.Texture2D.MipSlice = 0;
			}

			device->CreateDepthStencilView(Resource.Get(), &dsvDesc, handle);
			DSVHandle = handle;
			HasDSV = true;
			return true;
		}

		bool CD3D12Texture::createRenderTargetView()
		{
			ID3D12Device2* device = Driver->getDevice();
			CD3DX12_CPU_DESCRIPTOR_HANDLE handle;
			if (!Driver->getRTVHeap().allocate(RTVHeapIndex, handle))
			{
				os::Printer::log("CD3D12Texture: heap RTV plein", ELL_ERROR);
				return false;
			}

			if (TextureType == ETT_2D_ARRAY)
			{
				// A view covering all NumberOfArraySlices slices at once, so a single draw
				// with a geometry shader writing SV_RenderTargetArrayIndex can route each
				// primitive to its own slice.
				D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
				rtvDesc.Format = DxgiFormat;
				rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
				rtvDesc.Texture2DArray.MipSlice = 0;
				rtvDesc.Texture2DArray.FirstArraySlice = 0;
				rtvDesc.Texture2DArray.ArraySize = NumberOfArraySlices;
				device->CreateRenderTargetView(Resource.Get(), &rtvDesc, handle);
			}
			else
			{
				device->CreateRenderTargetView(Resource.Get(), nullptr, handle);
			}
			RTVHandle = handle;
			HasRTV = true;
			return true;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE CD3D12Texture::getRenderTargetView(u32 arraySlice)
		{
			const D3D12_CPU_DESCRIPTOR_HANDLE none = {};
			if (!HasRTV || arraySlice >= NumberOfArraySlices)
				return none;
			if (NumberOfArraySlices == 1)
				return RTVHandle;

			if (SliceRTVs.size() < NumberOfArraySlices)
				SliceRTVs.resize(NumberOfArraySlices);
			SSliceRTV& slice = SliceRTVs[arraySlice];
			if (slice.Valid)
				return slice.Handle;

			CD3DX12_CPU_DESCRIPTOR_HANDLE handle;
			if (!Driver->getRTVHeap().allocate(slice.HeapIndex, handle))
			{
				os::Printer::log("CD3D12Texture: RTV heap full (slice view)", ELL_ERROR);
				return none;
			}

			// A one-slice array view rather than a plain 2D view: the resource is an array and
			// the whole-array RTV of createRenderTargetView() is TEXTURE2DARRAY too.
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = DxgiFormat;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			rtvDesc.Texture2DArray.MipSlice = 0;
			rtvDesc.Texture2DArray.FirstArraySlice = arraySlice;
			rtvDesc.Texture2DArray.ArraySize = 1;
			Driver->getDevice()->CreateRenderTargetView(Resource.Get(), &rtvDesc, handle);
			slice.Handle = handle;
			slice.Valid = true;
			return slice.Handle;
		}

		bool CD3D12Texture::createResolveResource()
		{
			ID3D12Device2* device = Driver->getDevice();
			if (!device)
				return false;

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = Size.Width;
			desc.Height = Size.Height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DxgiFormat;
			desc.SampleDesc = { 1, 0 };
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE; // never an RTV/UAV, only a resolve target + SRV

			ResolvedState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				ResolvedState, nullptr, IID_PPV_ARGS(&ResolvedResource));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Texture: CreateCommittedResource (resolve MSAA) a echoue", ELL_ERROR);
				return false;
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE handle;
			if (!Driver->getSRVHeap().allocate(SRVHeapIndex, handle))
			{
				os::Printer::log("CD3D12Texture: heap SRV plein (resolve MSAA)", ELL_ERROR);
				return false;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DxgiFormat;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			srvDesc.Texture2D.MostDetailedMip = 0;

			device->CreateShaderResourceView(ResolvedResource.Get(), &srvDesc, handle);
			SRVHandle = handle;
			HasSRV = true;
			return true;
		}

		void CD3D12Texture::resolveIfNeeded(ID3D12GraphicsCommandList* cmdList)
		{
			if (SampleCount <= 1 || !cmdList || !Resource || !ResolvedResource)
				return;

			// Resource (multisampled RTV) must go through RESOLVE_SOURCE, ResolvedResource
			// (single-sample SRV) through RESOLVE_DEST -- the only valid states for ResolveSubresource.
			transitionTo(cmdList, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);

			if (ResolvedState != D3D12_RESOURCE_STATE_RESOLVE_DEST)
			{
				CD3DX12_RESOURCE_BARRIER toResolveDest = CD3DX12_RESOURCE_BARRIER::Transition(
					ResolvedResource.Get(), ResolvedState, D3D12_RESOURCE_STATE_RESOLVE_DEST);
				cmdList->ResourceBarrier(1, &toResolveDest);
				ResolvedState = D3D12_RESOURCE_STATE_RESOLVE_DEST;
			}

			cmdList->ResolveSubresource(ResolvedResource.Get(), 0, Resource.Get(), 0, DxgiFormat);

			CD3DX12_RESOURCE_BARRIER toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
				ResolvedResource.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			cmdList->ResourceBarrier(1, &toShaderResource);
			ResolvedState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

			// Falls back to RENDER_TARGET, the state CD3D12Driver::setRenderTarget()
			// expects when this texture is bound again.
			transitionTo(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}

		void CD3D12Texture::uploadInitialData(const void* data, u32 rowPitchBytes, bool expandR8G8B8ToRGBA8,
			UINT subresourceIndex)
		{
			ID3D12Device2* device = Driver->getDevice();

			D3D12_RESOURCE_DESC destDesc = Resource->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
			UINT numRows = 0;
			UINT64 rowSizeBytes = 0;
			UINT64 totalBytes = 0;
			// Computes the 256-byte-aligned pitch D3D12 requires for the copy
			// intermediate resource, rarely equal to the source pitch - hence the
			// row-by-row copy below instead of one memcpy.
			device->GetCopyableFootprints(&destDesc, subresourceIndex, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

			D3D12_HEAP_PROPERTIES uploadHeapProps = {};
			uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

			D3D12_RESOURCE_DESC uploadDesc = {};
			uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			uploadDesc.Width = totalBytes;
			uploadDesc.Height = 1;
			uploadDesc.DepthOrArraySize = 1;
			uploadDesc.MipLevels = 1;
			uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
			uploadDesc.SampleDesc = { 1, 0 };
			uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

			ComPtr<ID3D12Resource> uploadBuffer;
			HRESULT hr = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Texture: CreateCommittedResource (upload) a echoue", ELL_ERROR);
				return;
			}

			void* mapped = nullptr;
			D3D12_RANGE noRead = { 0, 0 };
			uploadBuffer->Map(0, &noRead, &mapped);

			const u8* src = static_cast<const u8*>(data);
			u8* dst = static_cast<u8*>(mapped);
			if (expandR8G8B8ToRGBA8)
			{
				// Source is ECF_R8G8B8 (3 bytes/pixel, R,G,B order). DxgiFormat was already
				// switched to B8G8R8A8_UNORM by applyNonRenderTargetChannelOrder(), so the
				// destination expects B,G,R,A order - hence the swap below (alpha forced to 0xFF).
				for (UINT row = 0; row < numRows; ++row)
				{
					const u8* srcRow = src + row * rowPitchBytes;
					u8* dstRow = dst + row * footprint.Footprint.RowPitch;
					for (UINT x = 0; x < Size.Width; ++x)
					{
						dstRow[x * 4 + 0] = srcRow[x * 3 + 2];
						dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
						dstRow[x * 4 + 2] = srcRow[x * 3 + 0];
						dstRow[x * 4 + 3] = 0xFF;
					}
				}
			}
			else
			{
				for (UINT row = 0; row < numRows; ++row)
				{
					memcpy(dst + row * footprint.Footprint.RowPitch,
						src + row * rowPitchBytes,
						rowSizeBytes);
				}
			}
			uploadBuffer->Unmap(0, nullptr);

			CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
			if (!cmdList)
				return;

			D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
			dstLoc.pResource = Resource.Get();
			dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLoc.SubresourceIndex = subresourceIndex;

			D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
			srcLoc.pResource = uploadBuffer.Get();
			srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLoc.PlacedFootprint = footprint;

			// CurrentState is already COPY_DEST from creation, so no barrier is needed
			// before the copy. Only mip 0/subresource 0 uses this function today.
			cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

			// Exit barrier: the texture must end up readable by a shader.
			transitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			upload.endAndWait(); // blocking -- see comment in CD3D12Driver.h
			// uploadBuffer is freed here, safe since endAndWait() already waited for
			// the GPU transfer to finish.
		}

		void CD3D12Texture::transitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState)
		{
			if (!cmdList || !Resource || CurrentState == newState)
				return;

			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				Resource.Get(), CurrentState, newState);
			cmdList->ResourceBarrier(1, &barrier);
			CurrentState = newState;
		}

		void* CD3D12Texture::lock(E_TEXTURE_LOCK_MODE mode, u32 mipmapLevel)
		{
			// mipmapLevel is bounded by MipLevelCount (always 1 outside
			// ETCF_CREATE_MIP_MAPS). For ETT_2D_ARRAY/CUBE/CUBE_ARRAY, MipLevelCount is
			// always 1, so subresourceIndex = mip + slice*MipLevels reduces to slice,
			// letting callers address a slice/face through the same parameter. ETT_3D
			// stays bounded by MipLevelCount alone: a D3D12 volume has one subresource
			// per mip (all Z slices combined), so reading an individual slice means
			// indexing into the blob from lock(0) (RowPitch*Height per slice).
			//
			// A null Resource should never reach here, but is checked as a safety net:
			// dereferencing it would be an access violation.
			if (!Resource)
			{
				os::Printer::log("CD3D12Texture::lock: texture sans ressource GPU", getName().getPath(), ELL_ERROR);
				return nullptr;
			}

			const UINT subresourceIndex = mipmapLevel;
			const UINT maxSubresource = (TextureType == ETT_3D || TextureType == ETT_3D_ARRAY) ?
				MipLevelCount : (MipLevelCount * NumberOfArraySlices);
			if (subresourceIndex >= maxSubresource)
			{
				os::Printer::log("CD3D12Texture::lock: mipmapLevel hors bornes", ELL_WARNING);
				return nullptr;
			}

			ID3D12Device2* device = Driver->getDevice();
			D3D12_RESOURCE_DESC destDesc = Resource->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
			UINT numRows = 0;
			UINT64 rowSizeBytes = 0;
			UINT64 totalBytes = 0;
			device->GetCopyableFootprints(&destDesc, subresourceIndex, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);
			StagingRowPitch = footprint.Footprint.RowPitch;
			// Pitch must reflect the mip actually locked - a non-zero mip has a
			// smaller (still 256-byte-aligned) footprint than mip 0.
			Pitch = StagingRowPitch;

			D3D12_HEAP_PROPERTIES stagingHeapProps = {};
			D3D12_RESOURCE_DESC stagingDesc = {};
			stagingDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			stagingDesc.Width = totalBytes;
			stagingDesc.Height = 1;
			stagingDesc.DepthOrArraySize = 1;
			stagingDesc.MipLevels = 1;
			stagingDesc.Format = DXGI_FORMAT_UNKNOWN;
			stagingDesc.SampleDesc = { 1, 0 };
			stagingDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

			LastLockMode = mode;
			LastLockMipLevel = mipmapLevel;

			if (LastLockMode == ETLM_READ_ONLY || LastLockMode == ETLM_READ_WRITE)
			{
				// The draws that produced this content may still be in the driver's open
				// command list, not yet submitted. The readback copy goes straight onto the
				// queue, so without this flush it would outrun those draws and read stale
				// data - this broke screen capture (SaveFrame() draws then immediately reads
				// back). D3D11's immediate context has no such gap.
				Driver->flushCommandList();

				// GPU readback uses a D3D12_HEAP_TYPE_READBACK resource, which must stay in
				// COPY_DEST permanently and can't serve as a copy source for unlock()'s
				// upload - hence this temporary readback resource for the READ_WRITE case.
				ComPtr<ID3D12Resource> readback;
				D3D12_HEAP_PROPERTIES readbackHeapProps = {};
				readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
				HRESULT hr = device->CreateCommittedResource(&readbackHeapProps, D3D12_HEAP_FLAG_NONE, &stagingDesc,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
				if (FAILED(hr))
					return nullptr;

				CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
				if (!cmdList)
					return nullptr;

				D3D12_RESOURCE_STATES stateBeforeCopy = CurrentState;
				transitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);

				D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
				srcLoc.pResource = Resource.Get();
				srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				srcLoc.SubresourceIndex = mipmapLevel;
				D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
				dstLoc.pResource = readback.Get();
				dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				dstLoc.PlacedFootprint = footprint;
				cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

				transitionTo(cmdList, stateBeforeCopy); // restores the original state (e.g. PIXEL_SHADER_RESOURCE)
				upload.endAndWait();

				void* readMapped = nullptr;
				D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(totalBytes) };
				readback->Map(0, &readRange, &readMapped);

				if (LastLockMode == ETLM_READ_ONLY)
				{
					// Read-only: the readback resource itself becomes StagingResource;
					// unlock() just does an Unmap().
					StagingResource = readback;
					MappedStagingData = readMapped;
					return MappedStagingData;
				}

				// READ_WRITE: copies the content into an UPLOAD resource (CPU-writable, and
				// usable as unlock()'s copy source), then releases the readback resource.
				D3D12_HEAP_PROPERTIES uploadHeapProps = {};
				uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
				HRESULT hrUpload = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &stagingDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&StagingResource));
				if (FAILED(hrUpload))
				{
					D3D12_RANGE noWrite = { 0, 0 };
					readback->Unmap(0, &noWrite);
					return nullptr;
				}

				D3D12_RANGE noRead = { 0, 0 };
				StagingResource->Map(0, &noRead, &MappedStagingData);
				memcpy(MappedStagingData, readMapped, static_cast<size_t>(totalBytes));

				D3D12_RANGE noWrite = { 0, 0 }; // nothing written to the readback copy, only read
				readback->Unmap(0, &noWrite);
			}
			else // ETLM_WRITE_ONLY
			{
				stagingHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
				HRESULT hr = device->CreateCommittedResource(&stagingHeapProps, D3D12_HEAP_FLAG_NONE, &stagingDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&StagingResource));
				if (FAILED(hr))
					return nullptr;

				D3D12_RANGE noRead = { 0, 0 };
				StagingResource->Map(0, &noRead, &MappedStagingData);
			}

			return MappedStagingData;
		}

		void CD3D12Texture::unlock()
		{
			if (!StagingResource || !MappedStagingData)
				return;

			if (LastLockMode == ETLM_READ_ONLY)
			{
				StagingResource->Unmap(0, nullptr);
			}
			else
			{
				D3D12_RANGE writtenRange = { 0, StagingResource->GetDesc().Width };
				StagingResource->Unmap(0, &writtenRange);

				ID3D12Device2* device = Driver->getDevice();
				D3D12_RESOURCE_DESC destDesc = Resource->GetDesc();
				D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
				UINT numRows = 0;
				UINT64 rowSizeBytes = 0;
				UINT64 totalBytes = 0;
				device->GetCopyableFootprints(&destDesc, LastLockMipLevel, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

				CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
				if (cmdList)
				{
					D3D12_RESOURCE_STATES stateBeforeCopy = CurrentState;
					transitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_DEST);

					D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
					dstLoc.pResource = Resource.Get();
					dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dstLoc.SubresourceIndex = LastLockMipLevel;
					D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
					srcLoc.pResource = StagingResource.Get();
					srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					srcLoc.PlacedFootprint = footprint;
					cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

					D3D12_RESOURCE_STATES restoreState =
						(stateBeforeCopy == D3D12_RESOURCE_STATE_COPY_DEST) ?
						D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : stateBeforeCopy;
					transitionTo(cmdList, restoreState);

					upload.endAndWait();
				}
			}

			StagingResource.Reset();
			MappedStagingData = nullptr;
		}

		void CD3D12Texture::regenerateMipMapLevels(void* mipmapData)
		{
			// mipmapData is ignored: regeneration always re-reads the GPU resource's
			// current mip 0, not an external CPU source.
			if (MipLevelCount <= 1)
			{
				os::Printer::log("CD3D12Texture::regenerateMipMapLevels: texture sans chaine de "
					"mipmaps (ETCF_CREATE_MIP_MAPS non actif a la creation)", ELL_WARNING);
				return;
			}
			generateMips();
		}

		UINT CD3D12Texture::computeMipLevels(UINT width, UINT height)
		{
			UINT levels = 1;
			UINT w = width;
			UINT h = height;
			while (w > 1 || h > 1)
			{
				w = (w > 1) ? (w >> 1) : 1;
				h = (h > 1) ? (h >> 1) : 1;
				++levels;
			}
			return levels;
		}

		void CD3D12Texture::generateMips()
		{
			if (MipLevelCount <= 1 || !Resource)
				return;

			// Mip generation is a pixel-shader blit needing a transient RTV per
			// destination mip. Impossible for a format without ALLOW_RENDER_TARGET,
			// chiefly block-compressed formats (BC1..BC7): calling CreateRenderTargetView
			// there flags the debug layer and removes the device.
			//
			// Not a real limitation: a compressed texture's mip chain always arrives
			// complete from the source file, so there's nothing to regenerate. D3D11's
			// GenerateMips() has the same no-op behavior without BIND_RENDER_TARGET.
			if (IImage::isCompressedFormat(ColorFormat) || !supportsRenderTarget(Driver, DxgiFormat))
			{
				os::Printer::log("CD3D12Texture::generateMips: format non utilisable en render target"
					" (bloc-compresse ?) -- mipmaps deja fournies par le fichier source, rien a faire",
					ELL_DEBUG);
				return;
			}

			ID3D12Device2* device = Driver->getDevice();
			ID3D12PipelineState* pso = Driver->getOrCreateMipGenPSO(DxgiFormat);
			if (!pso)
			{
				os::Printer::log("CD3D12Texture::generateMips: pipeline de generation de mipmaps "
					"indisponible (voir CD3D12Driver::createMipGenPipeline())", ELL_WARNING);
				return;
			}

			// "Resting" state shared by all subresources before/after this loop. Each
			// iteration transitions only the destination mip, reads the source mip
			// without a barrier (already in that state), then restores the destination
			// mip - so CurrentState itself never needs updating here.
			const D3D12_RESOURCE_STATES restState = CurrentState;

			// One upload submission per mip level, not one for the whole loop:
			// MipGenSRVHeap has a single descriptor, rewritten every iteration. A D3D12
			// descriptor table isn't snapshotted at recording time, so recording all
			// iterations on one command list would make every draw read the last
			// iteration's descriptor. Submitting synchronously each time guarantees the
			// previous draw finished reading before the descriptor is rewritten.
			for (UINT level = 1; level < MipLevelCount; ++level)
			{
				const UINT srcLevel = level - 1;
				UINT dstWidth = Size.Width >> level;
				UINT dstHeight = Size.Height >> level;
				if (dstWidth < 1) dstWidth = 1;
				if (dstHeight < 1) dstHeight = 1;

				// Transient single-mip SRV on the source mip: allocated in the CPU-only
				// heap, copied to the mip-gen's shader-visible heap, then freed - it only
				// needs to survive this one draw.
				UINT srvIndex = 0;
				CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle;
				if (!Driver->getSRVHeap().allocate(srvIndex, srvHandle))
				{
					os::Printer::log("CD3D12Texture::generateMips: heap SRV plein", ELL_WARNING);
					break;
				}
				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.Format = DxgiFormat;
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srvDesc.Texture2D.MostDetailedMip = srcLevel;
				srvDesc.Texture2D.MipLevels = 1;
				device->CreateShaderResourceView(Resource.Get(), &srvDesc, srvHandle);

				device->CopyDescriptorsSimple(1, Driver->getMipGenSRVHeapCPU(), srvHandle,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				Driver->getSRVHeap().free(srvIndex);

				// Transient RTV on the destination mip alone.
				UINT rtvIndex = 0;
				CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle;
				if (!Driver->getRTVHeap().allocate(rtvIndex, rtvHandle))
				{
					os::Printer::log("CD3D12Texture::generateMips: heap RTV plein", ELL_WARNING);
					break;
				}
				D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
				rtvDesc.Format = DxgiFormat;
				rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Texture2D.MipSlice = level;
				device->CreateRenderTargetView(Resource.Get(), &rtvDesc, rtvHandle);

				CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
				if (!cmdList)
				{
					Driver->getRTVHeap().free(rtvIndex);
					break;
				}

				ID3D12DescriptorHeap* heaps[] = { Driver->getMipGenSRVHeap() };
				cmdList->SetDescriptorHeaps(1, heaps);
				cmdList->SetGraphicsRootSignature(Driver->getMipGenRootSignature());
				cmdList->SetPipelineState(pso);
				cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

				CD3DX12_RESOURCE_BARRIER toRT = CD3DX12_RESOURCE_BARRIER::Transition(
					Resource.Get(), restState, D3D12_RESOURCE_STATE_RENDER_TARGET, level);
				cmdList->ResourceBarrier(1, &toRT);

				D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = rtvHandle;
				cmdList->OMSetRenderTargets(1, &rtvCpu, FALSE, nullptr);

				D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(dstWidth),
					static_cast<float>(dstHeight), 0.0f, 1.0f };
				D3D12_RECT scissor = { 0, 0, static_cast<LONG>(dstWidth), static_cast<LONG>(dstHeight) };
				cmdList->RSSetViewports(1, &viewport);
				cmdList->RSSetScissorRects(1, &scissor);

				cmdList->SetGraphicsRootDescriptorTable(0, Driver->getMipGenSRVHeapGPU());
				cmdList->DrawInstanced(3, 1, 0, 0);

				CD3DX12_RESOURCE_BARRIER toRest = CD3DX12_RESOURCE_BARRIER::Transition(
					Resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, restState, level);
				cmdList->ResourceBarrier(1, &toRest);

				upload.endAndWait();

				Driver->getRTVHeap().free(rtvIndex);
			}
		}

		// ============================ array / cube / 3D ============================

		CD3D12Texture::CD3D12Texture(const core::array<IImage*>& images, CD3D12Driver* driver,
			E_TEXTURE_TYPE type, const io::path& name)
			: ITexture(name), Driver(driver)
		{
			DriverType = EDT_DIRECT3D12;
			TextureType = type;
			Source = ETS_UNKNOWN;
			IsRenderTarget = false;
			MipMaps = false;
			MipLevelCount = 1;

			if (images.size() == 0)
			{
				os::Printer::log("CD3D12Texture: tableau d'images vide (array/cube/3D)", ELL_ERROR);
				return;
			}

			// ETT_3D_ARRAY has no hardware equivalent (no D3D API supports an array of
			// 3D textures), so it's treated as ETT_3D.
			if (TextureType == ETT_3D_ARRAY)
				TextureType = ETT_3D;

			IImage* first = images[0];
			OriginalSize = Size = first->getDimension();
			NumberOfArraySlices = images.size();

			for (u32 i = 1; i < images.size(); ++i)
			{
				if (images[i]->getDimension() != Size)
				{
					os::Printer::log("CD3D12Texture: toutes les slices/faces doivent avoir la meme taille", ELL_ERROR);
					return;
				}
			}

			const ECOLOR_FORMAT sourceFormat = first->getColorFormat();
			DxgiFormat = getD3D12ColorFormat(sourceFormat);
			HasAlpha = (sourceFormat == ECF_A8R8G8B8 || sourceFormat == ECF_A1R5G5B5 ||
				sourceFormat == ECF_A16B16G16R16F || sourceFormat == ECF_A32B32G32R32F);

			if (DxgiFormat == DXGI_FORMAT_UNKNOWN)
				return; // error already logged by getD3D12ColorFormat

			const bool expandR8G8B8 = (sourceFormat == ECF_R8G8B8 && DxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM);
			ColorFormat = expandR8G8B8 ? ECF_A8R8G8B8 : sourceFormat;
			Pitch = expandR8G8B8 ? Size.Width * 4 : Size.Width * (IImage::getBitsPerPixelFromFormat(ColorFormat) / 8);
			// Non-render-target: same channel reorientation as the plain IImage*
			// constructor.
			DxgiFormat = applyNonRenderTargetChannelOrder(DxgiFormat);

			if (!createResource(false))
				return;

			uploadArraySlices(images, expandR8G8B8);
		}

		void CD3D12Texture::uploadArraySlices(const core::array<IImage*>& images, bool expandR8G8B8ToRGBA8)
		{
			ID3D12Device2* device = Driver->getDevice();
			D3D12_RESOURCE_DESC destDesc = Resource->GetDesc();
			const bool isVolume = (TextureType == ETT_3D);

			CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
			if (!cmdList)
				return;

			// Keeps each upload buffer alive until the single submission at the end:
			// all copies are recorded on the same command list so CurrentState (one
			// scalar for the whole resource) is transitioned only once.
			std::vector<ComPtr<ID3D12Resource>> uploadBuffers(images.size());

			// A 3D volume has one subresource per mip (all Z slices combined), so
			// GetCopyableFootprints always targets subresource 0, with a footprint
			// covering the whole depth. In the array/cube case each image is its own
			// subresource instead.
			for (u32 slice = 0; slice < images.size(); ++slice)
			{
				IImage* image = images[slice];
				void* pixels = image->lock();
				if (!pixels)
					continue;

				const UINT subresourceIndex = isVolume ? 0 : slice;

				D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
				UINT numRows = 0;
				UINT64 rowSizeBytes = 0;
				UINT64 totalBytes = 0;
				device->GetCopyableFootprints(&destDesc, subresourceIndex, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

				// A volume's single footprint covers all Z slices, so only one upload
				// buffer is allocated (on the first iteration), sized for the whole volume;
				// each slice is written to its own offset.
				if (isVolume)
				{
					if (slice == 0)
					{
						D3D12_HEAP_PROPERTIES uploadHeapProps = {};
						uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
						D3D12_RESOURCE_DESC uploadDesc = {};
						uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
						uploadDesc.Width = totalBytes;
						uploadDesc.Height = 1;
						uploadDesc.DepthOrArraySize = 1;
						uploadDesc.MipLevels = 1;
						uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
						uploadDesc.SampleDesc = { 1, 0 };
						uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

						HRESULT hr = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
							D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffers[0]));
						if (FAILED(hr))
						{
							os::Printer::log("CD3D12Texture: CreateCommittedResource (upload volume) a echoue", ELL_ERROR);
							image->unlock();
							return;
						}
					}

					void* mapped = nullptr;
					D3D12_RANGE noRead = { 0, 0 };
					uploadBuffers[0]->Map(0, &noRead, &mapped);

					// Each Z slice occupies RowPitch*numRows contiguous bytes, with no extra
					// alignment between slices.
					const UINT64 sliceSizeBytes = static_cast<UINT64>(footprint.Footprint.RowPitch) * numRows;
					u8* dstSliceBase = static_cast<u8*>(mapped) + slice * sliceSizeBytes;
					const u8* src = static_cast<const u8*>(pixels);
					const u32 rowPitchBytes = image->getPitch();

					if (expandR8G8B8ToRGBA8)
					{
						// DxgiFormat is already B8G8R8A8_UNORM here, hence the R/B swap instead of
						// a direct R,G,B copy.
						for (UINT row = 0; row < numRows; ++row)
						{
							const u8* srcRow = src + row * rowPitchBytes;
							u8* dstRow = dstSliceBase + row * footprint.Footprint.RowPitch;
							for (UINT x = 0; x < Size.Width; ++x)
							{
								dstRow[x * 4 + 0] = srcRow[x * 3 + 2];
								dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
								dstRow[x * 4 + 2] = srcRow[x * 3 + 0];
								dstRow[x * 4 + 3] = 0xFF;
							}
						}
					}
					else
					{
						for (UINT row = 0; row < numRows; ++row)
						{
							memcpy(dstSliceBase + row * footprint.Footprint.RowPitch,
								src + row * rowPitchBytes, rowSizeBytes);
						}
					}

					uploadBuffers[0]->Unmap(0, nullptr);
					image->unlock();

					// A single copy for the whole volume, at the last slice, once the buffer
					// is fully filled.
					if (slice + 1 == images.size())
					{
						D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
						dstLoc.pResource = Resource.Get();
						dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
						dstLoc.SubresourceIndex = 0;

						D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
						srcLoc.pResource = uploadBuffers[0].Get();
						srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
						srcLoc.PlacedFootprint = footprint;

						cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
					}

					continue;
				}

				// Array/cube path: each image is its own independent subresource.
				D3D12_HEAP_PROPERTIES uploadHeapProps = {};
				uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
				D3D12_RESOURCE_DESC uploadDesc = {};
				uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				uploadDesc.Width = totalBytes;
				uploadDesc.Height = 1;
				uploadDesc.DepthOrArraySize = 1;
				uploadDesc.MipLevels = 1;
				uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
				uploadDesc.SampleDesc = { 1, 0 };
				uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

				HRESULT hr = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffers[slice]));
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12Texture: CreateCommittedResource (upload slice) a echoue", ELL_ERROR);
					image->unlock();
					continue;
				}

				void* mapped = nullptr;
				D3D12_RANGE noRead = { 0, 0 };
				uploadBuffers[slice]->Map(0, &noRead, &mapped);

				const u8* src = static_cast<const u8*>(pixels);
				u8* dst = static_cast<u8*>(mapped);
				const u32 rowPitchBytes = image->getPitch();

				if (expandR8G8B8ToRGBA8)
				{
					// DxgiFormat is already B8G8R8A8_UNORM here, hence the R/B swap instead of
					// a direct R,G,B copy.
					for (UINT row = 0; row < numRows; ++row)
					{
						const u8* srcRow = src + row * rowPitchBytes;
						u8* dstRow = dst + row * footprint.Footprint.RowPitch;
						for (UINT x = 0; x < Size.Width; ++x)
						{
							dstRow[x * 4 + 0] = srcRow[x * 3 + 2];
							dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
							dstRow[x * 4 + 2] = srcRow[x * 3 + 0];
							dstRow[x * 4 + 3] = 0xFF;
						}
					}
				}
				else
				{
					for (UINT row = 0; row < numRows; ++row)
					{
						memcpy(dst + row * footprint.Footprint.RowPitch,
							src + row * rowPitchBytes, rowSizeBytes);
					}
				}
				uploadBuffers[slice]->Unmap(0, nullptr);
				image->unlock();

				D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
				dstLoc.pResource = Resource.Get();
				dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dstLoc.SubresourceIndex = subresourceIndex;

				D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
				srcLoc.pResource = uploadBuffers[slice].Get();
				srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				srcLoc.PlacedFootprint = footprint;

				// CurrentState is already COPY_DEST from creation, so no barrier is needed
				// before each copy.
				cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
			}

			// A single ALL_SUBRESOURCES barrier for the whole resource, once all
			// faces/slices are copied.
			transitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			upload.endAndWait();
			// uploadBuffers are freed here, safe since endAndWait() already waited for
			// the GPU transfer to finish.
		}

		// ============================ combine N already-uploaded textures ============================

		CD3D12Texture::CD3D12Texture(const core::array<ITexture*>& surfaces, CD3D12Driver* driver,
			E_TEXTURE_TYPE type, const io::path& name)
			: ITexture(name), Driver(driver)
		{
			DriverType = EDT_DIRECT3D12;
			TextureType = type;
			Source = ETS_UNKNOWN;
			IsRenderTarget = false;
			MipMaps = false;

			if (surfaces.size() == 0)
			{
				os::Printer::log("CD3D12Texture: tableau de textures vide (combine array/cube)", ELL_ERROR);
				return;
			}

			// ETT_3D/ETT_3D_ARRAY makes no sense here: a volumetric texture has one
			// subresource per mip, so there's no individual slice to copy
			// resource-to-resource. ETT_3D is never requested by this constructor's
			// caller (CD3D12Driver::getTexture(files[], Type)).

			CD3D12Texture* first = static_cast<CD3D12Texture*>(surfaces[0]);
			OriginalSize = Size = first->getSize();
			DxgiFormat = first->getDxgiFormat();
			MipLevelCount = first->getMipLevelCount();
			ColorFormat = first->getColorFormat();
			NumberOfArraySlices = surfaces.size();
			HasAlpha = first->hasAlpha();

			for (u32 i = 1; i < surfaces.size(); ++i)
			{
				CD3D12Texture* slice = static_cast<CD3D12Texture*>(surfaces[i]);
				if (slice->getSize() != Size || slice->getDxgiFormat() != DxgiFormat ||
					slice->getMipLevelCount() != MipLevelCount)
				{
					os::Printer::log("CD3D12Texture: toutes les slices/faces doivent avoir la meme "
						"taille, le meme format et le meme nombre de mip levels", ELL_ERROR);
					return;
				}
			}

			if (!createResource(false))
				return;

			CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
			if (!cmdList)
				return;

			// Direct GPU->GPU copy, subresource by subresource, no intermediate upload
			// buffer: both resources are already complete D3D12 textures, so D3D12
			// handles the copy semantics (including block-compressed formats) directly.
			for (u32 i = 0; i < surfaces.size(); ++i)
			{
				CD3D12Texture* slice = static_cast<CD3D12Texture*>(surfaces[i]);
				const D3D12_RESOURCE_STATES sliceRestState = slice->getCurrentState();
				slice->transitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);

				for (UINT mip = 0; mip < MipLevelCount; ++mip)
				{
					const UINT dstSubresource = mip + i * MipLevelCount;

					// Each subresource is already COPY_DEST from creation, so no barrier is
					// needed before the first copy.
					D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
					dstLoc.pResource = Resource.Get();
					dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dstLoc.SubresourceIndex = dstSubresource;

					D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
					srcLoc.pResource = slice->getResource();
					srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					srcLoc.SubresourceIndex = mip;

					cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
				}

				slice->transitionTo(cmdList, sliceRestState);
			}

			transitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			upload.endAndWait();

			createShaderResourceView();
		}

	}
}

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
