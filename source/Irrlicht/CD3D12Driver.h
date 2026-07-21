// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#ifndef __C_VIDEO_DIRECTX_12_H_INCLUDED__
#define __C_VIDEO_DIRECTX_12_H_INCLUDED__
#include "os.h"
#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <array>
#include <mutex>
#include <functional>
#include <cstring>
#include <unordered_map>
#include <map>
#include <utility>
#include <memory>

#include "CNullDriver.h"
#include "SIrrCreationParameters.h"
#include "IMaterialRendererServices.h"
#include "IMaterialRenderer.h"
#include "IShaderConstantSetCallBack.h"
#include "IVertexBuffer.h"
#include "IIndexBuffer.h"
#include "CD3D12HardwareBuffer.h"
#include "CD3D12Texture.h"
#include "CD3D12PSOCache.h"
#include "CD3D12MaterialRenderer.h"
#include "CD3D12DefaultShaders.h"
#include "SMaterial.h"
#include "IDeferredContext.h"

// CD3DX12_* helpers shared with CD3D11On12Driver (interop) -- see CD3D12Helpers.h.
#include "CD3D12Helpers.h"

namespace irr
{
	namespace video
	{
		using Microsoft::WRL::ComPtr;

		static const UINT NativeFrameCount = 3;

		//! CPU-only (non shader-visible) descriptor heap allocator: RTV, DSV, CBV/SRV/UAV, Sampler.
		//! Simple linear free-list: descriptors are allocated one at a time and returned to the
		//! pool when their owning resource is destroyed.
		class CD3D12DescriptorHeapAllocator
		{
		public:
			CD3D12DescriptorHeapAllocator() = default;

