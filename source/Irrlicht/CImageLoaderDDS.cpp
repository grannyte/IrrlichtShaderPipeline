// Copyright (C) 2013 Patryk Nadrowski
// Heavily based on the DDS loader implemented by Thomas Alten
// and DDS loader from IrrSpintz implemented by Thomas Ince
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

/*
	Based on Code from Copyright (c) 2003 Randy Reddig
	Based on code from Nvidia's DDS example:
	http://www.nvidia.com/object/dxtc_decompression_code.html

	mainly c to cpp
*/

#include "CImageLoaderDDS.h"

#if defined(_IRR_COMPILE_WITH_DDS_LOADER_) || defined(_IRR_COMPILE_WITH_DDS_DECODER_LOADER_)

#include "IReadFile.h"
#include "os.h"
#include "CColorConverter.h"
#include "CImage.h"
#include "irrString.h"
#include <dxgiformat.h>
#include <string.h>

// Header flag values
#define DDSD_CAPS			0x00000001
#define DDSD_HEIGHT			0x00000002
#define DDSD_WIDTH			0x00000004
#define DDSD_PITCH			0x00000008
#define DDSD_PIXELFORMAT	0x00001000
#define DDSD_MIPMAPCOUNT	0x00020000
#define DDSD_LINEARSIZE		0x00080000
#define DDSD_DEPTH			0x00800000

// Pixel format flag values
#define DDPF_ALPHAPIXELS	0x00000001
#define DDPF_ALPHA			0x00000002
#define DDPF_FOURCC			0x00000004
#define DDPF_RGB			0x00000040
#define DDPF_COMPRESSED		0x00000080
#define DDPF_LUMINANCE		0x00020000

// Caps1 values
#define DDSCAPS1_COMPLEX	0x00000008
#define DDSCAPS1_TEXTURE	0x00001000
#define DDSCAPS1_MIPMAP		0x00400000

// Caps2 values
#define DDSCAPS2_CUBEMAP            0x00000200
#define DDSCAPS2_CUBEMAP_POSITIVEX  0x00000400
#define DDSCAPS2_CUBEMAP_NEGATIVEX  0x00000800
#define DDSCAPS2_CUBEMAP_POSITIVEY  0x00001000
#define DDSCAPS2_CUBEMAP_NEGATIVEY  0x00002000
#define DDSCAPS2_CUBEMAP_POSITIVEZ  0x00004000
#define DDSCAPS2_CUBEMAP_NEGATIVEZ  0x00008000
#define DDSCAPS2_VOLUME             0x00200000

namespace irr
{

namespace video
{


