// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanSamplerCache.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_
#include "irrMath.h"
#include "os.h"

namespace irr
{
	namespace video
	{
		VkSamplerAddressMode getVulkanAddressMode(u8 clamp)
		{
			switch (clamp)
			{
			case ETC_REPEAT:                   return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			case ETC_CLAMP:
			case ETC_CLAMP_TO_EDGE:            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case ETC_CLAMP_TO_BORDER:          return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			case ETC_MIRROR:                   return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			// MIRROR_CLAMP_TO_EDGE needs the optional samplerMirrorClampToEdge feature, which
			// SVulkanContext does not report, so the plain clamp stays the safe approximation.
			case ETC_MIRROR_CLAMP:
			case ETC_MIRROR_CLAMP_TO_EDGE:
			case ETC_MIRROR_CLAMP_TO_BORDER:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			default:                           return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			}
		}

		VkSampler CVulkanSamplerCache::getOrCreate(const SVulkanContext& context,
			const SMaterialLayer& layer, bool useMipMaps)
		{
			SVulkanSamplerKey key;
			key.BilinearFilter = layer.BilinearFilter;
			key.TrilinearFilter = layer.TrilinearFilter;
			key.AnisotropicFilter = layer.AnisotropicFilter;
			key.TextureWrapU = layer.TextureWrapU;
			key.TextureWrapV = layer.TextureWrapV;
			// SMaterialLayer has no W field; the W axis follows U, as it does on the D3D12 side.
			key.TextureWrapW = layer.TextureWrapU;
			key.LODBias = layer.LODBias;
			key.UseMipMaps = useMipMaps;
			// The reduction mode only when the device offers it; a layer asking without it samples
			// the ordinary average (warned by the driver's queryFeature() contract, not here).
			key.MinMaxFilter = context.HasSamplerFilterMinmax ? layer.MinMaxFilter : (u8)ETMINF_AVERAGE;
			key.MinLodEighths = static_cast<u16>(core::clamp(layer.MinLod * 8.f, 0.f, 8.f * 15.f));

			const size_t hash = key.computeHash();
			auto cached = Cache.find(hash);
			if (cached != Cache.end())
				return cached->second;

			if (!context.Device)
			{
				os::Printer::log("CVulkanSamplerCache::getOrCreate: no device", ELL_ERROR);
				return VK_NULL_HANDLE;
			}

			if (Cache.size() >= MaxCachedSamplers)
			{
				if (!CapWarned)
				{
					CapWarned = true;
					os::Printer::log("CVulkanSamplerCache: sampler cap reached, no further "
						"combination will be created (see MaxCachedSamplers)", ELL_WARNING);
				}
				return VK_NULL_HANDLE;
			}

			VkSamplerCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			// Same bilinear > trilinear > anisotropic priority as buildD3D12SamplerDesc(), NOT the
			// "linear if either flag is set" reading. It matters for the DEFAULT SMaterialLayer,
			// which is Bilinear=false/Trilinear=false/Anisotropic=16: under the priority rule that
			// is anisotropic filtering on both backends, whereas treating the two bools as the only
			// source of linear filtering would point-sample every texture in a default scene.
			const bool anisotropicOnly = !key.BilinearFilter && !key.TrilinearFilter && key.AnisotropicFilter > 0;
			info.magFilter = (key.BilinearFilter || key.TrilinearFilter || anisotropicOnly) ?
				VK_FILTER_LINEAR : VK_FILTER_NEAREST;
			info.minFilter = info.magFilter;
			// Bilinear takes mip-point, trilinear mip-linear; anisotropic gets mip-linear too, the
			// closest match to D3D12_FILTER_ANISOTROPIC.
			info.mipmapMode = (key.TrilinearFilter || anisotropicOnly) ?
				VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
			info.addressModeU = getVulkanAddressMode(key.TextureWrapU);
			info.addressModeV = getVulkanAddressMode(key.TextureWrapV);
			info.addressModeW = getVulkanAddressMode(key.TextureWrapW);
			info.compareEnable = VK_FALSE;
			info.compareOp = VK_COMPARE_OP_ALWAYS;
			info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			info.maxAnisotropy = 1.0f;
			// SMaterialLayer::MinLod: the finest mip the sampler may reach (tiled textures whose
			// finer mips are not resident yet). Meaningless with mip maps off.
			info.minLod = key.UseMipMaps ? static_cast<f32>(key.MinLodEighths) * 0.125f : 0.0f;
			// Mip maps off means mip 0 only, whatever LOD the hardware computes.
			info.maxLod = key.UseMipMaps ? VK_LOD_CLAMP_NONE : 0.0f;

			// SMaterialLayer::MinMaxFilter: min/max reduction over the footprint (1.2 core structure,
			// the EXT alias has the same layout). Only keyed when the device offers it.
			VkSamplerReductionModeCreateInfo reduction = {};
			reduction.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
			reduction.reductionMode = key.MinMaxFilter == ETMINF_MINIMUM ?
				VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
			if (key.MinMaxFilter != ETMINF_AVERAGE)
				info.pNext = &reduction;
			// Same eighths-of-a-level scaling as the other drivers; maxSamplerLodBias is a limit,
			// not a suggestion, and an s8 bias can reach ~15.9.
			const f32 maxBias = context.DeviceProperties.limits.maxSamplerLodBias;
			info.mipLodBias = core::clamp(static_cast<f32>(key.LODBias) * 0.125f, -maxBias, maxBias);

			// samplerAnisotropy is an optional device feature; maxSamplerAnisotropy stays at 1.0
			// when it was not enabled, which also caps the requested level.
			// Only the anisotropic branch of that priority actually enables anisotropy: an explicit
			// Bilinear/Trilinear request wins and pins MaxAnisotropy to 1, as on the D3D12 side.
			const f32 maxSupported = context.DeviceProperties.limits.maxSamplerAnisotropy;
			if (anisotropicOnly && maxSupported > 1.0f)
			{
				info.anisotropyEnable = VK_TRUE;
				info.maxAnisotropy = core::max_(1.0f, core::min_(static_cast<f32>(key.AnisotropicFilter), maxSupported));
			}

			VkSampler sampler = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanSamplerCache: vkCreateSampler",
				vk::CreateSampler(context.Device, &info, nullptr, &sampler)))
				return VK_NULL_HANDLE;

			Device = context.Device;
			Cache[hash] = sampler;

			if (Cache.size() > ExpectedSamplerCount && !CountWarned)
			{
				CountWarned = true;
				os::Printer::log("CVulkanSamplerCache: unusually many distinct sampler combinations",
					core::stringc(static_cast<u32>(Cache.size())).c_str(), ELL_WARNING);
			}
			return sampler;
		}

		void CVulkanSamplerCache::clear(const SVulkanContext& context)
		{
			if (context.Device)
				Device = context.Device;
			destroyAll();
		}

		void CVulkanSamplerCache::destroyAll()
		{
			if (Device)
			{
				for (auto& entry : Cache)
					vk::DestroySampler(Device, entry.second, nullptr);
			}
			Cache.clear();
			Device = VK_NULL_HANDLE;
			CountWarned = CapWarned = false;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
