// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanCompute.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_
#include "CVulkanShaderCompiler.h"
#include "CVulkanHardwareBuffer.h"
#include "CVulkanTexture.h"
#include "CVulkanPipelineCache.h" // vulkanHandleHash()
#include "matrix4.h"
#include "os.h"
#include <string.h>

namespace irr
{
	namespace video
	{
		namespace
		{
			// --- The slice of the SPIR-V binary format this reflector needs ---
			// Same hand-written approach, and the same symbol-table-indexed-by-id walk, as the
			// reflector CVulkanUserMaterial.cpp carries for the graphics stages. That one lives in an
			// anonymous namespace (internal linkage, nothing declared in a header), so it cannot be
			// called from here; the parsing helpers are duplicated instead, trimmed to what a compute
			// shader declares -- uniform blocks and their members. No external reflection library.
			enum
			{
				SpvOpName = 5, SpvOpMemberName = 6,
				SpvOpTypeBool = 20, SpvOpTypeInt = 21, SpvOpTypeFloat = 22, SpvOpTypeVector = 23,
				SpvOpTypeMatrix = 24, SpvOpTypeImage = 25, SpvOpTypeSampler = 26,
				SpvOpTypeSampledImage = 27, SpvOpTypeArray = 28, SpvOpTypeRuntimeArray = 29,
				SpvOpTypeStruct = 30, SpvOpTypePointer = 32, SpvOpConstant = 43, SpvOpVariable = 59,
				SpvOpDecorate = 71, SpvOpMemberDecorate = 72
			};

			enum
			{
				SpvDecorationBlock = 2, SpvDecorationBufferBlock = 3, SpvDecorationRowMajor = 4,
				SpvDecorationColMajor = 5, SpvDecorationArrayStride = 6, SpvDecorationMatrixStride = 7,
				SpvDecorationBinding = 33, SpvDecorationDescriptorSet = 34, SpvDecorationOffset = 35
			};

			enum
			{
				SpvStorageClassUniformConstant = 0, SpvStorageClassUniform = 2,
				SpvStorageClassPushConstant = 9, SpvStorageClassStorageBuffer = 12
			};

			//! SPIR-V majorness is the mirror of the source language's: a SPIR-V matrix is a list of
			//! columns, an HLSL one a list of rows.
			enum { SpirvMajorUndecorated = 0, SpirvMajorCol = 1, SpirvMajorRow = 2 };

			const u32 SpirvNoValue = 0xFFFFFFFFu;
			const u32 SpirvMaxIdBound = 4u * 1024u * 1024u;

			struct SSpirvId
			{
				u16 Op = 0;
				u32 Word0 = 0;
				u32 Word1 = 0;
				u32 Value = 0;
				u32 Set = SpirvNoValue;
				u32 Binding = SpirvNoValue;
				u32 ArrayStride = 0;
				bool IsBlock = false;
				bool IsBufferBlock = false;
				core::stringc Name;
				std::vector<u32> MemberTypes;
				std::vector<core::stringc> MemberNames;
				std::vector<u32> MemberOffsets;
				std::vector<u32> MemberMatrixStrides;
				std::vector<u8> MemberMajor;
			};

			struct SSpirvVariable
			{
				u32 TypeId = 0;
				u32 ResultId = 0;
				u32 StorageClass = 0;
			};

			void ensureMemberSlot(SSpirvId& id, u32 member)
			{
				if (id.MemberNames.size() <= member)
				{
					id.MemberNames.resize(member + 1);
					id.MemberOffsets.resize(member + 1, SpirvNoValue);
					id.MemberMatrixStrides.resize(member + 1, 0);
					id.MemberMajor.resize(member + 1, (u8)SpirvMajorUndecorated);
				}
			}

			//! Packed four bytes per word, low byte first, NUL-terminated, word padded.
			core::stringc decodeSpirvString(const u32* words, u32 maxWords)
			{
				core::stringc text;

				for (u32 i = 0; i < maxWords; ++i)
				{
					const u32 word = words[i];

					for (u32 b = 0; b < 4; ++b)
					{
						const c8 character = (c8)((word >> (b * 8)) & 0xFF);
						if (!character)
							return text;
						text += character;
					}
				}

				return text;
			}

			//! Size in bytes inside a std140/std430 block. `matrixStride` and `major` are the
			//! decorations of the MEMBER being measured, which is where SPIR-V puts them.
			u32 spirvTypeSize(const std::vector<SSpirvId>& ids, u32 typeId, u32 matrixStride, u8 major, u32 depth)
			{
				if (typeId >= ids.size() || depth > 16)
					return 0;

				const SSpirvId& type = ids[typeId];

				switch (type.Op)
				{
				case SpvOpTypeBool:
					return 4;
				case SpvOpTypeInt:
				case SpvOpTypeFloat:
					return type.Word0 / 8; // Word0 is the width in bits
				case SpvOpTypeVector:
					return type.Word1 * spirvTypeSize(ids, type.Word0, 0, (u8)SpirvMajorUndecorated, depth + 1);
				case SpvOpTypeMatrix:
				{
					const u32 columns = type.Word1;
					u32 rows = 0;
					if (type.Word0 < ids.size() && ids[type.Word0].Op == SpvOpTypeVector)
						rows = ids[type.Word0].Word1;
					const u32 stride = matrixStride ? matrixStride : 16;
					return stride * ((major == SpirvMajorRow) ? rows : columns);
				}
				case SpvOpTypeArray:
				{
					u32 length = 0;
					if (type.Word1 < ids.size() && ids[type.Word1].Op == SpvOpConstant)
						length = ids[type.Word1].Value;
					u32 stride = type.ArrayStride;
					if (!stride)
						stride = spirvTypeSize(ids, type.Word0, matrixStride, major, depth + 1);
					return stride * length;
				}
				case SpvOpTypeStruct:
				{
					// SPIR-V records no cumulative size: it is the highest member end.
					u32 size = 0;

					for (size_t m = 0; m < type.MemberTypes.size(); ++m)
					{
						const u32 offset = (m < type.MemberOffsets.size()) ? type.MemberOffsets[m] : SpirvNoValue;
						if (offset == SpirvNoValue)
							continue;
						const u32 memberStride = (m < type.MemberMatrixStrides.size()) ? type.MemberMatrixStrides[m] : 0;
						const u8 memberMajor = (m < type.MemberMajor.size()) ? type.MemberMajor[m] : (u8)SpirvMajorUndecorated;
						const u32 end = offset + spirvTypeSize(ids, type.MemberTypes[m], memberStride, memberMajor, depth + 1);
						if (end > size)
							size = end;
					}

					return (size + 15u) & ~15u; // std140 rounds a block up to a vec4
				}
				default:
					return 0;
				}
			}