	typedef struct {
		DXGI_FORMAT              dxgiFormat;
		u32 resourceDimension;
		u32                     miscFlag;
		u32                     arraySize;
		u32                     miscFlags2;
	} DDS_HEADER_DXT10;


#ifdef _IRR_COMPILE_WITH_DDS_DECODER_LOADER_

/*
DDSGetInfo()
extracts relevant info from a dds texture, returns 0 on success. The CPU decoder only knows
the legacy DXT1-5 FourCCs and 32 bit ARGB; anything else (a DX10 header included) is
DDS_PF_UNKNOWN to it.
*/
	__declspec(noinline) s32 DDSGetInfo(ddsHeader* dds, s32* width, s32* height, eDDSPixelFormat* pf)
{
	/* dummy test */
	if( dds == NULL )
		return -1;

	/* test dds header */
	if( *((s32*) dds->Magic) != *((s32*) "DDS ") )
		return -1;
	if( DDSLittleLong( dds->Size ) != 124 )
		return -1;
	if( !(DDSLittleLong( dds->Flags ) & DDSD_PIXELFORMAT) )
		return -1;
	if( !(DDSLittleLong( dds->Flags ) & DDSD_CAPS) )
		return -1;

	/* extract width and height */
	if( width != NULL )
		*width = DDSLittleLong( dds->Width );
	if( height != NULL )
		*height = DDSLittleLong( dds->Height );

	/* get pixel format */

	/* extract fourCC */
	const u32 fourCC = dds->PixelFormat.FourCC;

	/* test it */
	if( fourCC == 0 )
		*pf = DDS_PF_ARGB8888;
	else if( fourCC == *((u32*) "DXT1") )
		*pf = DDS_PF_DXT1;
	else if( fourCC == *((u32*) "DXT2") )
		*pf = DDS_PF_DXT2;
	else if( fourCC == *((u32*) "DXT3") )
		*pf = DDS_PF_DXT3;
	else if( fourCC == *((u32*) "DXT4") )
		*pf = DDS_PF_DXT4;
	else if( fourCC == *((u32*) "DXT5") )
		*pf = DDS_PF_DXT5;
	else
		*pf = DDS_PF_UNKNOWN;

	/* return ok */
	return 0;
}


/*
DDSDecompressARGB8888()
decompresses an argb 8888 format texture
*/
s32 DDSDecompressARGB8888(ddsHeader* dds, u8* data, s32 width, s32 height, u8* pixels)
{
	/* setup */
	u8* in = data;
	u8* out = pixels;

	/* walk y */
	for(s32 y = 0; y < height; y++)
	{
		/* walk x */
		for(s32 x = 0; x < width; x++)
		{
			*out++ = *in++;
			*out++ = *in++;
			*out++ = *in++;
			*out++ = *in++;
		}
	}

	/* return ok */
	return 0;
}


/*!
	DDSGetColorBlockColors()
	extracts colors from a dds color block
*/
void DDSGetColorBlockColors(ddsColorBlock* block, ddsColor colors[4])
{
	u16		word;


	/* color 0 */
	word = DDSLittleShort( block->colors[ 0 ] );
	colors[ 0 ].a = 0xff;

	/* extract rgb bits */
	colors[ 0 ].b = (u8) word;
	colors[ 0 ].b <<= 3;
	colors[ 0 ].b |= (colors[ 0 ].b >> 5);
	word >>= 5;
	colors[ 0 ].g = (u8) word;
	colors[ 0 ].g <<= 2;
	colors[ 0 ].g |= (colors[ 0 ].g >> 5);
	word >>= 6;
	colors[ 0 ].r = (u8) word;
	colors[ 0 ].r <<= 3;
	colors[ 0 ].r |= (colors[ 0 ].r >> 5);

	/* same for color 1 */
	word = DDSLittleShort( block->colors[ 1 ] );
	colors[ 1 ].a = 0xff;

	/* extract rgb bits */
	colors[ 1 ].b = (u8) word;
	colors[ 1 ].b <<= 3;
	colors[ 1 ].b |= (colors[ 1 ].b >> 5);
	word >>= 5;
	colors[ 1 ].g = (u8) word;
	colors[ 1 ].g <<= 2;
	colors[ 1 ].g |= (colors[ 1 ].g >> 5);
	word >>= 6;
	colors[ 1 ].r = (u8) word;
	colors[ 1 ].r <<= 3;
	colors[ 1 ].r |= (colors[ 1 ].r >> 5);

	/* use this for all but the super-freak math method */
	if( block->colors[ 0 ] > block->colors[ 1 ] )
	{
		/* four-color block: derive the other two colors.
		00 = color 0, 01 = color 1, 10 = color 2, 11 = color 3
		these two bit codes correspond to the 2-bit fields
		stored in the 64-bit block. */

		word = ((u16) colors[ 0 ].r * 2 + (u16) colors[ 1 ].r ) / 3;
		/* no +1 for rounding */
		/* as bits have been shifted to 888 */
		colors[ 2 ].r = (u8) word;
		word = ((u16) colors[ 0 ].g * 2 + (u16) colors[ 1 ].g) / 3;
		colors[ 2 ].g = (u8) word;
		word = ((u16) colors[ 0 ].b * 2 + (u16) colors[ 1 ].b) / 3;
		colors[ 2 ].b = (u8) word;
		colors[ 2 ].a = 0xff;

		word = ((u16) colors[ 0 ].r + (u16) colors[ 1 ].r * 2) / 3;
		colors[ 3 ].r = (u8) word;
		word = ((u16) colors[ 0 ].g + (u16) colors[ 1 ].g * 2) / 3;
		colors[ 3 ].g = (u8) word;
		word = ((u16) colors[ 0 ].b + (u16) colors[ 1 ].b * 2) / 3;
		colors[ 3 ].b = (u8) word;
		colors[ 3 ].a = 0xff;
	}
	else
	{
		/* three-color block: derive the other color.
		00 = color 0, 01 = color 1, 10 = color 2,
		11 = transparent.
		These two bit codes correspond to the 2-bit fields
		stored in the 64-bit block */

		word = ((u16) colors[ 0 ].r + (u16) colors[ 1 ].r) / 2;
		colors[ 2 ].r = (u8) word;
		word = ((u16) colors[ 0 ].g + (u16) colors[ 1 ].g) / 2;
		colors[ 2 ].g = (u8) word;
		word = ((u16) colors[ 0 ].b + (u16) colors[ 1 ].b) / 2;
		colors[ 2 ].b = (u8) word;
		colors[ 2 ].a = 0xff;

		/* random color to indicate alpha */
		colors[ 3 ].r = 0x00;
		colors[ 3 ].g = 0xff;
		colors[ 3 ].b = 0xff;
		colors[ 3 ].a = 0x00;
	}
}


/*
DDSDecodeColorBlock()
decodes a dds color block
fixme: make endian-safe
*/

void DDSDecodeColorBlock(u32* pixel, ddsColorBlock* block, s32 width, u32 colors[4])
{
	s32				r, n;
	u32	bits;
	u32	masks[] = { 3, 12, 3 << 4, 3 << 6 };	/* bit masks = 00000011, 00001100, 00110000, 11000000 */
	s32				shift[] = { 0, 2, 4, 6 };


	/* r steps through lines in y */
	for( r = 0; r < 4; r++, pixel += (width - 4) )	/* no width * 4 as u32 ptr inc will * 4 */
	{
		/* width * 4 bytes per pixel per line, each j dxtc row is 4 lines of pixels */

		/* n steps through pixels */
		for( n = 0; n < 4; n++ )
		{
			bits = block->row[ r ] & masks[ n ];
			bits >>= shift[ n ];

			switch( bits )
			{
			case 0:
				*pixel = colors[ 0 ];
				pixel++;
				break;

			case 1:
				*pixel = colors[ 1 ];
				pixel++;
				break;

			case 2:
				*pixel = colors[ 2 ];
				pixel++;
				break;

			case 3:
				*pixel = colors[ 3 ];
				pixel++;
				break;

			default:
				/* invalid */
				pixel++;
				break;
			}
		}
	}
}


/*
DDSDecodeAlphaExplicit()
decodes a dds explicit alpha block
*/
void DDSDecodeAlphaExplicit(u32* pixel, ddsAlphaBlockExplicit* alphaBlock, s32 width, u32 alphaZero)
{
	s32				row, pix;
	u16	word;
	ddsColor		color;


	/* clear color */
	color.r = 0;
	color.g = 0;
	color.b = 0;

	/* walk rows */
	for( row = 0; row < 4; row++, pixel += (width - 4) )
	{
		word = DDSLittleShort( alphaBlock->row[ row ] );

		/* walk pixels */
		for( pix = 0; pix < 4; pix++ )
		{
			/* zero the alpha bits of image pixel */
			*pixel &= alphaZero;
			color.a = word & 0x000F;
			color.a = color.a | (color.a << 4);
			*pixel |= *((u32*) &color);
			word >>= 4;		/* move next bits to lowest 4 */
			pixel++;		/* move to next pixel in the row */
		}
	}
}



/*
DDSDecodeAlpha3BitLinear()
decodes interpolated alpha block
*/
void DDSDecodeAlpha3BitLinear(u32* pixel, ddsAlphaBlock3BitLinear* alphaBlock, s32 width, u32 alphaZero)
{

	s32 row, pix;
	u32 stuff;
	u8 bits[ 4 ][ 4 ];
	u16 alphas[ 8 ];
	ddsColor aColors[ 4 ][ 4 ];

	/* get initial alphas */
	alphas[ 0 ] = alphaBlock->alpha0;
	alphas[ 1 ] = alphaBlock->alpha1;

	/* 8-alpha block */
	if( alphas[ 0 ] > alphas[ 1 ] )
	{
		/* 000 = alpha_0, 001 = alpha_1, others are interpolated */
		alphas[ 2 ] = ( 6 * alphas[ 0 ] +     alphas[ 1 ]) / 7;	/* bit code 010 */
		alphas[ 3 ] = ( 5 * alphas[ 0 ] + 2 * alphas[ 1 ]) / 7;	/* bit code 011 */
		alphas[ 4 ] = ( 4 * alphas[ 0 ] + 3 * alphas[ 1 ]) / 7;	/* bit code 100 */
		alphas[ 5 ] = ( 3 * alphas[ 0 ] + 4 * alphas[ 1 ]) / 7;	/* bit code 101 */
		alphas[ 6 ] = ( 2 * alphas[ 0 ] + 5 * alphas[ 1 ]) / 7;	/* bit code 110 */
		alphas[ 7 ] = (     alphas[ 0 ] + 6 * alphas[ 1 ]) / 7;	/* bit code 111 */
	}

	/* 6-alpha block */
	else
	{
		/* 000 = alpha_0, 001 = alpha_1, others are interpolated */
		alphas[ 2 ] = (4 * alphas[ 0 ] +     alphas[ 1 ]) / 5;	/* bit code 010 */
		alphas[ 3 ] = (3 * alphas[ 0 ] + 2 * alphas[ 1 ]) / 5;	/* bit code 011 */
		alphas[ 4 ] = (2 * alphas[ 0 ] + 3 * alphas[ 1 ]) / 5;	/* bit code 100 */
		alphas[ 5 ] = (    alphas[ 0 ] + 4 * alphas[ 1 ]) / 5;	/* bit code 101 */
		alphas[ 6 ] = 0;										/* bit code 110 */
		alphas[ 7 ] = 255;										/* bit code 111 */
	}

	/* decode 3-bit fields into array of 16 bytes with same value */

	/* first two rows of 4 pixels each */
	stuff = *((u32*) &(alphaBlock->stuff[ 0 ]));

	bits[ 0 ][ 0 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 0 ][ 1 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 0 ][ 2 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 0 ][ 3 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 1 ][ 0 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 1 ][ 1 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 1 ][ 2 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 1 ][ 3 ] = (u8) (stuff & 0x00000007);

	/* last two rows */
	stuff = *((u32*) &(alphaBlock->stuff[ 3 ])); /* last 3 bytes */

	bits[ 2 ][ 0 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 2 ][ 1 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 2 ][ 2 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 2 ][ 3 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 3 ][ 0 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 3 ][ 1 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 3 ][ 2 ] = (u8) (stuff & 0x00000007);
	stuff >>= 3;
	bits[ 3 ][ 3 ] = (u8) (stuff & 0x00000007);

	/* decode the codes into alpha values */
	for( row = 0; row < 4; row++ )
	{
		for( pix=0; pix < 4; pix++ )
		{
			aColors[ row ][ pix ].r = 0;
			aColors[ row ][ pix ].g = 0;
			aColors[ row ][ pix ].b = 0;
			aColors[ row ][ pix ].a = (u8) alphas[ bits[ row ][ pix ] ];
		}
	}

	/* write out alpha values to the image bits */
	for( row = 0; row < 4; row++, pixel += width-4 )
	{
		for( pix = 0; pix < 4; pix++ )
		{
			/* zero the alpha bits of image pixel */
			*pixel &= alphaZero;

			/* or the bits into the prev. nulled alpha */
			*pixel |= *((u32*) &(aColors[ row ][ pix ]));
			pixel++;
		}
	}
}


/*
DDSDecompressDXT1()
decompresses a dxt1 format texture
*/
s32 DDSDecompressDXT1(ddsHeader* dds, u8* data, s32 width, s32 height, u8* pixels)
{
	s32 x, y, xBlocks, yBlocks;
	u32 *pixel;
	ddsColorBlock *block;
	ddsColor colors[ 4 ];

	/* setup */
	xBlocks = width / 4;
	yBlocks = height / 4;

	/* walk y */
	for( y = 0; y < yBlocks; y++ )
	{
		/* 8 bytes per block */
		block = (ddsColorBlock*) (data + y * xBlocks * 8);

		/* walk x */
		for( x = 0; x < xBlocks; x++, block++ )
		{
			DDSGetColorBlockColors( block, colors );
			pixel = (u32*) (pixels + x * 16 + (y * 4) * width * 4);
			DDSDecodeColorBlock( pixel, block, width, (u32*) colors );
		}
	}

	/* return ok */
	return 0;
}


/*
DDSDecompressDXT3()
decompresses a dxt3 format texture
*/

s32 DDSDecompressDXT3(ddsHeader* dds, u8* data, s32 width, s32 height, u8* pixels)
{
	s32 x, y, xBlocks, yBlocks;
	u32 *pixel, alphaZero;
	ddsColorBlock *block;
	ddsAlphaBlockExplicit *alphaBlock;
	ddsColor colors[ 4 ];

	/* setup */
	xBlocks = width / 4;
	yBlocks = height / 4;

	/* create zero alpha */
	colors[ 0 ].a = 0;
	colors[ 0 ].r = 0xFF;
	colors[ 0 ].g = 0xFF;
	colors[ 0 ].b = 0xFF;
	alphaZero = *((u32*) &colors[ 0 ]);

	/* walk y */
	for( y = 0; y < yBlocks; y++ )
	{
		/* 8 bytes per block, 1 block for alpha, 1 block for color */
		block = (ddsColorBlock*) (data + y * xBlocks * 16);

		/* walk x */
		for( x = 0; x < xBlocks; x++, block++ )
		{
			/* get alpha block */
			alphaBlock = (ddsAlphaBlockExplicit*) block;

			/* get color block */
			block++;
			DDSGetColorBlockColors( block, colors );

			/* decode color block */
			pixel = (u32*) (pixels + x * 16 + (y * 4) * width * 4);
			DDSDecodeColorBlock( pixel, block, width, (u32*) colors );

			/* overwrite alpha bits with alpha block */
			DDSDecodeAlphaExplicit( pixel, alphaBlock, width, alphaZero );
		}
	}

	/* return ok */
	return 0;
}


/*
DDSDecompressDXT5()
decompresses a dxt5 format texture
*/
s32 DDSDecompressDXT5(ddsHeader* dds, u8* data, s32 width, s32 height, u8* pixels)
{
	s32 x, y, xBlocks, yBlocks;
	u32 *pixel, alphaZero;
	ddsColorBlock *block;
	ddsAlphaBlock3BitLinear *alphaBlock;
	ddsColor colors[ 4 ];

	/* setup */
	xBlocks = width / 4;
	yBlocks = height / 4;

	/* create zero alpha */
	colors[ 0 ].a = 0;
	colors[ 0 ].r = 0xFF;
	colors[ 0 ].g = 0xFF;
	colors[ 0 ].b = 0xFF;
	alphaZero = *((u32*) &colors[ 0 ]);

	/* walk y */
	for( y = 0; y < yBlocks; y++ )
	{
		/* 8 bytes per block, 1 block for alpha, 1 block for color */
		block = (ddsColorBlock*) (data + y * xBlocks * 16);

		/* walk x */
		for( x = 0; x < xBlocks; x++, block++ )
		{
			/* get alpha block */
			alphaBlock = (ddsAlphaBlock3BitLinear*) block;

			/* get color block */
			block++;
			DDSGetColorBlockColors( block, colors );

			/* decode color block */
			pixel = (u32*) (pixels + x * 16 + (y * 4) * width * 4);
			DDSDecodeColorBlock( pixel, block, width, (u32*) colors );

			/* overwrite alpha bits with alpha block */
			DDSDecodeAlpha3BitLinear( pixel, alphaBlock, width, alphaZero );
		}
	}

	/* return ok */
	return 0;
}


/*
DDSDecompressDXT2()
decompresses a dxt2 format texture (fixme: un-premultiply alpha)
*/
s32 DDSDecompressDXT2(ddsHeader* dds, u8* data, s32 width, s32 height, u8* pixels)
{
	/* decompress dxt3 first */
	const s32 r = DDSDecompressDXT3( dds, data, width, height, pixels );

	/* return to sender */
	return r;
}


/*
DDSDecompressDXT4()
decompresses a dxt4 format texture (fixme: un-premultiply alpha)
*/
s32 DDSDecompressDXT4(ddsHeader* dds, u8* data, s32 width, s32 height, u8* pixels)
{
	/* decompress dxt5 first */
	const s32 r = DDSDecompressDXT5( dds, data, width, height, pixels );

	/* return to sender */
	return r;
}


/*
DDSDecompress()
decompresses a dds texture into an rgba image buffer, returns 0 on success
*/
s32 DDSDecompress(ddsHeader* dds, u8* data, u8* pixels)
{
	s32 width, height;
	eDDSPixelFormat pf;

	/* get dds info */
	s32 r = DDSGetInfo( dds, &width, &height, &pf );
	if ( r )
		return r;

	/* decompress */
	switch( pf )
	{
	case DDS_PF_ARGB8888:
		/* fixme: support other [a]rgb formats */
		r = DDSDecompressARGB8888( dds, data, width, height, pixels );
		break;

	case DDS_PF_DXT1:
		r = DDSDecompressDXT1( dds, data, width, height, pixels );
		break;

	case DDS_PF_DXT2:
		r = DDSDecompressDXT2( dds, data, width, height, pixels );
		break;

	case DDS_PF_DXT3:
		r = DDSDecompressDXT3( dds, data, width, height, pixels );
		break;

	case DDS_PF_DXT4:
		r = DDSDecompressDXT4( dds, data, width, height, pixels );
		break;

	case DDS_PF_DXT5:
		r = DDSDecompressDXT5( dds, data, width, height, pixels );
		break;

	default: // DDS_PF_UNKNOWN
		r = -1;
		break;
	}

	/* return to sender */
	return r;
}

#endif


//! returns true if the file maybe is able to be loaded by this class
//! based on the file extension (e.g. ".tga")
bool CImageLoaderDDS::isALoadableFileExtension(const io::path& filename) const
{
	return core::hasFileExtension(filename, "dds");
}


//! returns true if the file maybe is able to be loaded by this class
bool CImageLoaderDDS::isALoadableFileFormat(io::IReadFile* file) const
{
	if (!file)
		return false;

	c8 MagicWord[4];
	file->read(&MagicWord, 4);

	return (MagicWord[0] == 'D' && MagicWord[1] == 'D' && MagicWord[2] == 'S');
}
#ifdef _IRR_COMPILE_WITH_DDS_DECODER_LOADER_

//! creates a surface from the file: the CPU decoder, always an A8R8G8B8 image of the top level.
IImage* CImageLoaderDDS::loadImage(io::IReadFile* file) const
{
	ddsHeader header;
	s32 width, height;
	eDDSPixelFormat pixelFormat;

	file->seek(0);
	file->read(&header, sizeof(ddsHeader));
	if (0 != DDSGetInfo(&header, &width, &height, &pixelFormat))
		return 0;

	const u32 newSize = file->getSize() - sizeof(ddsHeader);
	u8* memFile = new u8[newSize];
	file->read(memFile, newSize);

	IImage* image = new CImage(ECF_A8R8G8B8, core::dimension2d<u32>(width, height));
	if (DDSDecompress(&header, memFile, (u8*)image->lock()) == -1)
	{
		image->unlock();
		image->drop();
		image = 0;
	}

	delete[] memFile;
	return image;
}

#else // _IRR_COMPILE_WITH_DDS_DECODER_LOADER_

// The GPU path. What a driver receives is one of two things:
//
//  - a plain CImage of the top surface, for a 2D uncompressed file (the legacy 16/24/32 bit
//    layouts, luminance, alpha-only, the 8/16 bit DXGI layouts). Decoded to A8R8G8B8 unless the
//    layout is one CImage holds natively. Lockable, mip-mapped by the driver like any other image.
//
//  - the RAW FILE, header included, as a CImage flagged compressed with CImage::CompressedSize set
//    to the byte count. Used for every block-compressed format, for the uncompressed formats no
//    CImage can hold (floats, R8/R8G8/R16/R16G16, signed), and for any cube map, array or volume,
//    because that is where the mip chain, the faces and the slices live. CD3D11Texture and
//    CD3D12Texture hand it to DDSTextureLoader, CVulkanTexture parses it itself. An uncompressed
//    multi-surface file is repacked first into a tightly-pitched file in a layout all three drivers
//    accept (A8R8G8B8, or one of the 16 bit natives), so a legacy A8B8G8R8 or X8R8G8B8 cube map
//    arrives in a canonical form.
//
// Any file the table below does not know is refused with a log line naming the format; there is
// no fall-through into a driver-side crash.
namespace
{
	inline u32 ddsFourCC(c8 a, c8 b, c8 c, c8 d)
	{
		return (u32)(u8)a | ((u32)(u8)b << 8) | ((u32)(u8)c << 16) | ((u32)(u8)d << 24);
	}

