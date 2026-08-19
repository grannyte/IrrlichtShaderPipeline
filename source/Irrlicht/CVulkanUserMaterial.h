// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// A user shader material -- what addHighLevelShaderMaterial() and friends create, the counterpart of
// the user-shader half of CD3D12MaterialRenderer. The built-in E_MATERIAL_TYPEs stay in
// CVulkanMaterialRenderer: they need neither a compiler nor reflection.
//
// A material owns one VkShaderModule per stage, compiled from GLSL/HLSL/pre-compiled SPIR-V through
// CVulkanShaderCompiler, plus what a hand-written SPIR-V reflector found in each module: the uniform
// blocks (name, set, binding, size) with a CPU scratch mirror each, the variables inside them, and
// the merged descriptor binding list a VkDescriptorSetLayout is built from. Only sets
// 0..MaxUserDescriptorSets-1 are the user's; a shader reaching into DriverDescriptorSet is rejected.
//
// OnSetMaterial()/OnRender() keep their empty inherited body -- Vulkan wants all state up front at
// pipeline-creation time, so the driver reads the fields below directly. isTransparent() IS wired up:
// scene code sorts solid vs. transparent nodes on it.

#ifndef __C_VULKAN_USER_MATERIAL_H_INCLUDED__
#define __C_VULKAN_USER_MATERIAL_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include <vector>

#include "IVertexDescriptor.h" // IMaterialRenderer::OnRender() needs this, not included by IMaterialRenderer.h itself
#include "IMaterialRenderer.h"
#include "IShaderConstantSetCallBack.h"
#include "IGPUProgrammingServices.h" // E_GPU_SHADING_LANGUAGE
#include "EShaderTypes.h"
#include "irrString.h"
#include "CVulkanHelpers.h"
#include "CVulkanPipelineCache.h" // SVulkanPipelineKey::EBlendMode, MaxUserDescriptorSets

namespace irr
{
	namespace video
	{
		//! The five programmable graphics stages a user material can carry, used as an index into the
		//! per-stage arrays below. Compute is not served here: it has no draw and its own driver path.
		enum E_VULKAN_USER_STAGE
		{
			EVUS_VERTEX = 0,
			EVUS_FRAGMENT,
			EVUS_GEOMETRY,
			EVUS_HULL,   //!< Tessellation control in Vulkan terms.
			EVUS_DOMAIN, //!< Tessellation evaluation.
			EVUS_COUNT
		};

		//! One stage handed to compileFromSource(). Source is HLSL/GLSL text or, for EGSL_PCMP, the
		//! SPIR-V words themselves -- which contain zero bytes, so SourceLength is mandatory there.
		struct SVulkanUserShaderStageSource
		{
			const c8* Source = nullptr;
			u32 SourceLength = 0;   //!< In BYTES; 0 means "null-terminated, measure it".
			const c8* EntryPoint = nullptr; //!< 0 or "" is read as "main", as the compiler does.
		};

		//! A reflected member of a uniform block: the same Name -> {block, offset, size} table
		//! SD3D12UserShaderVariable holds on the D3D12 side.
		struct SVulkanUserShaderVariable
		{
			core::stringc Name;
			s32 Block = 0; //!< Index into the stage's block vector, not the SPIR-V binding.
			u32 Offset = 0;
			u32 Size = 0;
			//! True only for a 4x4 float matrix decorated RowMajor in SPIR-V, which is what DXC emits
			//! for HLSL's default column_major (SPIR-V majorness mirrors HLSL's), so it wants exactly
			//! the transpose SD3D12UserShaderVariable::TransposeOnSet applies. GLSL's default
			//! (ColMajor, or undecorated) does NOT: core::matrix4's 16 floats already land in a std140
			//! column-major mat4 as the intended transform, row-major-with-row-vectors and
			//! column-major-with-column-vectors sharing one memory layout, and transposing them would
			//! deform every piece of geometry the shader touches.
			bool TransposeOnSet = false;
		};

		//! A reflected uniform block. Set/Binding place it in the descriptor set layout; Scratch is
		//! the CPU mirror the constant setters write into and the driver uploads once per draw.
		struct SVulkanUserUniformBlock
		{
			core::stringc Name;
			u32 Set = 0;
			u32 Binding = 0;
			u32 Size = 0; //!< Bytes, == Scratch.size(); the std140 size the reflector computed.
			std::vector<u8> Scratch;
		};

