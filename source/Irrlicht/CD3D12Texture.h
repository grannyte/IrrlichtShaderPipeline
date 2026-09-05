// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// D3D12 2D textures and render target textures.
//
// State tracking: unlike vertex/index buffers (upload heap, always GENERIC_READ), a
// texture's resource state actually changes over the course of rendering (shader
// resource <-> render target <-> copy dest). CurrentState is therefore a member of the
// texture object itself rather than a separate global tracker - sufficient as long as a
// given resource is not recorded on two command lists in parallel, which does not
// currently happen here.
//
// Notes on current coverage:
// - ETCF_CREATE_MIP_MAPS is honored for the IImage-loading constructor: a full mip chain
//   is created (MipLevelCount, see computeMipLevels()) and filled via a pixel-shader blit
//   (CD3D12Driver::createMipGenPipeline()/getOrCreateMipGenPSO(), not a compute shader).
//   lock()/unlock() accept a mipmapLevel != 0. regenerateMipMapLevels() reruns the same
//   blit chain from the current mip 0. The empty/render-target texture constructor
//   (size-based) always stays at 1 mip level.
// - ECOLOR_FORMAT -> DXGI_FORMAT mapping covers ECF_A8R8G8B8, ECF_R8G8B8 (promoted to
//   DXGI_FORMAT_R8G8B8A8_UNORM, alpha forced to 0xFF since there is no 24-bit DXGI
//   equivalent; ColorFormat becomes ECF_A8R8G8B8 after creation, cf.
//   CD3D11Texture::createTexture()), ECF_R5G6B5, ECF_A1R5G5B5, the floating point formats
//   R16F/G16R16F/A16B16G16R16F/R32F/G32R32F/B32G32R32F/A32B32G32R32F, and the
//   block-compressed family ECF_DXT1-5 (+ the sRGB twins), ECF_BC4_U/S, ECF_BC5_U/S,
//   ECF_BC6_U/S, ECF_BC7_U/S. Not covered: R10G10B10A2/R11G11B10_FLOAT/R9G9B9E5 (no
//   corresponding ECOLOR_FORMAT value exists). An unsupported format fails with a clear
//   message rather than silently producing a corrupt texture.
// - .dds loading via the IImage* constructor mirrors CD3D11Texture: an image the loader
//   flagged compressed (IImage::isCompressed(), i.e. block-compressed, a wide format, a cube
//   map, an array or a volume) bypasses the ECOLOR_FORMAT path entirely and goes to a
//   dedicated DDS loader (DDSTextureLoader.h/.cpp on D3D11, DDSTextureLoader12.h/.cpp
//   here) operating directly on the raw file bytes (CImage::CompressedSize): DDS_HEADER/
//   DX10 header parsed, D3D12 resource created with the full mip chain/array slices/cube
//   faces, uploaded per-subresource (UPLOAD heap + CopyTextureRegion, same mechanism as
//   uploadArraySlices()). 1D textures (DDS_HEADER_DXT10::resourceDimension ==
//   TEXTURE1D) are rejected explicitly - E_TEXTURE_TYPE has no 1D equivalent in this
//   engine.
// - lock()/unlock(): ETLM_WRITE_ONLY, ETLM_READ_ONLY and ETLM_READ_WRITE are all real
//   paths - READ_WRITE does a GPU->CPU readback, copies the result into a CPU-writable
//   UPLOAD resource, and re-uploads it on unlock() (a full round trip is required because
//   a D3D12_HEAP_TYPE_READBACK resource must stay in D3D12_RESOURCE_STATE_COPY_DEST
//   permanently and cannot be reused as a copy source for the write).
// - 2D array / cube / cube array / 3D textures are supported via the
//   core::array<IImage*>& constructor, built from N already-loaded 2D images (one per
//   slice/face/Z-layer) rather than from already-uploaded ITexture* the way
//   CD3D11Texture(array<ITexture*>*, ...) does - this driver has no own file-loading path
//   (createImageFromFile is stubbed, see CD3D12Driver.h), so there is no existing
//   ITexture* source to recombine; CPU pixels are used directly. ETT_3D_ARRAY has no
//   hardware equivalent (no D3D API supports an array of 3D textures) and is treated as
//   ETT_3D (same behavior as CD3D11Texture::createTexture(), which uses
//   NumberOfArraySlices as depth in both cases). No mip chain for this path
//   (MipLevelCount always 1, unlike the plain IImage* constructor above). No
//   render-target view for these types either (image-array construction is
//   shader-read-only; rendering into an individual cube face is a separate, unhandled
//   need). The render-target-texture-array path (dimension2d<u32>& constructor below,
//   renderTarget=true, arraySlices > 1) is covered separately, see below.
// - The empty/render-target texture constructor (dimension2d<u32>&) accepts
//   arraySlices > 1 (TextureType becomes ETT_2D_ARRAY, RTV = whole-array view via
//   createRenderTargetView(), SRV whole-array handling already exists in
//   createShaderResourceView()'s ETT_2D_ARRAY branch). Intended use: a geometry shader
//   writing SV_RenderTargetArrayIndex routes each primitive to its own slice in a single
//   draw (stereo VR rendering, offline atlas baking). Combined with sampleCount > 1 this
//   is rejected (CD3D12Driver::addRenderTargetTexture() falls back to a single slice with
//   a warning) - same scope choice as D3D11 (CD3D11Texture doesn't support MSAA+array
//   either). The auto-managed depth/stencil (checkRTTDepthBuffer()) stays single-slice:
//   setRenderTarget() does not bind a DSV for an ETT_2D_ARRAY target (parity with
//   CD3D11Driver::setRenderTarget(), which makes the same choice for the same reason).
// - No depth/stencil support (ITexture::IsDepthStencil is always false here). The depth
//   buffer needed by beginScene()/zBuffer is managed separately by the driver, not
//   through this class.

