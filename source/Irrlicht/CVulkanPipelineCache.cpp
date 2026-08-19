// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanPipelineCache.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_
#include "os.h"

namespace irr
{
	namespace video
	{
		namespace
		{
			// Same mixing constant and shift pattern as SVulkanPipelineKey::computeHash().
			inline void combineHash(size_t& h, size_t v)
			{
				h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
			}

			// A combined depth/stencil format has to be named as the stencil attachment too, see
			// VkPipelineRenderingCreateInfo::stencilAttachmentFormat below.
			inline bool formatHasStencil(VkFormat format)
			{
				return format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT ||
					format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
			}
		}

		size_t hashVertexInputState(const VkPipelineVertexInputStateCreateInfo& vertexInput)
		{
			size_t h = vertexInput.vertexBindingDescriptionCount;
			combineHash(h, vertexInput.vertexAttributeDescriptionCount);
			for (u32 i = 0; i < vertexInput.vertexBindingDescriptionCount; ++i)
			{
				const VkVertexInputBindingDescription& binding = vertexInput.pVertexBindingDescriptions[i];
				combineHash(h, static_cast<size_t>(binding.binding));
				combineHash(h, static_cast<size_t>(binding.stride));
				combineHash(h, static_cast<size_t>(binding.inputRate));
			}
			for (u32 i = 0; i < vertexInput.vertexAttributeDescriptionCount; ++i)
			{
				const VkVertexInputAttributeDescription& attribute = vertexInput.pVertexAttributeDescriptions[i];
				combineHash(h, static_cast<size_t>(attribute.location));
				combineHash(h, static_cast<size_t>(attribute.binding));
				combineHash(h, static_cast<size_t>(attribute.format));
				combineHash(h, static_cast<size_t>(attribute.offset));
			}
			return h;
		}

		size_t hashDescriptorSetLayoutBindings(const VkDescriptorSetLayoutBinding* bindings, u32 bindingCount)
		{
			size_t h = bindingCount;
			for (u32 i = 0; i < bindingCount; ++i)
			{
				combineHash(h, static_cast<size_t>(bindings[i].binding));
				combineHash(h, static_cast<size_t>(bindings[i].descriptorType));
				combineHash(h, static_cast<size_t>(bindings[i].descriptorCount));
				combineHash(h, static_cast<size_t>(bindings[i].stageFlags));
				// pImmutableSamplers intentionally not hashed, see CVulkanPipelineCache.h.
			}
			return h;
		}

		size_t hashPipelineLayoutKey(const VkDescriptorSetLayout* setLayouts, u32 setLayoutCount,
			const VkPushConstantRange* pushConstantRange)
		{
			// Hashing the set layout handles is enough because getOrCreateDescriptorSetLayout()
			// already deduplicates them: equal binding lists always yield the very same handle.
			size_t h = setLayoutCount;
			for (u32 i = 0; i < setLayoutCount; ++i)
				combineHash(h, vulkanHandleHash(setLayouts[i]));
			if (pushConstantRange)
			{
				combineHash(h, static_cast<size_t>(pushConstantRange->stageFlags));
				combineHash(h, static_cast<size_t>(pushConstantRange->offset));
				combineHash(h, static_cast<size_t>(pushConstantRange->size));
			}
			return h;
		}

		VkDescriptorSetLayout CVulkanPipelineLayoutCache::getOrCreateDescriptorSetLayout(
			const SVulkanContext& context, const VkDescriptorSetLayoutBinding* bindings, u32 bindingCount)
		{
			const size_t hash = hashDescriptorSetLayoutBindings(bindings, bindingCount);
			auto cached = DescriptorSetLayouts.find(hash);
			if (cached != DescriptorSetLayouts.end())
				return cached->second;

			if (!context.Device)
			{
				os::Printer::log("CVulkanPipelineLayoutCache::getOrCreateDescriptorSetLayout: no device",
					ELL_ERROR);
				return VK_NULL_HANDLE;
			}

			VkDescriptorSetLayoutCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			info.bindingCount = bindingCount;
			info.pBindings = bindingCount ? bindings : nullptr;

			VkDescriptorSetLayout layout = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanPipelineLayoutCache: vkCreateDescriptorSetLayout",
				vk::CreateDescriptorSetLayout(context.Device, &info, nullptr, &layout)))
				return VK_NULL_HANDLE;

