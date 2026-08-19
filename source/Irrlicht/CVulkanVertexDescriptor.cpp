// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanVertexDescriptor.h"

#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CVulkanPipelineCache.h"
#include "os.h"

#include <algorithm>

namespace irr
{
	namespace video
	{
		namespace
		{
			// sizeof(S3DVertex): Pos 12b + Normal 12b + Color 4b + TCoords 8b. Spelled out rather
			// than taken from the header, whose S3DVertexScolorf does not compile on its own.
			const u32 kS3DVertexStride = 36;

			VkVertexInputAttributeDescription makeAttribute(u32 location, u32 binding, VkFormat format, u32 offset)
			{
				VkVertexInputAttributeDescription desc = {};
				desc.location = location;
				desc.binding = binding;
				desc.format = format;
				desc.offset = offset;
				return desc;
			}

			// Adds the binding for `bufferID` unless it is already there. Two attributes of the same
			// buffer always agree on stride and input rate, both being read off the descriptor.
			void addBinding(std::vector<VkVertexInputBindingDescription>& bindings, u32 bufferID,
				u32 stride, VkVertexInputRate inputRate)
			{
				for (size_t i = 0; i < bindings.size(); ++i)
				{
					if (bindings[i].binding == bufferID)
						return;
				}

				VkVertexInputBindingDescription binding = {};
				binding.binding = bufferID;
				binding.stride = stride;
				binding.inputRate = inputRate;
				bindings.push_back(binding);
			}
		}

		u32 getVulkanAttributeLocation(E_VERTEX_ATTRIBUTE_SEMANTIC semantic)
		{
			switch (semantic)
			{
			case EVAS_POSITION:
				return EVVL_POSITION;
			case EVAS_NORMAL:
				return EVVL_NORMAL;
			case EVAS_COLOR:
				return EVVL_COLOR;
			case EVAS_TEXCOORD0:
				return EVVL_TEXCOORD0;
			case EVAS_TEXCOORD1:
				return EVVL_TEXCOORD1;
			case EVAS_TANGENT:
				return EVVL_TANGENT;
			case EVAS_BINORMAL:
				return EVVL_BINORMAL;
			// Beyond the shipped shaders' inputs. Laid out in enum order from EVVL_FIRST_EXTRA so a
			// given semantic always lands on the same location; EVAS_TEXCOORD1 is skipped because it
			// already owns location 4 above.
			case EVAS_TEXCOORD2:
			case EVAS_TEXCOORD3:
			case EVAS_TEXCOORD4:
			case EVAS_TEXCOORD5:
			case EVAS_TEXCOORD6:
			case EVAS_TEXCOORD7:
			case EVAS_TEXCOORD8:
			case EVAS_TEXCOORD9:
			case EVAS_TEXCOORD10:
			case EVAS_TEXCOORD11:
			case EVAS_TEXCOORD12:
			case EVAS_TEXCOORD13:
			case EVAS_TEXCOORD14:
			case EVAS_TEXCOORD15:
				return EVVL_FIRST_EXTRA + (static_cast<u32>(semantic) - static_cast<u32>(EVAS_TEXCOORD2));
			case EVAS_BLEND_WEIGHTS:
				return EVVL_FIRST_EXTRA + 14;
			case EVAS_BLEND_INDICES:
				return EVVL_FIRST_EXTRA + 15;
			case EVAS_CUSTOM:
				return EVVL_FIRST_EXTRA + 16;
			default:
				return EVVL_INVALID;
			}
		}

		VkFormat getVulkanVertexAttributeFormat(E_VERTEX_ATTRIBUTE_TYPE type, u32 elementCount,
			E_VERTEX_ATTRIBUTE_SEMANTIC semantic)
		{
			switch (type)
			{
			case EVAT_BYTE:
				switch (elementCount)
				{
				case 1: return VK_FORMAT_R8_SNORM;
				case 2: return VK_FORMAT_R8G8_SNORM;
				case 4: return VK_FORMAT_R8G8B8A8_SNORM;
				default: return VK_FORMAT_UNDEFINED;
				}
			case EVAT_UBYTE:
				// A four-byte colour is B8G8R8A8_UNORM, not R8G8B8A8_UNORM: SColor packs 0xAARRGGBB
				// into a u32, so the bytes read B,G,R,A and only B8G8R8A8 delivers aColor.rgb as the
				// true colour. CD3D11VertexDescriptor::getFormat() and kS3DVertexInputLayout instead
				// declare R8G8B8A8_UNORM and leave the shader to undo the shift with a ".bgra"
				// swizzle. That convention is not carried over, because the GLSL in
				// source/Irrlicht/vulkan/shaders/ reads "aColor" straight: the correction belongs on
				// one side of the boundary only, and this backend puts it on the format.
				if (semantic == EVAS_COLOR && elementCount == 4)
					return VK_FORMAT_B8G8R8A8_UNORM;
				switch (elementCount)
				{
				case 1: return VK_FORMAT_R8_UNORM;
				case 2: return VK_FORMAT_R8G8_UNORM;
				case 4: return VK_FORMAT_R8G8B8A8_UNORM;
				default: return VK_FORMAT_UNDEFINED;
				}
			case EVAT_SHORT:
				switch (elementCount)
				{
				case 1: return VK_FORMAT_R16_SINT;
				case 2: return VK_FORMAT_R16G16_SINT;
				case 4: return VK_FORMAT_R16G16B16A16_SINT;
				default: return VK_FORMAT_UNDEFINED;
				}
			case EVAT_USHORT:
				switch (elementCount)
				{
				case 1: return VK_FORMAT_R16_UINT;
				case 2: return VK_FORMAT_R16G16_UINT;
				case 4: return VK_FORMAT_R16G16B16A16_UINT;
				default: return VK_FORMAT_UNDEFINED;
				}
			case EVAT_INT:
				switch (elementCount)
				{
				case 1: return VK_FORMAT_R32_SINT;
				case 2: return VK_FORMAT_R32G32_SINT;
				case 3: return VK_FORMAT_R32G32B32_SINT;
				case 4: return VK_FORMAT_R32G32B32A32_SINT;
				default: return VK_FORMAT_UNDEFINED;
				}
			case EVAT_UINT:
				switch (elementCount)
				{
				case 1: return VK_FORMAT_R32_UINT;
				case 2: return VK_FORMAT_R32G32_UINT;
				case 3: return VK_FORMAT_R32G32B32_UINT;
				case 4: return VK_FORMAT_R32G32B32A32_UINT;
				default: return VK_FORMAT_UNDEFINED;
				}
			// EVAT_DOUBLE folded into the 32-bit float formats as the D3D11 and D3D12 mappings do:
			// no engine vertex format declares one, and 64-bit vertex formats need an unrequested feature.
			case EVAT_DOUBLE:
			case EVAT_FLOAT:
				switch (elementCount)
				{
				case 1: return VK_FORMAT_R32_SFLOAT;
				case 2: return VK_FORMAT_R32G32_SFLOAT;
				case 3: return VK_FORMAT_R32G32B32_SFLOAT;
				case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
				default: return VK_FORMAT_UNDEFINED;
				}
			default:
				return VK_FORMAT_UNDEFINED;
			}
		}