	// D3DFORMAT numbers some writers put in the FourCC field for the wide formats.
	enum E_DDS_D3DFMT
	{
		EDF_R16F = 111,
		EDF_G16R16F = 112,
		EDF_A16B16G16R16F = 113,
		EDF_R32F = 114,
		EDF_G32R32F = 115,
		EDF_A32B32G32R32F = 116
	};

	const u32 DdsCaps2AllCubeFaces = DDSCAPS2_CUBEMAP_POSITIVEX | DDSCAPS2_CUBEMAP_NEGATIVEX |
		DDSCAPS2_CUBEMAP_POSITIVEY | DDSCAPS2_CUBEMAP_NEGATIVEY |
		DDSCAPS2_CUBEMAP_POSITIVEZ | DDSCAPS2_CUBEMAP_NEGATIVEZ;
	const u32 Dx10MiscTextureCube = 0x4;
	const u32 Dx10DimensionTexture1D = 2;
	const u32 Dx10DimensionTexture3D = 4;

	//! Channel layout of an uncompressed surface: legacy masks, or a DXGI format reduced to masks.
	struct SDdsPixelLayout
	{
		u32 BitCount;
		u32 RMask, GMask, BMask, AMask;
		bool Luminance; //!< RMask is a grey value to replicate into G and B
		bool AlphaOnly; //!< colour is white, AMask is all there is
	};

