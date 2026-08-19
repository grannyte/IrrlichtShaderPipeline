// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Vulkan compute, self-contained: the counterpart of the D3D12 driver's compute root signature, its
// compute PSO cache and its dispatchComputeShader()/dispatchComputeShaderToTexture() pair. Two rules
// the driver must honour, both free on the D3D12 side and neither enforceable here:
//   1. A dispatch goes OUTSIDE a dynamic-rendering instance -- vkCmdDispatch is illegal between
//      vkCmdBeginRendering and vkCmdEndRendering, and inside a VkRenderPass on the fallback path.
//      Record it before opening the instance, after closing it, or on its own command buffer, which
//      is what the D3D12 path does with its UploadScope command list.
//   2. Vulkan orders nothing across a dispatch. D3D12 gets that from the resource state transitions
//      it already issues; here the driver records the barrier* helpers below itself, around every
//      dispatch. Only the transition into VK_IMAGE_LAYOUT_GENERAL is issued for it, by
//      dispatchToTexture(), which alone knows the image becomes a storage image.

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
	namespace video
	{
		class CVulkanHardwareBuffer;
		class CVulkanTexture;

		// The compute descriptor layout: one set, fixed bindings, shaped after the D3D12 compute root
		// signature. A compute shader has nothing to vary on, so one driver-wide layout serves every
		// compute material, as ComputeRootSignature does there.
		//   set 0, binding 0 : STORAGE_BUFFER read  -- source buffer,      the SRV t0 equivalent
		//   set 0, binding 1 : STORAGE_BUFFER write -- destination buffer, the UAV u0 equivalent
		//   set 0, binding 2 : UNIFORM_BUFFER       -- the user uniform block, the CBV b0 equivalent
		//   set 0, binding 3 : STORAGE_IMAGE write  -- destination texture, dispatchToTexture() only
		// Bindings 1 and 3 are two shapes of one output: a shader uses one or the other, and only a
		// descriptor the pipeline statically uses has to be written, so the unused slot costs nothing.
		static const u32 VulkanComputeDescriptorSetIndex = 0;
		static const u32 VulkanComputeSrcBufferBinding = 0;
		static const u32 VulkanComputeDstBufferBinding = 1;
		static const u32 VulkanComputeUniformBinding = 2;
		static const u32 VulkanComputeDstImageBinding = 3;

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

		//! A compute material: one VkShaderModule plus its reflected uniform blocks. An
		//! IMaterialRenderer so addMaterialRenderer() can hand back an E_MATERIAL_TYPE, the shape
		//! addComputeShader() has -- but it draws nothing, so the inherited hooks stay empty.
		class CVulkanComputeMaterial : public IMaterialRenderer
		{
		public:
			//! Owned: the driver must drop() its compute materials before the device goes.
			VkShaderModule Module = VK_NULL_HANDLE;

			//! Only the first block reaches the shader, see the note on binding 2 above.
			std::vector<SVulkanComputeUniformBlock> Blocks;
			std::vector<SVulkanComputeVariable> Variables;

			IShaderConstantSetCallBack* CallBack = nullptr; //!< grab()'d by the driver, drop()'d here.
			s32 UserData = 0;
			core::stringc Name;       //!< Diagnostic only, for log messages.
			core::stringc EntryPoint; //!< Baked into the module; the pipeline needs it back.

			CVulkanComputeMaterial() = default;
			virtual ~CVulkanComputeMaterial();

			//! Compiles for EST_COMPUTE_SHADER and reflects. `sourceLength` is in BYTES, 0 meaning
			//! "measure it" -- except for EGSL_PCMP, whose words contain zero bytes.
			bool compileFromSource(const SVulkanContext& context, E_GPU_SHADING_LANGUAGE lang,
				const c8* source, u32 sourceLength = 0, const c8* entryPoint = 0);

			VkShaderModule getModule() const { return Module; }

			const c8* getEntryPointName() const;

			//! The block bound to binding 2, or 0 -- that descriptor is then left unwritten.
			const SVulkanComputeUniformBlock* getBoundBlock() const;

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
			//! Appends the module's uniform blocks and members; false (logged) on a bad module.
			bool reflectSpirv(const std::vector<u32>& spirv);

			VkDevice Device = VK_NULL_HANDLE; //!< Captured so the destructor needs no context.
		};

		//! The compute half of the driver: the two layouts, a pipeline cache keyed on the shader
		//! module, a uniform staging buffer and a descriptor pool. One per driver, not thread safe.
		class CVulkanCompute
		{
		public:
			explicit CVulkanCompute(const SVulkanContext& context);

			~CVulkanCompute(); //!< Calls clear(); the device must already be idle.

			//! Builds the layouts and the pool, once at start-up. False (logged) only stops the
			//! dispatches below, as a failed compute root signature does on D3D12.
			bool init();

			void clear(); //!< Destroys the pipelines, the pool, the layouts and the buffer. Idempotent.

			//! The cached compute pipeline for this module, created on the first ask (blocking).
			//! Keyed on the module handle alone: a module is created once per material and a compute
			//! pipeline has nothing else to vary on. Same contract as the D3D12 compute PSO cache.
			VkPipeline getOrCreatePipeline(VkShaderModule module, const c8* entryPoint = "main");

			//! Records a buffer-to-buffer dispatch: pipeline, a descriptor set built from (src, dst,
			//! uniform block), then vkCmdDispatch. `cmd` must be recording and NOT inside a rendering
			//! instance; the barriers around it are the caller's.
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

			//! SHADER_WRITE -> SHADER_READ for a later dispatch or draw, and -> TRANSFER_READ for a
			//! read-back copy.
			static void barrierAfterDispatch(VkCommandBuffer cmd, VkBuffer dst);

			//! General image form, layout transition included.
			static void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
				u32 mipLevels, VkImageLayout oldLayout, VkImageLayout newLayout,
				VkAccessFlags srcAccess, VkAccessFlags dstAccess,
				VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

			//! Into VK_IMAGE_LAYOUT_GENERAL for storage-image writes, tracked layout updated. No-op
			//! when already there; public so a driver batching dispatches can hoist it.
			static void barrierImageToStorage(VkCommandBuffer cmd, CVulkanTexture* texture);

			//! The other half: GENERAL -> SHADER_READ_ONLY_OPTIMAL, so the next draw can sample it.
			static void barrierImageToShaderRead(VkCommandBuffer cmd, CVulkanTexture* texture);

			VkDescriptorSetLayout getDescriptorSetLayout() const { return SetLayout; }
			VkPipelineLayout getPipelineLayout() const { return Layout; }
			bool isReady() const { return Layout != VK_NULL_HANDLE; }
			size_t pipelineCount() const { return Pipelines.size(); }

		private:
			//! One set from Pool. A dry pool is reset and retried once, safe only because a dispatch
			//! is submitted and waited on before the next -- as the D3D12 dispatch path also is.
			VkDescriptorSet allocateDescriptorSet();

			//! Grows the host-visible uniform buffer past `size` bytes, keeping it mapped.
			bool ensureUniformCapacity(VkDeviceSize size);

			//! Copies the block scratch in. False means no block to bind, not an error.
			bool uploadUniformBlock(CVulkanComputeMaterial* material);

			//! Shared front half of both dispatches: checks, limits, pipeline, uniform, descriptor set.
			bool prepareDispatch(CVulkanComputeMaterial* material, CVulkanHardwareBuffer* src,
				const core::vector3d<u32>& groupCount, const c8* what,
				VkPipeline& outPipeline, VkDescriptorSet& outSet, bool& outHasUniform);

			const SVulkanContext& Context;

			VkDescriptorSetLayout SetLayout = VK_NULL_HANDLE;
			VkPipelineLayout Layout = VK_NULL_HANDLE;
			VkDescriptorPool Pool = VK_NULL_HANDLE;

			VkPipelineCache PipelineCache = VK_NULL_HANDLE; //!< Shares compilation work; not serialized.
			std::unordered_map<size_t, VkPipeline> Pipelines; //!< vulkanHandleHash(module) -> pipeline, owned.

			// Host-visible, permanently mapped, grown on demand: one uniform block per dispatch.
			VkBuffer UniformBuffer = VK_NULL_HANDLE;
			VkDeviceMemory UniformMemory = VK_NULL_HANDLE;
			void* UniformMapped = nullptr;
			VkDeviceSize UniformCapacity = 0;
			VkDeviceSize UniformRange = 0; //!< Bytes the current block actually occupies.
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
