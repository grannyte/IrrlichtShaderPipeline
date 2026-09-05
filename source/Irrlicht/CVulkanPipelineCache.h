// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Cache of graphics pipelines, keyed on everything that determines a VkPipeline (shader modules,
// pipeline layout, blend, depth/stencil, rasterization, vertex input, attachment formats), plus the
// layout caches those pipelines are built against. Generic - not tied to any material system.

#ifndef __C_VULKAN_PIPELINE_CACHE_H_INCLUDED__
#define __C_VULKAN_PIPELINE_CACHE_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "irrTypes.h"
#include "CVulkanHelpers.h"
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace irr
{
	namespace video
	{
		//! Descriptor set holding the driver's own uniforms. Sets 0..3 belong to user shaders, which are
		//! authored for D3D11 and declare "register(bN)" - register space 0..3, one space per set once
		//! compiled to SPIR-V. Same split, for the same reason, as the DriverConstantRegisterSpace
		//! CD3D12MaterialRenderer.h documents: a shared set would overlap and fail pipeline creation.
		static const u32 DriverDescriptorSet = 4;

		//! Sets 0..3, so also the value of DriverDescriptorSet. A pipeline layout has no holes: callers
		//! pass an empty (zero-binding) layout for every user set a given shader leaves unused.
		static const u32 MaxUserDescriptorSets = 4;

		//! Hashes a Vulkan handle for the key fields below - a pointer on 64-bit builds, a uint64_t
		//! elsewhere, hence the two overloads.
#if defined(VK_USE_64_BIT_PTR_DEFINES) && VK_USE_64_BIT_PTR_DEFINES == 1
		inline size_t vulkanHandleHash(const void* handle)
		{
			return reinterpret_cast<size_t>(handle);
		}
#else
		inline size_t vulkanHandleHash(u64 handle)
		{
			return static_cast<size_t>(handle ^ (handle >> 32));
		}
#endif

		//! Everything that determines a VkPipeline. Two draws with an equal SVulkanPipelineKey can
		//! reuse the same pipeline.
		struct SVulkanPipelineKey
		{
			//! vulkanHandleHash() of the VkShaderModule, not a hash of the SPIR-V: a module is created
			//! once per shader and lives as long as the material owning it, so the handle identifies it.
			size_t VSHash = 0;
			size_t PSHash = 0;
			size_t GSHash = 0; //!< 0 if no geometry shader.
			//! 0 if no tessellation. HS/DS only ever exist together, so both or neither.
			size_t HSHash = 0;
			size_t DSHash = 0;
			//! Hash of the VkPipelineLayout the pipeline is created against. Part of the key because a
			//! pipeline embeds its layout and materials no longer share one; safe to compare by handle
			//! since getOrCreatePipelineLayout() deduplicates by set layouts + push constant range.
			size_t PipelineLayoutHash = 0;
			//! None: opaque. AlphaBlend: EMT_TRANSPARENT_ALPHA_CHANNEL/VERTEX_ALPHA. AddColor:
			//! EMT_TRANSPARENT_ADD_COLOR. Custom: factors decoded from SMaterial::MaterialTypeParam.
			enum class EBlendMode { None, AlphaBlend, AddColor, Custom };
			EBlendMode BlendMode = EBlendMode::None;
			//! Only meaningful when BlendMode == Custom. Colour and alpha carry separate factors: the
			//! "_COLOR" factors are not accepted on the alpha slot.
			VkBlendFactor CustomSrcColorFactor = VK_BLEND_FACTOR_ONE;
			VkBlendFactor CustomDstColorFactor = VK_BLEND_FACTOR_ZERO;
			VkBlendFactor CustomSrcAlphaFactor = VK_BLEND_FACTOR_ONE;
			VkBlendFactor CustomDstAlphaFactor = VK_BLEND_FACTOR_ZERO;
			VkBlendOp CustomBlendOp = VK_BLEND_OP_ADD; //!< Applied to the colour and the alpha slot alike.
			bool DepthTestEnable = true;
			bool DepthWriteEnable = true;
			//! Defaults to GREATER to match SMaterial's ECFN_GREATER and this fork's inverted depth
			//! convention (near plane at 1.0, far at 0.0).
			VkCompareOp DepthCompareOp = VK_COMPARE_OP_GREATER;
			VkCullModeFlags CullMode = VK_CULL_MODE_BACK_BIT;
			//! Irrlicht winds front faces clockwise, unlike Vulkan's counter-clockwise default.
			VkFrontFace FrontFace = VK_FRONT_FACE_CLOCKWISE;
			VkPolygonMode PolygonMode = VK_POLYGON_MODE_FILL;
			//! The full topology, not a class of them: unlike D3D12, Vulkan bakes list/strip/fan in.
			VkPrimitiveTopology Topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			//! Up to 8 colour attachments; single-target callers only need ColorFormats[0].
			u32 ColorAttachmentCount = 1;
			VkFormat ColorFormats[8] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
				VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
			//! VK_FORMAT_UNDEFINED for a pass with no depth attachment.
			VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT;
			//! Must match the sample count of every image the draw renders into.
			VkSampleCountFlagBits SampleCount = VK_SAMPLE_COUNT_1_BIT;
			//! hashVertexInputState() of the bindings/attributes handed to getOrCreate().
			size_t VertexLayoutHash = 0;
			//! Written to every active colour attachment - no independent per-attachment masks here.
			VkColorComponentFlags ColorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
				VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

			// Stencil shadow volumes. The defaults leave normal material pipelines unaffected.
			bool StencilTestEnable = false;
			u32 StencilReadMask = 0xFF;
			u32 StencilWriteMask = 0xFF;
			VkStencilOp StencilFailOp = VK_STENCIL_OP_KEEP;
			VkStencilOp StencilDepthFailOp = VK_STENCIL_OP_KEEP;
			VkStencilOp StencilPassOp = VK_STENCIL_OP_KEEP;
			VkCompareOp StencilCompareOp = VK_COMPARE_OP_ALWAYS;

			//! SMaterial::PolygonOffsetFactor/PolygonOffsetDirection. Both zero disables depth bias.
			f32 DepthBiasConstant = 0.0f;
			f32 DepthBiasSlope = 0.0f;

			//! SMaterial::AntiAliasing & EAAM_ALPHA_TO_COVERAGE: the fragment's alpha becomes its
			//! sample coverage mask. Meaningful on a multisampled target, harmless on a single-sample one.
			bool AlphaToCoverage = false;

			//! A stream-output geometry material: primitives are captured by transform feedback and
			//! never rasterized, the D3D11_SO_NO_RASTERIZED_STREAM of the D3D drivers.
			bool RasterizerDiscard = false;

			//! SMaterial::LogicOp: replaces blending on every attachment when enabled (logicOp feature).
			bool LogicOpEnable = false;
			VkLogicOp LogicOp = VK_LOGIC_OP_NO_OP;
			//! SMaterial::ConservativeRaster (VK_EXT_conservative_rasterization, overestimate).
			bool ConservativeRaster = false;
			//! SMaterial::SampleMask, VkPipelineMultisampleStateCreateInfo::pSampleMask.
			u32 SampleMask = 0xffffffffu;
			//! setViewPorts(): the count is pipeline state even with dynamic viewports.
			u32 ViewportCount = 1;

			//! Per-attachment blend overrides for an MRT draw, from the IRenderTarget entries of
			//! setRenderTarget(array). Bit i of TargetOverrideMask set means attachment i takes the
			//! fields below instead of the material blend; the driver never sets bit 0 (the first
			//! target follows the material, as on D3D11). Needs the independentBlend feature.
			u8 TargetOverrideMask = 0;
			bool TargetBlendEnable[8] = {};
			VkBlendFactor TargetSrcFactor[8] = { VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,
				VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE };
			VkBlendFactor TargetDstFactor[8] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO,
				VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO };
			VkColorComponentFlags TargetWriteMask[8] = {};

			bool operator==(const SVulkanPipelineKey& other) const
			{
				if (TargetOverrideMask != other.TargetOverrideMask)
					return false;
				for (u32 i = 0; i < 8; ++i)
				{
					if (!(TargetOverrideMask & (1u << i)))
						continue;
					if (TargetBlendEnable[i] != other.TargetBlendEnable[i] ||
						TargetSrcFactor[i] != other.TargetSrcFactor[i] ||
						TargetDstFactor[i] != other.TargetDstFactor[i] ||
						TargetWriteMask[i] != other.TargetWriteMask[i])
						return false;
				}
				return VSHash == other.VSHash && PSHash == other.PSHash &&
					AlphaToCoverage == other.AlphaToCoverage &&
					RasterizerDiscard == other.RasterizerDiscard &&
					LogicOpEnable == other.LogicOpEnable && LogicOp == other.LogicOp &&
					ConservativeRaster == other.ConservativeRaster &&
					SampleMask == other.SampleMask && ViewportCount == other.ViewportCount &&
					GSHash == other.GSHash &&
					HSHash == other.HSHash && DSHash == other.DSHash &&
					PipelineLayoutHash == other.PipelineLayoutHash &&
					BlendMode == other.BlendMode &&
					CustomSrcColorFactor == other.CustomSrcColorFactor &&
					CustomDstColorFactor == other.CustomDstColorFactor &&
					CustomSrcAlphaFactor == other.CustomSrcAlphaFactor &&
					CustomDstAlphaFactor == other.CustomDstAlphaFactor &&
					CustomBlendOp == other.CustomBlendOp &&
					DepthTestEnable == other.DepthTestEnable &&
					DepthWriteEnable == other.DepthWriteEnable &&
					DepthCompareOp == other.DepthCompareOp &&
					CullMode == other.CullMode && FrontFace == other.FrontFace &&
					PolygonMode == other.PolygonMode &&
					Topology == other.Topology &&
					ColorAttachmentCount == other.ColorAttachmentCount &&
					std::equal(std::begin(ColorFormats), std::end(ColorFormats), std::begin(other.ColorFormats)) &&
					DepthFormat == other.DepthFormat &&
					SampleCount == other.SampleCount &&
					VertexLayoutHash == other.VertexLayoutHash &&
					ColorWriteMask == other.ColorWriteMask &&
					StencilTestEnable == other.StencilTestEnable &&
					StencilReadMask == other.StencilReadMask &&
					StencilWriteMask == other.StencilWriteMask &&
					StencilFailOp == other.StencilFailOp &&
					StencilDepthFailOp == other.StencilDepthFailOp &&
					StencilPassOp == other.StencilPassOp &&
					StencilCompareOp == other.StencilCompareOp &&
					DepthBiasConstant == other.DepthBiasConstant &&
					DepthBiasSlope == other.DepthBiasSlope;
			}

			//! Combines all fields into the cache key. Colliding keys would incorrectly share a
			//! pipeline; there is no collision handling, see CVulkanPipelineCache::getOrCreate().
			size_t computeHash() const
			{
				size_t h = VSHash;
				auto combine = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
				combine(PSHash);
				combine(GSHash);
				combine(HSHash);
				combine(DSHash);
				combine(PipelineLayoutHash);
				combine(static_cast<size_t>(BlendMode));
				combine(static_cast<size_t>(CustomSrcColorFactor));
				combine(static_cast<size_t>(CustomDstColorFactor));
				combine(static_cast<size_t>(CustomSrcAlphaFactor));
				combine(static_cast<size_t>(CustomDstAlphaFactor));
				combine(static_cast<size_t>(CustomBlendOp));
				combine(static_cast<size_t>(DepthTestEnable));
				combine(static_cast<size_t>(DepthWriteEnable));
				combine(static_cast<size_t>(DepthCompareOp));
				combine(static_cast<size_t>(CullMode));
				combine(static_cast<size_t>(FrontFace));
				combine(static_cast<size_t>(PolygonMode));
				combine(static_cast<size_t>(Topology));
				combine(static_cast<size_t>(ColorAttachmentCount));
				for (u32 i = 0; i < 8; ++i)
					combine(static_cast<size_t>(ColorFormats[i]));
				combine(static_cast<size_t>(DepthFormat));
				combine(static_cast<size_t>(SampleCount));
				combine(VertexLayoutHash);
				combine(static_cast<size_t>(ColorWriteMask));
				combine(static_cast<size_t>(StencilTestEnable));
				combine(static_cast<size_t>(StencilReadMask));
				combine(static_cast<size_t>(StencilWriteMask));
				combine(static_cast<size_t>(StencilFailOp));
				combine(static_cast<size_t>(StencilDepthFailOp));
				combine(static_cast<size_t>(StencilPassOp));
				combine(static_cast<size_t>(StencilCompareOp));
				// static_cast<size_t> on a negative f32 is UB - go through a same-width signed int.
				combine(static_cast<size_t>(*reinterpret_cast<const s32*>(&DepthBiasConstant)));
				combine(static_cast<size_t>(*reinterpret_cast<const s32*>(&DepthBiasSlope)));
				combine(static_cast<size_t>(AlphaToCoverage));
				combine(static_cast<size_t>(RasterizerDiscard));
				combine(static_cast<size_t>(LogicOpEnable));
				combine(static_cast<size_t>(LogicOp));
				combine(static_cast<size_t>(ConservativeRaster));
				combine(static_cast<size_t>(SampleMask));
				combine(static_cast<size_t>(ViewportCount));
				combine(static_cast<size_t>(TargetOverrideMask));
				for (u32 i = 0; i < 8; ++i)
				{
					if (!(TargetOverrideMask & (1u << i)))
						continue;
					combine(static_cast<size_t>(TargetBlendEnable[i]));
					combine(static_cast<size_t>(TargetSrcFactor[i]));
					combine(static_cast<size_t>(TargetDstFactor[i]));
					combine(static_cast<size_t>(TargetWriteMask[i]));
				}
				return h;
			}
		};

		//! Hashes a vertex input state for SVulkanPipelineKey::VertexLayoutHash.
		size_t hashVertexInputState(const VkPipelineVertexInputStateCreateInfo& vertexInput);

		//! Hashes a binding list for the descriptor set layout cache: binding/type/count/stages of
		//! each entry. pImmutableSamplers is not hashed - no caller here uses immutable samplers.
		size_t hashDescriptorSetLayoutBindings(const VkDescriptorSetLayoutBinding* bindings, u32 bindingCount);

		//! Hashes the (set layouts, push constant range) pair for the pipeline layout cache.
		size_t hashPipelineLayoutKey(const VkDescriptorSetLayout* setLayouts, u32 setLayoutCount,
			const VkPushConstantRange* pushConstantRange);

		//! Descriptor set layouts and pipeline layouts, deduplicated so materials declaring the same
		//! bindings share one object - the counterpart of CD3D12Driver::getOrCreateRootSignature().
		//! Owns every handle it returns; callers only ever hold copies.
		class CVulkanPipelineLayoutCache
		{
		public:
			//! Returns the existing layout for this binding list, or creates one (VK_NULL_HANDLE on
			//! failure, already logged). An empty list is valid: it plugs a user set left unused.
			VkDescriptorSetLayout getOrCreateDescriptorSetLayout(const SVulkanContext& context,
				const VkDescriptorSetLayoutBinding* bindings, u32 bindingCount);

			//! Returns the existing pipeline layout for this combination, or creates one (VK_NULL_HANDLE
			//! on failure, already logged). `setLayouts` is indexed by set number, so reaching the
			//! driver's own uniforms means DriverDescriptorSet+1 entries; `pushConstantRange` is optional.
			VkPipelineLayout getOrCreatePipelineLayout(const SVulkanContext& context,
				const VkDescriptorSetLayout* setLayouts, u32 setLayoutCount,
				const VkPushConstantRange* pushConstantRange = nullptr);

			//! Destroys every cached layout; the pipelines built against them must already be gone.
			void clear();

			~CVulkanPipelineLayoutCache() { clear(); }

			size_t descriptorSetLayoutCount() const { return DescriptorSetLayouts.size(); }
			size_t pipelineLayoutCount() const { return PipelineLayouts.size(); }

		private:
			//! Captured on first creation so clear() takes no argument, like CD3D12PSOCache::clear().
			VkDevice Device = VK_NULL_HANDLE;
			std::unordered_map<size_t, VkDescriptorSetLayout> DescriptorSetLayouts;
			std::unordered_map<size_t, VkPipelineLayout> PipelineLayouts;
		};

		//! The entry point of each stage a pipeline is built from. HLSL authored for D3D names them
		//! per stage ("vsMain", "psMain", "gsMain"), so one name for all is not enough.
		struct SVulkanStageEntryPoints
		{
			const c8* Vertex = "main";
			const c8* TessControl = "main";
			const c8* TessEval = "main";
			const c8* Geometry = "main";
			const c8* Fragment = "main";
		};

		//! Pipeline cache. One per driver, not per frame - pipelines are immutable and expensive to
		//! create. Every pipeline it builds targets dynamic rendering (VK_KHR_dynamic_rendering, core
		//! in 1.3), so there is no VkRenderPass anywhere in the key.
		class CVulkanPipelineCache
		{
		public:
			//! Returns the existing pipeline for this key, or creates one (blocking - can take several
			//! milliseconds). VK_NULL_HANDLE on failure (already logged). Geometry and tessellation
			//! modules are optional; `patchControlPoints` is read only when the tessellation pair is set.
			//! A null `entryPoints` means "main" everywhere.
			VkPipeline getOrCreate(const SVulkanContext& context, const SVulkanPipelineKey& key,
				VkShaderModule vertexShader, VkShaderModule fragmentShader,
				const VkPipelineVertexInputStateCreateInfo& vertexInput,
				VkPipelineLayout pipelineLayout,
				VkShaderModule geometryShader = VK_NULL_HANDLE,
				VkShaderModule tessControlShader = VK_NULL_HANDLE,
				VkShaderModule tessEvalShader = VK_NULL_HANDLE,
				u32 patchControlPoints = 3,
				const SVulkanStageEntryPoints* entryPoints = nullptr);

			//! Destroys every cached pipeline and the driver-side VkPipelineCache; device must be idle.
			void clear();

			~CVulkanPipelineCache() { clear(); }

			size_t size() const { return Cache.size(); }

		private:
			//! Creates PipelineCache on first use. Failure is not fatal, hence the void return.
			void ensurePipelineCache(const SVulkanContext& context);

			VkDevice Device = VK_NULL_HANDLE;
			//! Lets the driver reuse compilation work across the pipelines below. Not serialized to
			//! disk - it only pays off within a single run.
			VkPipelineCache PipelineCache = VK_NULL_HANDLE;
			std::unordered_map<size_t, VkPipeline> Cache;
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