#ifndef __C_DIRECTX12_TEXTURE_H_INCLUDED__
#define __C_DIRECTX12_TEXTURE_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <irrArray.h>
#include "ITexture.h"
#include "IImage.h"
#include "CTiledResourceHelpers.h"

namespace irr
{
	namespace video
	{
		using Microsoft::WRL::ComPtr;

		class CD3D12Driver;

		//! Converts an Irrlicht color format to a DXGI format. Returns DXGI_FORMAT_UNKNOWN
		//! (and logs an error) for formats not currently supported.
		DXGI_FORMAT getD3D12ColorFormat(ECOLOR_FORMAT format);

		class CD3D12Texture : public ITexture
		{
		public:
			//! Standard texture, created from an image (normal loading path).
			CD3D12Texture(IImage* image, CD3D12Driver* driver, u32 flags, const io::path& name);

			//! Empty texture (e.g. a shader write target) or render target texture.
			//! renderTarget=true => initial state D3D12_RESOURCE_STATE_RENDER_TARGET + RTV
			//! preallocated in the driver's RTV heap; otherwise PIXEL_SHADER_RESOURCE + SRV.
			//! sampleCount > 1 => the RTV resource itself is multisampled (no SRV on it
			//! directly, see the ResolvedResource comment below); sampleCount == 1 (default)
			//! is the plain single-sample case.
			//! arraySlices > 1 => a real render-target-texture-array (TextureType becomes
			//! ETT_2D_ARRAY, RTV = a view covering all N slices at once, see
			//! createRenderTargetView()) so a geometry shader writing
			//! SV_RenderTargetArrayIndex can route each primitive to its own slice in a
			//! single draw. Combined with sampleCount > 1 is unsupported (rejected upstream
			//! by CD3D12Driver::addRenderTargetTexture()). arraySlices == 1 (default) is the
			//! plain single-slice case.
			//! unorderedAccess=true => the resource also gets ALLOW_UNORDERED_ACCESS +
			//! a UAV, so a compute shader can write it via
			//! CD3D12Driver::dispatchComputeShaderToTexture(); ignored (with a warning)
			//! when combined with renderTarget=true, since the render-target path never
			//! needs it here. Same non-RT/non-depth CurrentState (COPY_DEST) as the plain
			//! empty-texture case - transitionTo() moves it to UNORDERED_ACCESS on first
			//! compute write.
			CD3D12Texture(CD3D12Driver* driver, const core::dimension2d<u32>& size,
				const io::path& name, ECOLOR_FORMAT format, bool renderTarget,
				u32 sampleCount = 1, u32 sampleQuality = 0, u32 arraySlices = 1,
				bool unorderedAccess = false);

