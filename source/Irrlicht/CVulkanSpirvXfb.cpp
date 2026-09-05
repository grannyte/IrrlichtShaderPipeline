// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanSpirvXfb.h"

#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "IVertexDescriptor.h"
#include <map>
#include <string>
#include <string.h>

namespace irr
{
	namespace video
	{
		namespace
		{
			// The handful of SPIR-V opcodes and enumerants this needs, by value: no dependency on a
			// SPIR-V header, the numbers are fixed by the specification.
			enum
			{
				OpSourceContinued = 2, OpSource = 3, OpSourceExtension = 4, OpName = 5, OpMemberName = 6,
				OpString = 7, OpLine = 8, OpExtension = 10, OpExtInstImport = 11, OpMemoryModel = 14,
				OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17,
				OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24,
				OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32,
				OpConstant = 43, OpVariable = 59,
				OpDecorate = 71, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74,
				OpGroupMemberDecorate = 75, OpNoLine = 317, OpModuleProcessed = 330,
				OpExecutionModeId = 331, OpDecorateId = 332,
				OpDecorateString = 5632, OpMemberDecorateString = 5633
			};
			enum
			{
				DecorationBuiltIn = 11, DecorationLocation = 30, DecorationOffset = 35,
				DecorationXfbBuffer = 36, DecorationXfbStride = 37, DecorationUserSemantic = 5635
			};
			enum { BuiltInPosition = 0, StorageClassOutput = 3, CapabilityTransformFeedback = 53, ExecutionModeXfb = 11 };

			bool isAnnotation(u32 op)
			{
				return op == OpDecorate || op == OpMemberDecorate || op == OpDecorationGroup ||
					op == OpGroupDecorate || op == OpGroupMemberDecorate || op == OpDecorateId ||
					op == OpDecorateString || op == OpMemberDecorateString;
			}
			bool isDebug(u32 op)
			{
				return op == OpSourceContinued || op == OpSource || op == OpSourceExtension || op == OpName ||
					op == OpMemberName || op == OpString || op == OpLine || op == OpNoLine || op == OpModuleProcessed;
			}
			bool isPreamble(u32 op)
			{
				return op == OpExtension || op == OpExtInstImport || op == OpMemoryModel || op == OpEntryPoint ||
					op == OpExecutionMode || op == OpExecutionModeId || op == OpCapability;
			}

			//! A SPIR-V string literal: UTF-8 packed little-endian, four bytes a word, null-terminated.
			std::string readString(const std::vector<u32>& words, size_t first, size_t end)
			{
				std::string out;
				for (size_t w = first; w < end; ++w)
				{
					for (u32 b = 0; b < 4; ++b)
					{
						const char c = static_cast<char>((words[w] >> (8 * b)) & 0xff);
						if (!c)
							return out;
						out += c;
					}
				}
				return out;
			}

			struct SType
			{
				u32 Op = 0;
				u32 Width = 0;       //!< bits, scalars
				u32 Count = 0;       //!< components / columns / array length
				u32 ElementType = 0; //!< vector component, matrix column, array element
				std::vector<u32> Members;
			};

			struct SOutput
			{
				u32 Variable = 0;
				u32 TypeId = 0;       //!< pointee type
				s32 Member = -1;      //!< -1: the whole variable; else this member of a block struct
				std::string Semantic; //!< upper-case, no SV_ prefix, no index
				u32 Index = 0;
				bool HasSemantic = false;
				bool HasLocation = false;
				u32 Location = 0;
				bool Used = false;
			};

			//! "TEXCOORD1" -> ("TEXCOORD", 1); "SV_Position" -> ("POSITION", 0); "COLOR" -> ("COLOR", 0).
			void splitSemantic(std::string name, std::string& outName, u32& outIndex)
			{
				for (size_t i = 0; i < name.size(); ++i)
					if (name[i] >= 'a' && name[i] <= 'z')
						name[i] = static_cast<char>(name[i] - 'a' + 'A');
				if (name.compare(0, 3, "SV_") == 0)
					name = name.substr(3);
				size_t digits = 0;
				while (digits < name.size() && name[name.size() - 1 - digits] >= '0' && name[name.size() - 1 - digits] <= '9')
					++digits;
				outIndex = 0;
				if (digits && digits < name.size())
				{
					outIndex = static_cast<u32>(atoi(name.c_str() + name.size() - digits));
					name = name.substr(0, name.size() - digits);
				}
				outName = name;
			}