	//! Everything loadImage() decides from the headers.
	struct SDdsFile
	{
		u32 HeaderBytes;  //!< 128, or 148 with a DX10 header
		u32 Width, Height, Depth, MipLevels, Faces, ArraySize;
		bool Volume;
		ECOLOR_FORMAT Format; //!< what the texture ends up as
		bool RawOnly;         //!< uncompressed, but only deliverable as the raw file
		bool HasLayout;       //!< uncompressed with a decodable channel layout
		bool Decode;          //!< the layout is not one CImage holds natively: decode to A8R8G8B8
		SDdsPixelLayout Layout;
	};

	//! The four layouts a CImage holds as-is (what CColorConverter speaks), else ECF_UNKNOWN.
	ECOLOR_FORMAT nativePixelFormat(const SDdsPixelLayout& l)
	{
		if (l.Luminance || l.AlphaOnly)
			return ECF_UNKNOWN;
		if (l.BitCount == 32 && l.RMask == 0xff0000 && l.GMask == 0xff00 && l.BMask == 0xff && l.AMask == 0xff000000)
			return ECF_A8R8G8B8;
		if (l.BitCount == 24 && l.RMask == 0xff0000 && l.GMask == 0xff00 && l.BMask == 0xff)
			return ECF_R8G8B8;
		if (l.BitCount == 16 && l.RMask == 0xf800 && l.GMask == 0x7e0 && l.BMask == 0x1f && l.AMask == 0)
			return ECF_R5G6B5;
		if (l.BitCount == 16 && l.RMask == 0x7c00 && l.GMask == 0x3e0 && l.BMask == 0x1f && l.AMask == 0x8000)
			return ECF_A1R5G5B5;
		return ECF_UNKNOWN;
	}

