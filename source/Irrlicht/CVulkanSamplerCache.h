// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Cache of immutable VkSamplers, keyed on everything that determines one (filtering, anisotropy,
// addressing, LOD bias) - an SMaterialLayer plus SMaterial::UseMipMaps. Driver-owned and shared by
// every draw, the counterpart of CD3D12Driver::allocateSamplerTableSlot()/SamplerCombinationCache.

#ifndef __C_VULKAN_SAMPLER_CACHE_H_INCLUDED__
#define __C_VULKAN_SAMPLER_CACHE_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "irrTypes.h"
#include "SMaterialLayer.h"
#include "CVulkanHelpers.h"
#include <unordered_map>

namespace irr
{
	namespace video
	{
		//! Everything that determines a VkSampler. Two layers with an equal SVulkanSamplerKey are
		//! drawn with the very same sampler object.
		struct SVulkanSamplerKey
		{
			//! Both off means point sampling; trilinear wins over bilinear when both are set.
			bool BilinearFilter = false;
			bool TrilinearFilter = false;
			//! Maximum anisotropy degree, 0 disabled. Unclamped, so the key stays device-independent.
			u8 AnisotropicFilter = 0;
			//! E_TEXTURE_CLAMP values, u8 as SMaterialLayer stores them; W has no field there.
			u8 TextureWrapU = ETC_REPEAT;
			u8 TextureWrapV = ETC_REPEAT;
			u8 TextureWrapW = ETC_REPEAT;
			//! Raw eighths-of-a-mip-level units; the scaling to mipLodBias belongs to creation.
			s8 LODBias = 0;
			//! SMaterial::UseMipMaps, not a layer field: it caps maxLod to 0, so it is part of the key.
			bool UseMipMaps = true;

			bool operator==(const SVulkanSamplerKey& other) const
			{
				return BilinearFilter == other.BilinearFilter &&
					TrilinearFilter == other.TrilinearFilter &&
					AnisotropicFilter == other.AnisotropicFilter &&
					TextureWrapU == other.TextureWrapU &&
					TextureWrapV == other.TextureWrapV &&
					TextureWrapW == other.TextureWrapW &&
					LODBias == other.LODBias &&
					UseMipMaps == other.UseMipMaps;
			}

			//! Same idiom as SVulkanPipelineKey. Colliding keys share a sampler, uncorrected.
			size_t computeHash() const
			{
				size_t h = static_cast<size_t>(BilinearFilter);
				auto combine = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
				combine(static_cast<size_t>(TrilinearFilter));
				combine(static_cast<size_t>(AnisotropicFilter));
				combine(static_cast<size_t>(TextureWrapU));
				combine(static_cast<size_t>(TextureWrapV));
				combine(static_cast<size_t>(TextureWrapW));
				// s8 -> size_t sign-extends; through u8 keeps both bias directions distinct.
				combine(static_cast<size_t>(static_cast<u8>(LODBias)));
				combine(static_cast<size_t>(UseMipMaps));
				return h;
			}
		};

		//! Maps an E_TEXTURE_CLAMP (u8 as SMaterialLayer stores it) the way CVulkanTexture does.
		VkSamplerAddressMode getVulkanAddressMode(u8 clamp);

		//! Sampler cache. One per driver: samplers are immutable, so one handle serves any number of
		//! draws and descriptor sets still in flight - what a per-texture sampler cannot do, since
		//! changing its settings means destroying and recreating it.
		class CVulkanSamplerCache
		{
		public:
			//! Returns the existing sampler for this layer, or creates one (VK_NULL_HANDLE on failure,
			//! already logged). Owned by the cache and valid until clear(): callers hold copies and
			//! never destroy them. useMipMaps is SMaterial::UseMipMaps.
			VkSampler getOrCreate(const SVulkanContext& context, const SMaterialLayer& layer, bool useMipMaps);

			//! Destroys every cached sampler; no command buffer referencing one may still be in flight.
			void clear(const SVulkanContext& context);

			~CVulkanSamplerCache() { destroyAll(); }

			size_t size() const { return Cache.size(); }

		private:
			//! Body of clear() on the device captured at creation - the destructor gets no context.
			void destroyAll();

			//! Distinct combinations a scene is expected to use: the D3D12 side sizes its
			//! shader-visible sampler heap for ~64, so past that a one-off warning is worth printing.
			static const size_t ExpectedSamplerCount = 64;
			//! Hard cap, 16x the above and far under the 4000 samplers an implementation must support.
			static const size_t MaxCachedSamplers = 1024;

			//! Captured on first creation, like CVulkanPipelineLayoutCache::clear() does.
			VkDevice Device = VK_NULL_HANDLE;
			bool CountWarned = false; //!< Keeps the "unusually many combinations" warning to one line.
			bool CapWarned = false; //!< Same, for the hard cap.
			std::unordered_map<size_t, VkSampler> Cache;
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