			//! The D3D semantic name of an attribute, the table CD3D11VertexDescriptor::getSemanticName uses.
			const c8* semanticName(E_VERTEX_ATTRIBUTE_SEMANTIC semantic)
			{
				switch (semantic)
				{
				case EVAS_POSITION: return "POSITION";
				case EVAS_NORMAL: return "NORMAL";
				case EVAS_COLOR: return "COLOR";
				case EVAS_TANGENT: return "TANGENT";
				case EVAS_BINORMAL: return "BINORMAL";
				case EVAS_BLEND_WEIGHTS: return "BLENDWEIGHT";
				case EVAS_BLEND_INDICES: return "BLENDINDICES";
				case EVAS_CUSTOM: return "CUSTOM";
				default:
					if (semantic >= EVAS_TEXCOORD0 && semantic <= EVAS_TEXCOORD15)
						return "TEXCOORD";
					return "POSITION";
				}
			}

			u32 typeBytes(const std::map<u32, SType>& types, const std::map<u32, u32>& constants, u32 id)
			{
				std::map<u32, SType>::const_iterator it = types.find(id);
				if (it == types.end())
					return 0;
				const SType& t = it->second;
				switch (t.Op)
				{
				case OpTypeBool: return 4;
				case OpTypeInt:
				case OpTypeFloat: return t.Width / 8;
				case OpTypeVector:
				case OpTypeMatrix: return typeBytes(types, constants, t.ElementType) * t.Count;
				case OpTypeArray:
				{
					std::map<u32, u32>::const_iterator length = constants.find(t.Count);
					return typeBytes(types, constants, t.ElementType) * (length != constants.end() ? length->second : 1);
				}
				case OpTypeStruct:
				{
					u32 total = 0;
					for (size_t i = 0; i < t.Members.size(); ++i)
						total += typeBytes(types, constants, t.Members[i]);
					return total;
				}
				default: return 0;
				}
			}

			void emit(std::vector<u32>& out, u32 op, const std::vector<u32>& operands)
			{
				out.push_back(((u32)(operands.size() + 1) << 16) | op);
				out.insert(out.end(), operands.begin(), operands.end());
			}
		}