			//! 2D array / cube / cube array / 3D texture (depth = images.size() for
			//! ETT_3D), built from N already-loaded 2D images (one per slice/face/Z-layer).
			//! See the file header comment for details (ETT_3D_ARRAY treated as ETT_3D, no
			//! mipmaps, no RTV).
			CD3D12Texture(const core::array<IImage*>& images, CD3D12Driver* driver,
				E_TEXTURE_TYPE type, const io::path& name);

			//! Combines N already-uploaded textures (each ETT_2D, typically loaded
			//! individually via CD3D12Driver::getTexture(filename) - one per cube face or
			//! array slice, same role as CD3D11Texture(const array<ITexture*>*, ...)) into a
			//! single D3D12 array/cube resource. Copies GPU->GPU directly per subresource
			//! (CopyTextureRegion resource-to-resource, no intermediate CPU buffer): unlike
			//! the D3D11 equivalent (Map/memcpy with a manually computed pitch via
			//! IImage::getBitsPerPixelFromFormat(), which is wrong for block-compressed
			//! formats other than BC6/BC7), this copy works correctly for any format since
			//! D3D12 handles subresource copy semantics itself. Precondition (checked by the
			//! caller, CD3D12Driver::getTexture()): all surfaces share the same size,
			//! DxgiFormat, and mip level count.
			CD3D12Texture(const core::array<ITexture*>& surfaces, CD3D12Driver* driver,
				E_TEXTURE_TYPE type, const io::path& name);

			//! Tiled texture: a reserved resource (CreateReservedResource, 64KB_UNDEFINED_SWIZZLE) whose
			//! tiles CD3D12Driver::updateTileMappings() binds to a heap. mipLevels 0 = the full chain.
			//! Mips are never generated for it (the caller streams them in tile by tile).
			CD3D12Texture(CD3D12Driver* driver, const core::dimension2d<u32>& size, const io::path& name,
				ECOLOR_FORMAT format, u32 mipLevels, u32 arraySlices, bool renderTarget, STiledTextureTag);

			virtual ~CD3D12Texture();

			//! Created by the tiled constructor: no backing until its tiles are mapped.
			bool isTiled() const { return Tiled; }

			virtual void* lock(E_TEXTURE_LOCK_MODE mode = ETLM_READ_WRITE, u32 mipmapLevel = 0) _IRR_OVERRIDE_;
			virtual void unlock() _IRR_OVERRIDE_;
			virtual void regenerateMipMapLevels(void* mipmapData = 0) _IRR_OVERRIDE_;

			ID3D12Resource* getResource() const { return Resource.Get(); }
			UINT getMipLevelCount() const { return MipLevelCount; }

			//! False if the GPU resource could not be created (ECOLOR_FORMAT with no DXGI
			//! equivalent, CreateCommittedResource failure, device removed...). Constructors
			//! cannot otherwise signal failure; any caller that just constructed one must
			//! check this and discard the object rather than publish it - otherwise
			//! lock()/getResource()/a draw's binding end up operating on a null Resource.
			bool hasDeviceResource() const { return Resource != nullptr; }

			//! Issues a CurrentState -> newState resource barrier on cmdList if needed, and
			//! updates CurrentState. No-op if the requested state is already current.
			void transitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState);

			D3D12_RESOURCE_STATES getCurrentState() const { return CurrentState; }

			bool hasRenderTargetView() const { return HasRTV; }
			D3D12_CPU_DESCRIPTOR_HANDLE getRenderTargetView() const { return RTVHandle; }
			//! RTV over one slice of a render-target array (mip 0), created on first use for
			//! CD3D12Driver::setRenderTargetSlice(). Slice 0 of a single-slice texture is
			//! getRenderTargetView() itself. A null handle (ptr == 0, logged) if the heap is full.
			D3D12_CPU_DESCRIPTOR_HANDLE getRenderTargetView(u32 arraySlice);
			//! Slices of an array / faces of a cube; depth for ETT_3D. 1 for a plain 2D texture.
			UINT getArraySliceCount() const { return NumberOfArraySlices; }