	//! Layouts with channels wider than 8 bits that no CImage holds but every driver takes raw.
	ECOLOR_FORMAT rawPixelFormat(const SDdsPixelLayout& l)
	{
		if (l.BitCount == 32 && l.RMask == 0xffff && l.GMask == 0xffff0000 && !l.Luminance)
			return ECF_R16G16;
		if (l.BitCount == 16 && l.RMask == 0xffff)
			return ECF_R16;
		return ECF_UNKNOWN;
	}

	//! DXGI format the drivers' own .dds parsers read a repacked file's format as.
	DXGI_FORMAT dxgiFormatOf(ECOLOR_FORMAT format)
	{
		switch (format)
		{
		case ECF_A8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case ECF_R5G6B5:   return DXGI_FORMAT_B5G6R5_UNORM;
		case ECF_A1R5G5B5: return DXGI_FORMAT_B5G5R5A1_UNORM;
		case ECF_R16G16:   return DXGI_FORMAT_R16G16_UNORM;
		case ECF_R16:      return DXGI_FORMAT_R16_UNORM;
		default:           return DXGI_FORMAT_UNKNOWN;
		}
	}

	//! Legacy pixel format block describing a repacked file's format.
	void writeLegacyPixelFormat(ddsPixelFormat& pf, ECOLOR_FORMAT format)
	{
		pf.Size = sizeof(ddsPixelFormat);
		pf.FourCC = 0;
		pf.Flags = DDPF_RGB;
		pf.ABitMask = 0;
		switch (format)
		{
		case ECF_A8R8G8B8:
			pf.Flags |= DDPF_ALPHAPIXELS;
			pf.RGBBitCount = 32; pf.RBitMask = 0xff0000; pf.GBitMask = 0xff00; pf.BBitMask = 0xff; pf.ABitMask = 0xff000000;
			break;
		case ECF_R5G6B5:
			pf.RGBBitCount = 16; pf.RBitMask = 0xf800; pf.GBitMask = 0x7e0; pf.BBitMask = 0x1f;
			break;
		case ECF_A1R5G5B5:
			pf.Flags |= DDPF_ALPHAPIXELS;
			pf.RGBBitCount = 16; pf.RBitMask = 0x7c00; pf.GBitMask = 0x3e0; pf.BBitMask = 0x1f; pf.ABitMask = 0x8000;
			break;
		case ECF_R16G16:
			pf.RGBBitCount = 32; pf.RBitMask = 0xffff; pf.GBitMask = 0xffff0000; pf.BBitMask = 0;
			break;
		case ECF_R16:
			pf.Flags = DDPF_LUMINANCE;
			pf.RGBBitCount = 16; pf.RBitMask = 0xffff; pf.GBitMask = 0; pf.BBitMask = 0;
			break;
		default:
			break;
		}
	}

	//! One channel of a packed pixel widened (or narrowed) to 8 bits; `missing` if it has no mask.
	u8 channel8(u32 pixel, u32 mask, u8 missing)
	{
		if (!mask)
			return missing;
		u32 shift = 0;
		while (!((mask >> shift) & 1u))
			++shift;
		u32 bits = 0;
		while (shift + bits < 32 && ((mask >> (shift + bits)) & 1u))
			++bits;
		const u32 value = (pixel & mask) >> shift;
		if (bits >= 8)
			return (u8)(value >> (bits - 8));
		// Replicate the top bits downwards so full scale stays 255 (5 bits: v<<3 | v>>2).
		u32 out = 0;
		u32 filled = 0;
		while (filled < 8)
		{
			out = (out << bits) | value;
			filled += bits;
		}
		return (u8)(out >> (filled - 8));
	}

	//! Decodes one row of `width` pixels into A8R8G8B8.
	void decodeRow(const u8* src, u32 width, const SDdsPixelLayout& l, u32* dst)
	{
		const u32 bytes = l.BitCount / 8;
		for (u32 x = 0; x < width; ++x, src += bytes)
		{
			u32 p = 0;
			for (u32 b = 0; b < bytes; ++b)
				p |= (u32)src[b] << (8 * b);

			u8 r, g, bl, a;
			if (l.Luminance)
			{
				r = g = bl = channel8(p, l.RMask, 0);
				a = channel8(p, l.AMask, 255);
			}
			else if (l.AlphaOnly)
			{
				r = g = bl = 255;
				a = channel8(p, l.AMask, 255);
			}
			else
			{
				r = channel8(p, l.RMask, 0);
				g = channel8(p, l.GMask, 0);
				bl = channel8(p, l.BMask, 0);
				a = channel8(p, l.AMask, 255);
			}
			dst[x] = ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | bl;
		}
	}

