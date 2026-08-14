// DDSTextureLoader12.cpp
// Chargement DDS -> ID3D12Resource. Header/pixel-format parsing porte
// quasi tel-quel depuis DDSTextureLoader.cpp (D3D11, Microsoft) -- BitsPerPixel(),
// GetSurfaceInfo() (variantes bloc-compresse/generique uniquement : GetDXGIFormat()
// ci-dessous ne produit jamais de format packed/planar YUV, donc ces branches de la
// version originale sont omises), GetDXGIFormat() sont copiees quasi verbatim. La
// difference reelle avec la version D3D11 est uniquement la creation/upload de la
// ressource : D3D12 n'a pas d'equivalent a "CreateTexture2D(desc, initialData, ...)",
// donc CreateD3DResources12() ci-dessous fait un CreateCommittedResource (heap DEFAULT,
// COPY_DEST) suivi d'un upload explicite par subresource (heap UPLOAD +
// CopyTextureRegion), meme mecanisme que CD3D12Texture::uploadArraySlices().
#include "DDSTextureLoader12.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include "CD3D12Driver.h"
#include <vector>
#include <algorithm>

namespace irr
{
	namespace video
	{
		typedef unsigned char byte;
		typedef irr::u32 uint32;
		typedef irr::u64 uint64;

		//--------------------------------------------------------------------------------------
#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                              \
                ((uint32)(byte)(ch0) | ((uint32)(byte)(ch1) << 8) |       \
                ((uint32)(byte)(ch2) << 16) | ((uint32)(byte)(ch3) << 24))
#endif

#pragma pack(push, 1)

#define DDS_MAGIC 0x20534444 // "DDS "

		struct DDS_PIXELFORMAT12
		{
			uint32  size;
			uint32  flags;
			uint32  fourCC;
			uint32  RGBBitCount;
			uint32  RBitMask;
			uint32  GBitMask;
			uint32  BBitMask;
			uint32  ABitMask;
		};

#define DDS_FOURCC      0x00000004  // DDPF_FOURCC
#define DDS_RGB         0x00000040  // DDPF_RGB
#define DDS_LUMINANCE   0x00020000  // DDPF_LUMINANCE
#define DDS_ALPHA       0x00000002  // DDPF_ALPHA

#define DDS_HEADER_FLAGS_VOLUME         0x00800000  // DDSD_DEPTH

#define DDS_CUBEMAP_POSITIVEX 0x00000600
#define DDS_CUBEMAP_NEGATIVEX 0x00000a00
#define DDS_CUBEMAP_POSITIVEY 0x00001200
#define DDS_CUBEMAP_NEGATIVEY 0x00002200
#define DDS_CUBEMAP_POSITIVEZ 0x00004200
#define DDS_CUBEMAP_NEGATIVEZ 0x00008200
#define DDS_CUBEMAP_ALLFACES (DDS_CUBEMAP_POSITIVEX | DDS_CUBEMAP_NEGATIVEX |\
                              DDS_CUBEMAP_POSITIVEY | DDS_CUBEMAP_NEGATIVEY |\
                              DDS_CUBEMAP_POSITIVEZ | DDS_CUBEMAP_NEGATIVEZ)
#define DDS_CUBEMAP 0x00000200

		struct DDS_HEADER12
		{
			uint32          size;
			uint32          flags;
			uint32          height;
			uint32          width;
			uint32          pitchOrLinearSize;
			uint32          depth; // seulement si DDS_HEADER_FLAGS_VOLUME
			uint32          mipMapCount;
			uint32          reserved1[11];
			DDS_PIXELFORMAT12 ddspf;
			uint32          caps;
			uint32          caps2;
			uint32          caps3;
			uint32          caps4;
			uint32          reserved2;
		};

		struct DDS_HEADER_DXT10_12
		{
			DXGI_FORMAT dxgiFormat;
			uint32      resourceDimension;
			uint32      miscFlag;
			uint32      arraySize;
			uint32      reserved;
		};

#pragma pack(pop)