			//! The one shape core::matrix4 knows how to transpose. See SVulkanComputeVariable.
			bool spirvIsTransposableMatrix(const std::vector<SSpirvId>& ids, u32 typeId, u8 major)
			{
				if (major != SpirvMajorRow || typeId >= ids.size())
					return false;

				const SSpirvId& matrix = ids[typeId];
				if (matrix.Op != SpvOpTypeMatrix || matrix.Word1 != 4 || matrix.Word0 >= ids.size())
					return false;

				const SSpirvId& column = ids[matrix.Word0];
				if (column.Op != SpvOpTypeVector || column.Word1 != 4 || column.Word0 >= ids.size())
					return false;

				const SSpirvId& component = ids[column.Word0];
				return component.Op == SpvOpTypeFloat && component.Word0 == 32;
			}

			void logCompute(const c8* what, const c8* detail, ELOG_LEVEL level)
			{
				core::stringc message = "CVulkanCompute: ";
				message += what;
				if (detail)
					message += detail;
				os::Printer::log(message.c_str(), level);
			}
		}

		// ============================== CVulkanComputeMaterial ==============================

		//! The module is this object's own, so it dies with it -- the driver must drop() its compute
		//! materials before the device goes.
		CVulkanComputeMaterial::~CVulkanComputeMaterial()
		{
			if (Device && Module != VK_NULL_HANDLE && vk::DestroyShaderModule)
				vk::DestroyShaderModule(Device, Module, nullptr);
			Module = VK_NULL_HANDLE;

			if (CallBack)
			{
				CallBack->drop();
				CallBack = nullptr;
			}
		}

		const c8* CVulkanComputeMaterial::getEntryPointName() const
		{
			return EntryPoint.size() ? EntryPoint.c_str() : "main";
		}

		const SVulkanComputeUniformBlock* CVulkanComputeMaterial::getBoundBlock() const
		{
			return Blocks.empty() ? nullptr : &Blocks[0];
		}

		bool CVulkanComputeMaterial::compileFromSource(const SVulkanContext& context,
			E_GPU_SHADING_LANGUAGE lang, const c8* source, u32 sourceLength, const c8* entryPoint)
		{
			if (!source || (sourceLength == 0 && source[0] == 0))
			{
				logCompute("compileFromSource: a compute shader source is required", nullptr, ELL_ERROR);
				return false;
			}

			if (!context.Device || !vk::CreateShaderModule)
			{
				logCompute("compileFromSource: no device, or vkCreateShaderModule was never resolved",
					nullptr, ELL_ERROR);
				return false;
			}

			if (!CVulkanShaderCompiler::isLanguageSupported(lang))
			{
				logCompute("this build cannot compile shaders written in ",
					CVulkanShaderCompiler::getLanguageName(lang), ELL_ERROR);
				return false;
			}

			// Captured up front so the destructor can release the module even if a later step fails.
			Device = context.Device;
			const c8* name = (entryPoint && entryPoint[0]) ? entryPoint : "main";

			std::vector<u32> spirv;
			core::stringc compileError;

			if (!CVulkanShaderCompiler::compileToSpirv(source, sourceLength, name,
				EST_COMPUTE_SHADER, lang, spirv, compileError))
			{
				core::stringc message = "compute shader compilation failed (";
				message += CVulkanShaderCompiler::getLanguageName(lang);
				message += "): ";
				message += compileError;
				logCompute(message.c_str(), nullptr, ELL_ERROR);
				return false;
			}

			// Reflect first: a module whose declarations cannot be served by the fixed compute
			// layout is worth rejecting before it costs a VkShaderModule.
			if (!reflectSpirv(spirv))
				return false;

			VkShaderModuleCreateInfo createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			createInfo.codeSize = spirv.size() * sizeof(u32); // bytes, not words
			createInfo.pCode = spirv.data();

			VkShaderModule module = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanCompute: vkCreateShaderModule",
				vk::CreateShaderModule(context.Device, &createInfo, nullptr, &module)))
				return false;