		//! One descriptor the shaders declare. Stages declaring the same (Set, Binding) share an entry
		//! and OR their stage bit into StageFlags, which is what a VkDescriptorSetLayoutBinding needs.
		struct SVulkanUserDescriptorBinding
		{
			u32 Set = 0;
			u32 Binding = 0;
			VkDescriptorType Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			u32 Count = 1; //!< Array length of the declaration; 1 when it is not an array.
			VkShaderStageFlags StageFlags = 0;
			core::stringc Name; //!< Diagnostic only: a binding number says nothing in a log.
		};

		class CVulkanUserMaterial : public IMaterialRenderer
		{
		public:
			//! Compiled modules, indexed by E_VULKAN_USER_STAGE. Owned: destroyed by the destructor.
			VkShaderModule Modules[EVUS_COUNT] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
				VK_NULL_HANDLE, VK_NULL_HANDLE };

			//! Reflection output, per stage. Blocks[s][i].Scratch is what setConstantBuffer() and
			//! setVariable() write into and what the driver copies into its uniform ring per draw.
			std::vector<SVulkanUserUniformBlock> Blocks[EVUS_COUNT];
			std::vector<SVulkanUserShaderVariable> Variables[EVUS_COUNT];

			//! Every descriptor the material declares, merged across stages -- feed it to
			//! getDescriptorSetLayoutBindings() to build one VkDescriptorSetLayout per used set.
			std::vector<SVulkanUserDescriptorBinding> DescriptorBindings;

			//! Bit s set means set s carries at least one binding. A pipeline layout has no holes, so
			//! the driver still plugs an empty layout into every unused set below the highest used one.
			u32 UsedSetMask = 0;

			IShaderConstantSetCallBack* CallBack = nullptr; //!< grab()'d by the driver, drop()'d here.
			s32 UserData = 0;
			core::stringc Name; //!< Diagnostic: the material name, for log messages.

			//! Entry point actually baked into each module, "main" unless the caller asked otherwise;
			//! the pipeline needs the name back at VkPipelineShaderStageCreateInfo time.
			core::stringc EntryPoints[EVUS_COUNT];

			//! Blend state, copied at registration from the base material -- a user shader has none of
			//! its own, exactly as CD3D12MaterialRenderer inherits it from its BaseMaterialType.
			SVulkanPipelineKey::EBlendMode BlendMode = SVulkanPipelineKey::EBlendMode::None;
			VkBlendFactor CustomSrcColorFactor = VK_BLEND_FACTOR_ONE;
			VkBlendFactor CustomDstColorFactor = VK_BLEND_FACTOR_ZERO;
			VkBlendFactor CustomSrcAlphaFactor = VK_BLEND_FACTOR_ONE;
			VkBlendFactor CustomDstAlphaFactor = VK_BLEND_FACTOR_ZERO;
			VkBlendOp CustomBlendOp = VK_BLEND_OP_ADD;

			//! Base E_MATERIAL_TYPE the blend fields above were copied from. Also read so a shader
			//! based on EMT_ONETEXTURE_BLEND still decodes its per-instance factors.
			E_MATERIAL_TYPE BaseMaterialType = EMT_SOLID;

			CVulkanUserMaterial() = default;
			virtual ~CVulkanUserMaterial();

			virtual bool isTransparent() const _IRR_OVERRIDE_
			{
				return BlendMode != SVulkanPipelineKey::EBlendMode::None;
			}

			//! Compiles and reflects the whole material. `stages` is an array of EVUS_COUNT entries;
			//! vertex and fragment are mandatory, geometry optional, hull/domain a pair. Returns false
			//! -- logged, compiler diagnostics included -- so the caller drops the object unregistered.
			bool compileFromSource(const SVulkanContext& context, E_GPU_SHADING_LANGUAGE lang,
				const SVulkanUserShaderStageSource* stages);

			VkShaderModule getModule(E_VULKAN_USER_STAGE stage) const
			{
				return ((u32)stage < (u32)EVUS_COUNT) ? Modules[stage] : VK_NULL_HANDLE;
			}

			//! Never 0: an empty stored name means the module was built with "main".
			const c8* getEntryPoint(E_VULKAN_USER_STAGE stage) const;

