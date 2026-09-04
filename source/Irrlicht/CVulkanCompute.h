// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Vulkan compute, self-contained: the counterpart of the D3D12 driver's compute root signature, its
// compute PSO cache and its dispatchComputeShader()/dispatchComputeShaderToTexture() pair, plus the
// slot-based path CD3D11Driver::dispatchComputeShaderBound() offers. Two rules the driver must
// honour, both free on the D3D12 side and neither enforceable here:
//   1. A dispatch goes OUTSIDE a dynamic-rendering instance -- vkCmdDispatch is illegal between
//      vkCmdBeginRendering and vkCmdEndRendering. Record it before opening the instance, after
//      closing it, or on its own command buffer, which is what the D3D12 path does with its
//      UploadScope command list.
//   2. Vulkan orders nothing across a dispatch. D3D12 gets that from the resource state transitions
//      it already issues; here the driver records the barrier* helpers below itself, around every
//      dispatch. The image layout transitions a storage image or a sampled texture needs ARE issued
//      by the dispatch itself, which alone knows how each binding is used.
//
// BINDING CONVENTION. A compute shader's descriptors all live in set 0 and are found by reflection,
// so every material gets its own VkDescriptorSetLayout. The binding NUMBER is what ties a
// descriptor to an IVideoDriver slot, mirroring the D3D11 register files, and CVulkanShaderCompiler
// shifts DXC's HLSL registers accordingly (t# -> #, u# -> 16 + #, b# -> 32 + #, s# -> 48 + #):
//   binding  0..15 : SRV slot 0..15 -- read-only storage buffers and sampled textures
//   binding 16..31 : UAV slot 0..15 -- read/write storage buffers and storage images
//   binding 32..47 : uniform blocks (every block is uploaded from its CPU scratch, whatever the binding)
//   binding 48..63 : separate samplers; the sampler of the SRV texture at slot (binding - 48)
// The two-buffer dispatch()/dispatchToTexture() pair predates the slots and stays ORDINAL so GLSL
// written against "binding = 0 / 1 / 2" keeps working: the source buffer is the lowest-numbered
// storage buffer, the destination the next one (or the storage image), and the uniform block is
// whichever the shader declares.

#ifndef __C_VULKAN_COMPUTE_H_INCLUDED__
#define __C_VULKAN_COMPUTE_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include <vector>
#include <unordered_map>

#include "IVertexDescriptor.h" // IMaterialRenderer::OnRender() needs it; IMaterialRenderer.h does not pull it in
#include "IMaterialRenderer.h"
#include "IShaderConstantSetCallBack.h"
#include "IGPUProgrammingServices.h" // E_GPU_SHADING_LANGUAGE
#include "EShaderTypes.h"
#include "irrString.h"
#include "vector3d.h"
#include "CVulkanHelpers.h"

namespace irr
{
	namespace io
	{
		class IFileSystem;
	}

	namespace video
	{
		class CVulkanHardwareBuffer;
		class CVulkanTexture;

		//! The binding ranges of the convention above.
		static const u32 VulkanComputeDescriptorSetIndex = 0;
		static const u32 VulkanComputeSrvBindingBase = 0;
		static const u32 VulkanComputeUavBindingBase = 16;
		static const u32 VulkanComputeCbvBindingBase = 32;
		static const u32 VulkanComputeSamplerBindingBase = 48;
		//! One past the last binding number a compute shader may use here.
		static const u32 VulkanComputeMaxBindings = 64;

		//! Sets the pool holds before recycling. See allocateDescriptorSet().
		static const u32 VulkanComputeMaxDescriptorSets = 64;

		//! A reflected block member: the Name -> {block, offset, size} table the setters look up in.
		struct SVulkanComputeVariable
		{
			core::stringc Name;
			s32 Block = 0; //!< Index into CVulkanComputeMaterial::Blocks, not the SPIR-V binding.
			u32 Offset = 0;
			u32 Size = 0;
			//! Only a 32-bit float 4x4 decorated RowMajor, what DXC emits for HLSL's default
			//! column_major and core::matrix4 wants transposed. A GLSL mat4 must NOT be.
			bool TransposeOnSet = false;
		};