		void SVulkanVertexInputState::finalize()
		{
			CreateInfo = {};
			CreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			CreateInfo.vertexBindingDescriptionCount = static_cast<u32>(Bindings.size());
			CreateInfo.pVertexBindingDescriptions = Bindings.empty() ? nullptr : Bindings.data();
			CreateInfo.vertexAttributeDescriptionCount = static_cast<u32>(Attributes.size());
			CreateInfo.pVertexAttributeDescriptions = Attributes.empty() ? nullptr : Attributes.data();
		}

		size_t SVulkanVertexInputState::hash() const
		{
			return hashVertexInputState(CreateInfo);
		}

		bool buildVulkanVertexInputState(IVertexDescriptor* descriptor, SVulkanVertexInputState& out)
		{
			out.Bindings.clear();
			out.Attributes.clear();

			if (!descriptor)
			{
				out.finalize();
				return false;
			}

			const u32 count = descriptor->getAttributeCount();
			for (u32 i = 0; i < count; ++i)
			{
				IVertexAttribute* attr = descriptor->getAttribute(i);
				if (!attr)
					continue;

				const u32 location = getVulkanAttributeLocation(attr->getSemantic());
				const VkFormat format = getVulkanVertexAttributeFormat(attr->getType(),
					attr->getElementCount(), attr->getSemantic());

				if (location == EVVL_INVALID || format == VK_FORMAT_UNDEFINED)
				{
					os::Printer::log("CVulkanVertexDescriptor: unsupported vertex attribute skipped",
						attr->getName().c_str(), ELL_WARNING);
					continue;
				}

				const u32 bufferID = attr->getBufferID();
				// No D3D12_APPEND_ALIGNED_ELEMENT here: the offset CVertexDescriptor already
				// accumulated per buffer is written out verbatim.
				out.Attributes.push_back(makeAttribute(location, bufferID, format, attr->getOffset()));

				const VkVertexInputRate inputRate =
					(descriptor->getInstanceDataStepRate(bufferID) == EIDSR_PER_VERTEX) ?
					VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE;
				addBinding(out.Bindings, bufferID, descriptor->getVertexSize(bufferID), inputRate);
			}

			// Sorted so the hash depends on the layout, not on registration order (Vulkan takes either).
			std::sort(out.Bindings.begin(), out.Bindings.end(),
				[](const VkVertexInputBindingDescription& a, const VkVertexInputBindingDescription& b)
				{ return a.binding < b.binding; });
			std::sort(out.Attributes.begin(), out.Attributes.end(),
				[](const VkVertexInputAttributeDescription& a, const VkVertexInputAttributeDescription& b)
				{ return (a.binding != b.binding) ? (a.binding < b.binding) : (a.location < b.location); });

			out.finalize();
			return !out.Attributes.empty();
		}

		void getS3DVertexInputState(SVulkanVertexInputState& out)
		{
			out.Bindings.clear();
			out.Attributes.clear();

			// Stride and offsets match kS3DVertexInputLayout; locations, standard.vert's inputs.
			VkVertexInputBindingDescription binding = {};
			binding.binding = 0;
			binding.stride = kS3DVertexStride;
			binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
			out.Bindings.push_back(binding);

			out.Attributes.push_back(makeAttribute(EVVL_POSITION, 0, VK_FORMAT_R32G32B32_SFLOAT, 0));
			out.Attributes.push_back(makeAttribute(EVVL_NORMAL, 0, VK_FORMAT_R32G32B32_SFLOAT, 12));
			// B8G8R8A8 rather than R8G8B8A8, see getVulkanVertexAttributeFormat().
			out.Attributes.push_back(makeAttribute(EVVL_COLOR, 0, VK_FORMAT_B8G8R8A8_UNORM, 24));
			out.Attributes.push_back(makeAttribute(EVVL_TEXCOORD0, 0, VK_FORMAT_R32G32_SFLOAT, 28));

			out.finalize();
		}

		void resolveVulkanVertexInputState(IVertexDescriptor* descriptor, SVulkanVertexInputState& out)
		{
			if (!buildVulkanVertexInputState(descriptor, out))
				getS3DVertexInputState(out);
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
