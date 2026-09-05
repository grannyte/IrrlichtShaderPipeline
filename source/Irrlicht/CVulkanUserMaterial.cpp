// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanUserMaterial.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_
#include "CVulkanShaderCompiler.h"
#include "CVulkanSpirvXfb.h"
#include "matrix4.h"
#include "os.h"
#include <algorithm>
#include <string.h>

namespace irr
{
	namespace video
	{
		namespace
		{
			// --- The slice of the SPIR-V binary format the reflector below needs ---
			// A module is a 5-word header (magic, version, generator, id bound, schema) followed by
			// instructions, each opening with a word packing (wordCount << 16) | opcode. Result ids
			// are dense and < the bound, so a flat array indexed by id serves as the symbol table.
			// Anything not listed here is skipped by its word count; the walk never has to
			// understand the instruction it is stepping over.
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

			//! Matrix majorness as SPIR-V decorates it, which is the MIRROR of the source language's:
			//! a SPIR-V matrix is a list of columns, an HLSL one a list of rows.
			enum { SpirvMajorUndecorated = 0, SpirvMajorCol = 1, SpirvMajorRow = 2 };

			const u32 SpirvNoValue = 0xFFFFFFFFu; //!< "this decoration was never seen"

			//! Refuses to allocate a symbol table for an id bound no real shader reaches, so a
			//! corrupt header cannot turn into a multi-gigabyte allocation.
			const u32 SpirvMaxIdBound = 4u * 1024u * 1024u;

			//! Everything the walk remembers about one SPIR-V id. Which fields carry meaning depends
			//! on Op: Word0/Word1 are the first two type operands (component type and count, element
			//! type and length, pointee type and storage class), Value is an OpConstant literal.
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

			//! An OpVariable, set aside until the walk is over: decorations and types are emitted in
			//! their own section, so nothing about a variable is complete while the walk is running.
			struct SSpirvVariable
			{
				u32 TypeId = 0;
				u32 ResultId = 0;
				u32 StorageClass = 0;
			};

			//! The four per-member vectors always grow together, so one index is valid in all of them.
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

			//! SPIR-V literal string: packed four bytes per word, low byte first, NUL-terminated and
			//! padded to a word boundary.
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

			//! Size in bytes a type occupies inside a std140/std430 block. `matrixStride` and `major`
			//! are the decorations of the MEMBER being measured, which is where SPIR-V puts them.
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
					// RowMajor storage strides over the rows, ColMajor over the columns; for the
					// square matrices the engine passes the two counts agree anyway.
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
					// No cumulative size is recorded anywhere in SPIR-V: it is the highest member end.
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

			//! The one shape core::matrix4 knows how to transpose: a 32-bit float 4x4 laid out the
			//! way DXC lays out HLSL's default column_major. See SVulkanUserShaderVariable.
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

			//! Per-stage constants: the E_SHADER_TYPE the compiler front end wants, the bit a
			//! descriptor binding records, and the word used in log messages.
			struct SVulkanUserStageInfo
			{
				E_SHADER_TYPE ShaderType;
				VkShaderStageFlagBits Bit;
				const c8* Name;
			};

			const SVulkanUserStageInfo StageInfos[EVUS_COUNT] =
			{
				{ EST_VERTEX_SHADER, VK_SHADER_STAGE_VERTEX_BIT, "vertex" },
				{ EST_PIXEL_SHADER, VK_SHADER_STAGE_FRAGMENT_BIT, "fragment" },
				{ EST_GEOMETRY_SHADER, VK_SHADER_STAGE_GEOMETRY_BIT, "geometry" },
				{ EST_HULL_SHADER, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, "hull" },
				{ EST_DOMAIN_SHADER, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, "domain" },
			};

			//! "CVulkanUserMaterial: <stage> shader: <what><detail>", the shape of every log below.
			void logStage(const c8* stageName, const c8* what, const c8* detail, ELOG_LEVEL level)
			{
				core::stringc message = "CVulkanUserMaterial: ";
				message += stageName;
				message += " shader: ";
				message += what;
				if (detail)
					message += detail;
				os::Printer::log(message.c_str(), level);
			}