			//! Fills `outBindings` (cleared first) with the bindings of one set, ready for
			//! getOrCreateDescriptorSetLayout(). Empty is legitimate: that is an unused set's layout.
			void getDescriptorSetLayoutBindings(u32 set, std::vector<VkDescriptorSetLayoutBinding>& outBindings) const;

			//! The reflected blocks of one stage, so callers can walk all five in a loop. 0 on a bad
			//! stage, mirroring CD3D12MaterialRenderer::getStageBuffers().
			std::vector<SVulkanUserUniformBlock>* getStageBlocks(E_VULKAN_USER_STAGE stage);
			const std::vector<SVulkanUserUniformBlock>* getStageBlocks(E_VULKAN_USER_STAGE stage) const;

			//! E_SHADER_TYPE (the public shader-constant API) -> E_VULKAN_USER_STAGE. False for a type
			//! this material cannot carry, EST_COMPUTE_SHADER and EST_STREAM_OUTPUT_SHADER included.
			static bool stageFromShaderType(E_SHADER_TYPE type, E_VULKAN_USER_STAGE& outStage);

			//! Mirrors CD3D12MaterialRenderer::getConstantBufferID(): index into the stage's blocks,
			//! or -1 when the name is unknown.
			s32 getConstantBufferID(const c8* name, E_SHADER_TYPE stage) const;

			//! Overwrites the whole block `id` of `stage`, truncated to its size -- the "raw struct"
			//! shortcut alongside writing variable by variable.
			bool setConstantBuffer(s32 id, const void* data, size_t dataSizeBytes, E_SHADER_TYPE stage);

			//! Index into the stage's variables, the id the driver's getVertexShaderConstantID() and
			//! its siblings hand back. -1 when unknown.
			s32 getVariableID(const c8* name, E_SHADER_TYPE stage) const;

			//! Writes `count` floats at the variable's reflected offset, transposing a 4x4 matrix when
			//! TransposeOnSet says so, and truncating rather than overflowing a smaller variable.
			bool setVariable(s32 id, const f32* floats, int count, E_SHADER_TYPE stage);
			bool setVariable(s32 id, const s32* ints, int count, E_SHADER_TYPE stage);

			//! Name lookup plus write in one call, for callers holding no id.
			bool setVariable(const c8* name, const f32* floats, int count, E_SHADER_TYPE stage);
			bool setVariable(const c8* name, const s32* ints, int count, E_SHADER_TYPE stage);

			//! Width-agnostic sibling of the two above: the typed ones assume 4-byte elements, so a
			//! 64-bit type set through them would copy only half its bytes. Never transposes.
			bool setVariableRaw(s32 id, const void* data, u32 byteCount, E_SHADER_TYPE stage);

		private:
			//! Walks a SPIR-V module and appends what it declares to the stage's blocks/variables and
			//! to the shared binding list. False (logged) on a malformed module or a descriptor set
			//! outside the user range; unsupported declarations are warned about and skipped.
			bool reflectSpirv(const std::vector<u32>& spirv, E_VULKAN_USER_STAGE stage,
				VkShaderStageFlagBits stageBit, const c8* stageName);

			//! Compiles one stage and turns the words into a module. False (logged) on either failure.
			bool compileStage(const SVulkanContext& context, E_GPU_SHADING_LANGUAGE lang,
				const SVulkanUserShaderStageSource& source, E_VULKAN_USER_STAGE stage);

			//! Copies a just-written range into every OTHER stage declaring the same (set, binding).
			//! One Vulkan descriptor is shared by all stages, unlike a D3D12 per-stage root table, so
			//! without this the two scratches of a shared block would drift apart and one go stale.
			void mirrorBlockWrite(E_VULKAN_USER_STAGE stage, size_t blockIndex, u32 offset,
				const void* data, u32 byteCount);

			//! Adds a (set, binding) to DescriptorBindings, or ORs the stage bit into the existing
			//! entry. Warns when two stages declare the same slot as different descriptor types.
			void addDescriptorBinding(u32 set, u32 binding, VkDescriptorType type, u32 count,
				VkShaderStageFlags stageBit, const core::stringc& name);

			//! Captured by compileFromSource() so the destructor needs no context.
			VkDevice Device = VK_NULL_HANDLE;
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