		//! A reflected uniform block. Scratch is the CPU mirror the setters write into.
		struct SVulkanComputeUniformBlock
		{
			core::stringc Name;
			u32 Set = 0;
			u32 Binding = 0;
			u32 Size = 0; //!< Bytes, == Scratch.size(); the std140 size the reflector computed.
			std::vector<u8> Scratch;
		};

		//! One descriptor the shader declares, the unit the per-material layout is built from.
		struct SVulkanComputeBinding
		{
			u32 Binding = 0;
			VkDescriptorType Type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			core::stringc Name; //!< Diagnostic only: a binding number says nothing in a log.
			//! Set on the hidden counter DXC emits for an HLSL Append/ConsumeStructuredBuffer (named
			//! "counter.var.<buffer>", one uint, its own binding number): the name of the buffer it
			//! counts for. The dispatch binds that buffer's counter (CVulkanHardwareBuffer::
			//! getCounterBuffer()) here, which is what makes copyStructureCount() work on Vulkan.
			core::stringc CounterOf;
		};

		//! What a dispatch binds, indexed by BINDING NUMBER (not by slot): the driver translates its
		//! SRV/UAV slots into this table with the bases above. A storage buffer the shader declares
		//! and this table leaves empty gets a null buffer, which reads as zeros and swallows writes --
		//! the D3D11 semantics of an unbound slot, which callers rely on ("unbound reads 0"). A
		//! missing texture still fails the dispatch.
		struct SVulkanComputeResources
		{
			struct SEntry
			{
				CVulkanHardwareBuffer* Buffer = nullptr;
				CVulkanTexture* Texture = nullptr;
			};
			SEntry ByBinding[VulkanComputeMaxBindings];

			void setBuffer(u32 binding, CVulkanHardwareBuffer* buffer)
			{
				if (binding < VulkanComputeMaxBindings)
				{
					ByBinding[binding].Buffer = buffer;
					ByBinding[binding].Texture = nullptr;
				}
			}

			void setTexture(u32 binding, CVulkanTexture* texture)
			{
				if (binding < VulkanComputeMaxBindings)
				{
					ByBinding[binding].Buffer = nullptr;
					ByBinding[binding].Texture = texture;
				}
			}
		};

		//! A compute material: one VkShaderModule, its reflected uniform blocks and descriptor
		//! bindings, and the layout/pipeline built from them. An IMaterialRenderer so
		//! addMaterialRenderer() can hand back an E_MATERIAL_TYPE, the shape addComputeShader()
		//! has -- but it draws nothing, so the inherited hooks stay empty.
		class CVulkanComputeMaterial : public IMaterialRenderer
		{
		public:
			//! Owned: the driver must drop() its compute materials before the device goes.
			VkShaderModule Module = VK_NULL_HANDLE;

			//! Every uniform block the shader declares, each with its own scratch mirror.
			std::vector<SVulkanComputeUniformBlock> Blocks;
			std::vector<SVulkanComputeVariable> Variables;
			//! Every descriptor, uniform blocks included, sorted by binding number.
			std::vector<SVulkanComputeBinding> Bindings;

			//! Built lazily by CVulkanCompute::ensurePipeline() from Bindings; owned here so they die
			//! with the module they were built for.
			VkDescriptorSetLayout SetLayout = VK_NULL_HANDLE;
			VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
			VkPipeline Pipeline = VK_NULL_HANDLE;

			IShaderConstantSetCallBack* CallBack = nullptr; //!< grab()'d by the driver, drop()'d here.
			s32 UserData = 0;
			core::stringc Name;       //!< Diagnostic only, for log messages.
			core::stringc EntryPoint; //!< Baked into the module; the pipeline needs it back.

			CVulkanComputeMaterial() = default;
			virtual ~CVulkanComputeMaterial();