		//--------------------------------------------------------------------------------------
		static size_t BitsPerPixel12(DXGI_FORMAT fmt)
		{
			switch (fmt)
			{
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
			case DXGI_FORMAT_R32G32B32A32_UINT:
			case DXGI_FORMAT_R32G32B32A32_TYPELESS:
				return 128;
			case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R16G16B16A16_SNORM:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R32G32_FLOAT:
				return 64;
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R11G11B10_FLOAT:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_R16G16_UNORM:
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
				return 32;
			case DXGI_FORMAT_R8G8_UNORM:
			case DXGI_FORMAT_R16_UNORM:
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_B5G6R5_UNORM:
			case DXGI_FORMAT_B5G5R5A1_UNORM:
			case DXGI_FORMAT_B4G4R4A4_UNORM:
				return 16;
			case DXGI_FORMAT_R8_UNORM:
			case DXGI_FORMAT_A8_UNORM:
				return 8;
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC4_SNORM:
				return 4;
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC5_SNORM:
			case DXGI_FORMAT_BC6H_UF16:
			case DXGI_FORMAT_BC6H_SF16:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				return 8;
			default:
				return 0;
			}
		}

		//--------------------------------------------------------------------------------------
		// numBytes/rowBytes/numRows d'UNE subresource (un mip, une tranche Z pour un
		// volume) telle que rangee dans le fichier .dds source -- toujours tassee sans
		// alignement (contrairement au footprint destination D3D12, aligne sur 256
		// octets/ligne, calcule separement via GetCopyableFootprints()).
		static bool GetSurfaceInfo12(size_t width, size_t height, DXGI_FORMAT fmt,
			size_t& outNumBytes, size_t& outRowBytes, size_t& outNumRows)
		{
			bool bc = false;
			size_t bpe = 0;
			switch (fmt)
			{
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC4_SNORM:
				bc = true; bpe = 8;
				break;
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC5_SNORM:
			case DXGI_FORMAT_BC6H_UF16:
			case DXGI_FORMAT_BC6H_SF16:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				bc = true; bpe = 16;
				break;
			default:
				break;
			}

			if (bc)
			{
				const uint64 numBlocksWide = width ? std::max<uint64>(1u, (uint64(width) + 3u) / 4u) : 0;
				const uint64 numBlocksHigh = height ? std::max<uint64>(1u, (uint64(height) + 3u) / 4u) : 0;
				outRowBytes = static_cast<size_t>(numBlocksWide * bpe);
				outNumRows = static_cast<size_t>(numBlocksHigh);
				outNumBytes = outRowBytes * outNumRows;
				return true;
			}

			const size_t bpp = BitsPerPixel12(fmt);
			if (!bpp)
				return false;

			outRowBytes = (width * bpp + 7u) / 8u;
			outNumRows = height;
			outNumBytes = outRowBytes * height;
			return true;
		}

		//--------------------------------------------------------------------------------------
#define ISBITMASK12(r, g, b, a) (ddpf.RBitMask == r && ddpf.GBitMask == g && ddpf.BBitMask == b && ddpf.ABitMask == a)