			bool hasStageSource(const SVulkanUserShaderStageSource& source)
			{
				// A pre-compiled SPIR-V blob starts with a zero byte often enough that testing
				// Source[0] alone would reject it; a non-zero length settles it.
				return source.Source && (source.SourceLength > 0 || source.Source[0] != 0);
			}
		}

		//! The modules are this object's own, unlike the built-in renderers' borrowed ones, so they die
		//! with it -- which means the driver must drop() its user materials before the device goes.
		CVulkanUserMaterial::~CVulkanUserMaterial()
		{
			if (Device && vk::DestroyShaderModule)
			{
				for (u32 stage = 0; stage < EVUS_COUNT; ++stage)
					if (Modules[stage] != VK_NULL_HANDLE)
						vk::DestroyShaderModule(Device, Modules[stage], nullptr);
			}

			for (u32 stage = 0; stage < EVUS_COUNT; ++stage)
				Modules[stage] = VK_NULL_HANDLE;

			if (CallBack)
			{
				CallBack->drop();
				CallBack = nullptr;
			}
			if (StreamOutputLayout)
			{
				StreamOutputLayout->drop();
				StreamOutputLayout = nullptr;
			}
		}

		const c8* CVulkanUserMaterial::getEntryPoint(E_VULKAN_USER_STAGE stage) const
		{
			if ((u32)stage >= (u32)EVUS_COUNT || EntryPoints[stage].size() == 0)
				return "main";
			return EntryPoints[stage].c_str();
		}

		bool CVulkanUserMaterial::stageFromShaderType(E_SHADER_TYPE type, E_VULKAN_USER_STAGE& outStage)
		{
			switch (type)
			{
			case EST_VERTEX_SHADER:   outStage = EVUS_VERTEX; return true;
			case EST_PIXEL_SHADER:    outStage = EVUS_FRAGMENT; return true;
			case EST_GEOMETRY_SHADER: outStage = EVUS_GEOMETRY; return true;
			case EST_STREAM_OUTPUT_SHADER: outStage = EVUS_GEOMETRY; return true;
			case EST_HULL_SHADER:     outStage = EVUS_HULL; return true;
			case EST_DOMAIN_SHADER:   outStage = EVUS_DOMAIN; return true;
			default:                  return false;
			}
		}

		std::vector<SVulkanUserUniformBlock>* CVulkanUserMaterial::getStageBlocks(E_VULKAN_USER_STAGE stage)
		{
			// const_cast on the const overload rather than a second bounds check -- the object is
			// non-const here by construction, so this hands back a legitimately mutable reference.
			return const_cast<std::vector<SVulkanUserUniformBlock>*>(
				static_cast<const CVulkanUserMaterial*>(this)->getStageBlocks(stage));
		}

		const std::vector<SVulkanUserUniformBlock>* CVulkanUserMaterial::getStageBlocks(E_VULKAN_USER_STAGE stage) const
		{
			if ((u32)stage >= (u32)EVUS_COUNT)
				return nullptr;
			return &Blocks[stage];
		}

		void CVulkanUserMaterial::getDescriptorSetLayoutBindings(u32 set,
			std::vector<VkDescriptorSetLayoutBinding>& outBindings) const
		{
			outBindings.clear();

			for (size_t i = 0; i < DescriptorBindings.size(); ++i)
			{
				const SVulkanUserDescriptorBinding& source = DescriptorBindings[i];
				if (source.Set != set)
					continue;

				VkDescriptorSetLayoutBinding binding = {};
				binding.binding = source.Binding;
				binding.descriptorType = source.Type;
				binding.descriptorCount = source.Count;
				binding.stageFlags = source.StageFlags;
				outBindings.push_back(binding);
			}
		}