	//! Legacy header: the FourCC formats.
	ECOLOR_FORMAT formatFromFourCC(u32 fourCC, bool& rawOnly)
	{
		rawOnly = false;
		if (fourCC == ddsFourCC('D', 'X', 'T', '1')) return ECF_DXT1;
		if (fourCC == ddsFourCC('D', 'X', 'T', '2')) return ECF_DXT2;
		if (fourCC == ddsFourCC('D', 'X', 'T', '3')) return ECF_DXT3;
		if (fourCC == ddsFourCC('D', 'X', 'T', '4')) return ECF_DXT4;
		if (fourCC == ddsFourCC('D', 'X', 'T', '5')) return ECF_DXT5;
		if (fourCC == ddsFourCC('A', 'T', 'I', '1') || fourCC == ddsFourCC('B', 'C', '4', 'U')) return ECF_BC4_U;
		if (fourCC == ddsFourCC('B', 'C', '4', 'S')) return ECF_BC4_S;
		if (fourCC == ddsFourCC('A', 'T', 'I', '2') || fourCC == ddsFourCC('B', 'C', '5', 'U')) return ECF_BC5_U;
		if (fourCC == ddsFourCC('B', 'C', '5', 'S')) return ECF_BC5_S;

		rawOnly = true;
		switch (fourCC)
		{
		case EDF_R16F:          return ECF_R16F;
		case EDF_G16R16F:       return ECF_G16R16F;
		case EDF_A16B16G16R16F: return ECF_A16B16G16R16F;
		case EDF_R32F:          return ECF_R32F;
		case EDF_G32R32F:       return ECF_G32R32F;
		case EDF_A32B32G32R32F: return ECF_A32B32G32R32F;
		default:
			rawOnly = false;
			return ECF_UNKNOWN;
		}
	}

	//! DX10 header: block-compressed and wide formats map to an ECOLOR_FORMAT, the 8/16 bit
	//! colour layouts come back as a mask layout (hasLayout) to be decided like a legacy file.
	ECOLOR_FORMAT formatFromDxgi(u32 dxgi, bool& rawOnly, SDdsPixelLayout& layout, bool& hasLayout)
	{
		rawOnly = false;
		hasLayout = false;
		layout = SDdsPixelLayout();
		switch (dxgi)
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
		default:
			break;
		}

		rawOnly = true;
		switch (dxgi)
		{
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return ECF_A16B16G16R16F;
		case DXGI_FORMAT_R32G32B32A32_FLOAT: return ECF_A32B32G32R32F;
		case DXGI_FORMAT_R32G32B32_FLOAT:    return ECF_B32G32R32F;
		case DXGI_FORMAT_R32G32_FLOAT:       return ECF_G32R32F;
		case DXGI_FORMAT_R32_FLOAT:          return ECF_R32F;
		case DXGI_FORMAT_R16G16_FLOAT:       return ECF_G16R16F;
		case DXGI_FORMAT_R16_FLOAT:          return ECF_R16F;
		case DXGI_FORMAT_R8_UNORM:           return ECF_R8;
		case DXGI_FORMAT_R8_SNORM:           return ECF_R8S;
		case DXGI_FORMAT_R8G8_UNORM:         return ECF_R8G8;
		case DXGI_FORMAT_R16_UNORM:          return ECF_R16;
		case DXGI_FORMAT_R16G16_UNORM:       return ECF_R16G16;
		case DXGI_FORMAT_R8G8B8A8_SNORM:     return ECF_A8R8G8B8S;
		default:
			break;
		}
		rawOnly = false;

		hasLayout = true;
		switch (dxgi)
		{
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			layout.BitCount = 32; layout.RMask = 0xff; layout.GMask = 0xff00; layout.BMask = 0xff0000; layout.AMask = 0xff000000;
			break;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			layout.BitCount = 32; layout.RMask = 0xff0000; layout.GMask = 0xff00; layout.BMask = 0xff; layout.AMask = 0xff000000;
			break;
		case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			layout.BitCount = 32; layout.RMask = 0xff0000; layout.GMask = 0xff00; layout.BMask = 0xff;
			break;
		case DXGI_FORMAT_B5G6R5_UNORM:
			layout.BitCount = 16; layout.RMask = 0xf800; layout.GMask = 0x7e0; layout.BMask = 0x1f;
			break;
		case DXGI_FORMAT_B5G5R5A1_UNORM:
			layout.BitCount = 16; layout.RMask = 0x7c00; layout.GMask = 0x3e0; layout.BMask = 0x1f; layout.AMask = 0x8000;
			break;
		case DXGI_FORMAT_B4G4R4A4_UNORM:
			layout.BitCount = 16; layout.RMask = 0xf00; layout.GMask = 0xf0; layout.BMask = 0xf; layout.AMask = 0xf000;
			break;
		case DXGI_FORMAT_A8_UNORM:
			layout.BitCount = 8; layout.AMask = 0xff; layout.AlphaOnly = true;
			break;
		default:
			hasLayout = false;
			break;
		}
		return ECF_UNKNOWN;
	}

	//! Legacy header without FourCC: RGB, luminance and alpha-only masks.
	bool layoutFromLegacyMasks(const ddsPixelFormat& pf, SDdsPixelLayout& layout)
	{
		layout = SDdsPixelLayout();
		const u32 bits = pf.RGBBitCount;
		if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
			return false;
		layout.BitCount = bits;
		const u32 alphaMask = (pf.Flags & DDPF_ALPHAPIXELS) ? pf.ABitMask : 0;

		if (pf.Flags & DDPF_RGB)
		{
			layout.RMask = pf.RBitMask; layout.GMask = pf.GBitMask; layout.BMask = pf.BBitMask; layout.AMask = alphaMask;
		}
		else if (pf.Flags & DDPF_LUMINANCE)
		{
			layout.Luminance = true;
			layout.RMask = pf.RBitMask; layout.AMask = alphaMask;
		}
		else if (pf.Flags & DDPF_ALPHA)
		{
			layout.AlphaOnly = true;
			layout.AMask = pf.ABitMask ? pf.ABitMask : ((bits == 32) ? 0xffffffffu : ((1u << bits) - 1));
		}
		else
			return false; // DDPF_BUMPDUDV, YUV and friends

		return (layout.RMask | layout.GMask | layout.BMask | layout.AMask) != 0;
	}

