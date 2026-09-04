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
#include <algorithm>

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
			// shader declares. No external reflection library.
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

			//! OpTypeImage operands this reflector reads: Dim 5 is a texel buffer, and the "Sampled"
			//! operand says 1 for a texture that is sampled, 2 for a storage image.
			enum { SpvDimBuffer = 5, SpvImageSampled = 1, SpvImageStorage = 2 };

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

			const c8* descriptorTypeName(VkDescriptorType type)
			{
				switch (type)
				{
				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return "storage buffer";
				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return "uniform block";
				case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "storage image";
				case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "sampled image";
				case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "combined image sampler";
				case VK_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
				default: return "descriptor";
				}
			}

			bool bindingLess(const SVulkanComputeBinding& a, const SVulkanComputeBinding& b)
			{
				return a.Binding < b.Binding;
			}
		}

		// ============================== CVulkanComputeMaterial ==============================

		//! The module and the objects built on it are this object's own, so they die with it -- the
		//! driver must drop() its compute materials before the device goes.
		CVulkanComputeMaterial::~CVulkanComputeMaterial()
		{
			if (Device)
			{
				if (Pipeline != VK_NULL_HANDLE && vk::DestroyPipeline)
					vk::DestroyPipeline(Device, Pipeline, nullptr);
				if (PipelineLayout != VK_NULL_HANDLE && vk::DestroyPipelineLayout)
					vk::DestroyPipelineLayout(Device, PipelineLayout, nullptr);
				if (SetLayout != VK_NULL_HANDLE && vk::DestroyDescriptorSetLayout)
					vk::DestroyDescriptorSetLayout(Device, SetLayout, nullptr);
				if (Module != VK_NULL_HANDLE && vk::DestroyShaderModule)
					vk::DestroyShaderModule(Device, Module, nullptr);
			}
			Pipeline = VK_NULL_HANDLE;
			PipelineLayout = VK_NULL_HANDLE;
			SetLayout = VK_NULL_HANDLE;
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

		const SVulkanComputeBinding* CVulkanComputeMaterial::findBinding(u32 binding) const
		{
			for (size_t i = 0; i < Bindings.size(); ++i)
				if (Bindings[i].Binding == binding)
					return &Bindings[i];
			return nullptr;
		}

		const SVulkanComputeBinding* CVulkanComputeMaterial::findBindingByName(const c8* name) const
		{
			if (!name)
				return nullptr;
			for (size_t i = 0; i < Bindings.size(); ++i)
				if (Bindings[i].CounterOf.size() == 0 && Bindings[i].Name == name)
					return &Bindings[i];
			return nullptr;
		}

		const SVulkanComputeUniformBlock* CVulkanComputeMaterial::findBlock(u32 binding) const
		{
			for (size_t i = 0; i < Blocks.size(); ++i)
				if (Blocks[i].Binding == binding)
					return &Blocks[i];
			return nullptr;
		}

		bool CVulkanComputeMaterial::compileFromSource(const SVulkanContext& context,
			E_GPU_SHADING_LANGUAGE lang, const c8* source, u32 sourceLength, const c8* entryPoint,
			io::IFileSystem* includeFileSystem, const c8* includeDirectory)
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
				EST_COMPUTE_SHADER, lang, spirv, compileError, includeFileSystem, includeDirectory))
			{
				core::stringc message = "compute shader compilation failed (";
				message += CVulkanShaderCompiler::getLanguageName(lang);
				message += "): ";
				message += compileError;
				logCompute(message.c_str(), nullptr, ELL_ERROR);
				return false;
			}

			// Reflect first: a module whose declarations no layout can express is worth rejecting
			// before it costs a VkShaderModule.
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
				case SpvOpTypeSampler:
					if (wordCount >= 2 && words[1] < bound)
						ids[words[1]].Op = opcode;
					break;
				case SpvOpTypeImage:
					// Result, sampled type, Dim, Depth, Arrayed, MS, Sampled, Format: Dim and Sampled
					// are what decide the descriptor type.
					if (wordCount >= 9 && words[1] < bound)
					{
						ids[words[1]].Op = opcode;
						ids[words[1]].Word0 = words[3]; // Dim
						ids[words[1]].Word1 = words[7]; // Sampled
					}
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

			// Pass two: every variable is complete now, whatever order its parts appeared in. Each
			// descriptor becomes one SVulkanComputeBinding; a uniform block additionally gets its
			// scratch mirror and member table.
			for (size_t v = 0; v < variables.size(); ++v)
			{
				const SSpirvVariable& variable = variables[v];
				if (variable.TypeId >= ids.size() || ids[variable.TypeId].Op != SpvOpTypePointer)
					continue;

				u32 typeId = ids[variable.TypeId].Word0;

				// An array of descriptors is declared as an array of the resource type; peel it off.
				// The length is not kept: the layout gives every binding descriptorCount 1, so an
				// array of textures only has its first element served.
				bool isDescriptorArray = false;
				while (typeId < ids.size() && ids[typeId].Op == SpvOpTypeArray)
				{
					isDescriptorArray = true;
					typeId = ids[typeId].Word0;
				}

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

				VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
				bool isUniformBlock = false;

				if (variable.StorageClass == SpvStorageClassUniformConstant)
				{
					if (type.Op == SpvOpTypeImage)
					{
						if (type.Word0 == SpvDimBuffer)
						{
							logCompute("texel buffers (HLSL Buffer<T>/RWBuffer<T>) are not served by "
								"this driver, use a structured buffer instead: ", self.Name.c_str(), ELL_ERROR);
							return false;
						}
						descriptorType = (type.Word1 == SpvImageStorage) ?
							VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
					}
					else if (type.Op == SpvOpTypeSampledImage)
						descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					else if (type.Op == SpvOpTypeSampler)
						descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
					else
						continue; // an opaque type this reflector does not know; nothing to bind
				}
				else if (variable.StorageClass == SpvStorageClassStorageBuffer ||
					(variable.StorageClass == SpvStorageClassUniform && type.IsBufferBlock))
				{
					descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				}
				else if (variable.StorageClass == SpvStorageClassUniform && type.Op == SpvOpTypeStruct && type.IsBlock)
				{
					descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
					isUniformBlock = true;
				}
				else
				{
					continue; // inputs, outputs, workgroup memory, private variables
				}

				if (binding == SpirvNoValue)
				{
					logCompute("a resource has no binding decoration and cannot be laid out: ",
						(self.Name.size() ? self.Name : type.Name).c_str(), ELL_ERROR);
					return false;
				}

				if (set != VulkanComputeDescriptorSetIndex)
				{
					logCompute("compute resources must live in descriptor set 0 (register space 0): ",
						(self.Name.size() ? self.Name : type.Name).c_str(), ELL_ERROR);
					return false;
				}

				if (binding >= VulkanComputeMaxBindings)
				{
					logCompute("binding number past the 64 the compute slot convention covers: ",
						(self.Name.size() ? self.Name : type.Name).c_str(), ELL_ERROR);
					return false;
				}

				if (isDescriptorArray)
					logCompute("descriptor arrays get only their first element bound: ",
						self.Name.c_str(), ELL_WARNING);

				if (const SVulkanComputeBinding* existing = findBinding(binding))
				{
					core::stringc message = "two resources share binding ";
					message += core::stringc(binding);
					message += " (";
					message += existing->Name;
					message += " and ";
					message += self.Name.size() ? self.Name : type.Name;
					message += "); give them distinct [[vk::binding]]/layout(binding) numbers";
					logCompute(message.c_str(), nullptr, ELL_ERROR);
					return false;
				}

				SVulkanComputeBinding entry;
				entry.Binding = binding;
				entry.Type = descriptorType;
				entry.Name = self.Name.size() ? self.Name : type.Name;
				// DXC's hidden append/consume counter: "counter.var.<buffer>", a one-uint storage
				// buffer on a binding of its own. Remembered by the buffer it belongs to, so the
				// dispatch can bind that buffer's counter rather than treating it as a slot.
				static const c8* const counterPrefix = "counter.var.";
				static const u32 counterPrefixLength = 12;
				if (descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
					entry.Name.size() > counterPrefixLength &&
					entry.Name.subString(0, counterPrefixLength) == counterPrefix)
				{
					entry.CounterOf = entry.Name.subString(counterPrefixLength,
						(s32)entry.Name.size() - counterPrefixLength);
				}
				Bindings.push_back(entry);

				if (!isUniformBlock)
					continue;

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
					Bindings.pop_back();
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

			std::sort(Bindings.begin(), Bindings.end(), bindingLess);
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

			// Sized for VulkanComputeMaxDescriptorSets dispatches before the pool has to recycle,
			// each with as many descriptors of a kind as the slot convention allows.
			VkDescriptorPoolSize poolSizes[6] = {};
			poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			poolSizes[0].descriptorCount = 32 * VulkanComputeMaxDescriptorSets;
			poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			poolSizes[1].descriptorCount = 16 * VulkanComputeMaxDescriptorSets;
			poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			poolSizes[2].descriptorCount = 16 * VulkanComputeMaxDescriptorSets;
			poolSizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			poolSizes[3].descriptorCount = 16 * VulkanComputeMaxDescriptorSets;
			poolSizes[4].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			poolSizes[4].descriptorCount = 16 * VulkanComputeMaxDescriptorSets;
			poolSizes[5].type = VK_DESCRIPTOR_TYPE_SAMPLER;
			poolSizes[5].descriptorCount = 16 * VulkanComputeMaxDescriptorSets;

			VkDescriptorPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			poolInfo.maxSets = VulkanComputeMaxDescriptorSets;
			poolInfo.poolSizeCount = 6;
			poolInfo.pPoolSizes = poolSizes;

			if (vulkanFailed("CVulkanCompute: vkCreateDescriptorPool",
				vk::CreateDescriptorPool(Context.Device, &poolInfo, nullptr, &Pool)))
			{
				Pool = VK_NULL_HANDLE;
				return false;
			}

			// Optional: pipeline creation works without it, it only shares compilation work.
			if (vk::CreatePipelineCache)
			{
				VkPipelineCacheCreateInfo cacheInfo = {};
				cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
				vk::CreatePipelineCache(Context.Device, &cacheInfo, nullptr, &PipelineCache);
			}

			if (!createNullBuffers())
			{
				clear();
				return false;
			}

			return true;
		}

		bool CVulkanCompute::createNullBuffers()
		{
			// Host-visible so they can be zeroed by a memset, and big enough that a kernel indexing
			// an unbound buffer by thread id stays inside them (past the end robustBufferAccess
			// returns zeros anyway, which is the same answer).
			const VkMemoryPropertyFlags hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
				VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

			VkBuffer* buffers[2] = { &NullReadBuffer, &NullWriteBuffer };
			VkDeviceMemory* memories[2] = { &NullReadMemory, &NullWriteMemory };
			for (u32 i = 0; i < 2; ++i)
			{
				if (!createVulkanBuffer(Context, NullBufferSize, usage, hostFlags, *buffers[i], *memories[i]))
					return false;
				void* mapped = nullptr;
				if (vulkanFailed("CVulkanCompute: vkMapMemory (null buffer)",
					vk::MapMemory(Context.Device, *memories[i], 0, VK_WHOLE_SIZE, 0, &mapped)))
					return false;
				memset(mapped, 0, (size_t)NullBufferSize);
				vk::UnmapMemory(Context.Device, *memories[i]);
			}
			return true;
		}

		// The per-material pipelines and layouts belong to the materials (see the destructor of
		// CVulkanComputeMaterial); only the shared objects are released here.
		void CVulkanCompute::clear()
		{
			if (!Context.Device)
				return;

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

			VkBuffer* buffers[2] = { &NullReadBuffer, &NullWriteBuffer };
			VkDeviceMemory* memories[2] = { &NullReadMemory, &NullWriteMemory };
			for (u32 i = 0; i < 2; ++i)
			{
				if (*buffers[i] != VK_NULL_HANDLE && vk::DestroyBuffer)
					vk::DestroyBuffer(Context.Device, *buffers[i], nullptr);
				if (*memories[i] != VK_NULL_HANDLE && vk::FreeMemory)
					vk::FreeMemory(Context.Device, *memories[i], nullptr);
				*buffers[i] = VK_NULL_HANDLE;
				*memories[i] = VK_NULL_HANDLE;
			}
		}

		bool CVulkanCompute::ensurePipeline(CVulkanComputeMaterial* material)
		{
			if (!material || material->getModule() == VK_NULL_HANDLE)
				return false;
			if (material->Pipeline != VK_NULL_HANDLE)
				return true;
			if (!Context.Device)
				return false;

			if (material->SetLayout == VK_NULL_HANDLE)
			{
				// One VkDescriptorSetLayoutBinding per reflected descriptor, compute visibility only:
				// there is no other stage in this layout. Duplicates were rejected at reflection.
				std::vector<VkDescriptorSetLayoutBinding> bindings;
				bindings.reserve(material->Bindings.size());
				for (size_t i = 0; i < material->Bindings.size(); ++i)
				{
					VkDescriptorSetLayoutBinding binding = {};
					binding.binding = material->Bindings[i].Binding;
					binding.descriptorType = material->Bindings[i].Type;
					binding.descriptorCount = 1;
					binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
					bindings.push_back(binding);
				}

				VkDescriptorSetLayoutCreateInfo layoutInfo = {};
				layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
				layoutInfo.bindingCount = (u32)bindings.size();
				layoutInfo.pBindings = bindings.empty() ? nullptr : bindings.data();

				if (vulkanFailed("CVulkanCompute: vkCreateDescriptorSetLayout",
					vk::CreateDescriptorSetLayout(Context.Device, &layoutInfo, nullptr, &material->SetLayout)))
				{
					material->SetLayout = VK_NULL_HANDLE;
					return false;
				}
			}

			if (material->PipelineLayout == VK_NULL_HANDLE)
			{
				// One set and no push constant range: the whole compute interface is the bindings.
				VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
				pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
				pipelineLayoutInfo.setLayoutCount = 1;
				pipelineLayoutInfo.pSetLayouts = &material->SetLayout;

				if (vulkanFailed("CVulkanCompute: vkCreatePipelineLayout",
					vk::CreatePipelineLayout(Context.Device, &pipelineLayoutInfo, nullptr, &material->PipelineLayout)))
				{
					material->PipelineLayout = VK_NULL_HANDLE;
					return false;
				}
			}

			VkComputePipelineCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			info.stage.module = material->getModule();
			info.stage.pName = material->getEntryPointName();
			info.layout = material->PipelineLayout;
			info.basePipelineIndex = -1;

			VkPipeline pipeline = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanCompute: vkCreateComputePipelines",
				vk::CreateComputePipelines(Context.Device, PipelineCache, 1, &info, nullptr, &pipeline)))
				return false;

			material->Pipeline = pipeline;
			return true;
		}

		VkDescriptorSet CVulkanCompute::allocateDescriptorSet(VkDescriptorSetLayout layout)
		{
			if (Pool == VK_NULL_HANDLE || layout == VK_NULL_HANDLE)
				return VK_NULL_HANDLE;

			VkDescriptorSetAllocateInfo allocInfo = {};
			allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			allocInfo.descriptorPool = Pool;
			allocInfo.descriptorSetCount = 1;
			allocInfo.pSetLayouts = &layout;

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

		bool CVulkanCompute::uploadUniformBlocks(CVulkanComputeMaterial* material,
			std::vector<VkDeviceSize>& outOffsets)
		{
			outOffsets.clear();
			if (material->Blocks.empty())
				return true;

			// Every block at an offset the device accepts for a uniform descriptor.
			const VkDeviceSize alignment = Context.DeviceProperties.limits.minUniformBufferOffsetAlignment
				? Context.DeviceProperties.limits.minUniformBufferOffsetAlignment : 16;
			VkDeviceSize total = 0;
			for (size_t b = 0; b < material->Blocks.size(); ++b)
			{
				outOffsets.push_back(total);
				total += (material->Blocks[b].Scratch.size() + alignment - 1) & ~(alignment - 1);
			}

			if (!ensureUniformCapacity(total))
			{
				outOffsets.clear();
				return false;
			}

			// HOST_COHERENT memory, so no explicit flush is needed before the queue reads it.
			for (size_t b = 0; b < material->Blocks.size(); ++b)
				memcpy(static_cast<u8*>(UniformMapped) + outOffsets[b], material->Blocks[b].Scratch.data(),
					material->Blocks[b].Scratch.size());
			return true;
		}

		void CVulkanCompute::findOrdinalBindings(const CVulkanComputeMaterial* material,
			u32& outFirstBuffer, u32& outSecondBuffer, u32& outImage) const
		{
			outFirstBuffer = outSecondBuffer = outImage = VulkanComputeMaxBindings;
			// Bindings are sorted by number, so the first two storage buffers met are the lowest.
			for (size_t i = 0; i < material->Bindings.size(); ++i)
			{
				const SVulkanComputeBinding& binding = material->Bindings[i];
				if (binding.Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
				{
					if (outFirstBuffer == VulkanComputeMaxBindings)
						outFirstBuffer = binding.Binding;
					else if (outSecondBuffer == VulkanComputeMaxBindings)
						outSecondBuffer = binding.Binding;
				}
				else if (binding.Type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && outImage == VulkanComputeMaxBindings)
					outImage = binding.Binding;
			}
		}

		bool CVulkanCompute::dispatchBound(VkCommandBuffer cmd, CVulkanComputeMaterial* material,
			const SVulkanComputeResources& resources, const core::vector3d<u32>& groupCount,
			VkBuffer indirectBuffer, VkDeviceSize indirectOffset)
		{
			if (Pool == VK_NULL_HANDLE)
			{
				logCompute("dispatch: compute was never initialised", nullptr, ELL_ERROR);
				return false;
			}
			if (!cmd || !material || material->getModule() == VK_NULL_HANDLE)
			{
				logCompute("dispatch: no command buffer or no compute material", nullptr, ELL_ERROR);
				return false;
			}

			if (indirectBuffer == VK_NULL_HANDLE)
			{
				if (groupCount.X == 0 || groupCount.Y == 0 || groupCount.Z == 0)
					return false;

				// Over the limit the dispatch is a validation error and the device may be lost, so
				// it is worth one check here rather than a driver crash later.
				const u32* limit = Context.DeviceProperties.limits.maxComputeWorkGroupCount;
				if (groupCount.X > limit[0] || groupCount.Y > limit[1] || groupCount.Z > limit[2])
				{
					logCompute("dispatch: group count exceeds maxComputeWorkGroupCount", nullptr, ELL_ERROR);
					return false;
				}
			}
			else if (!vk::CmdDispatchIndirect)
			{
				logCompute("dispatch: vkCmdDispatchIndirect was never resolved", nullptr, ELL_ERROR);
				return false;
			}

			if (!ensurePipeline(material))
				return false;

			std::vector<VkDeviceSize> uniformOffsets;
			if (!uploadUniformBlocks(material, uniformOffsets))
				return false;

			VkDescriptorSet set = allocateDescriptorSet(material->SetLayout);
			if (set == VK_NULL_HANDLE)
				return false;

			// Reserved up front: the write structs point into these two.
			const size_t bindingCount = material->Bindings.size();
			std::vector<VkDescriptorBufferInfo> buffers;
			std::vector<VkDescriptorImageInfo> images;
			std::vector<VkWriteDescriptorSet> writes;
			std::vector<VkBuffer> counters; //!< append counters this dispatch touches, for the barriers
			buffers.reserve(bindingCount);
			images.reserve(bindingCount);
			writes.reserve(bindingCount);

			for (size_t i = 0; i < bindingCount; ++i)
			{
				const SVulkanComputeBinding& binding = material->Bindings[i];
				const SVulkanComputeResources::SEntry& entry = resources.ByBinding[binding.Binding];

				VkWriteDescriptorSet write = {};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.dstSet = set;
				write.dstBinding = binding.Binding;
				write.descriptorCount = 1;
				write.descriptorType = binding.Type;

				switch (binding.Type)
				{
				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
				{
					// Uniform blocks come from the material's own scratch, never from the slots.
					size_t blockIndex = material->Blocks.size();
					for (size_t b = 0; b < material->Blocks.size(); ++b)
						if (material->Blocks[b].Binding == binding.Binding)
							blockIndex = b;
					if (blockIndex >= material->Blocks.size() || blockIndex >= uniformOffsets.size())
						continue; // a block the reflector skipped (no laid-out member)

					VkDescriptorBufferInfo info = {};
					info.buffer = UniformBuffer;
					info.offset = uniformOffsets[blockIndex];
					info.range = material->Blocks[blockIndex].Scratch.size();
					buffers.push_back(info);
					write.pBufferInfo = &buffers.back();
					break;
				}
				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
				{
					VkBuffer target = VK_NULL_HANDLE;
					if (binding.CounterOf.size())
					{
						// The hidden counter of an append/consume buffer: the counter of whatever is
						// bound where that buffer is declared. Unbound, it counts into the scratch.
						const SVulkanComputeBinding* owner = material->findBindingByName(binding.CounterOf.c_str());
						CVulkanHardwareBuffer* ownerBuffer = owner ? resources.ByBinding[owner->Binding].Buffer : nullptr;
						target = ownerBuffer ? ownerBuffer->getCounterBuffer(true) : VK_NULL_HANDLE;
						if (target != VK_NULL_HANDLE)
						{
							counters.push_back(target);
						}
						else
							target = NullWriteBuffer;
					}
					else if (entry.Buffer && entry.Buffer->getBuffer() != VK_NULL_HANDLE)
					{
						target = entry.Buffer->getBuffer();
					}
					else
					{
						// Declared but not bound: the D3D11 null-view semantics, zeros on read and
						// discarded writes, through the two stand-in buffers.
						target = (binding.Binding >= VulkanComputeUavBindingBase &&
							binding.Binding < VulkanComputeCbvBindingBase) ? NullWriteBuffer : NullReadBuffer;
					}

					// VK_WHOLE_SIZE rather than the tracked size: a storage buffer's length is
					// whatever the shader's runtime array finds, and the buffer may have been grown
					// by an update().
					VkDescriptorBufferInfo info = {};
					info.buffer = target;
					info.range = VK_WHOLE_SIZE;
					buffers.push_back(info);
					write.pBufferInfo = &buffers.back();
					break;
				}
				case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
				{
					if (!entry.Texture || !entry.Texture->hasDeviceResource() ||
						entry.Texture->getImageView() == VK_NULL_HANDLE || !entry.Texture->isUnorderedAccess())
					{
						core::stringc message = "dispatch: no UAV texture (see addUAVTexture()) bound for "
							"storage image '";
						message += binding.Name;
						message += "' at binding ";
						message += core::stringc(binding.Binding);
						logCompute(message.c_str(), nullptr, ELL_ERROR);
						return false;
					}

					// A storage image descriptor is only valid in GENERAL.
					barrierImageToStorage(cmd, entry.Texture);

					VkDescriptorImageInfo info = {};
					info.imageView = entry.Texture->getImageView();
					info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
					images.push_back(info);
					write.pImageInfo = &images.back();
					break;
				}
				case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
				case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
				{
					if (!entry.Texture || !entry.Texture->hasDeviceResource() ||
						entry.Texture->getImageView() == VK_NULL_HANDLE)
					{
						core::stringc message = "dispatch: no texture bound for sampled image '";
						message += binding.Name;
						message += "' at binding ";
						message += core::stringc(binding.Binding);
						logCompute(message.c_str(), nullptr, ELL_ERROR);
						return false;
					}

					// A texture a previous dispatch wrote is still in GENERAL; sampling wants the
					// read-only layout.
					barrierImageToShaderRead(cmd, entry.Texture);

					VkDescriptorImageInfo info = {};
					info.imageView = entry.Texture->getImageView();
					info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					info.sampler = entry.Texture->getSampler();
					images.push_back(info);
					write.pImageInfo = &images.back();
					break;
				}
				case VK_DESCRIPTOR_TYPE_SAMPLER:
				{
					// s# has no resource of its own: it borrows the sampler of the SRV texture at
					// the same slot number, or failing that of the first SRV texture bound at all.
					const u32 slot = binding.Binding - VulkanComputeSamplerBindingBase;
					CVulkanTexture* source = (binding.Binding >= VulkanComputeSamplerBindingBase &&
						slot < VulkanComputeUavBindingBase - VulkanComputeSrvBindingBase) ?
						resources.ByBinding[VulkanComputeSrvBindingBase + slot].Texture : nullptr;
					for (u32 s = VulkanComputeSrvBindingBase; !source && s < VulkanComputeUavBindingBase; ++s)
						source = resources.ByBinding[s].Texture;
					if (!source || source->getSampler() == VK_NULL_HANDLE)
					{
						core::stringc message = "dispatch: no SRV texture bound to lend a sampler to '";
						message += binding.Name;
						message += "'";
						logCompute(message.c_str(), nullptr, ELL_ERROR);
						return false;
					}

					VkDescriptorImageInfo info = {};
					info.sampler = source->getSampler();
					images.push_back(info);
					write.pImageInfo = &images.back();
					break;
				}
				default:
					continue;
				}

				writes.push_back(write);
			}

			if (!writes.empty())
				vk::UpdateDescriptorSets(Context.Device, (u32)writes.size(), writes.data(), 0, nullptr);

			// The counters are host-visible and reset from the CPU (resetStructureCount()), read by
			// copies (copyStructureCount()) and bumped by every dispatch: the barriers around them
			// are this function's, the caller only knows about the buffers it bound.
			for (size_t i = 0; i < counters.size(); ++i)
				bufferBarrier(cmd, counters[i], 0, VK_WHOLE_SIZE,
					VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
					VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

			// COMPUTE bind point, so none of this disturbs the graphics state the driver has bound --
			// but the command buffer must still be outside a rendering instance, see the header.
			vk::CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, material->Pipeline);
			vk::CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, material->PipelineLayout,
				VulkanComputeDescriptorSetIndex, 1, &set, 0, nullptr);
			if (indirectBuffer != VK_NULL_HANDLE)
				vk::CmdDispatchIndirect(cmd, indirectBuffer, indirectOffset);
			else
				vk::CmdDispatch(cmd, groupCount.X, groupCount.Y, groupCount.Z);

			for (size_t i = 0; i < counters.size(); ++i)
				bufferBarrier(cmd, counters[i], 0, VK_WHOLE_SIZE,
					VK_ACCESS_SHADER_WRITE_BIT,
					VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			return true;
		}

		bool CVulkanCompute::dispatch(VkCommandBuffer cmd, CVulkanComputeMaterial* material,
			CVulkanHardwareBuffer* src, CVulkanHardwareBuffer* dst,
			const core::vector3d<u32>& groupCount)
		{
			if (!material || !src || !dst)
			{
				logCompute("dispatch: no compute material, or no source/destination buffer", nullptr, ELL_ERROR);
				return false;
			}

			u32 srcBinding = 0, dstBinding = 0, imageBinding = 0;
			findOrdinalBindings(material, srcBinding, dstBinding, imageBinding);
			if (srcBinding == VulkanComputeMaxBindings || dstBinding == VulkanComputeMaxBindings)
			{
				logCompute("dispatch: the shader must declare two storage buffers (source, destination): ",
					material->Name.c_str(), ELL_ERROR);
				return false;
			}

			SVulkanComputeResources resources;
			resources.setBuffer(srcBinding, src);
			resources.setBuffer(dstBinding, dst);
			return dispatchBound(cmd, material, resources, groupCount);
		}

		bool CVulkanCompute::dispatchToTexture(VkCommandBuffer cmd, CVulkanComputeMaterial* material,
			CVulkanHardwareBuffer* src, CVulkanTexture* dst, const core::vector3d<u32>& groupCount)
		{
			if (!material || !src || !dst)
			{
				logCompute("dispatchToTexture: no compute material, source buffer or destination texture",
					nullptr, ELL_ERROR);
				return false;
			}

			u32 srcBinding = 0, secondBinding = 0, imageBinding = 0;
			findOrdinalBindings(material, srcBinding, secondBinding, imageBinding);
			if (srcBinding == VulkanComputeMaxBindings || imageBinding == VulkanComputeMaxBindings)
			{
				logCompute("dispatchToTexture: the shader must declare a storage buffer (source) and a "
					"storage image (destination): ", material->Name.c_str(), ELL_ERROR);
				return false;
			}

			SVulkanComputeResources resources;
			resources.setBuffer(srcBinding, src);
			resources.setTexture(imageBinding, dst);
			return dispatchBound(cmd, material, resources, groupCount);
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
			// Every consumer at once: another dispatch or a draw reading the result, an indirect
			// draw/dispatch reading its arguments from it, a vertex fetch, and the read-back copy
			// the download path records.
			bufferBarrier(cmd, dst, 0, VK_WHOLE_SIZE,
				VK_ACCESS_SHADER_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT |
				VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
				VK_ACCESS_INDEX_READ_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
				VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
				VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
		}

		void CVulkanCompute::imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
			u32 mipLevels, u32 layerCount, VkImageLayout oldLayout, VkImageLayout newLayout,
			VkAccessFlags srcAccess, VkAccessFlags dstAccess,
			VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
		{
			if (!cmd || image == VK_NULL_HANDLE || !vk::CmdPipelineBarrier)
				return;

			// Whole image: every mip and every layer, the granularity CVulkanTexture tracks.
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
			barrier.subresourceRange.layerCount = layerCount ? layerCount : 1;

			vk::CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}

		// The source side is deliberately broad: the texture may have been sampled by a draw, written
		// by a previous dispatch, filled by a copy, or never touched (UNDEFINED); every dispatch here
		// is submitted on its own and waited on, so the coarse masks cost nothing measurable.
		void CVulkanCompute::barrierImageToStorage(VkCommandBuffer cmd, CVulkanTexture* texture)
		{
			if (!texture || !texture->hasDeviceResource())
				return;

			const VkImageLayout current = texture->getImageLayout();
			if (current == VK_IMAGE_LAYOUT_GENERAL)
				return;

			imageBarrier(cmd, texture->getImage(), texture->getAspectMask(), texture->getMipLevelCount(),
				texture->getLayerCount(), current, VK_IMAGE_LAYOUT_GENERAL,
				VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

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
				texture->getLayerCount(), current, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

			texture->setImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