			Module = module;
			EntryPoint = name;
			return true;
		}

		bool CVulkanComputeMaterial::reflectSpirv(const std::vector<u32>& spirv)
		{
			if (spirv.size() < SpirvHeaderSize / sizeof(u32) || spirv[0] != SpirvMagicWord)
			{
				logCompute("not a host-order SPIR-V module, cannot be reflected", nullptr, ELL_ERROR);
				return false;
			}

			const u32 bound = spirv[3]; // word 3 of the header, the exclusive upper bound of every id
			if (bound == 0 || bound > SpirvMaxIdBound)
			{
				logCompute("implausible SPIR-V id bound, module rejected", nullptr, ELL_ERROR);
				return false;
			}

			// Result ids are dense and below the bound, so a flat array indexed by id is the symbol
			// table; anything not recognised below is stepped over by its own word count.
			std::vector<SSpirvId> ids((size_t)bound);
			std::vector<SSpirvVariable> variables;
			// Pass one fills the symbol table; nothing is interpreted yet, so the order the module
			// lists names, decorations and types in does not matter.
			size_t offset = SpirvHeaderSize / sizeof(u32);

			while (offset < spirv.size())
			{
				const u32* words = &spirv[offset];
				const u32 wordCount = words[0] >> 16;
				const u16 opcode = (u16)(words[0] & 0xFFFFu);

				if (wordCount == 0 || offset + wordCount > spirv.size())
				{
					logCompute("truncated SPIR-V instruction stream", nullptr, ELL_ERROR);
					return false;
				}

				switch (opcode)
				{
				case SpvOpName:
					if (wordCount >= 3 && words[1] < bound)
						ids[words[1]].Name = decodeSpirvString(words + 2, wordCount - 2);
					break;
				case SpvOpMemberName:
					if (wordCount >= 4 && words[1] < bound)
					{
						SSpirvId& id = ids[words[1]];
						ensureMemberSlot(id, words[2]);
						id.MemberNames[words[2]] = decodeSpirvString(words + 3, wordCount - 3);
					}
					break;
				case SpvOpDecorate:
					if (wordCount >= 3 && words[1] < bound)
					{
						SSpirvId& id = ids[words[1]];
						switch (words[2])
						{
						case SpvDecorationBlock: id.IsBlock = true; break;
						case SpvDecorationBufferBlock: id.IsBufferBlock = true; break;
						case SpvDecorationArrayStride: if (wordCount >= 4) id.ArrayStride = words[3]; break;
						case SpvDecorationBinding: if (wordCount >= 4) id.Binding = words[3]; break;
						case SpvDecorationDescriptorSet: if (wordCount >= 4) id.Set = words[3]; break;
						default: break;
						}
					}
					break;
				case SpvOpMemberDecorate:
					if (wordCount >= 4 && words[1] < bound)
					{
						SSpirvId& id = ids[words[1]];
						ensureMemberSlot(id, words[2]);
						switch (words[3])
						{
						case SpvDecorationOffset: if (wordCount >= 5) id.MemberOffsets[words[2]] = words[4]; break;
						case SpvDecorationMatrixStride: if (wordCount >= 5) id.MemberMatrixStrides[words[2]] = words[4]; break;
						case SpvDecorationRowMajor: id.MemberMajor[words[2]] = (u8)SpirvMajorRow; break;
						case SpvDecorationColMajor: id.MemberMajor[words[2]] = (u8)SpirvMajorCol; break;
						default: break;
						}
					}
					break;
				case SpvOpTypeBool:
				case SpvOpTypeImage:
				case SpvOpTypeSampler:
					if (wordCount >= 2 && words[1] < bound)
						ids[words[1]].Op = opcode;
					break;
				case SpvOpTypeInt:
				case SpvOpTypeFloat:
				case SpvOpTypeRuntimeArray:
				case SpvOpTypeSampledImage:
					if (wordCount >= 3 && words[1] < bound)
					{
						ids[words[1]].Op = opcode;
						ids[words[1]].Word0 = words[2];
					}
					break;
				case SpvOpTypeVector:
				case SpvOpTypeMatrix:
				case SpvOpTypeArray:
					if (wordCount >= 4 && words[1] < bound)
					{
						ids[words[1]].Op = opcode;
						ids[words[1]].Word0 = words[2];
						ids[words[1]].Word1 = words[3];
					}
					break;
				case SpvOpTypePointer:
					if (wordCount >= 4 && words[1] < bound)
					{
						ids[words[1]].Op = opcode;
						ids[words[1]].Word0 = words[3]; // pointee type
						ids[words[1]].Word1 = words[2]; // storage class
					}
					break;
				case SpvOpTypeStruct:
					if (wordCount >= 2 && words[1] < bound)
					{
						SSpirvId& id = ids[words[1]];
						id.Op = opcode;
						id.MemberTypes.assign(words + 2, words + wordCount);
					}
					break;
				case SpvOpConstant:
					// Only the low word is kept: array lengths are the sole use. The result id sits
					// in words[2] here, after the result TYPE, as in OpVariable below.
					if (wordCount >= 4 && words[2] < bound)
					{
						ids[words[2]].Op = opcode;
						ids[words[2]].Value = words[3];
					}
					break;
				case SpvOpVariable:
					if (wordCount >= 4 && words[2] < bound)
					{
						SSpirvVariable variable;
						variable.TypeId = words[1];
						variable.ResultId = words[2];
						variable.StorageClass = words[3];
						variables.push_back(variable);
					}
					break;
				default:
					break;
				}

				offset += wordCount;
			}

			// Pass two: every variable is complete now, whatever order its parts appeared in.
			for (size_t v = 0; v < variables.size(); ++v)
			{
				const SSpirvVariable& variable = variables[v];
				if (variable.TypeId >= ids.size() || ids[variable.TypeId].Op != SpvOpTypePointer)
					continue;

				u32 typeId = ids[variable.TypeId].Word0;

				// An array of descriptors is declared as an array of the resource type; peel it off.
				// The length is not kept: every binding in the fixed layout has descriptorCount 1.
				while (typeId < ids.size() && ids[typeId].Op == SpvOpTypeArray)
					typeId = ids[typeId].Word0;

				if (typeId >= ids.size())
					continue;

				const SSpirvId& type = ids[typeId];
				const SSpirvId& self = ids[variable.ResultId];
				// GLSL without an explicit "set = N" emits no DescriptorSet decoration and means 0.
				const u32 set = (self.Set == SpirvNoValue) ? 0 : self.Set;
				const u32 binding = self.Binding;

				if (variable.StorageClass == SpvStorageClassPushConstant)
				{
					logCompute("push constants are not exposed by the shader-constant API, ignored: ",
						self.Name.c_str(), ELL_WARNING);
					continue;
				}

				if (variable.StorageClass == SpvStorageClassUniformConstant)
				{
					// Nothing is bound by name, so a misplaced declaration can only be warned about:
					// the dispatch writes the fixed slots and no others. A storage image is the
					// dispatchToTexture() output; anything else has no slot at all.
					if (type.Op == SpvOpTypeImage && binding != VulkanComputeDstImageBinding)
						logCompute("storage image declared outside the fixed destination binding 3, "
							"the dispatch will not write it: ", self.Name.c_str(), ELL_WARNING);
					continue;
				}

				if (variable.StorageClass == SpvStorageClassStorageBuffer ||
					(variable.StorageClass == SpvStorageClassUniform && type.IsBufferBlock))
				{
					if (binding != VulkanComputeSrcBufferBinding && binding != VulkanComputeDstBufferBinding)
						logCompute("storage buffer declared outside the fixed bindings 0 (source) and "
							"1 (destination), the dispatch will not bind it: ", type.Name.c_str(), ELL_WARNING);
					continue;
				}

				if (variable.StorageClass != SpvStorageClassUniform || type.Op != SpvOpTypeStruct || !type.IsBlock)
					continue;

				if (binding == SpirvNoValue)
				{
					logCompute("uniform block without a binding decoration, skipped: ",
						type.Name.c_str(), ELL_WARNING);
					continue;
				}

				if (set != VulkanComputeDescriptorSetIndex || binding != VulkanComputeUniformBinding)
				{
					logCompute("uniform block outside the fixed compute slot (set 0, binding 2), it will "
						"never be written: ", type.Name.c_str(), ELL_WARNING);
					continue;
				}

				// The block name is the struct's: the cbuffer name on the HLSL side (DXC prefixes it
				// with "type.") and the block name on the GLSL one.
				core::stringc blockName = type.Name;
				if (blockName.size() > 5 && blockName.subString(0, 5) == "type.")
					blockName = blockName.subString(5, (s32)blockName.size() - 5);
				if (blockName.size() == 0)
					blockName = self.Name;

				SVulkanComputeUniformBlock block;
				block.Name = blockName;
				block.Set = set;
				block.Binding = binding;
				block.Size = spirvTypeSize(ids, typeId, 0, (u8)SpirvMajorUndecorated, 0);

				if (block.Size == 0)
				{
					logCompute("uniform block with no laid-out member, skipped: ",
						blockName.c_str(), ELL_WARNING);
					continue;
				}

				if (!Blocks.empty())
				{
					logCompute("only one uniform block fits the fixed compute layout, ignoring: ",
						blockName.c_str(), ELL_WARNING);
					continue;
				}

				block.Scratch.assign(block.Size, 0);
				Blocks.push_back(block);
				const s32 blockIndex = (s32)(Blocks.size() - 1);

				for (size_t m = 0; m < type.MemberTypes.size(); ++m)
				{
					const u32 memberOffset = (m < type.MemberOffsets.size()) ? type.MemberOffsets[m] : SpirvNoValue;
					if (memberOffset == SpirvNoValue)
						continue;

					SVulkanComputeVariable member;
					member.Name = (m < type.MemberNames.size()) ? type.MemberNames[m] : core::stringc();
					if (member.Name.size() == 0)
						continue; // a module stripped of its debug names has nothing to look up

					const u32 memberStride = (m < type.MemberMatrixStrides.size()) ? type.MemberMatrixStrides[m] : 0;
					const u8 memberMajor = (m < type.MemberMajor.size()) ? type.MemberMajor[m] : (u8)SpirvMajorUndecorated;
					member.Block = blockIndex;
					member.Offset = memberOffset;
					member.Size = spirvTypeSize(ids, type.MemberTypes[m], memberStride, memberMajor, 0);
					member.TransposeOnSet = spirvIsTransposableMatrix(ids, type.MemberTypes[m], memberMajor);
					Variables.push_back(member);
				}
			}

			return true;
		}

		s32 CVulkanComputeMaterial::getConstantBufferID(const c8* name) const
		{
			if (!name)
				return -1;

			for (size_t i = 0; i < Blocks.size(); ++i)
				if (Blocks[i].Name == name)
					return (s32)i;

			return -1;
		}

		bool CVulkanComputeMaterial::setConstantBuffer(s32 id, const void* data, size_t dataSizeBytes)
		{
			if (id < 0 || (size_t)id >= Blocks.size() || !data)
				return false;

			// Truncating rather than refusing keeps a caller passing an oversized struct working, the
			// same tolerance the D3D12 constant setters have.
			SVulkanComputeUniformBlock& block = Blocks[id];
			const size_t bytes = (dataSizeBytes < block.Scratch.size()) ? dataSizeBytes : block.Scratch.size();
			memcpy(block.Scratch.data(), data, bytes);
			return true;
		}

		s32 CVulkanComputeMaterial::getVariableID(const c8* name) const
		{
			if (!name)
				return -1;

			for (size_t i = 0; i < Variables.size(); ++i)
				if (Variables[i].Name == name)
					return (s32)i;

			return -1;
		}

		bool CVulkanComputeMaterial::setVariableRaw(s32 id, const void* data, u32 byteCount)
		{
			if (id < 0 || (size_t)id >= Variables.size() || !data || byteCount == 0)
				return false;

			const SVulkanComputeVariable& variable = Variables[id];
			if (variable.Block < 0 || (size_t)variable.Block >= Blocks.size())
				return false;

			SVulkanComputeUniformBlock& block = Blocks[variable.Block];
			if (variable.Offset >= block.Scratch.size())
				return false;

			// Truncate to whichever is smaller: the variable, or what is left of the block.
			u32 bytes = (byteCount < variable.Size) ? byteCount : variable.Size;
			const u32 room = (u32)block.Scratch.size() - variable.Offset;
			if (bytes > room)
				bytes = room;

			memcpy(block.Scratch.data() + variable.Offset, data, bytes);
			return true;
		}

		bool CVulkanComputeMaterial::setVariable(s32 id, const f32* floats, int count)
		{
			if (id < 0 || (size_t)id >= Variables.size() || !floats || count <= 0)
				return false;

			// core::matrix4 is the only shape worth transposing, and only when the reflector saw the
			// RowMajor decoration DXC emits -- see SVulkanComputeVariable.
			if (Variables[id].TransposeOnSet && count >= 16)
			{
				core::matrix4 transposed;
				memcpy(transposed.pointer(), floats, 16 * sizeof(f32));
				transposed = transposed.getTransposed();
				return setVariableRaw(id, transposed.pointer(), 16 * sizeof(f32));
			}

			return setVariableRaw(id, floats, (u32)count * sizeof(f32));
		}

		bool CVulkanComputeMaterial::setVariable(s32 id, const s32* ints, int count)
		{
			if (!ints || count <= 0)
				return false;
			return setVariableRaw(id, ints, (u32)count * sizeof(s32));
		}

		bool CVulkanComputeMaterial::setVariable(const c8* name, const f32* floats, int count)
		{
			return setVariable(getVariableID(name), floats, count);
		}

		bool CVulkanComputeMaterial::setVariable(const c8* name, const s32* ints, int count)
		{
			return setVariable(getVariableID(name), ints, count);
		}

		// ================================== CVulkanCompute ==================================

		// The context is held by reference, not copied: the driver owns it and outlives this object.
		CVulkanCompute::CVulkanCompute(const SVulkanContext& context)
			: Context(context)
		{
		}

		CVulkanCompute::~CVulkanCompute()
		{
			clear();
		}

		bool CVulkanCompute::init()
		{
			if (!Context.Device || !vk::CreateDescriptorSetLayout || !vk::CreatePipelineLayout ||
				!vk::CreateDescriptorPool || !vk::CreateComputePipelines || !vk::CmdDispatch)
			{
				logCompute("init: no device, or the compute entry points were never resolved",
					nullptr, ELL_ERROR);
				return false;
			}

			// The four fixed slots, documented at the top of CVulkanCompute.h. Every binding is
			// visible to the compute stage only -- there is no other stage in this layout.
			// Compute visibility only -- no other stage exists in this layout.
			VkDescriptorSetLayoutBinding bindings[4] = {};
			bindings[0].binding = VulkanComputeSrcBufferBinding;
			bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			bindings[0].descriptorCount = 1;
			bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			bindings[1].binding = VulkanComputeDstBufferBinding;
			bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			bindings[1].descriptorCount = 1;
			bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			bindings[2].binding = VulkanComputeUniformBinding;
			bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			bindings[2].descriptorCount = 1;
			bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			bindings[3].binding = VulkanComputeDstImageBinding;
			bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[3].descriptorCount = 1;
			bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkDescriptorSetLayoutCreateInfo layoutInfo = {};
			layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			layoutInfo.bindingCount = 4;
			layoutInfo.pBindings = bindings;

			if (vulkanFailed("CVulkanCompute: vkCreateDescriptorSetLayout",
				vk::CreateDescriptorSetLayout(Context.Device, &layoutInfo, nullptr, &SetLayout)))
				return false;

			// One set and no push constant range: the whole compute interface is the four bindings.
			VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
			pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pipelineLayoutInfo.setLayoutCount = 1;
			pipelineLayoutInfo.pSetLayouts = &SetLayout;

			if (vulkanFailed("CVulkanCompute: vkCreatePipelineLayout",
				vk::CreatePipelineLayout(Context.Device, &pipelineLayoutInfo, nullptr, &Layout)))
			{
				clear();
				return false;
			}

			// Sized for VulkanComputeMaxDescriptorSets dispatches before the pool has to recycle;
			// two storage buffers per set, since bindings 0 and 1 are both of that type.
			VkDescriptorPoolSize poolSizes[3] = {};
			poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			poolSizes[0].descriptorCount = 2 * VulkanComputeMaxDescriptorSets;
			poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			poolSizes[1].descriptorCount = VulkanComputeMaxDescriptorSets;
			poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			poolSizes[2].descriptorCount = VulkanComputeMaxDescriptorSets;

			VkDescriptorPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			poolInfo.maxSets = VulkanComputeMaxDescriptorSets;
			poolInfo.poolSizeCount = 3;
			poolInfo.pPoolSizes = poolSizes;

			if (vulkanFailed("CVulkanCompute: vkCreateDescriptorPool",
				vk::CreateDescriptorPool(Context.Device, &poolInfo, nullptr, &Pool)))
			{
				clear();
				return false;
			}

			// Optional: pipeline creation works without it, it only shares compilation work.
			if (vk::CreatePipelineCache)
			{
				VkPipelineCacheCreateInfo cacheInfo = {};
				cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
				vk::CreatePipelineCache(Context.Device, &cacheInfo, nullptr, &PipelineCache);
			}

			return true;
		}

		// Pipelines first, then what they were built against: destroying a layout still referenced by
		// a live pipeline is undefined.
		void CVulkanCompute::clear()
		{
			if (!Context.Device)
				return;

			for (auto& entry : Pipelines)
				if (entry.second != VK_NULL_HANDLE && vk::DestroyPipeline)
					vk::DestroyPipeline(Context.Device, entry.second, nullptr);
			Pipelines.clear();

			if (PipelineCache != VK_NULL_HANDLE && vk::DestroyPipelineCache)
			{
				vk::DestroyPipelineCache(Context.Device, PipelineCache, nullptr);
				PipelineCache = VK_NULL_HANDLE;
			}

			if (Pool != VK_NULL_HANDLE && vk::DestroyDescriptorPool)
			{
				vk::DestroyDescriptorPool(Context.Device, Pool, nullptr);
				Pool = VK_NULL_HANDLE;
			}

			if (Layout != VK_NULL_HANDLE && vk::DestroyPipelineLayout)
			{
				vk::DestroyPipelineLayout(Context.Device, Layout, nullptr);
				Layout = VK_NULL_HANDLE;
			}

			if (SetLayout != VK_NULL_HANDLE && vk::DestroyDescriptorSetLayout)
			{
				vk::DestroyDescriptorSetLayout(Context.Device, SetLayout, nullptr);
				SetLayout = VK_NULL_HANDLE;
			}

			if (UniformMapped && vk::UnmapMemory)
				vk::UnmapMemory(Context.Device, UniformMemory);
			UniformMapped = nullptr;

			if (UniformBuffer != VK_NULL_HANDLE && vk::DestroyBuffer)
			{
				vk::DestroyBuffer(Context.Device, UniformBuffer, nullptr);
				UniformBuffer = VK_NULL_HANDLE;
			}

			if (UniformMemory != VK_NULL_HANDLE && vk::FreeMemory)
			{
				vk::FreeMemory(Context.Device, UniformMemory, nullptr);
				UniformMemory = VK_NULL_HANDLE;
			}

			UniformCapacity = 0;
			UniformRange = 0;
		}

		VkPipeline CVulkanCompute::getOrCreatePipeline(VkShaderModule module, const c8* entryPoint)
		{
			if (module == VK_NULL_HANDLE || Layout == VK_NULL_HANDLE)
				return VK_NULL_HANDLE;

			// The module handle is the whole key -- there is no vertex input, blend or attachment
			// state in a compute pipeline, so nothing else can make two of them differ.
			const size_t key = vulkanHandleHash(module);
			auto it = Pipelines.find(key);
			if (it != Pipelines.end())
				return it->second;

			VkComputePipelineCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			info.stage.module = module;
			info.stage.pName = (entryPoint && entryPoint[0]) ? entryPoint : "main";
			info.layout = Layout;
			info.basePipelineIndex = -1;

			VkPipeline pipeline = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanCompute: vkCreateComputePipelines",
				vk::CreateComputePipelines(Context.Device, PipelineCache, 1, &info, nullptr, &pipeline)))
				return VK_NULL_HANDLE;

			Pipelines[key] = pipeline;
			return pipeline;
		}

		VkDescriptorSet CVulkanCompute::allocateDescriptorSet()
		{
			if (Pool == VK_NULL_HANDLE || SetLayout == VK_NULL_HANDLE)
				return VK_NULL_HANDLE;

			VkDescriptorSetAllocateInfo allocInfo = {};
			allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocInfo.descriptorPool = Pool;
			allocInfo.descriptorSetCount = 1;
			allocInfo.pSetLayouts = &SetLayout;

			// Sets are never freed individually: the pool is recycled whole when it runs out.
			VkDescriptorSet set = VK_NULL_HANDLE;
			VkResult result = vk::AllocateDescriptorSets(Context.Device, &allocInfo, &set);

			if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
			{
				// Recycling the whole pool invalidates the sets of earlier dispatches, which is safe
				// only under the synchronous contract documented on allocateDescriptorSet().
				if (vk::ResetDescriptorPool)
					vk::ResetDescriptorPool(Context.Device, Pool, 0);
				result = vk::AllocateDescriptorSets(Context.Device, &allocInfo, &set);
			}

			if (vulkanFailed("CVulkanCompute: vkAllocateDescriptorSets", result))
				return VK_NULL_HANDLE;

			return set;
		}

		bool CVulkanCompute::ensureUniformCapacity(VkDeviceSize size)
		{
			if (size == 0)
				return false;
			if (UniformBuffer != VK_NULL_HANDLE && UniformCapacity >= size)
				return true;

			if (UniformMapped && vk::UnmapMemory)
				vk::UnmapMemory(Context.Device, UniformMemory);
			UniformMapped = nullptr;

			if (UniformBuffer != VK_NULL_HANDLE && vk::DestroyBuffer)
				vk::DestroyBuffer(Context.Device, UniformBuffer, nullptr);
			if (UniformMemory != VK_NULL_HANDLE && vk::FreeMemory)
				vk::FreeMemory(Context.Device, UniformMemory, nullptr);
			UniformBuffer = VK_NULL_HANDLE;
			UniformMemory = VK_NULL_HANDLE;
			UniformCapacity = 0;

			// Rounded up so a block growing a few bytes at a time does not reallocate every dispatch.
			// Powers of two from 256: a block growing a few bytes at a time then costs no realloc.
			VkDeviceSize capacity = 256;
			while (capacity < size)
				capacity *= 2;

			if (!createVulkanBuffer(Context, capacity, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				UniformBuffer, UniformMemory))
				return false;

			if (vulkanFailed("CVulkanCompute: vkMapMemory (uniform block)",
				vk::MapMemory(Context.Device, UniformMemory, 0, capacity, 0, &UniformMapped)))
			{
				UniformMapped = nullptr;
				return false;
			}

			UniformCapacity = capacity;
			return true;
		}

		bool CVulkanCompute::uploadUniformBlock(CVulkanComputeMaterial* material)
		{
			const SVulkanComputeUniformBlock* block = material->getBoundBlock();
			if (!block || block->Scratch.empty())
				return false;

			if (!ensureUniformCapacity((VkDeviceSize)block->Scratch.size()))
				return false;

			// HOST_COHERENT memory, so no explicit flush is needed before the queue reads it.
			memcpy(UniformMapped, block->Scratch.data(), block->Scratch.size());
			UniformRange = (VkDeviceSize)block->Scratch.size();
			return true;
		}

		bool CVulkanCompute::prepareDispatch(CVulkanComputeMaterial* material, CVulkanHardwareBuffer* src,
			const core::vector3d<u32>& groupCount, const c8* what,
			VkPipeline& outPipeline, VkDescriptorSet& outSet, bool& outHasUniform)
		{
			if (Layout == VK_NULL_HANDLE)
			{
				logCompute(what, ": compute was never initialised", ELL_ERROR);
				return false;
			}

			if (!material || material->getModule() == VK_NULL_HANDLE || !src)
			{
				logCompute(what, ": no compute material, or no source buffer", ELL_ERROR);
				return false;
			}

			if (groupCount.X == 0 || groupCount.Y == 0 || groupCount.Z == 0)
				return false;

			// Over the limit the dispatch is a validation error and the device may be lost, so it is
			// worth one check here rather than a driver crash later.
			const u32* limit = Context.DeviceProperties.limits.maxComputeWorkGroupCount;
			if (groupCount.X > limit[0] || groupCount.Y > limit[1] || groupCount.Z > limit[2])
			{
				logCompute(what, ": group count exceeds maxComputeWorkGroupCount", ELL_ERROR);
				return false;
			}

			outPipeline = getOrCreatePipeline(material->getModule(), material->getEntryPointName());
			if (outPipeline == VK_NULL_HANDLE)
				return false;

			// False here just means the shader declared no block, so binding 2 stays unwritten.
			outHasUniform = uploadUniformBlock(material);

			outSet = allocateDescriptorSet();
			return outSet != VK_NULL_HANDLE;
		}

		bool CVulkanCompute::dispatch(VkCommandBuffer cmd, CVulkanComputeMaterial* material,
			CVulkanHardwareBuffer* src, CVulkanHardwareBuffer* dst,
			const core::vector3d<u32>& groupCount)
		{
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkDescriptorSet set = VK_NULL_HANDLE;
			bool hasUniform = false;

			if (!cmd || !dst || dst->getBuffer() == VK_NULL_HANDLE ||
				!prepareDispatch(material, src, groupCount, "dispatch", pipeline, set, hasUniform))
				return false;

			// VK_WHOLE_SIZE rather than the tracked size: a storage buffer's length is whatever the
			// shader's runtime array finds, and the buffer may have been grown by an update().
			VkDescriptorBufferInfo buffers[3] = {};
			buffers[0].buffer = src->getBuffer();
			buffers[0].range = VK_WHOLE_SIZE;
			buffers[1].buffer = dst->getBuffer();
			buffers[1].range = VK_WHOLE_SIZE;
			buffers[2].buffer = UniformBuffer;
			buffers[2].range = UniformRange;

			VkWriteDescriptorSet writes[3] = {};
			u32 writeCount = 0;

			for (u32 i = 0; i < 3; ++i)
			{
				// The uniform slot is skipped when the shader declared no block: Vulkan only requires
				// a descriptor the pipeline statically uses to be written.
				if (i == 2 && !hasUniform)
					continue;

				writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[writeCount].dstSet = set;
				writes[writeCount].dstBinding = i;
				writes[writeCount].descriptorCount = 1;
				writes[writeCount].descriptorType = (i == 2) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
					VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				writes[writeCount].pBufferInfo = &buffers[i];
				++writeCount;
			}

			vk::UpdateDescriptorSets(Context.Device, writeCount, writes, 0, nullptr);

			// COMPUTE bind point, so none of this disturbs the graphics state the driver has bound --
			// but the command buffer must still be outside a rendering instance, see the header.
			vk::CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			vk::CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Layout,
				VulkanComputeDescriptorSetIndex, 1, &set, 0, nullptr);
			vk::CmdDispatch(cmd, groupCount.X, groupCount.Y, groupCount.Z);
			return true;
		}

		bool CVulkanCompute::dispatchToTexture(VkCommandBuffer cmd, CVulkanComputeMaterial* material,
			CVulkanHardwareBuffer* src, CVulkanTexture* dst, const core::vector3d<u32>& groupCount)
		{
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkDescriptorSet set = VK_NULL_HANDLE;
			bool hasUniform = false;

			if (!cmd || !dst || !dst->hasDeviceResource() || dst->getImageView() == VK_NULL_HANDLE ||
				!prepareDispatch(material, src, groupCount, "dispatchToTexture", pipeline, set, hasUniform))
				return false;

			// A storage image descriptor is only valid in GENERAL, and only this call knows the image
			// is about to be used as one -- hence the single barrier this class issues on its own.
			// Everything else around the dispatch stays the driver's, see CVulkanCompute.h.
			barrierImageToStorage(cmd, dst);

			// Binding 3 instead of binding 1: same output, the other shape. Binding 1 is left
			// unwritten, which is legal as long as this shader does not declare it.
			VkDescriptorImageInfo imageInfo = {};
			imageInfo.imageView = dst->getImageView();
			imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

			VkDescriptorBufferInfo buffers[2] = {};
			buffers[0].buffer = src->getBuffer();
			buffers[0].range = VK_WHOLE_SIZE;
			buffers[1].buffer = UniformBuffer;
			buffers[1].range = UniformRange;

			VkWriteDescriptorSet writes[3] = {};
			u32 writeCount = 0;

			writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[writeCount].dstSet = set;
			writes[writeCount].dstBinding = VulkanComputeSrcBufferBinding;
			writes[writeCount].descriptorCount = 1;
			writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[writeCount].pBufferInfo = &buffers[0];
			++writeCount;

			writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[writeCount].dstSet = set;
			writes[writeCount].dstBinding = VulkanComputeDstImageBinding;
			writes[writeCount].descriptorCount = 1;
			writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writes[writeCount].pImageInfo = &imageInfo;
			++writeCount;

			if (hasUniform)
			{
				writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[writeCount].dstSet = set;
				writes[writeCount].dstBinding = VulkanComputeUniformBinding;
				writes[writeCount].descriptorCount = 1;
				writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				writes[writeCount].pBufferInfo = &buffers[1];
				++writeCount;
			}

			vk::UpdateDescriptorSets(Context.Device, writeCount, writes, 0, nullptr);

			vk::CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			vk::CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Layout,
				VulkanComputeDescriptorSetIndex, 1, &set, 0, nullptr);
			vk::CmdDispatch(cmd, groupCount.X, groupCount.Y, groupCount.Z);
			return true;
		}

		// ==================================== Barriers ====================================

		void CVulkanCompute::bufferBarrier(VkCommandBuffer cmd, VkBuffer buffer,
			VkDeviceSize offset, VkDeviceSize size,
			VkAccessFlags srcAccess, VkAccessFlags dstAccess,
			VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
		{
			if (!cmd || buffer == VK_NULL_HANDLE || !vk::CmdPipelineBarrier)
				return;

			VkBufferMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			barrier.srcAccessMask = srcAccess;
			barrier.dstAccessMask = dstAccess;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer = buffer;
			barrier.offset = offset;
			barrier.size = size;

			vk::CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
		}

		// The D3D12 side gets this ordering from transitionTo(NON_PIXEL_SHADER_RESOURCE) and
		// transitionTo(UNORDERED_ACCESS); Vulkan has to be told.
		void CVulkanCompute::barrierBeforeDispatch(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst)
		{
			// TRANSFER covers the staging upload a device-local buffer was filled through; the
			// SHADER_WRITE half covers a previous dispatch that produced this input.
			const VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT |
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT;

			bufferBarrier(cmd, src, 0, VK_WHOLE_SIZE,
				VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT, srcStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

			bufferBarrier(cmd, dst, 0, VK_WHOLE_SIZE,
				VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				srcStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}

		void CVulkanCompute::barrierAfterDispatch(VkCommandBuffer cmd, VkBuffer dst)
		{
			// Both consumers at once: another dispatch or a draw reading the result, and the
			// read-back copy the download path records.
			bufferBarrier(cmd, dst, 0, VK_WHOLE_SIZE,
				VK_ACCESS_SHADER_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
				VK_PIPELINE_STAGE_HOST_BIT);
		}

		void CVulkanCompute::imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
			u32 mipLevels, VkImageLayout oldLayout, VkImageLayout newLayout,
			VkAccessFlags srcAccess, VkAccessFlags dstAccess,
			VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
		{
			if (!cmd || image == VK_NULL_HANDLE || !vk::CmdPipelineBarrier)
				return;

			// Whole image, every mip, one layer: nothing here targets an array or a single level.
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = oldLayout;
			barrier.newLayout = newLayout;
			barrier.srcAccessMask = srcAccess;
			barrier.dstAccessMask = dstAccess;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange.aspectMask = aspect;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = mipLevels ? mipLevels : 1;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = 1;

			vk::CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}

		// TOP_OF_PIPE in the source stage covers a texture still in UNDEFINED, which has no prior
		// access to wait on; the read stages cover one that was being sampled.
		void CVulkanCompute::barrierImageToStorage(VkCommandBuffer cmd, CVulkanTexture* texture)
		{
			if (!texture || !texture->hasDeviceResource())
				return;

			const VkImageLayout current = texture->getImageLayout();
			if (current == VK_IMAGE_LAYOUT_GENERAL)
				return;

			imageBarrier(cmd, texture->getImage(), texture->getAspectMask(), texture->getMipLevelCount(),
				current, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

			// Keeps the layout the texture tracks in step with what was just recorded.
			texture->setImageLayout(VK_IMAGE_LAYOUT_GENERAL);
		}

		// The counterpart of the transitionTo(PIXEL_SHADER_RESOURCE) the D3D12 dispatch-to-texture
		// path ends on, so the next draw can sample what was just written.
		void CVulkanCompute::barrierImageToShaderRead(VkCommandBuffer cmd, CVulkanTexture* texture)
		{
			if (!texture || !texture->hasDeviceResource())
				return;

			const VkImageLayout current = texture->getImageLayout();
			if (current == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
				return;

			imageBarrier(cmd, texture->getImage(), texture->getAspectMask(), texture->getMipLevelCount(),
				current, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

			texture->setImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