	inline u32 mipExtent(u32 base, u32 level)
	{
		const u32 e = base >> level;
		return e ? e : 1;
	}

	//! Bytes of the tightly packed payload of every surface of the file in `format`.
	u32 payloadBytes(const SDdsFile& d, ECOLOR_FORMAT format)
	{
		u32 total = 0;
		for (u32 layer = 0; layer < d.Faces * d.ArraySize; ++layer)
			for (u32 level = 0; level < d.MipLevels; ++level)
				total += IImage::getSurfaceSizeInBytes(format, mipExtent(d.Width, level), mipExtent(d.Height, level)) *
					mipExtent(d.Depth, level);
		return total;
	}

	//! Row pitch of a level-0 surface in a legacy uncompressed file: the header's when it names one
	//! that is at least tight, tight otherwise. Every other level is assumed tight, as DDSTextureLoader
	//! assumes for all of them.
	u32 legacyRowPitch(const ddsHeader& header, const SDdsFile& d, u32 level, u32 width)
	{
		const u32 tight = width * (d.Layout.BitCount / 8);
		if (level == 0 && d.HeaderBytes == sizeof(ddsHeader) && (header.Flags & DDSD_PITCH) &&
			header.PitchOrLinearSize > tight)
			return header.PitchOrLinearSize;
		return tight;
	}

	//! Header validation and geometry / format decisions. False (logged) for anything refused.
	bool describeDds(const ddsHeader& header, const DDS_HEADER_DXT10* dx10, const io::path& name, SDdsFile& d)
	{
		d = SDdsFile();
		d.HeaderBytes = dx10 ? sizeof(ddsHeader) + sizeof(DDS_HEADER_DXT10) : sizeof(ddsHeader);
		d.Width = header.Width ? header.Width : 1;
		d.Height = header.Height ? header.Height : 1;
		d.MipLevels = ((header.Flags & DDSD_MIPMAPCOUNT) && header.MipMapCount > 1) ? header.MipMapCount : 1;
		d.Volume = ((header.Flags & DDSD_DEPTH) && header.Depth > 1) || (header.Caps.caps2 & DDSCAPS2_VOLUME) != 0 ||
			(dx10 && dx10->resourceDimension == Dx10DimensionTexture3D);
		d.Depth = (d.Volume && header.Depth) ? header.Depth : 1;
		d.Faces = 1;
		d.ArraySize = 1;
		d.Format = ECF_UNKNOWN;

		if (dx10)
		{
			if (dx10->resourceDimension == Dx10DimensionTexture1D)
			{
				os::Printer::log("DDS: 1D textures are not supported", name, ELL_ERROR);
				return false;
			}
			if (dx10->miscFlag & Dx10MiscTextureCube)
				d.Faces = 6;
			if (dx10->arraySize > 1)
				d.ArraySize = dx10->arraySize;
		}
		else if (header.Caps.caps2 & DDSCAPS2_CUBEMAP)
		{
			if ((header.Caps.caps2 & DdsCaps2AllCubeFaces) != DdsCaps2AllCubeFaces)
			{
				os::Printer::log("DDS: partial cube maps (fewer than 6 faces) are not supported", name, ELL_ERROR);
				return false;
			}
			d.Faces = 6;
		}
		if (d.Volume && (d.Faces > 1 || d.ArraySize > 1))
		{
			os::Printer::log("DDS: a volume texture cannot also be a cube map or an array", name, ELL_ERROR);
			return false;
		}

		if (dx10)
			d.Format = formatFromDxgi(dx10->dxgiFormat, d.RawOnly, d.Layout, d.HasLayout);
		else if (header.PixelFormat.Flags & DDPF_FOURCC)
			d.Format = formatFromFourCC(header.PixelFormat.FourCC, d.RawOnly);
		else
			d.HasLayout = layoutFromLegacyMasks(header.PixelFormat, d.Layout);

		if (d.HasLayout)
		{
			const ECOLOR_FORMAT raw = rawPixelFormat(d.Layout);
			if (raw != ECF_UNKNOWN)
			{
				d.Format = raw;
				d.RawOnly = true;
			}
			else
			{
				const ECOLOR_FORMAT native = nativePixelFormat(d.Layout);
				d.Decode = (native == ECF_UNKNOWN);
				d.Format = d.Decode ? ECF_A8R8G8B8 : native;
			}
		}

		if (d.Format == ECF_UNKNOWN)
		{
			core::stringc what = "DDS: unsupported pixel format";
			if (dx10)
			{
				what += " (DXGI_FORMAT ";
				what += (s32)dx10->dxgiFormat;
				what += ")";
			}
			else if (header.PixelFormat.Flags & DDPF_FOURCC)
			{
				const u32 cc = header.PixelFormat.FourCC;
				what += " (FourCC ";
				if (cc >= 0x20202020u)
				{
					const c8 text[5] = { (c8)(cc & 0xff), (c8)((cc >> 8) & 0xff), (c8)((cc >> 16) & 0xff), (c8)((cc >> 24) & 0xff), 0 };
					what += text;
				}
				else
					what += (s32)cc;
				what += ")";
			}
			else
			{
				what += " (";
				what += (s32)header.PixelFormat.RGBBitCount;
				what += " bit masks)";
			}
			os::Printer::log(what.c_str(), name, ELL_ERROR);
			return false;
		}
		return true;
	}

	//! The raw file as a "compressed" CImage; takes ownership of `bytes` (deleted on failure).
	IImage* takeRawFile(u8* bytes, u32 byteCount, const SDdsFile& d, const io::path& name)
	{
		if (byteCount < d.HeaderBytes + payloadBytes(d, d.Format))
		{
			os::Printer::log("DDS: file is shorter than its header claims", name, ELL_ERROR);
			delete[] bytes;
			return 0;
		}
		CImage* image = new CImage(d.Format, core::dimension2d<u32>(d.Width, d.Height), bytes, true, true, true, d.MipLevels);
		image->CompressedSize = byteCount;
		return image;
	}

	//! Plain CImage of the top surface of a 2D uncompressed file; frees `bytes`.
	IImage* loadTopSurface(u8* bytes, u32 byteCount, const SDdsFile& d, const ddsHeader& header, const io::path& name)
	{
		const u32 srcBytesPerPixel = d.Layout.BitCount / 8;
		const u32 srcPitch = legacyRowPitch(header, d, 0, d.Width);
		const u32 needed = d.HeaderBytes + srcPitch * (d.Height - 1) + d.Width * srcBytesPerPixel;
		if (byteCount < needed)
		{
			os::Printer::log("DDS: file is shorter than its header claims", name, ELL_ERROR);
			delete[] bytes;
			return 0;
		}

		CImage* image = new CImage(d.Format, core::dimension2d<u32>(d.Width, d.Height));
		u8* dst = (u8*)image->lock();
		const u32 dstPitch = image->getPitch();
		const u8* src = bytes + d.HeaderBytes;
		for (u32 y = 0; y < d.Height; ++y, src += srcPitch, dst += dstPitch)
		{
			if (d.Decode)
				decodeRow(src, d.Width, d.Layout, (u32*)dst);
			else
				memcpy(dst, src, d.Width * srcBytesPerPixel);
		}
		image->unlock();
		delete[] bytes;
		return image;
	}

