#pragma once

#include "IReferenceCounted.h"
#include "irrTypes.h"

namespace irr
{
	namespace video
	{
		class ITexture;

		//! One mip level's tile dimensions in texels, from IVideoDriver::getTileShape().
		struct STileShape
		{
			u32 WidthInTexels;
			u32 HeightInTexels;
			u32 DepthInTexels;
		};

		//! A texture subregion in tile coordinates, for updateTileMappings()/updateTiles().
		struct STileRegion
		{
			u32 MipLevel;
			u32 ArraySlice;
			u32 TileX, TileY, TileZ;
			u32 TileCountX, TileCountY, TileCountZ;
		};

		//! Physical tile memory a tiled texture's regions are mapped to, see IVideoDriver::createTilePool().
		class ITilePool : public virtual IReferenceCounted
		{
		public:
			//! Number of tiles this pool backs, as passed to createTilePool().
			virtual u32 getTileCount() const = 0;
		};

	} // end namespace video
} // end namespace irr