		bool decorateSpirvForStreamOutput(std::vector<u32>& spirv, const IVertexDescriptor* layout,
			core::stringc& outError, core::stringc* outWarnings)
		{
			outError = "";
			if (!layout || layout->getAttributeCount() == 0)
			{
				outError = "stream output: the vertex descriptor has no attribute";
				return false;
			}
			if (spirv.size() < 5 || spirv[0] != 0x07230203u)
			{
				outError = "stream output: not a SPIR-V module";
				return false;
			}

			// ---- pass 1: read what the module declares ----
			std::map<u32, SType> types;
			std::map<u32, u32> constants;
			std::map<u32, std::pair<u32, u32> > pointers; // id -> (storage class, pointee)
			std::map<u32, std::string> names;
			std::map<u32, std::string> userSemantics;
			std::map<std::pair<u32, u32>, std::string> memberUserSemantics;
			std::map<u32, u32> locations;
			std::map<u32, u32> builtIns;
			std::map<std::pair<u32, u32>, u32> memberBuiltIns;
			std::vector<std::pair<u32, u32> > outputVariables; // (id, pointer type id)
			u32 entryPoint = 0;
			bool alreadyDecorated = false;

			for (size_t pos = 5; pos < spirv.size();)
			{
				const u32 word = spirv[pos];
				const u32 op = word & 0xffffu;
				const u32 len = word >> 16;
				if (len == 0 || pos + len > spirv.size())
				{
					outError = "stream output: malformed SPIR-V instruction stream";
					return false;
				}
				const size_t end = pos + len;
				switch (op)
				{
				case OpEntryPoint:
					if (!entryPoint && len >= 3)
						entryPoint = spirv[pos + 2];
					break;
				case OpName:
					if (len >= 3)
						names[spirv[pos + 1]] = readString(spirv, pos + 2, end);
					break;
				case OpDecorateString:
					if (len >= 4 && spirv[pos + 2] == DecorationUserSemantic)
						userSemantics[spirv[pos + 1]] = readString(spirv, pos + 3, end);
					break;
				case OpMemberDecorateString:
					if (len >= 5 && spirv[pos + 3] == DecorationUserSemantic)
						memberUserSemantics[std::make_pair(spirv[pos + 1], spirv[pos + 2])] = readString(spirv, pos + 4, end);
					break;
				case OpDecorate:
					if (len >= 3)
					{
						const u32 decoration = spirv[pos + 2];
						if (decoration == DecorationLocation && len >= 4)
							locations[spirv[pos + 1]] = spirv[pos + 3];
						else if (decoration == DecorationBuiltIn && len >= 4)
							builtIns[spirv[pos + 1]] = spirv[pos + 3];
						else if (decoration == DecorationXfbBuffer || decoration == DecorationXfbStride)
							alreadyDecorated = true;
					}
					break;
				case OpMemberDecorate:
					if (len >= 4)
					{
						const u32 decoration = spirv[pos + 3];
						if (decoration == DecorationBuiltIn && len >= 5)
							memberBuiltIns[std::make_pair(spirv[pos + 1], spirv[pos + 2])] = spirv[pos + 4];
						else if (decoration == DecorationXfbBuffer || decoration == DecorationXfbStride)
							alreadyDecorated = true;
					}
					break;
				case OpTypeBool:
				case OpTypeInt:
				case OpTypeFloat:
				case OpTypeVector:
				case OpTypeMatrix:
				case OpTypeArray:
				case OpTypeStruct:
				{
					SType t;
					t.Op = op;
					if ((op == OpTypeInt || op == OpTypeFloat) && len >= 3)
						t.Width = spirv[pos + 2];
					if ((op == OpTypeVector || op == OpTypeMatrix || op == OpTypeArray) && len >= 4)
					{
						t.ElementType = spirv[pos + 2];
						t.Count = spirv[pos + 3];
					}
					if (op == OpTypeStruct)
						for (size_t m = pos + 2; m < end; ++m)
							t.Members.push_back(spirv[m]);
					types[spirv[pos + 1]] = t;
					break;
				}
				case OpTypePointer:
					if (len >= 4)
						pointers[spirv[pos + 1]] = std::make_pair(spirv[pos + 2], spirv[pos + 3]);
					break;
				case OpConstant:
					if (len >= 4)
						constants[spirv[pos + 2]] = spirv[pos + 3];
					break;
				case OpVariable:
					if (len >= 4 && spirv[pos + 3] == StorageClassOutput)
						outputVariables.push_back(std::make_pair(spirv[pos + 2], spirv[pos + 1]));
					break;
				default:
					break;
				}
				pos = end;
			}

			if (alreadyDecorated)
				return true; // the author laid the capture out in the shader itself
			if (!entryPoint)
			{
				outError = "stream output: the module has no entry point";
				return false;
			}

			// ---- the candidates: every output variable, block members of built-in blocks apart ----
			std::vector<SOutput> outputs;
			for (size_t v = 0; v < outputVariables.size(); ++v)
			{
				const u32 id = outputVariables[v].first;
				std::map<u32, std::pair<u32, u32> >::const_iterator ptr = pointers.find(outputVariables[v].second);
				if (ptr == pointers.end())
					continue;
				const u32 pointee = ptr->second.second;
				std::map<u32, SType>::const_iterator type = types.find(pointee);

				// A glslang gl_PerVertex block: its members are the candidates, Position first of all.
				bool isBuiltInBlock = false;
				if (type != types.end() && type->second.Op == OpTypeStruct)
				{
					for (size_t m = 0; m < type->second.Members.size() && !isBuiltInBlock; ++m)
						isBuiltInBlock = memberBuiltIns.count(std::make_pair(pointee, (u32)m)) != 0;
				}
				if (isBuiltInBlock)
				{
					for (size_t m = 0; m < type->second.Members.size(); ++m)
					{
						std::map<std::pair<u32, u32>, u32>::const_iterator builtIn =
							memberBuiltIns.find(std::make_pair(pointee, (u32)m));
						if (builtIn == memberBuiltIns.end() || builtIn->second != BuiltInPosition)
							continue;
						SOutput o;
						o.Variable = id;
						o.TypeId = type->second.Members[m];
						o.Member = (s32)m;
						o.Semantic = "POSITION";
						o.HasSemantic = true;
						outputs.push_back(o);
					}
					continue;
				}

				SOutput o;
				o.Variable = id;
				o.TypeId = pointee;
				std::map<u32, std::string>::const_iterator semantic = userSemantics.find(id);
				std::map<u32, u32>::const_iterator builtIn = builtIns.find(id);
				std::map<u32, std::string>::const_iterator name = names.find(id);
				if (semantic != userSemantics.end())
				{
					splitSemantic(semantic->second, o.Semantic, o.Index);
					o.HasSemantic = true;
				}
				else if (builtIn != builtIns.end())
				{
					if (builtIn->second != BuiltInPosition)
						continue; // PointSize, ClipDistance...: nothing the descriptor could name
					o.Semantic = "POSITION";
					o.HasSemantic = true;
				}
				else if (name != names.end())
				{
					std::string n = name->second;
					if (n.compare(0, 8, "out.var.") == 0)
						n = n.substr(8);
					else if (n.compare(0, 4, "out.") == 0)
						n = n.substr(4);
					if (n == "gl_Position")
						n = "POSITION";
					splitSemantic(n, o.Semantic, o.Index);
					// A GLSL name ("outColor") is not a semantic; only keep names from the table.
					static const c8* const known[] = { "POSITION", "NORMAL", "COLOR", "TEXCOORD", "TANGENT",
						"BINORMAL", "BLENDWEIGHT", "BLENDINDICES", "CUSTOM" };
					for (size_t k = 0; k < sizeof(known) / sizeof(known[0]) && !o.HasSemantic; ++k)
						o.HasSemantic = (o.Semantic == known[k]);
				}
				std::map<u32, u32>::const_iterator location = locations.find(id);
				if (location != locations.end())
				{
					o.HasLocation = true;
					o.Location = location->second;
				}
				outputs.push_back(o);
			}

			// ---- match the descriptor's attributes, the way CD3D11VertexDescriptor::rebuildOutput counts them ----
			struct SMatch { u32 Buffer, Stride, Offset, Bytes; SOutput* Output; };
			std::vector<SMatch> matches;
			std::vector<u32> fallback; // attribute indices that matched nothing by name
			u32 semanticCounter[EVAS_COUNT] = {};
			core::stringc warnings;

			for (u32 a = 0; a < layout->getAttributeCount(); ++a)
			{
				IVertexAttribute* attribute = layout->getAttribute(a);
				if (!attribute)
					continue;
				const E_VERTEX_ATTRIBUTE_SEMANTIC semantic = attribute->getSemantic();
				const u32 counterSlot = (semantic >= EVAS_TEXCOORD0 && semantic <= EVAS_TEXCOORD15) ? EVAS_TEXCOORD0 : semantic;
				const u32 index = semanticCounter[counterSlot < EVAS_COUNT ? counterSlot : 0]++;
				const std::string wanted = semanticName(semantic);

				SMatch match;
				match.Buffer = attribute->getBufferID();
				match.Stride = layout->getVertexSize(match.Buffer);
				match.Offset = attribute->getOffset();
				match.Bytes = attribute->getTypeSize() * attribute->getElementCount();
				match.Output = 0;
				for (size_t o = 0; o < outputs.size() && !match.Output; ++o)
					if (!outputs[o].Used && outputs[o].HasSemantic && outputs[o].Semantic == wanted && outputs[o].Index == index)
						match.Output = &outputs[o];
				if (match.Output)
					match.Output->Used = true;
				else
					fallback.push_back((u32)matches.size());
				matches.push_back(match);
			}

			// Whatever found no semantic takes the remaining outputs in location order.
			if (!fallback.empty())
			{
				std::vector<SOutput*> remaining;
				for (size_t o = 0; o < outputs.size(); ++o)
					if (!outputs[o].Used)
						remaining.push_back(&outputs[o]);
				for (size_t i = 0; i < remaining.size(); ++i)
					for (size_t j = i + 1; j < remaining.size(); ++j)
						if (remaining[j]->Location < remaining[i]->Location)
							std::swap(remaining[i], remaining[j]);
				for (size_t f = 0; f < fallback.size(); ++f)
				{
					if (f >= remaining.size())
					{
						IVertexAttribute* attribute = layout->getAttribute(fallback[f]);
						outError = "stream output: no output of the geometry stage matches attribute '";
						outError += attribute ? attribute->getName().c_str() : "?";
						outError += "' (";
						outError += attribute ? semanticName(attribute->getSemantic()) : "?";
						outError += ") and no unmatched output is left";
						return false;
					}
					matches[fallback[f]].Output = remaining[f];
					remaining[f]->Used = true;
					IVertexAttribute* attribute = layout->getAttribute(fallback[f]);
					warnings += "attribute '";
					warnings += attribute ? attribute->getName().c_str() : "?";
					warnings += "' matched an output by location order, not by semantic; ";
				}
			}

			// ---- pass 2: rebuild with the decorations ----
			std::vector<u32> decorations;
			for (size_t m = 0; m < matches.size(); ++m)
			{
				const SMatch& match = matches[m];
				const u32 variableBytes = typeBytes(types, constants, match.Output->TypeId);
				if (variableBytes && variableBytes != match.Bytes)
				{
					warnings += "output for attribute ";
					warnings += (s32)m;
					warnings += " is ";
					warnings += (s32)variableBytes;
					warnings += " bytes but the descriptor declares ";
					warnings += (s32)match.Bytes;
					warnings += " (the whole variable is captured); ";
				}
				std::vector<u32> operands;
				operands.push_back(match.Output->Variable); operands.push_back(DecorationXfbBuffer); operands.push_back(match.Buffer);
				emit(decorations, OpDecorate, operands);
				operands.clear();
				operands.push_back(match.Output->Variable); operands.push_back(DecorationXfbStride); operands.push_back(match.Stride);
				emit(decorations, OpDecorate, operands);
				operands.clear();
				if (match.Output->Member < 0)
				{
					operands.push_back(match.Output->Variable); operands.push_back(DecorationOffset); operands.push_back(match.Offset);
					emit(decorations, OpDecorate, operands);
				}
				else
				{
					// The struct type, not the variable: the member of a block carries its Offset.
					std::map<u32, std::pair<u32, u32> >::const_iterator ptr = pointers.end();
					for (size_t v = 0; v < outputVariables.size() && ptr == pointers.end(); ++v)
						if (outputVariables[v].first == match.Output->Variable)
							ptr = pointers.find(outputVariables[v].second);
					if (ptr == pointers.end())
						continue;
					operands.push_back(ptr->second.second); operands.push_back((u32)match.Output->Member);
					operands.push_back(DecorationOffset); operands.push_back(match.Offset);
					emit(decorations, OpMemberDecorate, operands);
				}
			}

			std::vector<u32> out;
			out.reserve(spirv.size() + decorations.size() + 8);
			out.insert(out.end(), spirv.begin(), spirv.begin() + 5);

			bool sawCapability = false, insertedCapability = false;
			bool sawEntryPoint = false, insertedMode = false;
			bool insertedDecorations = false;
			for (size_t pos = 5; pos < spirv.size();)
			{
				const u32 word = spirv[pos];
				const u32 op = word & 0xffffu;
				const u32 len = word >> 16;
				const size_t end = pos + len;

				if (op == OpCapability)
					sawCapability = true;
				else if (sawCapability && !insertedCapability)
				{
					std::vector<u32> operands(1, (u32)CapabilityTransformFeedback);
					emit(out, OpCapability, operands);
					insertedCapability = true;
				}

				if (op == OpEntryPoint)
					sawEntryPoint = true;
				else if (sawEntryPoint && !insertedMode && op != OpExecutionMode && op != OpExecutionModeId)
				{
					std::vector<u32> operands;
					operands.push_back(entryPoint); operands.push_back(ExecutionModeXfb);
					emit(out, OpExecutionMode, operands);
					insertedMode = true;
				}

				if (!insertedDecorations && (isAnnotation(op) || (!isDebug(op) && !isPreamble(op))))
				{
					out.insert(out.end(), decorations.begin(), decorations.end());
					insertedDecorations = true;
				}

				// DXC's reflection extensions go: the semantics were read above, and a driver need
				// not know SPV_GOOGLE_hlsl_functionality1 to run the module.
				bool strip = (op == OpDecorateString || op == OpMemberDecorateString);
				if (op == OpExtension)
				{
					const std::string extension = readString(spirv, pos + 1, end);
					strip = (extension == "SPV_GOOGLE_hlsl_functionality1" || extension == "SPV_GOOGLE_user_type");
				}
				if (!strip)
					out.insert(out.end(), spirv.begin() + pos, spirv.begin() + end);
				pos = end;
			}

			if (!insertedCapability || !insertedMode || !insertedDecorations)
			{
				outError = "stream output: could not place the transform feedback declarations in the module";
				return false;
			}

			spirv.swap(out);
			if (outWarnings)
				*outWarnings = warnings;
			return true;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