		void CVulkanUserMaterial::addDescriptorBinding(u32 set, u32 binding, VkDescriptorType type,
			u32 count, VkShaderStageFlags stageBit, const core::stringc& name)
		{
			for (size_t i = 0; i < DescriptorBindings.size(); ++i)
			{
				SVulkanUserDescriptorBinding& existing = DescriptorBindings[i];
				if (existing.Set != set || existing.Binding != binding)
					continue;

				// Same slot from another stage: one Vulkan descriptor serves them all, so the entry
				// is shared and only its visibility grows.
				if (existing.Type != type)
					os::Printer::log("CVulkanUserMaterial: two stages declare the same (set, binding) "
						"as different descriptor types, keeping the first: ", name.c_str(), ELL_WARNING);

				existing.StageFlags |= stageBit;
				if (count > existing.Count)
					existing.Count = count;
				return;
			}

			SVulkanUserDescriptorBinding entry;
			entry.Set = set;
			entry.Binding = binding;
			entry.Type = type;
			entry.Count = count;
			entry.StageFlags = stageBit;
			entry.Name = name;
			DescriptorBindings.push_back(entry);
			UsedSetMask |= (1u << set);
		}

		bool CVulkanUserMaterial::reflectSpirv(const std::vector<u32>& spirv, E_VULKAN_USER_STAGE stage,
			VkShaderStageFlagBits stageBit, const c8* stageName)
		{
			if (spirv.size() < SpirvHeaderSize / sizeof(u32) || spirv[0] != SpirvMagicWord)
			{
				// A byte-swapped magic word lands here too: this reflector reads host-order words
				// only, which is what both front ends and every EGSL_PCMP blob produce.
				logStage(stageName, "not a host-order SPIR-V module, cannot be reflected", nullptr, ELL_ERROR);
				return false;
			}

			const u32 bound = spirv[3];
			if (bound == 0 || bound > SpirvMaxIdBound)
			{
				logStage(stageName, "implausible SPIR-V id bound, module rejected", nullptr, ELL_ERROR);
				return false;
			}

			std::vector<SSpirvId> ids((size_t)bound);
			std::vector<SSpirvVariable> variables;
			// Pass one: fill the symbol table. Every instruction below writes into it and nothing is
			// interpreted yet, so the order the module lists names, decorations and types in is free.
			size_t offset = SpirvHeaderSize / sizeof(u32);

			while (offset < spirv.size())
			{
				const u32* words = &spirv[offset];
				const u32 wordCount = words[0] >> 16;
				const u16 opcode = (u16)(words[0] & 0xFFFFu);

				if (wordCount == 0 || offset + wordCount > spirv.size())
				{
					logStage(stageName, "truncated SPIR-V instruction stream", nullptr, ELL_ERROR);
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
					// Only the low word is kept: array lengths are the sole use, and none overflow it.
					// Note the result id sits in words[2] here, after the result TYPE -- as in
					// OpVariable below, and unlike every OpType* above.
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

			// Pass two: every variable is now complete -- its pointer type, its decorations and the
			// names of the struct it points at have all been seen, wherever they appeared.
			for (size_t v = 0; v < variables.size(); ++v)
			{
				const SSpirvVariable& variable = variables[v];
				if (variable.TypeId >= ids.size() || ids[variable.TypeId].Op != SpvOpTypePointer)
					continue;

				// An array of descriptors is declared as an array of the resource type; peel it off,
				// keeping its length as the descriptor count.
				u32 typeId = ids[variable.TypeId].Word0;
				u32 descriptorCount = 1;

				while (typeId < ids.size() && ids[typeId].Op == SpvOpTypeArray)
				{
					const u32 lengthId = ids[typeId].Word1;
					const u32 length = (lengthId < ids.size() && ids[lengthId].Op == SpvOpConstant) ?
						ids[lengthId].Value : 1;
					descriptorCount *= length ? length : 1;
					typeId = ids[typeId].Word0;
				}

				if (typeId >= ids.size())
					continue;

				const SSpirvId& type = ids[typeId];
				const SSpirvId& self = ids[variable.ResultId];
				// GLSL without an explicit "set = N" emits no DescriptorSet decoration and means 0.
				const u32 set = (self.Set == SpirvNoValue) ? 0 : self.Set;
				const u32 binding = self.Binding;

				if (variable.StorageClass == SpvStorageClassUniform ||
					variable.StorageClass == SpvStorageClassStorageBuffer)
				{
					if (type.Op != SpvOpTypeStruct)
						continue;

					if (variable.StorageClass == SpvStorageClassStorageBuffer || type.IsBufferBlock)
					{
						logStage(stageName, "storage buffers are not supported by this material, skipped: ",
							type.Name.c_str(), ELL_WARNING);
						continue;
					}

					if (!type.IsBlock)
						continue;

					if (binding == SpirvNoValue)
					{
						logStage(stageName, "uniform block without a binding decoration, skipped: ",
							type.Name.c_str(), ELL_WARNING);
						continue;
					}

					if (set >= MaxUserDescriptorSets)
					{
						// Sets 0..MaxUserDescriptorSets-1 are the user range; the next one is the
						// driver's own uniforms, and sharing it would collide with them.
						logStage(stageName, "uniform block declared in a descriptor set outside the user "
							"range 0..3, which belongs to the driver: ", type.Name.c_str(), ELL_ERROR);
						return false;
					}

					if (descriptorCount != 1)
					{
						logStage(stageName, "arrays of uniform blocks are not supported, skipped: ",
							type.Name.c_str(), ELL_WARNING);
						continue;
					}

					// The block name is the struct's, not the variable's: that is the cbuffer name on
					// the HLSL side (DXC prefixes it with "type.") and the block name on the GLSL one.
					core::stringc blockName = type.Name;
					if (blockName.size() > 5 && blockName.subString(0, 5) == "type.")
						blockName = blockName.subString(5, (s32)blockName.size() - 5);
					if (blockName.size() == 0)
						blockName = self.Name;

					SVulkanUserUniformBlock block;
					block.Name = blockName;
					block.Set = set;
					block.Binding = binding;
					block.Size = spirvTypeSize(ids, typeId, 0, (u8)SpirvMajorUndecorated, 0);

					if (block.Size == 0)
					{
						logStage(stageName, "uniform block with no laid-out member, skipped: ",
							blockName.c_str(), ELL_WARNING);
						continue;
					}

					block.Scratch.assign(block.Size, 0);
					Blocks[stage].push_back(block);
					const s32 blockIndex = (s32)(Blocks[stage].size() - 1);

					for (size_t m = 0; m < type.MemberTypes.size(); ++m)
					{
						const u32 memberOffset = (m < type.MemberOffsets.size()) ? type.MemberOffsets[m] : SpirvNoValue;
						if (memberOffset == SpirvNoValue)
							continue;

						SVulkanUserShaderVariable member;
						member.Name = (m < type.MemberNames.size()) ? type.MemberNames[m] : core::stringc();
						if (member.Name.size() == 0)
							continue; // a module stripped of its debug names has nothing to look up

						const u32 memberStride = (m < type.MemberMatrixStrides.size()) ? type.MemberMatrixStrides[m] : 0;
						const u8 memberMajor = (m < type.MemberMajor.size()) ? type.MemberMajor[m] : (u8)SpirvMajorUndecorated;
						member.Block = blockIndex;
						member.Offset = memberOffset;
						member.Size = spirvTypeSize(ids, type.MemberTypes[m], memberStride, memberMajor, 0);
						member.TransposeOnSet = spirvIsTransposableMatrix(ids, type.MemberTypes[m], memberMajor);
						Variables[stage].push_back(member);
					}

					addDescriptorBinding(set, binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stageBit, blockName);
				}
				else if (variable.StorageClass == SpvStorageClassUniformConstant)
				{
					if (type.Op != SpvOpTypeSampledImage)
					{
						if (type.Op == SpvOpTypeImage || type.Op == SpvOpTypeSampler)
							logStage(stageName, "separate images and samplers are not supported, only "
								"combined image samplers, skipped: ", self.Name.c_str(), ELL_WARNING);
						continue;
					}

					if (binding == SpirvNoValue)
					{
						logStage(stageName, "sampler without a binding decoration, skipped: ",
							self.Name.c_str(), ELL_WARNING);
						continue;
					}

					if (set >= MaxUserDescriptorSets)
					{
						logStage(stageName, "sampler declared in a descriptor set outside the user range "
							"0..3, which belongs to the driver: ", self.Name.c_str(), ELL_ERROR);
						return false;
					}

					addDescriptorBinding(set, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
						descriptorCount, stageBit, self.Name);
				}
				else if (variable.StorageClass == SpvStorageClassPushConstant)
				{
					logStage(stageName, "push constants are not exposed by the shader-constant API, "
						"ignored: ", self.Name.c_str(), ELL_WARNING);
				}
			}

			return true;
		}

		bool CVulkanUserMaterial::compileStage(const SVulkanContext& context, E_GPU_SHADING_LANGUAGE lang,
			const SVulkanUserShaderStageSource& source, E_VULKAN_USER_STAGE stage)
		{
			const SVulkanUserStageInfo& info = StageInfos[stage];
			const c8* entryPoint = (source.EntryPoint && source.EntryPoint[0]) ? source.EntryPoint : "main";

			std::vector<u32> spirv;
			core::stringc compileError;

			// The geometry stage of a stream-output material: compiled with its semantics kept, then
			// given the transform feedback layout the vertexTypeOut descriptor describes.
			const bool streamOutput = (stage == EVUS_GEOMETRY && StreamOutputLayout && context.HasTransformFeedback);

			if (!CVulkanShaderCompiler::compileToSpirv(source.Source, source.SourceLength, entryPoint,
				info.ShaderType, lang, spirv, compileError, 0, 0, streamOutput))
			{
				core::stringc message = "compilation failed (";
				message += CVulkanShaderCompiler::getLanguageName(lang);
				message += "): ";
				message += compileError;
				logStage(info.Name, message.c_str(), nullptr, ELL_ERROR);
				return false;
			}

			if (streamOutput)
			{
				core::stringc patchError, patchWarnings;
				if (!decorateSpirvForStreamOutput(spirv, StreamOutputLayout, patchError, &patchWarnings))
				{
					logStage(info.Name, patchError.c_str(), nullptr, ELL_ERROR);
					return false;
				}
				if (patchWarnings.size())
					logStage(info.Name, "stream output: ", patchWarnings.c_str(), ELL_WARNING);
				StreamOutput = true;
			}

			// Reflect before creating the module: a set outside the user range is a hard error, and
			// there is no point holding a VkShaderModule that can never get a pipeline layout.
			if (!reflectSpirv(spirv, stage, info.Bit, info.Name))
				return false;

			VkShaderModuleCreateInfo createInfo = {};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			createInfo.codeSize = spirv.size() * sizeof(u32); // bytes, not words
			createInfo.pCode = spirv.data();

			VkShaderModule module = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanUserMaterial: vkCreateShaderModule",
				vk::CreateShaderModule(context.Device, &createInfo, nullptr, &module)))
				return false;

			Modules[stage] = module;
			EntryPoints[stage] = entryPoint;
			return true;
		}