		static DXGI_FORMAT GetDXGIFormat12(const DDS_PIXELFORMAT12& ddpf)
		{
			if (ddpf.flags & DDS_RGB)
			{
				switch (ddpf.RGBBitCount)
				{
				case 32:
					if (ISBITMASK12(0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000)) return DXGI_FORMAT_R8G8B8A8_UNORM;
					if (ISBITMASK12(0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000)) return DXGI_FORMAT_B8G8R8A8_UNORM;
					if (ISBITMASK12(0x00ff0000, 0x0000ff00, 0x000000ff, 0x00000000)) return DXGI_FORMAT_B8G8R8X8_UNORM;
					if (ISBITMASK12(0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000)) return DXGI_FORMAT_R10G10B10A2_UNORM;
					if (ISBITMASK12(0x0000ffff, 0xffff0000, 0x00000000, 0x00000000)) return DXGI_FORMAT_R16G16_UNORM;
					if (ISBITMASK12(0xffffffff, 0x00000000, 0x00000000, 0x00000000)) return DXGI_FORMAT_R32_FLOAT;
					break;
				case 16:
					if (ISBITMASK12(0x7c00, 0x03e0, 0x001f, 0x8000)) return DXGI_FORMAT_B5G5R5A1_UNORM;
					if (ISBITMASK12(0xf800, 0x07e0, 0x001f, 0x0000)) return DXGI_FORMAT_B5G6R5_UNORM;
					if (ISBITMASK12(0x0f00, 0x00f0, 0x000f, 0xf000)) return DXGI_FORMAT_B4G4R4A4_UNORM;
					break;
				}
			}
			else if (ddpf.flags & DDS_LUMINANCE)
			{
				if (8 == ddpf.RGBBitCount && ISBITMASK12(0x000000ff, 0x00000000, 0x00000000, 0x00000000))
					return DXGI_FORMAT_R8_UNORM;
				if (16 == ddpf.RGBBitCount)
				{
					if (ISBITMASK12(0x0000ffff, 0x00000000, 0x00000000, 0x00000000)) return DXGI_FORMAT_R16_UNORM;
					if (ISBITMASK12(0x000000ff, 0x00000000, 0x00000000, 0x0000ff00)) return DXGI_FORMAT_R8G8_UNORM;
				}
			}
			else if (ddpf.flags & DDS_ALPHA)
			{
				if (8 == ddpf.RGBBitCount)
					return DXGI_FORMAT_A8_UNORM;
			}
			else if (ddpf.flags & DDS_FOURCC)
			{
				if (MAKEFOURCC('D', 'X', 'T', '1') == ddpf.fourCC) return DXGI_FORMAT_BC1_UNORM;
				if (MAKEFOURCC('D', 'X', 'T', '3') == ddpf.fourCC) return DXGI_FORMAT_BC2_UNORM;
				if (MAKEFOURCC('D', 'X', 'T', '5') == ddpf.fourCC) return DXGI_FORMAT_BC3_UNORM;
				// DXT2/4 (alpha premultipliee) partagent le meme format DXGI que DXT3/5 --
				// aucune conversion de donnees, meme choix que GetDXGIFormat() D3D11.
				if (MAKEFOURCC('D', 'X', 'T', '2') == ddpf.fourCC) return DXGI_FORMAT_BC2_UNORM;
				if (MAKEFOURCC('D', 'X', 'T', '4') == ddpf.fourCC) return DXGI_FORMAT_BC3_UNORM;
				if (MAKEFOURCC('A', 'T', 'I', '1') == ddpf.fourCC) return DXGI_FORMAT_BC4_UNORM;
				if (MAKEFOURCC('B', 'C', '4', 'U') == ddpf.fourCC) return DXGI_FORMAT_BC4_UNORM;
				if (MAKEFOURCC('B', 'C', '4', 'S') == ddpf.fourCC) return DXGI_FORMAT_BC4_SNORM;
				if (MAKEFOURCC('A', 'T', 'I', '2') == ddpf.fourCC) return DXGI_FORMAT_BC5_UNORM;
				if (MAKEFOURCC('B', 'C', '5', 'U') == ddpf.fourCC) return DXGI_FORMAT_BC5_UNORM;
				if (MAKEFOURCC('B', 'C', '5', 'S') == ddpf.fourCC) return DXGI_FORMAT_BC5_SNORM;

				switch (ddpf.fourCC)
				{
				case 36:  return DXGI_FORMAT_R16G16B16A16_UNORM; // D3DFMT_A16B16G16R16
				case 110: return DXGI_FORMAT_R16G16B16A16_SNORM; // D3DFMT_Q16W16V16U16
				case 111: return DXGI_FORMAT_R16_FLOAT;          // D3DFMT_R16F
				case 112: return DXGI_FORMAT_R16G16_FLOAT;       // D3DFMT_G16R16F
				case 113: return DXGI_FORMAT_R16G16B16A16_FLOAT; // D3DFMT_A16B16G16R16F
				case 114: return DXGI_FORMAT_R32_FLOAT;          // D3DFMT_R32F
				case 115: return DXGI_FORMAT_R32G32_FLOAT;       // D3DFMT_G32R32F
				case 116: return DXGI_FORMAT_R32G32B32A32_FLOAT; // D3DFMT_A32B32G32R32F
				}
			}

			return DXGI_FORMAT_UNKNOWN;
		}