			//! Compiles for EST_COMPUTE_SHADER and reflects. `sourceLength` is in BYTES, 0 meaning
			//! "measure it" -- except for EGSL_PCMP, whose words contain zero bytes. The last two
			//! arguments resolve `#include` (see CVulkanShaderCompiler::compileToSpirv()).
			bool compileFromSource(const SVulkanContext& context, E_GPU_SHADING_LANGUAGE lang,
				const c8* source, u32 sourceLength = 0, const c8* entryPoint = 0,
				io::IFileSystem* includeFileSystem = 0, const c8* includeDirectory = 0);

			VkShaderModule getModule() const { return Module; }

			const c8* getEntryPointName() const;

			//! The declared descriptor at this binding number, or 0.
			const SVulkanComputeBinding* findBinding(u32 binding) const;

			//! The declared descriptor with this (source) name, or 0.
			const SVulkanComputeBinding* findBindingByName(const c8* name) const;

			//! The reflected block bound at `binding`, or 0.
			const SVulkanComputeUniformBlock* findBlock(u32 binding) const;

			s32 getConstantBufferID(const c8* name) const; //!< Index into Blocks, or -1.

			bool setConstantBuffer(s32 id, const void* data, size_t dataSizeBytes);

			s32 getVariableID(const c8* name) const; //!< Index into Variables, or -1.

			//! Writes at the variable's offset, transposing when TransposeOnSet says so, truncating
			//! rather than overflowing.
			bool setVariable(s32 id, const f32* floats, int count);
			bool setVariable(s32 id, const s32* ints, int count);
			bool setVariable(const c8* name, const f32* floats, int count);
			bool setVariable(const c8* name, const s32* ints, int count);

			//! Width-agnostic sibling of the above, which assume 4-byte elements. Never transposes.
			bool setVariableRaw(s32 id, const void* data, u32 byteCount);

		private:
			//! Appends the module's descriptors, uniform blocks and members; false (logged) on a bad
			//! module, a descriptor outside set 0, or a binding number past VulkanComputeMaxBindings.
			bool reflectSpirv(const std::vector<u32>& spirv);

			VkDevice Device = VK_NULL_HANDLE; //!< Captured so the destructor needs no context.
		};

		//! The compute half of the driver: a descriptor pool, a uniform staging buffer and the
		//! per-material layouts/pipelines it creates on demand. One per driver, not thread safe.
		class CVulkanCompute
		{
		public:
			explicit CVulkanCompute(const SVulkanContext& context);

			~CVulkanCompute(); //!< Calls clear(); the device must already be idle.

			//! Builds the pool and the pipeline cache, once at start-up. False (logged) only stops
			//! the dispatches below, as a failed compute root signature does on D3D12.
			bool init();

			void clear(); //!< Destroys the pool and the buffer. Idempotent.

			//! The material's set layout, pipeline layout and pipeline, created on the first ask
			//! (blocking). False (logged) on a binding table the layout cannot express -- two
			//! resources on one binding number, a texel buffer, a descriptor array.
			bool ensurePipeline(CVulkanComputeMaterial* material);

			//! Records one dispatch with every declared binding filled from `resources`: pipeline,
			//! descriptor set, the image layout transitions the bound textures need, then
			//! vkCmdDispatch -- or vkCmdDispatchIndirect when `indirectBuffer` is given, the group
			//! count being read from it at `indirectOffset`. `cmd` must be recording and NOT inside a
			//! rendering instance; the buffer barriers around it are the caller's.
			bool dispatchBound(VkCommandBuffer cmd, CVulkanComputeMaterial* material,
				const SVulkanComputeResources& resources, const core::vector3d<u32>& groupCount,
				VkBuffer indirectBuffer = VK_NULL_HANDLE, VkDeviceSize indirectOffset = 0);

			//! The two-buffer form, ordinal as the file header explains: `src` on the lowest storage
			//! buffer binding, `dst` on the next.
			bool dispatch(VkCommandBuffer cmd, CVulkanComputeMaterial* material,
				CVulkanHardwareBuffer* src, CVulkanHardwareBuffer* dst,
				const core::vector3d<u32>& groupCount);

			//! Same with the output in a texture bound as a storage image, for a result sampled by
			//! later draws rather than read back. Leaves the image in VK_IMAGE_LAYOUT_GENERAL; the
			//! caller moves it back with barrierImageToShaderRead().
			bool dispatchToTexture(VkCommandBuffer cmd, CVulkanComputeMaterial* material,
				CVulkanHardwareBuffer* src, CVulkanTexture* dst,
				const core::vector3d<u32>& groupCount);