		bool CVulkanUserMaterial::compileFromSource(const SVulkanContext& context, E_GPU_SHADING_LANGUAGE lang,
			const SVulkanUserShaderStageSource* stages)
		{
			if (!stages)
			{
				os::Printer::log("CVulkanUserMaterial::compileFromSource: no stage array", ELL_ERROR);
				return false;
			}

			if (!context.Device || !vk::CreateShaderModule)
			{
				os::Printer::log("CVulkanUserMaterial::compileFromSource: no device, or "
					"vkCreateShaderModule was never resolved", ELL_ERROR);
				return false;
			}

			// Captured up front so the destructor can release whatever was created before a later
			// stage failed -- the caller only has to drop() the object.
			Device = context.Device;

			if (!CVulkanShaderCompiler::isLanguageSupported(lang))
			{
				os::Printer::log("CVulkanUserMaterial: this build cannot compile shaders written in ",
					CVulkanShaderCompiler::getLanguageName(lang), ELL_ERROR);
				return false;
			}

			if (!hasStageSource(stages[EVUS_VERTEX]) || !hasStageSource(stages[EVUS_FRAGMENT]))
			{
				os::Printer::log("CVulkanUserMaterial: a user material needs both a vertex and a "
					"fragment shader", ELL_ERROR);
				return false;
			}

			// Tessellation is a pair: a control shader without an evaluation shader (or the reverse)
			// cannot produce a pipeline, same rule as the D3D12 hull/domain pair.
			if (hasStageSource(stages[EVUS_HULL]) != hasStageSource(stages[EVUS_DOMAIN]))
			{
				os::Printer::log("CVulkanUserMaterial: hull and domain shaders must be supplied "
					"together or not at all", ELL_ERROR);
				return false;
			}

			for (u32 stage = 0; stage < EVUS_COUNT; ++stage)
			{
				if (!hasStageSource(stages[stage]))
					continue;
				if (!compileStage(context, lang, stages[stage], (E_VULKAN_USER_STAGE)stage))
					return false;
			}

			return true;
		}

