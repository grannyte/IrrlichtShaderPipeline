// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// A registered built-in material -- one E_MATERIAL_TYPE, same role as CD3D12MaterialRenderer on the
// D3D12 side. Only built-in types live here: addHighLevelShaderMaterial() is not served by this
// backend, so there is no shader compiler and no reflection anywhere below. A renderer carries the
// SPIR-V modules its type draws with plus the blend state baked at registration, both read directly
// by CVulkanDriver when it builds an SVulkanPipelineKey; the modules are borrowed from a
// CVulkanShaderModuleCache, so all 24 renderers share a handful of them. OnSetMaterial()/OnRender()
// keep their empty inherited body -- Vulkan needs all state up front at pipeline-creation time.
// isTransparent() IS wired up: scene code sorts solid vs. transparent nodes on it.

#ifndef __C_VULKAN_MATERIAL_RENDERER_H_INCLUDED__
#define __C_VULKAN_MATERIAL_RENDERER_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include <vector>
#include <string>
#include <unordered_map>

#include "IVertexDescriptor.h" // IMaterialRenderer::OnRender() needs this, not included by IMaterialRenderer.h itself
#include "IMaterialRenderer.h"
#include "irrString.h"
#include "CVulkanHelpers.h"
#include "CVulkanPipelineCache.h" // SVulkanPipelineKey::EBlendMode, DriverDescriptorSet

namespace irr
{
	namespace video
	{
		//! Uniform buffers the built-in shaders declare in set DriverDescriptorSet, at these bindings.
		enum E_VULKAN_DRIVER_UNIFORM
		{
			EVDU_PER_OBJECT = 0, //!< World matrix; EVDU_PER_FRAME is view/projection/camera position.
			EVDU_PER_FRAME,
			EVDU_CLIP_PLANES,
			EVDU_LIGHTING,
			EVDU_FOG,
			EVDU_COUNT
		};

		//! Set 0: SMaterial::TextureLayer[0] and [1] as combined image samplers. The driver binds a
		//! default texture on layer 1 for single-texture materials, keeping the set complete.
		static const u32 VulkanMaterialTextureSet = 0;
		static const u32 VulkanMaterialTextureBindingCount = 2;

		//! Bindings of set DriverDescriptorSet, as a FIXED table: glslang drops a uniform block a given
		//! shader never reads, so per-shader reflection would yield mutually incompatible layouts.
		const VkDescriptorSetLayoutBinding* getVulkanDriverUniformBindings(u32& outCount);

		const VkDescriptorSetLayoutBinding* getVulkanMaterialTextureBindings(u32& outCount);

		//! VkShaderModules for the embedded SPIR-V of CVulkanDefaultShaders.h, keyed on source file
		//! name ("solid.frag"), which also makes SVulkanPipelineKey's "hash the handle" shortcut
		//! correct: one name always yields one handle. Owns every module it returns.
		class CVulkanShaderModuleCache
		{
		public:
			//! Returns the module for this default shader name, creating it on first use. VK_NULL_HANDLE
			//! if the name is absent from VulkanDefaultShaders[] or creation failed (logged).
			VkShaderModule getOrCreate(const SVulkanContext& context, const c8* name);

			void clear(); //!< Destroys every module; the pipelines built from them must already be gone.

			~CVulkanShaderModuleCache() { clear(); }

			size_t size() const { return Modules.size(); }

		private:
			VkDevice Device = VK_NULL_HANDLE; //!< Captured on first creation, so clear() takes no argument.
			std::unordered_map<std::string, VkShaderModule> Modules;
		};

		class CVulkanMaterialRenderer : public IMaterialRenderer
		{
		public:
			//! EVT_STANDARD/EVT_TANGENTS form, borrowed from a cache that outlives the renderer.
			VkShaderModule VS = VK_NULL_HANDLE;
			VkShaderModule PS = VK_NULL_HANDLE;
			//! EVT_2TCOORDS variant, VK_NULL_HANDLE except on the 12 multi-texture built-ins.
			VkShaderModule VS2TCoords = VK_NULL_HANDLE;
			VkShaderModule PS2TCoords = VK_NULL_HANDLE;

			//! Blend state baked at registration and read when the driver builds a pipeline key, except
			//! for EMT_ONETEXTURE_BLEND, whose factors are per-instance and decoded there.
			SVulkanPipelineKey::EBlendMode BlendMode = SVulkanPipelineKey::EBlendMode::None;
			VkBlendFactor CustomSrcColorFactor = VK_BLEND_FACTOR_ONE;
			VkBlendFactor CustomDstColorFactor = VK_BLEND_FACTOR_ZERO;
			VkBlendFactor CustomSrcAlphaFactor = VK_BLEND_FACTOR_ONE;
			VkBlendFactor CustomDstAlphaFactor = VK_BLEND_FACTOR_ZERO;
			VkBlendOp CustomBlendOp = VK_BLEND_OP_ADD;

			//! The type this renderer was built for, hence also its index in the driver's registry.
			E_MATERIAL_TYPE BaseMaterialType = EMT_SOLID;

			core::stringc VertexShaderName; //!< Diagnostic: a module handle says nothing in a debugger.
			core::stringc FragmentShaderName;

			CVulkanMaterialRenderer() = default;
			virtual ~CVulkanMaterialRenderer() {}

			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return BlendMode != SVulkanPipelineKey::EBlendMode::None;
			}

			//! Falls back to the shared form when this material has no EVT_2TCOORDS variant, as below.
			VkShaderModule getVertexModule(bool use2TCoords) const
			{
				return (use2TCoords && VS2TCoords != VK_NULL_HANDLE) ? VS2TCoords : VS;
			}

			VkShaderModule getFragmentModule(bool use2TCoords) const
			{
				return (use2TCoords && PS2TCoords != VK_NULL_HANDLE) ? PS2TCoords : PS;
			}

			bool has2TCoordsVariant() const
			{
				return VS2TCoords != VK_NULL_HANDLE && PS2TCoords != VK_NULL_HANDLE;
			}
		};

		//! Builds every built-in renderer in E_MATERIAL_TYPE order (EMT_SOLID .. EMT_ONETEXTURE_BLEND),
		//! appending them to `outRenderers` with one reference each for the caller to drop() once
		//! addMaterialRenderer() has grabbed it. `moduleCache` must outlive them; false (logged) on a
		//! missing shader name or a module that fails to create.
		bool createVulkanBuiltInMaterialRenderers(const SVulkanContext& context,
			CVulkanShaderModuleCache& moduleCache,
			std::vector<CVulkanMaterialRenderer*>& outRenderers);

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