			//! True if this texture is a depth buffer (decided by FORMAT, not the caller:
			//! addRenderTargetTexture(ECF_D32/D24S8/...) produces a DSV, never an RTV). Such
			//! a texture binds as setRenderTarget()'s depth-stencil target, not as a color
			//! target.
			bool isDepthTexture() const { return IsDepth; }
			bool hasDepthStencilView() const { return HasDSV; }
			D3D12_CPU_DESCRIPTOR_HANDLE getDepthStencilView() const { return DSVHandle; }
			//! Actual DXGI format of the resource, needed so CD3D12Driver::setRenderTarget()
			//! can fill SPSOKey::RTVFormats[] with the effective format of each bound target
			//! (a render target texture can differ from DXGI_FORMAT_R8G8B8A8_UNORM, see
			//! addRenderTargetTexture()).
			DXGI_FORMAT getDxgiFormat() const { return DxgiFormat; }

			bool hasShaderResourceView() const { return HasSRV; }
			D3D12_CPU_DESCRIPTOR_HANDLE getShaderResourceView() const { return SRVHandle; }
			UINT getSRVHeapIndex() const { return SRVHeapIndex; }

			//! True for a texture created with unorderedAccess=true (see the size-based
			//! constructor) - a compute shader can write it via dispatchComputeShaderToTexture.
			bool hasUnorderedAccessView() const { return HasUAV; }
			D3D12_CPU_DESCRIPTOR_HANDLE getUnorderedAccessView() const { return UAVHandle; }

			//! Sample count of the RTV resource (1 = no MSAA). Read by
			//! CD3D12Driver::setRenderTarget() to propagate SPSOKey::SampleCount and by
			//! checkRTTDepthBuffer() to match the depth buffer.
			UINT getSampleCount() const { return SampleCount; }

			//! If SampleCount > 1, resolves Resource (multisampled, never directly
			//! sampleable by a PSMain that only reads Texture2D) into ResolvedResource
			//! (single-sample, carries the SRV returned by getShaderResourceView()) via
			//! ResolveSubresource. No-op if SampleCount == 1. Called by
			//! CD3D12Driver::allocateSRVTableSlot() before every shader read of this
			//! texture - there is no "content changed since last resolve" tracking, so a
			//! redundant resolve is correct but not optimal if the RTT is sampled multiple
			//! times without being redrawn in between.
			void resolveIfNeeded(ID3D12GraphicsCommandList* cmdList);

		private:
			bool createResource(bool asRenderTarget);
			bool createShaderResourceView();
			bool createRenderTargetView();
			//! Creates the UAV (same heap as SRV/CBV, see CD3D12Driver::getSRVHeap()) for a
			//! texture created with unorderedAccess=true. Only called from createResource().
			bool createUnorderedAccessView();
			//! Creates ResolvedResource (D3D12_HEAP_TYPE_DEFAULT, single-sample,
			//! D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) and its SRV (HasSRV/SRVHandle
			//! point at it, never at the MSAA resource itself). Only called when
			//! SampleCount > 1, from createResource().
			bool createResolveResource();
			void uploadInitialData(const void* data, u32 rowPitchBytes, bool expandR8G8B8ToRGBA8 = false,
				UINT subresourceIndex = 0);

			//! Uploads N images in a single beginUpload()/endUploadAndWait(), one per
			//! subresource (array slice/cube face) or Z-layer (3D volume). A single
			//! ALL_SUBRESOURCES barrier at the end rather than one per slice, since
			//! CurrentState is a single scalar for the whole resource - transitioning slice
			//! by slice would break that invariant from the 2nd iteration on (the first
			//! iteration's ALL_SUBRESOURCES transition would move every subresource to
			//! PIXEL_SHADER_RESOURCE, including ones not yet copied).
			void uploadArraySlices(const core::array<IImage*>& images, bool expandR8G8B8ToRGBA8);

			//! Number of mip levels for a full width x height chain (down to 1x1 at the
			//! top), same formula as D3D11CalcSubresource/implicit
			//! D3D11_RESOURCE_MISC_GENERATE_MIPS: floor(log2(max(width,height))) + 1.
			static UINT computeMipLevels(UINT width, UINT height);