			Device = context.Device;
			DescriptorSetLayouts[hash] = layout;
			return layout;
		}

		VkPipelineLayout CVulkanPipelineLayoutCache::getOrCreatePipelineLayout(const SVulkanContext& context,
			const VkDescriptorSetLayout* setLayouts, u32 setLayoutCount,
			const VkPushConstantRange* pushConstantRange)
		{
			const size_t hash = hashPipelineLayoutKey(setLayouts, setLayoutCount, pushConstantRange);
			auto cached = PipelineLayouts.find(hash);
			if (cached != PipelineLayouts.end())
				return cached->second;

			if (!context.Device)
			{
				os::Printer::log("CVulkanPipelineLayoutCache::getOrCreatePipelineLayout: no device", ELL_ERROR);
				return VK_NULL_HANDLE;
			}

			VkPipelineLayoutCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			info.setLayoutCount = setLayoutCount;
			info.pSetLayouts = setLayoutCount ? setLayouts : nullptr;
			info.pushConstantRangeCount = pushConstantRange ? 1 : 0;
			info.pPushConstantRanges = pushConstantRange;

			VkPipelineLayout layout = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanPipelineLayoutCache: vkCreatePipelineLayout",
				vk::CreatePipelineLayout(context.Device, &info, nullptr, &layout)))
				return VK_NULL_HANDLE;

			Device = context.Device;
			PipelineLayouts[hash] = layout;
			return layout;
		}

		void CVulkanPipelineLayoutCache::clear()
		{
			if (Device)
			{
				for (auto& entry : PipelineLayouts)
					vk::DestroyPipelineLayout(Device, entry.second, nullptr);
				for (auto& entry : DescriptorSetLayouts)
					vk::DestroyDescriptorSetLayout(Device, entry.second, nullptr);
			}
			PipelineLayouts.clear();
			DescriptorSetLayouts.clear();
			Device = VK_NULL_HANDLE;
		}

		void CVulkanPipelineCache::ensurePipelineCache(const SVulkanContext& context)
		{
			if (PipelineCache != VK_NULL_HANDLE || !context.Device)
				return;

			VkPipelineCacheCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
			if (vulkanFailed("CVulkanPipelineCache: vkCreatePipelineCache",
				vk::CreatePipelineCache(context.Device, &info, nullptr, &PipelineCache)))
				PipelineCache = VK_NULL_HANDLE;
			else
				Device = context.Device;
		}

		VkPipeline CVulkanPipelineCache::getOrCreate(const SVulkanContext& context,
			const SVulkanPipelineKey& key, VkShaderModule vertexShader, VkShaderModule fragmentShader,
			const VkPipelineVertexInputStateCreateInfo& vertexInput, VkPipelineLayout pipelineLayout,
			VkShaderModule geometryShader, VkShaderModule tessControlShader, VkShaderModule tessEvalShader,
			u32 patchControlPoints, const c8* entryPoint)
		{
			// Keyed on the hash alone, with no comparison of the keys themselves: two distinct keys
			// hashing to the same value would silently share a pipeline. Same known limitation as
			// CD3D12PSOCache::getOrCreate().
			const size_t hash = key.computeHash();
			auto it = Cache.find(hash);
			if (it != Cache.end())
				return it->second;

			if (!context.Device || !vertexShader || !fragmentShader || !pipelineLayout)
			{
				os::Printer::log("CVulkanPipelineCache::getOrCreate: null parameter, pipeline not created",
					ELL_ERROR);
				return VK_NULL_HANDLE;
			}
			if (!context.HasDynamicRendering)
			{
				os::Printer::log("CVulkanPipelineCache::getOrCreate: dynamic rendering unavailable,"
					" pipeline not created", ELL_ERROR);
				return VK_NULL_HANDLE;
			}

			ensurePipelineCache(context);

			// At most five stages, packed contiguously: an absent optional module simply adds nothing,
			// which is how Vulkan expresses what D3D12 expresses with an empty bytecode slot.
			VkPipelineShaderStageCreateInfo stages[5] = {};
			u32 stageCount = 0;
			auto addStage = [&](VkShaderStageFlagBits stage, VkShaderModule module)
			{
				if (module == VK_NULL_HANDLE)
					return;
				VkPipelineShaderStageCreateInfo& info = stages[stageCount++];
				info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				info.stage = stage;
				info.module = module;
				info.pName = entryPoint;
			};
			addStage(VK_SHADER_STAGE_VERTEX_BIT, vertexShader);
			addStage(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tessControlShader);
			addStage(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, tessEvalShader);
			addStage(VK_SHADER_STAGE_GEOMETRY_BIT, geometryShader);
			addStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader);

			VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
			inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
			inputAssembly.topology = key.Topology;
			// Strips are cut by the 0xFFFF/0xFFFFFFFF index the same way D3D12 cuts them, and a list
			// topology ignores the flag entirely, so it can stay on unconditionally.
			inputAssembly.primitiveRestartEnable = VK_TRUE;

			VkPipelineTessellationStateCreateInfo tessellation = {};
			tessellation.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
			tessellation.patchControlPoints = patchControlPoints;

			// Viewport and scissor are dynamic (vkCmdSetViewport/vkCmdSetScissor), so the pipeline only
			// declares their count - one viewport, one scissor, as D3D12's RSSetViewports() implies.
			VkPipelineViewportStateCreateInfo viewportState = {};
			viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			viewportState.viewportCount = 1;
			viewportState.scissorCount = 1;

			const VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dynamicState = {};
			dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
			dynamicState.dynamicStateCount = 2;
			dynamicState.pDynamicStates = dynamicStates;

			VkPipelineRasterizationStateCreateInfo rasterization = {};
			rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
			rasterization.depthClampEnable = VK_FALSE;
			rasterization.rasterizerDiscardEnable = VK_FALSE;
			rasterization.polygonMode = key.PolygonMode;
			rasterization.cullMode = key.CullMode;
			rasterization.frontFace = key.FrontFace;
			rasterization.depthBiasEnable = (key.DepthBiasConstant != 0.0f || key.DepthBiasSlope != 0.0f) ?
				VK_TRUE : VK_FALSE;
			rasterization.depthBiasConstantFactor = key.DepthBiasConstant;
			rasterization.depthBiasClamp = 0.0f;
			rasterization.depthBiasSlopeFactor = key.DepthBiasSlope;
			rasterization.lineWidth = 1.0f;

			VkPipelineMultisampleStateCreateInfo multisample = {};
			multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
			// Must match the sample count of every image the draw renders into, or the draw is invalid.
			multisample.rasterizationSamples = key.SampleCount;
			multisample.sampleShadingEnable = VK_FALSE;
			multisample.minSampleShading = 1.0f;
			multisample.alphaToCoverageEnable = VK_FALSE;
			multisample.alphaToOneEnable = VK_FALSE;

			// Depth/stencil. The stencil ops support shadow volumes; front and back faces get the same
			// ops, since a pipeline needing them already culls one face.
			VkPipelineDepthStencilStateCreateInfo depthStencil = {};
			depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			depthStencil.depthTestEnable = key.DepthTestEnable ? VK_TRUE : VK_FALSE;
			depthStencil.depthWriteEnable = key.DepthWriteEnable ? VK_TRUE : VK_FALSE;
			depthStencil.depthCompareOp = key.DepthCompareOp;
			depthStencil.depthBoundsTestEnable = VK_FALSE;
			depthStencil.stencilTestEnable = key.StencilTestEnable ? VK_TRUE : VK_FALSE;
			depthStencil.front.failOp = key.StencilFailOp;
			depthStencil.front.passOp = key.StencilPassOp;
			depthStencil.front.depthFailOp = key.StencilDepthFailOp;
			depthStencil.front.compareOp = key.StencilCompareOp;
			depthStencil.front.compareMask = key.StencilReadMask;
			depthStencil.front.writeMask = key.StencilWriteMask;
			// Baked into the pipeline rather than set per draw: VK_DYNAMIC_STATE_STENCIL_REFERENCE is
			// deliberately not among the dynamic states above.
			depthStencil.front.reference = 0;
			depthStencil.back = depthStencil.front;

			// Blend - one set of parameters replicated to every active attachment, mirroring D3D12's
			// IndependentBlendEnable=FALSE: an MRT draw inherits the same blend everywhere. Four modes
			// are covered (see SVulkanPipelineKey::EBlendMode); anything else falls back to None.
			VkPipelineColorBlendAttachmentState blendAttachment = {};
			blendAttachment.colorWriteMask = key.ColorWriteMask;
			switch (key.BlendMode)
			{
			case SVulkanPipelineKey::EBlendMode::AlphaBlend:
				blendAttachment.blendEnable = VK_TRUE;
				blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
				blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
				break;
			case SVulkanPipelineKey::EBlendMode::AddColor:
				// EMT_TRANSPARENT_ADD_COLOR: pure additive blend, not alpha-weighted.
				blendAttachment.blendEnable = VK_TRUE;
				blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
				blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
				break;
			case SVulkanPipelineKey::EBlendMode::Custom:
				// EMT_ONETEXTURE_BLEND: source * sourceFactor + dest * destFactor, factors decoded
				// from SMaterial::MaterialTypeParam by the caller.
				blendAttachment.blendEnable = VK_TRUE;
				blendAttachment.srcColorBlendFactor = key.CustomSrcColorFactor;
				blendAttachment.dstColorBlendFactor = key.CustomDstColorFactor;
				blendAttachment.colorBlendOp = key.CustomBlendOp;
				blendAttachment.srcAlphaBlendFactor = key.CustomSrcAlphaFactor;
				blendAttachment.dstAlphaBlendFactor = key.CustomDstAlphaFactor;
				blendAttachment.alphaBlendOp = key.CustomBlendOp;
				break;
			case SVulkanPipelineKey::EBlendMode::None:
			default:
				blendAttachment.blendEnable = VK_FALSE;
				blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
				blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
				blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
				blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
				break;
			}

			const u32 colorCount = key.ColorAttachmentCount < 8 ? key.ColorAttachmentCount : 8;
			VkPipelineColorBlendAttachmentState blendAttachments[8];
			for (u32 i = 0; i < colorCount; ++i)
				blendAttachments[i] = blendAttachment;

			VkPipelineColorBlendStateCreateInfo colorBlend = {};
			colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			colorBlend.logicOpEnable = VK_FALSE;
			colorBlend.logicOp = VK_LOGIC_OP_NO_OP;
			colorBlend.attachmentCount = colorCount;
			colorBlend.pAttachments = colorCount ? blendAttachments : nullptr;

			// Dynamic rendering replaces the VkRenderPass: the attachment formats the pipeline is
			// compiled against are chained here instead of read from a render pass object.
			VkFormat colorFormats[8];
			for (u32 i = 0; i < colorCount; ++i)
				colorFormats[i] = key.ColorFormats[i];

			VkPipelineRenderingCreateInfo renderingInfo = {};
			renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
			renderingInfo.colorAttachmentCount = colorCount;
			renderingInfo.pColorAttachmentFormats = colorCount ? colorFormats : nullptr;
			renderingInfo.depthAttachmentFormat = key.DepthFormat;
			renderingInfo.stencilAttachmentFormat = formatHasStencil(key.DepthFormat) ?
				key.DepthFormat : VK_FORMAT_UNDEFINED;

			VkGraphicsPipelineCreateInfo desc = {};
			desc.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			desc.pNext = &renderingInfo;
			desc.stageCount = stageCount;
			desc.pStages = stages;
			desc.pVertexInputState = &vertexInput;
			desc.pInputAssemblyState = &inputAssembly;
			desc.pTessellationState = (tessControlShader && tessEvalShader) ? &tessellation : nullptr;
			desc.pViewportState = &viewportState;
			desc.pRasterizationState = &rasterization;
			desc.pMultisampleState = &multisample;
			desc.pDepthStencilState = &depthStencil;
			desc.pColorBlendState = &colorBlend;
			desc.pDynamicState = &dynamicState;
			desc.layout = pipelineLayout;
			// No render pass and no subpass index: both are replaced by renderingInfo above.
			desc.renderPass = VK_NULL_HANDLE;

			VkPipeline pipeline = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanPipelineCache::getOrCreate: vkCreateGraphicsPipelines",
				vk::CreateGraphicsPipelines(context.Device, PipelineCache, 1, &desc, nullptr, &pipeline)))
				return VK_NULL_HANDLE;

			Device = context.Device;
			Cache[hash] = pipeline;
			return pipeline;
		}

		void CVulkanPipelineCache::clear()
		{
			// Pipelines first, then the object that cached their compilation - never the other way.
			if (Device)
			{
				for (auto& entry : Cache)
					vk::DestroyPipeline(Device, entry.second, nullptr);
				if (PipelineCache != VK_NULL_HANDLE)
					vk::DestroyPipelineCache(Device, PipelineCache, nullptr);
			}
			Cache.clear();
			PipelineCache = VK_NULL_HANDLE;
			Device = VK_NULL_HANDLE;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