	//! Repacks an uncompressed cube map / array / volume into a tightly pitched file in d.Format,
	//! decoding each row when the source layout is not native; frees `bytes`.
	IImage* repackRawFile(u8* bytes, u32 byteCount, const SDdsFile& d, const ddsHeader& header, const io::path& name)
	{
		const u32 srcBytesPerPixel = d.Layout.BitCount / 8;
		const u32 dstBytesPerPixel = IImage::getBitsPerPixelFromFormat(d.Format) / 8;
		const u32 outBytes = d.HeaderBytes + payloadBytes(d, d.Format);
		u8* out = new u8[outBytes];

		// Header: geometry and caps as they were, pixel format replaced by the canonical one.
		memcpy(out, bytes, d.HeaderBytes);
		ddsHeader* outHeader = (ddsHeader*)out;
		outHeader->Flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH |
			(d.MipLevels > 1 ? DDSD_MIPMAPCOUNT : 0) | (d.Volume ? DDSD_DEPTH : 0);
		outHeader->PitchOrLinearSize = d.Width * dstBytesPerPixel;
		outHeader->MipMapCount = d.MipLevels;
		outHeader->Depth = d.Volume ? d.Depth : 0;
		if (d.HeaderBytes > sizeof(ddsHeader))
			((DDS_HEADER_DXT10*)(out + sizeof(ddsHeader)))->dxgiFormat = dxgiFormatOf(d.Format);
		else
			writeLegacyPixelFormat(outHeader->PixelFormat, d.Format);

		const u8* src = bytes + d.HeaderBytes;
		const u8* srcEnd = bytes + byteCount;
		u8* dst = out + d.HeaderBytes;
		for (u32 layer = 0; layer < d.Faces * d.ArraySize; ++layer)
		{
			for (u32 level = 0; level < d.MipLevels; ++level)
			{
				const u32 width = mipExtent(d.Width, level);
				const u32 height = mipExtent(d.Height, level);
				const u32 depth = mipExtent(d.Depth, level);
				const u32 srcPitch = legacyRowPitch(header, d, level, width);
				for (u32 slice = 0; slice < depth; ++slice)
				{
					for (u32 y = 0; y < height; ++y, src += srcPitch, dst += width * dstBytesPerPixel)
					{
						if (src + width * srcBytesPerPixel > srcEnd)
						{
							os::Printer::log("DDS: file is shorter than its header claims", name, ELL_ERROR);
							delete[] out;
							delete[] bytes;
							return 0;
						}
						if (d.Decode)
							decodeRow(src, width, d.Layout, (u32*)dst);
						else
							memcpy(dst, src, width * srcBytesPerPixel);
					}
				}
			}
		}
		delete[] bytes;

		CImage* image = new CImage(d.Format, core::dimension2d<u32>(d.Width, d.Height), out, true, true, true, d.MipLevels);
		image->CompressedSize = outBytes;
		return image;
	}
}

//! creates a surface from the file
IImage* CImageLoaderDDS::loadImage(io::IReadFile* file) const
{
	const io::path& name = file->getFileName();
	const u32 byteCount = (u32)file->getSize();
	if (byteCount < sizeof(ddsHeader))
	{
		os::Printer::log("DDS: file too short for a header", name, ELL_ERROR);
		return 0;
	}

	u8* bytes = new u8[byteCount];
	file->seek(0);
	if ((u32)file->read(bytes, byteCount) != byteCount)
	{
		os::Printer::log("DDS: could not read the file", name, ELL_ERROR);
		delete[] bytes;
		return 0;
	}

	ddsHeader header;
	memcpy(&header, bytes, sizeof(header));
	if (memcmp(header.Magic, "DDS ", 4) != 0 || header.Size != 124 ||
		!(header.Flags & DDSD_PIXELFORMAT) || !(header.Flags & DDSD_CAPS))
	{
		os::Printer::log("DDS: not a .dds header", name, ELL_ERROR);
		delete[] bytes;
		return 0;
	}

	DDS_HEADER_DXT10 dx10;
	const bool hasDx10 = (header.PixelFormat.Flags & DDPF_FOURCC) && header.PixelFormat.FourCC == ddsFourCC('D', 'X', '1', '0');
	if (hasDx10)
	{
		if (byteCount < sizeof(ddsHeader) + sizeof(DDS_HEADER_DXT10))
		{
			os::Printer::log("DDS: file too short for its DX10 header", name, ELL_ERROR);
			delete[] bytes;
			return 0;
		}
		memcpy(&dx10, bytes + sizeof(ddsHeader), sizeof(dx10));
	}

	SDdsFile d;
	if (!describeDds(header, hasDx10 ? &dx10 : 0, name, d))
	{
		delete[] bytes;
		return 0;
	}

	if (IImage::isCompressedFormat(d.Format))
		return takeRawFile(bytes, byteCount, d, name);

	const bool multiSurface = d.Faces > 1 || d.ArraySize > 1 || d.Volume;
	if (d.RawOnly)
	{
		// A padded legacy file has to be repacked, the drivers read every level tightly.
		if (d.HasLayout && legacyRowPitch(header, d, 0, d.Width) != d.Width * (d.Layout.BitCount / 8))
			return repackRawFile(bytes, byteCount, d, header, name);
		return takeRawFile(bytes, byteCount, d, name);
	}

	if (multiSurface)
	{
		// No driver stores 24 bit texels; widen to A8R8G8B8 while repacking.
		if (d.Format == ECF_R8G8B8)
		{
			d.Format = ECF_A8R8G8B8;
			d.Decode = true;
		}
		return repackRawFile(bytes, byteCount, d, header, name);
	}

	return loadTopSurface(bytes, byteCount, d, header, name);
}

#endif // _IRR_COMPILE_WITH_DDS_DECODER_LOADER_


//! creates a loader which is able to load dds images
IImageLoader* createImageLoaderDDS()
{
	return new CImageLoaderDDS();
}


} // end namespace video
} // end namespace irr

#endif

