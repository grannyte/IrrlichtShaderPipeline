#ifndef __C_DIRECTX11_TEXTURE_H_INCLUDED__
#define __C_DIRECTX11_TEXTURE_H_INCLUDED__

#include "IrrCompileConfig.h"

#ifdef _IRR_WINDOWS_

#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include <irrArray.h>
#include "ITexture.h"
#include "IImage.h"
#include "CTiledResourceHelpers.h"
#include <d3d11.h>

namespace irr
{
	namespace video
	{
		class CD3D11Driver;
		// forward declaration for RTT depth buffer handling

		class CD3D11Texture : public ITexture
		{
		public:

			//! constructor
			CD3D11Texture(IImage* image, CD3D11Driver* driver,
				u32 flags, const io::path& name, u32 arraySlices = 1, void* mipmapData = 0);

			//! rendertarget constructor
			CD3D11Texture(CD3D11Driver* driver, const core::dimension2d<u32>& size, const io::path& name,
				const ECOLOR_FORMAT format = ECF_UNKNOWN, u32 arraySlices = 1,
				u32 sampleCount = 1, u32 sampleQuality = 0, bool unorderedAccess = false);
			//! Array constructor
			CD3D11Texture(const core::array<ITexture*>* surfaces, CD3D11Driver* driver,
				u32 flags, const io::path& name, E_TEXTURE_TYPE Type, u32 arraySlices, void* mipmapData);
			//! Tiled texture (D3D11_RESOURCE_MISC_TILED): address space only, its tiles mapped by
			//! CD3D11Driver::updateTileMappings(). mipLevels 0 = the full chain.
			CD3D11Texture(CD3D11Driver* driver, const core::dimension2d<u32>& size, const io::path& name,
				ECOLOR_FORMAT format, u32 mipLevels, u32 arraySlices, bool renderTarget, STiledTextureTag);
			//! destructor
			virtual ~CD3D11Texture();

			//! lock function
			virtual void* lock(E_TEXTURE_LOCK_MODE mode = ETLM_READ_WRITE, u32 mipmapLevel = 0);

			virtual void* lock(bool readOnly, u32 mipmapLevel = 0, u32 arraySlice = 0);

			void MapArraySlice(HRESULT& hr, const irr::u32& mipmapLevel, const irr::u32& arraySlice, D3D11_MAPPED_SUBRESOURCE& mappedData, D3D11_MAP MapDirection, ID3D11Resource* LocalTextureBuffer);

			//! unlock function
			virtual void unlock();

			//! Regenerates the mip map levels of the texture. Useful after locking and
			//! modifying the texture
			virtual void regenerateMipMapLevels(void* mipmapData = 0);

			virtual u32 getNumberOfArraySlices() const;

		public:
			//! return texture resource
			//! return texture resource
			inline ID3D11Resource* getTextureResource() const
			{
				return Texture;
			}

			//! return render target view
			ID3D11RenderTargetView* getRenderTargetView() const;

			//! render target view for one array slice, created on first use
			ID3D11RenderTargetView* getRenderTargetView(u32 arraySlice);

			//! return shader resource view
			ID3D11ShaderResourceView* getShaderResourceView() const;

			//! return unordered access view (compute-writable textures only, see IsUnorderedAccess)
			ID3D11UnorderedAccessView* getUnorderedAccessView() const;

			//! Created by the tiled constructor: no backing until its tiles are mapped.
			bool isTiled() const { return Tiled; }

		private:
			friend class CD3D11Driver;

			ID3D11Device* Device;
			ID3D11DeviceContext* Context;
			ID3D11Resource* Texture;
			ID3D11RenderTargetView* RTView;
			//! per-slice views, built lazily; index is the array slice
			core::array<ID3D11RenderTargetView*> SliceRTViews;
			ID3D11ShaderResourceView* SRView;
			ID3D11UnorderedAccessView* UAView;
			ID3D11DepthStencilView* dsView;
			D3D11_RESOURCE_DIMENSION TextureDimension;
			D3D11_MAP LastMapDirection;

			CD3D11Driver* Driver;
			u32 NumberOfMipLevels;
			u32 NumberOfArraySlices;
			u32 SampleCount;
			u32 SampleQuality;

			ID3D11Resource* TextureBuffer;		// staging texture used for lock/unlock
			u32 MipLevelLocked;
			u32 ArraySliceLocked;

			bool HardwareMipMaps;
			//! Pixels went to CreateTexture2D as initial data, so copyTexture() has nothing to do.
			bool UploadedAtCreation = false;
			bool Tiled = false;

			//! Single-sample twin of a multisampled render target, resolved into by
			//! getShaderResourceView() so the target can be sampled; null for everything else.
			ID3D11Texture2D* ResolvedTexture = 0;
			ID3D11ShaderResourceView* ResolvedSRView = 0;
			DXGI_FORMAT ResolveFormat = DXGI_FORMAT_UNKNOWN;

			//! creates hardware render target
			void createRenderTarget(const ECOLOR_FORMAT format);

			//! creates the hardware texture
			bool createTexture(u32 flags, IImage* image);

			//! copies the image to the texture
			bool copyTexture(IImage* image);

			//! copies the texture to the texture layer
			bool copyTexture(ITexture* image, int layer);

			//! set Pitch based on the d3d format
			void setPitch(DXGI_FORMAT d3dformat);

			//! create texture buffer needed for lock/unlock
			bool createTextureBuffer();

			//! create views to bound texture to pipeline
			bool createViews();
		};
	}
}

#endif
#endif
#endif