			void init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
				UINT capacity, bool shaderVisible = false)
			{
				Device = device;
				Type = type;
				Capacity = capacity;

				D3D12_DESCRIPTOR_HEAP_DESC desc = {};
				desc.Type = type;
				desc.NumDescriptors = capacity;
				desc.Flags = shaderVisible ?
					D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE :
					D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

				HRESULT hr = Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&Heap));
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12DescriptorHeapAllocator: CreateDescriptorHeap failed", ELL_ERROR);
					return;
				}

				DescriptorSize = Device->GetDescriptorHandleIncrementSize(type);
				HeapStartCPU = Heap->GetCPUDescriptorHandleForHeapStart();
				if (shaderVisible)
					HeapStartGPU = Heap->GetGPUDescriptorHandleForHeapStart();

				FreeList.clear();
				FreeList.reserve(capacity);
				for (UINT i = 0; i < capacity; ++i)
					FreeList.push_back(capacity - 1 - i); // pop_back() yields ascending order
			}

			//! Allocates a free descriptor. Returns false if the heap is full
			//! (grow Capacity on the next init() call; no dynamic heap growth here).
			bool allocate(UINT& outIndex, CD3DX12_CPU_DESCRIPTOR_HANDLE& outHandle)
			{
				std::lock_guard<std::mutex> lock(Mutex);
				if (FreeList.empty())
				{
					os::Printer::log("CD3D12DescriptorHeapAllocator: heap full", ELL_WARNING);
					return false;
				}
				outIndex = FreeList.back();
				FreeList.pop_back();
				outHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(HeapStartCPU, outIndex, DescriptorSize);
				return true;
			}

			void free(UINT index)
			{
				std::lock_guard<std::mutex> lock(Mutex);
				FreeList.push_back(index);
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(UINT index) const
			{
				return CD3DX12_CPU_DESCRIPTOR_HANDLE(HeapStartCPU, index, DescriptorSize);
			}

			ID3D12DescriptorHeap* getHeap() const { return Heap.Get(); }
			UINT getDescriptorSize() const { return DescriptorSize; }

		private:
			ComPtr<ID3D12DescriptorHeap> Heap;
			ID3D12Device* Device = nullptr;
			D3D12_DESCRIPTOR_HEAP_TYPE Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCPU = {};
			D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGPU = {};
			UINT DescriptorSize = 0;
			UINT Capacity = 0;
			std::vector<UINT> FreeList;
			std::mutex Mutex;
		};

		//! Per frame-in-flight state: its own command allocator (never share an allocator between
		//! command lists recorded in parallel) plus the fence value marking "this frame has been
		//! fully consumed by the GPU".
		struct SD3D12FrameContext
		{
			ComPtr<ID3D12CommandAllocator> CommandAllocator;
			UINT64 FenceValue = 0;
			ComPtr<ID3D12Resource> BackBuffer;
			CD3DX12_CPU_DESCRIPTOR_HANDLE RTVHandle;

			// --- Per-frame constant buffer ring (b0/b1) + shader-visible SRV heap ---
			// A linear allocator reset every beginScene() (never freed individually): its contents
			// only need to survive a single frame.

			//! Upload heap mapped once for its lifetime; each drawMeshBuffer() call sub-allocates its
			//! CBV b0 (World) and CBV b1 (View+Proj) from it, at a 256-byte aligned offset
			//! (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT).
			ComPtr<ID3D12Resource> ConstantRing;
			void* ConstantRingMapped = nullptr;
			UINT64 ConstantRingOffset = 0;
			//! Current capacity of ConstantRing (may exceed ConstantRingSizePerFrame after growing --
			//! see CD3D12Driver::growConstantRing()/allocateConstant()); initialized to
			//! ConstantRingSizePerFrame in createFrameDrawResources().
			UINT64 ConstantRingCapacity = 0;

			//! Shader-visible CBV/SRV/UAV heap (distinct from the driver's CPU-only CBVSRVUAVHeap):
			//! each drawMeshBuffer() call copies (CopyDescriptorsSimple) the current texture's SRV t0
			//! descriptor here from the CPU-only heap so it can be passed to
			//! SetGraphicsRootDescriptorTable (a descriptor table must point into a shader-visible
			//! heap, never directly into the CPU-only texture heap).
			ComPtr<ID3D12DescriptorHeap> ShaderVisibleSRVHeap;
			D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleSRVHeapStartCPU = {};
			D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleSRVHeapStartGPU = {};
			UINT ShaderVisibleSRVNext = 0;
			//! Current capacity of ShaderVisibleSRVHeap (may exceed ShaderVisibleSRVCapacityPerFrame
			//! after growing -- see CD3D12Driver::growShaderVisibleSRVHeap()); initialized to
			//! ShaderVisibleSRVCapacityPerFrame in createFrameDrawResources().
			UINT ShaderVisibleSRVCapacity = 0;

			//! Vertex ring (upload heap, mapped once) for geometry generated on the fly by the
			//! immediate 2D/3D draw functions (draw2DImage, draw3DLine, draw2DVertexPrimitiveList...)
			//! -- distinct from ConstantRing (no 256-byte alignment requirement for a vertex buffer)
			//! and from the persistent IHardwareBuffer objects used by drawMeshBuffer(). Same rule:
			//! cursor reset every beginScene().
			ComPtr<ID3D12Resource> VertexRing;
			void* VertexRingMapped = nullptr;
			UINT64 VertexRingOffset = 0;
			//! Current capacity of VertexRing (may exceed VertexRingSizePerFrame after growing -- see
			//! CD3D12Driver::growVertexRing()); initialized to VertexRingSizePerFrame in
			//! createFrameDrawResources().
			UINT64 VertexRingCapacity = 0;
		};

		//! Depth/stencil buffer dedicated to a render-target-texture (see
		//! CD3D12Driver::checkRTTDepthBuffer()). A default-heap resource + its DSV, indexed in the
		//! pool by size -- unlike SD3D12FrameContext, this resource is NOT multi-buffered per frame
		//! (same reasoning as the driver's DepthStencilBuffer: fully replayed on each use, never read
		//! by a previous frame in parallel).
		struct SD3D12RTTDepthBuffer
		{
			core::dimension2d<u32> Size;
			//! The pool is keyed on (Size, SampleCount): a depth buffer paired with an MSAA RTV must
			//! have the same SampleDesc.Count as it (D3D12 rejects the draw otherwise), so a
			//! single-sample depth buffer already cached for a given size cannot be reused once an
			//! MSAA RTT of the same size appears.
			UINT SampleCount = 1;
			ComPtr<ID3D12Resource> Buffer;
			CD3DX12_CPU_DESCRIPTOR_HANDLE DSVHandle;
			UINT DSVHeapIndex = 0;
		};

		//! Initial size of the per-frame constant ring (1 MB => ~4096 CBVs at 256-byte alignment,
		//! comfortably enough for a reasonable number of drawMeshBuffer() calls per frame). Just a
		//! starting capacity (SD3D12FrameContext::ConstantRingCapacity): beyond it, allocateConstant()
		//! grows the ring on demand via growConstantRing() instead of failing.
		//! Number of SRV/sampler descriptors (t0..t8 / s0..s8) the shared root signature exposes for
		//! every draw call. Sized from OuterSpace's actual shader catalog (x64\Data\config\Shaders.xml):
		//! the most texture-hungry shader observed (Atmosphere/Fluid) declares up to 9 t0-t8 registers.
		//! A register not declared by a given shader (a built-in with 1-2 textures, or a user shader
		//! with gaps such as GasGiant skipping t2) is still covered by the table -- see
		//! allocateSRVTableSlot()/allocateSamplerTableSlot(), which always fill all 9 slots
		//! (NullTexture/default sampler for unset registers). This makes non-contiguous registers
		//! transparent without needing to know precisely which registers a given shader uses: D3D12
		//! only requires the root signature to be a superset of the registers a shader actually
		//! declares, not an exact match.
		//! Single source of truth: MATERIAL_MAX_TEXTURES (SMaterial.h, = _IRR_MATERIAL_MAX_TEXTURES_ in
		//! IrrCompileConfig.h), the same budget CD3D11Driver already uses to size its
		//! CurrentTexture[]/SamplerDesc[] arrays and that SMaterial::TextureLayer[] uses. A user shader
		//! reflecting a register beyond a smaller, disconnected value (e.g. the water shader, which
		//! goes up to s11 for triplanar normal mapping) would otherwise make CreateGraphicsPipelineState
		//! fail silently.
		static const UINT MaxUserShaderTextureSlots = MATERIAL_MAX_TEXTURES;
		static const UINT64 ConstantRingSizePerFrame = 1 * 1024 * 1024;
		//! Initial capacity of the per-frame shader-visible SRV table (one group of
		//! MaxUserShaderTextureSlots entries per drawMeshBuffer() call, plus the user CBV tables --
		//! see allocateUserCBVTable()). Just a starting capacity
		//! (SD3D12FrameContext::ShaderVisibleSRVCapacity): beyond it, allocateSRVTableSlot()/
		//! allocateUserCBVTable()/allocateDescriptorTableSlot() grow the heap on demand via
		//! growShaderVisibleSRVHeap() instead of failing.
		static const UINT ShaderVisibleSRVCapacityPerFrame = 16384;
		//! Capacity of the shader-visible sampler heap, distinct from the "one slot per draw" scheme
		//! of the SRV heap above. Unlike textures (potentially hundreds, one per draw), the number of
		//! distinct filter/addressing combinations used by a scene stays small (deduplicated via
		//! SamplerCache): this heap is therefore persistent (created once, never reset per frame) --
		//! each unique combination gets a fixed slot the first time it is encountered.
		//! A hard hardware limit exists regardless (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE = 2048):
		//! a "one slot per draw" scheme like the SRV heap's would overflow at the 2049th draw call of
		//! a frame.
		//! Initial capacity (CD3D12Driver::ShaderVisibleSamplerHeapCapacity exceeds it once a growth
		//! happens -- see growShaderVisibleSamplerHeap()); still capped by
		//! D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE above, which is a real hardware limit.
		// Targets ~64 deduplicated combinations (SamplerCombinationCache) before growth, regardless of
		// MaxUserShaderTextureSlots -- always well below D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE
		// (2048): with MaxUserShaderTextureSlots = MATERIAL_MAX_TEXTURES = 16, 64 x 16 = 1024.
		static const UINT ShaderVisibleSamplerCapacity = MaxUserShaderTextureSlots * 64;
		//! Initial size of the per-frame vertex ring (2 MB -- immediate 2D/3D drawing only, not
		//! persistent mesh geometry, which goes through CD3D12HardwareBuffer). Just a starting
		//! capacity (SD3D12FrameContext::VertexRingCapacity): beyond it, allocateVertices() grows the
		//! ring on demand via growVertexRing() instead of failing.
		static const UINT64 VertexRingSizePerFrame = 2 * 1024 * 1024;

		//! Native D3D12 driver, distinct from CD3D11On12Driver (the existing D3D11-on-12 interop
		//! implementation). Derives from CNullDriver exactly like CD3D11Driver: the refcounted
		//! texture cache, material renderer registry, image loaders/writers, mesh manipulator,
		//! vertex descriptors and driver attributes all come from the base class -- only the D3D12
		//! specifics are redefined here. The single entry point to hook for textures is
		//! createDeviceDependentTexture() (see below).
		class CD3D12Driver : public CNullDriver, public IMaterialRendererServices
		{
		public:
			// CD3D12DeferredContext shares the device/queue/root signature/default shaders with the
			// immediate driver and reads its current back buffer/depth-stencil at recording time --
			// this requires direct access to private members (same reasoning as
			// "friend class CD3D11DeferredContext;" in CD3D11Driver.h).
			friend class CD3D12DeferredContext;

			// A non-null `sharedResources` (CD3D12DeferredContext only) means this driver does not own
			// its own textures/heaps/upload pipeline -- it shares those of `sharedResources` (see
			// TextureCache/RTVHeap/.../ResourceOwner below). A deferred context is a command list
			// recorder, not an independent owner of GPU resources.
			CD3D12Driver(const irr::SIrrlichtCreationParameters& params,
				io::IFileSystem* io, HWND window, CD3D12Driver* sharedResources = nullptr);
			virtual ~CD3D12Driver();

			bool initDriver(HWND hwnd);

			// --- IVideoDriver: core scene lifecycle ---
			virtual bool beginScene(bool backBuffer = true, bool zBuffer = true,
				SColor color = SColor(255, 0, 0, 0),
				const SExposedVideoData& videoData = SExposedVideoData(),
				core::rect<s32>* sourceRect = 0) _IRR_OVERRIDE_;
			virtual bool endScene() _IRR_OVERRIDE_;
			virtual const wchar_t* getName() const _IRR_OVERRIDE_ { return Name.c_str(); }
			virtual E_DRIVER_TYPE getDriverType() const _IRR_OVERRIDE_ { return EDT_DIRECT3D12; } // native, distinct from EDT_DIRECT3D11ON12
			virtual void OnResize(const core::dimension2d<u32>& size) _IRR_OVERRIDE_;

			// --- IVideoDriver: vertex/index buffers ---
			virtual std::shared_ptr<video::IHardwareBuffer> createHardwareBuffer(scene::IIndexBuffer* indexBuffer) _IRR_OVERRIDE_;
			virtual std::shared_ptr<video::IHardwareBuffer> createHardwareBuffer(scene::IVertexBuffer* vertexBuffer) _IRR_OVERRIDE_;

			// --- IVideoDriver: compute buffers ---
			// Default (VRAM) resource with both UAV and SRV views created (see CD3D12HardwareBuffer.h):
			// dispatchComputeShader() decides at call time which of the two buffers (Src/Dst) plays
			// which role, same as the D3D11 side.
			virtual std::shared_ptr<video::IHardwareBuffer> createHardwareBuffer(scene::IComputeBuffer* computeBuffer) _IRR_OVERRIDE_;

			//! Active compute material = MaterialRenderers[Material.MaterialType] (see setMaterial(),
			//! same state as the graphics pipeline -- no separate state for compute). Creates/
			//! refreshes the Src/Dst CD3D12HardwareBuffer objects if needed, transitions them to
			//! NON_PIXEL_SHADER_RESOURCE/UNORDERED_ACCESS (see CD3D12HardwareBuffer::transitionTo()),
			//! binds the compute PSO (getOrCreateComputePSO()) plus the SRV t0/UAV u0/user CBV b0..b7
			//! tables (see createComputeRootSignature()), then Dispatch()es. No-op (with a logged
			//! warning/error) if Src/Dst is null/empty or no compute material is active.
			virtual void dispatchComputeShader(const core::vector3d<u32>& groupCount,
				scene::IComputeBuffer* Src, scene::IComputeBuffer* Dst) _IRR_OVERRIDE_;

			// --- IVideoDriver: transformations and current material ---
			virtual void setTransform(E_TRANSFORMATION_STATE state, const core::matrix4& mat) _IRR_OVERRIDE_;
			virtual const core::matrix4& getTransform(E_TRANSFORMATION_STATE state) const _IRR_OVERRIDE_;
			virtual void setMaterial(const SMaterial& material) _IRR_OVERRIDE_;

			// --- IVideoDriver: drawing ---
			// drawMeshBuffer() supports multi-stream (up to D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
			// vertex buffers, IASetVertexBuffers across all slots) and instancing (EIDSR_PER_INSTANCE
			// buffer(s) via mb->getVertexDescriptor() -> DrawIndexedInstanced/DrawInstanced with
			// InstanceCount>1), same logic as CD3D11Driver::drawMeshBuffer()/renderArray().
			virtual void drawMeshBuffer(const scene::IMeshBuffer* mb) _IRR_OVERRIDE_;
			virtual void drawMeshBufferNormals(const scene::IMeshBuffer* mb, f32 length = 10.f, SColor color = 0xffffffff) _IRR_OVERRIDE_;

			// --- IVideoDriver: render target management ---
			virtual const core::dimension2d<u32>& getScreenSize() const _IRR_OVERRIDE_ { return WindowSize; }
			virtual const core::dimension2d<u32>& getCurrentRenderTargetSize() const _IRR_OVERRIDE_ { return CurrentRenderTargetSize; }
			virtual ECOLOR_FORMAT getColorFormat() const _IRR_OVERRIDE_ { return ECF_A8R8G8B8; }
			virtual void clearZBuffer() _IRR_OVERRIDE_;
			virtual bool setRenderTarget(video::ITexture* texture, bool clearBackBuffer = true,
				bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0),
				video::ITexture* depthStencil = 0) _IRR_OVERRIDE_;
			virtual bool setRenderTarget(const core::array<video::IRenderTarget>& texture,
				const core::array<bool>& clearBackBuffer, bool clearZBuffer = true,
				SColor color = video::SColor(0, 0, 0, 0), video::ITexture* depthStencil = 0) _IRR_OVERRIDE_;
			virtual bool setRenderTarget(E_RENDER_TARGET target, bool clearTarget = true,
				bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0)) _IRR_OVERRIDE_;

			// --- IVideoDriver: 2D drawing ---
			// All of these funnel through drawImmediate2D() (see the private section): same pipeline
			// as drawMeshBuffer() (default root signature/PSO/shader), but geometry generated on the
			// fly into the frame's vertex ring (VertexRing) instead of a persistent IHardwareBuffer,
			// with a pixel-perfect orthographic projection (World=Identity, View=Identity, Proj=ortho
			// screen -- same formula as CD3D11Driver::draw2DImage, see build2DProjection()).
			virtual void draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos) _IRR_OVERRIDE_;
			virtual void draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos,
				const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect = 0,
				SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false) _IRR_OVERRIDE_;
			virtual void draw2DImage(const video::ITexture* texture, const core::rect<s32>& destRect,
				const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect = 0,
				const video::SColor* const colors = 0, bool useAlphaChannelOfTexture = false) _IRR_OVERRIDE_;
			virtual void draw2DImageBatch(const video::ITexture* texture,
				const core::position2d<s32>& pos,
				const core::array<core::rect<s32> >& sourceRects,
				const core::array<s32>& indices,
				s32 kerningWidth = 0,
				const core::rect<s32>* clipRect = 0,
				SColor color = SColor(255, 255, 255, 255),
				bool useAlphaChannelOfTexture = false) _IRR_OVERRIDE_;
			virtual void draw2DImageBatch(const video::ITexture* texture,
				const core::array<core::position2d<s32> >& positions,
				const core::array<core::rect<s32> >& sourceRects,
				const core::rect<s32>* clipRect = 0,
				SColor color = SColor(255, 255, 255, 255),
				bool useAlphaChannelOfTexture = false) _IRR_OVERRIDE_;
			virtual void draw2DRectangle(SColor color, const core::rect<s32>& pos,
				const core::rect<s32>* clip = 0) _IRR_OVERRIDE_;
			virtual void draw2DRectangle(const core::rect<s32>& pos,
				SColor colorLeftUp, SColor colorRightUp,
				SColor colorLeftDown, SColor colorRightDown,
				const core::rect<s32>* clip = 0) _IRR_OVERRIDE_;
			virtual void batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
				const irr::core::array < SColor>& color,
				const irr::core::array <core::rect<s32>>* clip = 0) _IRR_OVERRIDE_;
			virtual void batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
				irr::core::array < SColor>& colorLeftUp, irr::core::array < SColor>& colorRightUp,
				irr::core::array < SColor>& colorLeftDown, irr::core::array < SColor>& colorRightDown,
				const irr::core::array <core::rect<s32>>* clip = 0) _IRR_OVERRIDE_;
			virtual void draw2DRectangleOutline(const core::recti& pos,
				SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;
			virtual void draw2DLine(const core::position2d<s32>& start,
				const core::position2d<s32>& end,
				SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;
			virtual void drawPixel(u32 x, u32 y, const SColor& color) _IRR_OVERRIDE_;
			virtual void draw2DPolygon(core::position2d<s32> center,
				f32 radius,
				video::SColor color = SColor(100, 255, 255, 255),
				s32 vertexCount = 10) _IRR_OVERRIDE_;
			virtual void draw2DVertexPrimitiveList(const void* vertices, u32 vertexCount,
				const void* indexList, u32 primCount,
				E_VERTEX_TYPE vType = EVT_STANDARD,
				scene::E_PRIMITIVE_TYPE pType = scene::EPT_TRIANGLES,
				E_INDEX_TYPE iType = EIT_16BIT) _IRR_OVERRIDE_;

			// --- IVideoDriver: immediate 3D drawing ---
			// Same mechanism as 2D drawing (drawImmediate3D()), but World/View/Proj come from the
			// current transform state (Matrices[]) rather than an orthographic projection -- documented
			// contract ("drawn using the current transformation matrix and material"; the caller is
			// expected to have set ETS_WORLD if necessary).
			virtual void draw3DLine(const core::vector3df& start,
				const core::vector3df& end, SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;
			virtual void draw3DTriangle(const core::triangle3df& triangle,
				SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;
			virtual void draw3DBox(const core::aabbox3d<f32>& box,
				SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;

			// --- IVideoDriver: stencil shadow volumes ---
			// Requires DepthStencilFormat == D24_UNORM_S8_UINT (see below): a stencil plane must exist.
			virtual void drawStencilShadowVolume(const core::array<core::vector3df>& triangles,
				bool zfail = true, u32 debugDataVisible = 0) _IRR_OVERRIDE_;
			virtual void drawStencilShadow(bool clearStencilBuffer = false,
				video::SColor leftUpEdge = video::SColor(255, 0, 0, 0),
				video::SColor rightUpEdge = video::SColor(255, 0, 0, 0),
				video::SColor leftDownEdge = video::SColor(255, 0, 0, 0),
				video::SColor rightDownEdge = video::SColor(255, 0, 0, 0)) _IRR_OVERRIDE_;

			// --- IVideoDriver: occlusion queries ---
			// D3D12_QUERY_TYPE_BINARY_OCCLUSION (visible/not visible, cheaper and sufficient for
			// getOcclusionQueryResult(), which only promises an approximation) via a single
			// ID3D12QueryHeap + a READBACK resource, indexed by a slot allocated per node (see
			// OcclusionQueryMap).
			virtual void addOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node,
				const scene::IMesh* mesh = 0) _IRR_OVERRIDE_;
			virtual void removeOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node) _IRR_OVERRIDE_;
			virtual void removeAllOcclusionQueries() _IRR_OVERRIDE_;
			virtual void runOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool visible = false) _IRR_OVERRIDE_;
			virtual void runAllOcclusionQueries(bool visible = false) _IRR_OVERRIDE_;
			virtual void updateOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool block = true) _IRR_OVERRIDE_;
			virtual void updateAllOcclusionQueries(bool block = true) _IRR_OVERRIDE_;
			virtual u32 getOcclusionQueryResult(std::shared_ptr<scene::ISceneNode> node) const _IRR_OVERRIDE_;

			// --- IVideoDriver: screenshot capture ---
			// Copies the back buffer that was actually presented on the last endScene() (see
			// LastPresentedFrameIndex below, distinct from CurrentFrameIndex, which already points at
			// the next back buffer to draw once endScene() has returned) into a READBACK resource, same
			// scheme as CD3D12Texture::lock(ETLM_READ_ONLY). Only ECF_A8R8G8B8/ERT_FRAME_BUFFER are
			// supported; anything else returns nullptr with a warning rather than a truncated/wrong image.
			virtual IImage* createScreenShot(video::ECOLOR_FORMAT format = video::ECF_UNKNOWN, video::E_RENDER_TARGET target = video::ERT_FRAME_BUFFER) _IRR_OVERRIDE_;

			// --- IVideoDriver: D3D12-specific only. Everything not listed here (image loaders/
			// writers, createImage*/createImageFromFile/writeImageToFile, the name-based texture cache
			// -- getTexture()/findTexture()/renameTexture()/getTextureCount()/getTextureByIndex()/
			// removeTexture()/removeAllTextures(), makeColorKeyTexture()/makeNormalMapTexture(), texture
			// creation flags, dynamic lights, vertex descriptors, driver attributes, FPS/primitive
			// counters) is inherited as-is from CNullDriver, same as CD3D11Driver -- the only hook
			// needed is createDeviceDependentTexture() below.
			virtual bool queryFeature(E_VIDEO_DRIVER_FEATURE feature) const _IRR_OVERRIDE_;
			virtual bool checkDriverReset() _IRR_OVERRIDE_ { return false; }
			//! Geometry shader stream-output: the buffer must have been created/be recreatable as
			//! EHM_STATIC (the default-heap path of CD3D12HardwareBuffer, the only one that supports
			//! transitioning to D3D12_RESOURCE_STATE_STREAM_OUT -- an EHM_DYNAMIC/EHM_STREAM upload-heap
			//! buffer stays fixed in GENERIC_READ). buffer == nullptr detaches the current target
			//! (SOSetTargets(0, 0, nullptr)) and transitions it back to VERTEX_AND_CONSTANT_BUFFER so a
			//! later draw can read it as a normal vertex buffer (see CD3D12MaterialRenderer.h for the
			//! rest of the GS support).
			virtual bool setStreamOutputBuffer(scene::IVertexBuffer* buffer) _IRR_OVERRIDE_;
			//! The registry itself (name + IMaterialRenderer*, grab/drop) is CNullDriver::
			//! MaterialRenderers -- only the D3D12 counterpart of each entry (CD3D12MaterialRenderer*,
			//! carrying the blobs/reflection getPSOForMaterial() needs) is stored alongside, in
			//! NativeRenderers, at the same index. addMaterialRenderer() also accepts a "foreign"
			//! IMaterialRenderer (not built by this driver): it is registered/queryable normally, but
			//! its NativeRenderers entry stays null and this driver cannot derive a PSO from it.
			virtual s32 addMaterialRenderer(IMaterialRenderer* renderer, const c8* name = 0) _IRR_OVERRIDE_;
			virtual IGPUProgrammingServices* getGPUProgrammingServices() _IRR_OVERRIDE_ { return this; }
			//! User-defined clip planes. Stored CPU-side (ClipPlanes/ClipPlaneEnabled) and evaluated in
			//! the pixel shader (clip() on dot(WorldPos, plane), see CD3D12DefaultShaders.h and
			//! bindTransformsAndTexture()) -- D3D12 has no fixed-function equivalent, unlike D3D9/OpenGL.
			virtual bool setClipPlane(u32 index, const core::plane3df& plane, bool enable = false) _IRR_OVERRIDE_;
			virtual void enableClipPlane(u32 index, bool enable) _IRR_OVERRIDE_;
			//! Not part of IVideoDriver (see CD3D11Driver::getClipPlane, same reasoning: used by tests /
			//! a future CD3D12MaterialRenderer rather than by the engine).
			//! Inline (like getDevice()/getRootSignature() below): a direct call on a concrete
			//! CD3D12Driver* (outside IVideoDriver virtuals) from another module (OuterSpaceTest) would
			//! otherwise require a DLL export this class does not have.
			void getClipPlane(u32 index, core::plane3df& plane, bool& enable) const
			{
				if (index > 2)
				{
					enable = false;
					return;
				}
				plane = ClipPlanes[index];
				enable = ClipPlaneEnabled[index];
			}
			//! Number of quality levels available for a format at numSamples samples
			//! (D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS), 0 if numSamples isn't supported for that
			//! format -- same contract/signature as CD3D11Driver::queryMultisampleLevels(). Not part of
			//! IVideoDriver (like getClipPlane() just above): typical caller is CD3D12DriverTests, which
			//! has access to the concrete type. virtual (not _IRR_OVERRIDE_, nothing to override) to
			//! stay consistent with CD3D11Driver.h and let a future derived driver override it.
			virtual u32 queryMultisampleLevels(ECOLOR_FORMAT format, u32 numSamples) const;
			virtual core::stringc getVendorInfo() _IRR_OVERRIDE_ { return "D3D12 (native)"; }
			virtual core::dimension2du getMaxTextureSize() const _IRR_OVERRIDE_ { return core::dimension2du(16384, 16384); } // D3D_FEATURE_LEVEL_11_0 limit
			//! 8 == MAX_LIGHTS on the shader side (CD3D12DefaultShaders.h/bindLighting()) -- the
			//! CNullDriver default (0) would be misleading now that dynamic lighting actually works.
			//! Storage (CNullDriver::Lights) and the other five accessors belong to the base class.
			virtual u32 getMaximalDynamicLightAmount() const _IRR_OVERRIDE_ { return 8; }
			virtual u32 getMaximalPrimitiveCount() const _IRR_OVERRIDE_ { return 0xFFFFFFFF; }
			// setFog()/getFog() (stored in FogColor/FogType/...), getFPS() and getPrimitiveCountDrawn()
			// belong to CNullDriver -- bindFog() (CD3D12Driver.cpp) simply reads those same fields to
			// send them to the GPU.

			// --- IGPUProgrammingServices: user shader plugin system. CD3D12Driver returns itself via
			// getGPUProgrammingServices() above. HLSL only (D3DCompile/D3DReflect, see
			// CD3D12Driver.cpp) -- no geometry/hull/domain shader (already rejected by the root
			// signature, see createRootSignature()): any non-null GS/HS/DS parameter is ignored with a
			// warning rather than failing creation. See the CD3D12MaterialRenderer comment
			// (CD3D12Driver.h) for its limitations (one cbuffer per stage, same t0/s0 as built-in
			// materials).
			virtual s32 addHighLevelShaderMaterial(
				const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget, const c8* pixelShaderProgram,
				const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
				const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES, scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) _IRR_OVERRIDE_;
			virtual s32 addHighLevelShaderMaterial(
				const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName = 0,
				E_VERTEX_SHADER_TYPE vsCompileTarget = EVST_VS_1_1, const c8* pixelShaderProgram = 0,
				const c8* pixelShaderEntryPointName = 0, E_PIXEL_SHADER_TYPE psCompileTarget = EPST_PS_1_1,
				const c8* geometryShaderProgram = 0, const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				const c8* hullShaderProgram = 0, const c8* hullShaderEntryPointName = "main",
				E_HULL_SHADER_TYPE hsCompileTarget = EHST_HS_5_0,
				const c8* domainShaderProgram = 0, const c8* domainShaderEntryPointName = "main",
				E_DOMAIN_SHADER_TYPE dsCompileTarget = EDST_DS_5_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES, scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) _IRR_OVERRIDE_;
			virtual s32 addHighLevelShaderMaterialFromFiles(
				const io::path& vertexShaderProgramFileName, const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget, const io::path& pixelShaderProgramFileName,
				const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
				const io::path& geometryShaderProgramFileName, const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES, scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) _IRR_OVERRIDE_;
			virtual s32 addHighLevelShaderMaterialFromFiles(
				const io::path& vertexShaderProgramFile, const c8* vertexShaderEntryPointName = "main",
				E_VERTEX_SHADER_TYPE vsCompileTarget = EVST_VS_1_1, const io::path& pixelShaderProgramFile = "",
				const c8* pixelShaderEntryPointName = "main", E_PIXEL_SHADER_TYPE psCompileTarget = EPST_PS_1_1,
				const io::path& geometryShaderProgramFileName = "", const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				const io::path& hullShaderProgram = "", const c8* hullShaderEntryPointName = "main",
				E_HULL_SHADER_TYPE hsCompileTarget = EHST_HS_5_0,
				const io::path& domainShaderProgram = "", const c8* domainShaderEntryPointName = "main",
				E_DOMAIN_SHADER_TYPE dsCompileTarget = EDST_DS_5_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES, scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) _IRR_OVERRIDE_;
			virtual s32 addHighLevelShaderMaterialFromFiles(
				io::IReadFile* vertexShaderProgram, const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget, io::IReadFile* pixelShaderProgram,
				const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
				io::IReadFile* geometryShaderProgram, const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES, scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) _IRR_OVERRIDE_;
			virtual s32 addHighLevelShaderMaterialFromFiles(
				io::IReadFile* vertexShaderProgram, const c8* vertexShaderEntryPointName = "main",
				E_VERTEX_SHADER_TYPE vsCompileTarget = EVST_VS_1_1, io::IReadFile* pixelShaderProgram = 0,
				const c8* pixelShaderEntryPointName = "main", E_PIXEL_SHADER_TYPE psCompileTarget = EPST_PS_1_1,
				io::IReadFile* geometryShaderProgram = 0, const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				io::IReadFile* hullShaderProgram = 0, const c8* hullShaderEntryPointName = "main",
				E_HULL_SHADER_TYPE hsCompileTarget = EHST_HS_5_0,
				io::IReadFile* domainShaderProgram = 0, const c8* domainShaderEntryPointName = "main",
				E_DOMAIN_SHADER_TYPE dsCompileTarget = EDST_DS_5_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES, scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) _IRR_OVERRIDE_;
			//! No assembly shaders (vs_1_1/ps_1_1 etc.) under D3D12 -- unsupported stub, unlike
			//! addHighLevelShaderMaterial* above (see CD3D12Driver.cpp).
			virtual s32 addShaderMaterial(const c8* vertexShaderProgram = 0, const c8* pixelShaderProgram = 0,
				IShaderConstantSetCallBack* callback = 0, E_MATERIAL_TYPE baseMaterial = EMT_SOLID, s32 userData = 0) _IRR_OVERRIDE_ { return -1; }
			virtual s32 addShaderMaterialFromFiles(const io::path& vertexShaderProgramFileName, const io::path& pixelShaderProgramFileName,
				IShaderConstantSetCallBack* callback = 0, E_MATERIAL_TYPE baseMaterial = EMT_SOLID, s32 userData = 0) _IRR_OVERRIDE_ { return -1; }
			virtual s32 addShaderMaterialFromFiles(io::IReadFile* vertexShaderProgram, io::IReadFile* pixelShaderProgram,
				IShaderConstantSetCallBack* callback = 0, E_MATERIAL_TYPE baseMaterial = EMT_SOLID, s32 userData = 0) _IRR_OVERRIDE_ { return -1; }
			//! Compiles and reflects via CD3D12MaterialRenderer::compileComputeFromHLSL() and registers
			//! in the same MaterialRenderers registry as drawing materials (see addMaterialRenderer())
			//! -- a compute shader is selected like any other material, via
			//! setMaterial({MaterialType = this return value}) before dispatchComputeShader(), same
			//! convention as CD3D11Driver::setComputeState().
			virtual s32 addComputeShader(const c8* computeShaderProgram = 0, const c8* pixelShaderEntryPointName = 0,
				E_COMPUTE_SHADER_TYPE csCompileTarget = ECST_CS_4_0, IShaderConstantSetCallBack* callback = 0, s32 userData = 0) _IRR_OVERRIDE_;
			virtual s32 addComputeShaderFromFile(const io::path& computeShaderProgramFileName, const c8* pixelShaderEntryPointName = 0,
				E_COMPUTE_SHADER_TYPE csCompileTarget = ECST_CS_4_0, IShaderConstantSetCallBack* callback = 0, s32 userData = 0) _IRR_OVERRIDE_;

			// --- IVideoDriver: textures ---
			//! Single texture hook on CNullDriver (same role as
			//! CD3D11Driver::createDeviceDependentTexture()): addTexture(size/name/format),
			//! addTexture(name, IImage*), getTexture(file) and the whole refcounted cache
			//! (Textures/textureArrayLock, grabbed on insertion, dropped on removeTexture()) belong to
			//! the base class and all funnel through here to build the GPU resource. Returns nullptr if
			//! the D3D12 resource could not be created -- CNullDriver then propagates the nullptr to the
			//! caller instead of caching a half-built texture.
			virtual ITexture* createDeviceDependentTexture(IImage* surface, const io::path& name, void* mipmapData = 0) _IRR_OVERRIDE_;
			virtual ITexture* createDeviceDependentTexture(const core::array<ITexture*>& surfaces,
				const E_TEXTURE_TYPE Type, const io::path& name, void* mipmapData = 0) _IRR_OVERRIDE_;

			//! Declaring one addTexture() overload hides all the base class's others for name
			//! resolution on a CD3D12Driver* (name hiding) -- re-expose them.
			using CNullDriver::addTexture;
			//! Same as CNullDriver::addTexture(), minus the IImage::isRenderTargetOnlyFormat() guard --
			//! see CD3D12Driver.cpp for why it doesn't apply to this driver.
			virtual ITexture* addTexture(const core::dimension2d<u32>& size, const io::path& name,
				ECOLOR_FORMAT format = ECF_A8R8G8B8) _IRR_OVERRIDE_;
			virtual ITexture* addRenderTargetTexture(const core::dimension2d<u32>& size,
				const io::path& name = "rt", const ECOLOR_FORMAT format = ECF_UNKNOWN) _IRR_OVERRIDE_;
			//! arraySlices > 1 creates a true render-target-texture array (see CD3D12Texture.h) -- a
			//! geometry shader writing SV_RenderTargetArrayIndex can route each primitive to its own
			//! slice in a single draw. Combining sampleCount > 1 with an array is rejected (warning +
			//! fallback to arraySlices=1, see CD3D12Driver.cpp) -- MSAA+array is out of scope, same
			//! choice as CD3D11Texture. sampleCount<=1, arraySlices<=1 reproduces the overload above
			//! exactly.
			virtual ITexture* addRenderTargetTexture(const core::dimension2d<u32>& size,
				const io::path& name, const ECOLOR_FORMAT format,
				u32 sampleCount, u32 sampleQuality, u32 arraySlices) _IRR_OVERRIDE_;

			//! 2D array / cube / cube array / 3D texture, built from N already-loaded 2D images (one
			//! per slice/face/Z-layer). Complements getTexture(files[], Type) (inherited from
			//! CNullDriver, which loads each file then goes through
			//! createDeviceDependentTexture(array<ITexture*>&, ...) above): here the caller already has
			//! its IImages in hand and has no files to read. ETT_CUBE requires exactly 6 images,
			//! ETT_CUBE_ARRAY a multiple of 6; ETT_3D_ARRAY is accepted but treated as ETT_3D (see
			//! CD3D12Texture.h -- no D3D API supports an array of 3D textures).
			//! Virtual despite being outside IVideoDriver: CD3D12Driver isn't exported via a class
			//! dllexport macro, so a direct call from OuterSpaceTest to a non-virtual method defined
			//! out-of-line (CD3D12Driver.cpp) would be an unresolved symbol at link time -- calling
			//! through the vtable of a virtual method needs no export (same reasoning as
			//! setClipPlane()/addTexture() above, which also call unexported things like
			//! os::Printer::log() or CD3D12Texture's constructor). getClipPlane() just above gets away
			//! without this (inline, trivial body) because it calls nothing like that.
			virtual ITexture* addTextureArray(const core::array<IImage*>& images, E_TEXTURE_TYPE type, const io::path& name);

			// TODO: createHardwareBuffer(IComputeBuffer*) is done (default-heap buffers + UAV/SRV
			//       views) but dispatchComputeShader() remains a stub (no compute root
			//       signature/PSO, no shader-visible heap -- see the TODO next to its declaration).
			// --- IVideoDriver: deferred context (see CD3D12DeferredContext.h) ---
			// Reuses the existing generic IDeferredContext rather than a lighter D3D12-specific
			// interface -- keeps a uniform driver->createDeferredContext()/executeDeferredContext() API
			// across all backends (D3D11/D3D12/generic). CD3D12Driver itself does NOT implement
			// IDeferredContext (unlike CD3D11Driver, which does for its older, now-legacy Option-A
			// path): only CD3D12DeferredContext (a dedicated subclass) implements it, so
			// getDeferredContextControl() keeps IVideoDriver's default body (returns nullptr) without
			// needing an override here.
			virtual IVideoDriver* createDeferredContext() _IRR_OVERRIDE_;
			virtual void executeDeferredContext(IDeferredContext* context) _IRR_OVERRIDE_;

			//! Issues a resource barrier if necessary (no-op if the state doesn't change) on the
			//! current frame's command list, and updates CurrentState on the texture.
			//! Will take an explicit command list once deferred contexts can record on multiple lists
			//! in parallel -- for now only one list exists at a time (the current frame's), so there's
			//! no ambiguity.
			void transitionTexture(CD3D12Texture* texture, D3D12_RESOURCE_STATES newState);

			//! Synchronous upload context: for resources created OUTSIDE a beginScene()/endScene() pair
			//! (texture loading at startup, streaming...), CommandList (owned by the current frame)
			//! cannot be reused. A dedicated allocator + command list, executed immediately and waited
			//! on via the same Fence as endScene().
			//! Synchronous by design (blocks the CPU until the transfer finishes) -- fine for startup
			//! loading, not for hitch-free hot streaming. A true asynchronous copy queue
			//! (D3D12_COMMAND_LIST_TYPE_COPY, on its own thread) would be the natural next step if
			//! profiling calls for it.
			//!
			//! ALWAYS go through UploadScope below, never call beginUpload()/endUploadAndWait() by hand.
			//!
			//! Two reasons:
			//!  - Concurrency. The upload pipeline (UploadAllocator/UploadCommandList/UploadFence) is a
			//!    single instance shared by the driver and all its deferred contexts, but the engine
			//!    creates textures from multiple threads: CNullDriver::getTexture(files[],Type) loads
			//!    its slices via concurrency::parallel_for, and the game creates textures from PPL
			//!    tasks. Two concurrent Reset() calls on the same command list corrupt recording. D3D11
			//!    didn't have this problem (ID3D11Device is free-threaded and takes initial data
			//!    directly in CreateTexture2D(pInitialData), so no shared command list is involved) --
			//!    this is a D3D12-specific constraint.
			//!  - Early-return paths. Several callers `return` partway through recording on failure;
			//!    without RAII, UploadInProgress would stay true forever and every subsequent upload on
			//!    the driver would fail.
			class UploadScope
			{
			public:
				explicit UploadScope(CD3D12Driver* driver);
				~UploadScope();
				UploadScope(const UploadScope&) = delete;
				UploadScope& operator=(const UploadScope&) = delete;

				//! nullptr if the upload pipeline isn't available: record nothing.
				ID3D12GraphicsCommandList* commandList() const { return CmdList; }

				//! Closes, submits and waits for the GPU. Call explicitly when the rest of the code
				//! needs the transfer to be complete (the common case). Idempotent; the destructor
				//! handles it otherwise, including on an early return.
				void endAndWait();

			private:
				CD3D12Driver* Driver;
				std::unique_lock<std::mutex> Lock;
				ID3D12GraphicsCommandList* CmdList;
			};

		private:
			ID3D12GraphicsCommandList* beginUpload();
			void endUploadAndWait();
		public:

			//! Needed by CD3D12HardwareBuffer/CD3D12Texture to create their ID3D12Resource objects and
			//! allocate their descriptors.
			ID3D12Device2* getDevice() const { return Device.Get(); }
			CD3D12DescriptorHeapAllocator& getRTVHeap() { return RTVHeap; }
			CD3D12DescriptorHeapAllocator& getSRVHeap() { return CBVSRVUAVHeap; }
			//! Needed by CD3D12Texture for the DSV of a depth render-target-texture
			//! (addRenderTargetTexture(ECF_D32/D24S8/...)).
			CD3D12DescriptorHeapAllocator& getDSVHeap() { return DSVHeap; }

			// ======================= Deferred GPU resource destruction =======================
			//
			// The D3D12 rule this driver used to violate, causing the client to crash at startup.
			//
			// An ID3D12GraphicsCommandList holds NO reference on the resources recorded into it: keeping
			// them alive until the GPU has finished executing the list is the application's
			// responsibility. Releasing an ID3D12Resource still referenced by an in-flight command list
			// produces "OBJECT_DELETED_WHILE_STILL_IN_USE" from the debug layer, then
			// DXGI_ERROR_DEVICE_REMOVED -- after which everything fails, including subsequent
			// CreateCommittedResource calls, and the first resulting texture is one with no Resource, on
			// which lock() crashes.
			//
			// D3D11 didn't have this problem: its runtime takes its own internal reference on anything
			// bound/recorded into a context and only truly releases the object after the GPU is done
			// (automatic deferred destruction). That's why CNullDriver::removeTexture() -- which drops
			// immediately -- is perfectly correct on D3D11 and other drivers, and isn't here. The fix
			// therefore lives in ~CD3D12Texture/~CD3D12HardwareBuffer (the single choke point through
			// which ALL releases pass, regardless of origin: removeTexture(), removeAllTextures(), a
			// replaced RTT, a caller's drop()...), not in removeTexture(), which is just one path among
			// others.
			//
			// Retired resources are stamped with the next fence value to be signaled and released only
			// once the GPU has passed it (drainRetiredResources()).

			//! Takes ownership of the resource and releases it once the GPU is done using it.
			void retireResource(ComPtr<ID3D12Resource>&& resource);

			//! Same, for an ENTIRE descriptor heap (not just a slot) -- used when a per-frame
			//! shader-visible ring/heap or the persistent sampler heap needs to grow (see
			//! growConstantRing()/growShaderVisibleSRVHeap()/growShaderVisibleSamplerHeap() below): the
			//! old heap stays referenced by commands already recorded in this command list until the GPU
			//! has passed them, exactly like an ID3D12Resource.
			void retireResource(ComPtr<ID3D12DescriptorHeap>&& heap);

			//! Same, for a single descriptor slot: reassigning it immediately would let an in-flight
			//! command list read a descriptor already overwritten by another texture.
			void retireDescriptor(CD3D12DescriptorHeapAllocator& heap, UINT index);

			//! Submits everything recorded since beginScene() and waits for the GPU to execute it, then
			//! reopens the command list, restoring its state (shader-visible heaps, render targets,
			//! viewport, scissor, stream-output).
			//!
			//! Call before any CPU readback of a render's result. On the D3D11 immediate context this
			//! problem doesn't exist: by the time the caller reads a texture back, everything it "drew"
			//! into it has already gone to the card. Under D3D12 those draws are still sitting in the
			//! open, unsubmitted command list (endScene() hasn't run yet): a readback copy issued on the
			//! queue would race ahead of them and read the texture as it was BEFORE the render. This is
			//! what made the engine's screenshot capture unreadable.
			//!
			//! Expensive (full CPU/GPU synchronization): reserved for readbacks, not to be called every
			//! frame. No-op outside beginScene()/endScene() (nothing recorded to submit).
			void flushCommandList();

			//! Needed by CD3D12HardwareBuffer for its per frame-in-flight N-buffering of non-EHM_STATIC
			//! vertex/index buffers: each buffer writes into the current slot's resource rather than a
			//! single pointer shared across all frames (see the header comment of
			//! CD3D12HardwareBuffer.h on the tearing risk this fixes).
			UINT getCurrentFrameIndex() const { return CurrentFrameIndex; }
			UINT getFrameCount() const { return NativeFrameCount; }

			//! Generic root signature shared by all PSOs (see the header comment of CD3D12PSOCache.h
			//! for its exact layout) and the associated cache. Exposed so D3D12 material renderers can
			//! call CD3D12PSOCache::getOrCreate() directly.
			ID3D12RootSignature* getRootSignature() const { return RootSignature.Get(); }
			CD3D12PSOCache& getPSOCache() { return PSOCache; }

			//! Root signature/PSO/heap dedicated to pixel-shader-blit mipmap generation (see
			//! CD3D12Texture::generateMips() and the header comment of createMipGenPipeline()) --
			//! distinct from RootSignature/PSOCache above, which expect World/View/Proj/clip planes that
			//! this simple blit doesn't need.
			ID3D12RootSignature* getMipGenRootSignature() const { return MipGenRootSignature.Get(); }
			ID3D12PipelineState* getOrCreateMipGenPSO(DXGI_FORMAT rtvFormat);
			ID3D12DescriptorHeap* getMipGenSRVHeap() const { return MipGenSRVHeap.Get(); }
			D3D12_CPU_DESCRIPTOR_HANDLE getMipGenSRVHeapCPU() const { return MipGenSRVHeapCPU; }
			D3D12_GPU_DESCRIPTOR_HANDLE getMipGenSRVHeapGPU() const { return MipGenSRVHeapGPU; }

			// --- Material renderers + arbitrary input layout ---
			// Covers EMT_SOLID, EMT_TRANSPARENT_ALPHA_CHANNEL, EMT_TRANSPARENT_ADD_COLOR,
			// EMT_TRANSPARENT_VERTEX_ALPHA, EMT_TRANSPARENT_ALPHA_CHANNEL_REF and
			// EMT_ONETEXTURE_BLEND via D3D12DefaultShaderHLSL (see CD3D12DefaultShaders.h).
			// No IMaterialRenderer plugin path via addHighLevelShaderMaterial/addShaderMaterial --
			// drawMeshBuffer() calls getPSOForMaterial() directly with the current SMaterial rather
			// than going through the engine's standard material renderer system.
			// descriptor: the IVertexDescriptor of the scene::IMeshBuffer currently being drawn -- null
			// for any caller without a relevant mesh buffer (2D/immediate drawing, shadow volumes,
			// occlusion queries), which then falls back to kS3DVertexInputLayout via
			// resolveInputLayout(). The SMaterial->PSO shader mapping remains EVT_STANDARD-only (VSMain
			// only reads POSITION/NORMAL/COLOR/TEXCOORD0): a "2tcoords"/"tangents" descriptor produces a
			// correct input layout (extra TEXCOORD1/TANGENT/BINORMAL attributes are declared with the
			// right offset/format) but those extra attributes are still ignored by the default shader.
			//! primitiveType: the topology actually submitted to IASetPrimitiveTopology by the caller
			//! (drawMeshBuffer()) -- determines key.TopologyType (POINT/LINE/TRIANGLE), unless an HS is
			//! bound on the active renderer (forces PATCH, see the implementation). Defaults to
			//! EPT_TRIANGLES for callers without a mesh buffer (2D/immediate drawing, shadow volumes,
			//! occlusion queries).
			SPSOKey buildPSOKeyFromMaterial(const SMaterial& material, IVertexDescriptor* descriptor = nullptr,
				scene::E_PRIMITIVE_TYPE primitiveType = scene::EPT_TRIANGLES) const;
			ID3D12PipelineState* getPSOForMaterial(const SMaterial& material, IVertexDescriptor* descriptor = nullptr,
				scene::E_PRIMITIVE_TYPE primitiveType = scene::EPT_TRIANGLES);

			//! Converts a generic IVertexDescriptor (the same attributes used for the input layout, see
			//! kS3DVertexInputLayout) into a D3D12_SO_DECLARATION_ENTRY declaration plus per-slot
			//! strides, for the D3D12_STREAM_OUTPUT_DESC of a geometry-shader stream-output PSO (see
			//! getPSOForMaterial()) -- same calculation as CD3D11VertexDescriptor::rebuildOutput() on the
			//! D3D11 side (semantic name/index/component count derived from
			//! E_VERTEX_ATTRIBUTE_SEMANTIC/TYPE/COUNT), but generic (IVertexAttribute) rather than a
			//! dedicated D3D11 subclass, since D3D12 doesn't need a separate CD3D12VertexDescriptor
			//! object for this. Returns false (outEntries empty) if descriptor is null or has no
			//! attributes.
			static bool buildStreamOutputDeclaration(IVertexDescriptor* descriptor,
				std::vector<D3D12_SO_DECLARATION_ENTRY>& outEntries, std::vector<UINT>& outStrides);

			//! Input-layout equivalent of buildStreamOutputDeclaration() above -- see its implementation
			//! comment (CD3D12Driver.cpp) for why it's generic rather than a dedicated
			//! CD3D12VertexDescriptor subclass. Returns false (outElements empty) if descriptor is null
			//! or has no attributes.
			static bool buildInputLayoutDescription(IVertexDescriptor* descriptor,
				std::vector<D3D12_INPUT_ELEMENT_DESC>& outElements);

			//! Resolves descriptor to a D3D12_INPUT_ELEMENT_DESC[]/count, via
			//! buildInputLayoutDescription() (stored in storage, which must outlive the use of
			//! outElements) if descriptor is non-null and valid, otherwise kS3DVertexInputLayout. Single
			//! resolution point shared by buildPSOKeyFromMaterial()/getPSOForMaterial() so they always
			//! agree on the same layout for the same descriptor.
			void resolveInputLayout(IVertexDescriptor* descriptor,
				std::vector<D3D12_INPUT_ELEMENT_DESC>& storage,
				const D3D12_INPUT_ELEMENT_DESC*& outElements, UINT& outCount) const;

			// --- IMaterialRendererServices, the IShaderConstantSetCallBack side of the API
			// (see addHighLevelShaderMaterial() above). All the methods below only make sense during
			// the call to CallBack->OnSetConstants() from bindDrawState() (see CD3D12Driver.cpp):
			// ActiveUserShader points at the active user shader's registration there, nullptr otherwise.
			virtual void setBasicRenderStates(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates) _IRR_OVERRIDE_ {}
			virtual s32 getVertexShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual s32 getPixelShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual bool setVertexShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			virtual bool setVertexShaderConstant(s32 index, const s32* ints, int count) _IRR_OVERRIDE_;
			virtual bool setPixelShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			virtual bool setPixelShaderConstant(s32 index, const s32* ints, int count) _IRR_OVERRIDE_;
			//! GS-side counterparts of VS/PS above.
			virtual s32 getGeometryShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual bool setGeometryShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			//! Same, for HS/DS (same -1/false defaults as IMaterialRendererServices.h for CS, not yet
			//! overridden below).
			virtual s32 getHullShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual bool setHullShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			virtual s32 getDomainShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual bool setDomainShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			//! Same, for CS. ActiveMaterialRendererIndex (see dispatchComputeShader()) is updated
			//! before calling CallBack->OnSetConstants(), same convention as bindDrawState() for the
			//! graphics stages.
			virtual s32 getComputeShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual bool setComputeShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			//! Register-based path (assembly shaders): not applicable here, see addShaderMaterial().
			virtual void setVertexShaderConstant(const f32* data, s32 startRegister, s32 constantAmount = 1) _IRR_OVERRIDE_;
			virtual void setPixelShaderConstant(const f32* data, s32 startRegister, s32 constantAmount = 1) _IRR_OVERRIDE_;
			virtual IVideoDriver* getVideoDriver() _IRR_OVERRIDE_ { return this; }

		private:
			bool createDeviceAndQueue();
			bool createSwapChain(HWND hwnd, uint32_t width, uint32_t height);
			bool createDescriptorHeaps();
			void updateRenderTargetViews();
			bool createDepthStencilBuffer(UINT width, UINT height);
			//! Depth/stencil buffer dedicated to a render-target-texture, cached by size (same pooling
			//! strategy -- "one buffer per distinct size, reused across calls" -- as
			//! CD3D11Driver::DepthBuffers/checkDepthBuffer()). Created on demand from DSVHeap (see
			//! SD3D12RTTDepthBuffer below); never freed before the driver is destroyed (no
			//! removeDepthSurface() equivalent here: RTT sizes used by an application generally stay
			//! stable once the scene is loaded). Returns nullptr if DSVHeap is full or resource creation
			//! fails (error already logged) -- setRenderTarget() then simply disables depth-testing for
			//! that target (same fallback behavior as HasDepthStencilBuffer=false for the back buffer).
			//! sampleCount (default 1) selects/creates the pool entry matching that sample count -- see
			//! the SD3D12RTTDepthBuffer::SampleCount comment.
			SD3D12RTTDepthBuffer* checkRTTDepthBuffer(const core::dimension2d<u32>& size, UINT sampleCount = 1);
			bool createRootSignature();

			//! Compiles the mipmap-generation HLSL shader (D3D12MipGenShaderHLSL,
			//! CD3D12DefaultShaders.h -- full-screen triangle from SV_VertexID, bilinear sampling of the
			//! parent mip acting as an implicit 2x2 box filter) and creates its dedicated root signature
			//! (one SRV t0 table + a static linear/clamp sampler, pixel-shader visible only -- no
			//! b0/b1/b2: this blit has no transform or clip planes to apply), plus the 1-descriptor
			//! shader-visible SRV heap reused sequentially by CD3D12Texture::generateMips() (synchronous
			//! beginUpload()/endUploadAndWait() path, never two generations in flight at once). Called
			//! once from initDriver(), after createRootSignature(). Non-fatal on failure (like
			//! createStreamOutputResources()): only regenerateMipMapLevels()/texture loading with
			//! ETCF_CREATE_MIP_MAPS active are affected, not the rest of the driver.
			bool createMipGenPipeline();

			//! Registers the built-in CD3D12MaterialRenderer objects, one per E_MATERIAL_TYPE value,
			//! same order as the enum in EMaterialTypes.h (needed so the registration index
			//! (addMaterialRenderer()) matches MaterialType, same convention as
			//! CD3D11Driver::createMaterialRenderers()/CNullDriver::sBuiltInMaterialTypeNames). Each
			//! CD3D12MaterialRenderer compiles its own VS/PS (CD3D12MaterialRenderer::compileBuiltIn(),
			//! see CD3D12MaterialRenderer.h/.cpp) from D3D12DefaultShaderHLSL -- same division of
			//! responsibility as CD3D11Driver::createMaterialRenderers()/CD3D11MaterialRenderer (the
			//! renderer compiles, the driver just orchestrates which entry point/blend mode goes with
			//! which E_MATERIAL_TYPE). Called once from initDriver(), after createRootSignature().
			bool createBuiltInMaterialRenderers();

			//! Returns the CD3D12MaterialRenderer registered for material.MaterialType, or nullptr
			//! (index out of range, or a "foreign" renderer -- see SD3D12MaterialRendererEntry::Native).
			//! Factors out the same check made by
			//! choosePixelShaderForMaterial()/chooseVertexShaderForMaterial()/buildPSOKeyFromMaterial().
			const CD3D12MaterialRenderer* getNativeMaterialRenderer(const SMaterial& material) const;

			//! The VS/PS of the EMT_SOLID renderer (always registered first by
			//! createBuiltInMaterialRenderers(), see initDriver()) -- the fallback used wherever this
			//! driver needs a "base opaque shader":
			//! choosePixelShaderForMaterial()/chooseVertexShaderForMaterial() for a MaterialType that was
			//! never registered, buildShadowVolumeStencilKey()/getOrCreateAuxPSO()/drawStencilShadow()
			//! for the auxiliary shadow-volume PSOs (which have no relevant SMaterial). Returns nullptr
			//! before createBuiltInMaterialRenderers() has run (never happens in practice: initDriver()
			//! fails before anything else calls these functions).
			ID3DBlob* getSolidVertexShader() const;
			ID3DBlob* getSolidPixelShader() const;

			//! Chooses the pixel shader blob to use for this SMaterial -- read from the
			//! CD3D12MaterialRenderer registered for material.MaterialType (see MaterialRenderers),
			//! whether a built-in type (createBuiltInMaterialRenderers()) or a user shader
			//! (registerUserShaderMaterial()). Used both by buildPSOKeyFromMaterial() (to hash the right
			//! PS into the key) and getPSOForMaterial() (to compile the PSO with the right bytecode) --
			//! keeps the two in sync.
			//! A non-null descriptor with getID()==EVT_2TCOORDS switches to renderer->PS2TCoords when
			//! that renderer has one (the multi-texture materials), otherwise falls back silently to
			//! renderer->PS.
			ID3DBlob* choosePixelShaderForMaterial(const SMaterial& material, IVertexDescriptor* descriptor = nullptr) const;

			//! Vertex-shader counterpart of choosePixelShaderForMaterial() above; same
			//! descriptor->VS2TCoords switch.
			ID3DBlob* chooseVertexShaderForMaterial(const SMaterial& material, IVertexDescriptor* descriptor = nullptr) const;

			//! Shared registration path for addHighLevelShaderMaterial()/
			//! addHighLevelShaderMaterialFromFiles() (once the HLSL sources have been reduced to
			//! strings): builds a CD3D12MaterialRenderer, delegates compilation+reflection to it
			//! (CD3D12MaterialRenderer::compileFromHLSL(), see CD3D12MaterialRenderer.h/.cpp -- same
			//! division of responsibility as CD3D11Driver::addHighLevelShaderMaterial()/
			//! CD3D11MaterialRenderer), then registers it via addMaterialRenderer() -- returns the index
			//! obtained (the SMaterial::MaterialType to use), or -1 on a compilation/reflection failure
			//! (already logged). geometryShaderProgram is compiled/reflected, as are
			//! hullShaderProgram/domainShaderProgram (which must be supplied together -- see
			//! CD3D12MaterialRenderer::compileFromHLSL()). vertexTypeOut is only grabbed/stored
			//! (CD3D12MaterialRenderer::StreamOutputVertexType) if geometryShaderProgram is non-null.
			//! baseMaterial: the user shader inherits the blend state of the corresponding built-in
			//! material (see CD3D12MaterialRenderer::BaseMaterialType) -- same role as a
			//! CD3D11MaterialRenderer's BaseRenderer, whose OnSetMaterial() delegates blend states to the
			//! base renderer.
			s32 registerUserShaderMaterial(
				const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget,
				const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName,
				E_PIXEL_SHADER_TYPE psCompileTarget,
				const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName,
				E_GEOMETRY_SHADER_TYPE gsCompileTarget,
				const c8* hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
				const c8* domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
				IVertexDescriptor* vertexTypeOut,
				IShaderConstantSetCallBack* callback, E_MATERIAL_TYPE baseMaterial, s32 userData);

			//! Separate root signature for compute (no graphics state to share) -- SRV t0 table (Src
			//! buffer), UAV u0 table (Dst buffer), CBV b0..b7/space0 table (user cbuffers, same
			//! convention as MaxUserShaderCBVSlotsPerStage on the graphics side). Called once at init,
			//! like createRootSignature().
			bool createComputeRootSignature();

			//! Compute PSO cache, keyed by a hash of the CS blob pointer (same scheme as VSHash/PSHash in
			//! SPSOKey) -- no need for a key as rich as SPSOKey here, a compute shader has no
			//! blend/depth/rasterizer/input layout to fix. Creates the PSO on the first call for a given
			//! blob, reuses it afterward.
			ID3D12PipelineState* getOrCreateComputePSO(ID3DBlob* computeShader);

			//! Copies a CPU-only descriptor (SRV or UAV, same CBV/SRV/UAV heap as the CBVs -- see
			//! allocateUserCBVTable()) into the next free slot of the current frame's shader-visible
			//! heap. Generalization of allocateSRVTableSlot() for an already-resolved handle
			//! (CD3D12HardwareBuffer::getShaderResourceView()/getUnorderedAccessView()) rather than a
			//! CD3D12Texture -- reused by dispatchComputeShader().
			D3D12_GPU_DESCRIPTOR_HANDLE allocateDescriptorTableSlot(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);

			//! Translates an E_BLEND_FACTOR (SMaterial.h, decoded from MaterialTypeParam via
			//! unpack_textureBlendFunc() for EMT_ONETEXTURE_BLEND) to the equivalent D3D12_BLEND.
			//! forAlphaChannel substitutes the "_COLOR" variants (not valid on
			//! SrcBlendAlpha/DestBlendAlpha under D3D12 -- the validation layer rejects them) with their
			//! closest "_ALPHA" equivalent; since this driver applies the same factor to RGB and alpha
			//! (see SPSOKey::CustomSrcBlend/CustomDestBlend), this is the least surprising approximation
			//! rather than decoding unpack_textureBlendFuncSeparate() (distinct RGB/alpha factors, not
			//! done here).
			static D3D12_BLEND getD3D12BlendFactor(E_BLEND_FACTOR factor, bool forAlphaChannel);

			//! Translates an E_COMPARISON_FUNC (SMaterial::ZBuffer) to the equivalent
			//! D3D12_COMPARISON_FUNC, a direct mapping with no inversion -- same approach as
			//! CD3D11Driver::getDepthFunction(). OuterSpace uses a reversed depth convention (see
			//! clearZBuffer()/the SMaterial::ZBuffer=ECFN_GREATER default): this mapping just exposes the
			//! comparison requested by the material faithfully; the "reversed" semantics come from the
			//! 0.0f clear value and the projection matrix (CMatrix4::buildProjectionMatrixPerspectiveFovLH),
			//! not from here.
			static D3D12_COMPARISON_FUNC getD3D12DepthFunc(E_COMPARISON_FUNC func);

			void waitForFrame(UINT frameIndex);
			UINT64 signalFence();

			//! Creates, for each frame-in-flight, the constant buffer ring (b0/b1) and shader-visible SRV
			//! heap described on SD3D12FrameContext. Called once at init, after
			//! createRootSignature()/createBuiltInMaterialRenderers().
			bool createFrameDrawResources();

			//! Creates the pair of 8-byte resources used as the "buffer filled size" counter for
			//! SOSetTargets -- StreamOutputCounter (default heap, D3D12_RESOURCE_STATE_STREAM_OUT at
			//! rest, the only valid state for BufferFilledSizeLocation) and StreamOutputCounterUpload
			//! (upload heap, permanently holding a UINT64 = 0, the source for the reset copied in
			//! resetStreamOutputCounter() before each new SO target). Called once at init, like
			//! createFrameDrawResources()/createNullTexture().
			bool createStreamOutputResources();

			//! Resets StreamOutputCounter to 0 (CopyBufferRegion from StreamOutputCounterUpload) before
			//! binding it via SOSetTargets -- unlike D3D11's SOSetTargets(..., pOffsets), D3D12 offers no
			//! implicit reset of the fill counter: without this, a stream-output pass would keep writing
			//! after the previous call's contents instead of restarting from offset 0 of the target
			//! buffer.
			void resetStreamOutputCounter();

			//! Creates the 1x1 opaque white fallback texture used when SMaterial::getTexture(0) is null
			//! -- the default shader always samples BaseTexture, so the t0 descriptor table must always
			//! point at something valid (same reasoning as NullTexture in CD3D11Driver).
			bool createNullTexture();

			//! Sub-allocates sizeBytes from the current frame's constant ring (see SD3D12FrameContext),
			//! copies data into it, and returns the GPU address to pass to
			//! SetGraphicsRootConstantBufferView. Grows the ring on demand (growConstantRing()) if the
			//! current capacity is insufficient -- only returns 0 (logs a warning rather than crashing)
			//! if THAT growth attempt fails (CreateCommittedResource/Map, e.g. GPU memory exhausted).
			D3D12_GPU_VIRTUAL_ADDRESS allocateConstant(const void* data, size_t sizeBytes);

			//! Grows the frame's ConstantRing so it can hold at least minCapacity bytes: creates a new
			//! upload buffer (current capacity doubled until it covers minCapacity), maps it, then
			//! retires the old one via retireResource() -- GPU addresses already recorded in this command
			//! list for earlier allocations THIS frame stay valid (they point into the old buffer, kept
			//! alive until the GPU has consumed it), so no copy is needed: new allocations simply restart
			//! at offset 0 in the new buffer. Returns false if creation fails (the caller keeps the old
			//! ring as-is).
			bool growConstantRing(SD3D12FrameContext& frame, UINT64 minCapacity);

			//! Maximum number of descriptors a single draw can consume in the frame's shader-visible heap:
			//! the SRV t0..t8 table (MaxUserShaderTextureSlots) plus one CBV b0..b7 table
			//! (MaxUserShaderCBVSlotsPerStage) for each of the 5 programmable stages (VS/PS/GS/HS/DS, see
			//! allocateUserCBVTable() in bindDrawState()).
			static const UINT MaxShaderVisibleSRVDescriptorsPerDraw =
				MaxUserShaderTextureSlots + 5 * MaxUserShaderCBVSlotsPerStage;

			//! Ensures `frame`'s shader-visible heap has room for `count` more descriptors, growing it if
			//! needed.
			//!
			//! Must be called BEFORE a draw's first SetGraphicsRootDescriptorTable(), never in between.
			//! growShaderVisibleSRVHeap() creates a NEW heap and binds it to the command list
			//! (rebindShaderVisibleHeaps()): any GPU handle already placed in a root argument then points
			//! into the OLD heap, which is no longer the one bound at draw time. D3D12 rejects exactly
			//! that (EXECUTION ERROR #554 SET_DESCRIPTOR_HEAP_INVALID) and the draw samples wrong
			//! descriptors. Reserving a draw's worst case up front guarantees that, if growth happens, it
			//! happens BEFORE any root argument of that draw is set: all its handles then come from the
			//! same heap, the one that will be bound.
			bool reserveShaderVisibleSRVDescriptors(SD3D12FrameContext& frame, UINT count);

			//! Copies the CPU-only SRV descriptors of MaxUserShaderTextureSlots textures (or NullTexture
			//! for any null/SRV-less entry -- the normal case for any register beyond the number of
			//! layers actually used by the material, see SMaterial::TextureLayer[]) into the next free,
			//! CONTIGUOUS slots of the current frame's shader-visible heap, and returns the GPU handle of
			//! the first one (t0) to pass to SetGraphicsRootDescriptorTable -- the table covers t0..t8
			//! (see createRootSignature()). All 9 slots are always filled (rather than precisely
			//! reflecting the registers used by the current shader): this makes non-contiguous registers
			//! (e.g. GasGiant skipping t2) transparent for free, since D3D12 only requires the root
			//! signature to be a superset, not an exact match -- see the MaxUserShaderTextureSlots
			//! comment. Grows the frame's shader-visible heap on demand (growShaderVisibleSRVHeap()) if
			//! needed -- only returns a null handle if THAT growth fails, or if no fallback texture is
			//! available for slot 0.
			D3D12_GPU_DESCRIPTOR_HANDLE allocateSRVTableSlot(const std::array<CD3D12Texture*, MaxUserShaderTextureSlots>& textures);

			//! Grows the frame's ShaderVisibleSRVHeap so it can hold at least minCapacity more
			//! descriptors starting at ShaderVisibleSRVNext: creates a new shader-visible heap (current
			//! capacity doubled until it covers the need), retires the old one via retireResource() (same
			//! lifetime guarantees as an ID3D12Resource -- see its comment), resets ShaderVisibleSRVNext
			//! to 0 (nothing to copy, no allocation already made this frame will be requested at a
			//! different index) and binds the new heap to the current command list
			//! (rebindShaderVisibleHeaps()). Returns false if creation fails (the caller keeps the old
			//! heap as-is).
			bool growShaderVisibleSRVHeap(SD3D12FrameContext& frame, UINT minCapacity);

			//! Writes MaxUserShaderCBVSlotsPerStage contiguous CBV descriptors into the current frame's
			//! shader-visible heap (same CBV/SRV/UAV heap as allocateSRVTableSlot(), different descriptor
			//! type) for the ONE table at register space `space` (0..MaxUserShaderRegisterSpaces-1):
			//! one descriptor per buffer entry whose Space matches, placed at slot
			//! SD3D12UserShaderCBuffer::BindPoint (previously uploaded into the constant ring via
			//! allocateConstant()) -- entries for a DIFFERENT space in `buffers` are skipped, since
			//! each space gets its own root-signature table/call (see bindDrawState()). Remaining
			//! slots left as a null CBV (BufferLocation=0) so the whole bound table stays valid even
			//! if the shader doesn't read every register. Grows the frame's shader-visible heap on
			//! demand (growShaderVisibleSRVHeap()) if needed. Returns a null handle if no buffer
			//! matches `space` or if this growth fails.
			D3D12_GPU_DESCRIPTOR_HANDLE allocateUserCBVTable(const std::vector<SD3D12UserShaderCBuffer>& buffers, UINT space);

			//! Translates an SMaterialLayer's filtering/addressing settings into a D3D12_SAMPLER_DESC.
			//! Takes the MaxUserShaderTextureSlots layers (s0..s8, see
			//! createRootSignature()/allocateSRVTableSlot()) -- deduplicates the whole combination of all
			//! 9 via SamplerCombinationCache (not independent per-slot caches: the sampler table covers
			//! s0..s8 contiguously, so all 9 slots must be reserved/created together), then copies the
			//! CPU-only descriptors, contiguously, into the persistent shader-visible sampler heap -- same
			//! D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER heap as before, always allocated in groups of
			//! MaxUserShaderTextureSlots slots now (see ShaderVisibleSamplerCount/
			//! ShaderVisibleSamplerCapacity). Priority order per layer is bilinear > trilinear >
			//! anisotropic, same order as CD3D11Driver::setBasicRenderStates() (see CD3D11CallBridge.h)
			//! for consistent rendering between drivers.
			//! useMipMaps (SMaterial::UseMipMaps): applied uniformly to all MaxUserShaderTextureSlots
			//! samplers of this call -- see buildD3D12SamplerDesc(). True by default for callers that
			//! don't pass an SMaterial (2D/immediate drawing, shadow volumes). Grows the persistent
			//! shader-visible sampler heap on demand (growShaderVisibleSamplerHeap()) if needed.
			D3D12_GPU_DESCRIPTOR_HANDLE allocateSamplerTableSlot(const std::array<SMaterialLayer, MaxUserShaderTextureSlots>& layers, bool useMipMaps = true);

			//! Grows ShaderVisibleSamplerHeap so it can hold at least minCapacity descriptors: creates a
			//! new shader-visible sampler heap (current capacity doubled until it covers the need), COPIES
			//! IN the ShaderVisibleSamplerCount descriptors already cached (SamplerCombinationCache
			//! references these slots by INDEX, not pointer -- unlike ConstantRing/ShaderVisibleSRVHeap
			//! this heap is persistent, so its entries must stay valid across frames), retires the old heap
			//! via retireResource(), and binds the new heap to the current command list
			//! (rebindShaderVisibleHeaps()). Returns false if creation fails (the caller keeps the old
			//! heap as-is).
			bool growShaderVisibleSamplerHeap(UINT minCapacity);

			//! Same mapping as CD3D11Driver::getTextureWrapMode() (the D3D11_TEXTURE_ADDRESS_MODE/
			//! D3D12_TEXTURE_ADDRESS_MODE enums are identical).
			static D3D12_TEXTURE_ADDRESS_MODE getD3D12TextureWrapMode(u8 clamp);

			//! Sub-allocates vertexCount*stride bytes from the current frame's vertex ring (VertexRing,
			//! see SD3D12FrameContext) and copies data into it. No particular alignment requirement
			//! (unlike allocateConstant()). Grows the ring on demand (growVertexRing()) if needed -- the
			//! returned view's SizeInBytes is only 0 if that growth fails.
			D3D12_VERTEX_BUFFER_VIEW allocateVertices(const void* data, u32 vertexCount, u32 stride);

			//! Same principle as growConstantRing(), for the frame's vertex ring (VertexRing).
			bool growVertexRing(SD3D12FrameContext& frame, UINT64 minCapacity);

			//! Binds the current frame's shader-visible SRV/CBV/UAV heap
			//! (Frames[CurrentFrameIndex].ShaderVisibleSRVHeap) and the persistent shader-visible sampler
			//! heap (ShaderVisibleSamplerHeap) to the current command list -- call again after replacing
			//! either one (growShaderVisibleSRVHeap()/growShaderVisibleSamplerHeap()): an
			//! ID3D12GraphicsCommandList resolves descriptor tables of subsequent commands against the
			//! most recently bound heaps (SetDescriptorHeaps()); commands already recorded before the
			//! replacement keep resolving against the old heap (the one bound when they were emitted).
			void rebindShaderVisibleHeaps();

			//! Binds PSO/root signature/CBV b0-b1/SRV t0 table for this material and these transforms
			//! (transposed before upload, see drawMeshBuffer()). Shared by drawMeshBuffer() and all
			//! immediate 2D/3D draw functions -- touches neither topology nor vertex/index buffers, left
			//! to the caller. Returns false if no PSO could be obtained (nothing is bound).
			//! descriptor: passed through to getPSOForMaterial() as-is, see its comment -- null by
			//! default for drawImmediate()/2D/3D callers without a scene::IMeshBuffer (falls back to
			//! EVT_STANDARD).
			//! primitiveType: passed through to getPSOForMaterial()/buildPSOKeyFromMaterial() as-is --
			//! EPT_TRIANGLES by default.
			bool bindDrawState(const SMaterial& material, const core::matrix4& world,
				const core::matrix4& view, const core::matrix4& proj, IVertexDescriptor* descriptor = nullptr,
				scene::E_PRIMITIVE_TYPE primitiveType = scene::EPT_TRIANGLES);

			//! The part of bindDrawState() that doesn't pick a PSO -- just the CBV b0/b1/b2, the SRV t0
			//! table and the sampler s0 table. Used directly by shadow volumes/occlusion queries, which
			//! need to force a specific auxiliary PSO (getOrCreateAuxPSO()) rather than the one from
			//! getPSOForMaterial() -- these callers have no relevant SMaterial; a default layer
			//! (SMaterialLayer(), the same "normal" settings as a fresh SMaterial) is enough for them
			//! since they don't depend on texture filtering (stencil marking with no color write, debug
			//! lines).
			//! texture2/layer2: second texture layer (SMaterial::TextureLayer[1]) for multi-texture
			//! materials (EMT_SOLID_2_LAYER, EMT_LIGHTMAP*...) -- null by default for callers that don't
			//! have one (2D/3D immediate drawing, shadow volumes, occlusion queries), see
			//! allocateSRVTableSlot()/allocateSamplerTableSlot().
			//! extraTextures/extraLayers/extraCount: layers 2..8 (t2..t8/s2..s8) for a material using
			//! more than 2 textures -- only bindDrawState() (drawMeshBuffer) fills these in, from
			//! SMaterial::TextureLayer[2..8]; every other caller (2D/3D immediate drawing, shadow
			//! volumes, occlusion queries) leaves extraCount at 0, which falls back to
			//! NullTexture/default sampler for t2..t8.
			//! useMipMaps (SMaterial::UseMipMaps): true by default for all callers that don't pass an
			//! SMaterial (2D/immediate drawing, shadow volumes) -- only bindDrawState() fills it in from
			//! the material actually being drawn.
			void bindTransformsAndTexture(const core::matrix4& world, const core::matrix4& view,
				const core::matrix4& proj, video::ITexture* texture,
				const SMaterialLayer& layer = SMaterialLayer(),
				video::ITexture* texture2 = nullptr,
				const SMaterialLayer& layer2 = SMaterialLayer(),
				video::ITexture* const* extraTextures = nullptr,
				const SMaterialLayer* extraLayers = nullptr,
				UINT extraCount = 0,
				bool useMipMaps = true);

			//! CBV b3 (root descriptor, see createRootSignature()) -- dynamic lighting
			//! (SMaterial::Lighting/AmbientColor/DiffuseColor/SpecularColor/EmissiveColor/ColorMaterial/
			//! NormalizeNormals plus the CNullDriver::Lights list, see addDynamicLight()/
			//! getDynamicLight()). Called by bindDrawState() right after bindTransformsAndTexture() --
			//! see its implementation comment (CD3D12Driver.cpp) for the exact layout, which must stay
			//! identical to the LightingCB block in D3D12DefaultShaderHLSL (CD3D12DefaultShaders.h).
			void bindLighting(const SMaterial& material);

			//! CBV b4 (root descriptor, see createRootSignature()) -- fog (SMaterial::FogEnable plus the
			//! parameters set by setFog()/getFog(), see also
			//! CNullDriverCommon::FogColor/FogType/FogStart/FogEnd/FogDensity). Called by bindDrawState()
			//! right after bindLighting() -- see its implementation comment (CD3D12Driver.cpp) for the
			//! exact layout, which must stay identical to the FogCB block in D3D12DefaultShaderHLSL
			//! (CD3D12DefaultShaders.h).
			void bindFog(const SMaterial& material);

			//! Auxiliary PSO key for stencil-marking a shadow volume: no color write
			//! (RenderTargetWriteMask=0), no depth write. useDepthFailOp selects which stencil op field
			//! "op" is written to -- StencilPassOp (zpass technique, on depth-test success) or
			//! StencilDepthFailOp (zfail technique, on depth-test failure); the untouched field stays
			//! D3D12_STENCIL_OP_KEEP. cullMode determines which face of the volume this PSO handles
			//! (the same op is applied to FrontFace and BackFace, see the comment in
			//! CD3D12PSOCache.h -- irrelevant since cullMode already discards the other side).
			SPSOKey buildShadowVolumeStencilKey(D3D12_CULL_MODE cullMode, D3D12_STENCIL_OP op, bool useDepthFailOp) const;

			//! Draws vertices (already flattened, no indices) with the given topology, via
			//! bindDrawState() + allocateVertices(). Used by all immediate 2D/3D draw functions
			//! (draw2DImage, draw3DLine, etc.).
			void drawImmediate(const S3DVertex* vertices, u32 vertexCount, D3D_PRIMITIVE_TOPOLOGY topology,
				const SMaterial& material, const core::matrix4& world, const core::matrix4& view,
				const core::matrix4& proj);

			//! Pixel-perfect orthographic projection matrix for 2D drawing (used with World/View =
			//! Identity) -- same formula as CD3D11Driver::draw2DImage (buildProjectionMatrixOrthoLH +
			//! setTranslation), for consistent rendering across this engine's drivers.
			core::matrix4 build2DProjection() const;

			//! Material used by the 2D draw functions: no lighting/depth/culling, alpha blend if
			//! alphaBlend is true, texture = texture (NullTexture if null, see allocateSRVTableSlot()).
			SMaterial build2DMaterial(bool alphaBlend, video::ITexture* texture) const;

			//! Restricts the rasterizer (RSSetScissorRects) to the clip rectangle (screen coordinates,
			//! top-left origin) if non-null, otherwise to the whole current render target. Call before
			//! each clipped 2D draw.
			void setScissorFromClip(const core::rect<s32>* clip);

		public:
			//! CNullDriver::setViewPort() is an empty stub and its getViewPort() returns a ViewPort that
			//! was never set (zero width/height). Without these overrides, any caller driving the
			//! viewport itself was ignored -- and worse, one that reads getViewPort() back got an empty
			//! rectangle. This is what made the RmlUI interface disappear: IrrRocketRenderer::
			//! EnableScissorRegion() computes scale = screen / getViewPort().getWidth(), i.e. a division
			//! by zero, producing an infinite scale and geometry projected off-screen. Irrlicht's native
			//! UI never touches the viewport, which is why it kept rendering fine.
			virtual void setViewPort(const core::rect<s32>& area) _IRR_OVERRIDE_;
			virtual const core::rect<s32>& getViewPort() const _IRR_OVERRIDE_;

		private:

			//! Auxiliary PSOs (shadow volumes) that don't go through buildPSOKeyFromMaterial() -- same
			//! default shader/root signature/input layout (see CD3D12PSOCache.h), but with stencil/
			//! write-mask states buildPSOKeyFromMaterial() never builds for a normal SMaterial.
			ID3D12PipelineState* getOrCreateAuxPSO(const SPSOKey& key);

			//! Occlusion query resources (see OcclusionQueries below).
			bool createOcclusionQueryResources();

			irr::SIrrlichtCreationParameters Params;
			core::stringw Name;

			ComPtr<IDXGIFactory6> DXGIFactory;
			ComPtr<IDXGIAdapter4> Adapter;
			ComPtr<ID3D12Device2> Device;

			ComPtr<ID3D12CommandQueue> DirectQueue;
			ComPtr<IDXGISwapChain4> SwapChain;
			ComPtr<ID3D12GraphicsCommandList> CommandList;

			// The driver this CD3D12Driver shares resources with (Owned*Heap/OwnedTextureCache/
			// Owned*Upload*) -- itself for an immediate driver (self-owning), the immediate driver for a
			// CD3D12DeferredContext (see its constructor). Used to construct CD3D12Texture with a
			// pointer that outlives this context: a deferred context can be destroyed before the
			// textures it loaded (they live in the shared cache), so CD3D12Texture must never retain
			// "this" but always ResourceOwner (see addTexture()/getTexture()/addRenderTargetTexture()/
			// addTextureArray() in CD3D12Driver.cpp) -- ResourceOwner itself lives as long as the whole
			// GPU pipeline (the immediate driver).
			CD3D12Driver* ResourceOwner;

			// Actual storage -- always declared here even when this instance doesn't own the heaps (see
			// RTVHeap/DSVHeap/CBVSRVUAVHeap just below: references pointing either at these Owned*
			// members (immediate driver) or at a shared immediate driver's (CD3D12DeferredContext, see
			// its constructor)).
			CD3D12DescriptorHeapAllocator OwnedRTVHeap;
			CD3D12DescriptorHeapAllocator OwnedDSVHeap;
			CD3D12DescriptorHeapAllocator OwnedCBVSRVUAVHeap;

			// References actually used throughout the driver -- bound in the constructor. A
			// CD3D12DeferredContext shares the SAME heaps as its immediate driver (no independent copy):
			// a texture loaded from any context occupies a single descriptor slot, regardless of which
			// context requested it. No synchronization here: by contract, only one allocation
			// (upload/texture load) happens at a time, even though multiple contexts may draw in
			// parallel (read-only access to these heaps once descriptors are allocated).
			CD3D12DescriptorHeapAllocator& RTVHeap;
			CD3D12DescriptorHeapAllocator& DSVHeap;
			CD3D12DescriptorHeapAllocator& CBVSRVUAVHeap; // not shader-visible; copied to a shader-visible heap at bind time

			//! Persistent (not per-frame, see ShaderVisibleSamplerCapacity) shader-visible sampler heap
			//! -- each unique filter/addressing combination (D3D12_SAMPLER_DESC packed into a u64 key,
			//! see allocateSamplerTableSlot()) gets a slot created directly in it (Device->CreateSampler
			//! can write directly into a shader-visible heap, no CPU-only intermediate + copy needed as
			//! for texture SRVs) the first time it's encountered, then reuses that slot on every
			//! subsequent draw with the same settings.
			//! Always allocated in groups of MaxUserShaderTextureSlots contiguous slots (s0..s8, see
			//! allocateSamplerTableSlot()/SamplerCombinationCache below) -- the root signature's sampler
			//! table covers s0..s8 together.
			ComPtr<ID3D12DescriptorHeap> ShaderVisibleSamplerHeap;
			D3D12_CPU_DESCRIPTOR_HANDLE ShaderVisibleSamplerHeapStartCPU = {};
			D3D12_GPU_DESCRIPTOR_HANDLE ShaderVisibleSamplerHeapStartGPU = {};
			UINT SamplerDescriptorSize = 0;
			UINT ShaderVisibleSamplerCount = 0;
			//! Current capacity of ShaderVisibleSamplerHeap (may exceed ShaderVisibleSamplerCapacity
			//! after growing -- see growShaderVisibleSamplerHeap()); initialized to
			//! ShaderVisibleSamplerCapacity in createFrameDrawResources().
			UINT ShaderVisibleSamplerHeapCapacity = 0;
			//! Key = the MaxUserShaderTextureSlots D3D12_SAMPLER_DESC values packed together (see
			//! allocateSamplerTableSlot()), value = index of the first of the 9 contiguous slots (s0) in
			//! ShaderVisibleSamplerHeap (s1..s8 are always slot+1..slot+8). Since the table covers
			//! s0..s8 together, all 9 slots must be reserved/created as a single unit, not independently.
			std::map<std::array<UINT64, MaxUserShaderTextureSlots>, UINT> SamplerCombinationCache;

			// Depth/stencil: a single resource shared across frames (unlike the back buffers). Unlike
			// the back buffer, it doesn't need to be distinct per frame-in-flight: it's fully replayed
			// (cleared + written) every frame and never read by a previous frame in parallel -- no
			// tearing risk since it's never "presented". Recreated in OnResize().
			ComPtr<ID3D12Resource> DepthStencilBuffer;
			CD3DX12_CPU_DESCRIPTOR_HANDLE DSVHandle;
			UINT DSVHeapIndex = 0;
			bool HasDepthStencilBuffer = false;
			//! True if the CURRENT scene actually bound a DSV via OMSetRenderTargets() in beginScene()
			//! (zBuffer==true AND HasDepthStencilBuffer) -- see its comment. buildPSOKeyFromMaterial()
			//! must read this flag rather than assume DepthStencilFormat unconditionally: a PSO whose
			//! DSVFormat != UNKNOWN requires a DSV to actually be bound at draw time, otherwise D3D12
			//! rejects the call. A caller doing beginScene(backBuffer, false, ...) (no zBuffer, e.g. a
			//! pure stream-output pass) would otherwise draw silently into the void without this flag.
			bool CurrentSceneHasDepthStencil = false;
			//! True if the CURRENT scene's beginScene(backBuffer, ...) transitioned the back buffer
			//! from PRESENT to RENDER_TARGET (backBuffer==true). endScene() must only transition it
			//! back to PRESENT when this is true -- a beginScene(false, ...) pass (e.g. a pure
			//! stream-output pass) leaves the back buffer in PRESENT the whole scene, and an
			//! unconditional RENDER_TARGET->PRESENT barrier in endScene() would be an invalid
			//! transition from the resource's actual state.
			bool CurrentSceneHasBackBuffer = false;
			//! Format of the DSV actually bound by the last OMSetRenderTargets() -- not necessarily
			//! DepthStencilFormat. A PSO must declare exactly the format of the DSV bound at draw time,
			//! or D3D12 rejects it. Three possible sources, two different formats:
			//!   - the back buffer's depth buffer and checkRTTDepthBuffer()'s pool: DepthStencilFormat.
			//!   - a depth texture passed explicitly to setRenderTarget(..., depthStencil): its own
			//!     format (OuterSpace's G-buffer creates it as D32_FLOAT), which has no reason to match
			//!     DepthStencilFormat.
			//! buildPSOKeyFromMaterial() used to read DepthStencilFormat unconditionally, so every draw
			//! in the deferred render pass got a PSO in D24_UNORM_S8_UINT against a D32_FLOAT DSV and was
			//! rejected -- silently in release, hence the black screen.
			DXGI_FORMAT CurrentDSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
			//! D24_UNORM_S8_UINT since stencil shadow volumes require a stencil plane
			//! (drawStencilShadowVolume()/drawStencilShadow()). Used to be D32_FLOAT (no stencil).
			static const DXGI_FORMAT DepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
			//! Pool of depth/stencil buffers dedicated to render-target-textures, one per distinct size
			//! encountered -- see checkRTTDepthBuffer()/SD3D12RTTDepthBuffer. Distinct from
			//! DepthStencilBuffer/DSVHandle above, which remain the back buffer's buffer (sized to
			//! WindowSize, recreated by OnResize()).
			std::vector<std::unique_ptr<SD3D12RTTDepthBuffer>> RTTDepthBuffers;
			//! DSV actually bound by the last setRenderTarget() call (back buffer -> DSVHandle above;
			//! render-target-texture -> the DSV of the matching SD3D12RTTDepthBuffer pool entry, or {} if
			//! creation failed). Read by clearZBuffer() (IVideoDriver, callable outside
			//! setRenderTarget()) to target the right DSV rather than assuming the back buffer.
			D3D12_CPU_DESCRIPTOR_HANDLE CurrentDSVHandle = {};

			// Command allocator + command list + fence dedicated to beginUpload()/endUploadAndWait(),
			// separate from the per-frame allocators/Fence so they never conflict with a frame's Reset()
			// while an upload is in flight. Actual storage -- see UploadAllocator/UploadCommandList/
			// UploadInProgress/UploadFence* below for the references actually used.
			ComPtr<ID3D12CommandAllocator> OwnedUploadAllocator;
			ComPtr<ID3D12GraphicsCommandList> OwnedUploadCommandList;
			bool OwnedUploadInProgress = false;
			ComPtr<ID3D12Fence> OwnedUploadFence;
			UINT64 OwnedUploadFenceValue = 0;
			HANDLE OwnedUploadFenceEvent = nullptr;
			//! Serializes the upload pipeline above. Shared like it (a deferred context takes its
			//! immediate driver's), otherwise two threads going through different driver objects but the
			//! SAME UploadCommandList wouldn't exclude each other. See UploadScope for why this lock is
			//! necessary.
			std::mutex OwnedUploadMutex;

			// Same sharing mechanism as RTVHeap/DSVHeap/CBVSRVUAVHeap above: a CD3D12DeferredContext
			// shares its immediate driver's upload pipeline rather than keeping an independent one. Only
			// one upload at a time regardless of which context triggered it -- guaranteed by
			// UploadMutex. Distinct from Fence/FenceValue/FenceEvent below (never shared, used for
			// end-of-frame/occlusion queries, potentially signaled from multiple contexts drawing in
			// parallel -- sharing THAT fence would expose ++FenceValue to a real race).
			ComPtr<ID3D12CommandAllocator>& UploadAllocator;
			ComPtr<ID3D12GraphicsCommandList>& UploadCommandList;
			bool& UploadInProgress;
			ComPtr<ID3D12Fence>& UploadFence;
			UINT64& UploadFenceValue;
			HANDLE& UploadFenceEvent;
			std::mutex& UploadMutex;

			//! A GPU resource (or descriptor slot) pending release -- see
			//! retireResource()/retireDescriptor(). Non-null `Heap` means a descriptor slot, non-null
			//! `DescriptorHeapResource` means an entire descriptor heap (dynamic growth, see
			//! growConstantRing()/growShaderVisibleSRVHeap()/growShaderVisibleSamplerHeap()), otherwise
			//! it's `Resource`.
			struct SRetiredResource
			{
				UINT64 FenceValue = 0;
				ComPtr<ID3D12Resource> Resource;
				ComPtr<ID3D12DescriptorHeap> DescriptorHeapResource;
				CD3D12DescriptorHeapAllocator* Heap = nullptr;
				UINT DescriptorIndex = 0;
			};

			// Always the owner's (ResourceOwner): CD3D12Texture/CD3D12HardwareBuffer objects created
			// from a deferred context already receive ResourceOwner as their Driver, and it's the shared
			// DirectQueue (hence the immediate driver's Fence) that actually orders GPU work.
			std::vector<SRetiredResource> OwnedRetiredResources;
			std::mutex OwnedRetireMutex;

			//! Releases everything the GPU has finished using. Called every frame (beginScene());
			//! `force` (destructor, after a full GPU wait) empties the queue without checking the fence.
			void drainRetiredResources(bool force = false);

			// The texture cache (CNullDriver::Textures, refcounted and guarded by textureArrayLock), the
			// image loaders/writers (SurfaceLoader/SurfaceWriter) and the mesh manipulator belong to
			// CNullDriver -- no local copy here. CD3D12DeferredContext used to share OwnedTextureCache by
			// reference; it now delegates cache operations to its immediate driver (see
			// CD3D12DeferredContext.h), preserving the same semantics (a texture loaded once regardless
			// of which context requested it) without duplicating the cache.

			std::array<SD3D12FrameContext, NativeFrameCount> Frames;
			UINT CurrentFrameIndex = 0;

			//! Index of the frame whose back buffer was actually presented on the last endScene().
			//! Distinct from CurrentFrameIndex, already advanced to the next back buffer to draw
			//! (SwapChain->GetCurrentBackBufferIndex()) by the time createScreenShot() is typically
			//! called (after endScene()). HasPresentedFrame stays false until an endScene() has run (the
			//! back buffers exist from init but their contents aren't a valid render yet).
			UINT LastPresentedFrameIndex = 0;
			bool HasPresentedFrame = false;

			ComPtr<ID3D12Fence> Fence;
			UINT64 FenceValue = 0;
			HANDLE FenceEvent = nullptr;

			bool TearingSupported = false;
			core::dimension2d<u32> WindowSize;

			//! True between beginScene() and endScene(), i.e. exactly when CommandList is open (Reset()
			//! done, Close() not yet called). Unlike the D3D11 immediate context, which is always
			//! recording, a closed D3D12 command list ignores anything sent to it -- and submitting it
			//! anyway (Close() on an already-closed list, then ExecuteCommandLists) removes the device
			//! (DXGI_ERROR_INVALID_CALL). A caller drawing outside a scene, or calling endScene() without
			//! beginScene(), must be rejected here rather than taking down the whole device -- see
			//! endScene().
			bool SceneOpen = false;

			//! bindDrawState() refuses to draw outside a scene (see SceneOpen); a misbehaving caller
			//! typically does this thousands of times per second, so the warning is only logged once.
			bool WarnedDrawOutsideScene = false;

			//! Same reason: setRenderTarget() receiving a depthStencil texture without a DSV is called
			//! once per post-process pass and per frame. One warning is enough.
			bool WarnedDepthStencilWithoutDSV = false;

			// --- Generic root signature + PSO cache ---
			// See the header comment of CD3D12PSOCache.h for the exact layout.
			ComPtr<ID3D12RootSignature> RootSignature;
			CD3D12PSOCache PSOCache;

			// --- Mipmap generation via pixel-shader blit (see createMipGenPipeline() and
			// CD3D12Texture::generateMips()) -- reuses PSOCache above (distinct VS/PS hashes prevent any
			// collision with material PSOs), but needs its own root signature (no World/View/Proj/clip
			// planes) and its own shader-visible SRV heap (independent of SD3D12FrameContext's per-frame
			// one, since mipmap generation runs outside beginScene()/endScene()).
			ComPtr<ID3D12RootSignature> MipGenRootSignature;
			ComPtr<ID3DBlob> MipGenVS;
			ComPtr<ID3DBlob> MipGenPS;
			ComPtr<ID3D12DescriptorHeap> MipGenSRVHeap;
			D3D12_CPU_DESCRIPTOR_HANDLE MipGenSRVHeapCPU = {};
			D3D12_GPU_DESCRIPTOR_HANDLE MipGenSRVHeapGPU = {};

			// --- Geometry shader stream-output (see createStreamOutputResources()/
			// resetStreamOutputCounter()/setStreamOutputBuffer()) ---
			ComPtr<ID3D12Resource> StreamOutputCounter;
			ComPtr<ID3D12Resource> StreamOutputCounterUpload;
			D3D12_RESOURCE_STATES StreamOutputCounterState = D3D12_RESOURCE_STATE_COMMON;
			//! Buffer currently bound as the SO target (nullptr if none) -- transitioned back to
			//! VERTEX_AND_CONSTANT_BUFFER by setStreamOutputBuffer() as soon as it's replaced or
			//! detached, so a later draw can read it back.
			CD3D12HardwareBuffer* CurrentStreamOutputBuffer = nullptr;

			// --- Compute (see createComputeRootSignature()/getOrCreateComputePSO()/
			// dispatchComputeShader()) -- root signature and PSO cache separate from the graphics
			// pipeline's; a compute shader shares no state with a VS/PS.
			ComPtr<ID3D12RootSignature> ComputeRootSignature;
			std::unordered_map<size_t, ComPtr<ID3D12PipelineState>> ComputePSOCache;

			// --- Material registry (built-in types + user shaders) ---
			// Each CD3D12MaterialRenderer compiles its own blobs (see createBuiltInMaterialRenderers()/
			// CD3D12MaterialRenderer::compileBuiltIn()) so the MaterialRenderers registry below is the
			// single source of truth -- no shared blobs stored directly on the driver.
			//! Number of built-in E_MATERIAL_TYPE values (EMT_SOLID..EMT_ONETEXTURE_BLEND = 24 values,
			//! see EMaterialTypes.h) registered by createBuiltInMaterialRenderers() before any user
			//! shader -- also the index (SMaterial::MaterialType) of the first user shader ever
			//! registered by registerUserShaderMaterial(). Kept as a named constant (rather than reading
			//! MaterialRenderers.size() at init) purely for the consistency check in
			//! createBuiltInMaterialRenderers() -- actual per-draw indexing goes straight through
			//! MaterialRenderers[material.MaterialType], which no longer distinguishes built-in from
			//! user (see choosePixelShaderForMaterial()).
			static const s32 UserShaderMaterialTypeBase = 24;
			//! Root parameter index (see createRootSignature()) of the CBV b0..b7 descriptor table
			//! reserved for user shader VS cbuffers AT A GIVEN SPACE -- indexed by
			//! CD3D12MaterialRenderer::SD3D12UserShaderCBuffer::Space (0..MaxUserShaderRegisterSpaces-1,
			//! see MaxUserShaderCBVSlotsPerStage/MaxUserShaderRegisterSpaces, CD3D12MaterialRenderer.h)
			//! -- fixed shared slots rather than a root signature per shader. PS/GS/HS/DS below are
			//! the same shape, one array per stage. LightingConstantSlot/FogConstantSlot (further
			//! down, currently hardcoded 25/26 in bindLighting()/bindFog()) come right after these
			//! 5*MaxUserShaderRegisterSpaces slots -- renumber them too if this count changes.
			static constexpr UINT UserShaderConstantSlotVS[MaxUserShaderRegisterSpaces] = { 5, 6, 7, 8 };
			static constexpr UINT UserShaderConstantSlotPS[MaxUserShaderRegisterSpaces] = { 9, 10, 11, 12 };
			//! Same tables as VS/PS above, but visible on the GS side (see
			//! CD3D12MaterialRenderer::GS/GSBuffers).
			static constexpr UINT UserShaderConstantSlotGS[MaxUserShaderRegisterSpaces] = { 13, 14, 15, 16 };
			//! Same, for HS/DS (see CD3D12MaterialRenderer::HS/DS, HSBuffers/DSBuffers).
			static constexpr UINT UserShaderConstantSlotHS[MaxUserShaderRegisterSpaces] = { 17, 18, 19, 20 };
			static constexpr UINT UserShaderConstantSlotDS[MaxUserShaderRegisterSpaces] = { 21, 22, 23, 24 };
			//! Root parameter indices of the driver's own Lighting (b3)/Fog (b4) CBVs -- see
			//! createRootSignature()'s rootParams[25]/[26] and bindLighting()/bindFog(). Named here
			//! (rather than the literal 10/11 these replaced) so the 5*MaxUserShaderRegisterSpaces
			//! shift when that constant changes has exactly one place to update.
			static const UINT LightingConstantSlot = 25;
			static const UINT FogConstantSlot = 26;
			//! D3D12 counterpart of CNullDriver::MaterialRenderers, at the SAME index: NativeRenderers[i]
			//! is the CD3D12MaterialRenderer* for MaterialRenderers[i].Renderer when that renderer was
			//! actually built by this driver (compiled blobs + reflection needed by
			//! getPSOForMaterial()/setVertexShaderConstant()), otherwise nullptr (a "foreign" renderer
			//! registered via addMaterialRenderer()). The name, the IMaterialRenderer* pointer and its
			//! grab/drop belong to the base class -- this vector owns nothing, it only indexes. Filled by
			//! createBuiltInMaterialRenderers() (indices 0..23) then registerUserShaderMaterial() (indices
			//! 24+).
			//!
			//! Only truly populated on the immediate driver (ResourceOwner): a CD3D12DeferredContext
			//! leaves its own empty and reads its owner's via getNativeRenderer() below. This registry
			//! must never be copied when a deferred context is constructed -- a user shader material
			//! registered AFTER that (addHighLevelShaderMaterial()) would otherwise be invisible to the
			//! context, and its SMaterial::MaterialType would fall outside the copy's bounds.
			std::vector<CD3D12MaterialRenderer*> NativeRenderers;

			//! CD3D12MaterialRenderer* of entry idx, or nullptr if that index doesn't exist / the
			//! renderer isn't a native D3D12 material. Always read from ResourceOwner (== this for the
			//! immediate driver, == the immediate driver for a deferred context), so it's always current,
			//! including for materials registered after the context was created.
			CD3D12MaterialRenderer* getNativeRenderer(s32 idx) const
			{
				const std::vector<CD3D12MaterialRenderer*>& renderers = ResourceOwner->NativeRenderers;
				if (idx < 0 || static_cast<size_t>(idx) >= renderers.size())
					return nullptr;
				return renderers[idx];
			}
			//! MaterialType of the active material (the one whose PSO was just bound in
			//! bindDrawState()), or -1 -- see setVertexShaderConstant()/getVertexShaderConstantID() etc.,
			//! which read MaterialRenderers[ActiveMaterialRendererIndex].Native. Valid (non-negative) for
			//! both built-in and user materials, but Native only has reflected cbuffers/variables for a
			//! user shader (see createBuiltInMaterialRenderers(): VSBuffers/PSBuffers/VSVariables/
			//! PSVariables stay empty for built-in types).
			s32 ActiveMaterialRendererIndex = -1;

			// --- Current transform/material state (IVideoDriver::setTransform/setMaterial have no
			// default implementation in CNullDriverCommon -- each driver carries its own state, like
			// CD3D11Driver::Matrices/Material). ---
			core::matrix4 Matrices[ETS_COUNT];
			SMaterial Material;

			//! See setClipPlane()/enableClipPlane() above. Three planes, same as CD3D11Driver
			//! (MaxUserClipPlanes has never exceeded 3 on any driver in this fork); uploaded to b2 on
			//! every draw in bindTransformsAndTexture() -- changes rarely, but the cost of one extra
			//! 64-byte CBV per draw is negligible next to b0/b1 already there.
			core::array<core::plane3df> ClipPlanes;
			bool ClipPlaneEnabled[3] = { false, false, false };

			//! 1x1 white fallback texture (see createNullTexture()). Owned by CNullDriver's cache like
			//! any other addTexture() result: this raw pointer is just a shortcut, it holds no reference
			//! of its own and isn't dropped here.
			ITexture* NullTexture = nullptr;

			//! Size of a descriptor (identical for the CPU-only heap and the per-frame shader-visible
			//! heaps, same D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV type) -- cached in
			//! createFrameDrawResources() rather than calling GetDescriptorHandleIncrementSize() on every
			//! allocateSRVTableSlot().
			UINT CBVSRVUAVDescriptorSize = 0;

			// --- Current render target (setRenderTarget()) ---
			// nullptr = back buffer. setRenderTarget(0) / setRenderTarget(ERT_FRAME_BUFFER) ALWAYS
			// reverts to the back buffer -- same as CD3D11Driver::setRenderTarget()
			// (CurrentBackBuffer = DefaultBackBuffer when tex == NULL), and what the engine relies on.
			// This driver used to "restore" a remembered PreviousRenderTarget instead, under a
			// "setRenderTarget(0) restores the previous target" contract that doesn't actually exist:
			// the PostProcessManager's final compositing and the whole UI (RmlUI + GUI) ended up back in
			// the last bound render-target-texture instead of the back buffer, which stayed at the color
			// it was cleared to in beginScene(). That was the cause of a black screen even though the
			// whole pipeline ran without a single D3D12 error.
			ITexture* CurrentRenderTarget = nullptr;
			core::dimension2d<u32> CurrentRenderTargetSize;

			//! Number of targets/formats actually bound by the last setRenderTarget() call (1 for the
			//! back buffer or the single-target path, up to 8 for MRT) -- read by
			//! buildPSOKeyFromMaterial() so SPSOKey::NumRenderTargets/RTVFormats exactly match what
			//! OMSetRenderTargets() bound (D3D12 requires this). Initialized to the back-buffer state (1
			//! x R8G8B8A8_UNORM) in the constructor.
			UINT CurrentRTVCount = 1;
			DXGI_FORMAT CurrentRTVFormats[8] = {};
			//! Sample count of the currently bound target (1 = back buffer or a single-sample RTT) --
			//! read by buildPSOKeyFromMaterial()/buildShadowVolumeStencilKey()/drawStencilShadow() so
			//! that SPSOKey::SampleCount exactly matches what OMSetRenderTargets() bound, same reasoning
			//! as CurrentRTVFormats above. The back buffer itself always stays single-sample in this
			//! configuration (swapchain flip-model limitation) -- only render-target-textures can be
			//! MSAA.
			UINT CurrentRTVSampleCount = 1;

			//! The RTV handles actually passed to the last OMSetRenderTargets() -- remembered so the
			//! same target can be RE-bound after a flushCommandList() (a reset command list retains no
			//! bindings). Without these, anything following a mid-frame flush would draw with no render
			//! target.
			D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTVHandles[8] = {};

			// --- Occlusion queries ---
			// A single ID3D12QueryHeap (D3D12_QUERY_TYPE_BINARY_OCCLUSION) + one shared READBACK
			// resource (no per-frame multi-buffering), mapped once. A runOcclusionQuery() result is only
			// reliable starting from the next beginScene() (no mid-frame flush/wait) -- see the comment
			// on updateOcclusionQuery() in the .cpp. Sufficient for the normal usage pattern (run this
			// frame, update the next), not for an update(block=true) in the same frame as the run.
			struct SD3D12OcclusionQuery
			{
				UINT Slot = 0;
				std::vector<core::vector3df> Positions; // flattened local geometry, snapshotted at addOcclusionQuery()
				UINT64 PendingFenceValue = 0; // 0 = no GPU request in flight for this slot
				u32 LastResult = 0;
			};
			std::unordered_map<std::shared_ptr<scene::ISceneNode>, SD3D12OcclusionQuery> OcclusionQueries;
			std::vector<UINT> FreeOcclusionSlots;
			ComPtr<ID3D12QueryHeap> OcclusionQueryHeap;
			ComPtr<ID3D12Resource> OcclusionReadback;
			void* OcclusionReadbackMapped = nullptr;
			static const UINT OcclusionQueryCapacity = 256;

			// DriverAttributes and the vertex descriptors (CNullDriver::VertexDescriptor, pre-filled
			// with "standard"/"2tcoords"/"tangents"/"standardcolorf" by
			// CNullDriver::createVertexDescriptors()) come from the base class -- initDriver() only
			// adjusts the attributes that actually depend on the D3D12 device.
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_

#endif // __C_VIDEO_DIRECTX_12_H_INCLUDED__