		//! Names come from OpName/OpMemberName, so a module compiled with its debug information
		//! stripped reflects its bindings but nothing addressable by name -- as on the D3D12 side,
		//! where a stripped blob loses its cbuffer names too.
		s32 CVulkanUserMaterial::getConstantBufferID(const c8* name, E_SHADER_TYPE stage) const
		{
			E_VULKAN_USER_STAGE userStage = EVUS_VERTEX;
			if (!name || !stageFromShaderType(stage, userStage))
				return -1;

			for (size_t i = 0; i < Blocks[userStage].size(); ++i)
				if (Blocks[userStage][i].Name == name)
					return (s32)i;

			return -1;
		}

		bool CVulkanUserMaterial::setConstantBuffer(s32 id, const void* data, size_t dataSizeBytes, E_SHADER_TYPE stage)
		{
			E_VULKAN_USER_STAGE userStage = EVUS_VERTEX;
			if (!data || !stageFromShaderType(stage, userStage))
				return false;
			if (id < 0 || (size_t)id >= Blocks[userStage].size())
				return false;

			std::vector<u8>& scratch = Blocks[userStage][(size_t)id].Scratch;
			const size_t bytes = std::min<size_t>(dataSizeBytes, scratch.size());
			memcpy(scratch.data(), data, bytes);
			mirrorBlockWrite(userStage, (size_t)id, 0, scratch.data(), (u32)bytes);
			return true;
		}

