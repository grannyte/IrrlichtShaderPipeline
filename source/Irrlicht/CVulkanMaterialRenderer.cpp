// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanMaterialRenderer.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_
#include "CVulkanDefaultShaders.h"
#include "os.h"

namespace irr
{
	namespace video
	{
		namespace
		{
			//! One built-in E_MATERIAL_TYPE: the embedded shaders it draws with and how it blends. The
			//! row index must equal the type value -- the driver registers the rows in sequence.
			struct SBuiltInMaterialDesc
			{
				E_MATERIAL_TYPE Type;
				const c8* VertexShader;
				const c8* FragmentShader;
				//! Both null except on the 12 multi-texture types, which get a second UV set variant.
				const c8* VertexShader2TCoords;
				const c8* FragmentShaderUV2;
				SVulkanPipelineKey::EBlendMode BlendMode;
			};

			const c8* const StandardVS = "standard.vert";
			const c8* const Standard2TCoordsVS = "standard_2tcoords.vert";
			const c8* const TangentsVS = "tangents.vert"; // EVT_TANGENTS, the 6 normal/parallax types only

			//! Exhaustive over EMT_SOLID..EMT_ONETEXTURE_BLEND: no type falls through to a default.
			const SBuiltInMaterialDesc BuiltInMaterials[] =
			{
				{ EMT_SOLID, StandardVS, "solid.frag", 0, 0, SVulkanPipelineKey::EBlendMode::None },
				{ EMT_SOLID_2_LAYER, StandardVS, "solid_2layer.frag",
					Standard2TCoordsVS, "solid_2layer_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_LIGHTMAP, StandardVS, "lightmap.frag",
					Standard2TCoordsVS, "lightmap_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_LIGHTMAP_ADD, StandardVS, "lightmap_add.frag",
					Standard2TCoordsVS, "lightmap_add_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_LIGHTMAP_M2, StandardVS, "lightmap_m2.frag",
					Standard2TCoordsVS, "lightmap_m2_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_LIGHTMAP_M4, StandardVS, "lightmap_m4.frag",
					Standard2TCoordsVS, "lightmap_m4_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_LIGHTMAP_LIGHTING, StandardVS, "lightmap_lighting.frag",
					Standard2TCoordsVS, "lightmap_lighting_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_LIGHTMAP_LIGHTING_M2, StandardVS, "lightmap_lighting_m2.frag",
					Standard2TCoordsVS, "lightmap_lighting_m2_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_LIGHTMAP_LIGHTING_M4, StandardVS, "lightmap_lighting_m4.frag",
					Standard2TCoordsVS, "lightmap_lighting_m4_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_DETAIL_MAP, StandardVS, "detail_map.frag",
					Standard2TCoordsVS, "detail_map_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_SPHERE_MAP, StandardVS, "sphere_map.frag",
					Standard2TCoordsVS, "sphere_map_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				{ EMT_REFLECTION_2_LAYER, StandardVS, "reflection_2layer.frag",
					Standard2TCoordsVS, "reflection_2layer_uv2.frag", SVulkanPipelineKey::EBlendMode::None },
				// The two plain transparent types reuse the solid shader; only the blend differs.
				{ EMT_TRANSPARENT_ADD_COLOR, StandardVS, "solid.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::AddColor },
				{ EMT_TRANSPARENT_ALPHA_CHANNEL, StandardVS, "solid.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::AlphaBlend },
				// Alpha-test, NOT blending: punched through, not translucent, so isTransparent() is false.
				{ EMT_TRANSPARENT_ALPHA_CHANNEL_REF, StandardVS, "alpha_test.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::None },
				{ EMT_TRANSPARENT_VERTEX_ALPHA, StandardVS, "vertex_alpha.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::AlphaBlend },
				{ EMT_TRANSPARENT_REFLECTION_2_LAYER, StandardVS, "transparent_reflection_2layer.frag",
					Standard2TCoordsVS, "transparent_reflection_2layer_uv2.frag",
					SVulkanPipelineKey::EBlendMode::AlphaBlend },
				// Normal/parallax map, per-pixel lighting. _TRANSPARENT_ADD_COLOR reuses the opaque
				// shader, _TRANSPARENT_VERTEX_ALPHA takes its alpha from the vertex instead.
				{ EMT_NORMAL_MAP_SOLID, TangentsVS, "normal_map.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::None },
				{ EMT_NORMAL_MAP_TRANSPARENT_ADD_COLOR, TangentsVS, "normal_map.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::AddColor },
				{ EMT_NORMAL_MAP_TRANSPARENT_VERTEX_ALPHA, TangentsVS, "normal_map_vertex_alpha.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::AlphaBlend },
				{ EMT_PARALLAX_MAP_SOLID, TangentsVS, "parallax_map.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::None },
				{ EMT_PARALLAX_MAP_TRANSPARENT_ADD_COLOR, TangentsVS, "parallax_map.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::AddColor },
				{ EMT_PARALLAX_MAP_TRANSPARENT_VERTEX_ALPHA, TangentsVS, "parallax_map_vertex_alpha.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::AlphaBlend },
				// Fallback value only: the real factors are per-instance, decoded by the driver.
				{ EMT_ONETEXTURE_BLEND, StandardVS, "solid.frag", 0, 0,
					SVulkanPipelineKey::EBlendMode::Custom },
			};

			const size_t BuiltInMaterialCount = sizeof(BuiltInMaterials) / sizeof(BuiltInMaterials[0]);
		}

		const VkDescriptorSetLayoutBinding* getVulkanDriverUniformBindings(u32& outCount)
		{
			// Hand-written union, never reflected -- see the header. Stage flags follow where the GLSL
			// reads each block: matrices in the vertex stage, clip planes and fog in the fragment one.
			static const VkDescriptorSetLayoutBinding bindings[EVDU_COUNT] =
			{
				{ EVDU_PER_OBJECT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
				{ EVDU_PER_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr },
				{ EVDU_CLIP_PLANES, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
				{ EVDU_LIGHTING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
				{ EVDU_FOG, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
			};

			outCount = EVDU_COUNT;
			return bindings;
		}

		const VkDescriptorSetLayoutBinding* getVulkanMaterialTextureBindings(u32& outCount)
		{
			// Fixed for the same reason: a single-texture shader declares no Layer1Texture, yet every
			// built-in pipeline has to agree on one set 0 layout.
			static const VkDescriptorSetLayoutBinding bindings[VulkanMaterialTextureBindingCount] =
			{
				{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
				{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
			};

			outCount = VulkanMaterialTextureBindingCount;
			return bindings;
		}

		VkShaderModule CVulkanShaderModuleCache::getOrCreate(const SVulkanContext& context, const c8* name)
		{
			if (!name)
				return VK_NULL_HANDLE;

			auto cached = Modules.find(name);
			if (cached != Modules.end())
				return cached->second;

			// A miss here is a programming error, not a runtime condition: the table is compiled in.
			const SVulkanDefaultShader* shader = getVulkanDefaultShader(name);
			if (!shader)
			{
				core::stringc message = "CVulkanShaderModuleCache: shader inconnu dans la table integree : ";
				message += name;
				os::Printer::log(message.c_str(), ELL_ERROR);
				return VK_NULL_HANDLE;
			}

			if (!context.Device)
			{
				os::Printer::log("CVulkanShaderModuleCache::getOrCreate: pas de device", ELL_ERROR);
				return VK_NULL_HANDLE;
			}

			VkShaderModuleCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			info.codeSize = shader->CodeSize; // bytes, not words
			info.pCode = shader->Code;

			VkShaderModule module = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanShaderModuleCache: vkCreateShaderModule",
				vk::CreateShaderModule(context.Device, &info, nullptr, &module)))
				return VK_NULL_HANDLE;

			Device = context.Device;
			Modules[name] = module;
			return module;
		}

		void CVulkanShaderModuleCache::clear()
		{
			if (Device && vk::DestroyShaderModule)
			{
				for (auto& entry : Modules)
					vk::DestroyShaderModule(Device, entry.second, nullptr);
			}
			Modules.clear();
			Device = VK_NULL_HANDLE;
		}

		bool createVulkanBuiltInMaterialRenderers(const SVulkanContext& context,
			CVulkanShaderModuleCache& moduleCache,
			std::vector<CVulkanMaterialRenderer*>& outRenderers)
		{
			const size_t firstIndex = outRenderers.size();

			for (size_t i = 0; i < BuiltInMaterialCount; ++i)
			{
				const SBuiltInMaterialDesc& desc = BuiltInMaterials[i];

				// Registration order reproduces the E_MATERIAL_TYPE values, as on every backend.
				if (static_cast<size_t>(desc.Type) != firstIndex + i)
				{
					os::Printer::log("CVulkanMaterialRenderer: la table des materiaux integres est "
						"desynchronisee de E_MATERIAL_TYPE", ELL_ERROR);
					return false;
				}

				CVulkanMaterialRenderer* renderer = new CVulkanMaterialRenderer();
				renderer->BaseMaterialType = desc.Type;
				renderer->BlendMode = desc.BlendMode;
				renderer->VertexShaderName = desc.VertexShader;
				renderer->FragmentShaderName = desc.FragmentShader;
				renderer->VS = moduleCache.getOrCreate(context, desc.VertexShader);
				renderer->PS = moduleCache.getOrCreate(context, desc.FragmentShader);

				// The 2TCoords entries are declared as a pair, so one without the other is an error.
				if (desc.VertexShader2TCoords && desc.FragmentShaderUV2)
				{
					renderer->VS2TCoords = moduleCache.getOrCreate(context, desc.VertexShader2TCoords);
					renderer->PS2TCoords = moduleCache.getOrCreate(context, desc.FragmentShaderUV2);
				}

				const bool ok = renderer->VS != VK_NULL_HANDLE && renderer->PS != VK_NULL_HANDLE &&
					(!desc.VertexShader2TCoords || renderer->has2TCoordsVariant());
				if (!ok)
				{
					renderer->drop();
					os::Printer::log("CVulkanMaterialRenderer: creation d'un module SPIR-V integre a "
						"echoue — createVulkanBuiltInMaterialRenderers() a echoue", ELL_ERROR);
					return false;
				}

				outRenderers.push_back(renderer); // one reference, for the caller to drop()
			}

			return true;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