		//--------------------------------------------------------------------------------------
		// Cree la resource DEFAULT + upload tous les subresources (mip x tranche pour
		// 2D/array/cube, mip seul pour un volume -- chaque mip d'un volume couvre TOUTES
		// ses tranches Z en une seule subresource D3D12, meme convention que
		// CD3D12Texture::uploadArraySlices()/isVolume).
		static bool CreateAndUploadResource(CD3D12Driver* driver, D3D12_RESOURCE_DIMENSION resDim,
			size_t width, size_t height, size_t depth, size_t mipCount, size_t arraySize,
			DXGI_FORMAT format, const byte* bitData, size_t bitSize, ComPtr<ID3D12Resource>& outResource)
		{
			ID3D12Device2* device = driver->getDevice();
			if (!device)
				return false;

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = resDim;
			desc.Width = static_cast<UINT64>(width);
			desc.Height = static_cast<UINT>(height);
			desc.DepthOrArraySize = static_cast<UINT16>((resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? depth : arraySize);
			desc.MipLevels = static_cast<UINT16>(mipCount);
			desc.Format = format;
			desc.SampleDesc = { 1, 0 };
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;

			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outResource));
			if (FAILED(hr))
			{
				os::Printer::log("DDSTextureLoader12: CreateCommittedResource a echoue", ELL_ERROR);
				return false;
			}

