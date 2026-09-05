// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#ifndef __I_IMAGE_H_INCLUDED__
#define __I_IMAGE_H_INCLUDED__

#include "IReferenceCounted.h"
#include "position2d.h"
#include "rect.h"
#include "SColor.h"

namespace irr
{
namespace video
{

//! Interface for software image data.
/** Image loaders create these images from files. IVideoDrivers convert
these images into their (hardware) textures.
*/
class IImage : public virtual IReferenceCounted
{
public:

	//! Lock function. Use this to get a pointer to the image data.
	/** After you don't need the pointer anymore, you must call unlock().
	\return Pointer to the image data. What type of data is pointed to
	depends on the color format of the image. For example if the color
	format is ECF_A8R8G8B8, it is of u32. Be sure to call unlock() after
	you don't need the pointer any more. */
	virtual void* lock() = 0;

	//! Unlock function.
	/** Should be called after the pointer received by lock() is not
	needed anymore. */
	virtual void unlock() = 0;

	//! Returns width and height of image data.
	virtual const core::dimension2d<u32>& getDimension() const = 0;

	//! Returns bits per pixel.
	virtual u32 getBitsPerPixel() const = 0;

	//! Returns bytes per pixel
	virtual u32 getBytesPerPixel() const = 0;

	//! Returns mipmaps count
	virtual u32 getMipMapsCount() const =0;

	//! Returns image data size in bytes
	virtual u32 getImageDataSizeInBytes() const = 0;

	//! Returns image data size in pixels
	virtual u32 getImageDataSizeInPixels() const = 0;

	//! Returns a pixel
	virtual SColor getPixel(u32 x, u32 y) const = 0;

	//! Sets a pixel
	virtual void setPixel(u32 x, u32 y, const SColor &color, bool blend = false ) = 0;

	//! Returns the color format
	virtual ECOLOR_FORMAT getColorFormat() const = 0;

	//! Returns mask for red value of a pixel
	virtual u32 getRedMask() const = 0;

	//! Returns mask for green value of a pixel
	virtual u32 getGreenMask() const = 0;

	//! Returns mask for blue value of a pixel
	virtual u32 getBlueMask() const = 0;

	//! Returns mask for alpha value of a pixel
	virtual u32 getAlphaMask() const = 0;

	//! Returns pitch of image
	virtual u32 getPitch() const =0;

	//! Copies the image into the target, scaling the image to fit
	virtual void copyToScaling(void* target, u32 width, u32 height, ECOLOR_FORMAT format=ECF_A8R8G8B8, u32 pitch=0) =0;

	//! Copies the image into the target, scaling the image to fit
	virtual void copyToScaling(IImage* target) =0;

	//! copies this surface into another
	virtual void copyTo(IImage* target, const core::position2d<s32>& pos=core::position2d<s32>(0,0)) =0;

	//! copies this surface into another
	virtual void copyTo(IImage* target, const core::position2d<s32>& pos, const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect=0) =0;

	//! copies this surface into another, using the alpha mask and cliprect and a color to add with
	virtual void copyToWithAlpha(IImage* target, const core::position2d<s32>& pos,
			const core::rect<s32>& sourceRect, const SColor &color,
			const core::rect<s32>* clipRect = 0) =0;

	//! copies this surface into another, scaling it to fit, appyling a box filter
	virtual void copyToScalingBoxFilter(IImage* target, s32 bias = 0, bool blend = false) = 0;

	//! fills the surface with given color
	virtual void fill(const SColor &color) =0;

	//! Inform whether the image is compressed
	virtual bool isCompressed() const = 0;

	//! Check whether the image has MipMaps
	/** \return True if image has MipMaps, else false. */
	virtual bool hasMipMaps() const = 0;

	//! get the amount of Bits per Pixel of the given color format
	static u32 getBitsPerPixelFromFormat(const ECOLOR_FORMAT format)
	{
		switch(format)
		{
		case ECF_A1R5G5B5:
			return 16;
		case ECF_R5G6B5:
			return 16;
		case ECF_R8G8B8:
			return 24;
		case ECF_A8R8G8B8:
			return 32;
		case ECF_DXT1:
			return 16;
		case ECF_DXT2:
		case ECF_DXT3:
		case ECF_DXT4:
		case ECF_DXT5:
			return 32;
		case ECF_D16:
			return 16;
		case ECF_D32:
			return 32;
		case ECF_D24S8:
			return 32;
		case ECF_R8:
			return 8;
		case ECF_R8G8:
			return 16;
		case ECF_R16:
			return 16;
		case ECF_R16G16:
			return 32;
		case ECF_R16F:
			return 16;
		case ECF_G16R16F:
			return 32;
		case ECF_A16B16G16R16F:
			return 64;
		case ECF_R32F:
			return 32;
		case ECF_G32R32F:
			return 64;
		case ECF_A32B32G32R32F:
			return 128;
		case ECF_BC6_U:
		case ECF_BC6_S:
			return 8;
		case ECF_BC7_S:
		case ECF_BC7_U:
			return 8;
		case ECF_DXT1_SRGB:
			return 16;
		case ECF_DXT3_SRGB:
		case ECF_DXT5_SRGB:
			return 32;
		case ECF_BC4_U:
		case ECF_BC4_S:
		case ECF_BC5_U:
		case ECF_BC5_S:
			return 8;
		default:
			return 0;
		}
	}

