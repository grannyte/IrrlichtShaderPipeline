// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// IVertexDescriptor -> Vulkan vertex input state. Free functions plus one small owning struct, not
// an IVertexDescriptor subclass as on D3D11: describing a layout here needs no device object.

#ifndef __C_VULKAN_VERTEX_DESCRIPTOR_H_INCLUDED__
#define __C_VULKAN_VERTEX_DESCRIPTOR_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "IVertexDescriptor.h"
#include "CVulkanHelpers.h"
#include <vector>

namespace irr
{
	namespace video
	{
		//! Input locations declared by source/Irrlicht/vulkan/shaders/*.vert, and the one authority for
		//! them: Vulkan matches by location alone - no HLSL semantic - so disagreeing draws garbage.
		enum E_VULKAN_VERTEX_LOCATION
		{
			EVVL_POSITION = 0,		//!< "vec3 aPos", declared by all three .vert files.
			EVVL_NORMAL = 1,		//!< "vec3 aNormal".
			EVVL_COLOR = 2,			//!< "vec4 aColor".
			EVVL_TEXCOORD0 = 3,		//!< "vec2 aUV".
			//! Location 4 is "vec2 aUV2" in standard_2tcoords.vert and "vec3 aTangent" in
			//! tangents.vert. They never clash: no vertex format carries both, and a draw binds one.
			EVVL_TEXCOORD1 = 4,
			EVVL_TANGENT = 4,
			EVVL_BINORMAL = 5,		//!< "vec3 aBinormal", tangents.vert only.
			EVVL_FIRST_EXTRA = 6,		//!< First location no shipped shader declares.
			EVVL_INVALID = 0xffffffff	//!< No usable location; the attribute is dropped.
		};

		//! Location the semantic feeds. EVAS_TEXCOORD2..15, the blend semantics and EVAS_CUSTOM get
		//! sequential locations from EVVL_FIRST_EXTRA; only a custom shader reads those.
		u32 getVulkanAttributeLocation(E_VERTEX_ATTRIBUTE_SEMANTIC semantic);

		//! VkFormat for one engine attribute; `semantic` is read for EVAS_COLOR only, where byte order
		//! matters (see the .cpp). VK_FORMAT_UNDEFINED, as on D3D11, for combinations never produced.
		//! `d3dColorOrder` hands a four-byte colour over as R8G8B8A8_UNORM, the shifted order the
		//! D3D11/D3D12 drivers use and that D3D-authored user shaders undo with a ".bgra".
		VkFormat getVulkanVertexAttributeFormat(E_VERTEX_ATTRIBUTE_TYPE type, u32 elementCount,
			E_VERTEX_ATTRIBUTE_SEMANTIC semantic = EVAS_CUSTOM, bool d3dColorOrder = false);

		//! The two arrays plus the create info pointing at them; must outlive the pipeline creation
		//! that reads it, which dereferences those pointers.
		struct SVulkanVertexInputState
		{
			std::vector<VkVertexInputBindingDescription> Bindings;
			std::vector<VkVertexInputAttributeDescription> Attributes;
			VkPipelineVertexInputStateCreateInfo CreateInfo;	//!< Rebuilt by finalize(); never assign directly.

			SVulkanVertexInputState()
			{
				finalize();
			}

			//! CreateInfo points into this object's own vectors, so a copy must re-aim it.
			SVulkanVertexInputState(const SVulkanVertexInputState& other)
				: Bindings(other.Bindings), Attributes(other.Attributes)
			{
				finalize();
			}

			SVulkanVertexInputState& operator=(const SVulkanVertexInputState& other)
			{
				if (this != &other)
				{
					Bindings = other.Bindings;
					Attributes = other.Attributes;
					finalize();
				}
				return *this;
			}

			//! Points CreateInfo at the current vector contents; the builders below already call it.
			void finalize();

			const VkPipelineVertexInputStateCreateInfo& get() const { return CreateInfo; }

			//! SVulkanPipelineKey::VertexLayoutHash, from hashVertexInputState() unchanged.
			size_t hash() const;

			bool empty() const { return Attributes.empty(); }	//!< No attribute survived translation.
		};

		//! Fills `out` from `descriptor`, honouring each attribute's buffer index (which becomes the
		//! binding), byte offset and type/element count. One binding per buffer index used, strided with
		//! getVertexSize(), per-instance where the descriptor says so. False on a null or empty result.
		bool buildVulkanVertexInputState(IVertexDescriptor* descriptor, SVulkanVertexInputState& out,
			bool d3dColorOrder = false);

		//! EVT_STANDARD fallback, counterpart of kS3DVertexInputLayout on the D3D12 side: position/
		//! normal/colour/uv from one S3DVertex-strided binding, for callers with no mesh buffer on hand.
		void getS3DVertexInputState(SVulkanVertexInputState& out);

		//! "Which layout for this draw", in one place: the descriptor's state when it yields one, the
		//! fallback otherwise. Mirrors CD3D12Driver::resolveInputLayout(), for its reason - whoever
		//! hashes the layout and whoever builds the pipeline must resolve identically.
		//! `d3dColorOrder` is set for a user material: its HLSL was written against the D3D drivers'
		//! shifted vertex colour and compensates with ".bgra", so it must see the same bytes here.
		//! The built-in GLSL reads the true colour and keeps B8G8R8A8.
		void resolveVulkanVertexInputState(IVertexDescriptor* descriptor, SVulkanVertexInputState& out,
			bool d3dColorOrder = false);

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