			CD3D12Driver::UploadScope upload(driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
			if (!cmdList)
				return false;

			std::vector<ComPtr<ID3D12Resource>> uploadBuffers;
			uploadBuffers.reserve((resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? mipCount : (mipCount * arraySize));

			const byte* src = bitData;
			const byte* srcEnd = bitData + bitSize;
			D3D12_RESOURCE_DESC destDesc = outResource->GetDesc();

			auto uploadOneSubresource = [&](UINT subresourceIndex, size_t srcRowBytes, size_t srcNumRows,
				size_t sliceCount, size_t srcSliceBytes) -> bool
			{
				D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
				UINT numRows = 0;
				UINT64 rowSizeBytes = 0;
				UINT64 totalBytes = 0;
				device->GetCopyableFootprints(&destDesc, subresourceIndex, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

				D3D12_HEAP_PROPERTIES uploadHeapProps = {};
				uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
				D3D12_RESOURCE_DESC uploadDesc = {};
				uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				uploadDesc.Width = totalBytes;
				uploadDesc.Height = 1;
				uploadDesc.DepthOrArraySize = 1;
				uploadDesc.MipLevels = 1;
				uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
				uploadDesc.SampleDesc = { 1, 0 };
				uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

				ComPtr<ID3D12Resource> uploadBuffer;
				HRESULT hrBuf = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
				if (FAILED(hrBuf))
				{
					os::Printer::log("DDSTextureLoader12: CreateCommittedResource (upload) a echoue", ELL_ERROR);
					return false;
				}

				void* mapped = nullptr;
				D3D12_RANGE noRead = { 0, 0 };
				uploadBuffer->Map(0, &noRead, &mapped);

				const size_t copyRowBytes = std::min<size_t>(srcRowBytes, static_cast<size_t>(rowSizeBytes));
				const size_t sliceStrideDst = static_cast<size_t>(footprint.Footprint.RowPitch) * numRows;
				for (size_t z = 0; z < sliceCount; ++z)
				{
					if (src + srcSliceBytes > srcEnd)
					{
						os::Printer::log("DDSTextureLoader12: donnees .dds tronquees", ELL_ERROR);
						uploadBuffer->Unmap(0, nullptr);
						return false;
					}
					byte* dstSliceBase = static_cast<byte*>(mapped) + z * sliceStrideDst;
					const byte* srcSliceBase = src + z * srcSliceBytes;
					for (size_t row = 0; row < srcNumRows && row < numRows; ++row)
					{
						memcpy(dstSliceBase + row * footprint.Footprint.RowPitch,
							srcSliceBase + row * srcRowBytes, copyRowBytes);
					}
				}
				src += srcSliceBytes * sliceCount;

				uploadBuffer->Unmap(0, nullptr);

				D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
				dstLoc.pResource = outResource.Get();
				dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dstLoc.SubresourceIndex = subresourceIndex;

				D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
				srcLoc.pResource = uploadBuffer.Get();
				srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				srcLoc.PlacedFootprint = footprint;

				cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
				uploadBuffers.push_back(uploadBuffer);
				return true;
			};

			bool ok = true;
			if (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
			{
				size_t w = width, h = height, d = depth;
				for (size_t mip = 0; mip < mipCount && ok; ++mip)
				{
					size_t sliceNumBytes = 0, sliceRowBytes = 0, sliceNumRows = 0;
					if (!GetSurfaceInfo12(w, h, format, sliceNumBytes, sliceRowBytes, sliceNumRows))
					{
						ok = false;
						break;
					}
					ok = uploadOneSubresource(static_cast<UINT>(mip), sliceRowBytes, sliceNumRows, d, sliceNumBytes);
					w = std::max<size_t>(1, w >> 1);
					h = std::max<size_t>(1, h >> 1);
					d = std::max<size_t>(1, d >> 1);
				}
			}
			else
			{
				for (size_t slice = 0; slice < arraySize && ok; ++slice)
				{
					size_t w = width, h = height;
					for (size_t mip = 0; mip < mipCount && ok; ++mip)
					{
						size_t mipNumBytes = 0, mipRowBytes = 0, mipNumRows = 0;
						if (!GetSurfaceInfo12(w, h, format, mipNumBytes, mipRowBytes, mipNumRows))
						{
							ok = false;
							break;
						}
						const UINT subresourceIndex = static_cast<UINT>(mip + slice * mipCount);
						ok = uploadOneSubresource(subresourceIndex, mipRowBytes, mipNumRows, 1, mipNumBytes);
						w = std::max<size_t>(1, w >> 1);
						h = std::max<size_t>(1, h >> 1);
					}
				}
			}

			if (ok)
			{
				CD3DX12_RESOURCE_BARRIER toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
					outResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				cmdList->ResourceBarrier(1, &toShaderResource);
			}

			upload.endAndWait();
			return ok;
		}

		//--------------------------------------------------------------------------------------
		bool CreateDDSTextureFromMemory12(CD3D12Driver* driver, const unsigned char* ddsData,
			size_t ddsDataSize, SDDSTexture12Result& outResult)
		{
			if (!driver || !ddsData)
			{
				os::Printer::log("DDSTextureLoader12: driver/donnees nulles", ELL_ERROR);
				return false;
			}

			if (ddsDataSize < sizeof(uint32) + sizeof(DDS_HEADER12))
			{
				os::Printer::log("DDSTextureLoader12: fichier .dds trop court", ELL_ERROR);
				return false;
			}

			const uint32 magic = *reinterpret_cast<const uint32*>(ddsData);
			if (magic != DDS_MAGIC)
			{
				os::Printer::log("DDSTextureLoader12: signature .dds invalide", ELL_ERROR);
				return false;
			}

			const DDS_HEADER12* header = reinterpret_cast<const DDS_HEADER12*>(ddsData + sizeof(uint32));
			if (header->size != sizeof(DDS_HEADER12) || header->ddspf.size != sizeof(DDS_PIXELFORMAT12))
			{
				os::Printer::log("DDSTextureLoader12: header .dds invalide", ELL_ERROR);
				return false;
			}

			bool hasDXT10Header = false;
			if ((header->ddspf.flags & DDS_FOURCC) && MAKEFOURCC('D', 'X', '1', '0') == header->ddspf.fourCC)
			{
				if (ddsDataSize < sizeof(uint32) + sizeof(DDS_HEADER12) + sizeof(DDS_HEADER_DXT10_12))
				{
					os::Printer::log("DDSTextureLoader12: header DX10 tronque", ELL_ERROR);
					return false;
				}
				hasDXT10Header = true;
			}

			size_t width = header->width;
			size_t height = header->height;
			size_t depth = header->depth ? header->depth : 1;
			size_t arraySize = 1;
			size_t mipCount = header->mipMapCount ? header->mipMapCount : 1;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			bool isCubeMap = false;
			D3D12_RESOURCE_DIMENSION resDim = D3D12_RESOURCE_DIMENSION_UNKNOWN;

			const byte* bitData = ddsData + sizeof(uint32) + sizeof(DDS_HEADER12);
			size_t bitSize = ddsDataSize - (sizeof(uint32) + sizeof(DDS_HEADER12));

			if (hasDXT10Header)
			{
				const DDS_HEADER_DXT10_12* ext = reinterpret_cast<const DDS_HEADER_DXT10_12*>(
					ddsData + sizeof(uint32) + sizeof(DDS_HEADER12));
				bitData += sizeof(DDS_HEADER_DXT10_12);
				bitSize -= sizeof(DDS_HEADER_DXT10_12);

				arraySize = ext->arraySize ? ext->arraySize : 1;
				format = ext->dxgiFormat;
				resDim = static_cast<D3D12_RESOURCE_DIMENSION>(ext->resourceDimension);

				if (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (ext->miscFlag & 0x4 /* D3D11_RESOURCE_MISC_TEXTURECUBE */))
				{
					arraySize *= 6;
					isCubeMap = true;
				}
				if (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
					arraySize = 1;
				else
					depth = 1;
			}
			else
			{
				format = GetDXGIFormat12(header->ddspf);
				if (format == DXGI_FORMAT_UNKNOWN)
				{
					os::Printer::log("DDSTextureLoader12: format de pixel .dds non reconnu (legacy sans header DX10)", ELL_ERROR);
					return false;
				}

				if (header->flags & DDS_HEADER_FLAGS_VOLUME)
				{
					resDim = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
				}
				else
				{
					if (header->caps2 & DDS_CUBEMAP)
					{
						if ((header->caps2 & DDS_CUBEMAP_ALLFACES) != DDS_CUBEMAP_ALLFACES)
						{
							os::Printer::log("DDSTextureLoader12: cube map .dds incomplete (6 faces requises)", ELL_ERROR);
							return false;
						}
						arraySize = 6;
						isCubeMap = true;
					}
					depth = 1;
					resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				}
			}

			if (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE1D || resDim == D3D12_RESOURCE_DIMENSION_UNKNOWN)
			{
				os::Printer::log("DDSTextureLoader12: dimension de ressource .dds non supportee (1D absent de E_TEXTURE_TYPE)", ELL_ERROR);
				return false;
			}

			if (BitsPerPixel12(format) == 0)
			{
				os::Printer::log("DDSTextureLoader12: DXGI_FORMAT .dds non supporte par ce chargeur", ELL_ERROR);
				return false;
			}

			ComPtr<ID3D12Resource> resource;
			if (!CreateAndUploadResource(driver, resDim, width, height, depth, mipCount, arraySize,
				format, bitData, bitSize, resource))
			{
				return false;
			}

			outResult.Resource = resource;
			outResult.Format = format;
			outResult.Width = static_cast<UINT>(width);
			outResult.Height = static_cast<UINT>(height);
			outResult.MipLevels = static_cast<UINT>(mipCount);
			outResult.ArraySize = static_cast<UINT>(arraySize);
			outResult.IsCubeMap = isCubeMap;
			outResult.Dimension = resDim;
			return true;
		}
	}
}

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