			//! Fills MipLevelCount-1 mip levels via successive pixel-shader blits from the
			//! previous mip (see CD3D12Driver::createMipGenPipeline()/getOrCreateMipGenPSO()).
			//! No-op (with a warning) if MipLevelCount <= 1 or if the driver's dedicated
			//! pipeline could not be created. Precondition: mip 0 is already uploaded and the
			//! resource is in its resting state (CurrentState, uniform across all
			//! subresources).
			void generateMips();

			CD3D12Driver* Driver;
			ComPtr<ID3D12Resource> Resource;
			bool Tiled = false;
			D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;
			DXGI_FORMAT DxgiFormat = DXGI_FORMAT_UNKNOWN;
			//! 1 if no mipmaps, otherwise computeMipLevels(Size.Width, Size.Height).
			//! Determines desc.MipLevels at creation (createResource()) and the number of
			//! generateMips() iterations.
			UINT MipLevelCount = 1;

			//! 1 for ETT_2D. For ETT_2D_ARRAY/ETT_CUBE/ETT_CUBE_ARRAY, number of
			//! slices/faces (DepthOrArraySize of the resource). For ETT_3D (and
			//! ETT_3D_ARRAY, an alias of ETT_3D), depth of the volume - not a "real" array,
			//! same convention as CD3D11Texture.
			UINT NumberOfArraySlices = 1;

			bool HasSRV = false;
			D3D12_CPU_DESCRIPTOR_HANDLE SRVHandle = {};
			UINT SRVHeapIndex = 0;

			bool HasUAV = false;
			D3D12_CPU_DESCRIPTOR_HANDLE UAVHandle = {};
			UINT UAVHeapIndex = 0;

			bool HasRTV = false;
			D3D12_CPU_DESCRIPTOR_HANDLE RTVHandle = {};
			UINT RTVHeapIndex = 0;

			//! Per-slice RTVs of a render-target array, see getRenderTargetView(u32). Empty until
			//! a slice is asked for; an entry stays Valid == false if its allocation failed.
			struct SSliceRTV
			{
				bool Valid = false;
				UINT HeapIndex = 0;
				D3D12_CPU_DESCRIPTOR_HANDLE Handle = {};
			};
			std::vector<SSliceRTV> SliceRTVs;

			//! Depth texture (see isDepthTexture()). Decided by FORMAT in createResource():
			//! TYPELESS + ALLOW_DEPTH_STENCIL resource, DSV + SRV, never an RTV. The DSV
			//! slot comes from CD3D12Driver::getDSVHeap().
			bool IsDepth = false;
			bool HasDSV = false;
			D3D12_CPU_DESCRIPTOR_HANDLE DSVHandle = {};
			UINT DSVHeapIndex = 0;

			//! Creates the DSV (and nothing else) on Resource. See createResource().
			bool createDepthStencilView();

			//! See the MSAA strategy comment at the top of CD3D12Texture.cpp (explicit
			//! resolve, since PSMain cannot read Texture2DMS). 1 = no MSAA (Resource then
			//! carries the RTV and SRV directly). > 1: Resource carries only the RTV (no SRV
			//! on it, see createResource()); ResolvedResource (single-sample) carries the
			//! SRV and is filled by resolveIfNeeded().
			UINT SampleCount = 1;
			UINT SampleQuality = 0;
			ComPtr<ID3D12Resource> ResolvedResource;
			D3D12_RESOURCE_STATES ResolvedState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

			// Intermediate resource for lock()/unlock() - recreated on each lock, released
			// on unlock. See the READ_WRITE limitations note in the file header comment.
			ComPtr<ID3D12Resource> StagingResource;
			E_TEXTURE_LOCK_MODE LastLockMode = ETLM_READ_WRITE;
			void* MappedStagingData = nullptr;
			UINT StagingRowPitch = 0;
			//! Mip level requested by the last lock(), reused by unlock() to target the
			//! right subresource (GetCopyableFootprints/D3D12_TEXTURE_COPY_LOCATION).
			UINT LastLockMipLevel = 0;
		};

	}
}

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
#endif