		s32 CVulkanUserMaterial::getVariableID(const c8* name, E_SHADER_TYPE stage) const
		{
			E_VULKAN_USER_STAGE userStage = EVUS_VERTEX;
			if (!name || !stageFromShaderType(stage, userStage))
				return -1;

			for (size_t i = 0; i < Variables[userStage].size(); ++i)
				if (Variables[userStage][i].Name == name)
					return (s32)i;

			return -1;
		}

		void CVulkanUserMaterial::mirrorBlockWrite(E_VULKAN_USER_STAGE stage, size_t blockIndex, u32 offset,
			const void* data, u32 byteCount)
		{
			// Copied by (set, binding) rather than by name: that pair is what the descriptor actually
			// is, and two stages may well spell the same block differently.
			const u32 set = Blocks[stage][blockIndex].Set;
			const u32 binding = Blocks[stage][blockIndex].Binding;

			for (u32 other = 0; other < EVUS_COUNT; ++other)
			{
				if ((E_VULKAN_USER_STAGE)other == stage)
					continue;

				for (size_t b = 0; b < Blocks[other].size(); ++b)
				{
					SVulkanUserUniformBlock& target = Blocks[other][b];
					if (target.Set != set || target.Binding != binding || offset >= target.Scratch.size())
						continue;

					const size_t bytes = std::min<size_t>(byteCount, target.Scratch.size() - offset);
					memcpy(target.Scratch.data() + offset, data, bytes);
				}
			}
		}