	//! test if this is compressed color format
	static bool isCompressedFormat(const ECOLOR_FORMAT format)
	{
		return getBlockBytes(format) != 0;
	}

	//! Bytes one 4x4 block of a block-compressed format occupies, 0 for any other format.
	/** Every compressed format the engine knows is a 4x4 block format: 8 bytes for the
	one-channel and colour-only families (BC1/DXT1, BC4), 16 for all the others. */
	static u32 getBlockBytes(const ECOLOR_FORMAT format)
	{
		switch(format)
		{
			case ECF_DXT1:
			case ECF_DXT1_SRGB:
			case ECF_BC4_U:
			case ECF_BC4_S:
				return 8;
			case ECF_DXT2:
			case ECF_DXT3:
			case ECF_DXT4:
			case ECF_DXT5:
			case ECF_DXT3_SRGB:
			case ECF_DXT5_SRGB:
			case ECF_BC5_U:
			case ECF_BC5_S:
			case ECF_BC6_U:
			case ECF_BC6_S:
			case ECF_BC7_S:
			case ECF_BC7_U:
				return 16;
			default:
				return 0;
		}
	}

	//! Bytes one tightly packed surface of `width` x `height` texels takes in `format`.
	/** Whole 4x4 blocks for a block-compressed format (a 1x1 mip level still costs one block),
	rows of getBitsPerPixelFromFormat() otherwise. The layout every .dds / .ktx style container
	stores its mip levels in. */
	static u32 getSurfaceSizeInBytes(const ECOLOR_FORMAT format, u32 width, u32 height)
	{
		const u32 blockBytes = getBlockBytes(format);
		if (blockBytes)
			return ((width + 3) / 4) * ((height + 3) / 4) * blockBytes;
		return width * height * (getBitsPerPixelFromFormat(format) / 8);
	}

	//! True if the format carries an alpha channel a shader can read.
	/** DXT1/BC1 is counted as opaque: its 1 bit alpha is an encoder choice most content
	never uses, and treating it as transparent would push every DXT1 material onto the
	transparent path. */
	static bool hasAlphaFormat(const ECOLOR_FORMAT format)
	{
		switch(format)
		{
			case ECF_A1R5G5B5:
			case ECF_A8R8G8B8:
			case ECF_A8R8G8B8S:
			case ECF_A16B16G16R16F:
			case ECF_A32B32G32R32F:
			case ECF_DXT2:
			case ECF_DXT3:
			case ECF_DXT4:
			case ECF_DXT5:
			case ECF_DXT3_SRGB:
			case ECF_DXT5_SRGB:
			case ECF_BC7_U:
			case ECF_BC7_S:
				return true;
			default:
				return false;
		}
	}

	//! test if the color format is only viable for depth/stencil textures
	static bool isDepthFormat(const ECOLOR_FORMAT format)
	{
		switch(format)
		{
			case ECF_D16:
			case ECF_D32:
			case ECF_D24S8:
			case ECF_DF32S8:
				return true;
			default:
				return false;
		}
	}

	//! test if the color format is only viable for RenderTarget textures
	/** Since we don't have support for e.g. floating point IImage formats
	one should test if the color format can be used for arbitrary usage, or
	if it is restricted to RTTs. */
	static bool isRenderTargetOnlyFormat(const ECOLOR_FORMAT format)
	{
		switch(format)
		{
			case ECF_A1R5G5B5:
			case ECF_R5G6B5:
			case ECF_R8G8B8:
			case ECF_A8R8G8B8:
			case ECF_DXT1:
			case ECF_DXT2:
			case ECF_DXT3:
			case ECF_DXT4:
			case ECF_DXT5:
			case ECF_BC6_U:
			case ECF_BC6_S:
			case ECF_BC7_S:
			case ECF_BC7_U:
			case ECF_DXT1_SRGB:
			case ECF_DXT3_SRGB:
			case ECF_DXT5_SRGB:
			case ECF_BC4_U:
			case ECF_BC4_S:
			case ECF_BC5_U:
			case ECF_BC5_S:
				return false;
			default:
				return true;
		}
	}

};

} // end namespace video
} // end namespace irr

#endif