			// --- Barriers, the driver's to call. They record into `cmd` and cannot fail. ---

			//! General form, for a caller with its own access/stage pair.
			static void bufferBarrier(VkCommandBuffer cmd, VkBuffer buffer,
				VkDeviceSize offset, VkDeviceSize size,
				VkAccessFlags srcAccess, VkAccessFlags dstAccess,
				VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

			//! Makes an upload or an earlier dispatch visible to the one about to be recorded.
			//! Either handle may be VK_NULL_HANDLE.
			static void barrierBeforeDispatch(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst);

			//! SHADER_WRITE -> every later consumer: a dispatch or draw reading it, an indirect
			//! command reading it, and a read-back copy.
			static void barrierAfterDispatch(VkCommandBuffer cmd, VkBuffer dst);

			//! General image form, layout transition included, every mip and layer.
			static void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
				u32 mipLevels, u32 layerCount, VkImageLayout oldLayout, VkImageLayout newLayout,
				VkAccessFlags srcAccess, VkAccessFlags dstAccess,
				VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

			//! Into VK_IMAGE_LAYOUT_GENERAL for storage-image writes, tracked layout updated. No-op
			//! when already there; public so a driver batching dispatches can hoist it.
			static void barrierImageToStorage(VkCommandBuffer cmd, CVulkanTexture* texture);

			//! The other half: back to SHADER_READ_ONLY_OPTIMAL, so the next draw can sample it.
			static void barrierImageToShaderRead(VkCommandBuffer cmd, CVulkanTexture* texture);

			bool isReady() const { return Pool != VK_NULL_HANDLE; }

		private:
			//! One set of `layout` from Pool. A dry pool is reset and retried once, safe only because
			//! a dispatch is submitted and waited on before the next -- as the D3D12 dispatch path
			//! also is.
			VkDescriptorSet allocateDescriptorSet(VkDescriptorSetLayout layout);

			//! Grows the host-visible uniform buffer past `size` bytes, keeping it mapped.
			bool ensureUniformCapacity(VkDeviceSize size);

			//! Copies every block's scratch into the uniform buffer, one aligned range per block, and
			//! fills `outOffsets` (Blocks order). False on an allocation failure only; a material
			//! without blocks succeeds with an empty table.
			bool uploadUniformBlocks(CVulkanComputeMaterial* material, std::vector<VkDeviceSize>& outOffsets);

			//! The lowest-numbered storage buffer bindings and the first storage image, for the
			//! ordinal dispatches.
			void findOrdinalBindings(const CVulkanComputeMaterial* material, u32& outFirstBuffer,
				u32& outSecondBuffer, u32& outImage) const;

			//! The two stand-ins for a declared-but-unbound storage buffer: a zero-filled one for the
			//! SRV range (reads give 0, as a null D3D11 SRV does) and a scratch one for the UAV range
			//! and for the counter of an unbound append buffer (writes land nowhere that matters).
			bool createNullBuffers();

			const SVulkanContext& Context;

			VkDescriptorPool Pool = VK_NULL_HANDLE;
			VkPipelineCache PipelineCache = VK_NULL_HANDLE; //!< Shares compilation work; not serialized.

			// Host-visible, permanently mapped, grown on demand: one dispatch's blocks at a time.
			VkBuffer UniformBuffer = VK_NULL_HANDLE;
			VkDeviceMemory UniformMemory = VK_NULL_HANDLE;
			void* UniformMapped = nullptr;
			VkDeviceSize UniformCapacity = 0;

			static const VkDeviceSize NullBufferSize = 1024 * 1024;
			VkBuffer NullReadBuffer = VK_NULL_HANDLE;
			VkDeviceMemory NullReadMemory = VK_NULL_HANDLE;
			VkBuffer NullWriteBuffer = VK_NULL_HANDLE;
			VkDeviceMemory NullWriteMemory = VK_NULL_HANDLE;
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