		// The writers below truncate silently instead of overflowing when the shader declares a
		// variable smaller than what the caller supplies -- same defensive spirit as the rest of the
		// backend, and the same behaviour as the D3D12 one.

		bool CVulkanUserMaterial::setVariable(s32 id, const f32* floats, int count, E_SHADER_TYPE stage)
		{
			E_VULKAN_USER_STAGE userStage = EVUS_VERTEX;
			if (!floats || count <= 0 || !stageFromShaderType(stage, userStage))
				return false;
			if (id < 0 || (size_t)id >= Variables[userStage].size())
				return false;

			const SVulkanUserShaderVariable& variable = Variables[userStage][(size_t)id];
			if (variable.Block < 0 || (size_t)variable.Block >= Blocks[userStage].size())
				return false;

			std::vector<u8>& scratch = Blocks[userStage][(size_t)variable.Block].Scratch;
			if (variable.Offset >= scratch.size())
				return false;

			size_t bytes = (size_t)count * sizeof(f32);
			const size_t available = scratch.size() - variable.Offset;
			if (bytes > available)
				bytes = available;

			if (variable.TransposeOnSet && count >= 16 && bytes >= sizeof(f32) * 16)
			{
				// Transposed into a local copy, never in place: rewriting the caller's array would
				// silently corrupt the matrix it still holds.
				core::matrix4 source;
				memcpy(source.pointer(), floats, sizeof(f32) * 16);
				const core::matrix4 transposed = source.getTransposed();
				memcpy(scratch.data() + variable.Offset, transposed.pointer(), sizeof(f32) * 16);
				bytes = sizeof(f32) * 16;
			}
			else
				memcpy(scratch.data() + variable.Offset, floats, bytes);

			mirrorBlockWrite(userStage, (size_t)variable.Block, variable.Offset,
				scratch.data() + variable.Offset, (u32)bytes);
			return true;
		}

		bool CVulkanUserMaterial::setVariable(s32 id, const s32* ints, int count, E_SHADER_TYPE stage)
		{
			// Straight to the raw writer: an integer variable is never a matrix, so the transpose
			// branch above would only be a trap here.
			if (count <= 0)
				return false;
			return setVariableRaw(id, ints, (u32)((size_t)count * sizeof(s32)), stage);
		}

		bool CVulkanUserMaterial::setVariable(const c8* name, const f32* floats, int count, E_SHADER_TYPE stage)
		{
			return setVariable(getVariableID(name, stage), floats, count, stage);
		}

		bool CVulkanUserMaterial::setVariable(const c8* name, const s32* ints, int count, E_SHADER_TYPE stage)
		{
			return setVariable(getVariableID(name, stage), ints, count, stage);
		}

		bool CVulkanUserMaterial::setVariableRaw(s32 id, const void* data, u32 byteCount, E_SHADER_TYPE stage)
		{
			E_VULKAN_USER_STAGE userStage = EVUS_VERTEX;
			if (!data || byteCount == 0 || !stageFromShaderType(stage, userStage))
				return false;
			if (id < 0 || (size_t)id >= Variables[userStage].size())
				return false;

			const SVulkanUserShaderVariable& variable = Variables[userStage][(size_t)id];
			if (variable.Block < 0 || (size_t)variable.Block >= Blocks[userStage].size())
				return false;

			std::vector<u8>& scratch = Blocks[userStage][(size_t)variable.Block].Scratch;
			if (variable.Offset >= scratch.size())
				return false;

			const size_t bytes = std::min<size_t>(byteCount, scratch.size() - variable.Offset);
			memcpy(scratch.data() + variable.Offset, data, bytes);
			mirrorBlockWrite(userStage, (size_t)variable.Block, variable.Offset,
				scratch.data() + variable.Offset, (u32)bytes);
			return true;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
