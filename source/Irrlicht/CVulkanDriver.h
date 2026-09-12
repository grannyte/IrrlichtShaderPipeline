// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Native Vulkan driver, structured like CD3D12Driver: an explicit API with per-frame command
// buffers, a pipeline cache keyed on render state, and per-material descriptor set layouts.
//
// Requires VK_KHR_dynamic_rendering (core in 1.3). The alternative is explicit VkRenderPass and
// VkFramebuffer objects, which do not fit IVideoDriver::setRenderTarget()'s "bind these targets
// now" model without caching a render pass per target combination. initDriver() fails cleanly if
// the extension is missing so createDeviceEx() can fall back to another backend.
//
// Two kinds of material live in the one registry SMaterial::MaterialType indexes: the built-in
// E_MATERIAL_TYPEs, whose pre-compiled SPIR-V is embedded (CVulkanDefaultShaders.h) and served by
// CVulkanMaterialRenderer, and user shaders created through addHighLevelShaderMaterial(), which are
// CVulkanUserMaterial objects carrying their own compiled modules, reflection and descriptor set
// layouts. Compute shaders are a third kind: CVulkanComputeMaterial objects registered through
// addComputeShader(), dispatched by CVulkanCompute (see dispatchComputeShader()).

#ifndef __C_VULKAN_DRIVER_H_INCLUDED__
#define __C_VULKAN_DRIVER_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CNullDriver.h"
#include "CVulkanHelpers.h"
#include "CVulkanPipelineCache.h"
#include "CVulkanMaterialRenderer.h"
#include "CVulkanUserMaterial.h"
#include "CVulkanVertexDescriptor.h"
#include "CVulkanImmediate.h"
#include "CVulkanRenderTarget.h"
#include "CVulkanCompute.h"
#include "CVulkanOcclusionQuery.h"
#include "CVulkanSamplerCache.h"
#include "IDeferredContext.h"
#include "SIrrCreationParameters.h"
#include <vector>
#include <mutex>

//! Debug builds ask the loader for VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils, and route
//! whatever they report through os::Printer::log. Both are looked up before use, so a machine with
//! no SDK (hence no layer) still gets a working instance -- and, since the loader itself implements
//! debug_utils, still gets loader diagnostics. Never on in release: the layer costs several times
//! the frame time. Define this by hand to test a release build against validation.
#if defined(_DEBUG) && !defined(_IRR_VULKAN_DEBUG_LAYER_)
#define _IRR_VULKAN_DEBUG_LAYER_
#endif

namespace irr
{
	namespace video
	{
		class CVulkanTexture;
		class CVulkanHardwareBuffer;

		//! One frame in flight: its command buffer, the fence that says the GPU is done with it,
		//! and the transient allocators reset when it comes round again. Mirrors
		//! SD3D12FrameContext -- same reason, the CPU must not touch resources the GPU still reads.
		struct SVulkanFrameContext
		{
			VkCommandPool CommandPool = VK_NULL_HANDLE;
			VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
			//! Signalled when this frame's submission completes; waited on before reusing it.
			VkFence Fence = VK_NULL_HANDLE;
			//! Signalled by AcquireNextImageKHR, waited on by the submission.
			VkSemaphore ImageAvailable = VK_NULL_HANDLE;
			//! Signalled by the submission, waited on by the present.
			VkSemaphore RenderFinished = VK_NULL_HANDLE;
			//! All reset together each frame; descriptor sets are allocated per draw from
			//! DescriptorPools[CurrentDescriptorPool], and a further pool is appended whenever a
			//! frame outruns the one before it. A fixed budget silently dropped every draw past it.
			std::vector<VkDescriptorPool> DescriptorPools;
			u32 CurrentDescriptorPool = 0;
			//! Host-visible ring for per-draw uniforms, the counterpart of the D3D12 constant ring.
			//! Grows on demand; see RetiredUniform* below.
			VkBuffer UniformRing = VK_NULL_HANDLE;
			VkDeviceMemory UniformRingMemory = VK_NULL_HANDLE;
			u8* UniformRingMapped = nullptr;
			VkDeviceSize UniformRingCapacity = 0;
			VkDeviceSize UniformRingNext = 0;
			//! Rings this frame outgrew. Descriptor sets already written still point into them, so
			//! they are only freed once the fence proves the GPU is done with this frame.
			std::vector<VkBuffer> RetiredUniformBuffers;
			std::vector<VkDeviceMemory> RetiredUniformMemory;
			//! Transient vertex storage for the 2D/immediate paths, rewound when the frame comes
			//! round. Owned here; a pointer because the ring holds a context reference.
			CVulkanImmediateRing* Immediate = nullptr;
		};

		class CVulkanDriver : public CNullDriver, public IVulkanUploadContext,
			public IMaterialRendererServices
		{
			//! The deferred context is a second driver on the same device (see CVulkanDeferredContext.h);
			//! it reaches the immediate driver's members the way CD3D12DeferredContext reaches its owner's.
			friend class CVulkanDeferredContext;
		public:
			CVulkanDriver(const irr::SIrrlichtCreationParameters& params, io::IFileSystem* io, HWND window);
			virtual ~CVulkanDriver();

			//! A CVulkanDeferredContext recording on this device, or 0 (logged) when it could not be
			//! built. Record into it from any thread, then executeDeferredContext() it from the thread
			//! that owns this driver; it draws into its own render target texture, as the D3D12 one does.
			virtual IVideoDriver* createDeferredContext() _IRR_OVERRIDE_;
			virtual void executeDeferredContext(IDeferredContext* context) _IRR_OVERRIDE_;

			//! Full device bring-up: loader, instance, surface, physical/logical device, swapchain,
			//! frame contexts, shader modules and built-in materials. Returns false (logged) on any
			//! failure, leaving the object safe to drop().
			bool initDriver(HWND hwnd);

			// --- IVideoDriver: scene lifecycle ---
			virtual bool beginScene(bool backBuffer = true, bool zBuffer = true,
				SColor color = SColor(255, 0, 0, 0),
				const SExposedVideoData& videoData = SExposedVideoData(),
				core::rect<s32>* sourceRect = 0) _IRR_OVERRIDE_;
			virtual bool endScene() _IRR_OVERRIDE_;
			virtual const wchar_t* getName() const _IRR_OVERRIDE_ { return Name.c_str(); }
			virtual E_DRIVER_TYPE getDriverType() const _IRR_OVERRIDE_ { return EDT_VULKAN; }
			virtual void OnResize(const core::dimension2d<u32>& size) _IRR_OVERRIDE_;
			virtual bool queryFeature(E_VIDEO_DRIVER_FEATURE feature) const _IRR_OVERRIDE_;

			// --- IVideoDriver: state ---
			virtual void setTransform(E_TRANSFORMATION_STATE state, const core::matrix4& mat) _IRR_OVERRIDE_;
			virtual const core::matrix4& getTransform(E_TRANSFORMATION_STATE state) const _IRR_OVERRIDE_
			{
				return Matrices[state];
			}
			virtual void setMaterial(const SMaterial& material) _IRR_OVERRIDE_;
			virtual void setViewPort(const core::rect<s32>& area) _IRR_OVERRIDE_;

			//! Up to 16 viewports (D3D11.x feature set, multiViewport feature); the shader picks one
			//! with SV_ViewportArrayIndex / gl_ViewportIndex.
			virtual void setViewPorts(const core::array<core::rect<s32> >& areas) _IRR_OVERRIDE_;
			//! The area setViewPort() last accepted, not an empty rect: GUI code divides by it.
			virtual const core::rect<s32>& getViewPort() const _IRR_OVERRIDE_ { return ViewPort; }
			virtual void clearZBuffer() _IRR_OVERRIDE_;

			//! The three planes the built-in fragment shaders evaluate (ClipPlanesCB, set 4
			//! binding 2); a disabled one is uploaded as (0,0,0,1), which never clips.
			virtual bool setClipPlane(u32 index, const core::plane3df& plane, bool enable = false) _IRR_OVERRIDE_;
			virtual void enableClipPlane(u32 index, bool enable) _IRR_OVERRIDE_;

			// --- IVideoDriver: capabilities. Fixed answers, except the texture size, which the
			// physical device reports.
			virtual ECOLOR_FORMAT getColorFormat() const _IRR_OVERRIDE_ { return ECF_A8R8G8B8; }
			virtual const core::dimension2d<u32>& getScreenSize() const _IRR_OVERRIDE_ { return ScreenSize; }
			virtual const core::dimension2d<u32>& getCurrentRenderTargetSize() const _IRR_OVERRIDE_
			{
				return CurrentRenderTargetSize;
			}
			virtual core::dimension2du getMaxTextureSize() const _IRR_OVERRIDE_;
			//! 8 == MAX_LIGHTS in common.glsl; the CNullDriver default of 0 would say lighting is dead.
			virtual u32 getMaximalDynamicLightAmount() const _IRR_OVERRIDE_ { return 8; }
			virtual u32 getMaximalPrimitiveCount() const _IRR_OVERRIDE_ { return 0xFFFFFFFF; }
			virtual core::stringc getVendorInfo() _IRR_OVERRIDE_ { return VendorInfo; }
			//! Vulkan has no D3D-style quality levels: 1 when the device can rasterize that many
			//! samples into a colour+depth attachment, 0 otherwise. Not an IVideoDriver method.
			virtual u32 queryMultisampleLevels(ECOLOR_FORMAT format, u32 numSamples) const;

			// --- IVideoDriver: drawing ---
			virtual void drawMeshBuffer(const scene::IMeshBuffer* mb) _IRR_OVERRIDE_;
			//! Debug aid: one line per vertex along its normal. EVT_STANDARD only, as on D3D12.
			virtual void drawMeshBufferNormals(const scene::IMeshBuffer* mb, f32 length = 10.f,
				SColor color = 0xffffffff) _IRR_OVERRIDE_;

			// The 3D immediate primitives. All three draw with the CURRENT material and transforms,
			// the contract IVideoDriver.h documents -- no 2D projection is substituted.
			virtual void draw3DLine(const core::vector3df& start, const core::vector3df& end,
				SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;
			virtual void draw3DTriangle(const core::triangle3df& triangle,
				SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;
			virtual void draw3DBox(const core::aabbox3d<f32>& box,
				SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;

			virtual void draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos,
				const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect = 0,
				SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false) _IRR_OVERRIDE_;
			//! Scaled blit with per-corner tint. CNullDriver's fallback drops `destRect`'s size, so
			//! this override is what makes a stretched image actually stretch.
			virtual void draw2DImage(const video::ITexture* texture, const core::rect<s32>& destRect,
				const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect = 0,
				const video::SColor* const colors = 0, bool useAlphaChannelOfTexture = false) _IRR_OVERRIDE_;
			//! Both batches are loops over the single-image path -- one draw per image, like the
			//! D3D12 backend; batching them into one vertex buffer is a later optimisation.
			virtual void draw2DImageBatch(const video::ITexture* texture, const core::position2d<s32>& pos,
				const core::array<core::rect<s32> >& sourceRects, const core::array<s32>& indices,
				s32 kerningWidth = 0, const core::rect<s32>* clipRect = 0,
				SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false) _IRR_OVERRIDE_;
			virtual void draw2DImageBatch(const video::ITexture* texture,
				const core::array<core::position2d<s32> >& positions,
				const core::array<core::rect<s32> >& sourceRects, const core::rect<s32>* clipRect = 0,
				SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false) _IRR_OVERRIDE_;

			virtual void draw2DRectangle(SColor color, const core::rect<s32>& pos,
				const core::rect<s32>* clip = 0) _IRR_OVERRIDE_;
			virtual void draw2DRectangle(const core::rect<s32>& pos, SColor colorLeftUp, SColor colorRightUp,
				SColor colorLeftDown, SColor colorRightDown, const core::rect<s32>* clip = 0) _IRR_OVERRIDE_;
			virtual void batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
				const irr::core::array<SColor>& color,
				const irr::core::array<core::rect<s32>>* clip = 0) _IRR_OVERRIDE_;
			virtual void batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
				irr::core::array<SColor>& colorLeftUp, irr::core::array<SColor>& colorRightUp,
				irr::core::array<SColor>& colorLeftDown, irr::core::array<SColor>& colorRightDown,
				const irr::core::array<core::rect<s32>>* clip = 0) _IRR_OVERRIDE_;
			virtual void draw2DRectangleOutline(const core::recti& pos,
				SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;

			virtual void draw2DLine(const core::position2d<s32>& start, const core::position2d<s32>& end,
				SColor color = SColor(255, 255, 255, 255)) _IRR_OVERRIDE_;
			virtual void drawPixel(u32 x, u32 y, const SColor& color) _IRR_OVERRIDE_;
			virtual void draw2DPolygon(core::position2d<s32> center, f32 radius,
				video::SColor color = SColor(100, 255, 255, 255), s32 vertexCount = 10) _IRR_OVERRIDE_;
			//! EVT_STANDARD only; an index list is flattened on the CPU, so the ring holds a plain
			//! non-indexed stream. Same restriction as the D3D12 backend.
			virtual void draw2DVertexPrimitiveList(const void* vertices, u32 vertexCount,
				const void* indexList, u32 primitiveCount, E_VERTEX_TYPE vType = EVT_STANDARD,
				scene::E_PRIMITIVE_TYPE pType = scene::EPT_TRIANGLES,
				E_INDEX_TYPE iType = EIT_16BIT) _IRR_OVERRIDE_;

			// --- IVideoDriver: render targets ---
			//! `texture` == 0 goes back to the swapchain image. `depthStencil` is honoured when it
			//! carries a depth format; otherwise a depth image is borrowed from DepthPool.
			virtual bool setRenderTarget(video::ITexture* texture, bool clearBackBuffer = true,
				bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0),
				video::ITexture* depthStencil = 0) _IRR_OVERRIDE_;
			//! MRT. One VkRenderingInfo covers every attachment, so a single loadOp decides for all:
			//! clearBackBuffer[0] is the one read, unlike D3D12's per-view clears.
			virtual bool setRenderTarget(const core::array<video::IRenderTarget>& targets,
				const core::array<bool>& clearBackBuffer, bool clearZBuffer, SColor color,
				video::ITexture* depthStencil) _IRR_OVERRIDE_;

			virtual ITexture* addRenderTargetTexture(const core::dimension2d<u32>& size,
				const io::path& name = "rt", const ECOLOR_FORMAT format = ECF_UNKNOWN) _IRR_OVERRIDE_;

			//! Multisample/array form. `arraySlices` > 1 gives an ETT_2D_ARRAY target whose slices are
			//! bound one at a time through setRenderTargetSlice(). `sampleCount` > 1 is not served by
			//! this backend (no resolve pass): it is logged and a single-sample target is created.
			virtual ITexture* addRenderTargetTexture(const core::dimension2d<u32>& size,
				const io::path& name, const ECOLOR_FORMAT format,
				u32 sampleCount, u32 sampleQuality, u32 arraySlices,
				E_TEXTURE_TYPE type = ETT_2D) _IRR_OVERRIDE_;

			//! Binds one slice of a render-target array as the current colour target.
			virtual bool setRenderTargetSlice(video::ITexture* texture, u32 arraySlice,
				bool clearTarget = true, SColor color = video::SColor(0, 0, 0, 0)) _IRR_OVERRIDE_;

			//! ERT_FRAME_BUFFER goes back to the swapchain; the other E_RENDER_TARGET values have no
			//! Vulkan equivalent and are refused.
			virtual bool setRenderTarget(E_RENDER_TARGET target, bool clearTarget = true,
				bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0)) _IRR_OVERRIDE_;

			//! vkCmdCopyImage between two textures of one size and format, depth included. Recorded
			//! on the frame's command buffer when a scene is open (the rendering instance is
			//! suspended around it), on a one-shot upload buffer otherwise.
			virtual bool copyTexture(ITexture* dest, ITexture* source, u32 destSlice = 0) _IRR_OVERRIDE_;

			//! A texture with STORAGE usage, the target of dispatchComputeShaderToTexture().
			virtual ITexture* addUAVTexture(const core::dimension2d<u32>& size,
				const io::path& name = "uav", const ECOLOR_FORMAT format = ECF_A32B32G32R32F) _IRR_OVERRIDE_;

			//! Cube maps and 2D arrays: every slice is copied on the GPU into one layer of a new
			//! image, so the slices only have to be Vulkan textures of one size and format.
			virtual ITexture* createDeviceDependentTexture(const core::array<ITexture*>& surfaces,
				const E_TEXTURE_TYPE Type, const io::path& name, void* mipmapData = 0) _IRR_OVERRIDE_;

			// --- IVideoDriver: stencil shadows. The swapchain depth buffer carries a stencil aspect
			// when SIrrlichtCreationParameters::Stencilbuffer asked for one; without it both calls
			// are no-ops, as on the other backends.
			virtual void drawStencilShadowVolume(const core::array<core::vector3df>& triangles,
				bool zfail = true, u32 debugDataVisible = 0) _IRR_OVERRIDE_;
			virtual void drawStencilShadow(bool clearStencilBuffer = false,
				video::SColor leftUpEdge = video::SColor(0, 0, 0, 0),
				video::SColor rightUpEdge = video::SColor(0, 0, 0, 0),
				video::SColor leftDownEdge = video::SColor(0, 0, 0, 0),
				video::SColor rightDownEdge = video::SColor(0, 0, 0, 0)) _IRR_OVERRIDE_;

			// --- IVideoDriver: occlusion queries, one VK_QUERY_TYPE_OCCLUSION slot per node (see
			// CVulkanOcclusionQuery). runOcclusionQuery() records the query on the frame's command
			// buffer; the result is read back by updateOcclusionQuery() once that frame completed.
			virtual void addOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node,
				const scene::IMesh* mesh = 0) _IRR_OVERRIDE_;
			virtual void removeOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node) _IRR_OVERRIDE_;
			virtual void removeAllOcclusionQueries() _IRR_OVERRIDE_;
			virtual void runOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool visible = false) _IRR_OVERRIDE_;
			virtual void runAllOcclusionQueries(bool visible = false) _IRR_OVERRIDE_;
			virtual void updateOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool block = true) _IRR_OVERRIDE_;
			virtual void updateAllOcclusionQueries(bool block = true) _IRR_OVERRIDE_;
			virtual u32 getOcclusionQueryResult(std::shared_ptr<scene::ISceneNode> node) const _IRR_OVERRIDE_;

			// --- IVideoDriver: compute. Every dispatch is synchronous and records on its own
			// command buffer (beginUpload()/endUploadAndWait()), exactly like the D3D12 backend: the
			// caller may dispatch outside beginScene()/endScene() and read the result back at once.
			virtual std::shared_ptr<video::IHardwareBuffer> createHardwareBuffer(scene::IComputeBuffer* computeBuffer) _IRR_OVERRIDE_;
			virtual void dispatchComputeShader(const core::vector3d<u32>& groupCount,
				scene::IComputeBuffer* Src, scene::IComputeBuffer* Dst) _IRR_OVERRIDE_;
			virtual void dispatchComputeShaderToTexture(const core::vector3d<u32>& groupCount,
				scene::IComputeBuffer* Src, ITexture* Dst) _IRR_OVERRIDE_;

			//! The multi-slot compute path CD3D11Driver offers: buffers and textures are bound to
			//! numbered slots, then dispatched against whatever the active compute material declares
			//! at those slots (see CVulkanComputeMaterial's reflected binding table).
			virtual void bindComputeBuffer(u32 slot, scene::IComputeBuffer* buffer,
				E_HARDWARE_BUFFER_TYPE binding) _IRR_OVERRIDE_;
			virtual void bindComputeTexture(u32 slot, ITexture* texture, bool asUAV) _IRR_OVERRIDE_;
			virtual void dispatchComputeShaderBound(const core::vector3d<u32>& groupCount) _IRR_OVERRIDE_;
			virtual void unbindComputeResources() _IRR_OVERRIDE_;
			virtual void computeBarrier(scene::IComputeBuffer* buffer) _IRR_OVERRIDE_;
			virtual void computeBarrierAll() _IRR_OVERRIDE_;
			virtual void dispatchComputeShaderIndirect(scene::IComputeBuffer* argBuffer, u32 byteOffset) _IRR_OVERRIDE_;
			//! The hidden append/consume counter is emulated: DXC compiles an HLSL
			//! AppendStructuredBuffer to a data buffer plus a one-uint counter buffer, which the
			//! dispatch binds from CVulkanHardwareBuffer::getCounterBuffer(). copyStructureCount() is
			//! then a 4-byte copy and resetStructureCount() a host write (applied at once, every
			//! dispatch having been waited on).
			virtual void copyStructureCount(scene::IComputeBuffer* dst, u32 dstByteOffset,
				scene::IComputeBuffer* appendBuffer) _IRR_OVERRIDE_;
			virtual void resetStructureCount(scene::IComputeBuffer* appendBuffer, u32 value = 0) _IRR_OVERRIDE_;
			virtual bool beginComputeReadback(scene::IComputeBuffer* buffer, u32 slot) _IRR_OVERRIDE_;
			virtual bool tryReadComputeBuffer(scene::IComputeBuffer* buffer, u32 slot, void* dst,
				u32 bytes, bool wait) _IRR_OVERRIDE_;
			virtual void drawMeshBufferInstancedIndirect(const scene::IMeshBuffer* mb,
				scene::IComputeBuffer* instanceBuffer, u32 instanceStride,
				scene::IComputeBuffer* argBuffer, u32 byteOffset) _IRR_OVERRIDE_;

			//! Stream output through VK_EXT_transform_feedback. `buffer` must have been declared with
			//! setBufferType(scene::EBT_STREAM); every draw until the next call (0 unbinds) appends the
			//! primitives of its geometry stage into it, and the byte count lands in the buffer's
			//! counter so a later drawMeshBuffer() of that buffer draws exactly what was captured
			//! (the D3D11 DrawAuto). False (logged once) when the device lacks the extension.
			virtual bool setStreamOutputBuffer(scene::IVertexBuffer* buffer) _IRR_OVERRIDE_;

			// --- IGPUProgrammingServices: compute shaders. The source language is the build's
			// default (see CVulkanShaderCompiler.h); a file whose first word is the SPIR-V magic is
			// taken as a pre-compiled module.
			virtual s32 addComputeShader(const c8* computeShaderProgram,
				const c8* computeShaderEntryPointName = "main",
				E_COMPUTE_SHADER_TYPE csCompileTarget = ECST_CS_5_0,
				IShaderConstantSetCallBack* callback = 0, s32 userData = 0) _IRR_OVERRIDE_;
			virtual s32 addComputeShaderFromFile(const io::path& computeShaderProgramFileName,
				const c8* computeShaderEntryPointName = "main",
				E_COMPUTE_SHADER_TYPE csCompileTarget = ECST_CS_5_0,
				IShaderConstantSetCallBack* callback = 0, s32 userData = 0) _IRR_OVERRIDE_;

			//! Reads the last presented swapchain image back. Call after endScene(), like the D3D12
			//! driver: before that the image holds the previous frame.
			virtual IImage* createScreenShot(video::ECOLOR_FORMAT format = video::ECF_UNKNOWN,
				video::E_RENDER_TARGET target = video::ERT_FRAME_BUFFER) _IRR_OVERRIDE_;

			// --- IVideoDriver: resources ---
			virtual std::shared_ptr<video::IHardwareBuffer> createHardwareBuffer(scene::IIndexBuffer* indexBuffer) _IRR_OVERRIDE_;
			virtual std::shared_ptr<video::IHardwareBuffer> createHardwareBuffer(scene::IVertexBuffer* vertexBuffer) _IRR_OVERRIDE_;

			//! Keeps NativeRenderers/UserRenderers in step with MaterialRenderers, so
			//! material.MaterialType can be resolved back to the SPIR-V modules, layouts and blend
			//! state a pipeline key needs.
			virtual s32 addMaterialRenderer(IMaterialRenderer* renderer, const c8* name = 0) _IRR_OVERRIDE_;

			// --- IGPUProgrammingServices: user shader materials ---
			// Each one is a CVulkanUserMaterial (its own compiler front end and SPIR-V reflection),
			// registered in the SAME registry as the built-ins, so SMaterial::MaterialType selects it
			// like any other type. `shadingLang` reaches CVulkanShaderCompiler untouched -- GLSL, HLSL
			// or a pre-compiled SPIR-V blob, whichever this build carries. A stage that fails to
			// compile logs its front end's diagnostics and the call returns -1, nothing registered.
			virtual s32 addHighLevelShaderMaterial(
				const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget, const c8* pixelShaderProgram,
				const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
				const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
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
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) _IRR_OVERRIDE_;
			virtual s32 addHighLevelShaderMaterialFromFiles(
				const io::path& vertexShaderProgramFileName, const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget, const io::path& pixelShaderProgramFileName,
				const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
				const io::path& geometryShaderProgramFileName, const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
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
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) _IRR_OVERRIDE_;
			virtual s32 addHighLevelShaderMaterialFromFiles(
				io::IReadFile* vertexShaderProgram, const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget, io::IReadFile* pixelShaderProgram,
				const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
				io::IReadFile* geometryShaderProgram, const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
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
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) _IRR_OVERRIDE_;

			//! The legacy assembly-shader entry points. Vulkan consumes SPIR-V only, so there is no
			//! assembler to run: the sources are handed to the high-level path in the default
			//! language instead, which is right for a GLSL/HLSL pair and fails with the front end's
			//! own diagnostics for anything else.
			virtual s32 addShaderMaterial(const c8* vertexShaderProgram = 0, const c8* pixelShaderProgram = 0,
				IShaderConstantSetCallBack* callback = 0, E_MATERIAL_TYPE baseMaterial = EMT_SOLID,
				s32 userData = 0) _IRR_OVERRIDE_;
			virtual s32 addShaderMaterialFromFiles(const io::path& vertexShaderProgramFileName,
				const io::path& pixelShaderProgramFileName, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = EMT_SOLID, s32 userData = 0) _IRR_OVERRIDE_;
			virtual s32 addShaderMaterialFromFiles(io::IReadFile* vertexShaderProgram,
				io::IReadFile* pixelShaderProgram, IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = EMT_SOLID, s32 userData = 0) _IRR_OVERRIDE_;

			// --- IMaterialRendererServices, the IShaderConstantSetCallBack side of the API. Every
			// method below only means anything during the CallBack->OnSetConstants() bindDrawState()
			// issues: ActiveMaterialRendererIndex names the user material being drawn there, and is
			// -1 the rest of the time, so a stray call fails cleanly with -1/false.

			//! Nothing to do: Vulkan wants every render state at pipeline-creation time, so the
			//! material is already baked into the SVulkanPipelineKey by the time a renderer could
			//! ask for it -- the same empty body the D3D12 backend keeps, for the same reason.
			virtual void setBasicRenderStates(const SMaterial& material, const SMaterial& lastMaterial,
				bool resetAllRenderstates) _IRR_OVERRIDE_ {}

			virtual s32 getVertexShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual s32 getPixelShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual s32 getGeometryShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual s32 getHullShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual s32 getDomainShaderConstantID(const c8* name) _IRR_OVERRIDE_;

			virtual bool setVertexShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			virtual bool setVertexShaderConstant(s32 index, const s32* ints, int count) _IRR_OVERRIDE_;
			virtual bool setPixelShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			virtual bool setPixelShaderConstant(s32 index, const s32* ints, int count) _IRR_OVERRIDE_;
			virtual bool setGeometryShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			virtual bool setGeometryShaderConstant(s32 index, const s32* ints, int count) _IRR_OVERRIDE_;
			virtual bool setHullShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			virtual bool setHullShaderConstant(s32 index, const s32* ints, int count) _IRR_OVERRIDE_;
			virtual bool setDomainShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			virtual bool setDomainShaderConstant(s32 index, const s32* ints, int count) _IRR_OVERRIDE_;

			//! Wider scalar types, every stage. These never transpose: only the f32 path can be
			//! looking at a 4x4 matrix. What the shader may actually declare is a shader-model
			//! question, not an API one.
			virtual bool setVertexShaderConstant(s32 index, const u32* uints, int count) _IRR_OVERRIDE_;
			virtual bool setVertexShaderConstant(s32 index, const f64* doubles, int count) _IRR_OVERRIDE_;
			virtual bool setVertexShaderConstant(s32 index, const s64* longs, int count) _IRR_OVERRIDE_;
			virtual bool setVertexShaderConstant(s32 index, const u64* ulongs, int count) _IRR_OVERRIDE_;
			virtual bool setPixelShaderConstant(s32 index, const u32* uints, int count) _IRR_OVERRIDE_;
			virtual bool setPixelShaderConstant(s32 index, const f64* doubles, int count) _IRR_OVERRIDE_;
			virtual bool setPixelShaderConstant(s32 index, const s64* longs, int count) _IRR_OVERRIDE_;
			virtual bool setPixelShaderConstant(s32 index, const u64* ulongs, int count) _IRR_OVERRIDE_;
			virtual bool setGeometryShaderConstant(s32 index, const u32* uints, int count) _IRR_OVERRIDE_;
			virtual bool setGeometryShaderConstant(s32 index, const f64* doubles, int count) _IRR_OVERRIDE_;
			virtual bool setGeometryShaderConstant(s32 index, const s64* longs, int count) _IRR_OVERRIDE_;
			virtual bool setGeometryShaderConstant(s32 index, const u64* ulongs, int count) _IRR_OVERRIDE_;
			virtual bool setHullShaderConstant(s32 index, const u32* uints, int count) _IRR_OVERRIDE_;
			virtual bool setHullShaderConstant(s32 index, const f64* doubles, int count) _IRR_OVERRIDE_;
			virtual bool setHullShaderConstant(s32 index, const s64* longs, int count) _IRR_OVERRIDE_;
			virtual bool setHullShaderConstant(s32 index, const u64* ulongs, int count) _IRR_OVERRIDE_;
			virtual bool setDomainShaderConstant(s32 index, const u32* uints, int count) _IRR_OVERRIDE_;
			virtual bool setDomainShaderConstant(s32 index, const f64* doubles, int count) _IRR_OVERRIDE_;
			virtual bool setDomainShaderConstant(s32 index, const s64* longs, int count) _IRR_OVERRIDE_;
			virtual bool setDomainShaderConstant(s32 index, const u64* ulongs, int count) _IRR_OVERRIDE_;

			//! Compute stage: meaningful only during a compute material's OnSetConstants(), which
			//! dispatchComputeShader() issues with ActiveMaterialRendererIndex naming that material.
			virtual s32 getComputeShaderConstantID(const c8* name) _IRR_OVERRIDE_;
			virtual bool setComputeShaderConstant(s32 index, const f32* floats, int count) _IRR_OVERRIDE_;
			virtual bool setComputeShaderConstant(s32 index, const s32* ints, int count) _IRR_OVERRIDE_;
			virtual bool setComputeShaderConstant(s32 index, const u32* uints, int count) _IRR_OVERRIDE_;
			virtual bool setComputeShaderConstant(s32 index, const f64* doubles, int count) _IRR_OVERRIDE_;
			virtual bool setComputeShaderConstant(s32 index, const s64* longs, int count) _IRR_OVERRIDE_;
			virtual bool setComputeShaderConstant(s32 index, const u64* ulongs, int count) _IRR_OVERRIDE_;

			//! Register-based path: assembly shaders have no Vulkan equivalent, see addShaderMaterial().
			virtual void setVertexShaderConstant(const f32* data, s32 startRegister,
				s32 constantAmount = 1) _IRR_OVERRIDE_;
			virtual void setPixelShaderConstant(const f32* data, s32 startRegister,
				s32 constantAmount = 1) _IRR_OVERRIDE_;

			virtual IVideoDriver* getVideoDriver() _IRR_OVERRIDE_ { return this; }

			//! Whole-block access next to the per-variable setters above, for a caller that would
			//! rather hand over its own struct than name every field. Not part of
			//! IMaterialRendererServices; same -1/false-outside-OnSetConstants() rule as the rest.
			s32 getShaderConstantBufferID(const c8* name, E_SHADER_TYPE stage);
			bool setShaderConstantBuffer(s32 id, const void* data, size_t dataSizeBytes, E_SHADER_TYPE stage);

			// --- IVulkanUploadContext: one-shot upload command buffers for textures and buffers ---
			virtual VkCommandBuffer beginUpload() _IRR_OVERRIDE_;
			virtual void endUploadAndWait(VkCommandBuffer commandBuffer) _IRR_OVERRIDE_;

			//! Shared device state handed to textures, buffers and the pipeline cache instead of a
			//! back-pointer to this class.
			const SVulkanContext& getContext() const { return Context; }

		protected:
			virtual ITexture* createDeviceDependentTexture(IImage* surface, const io::path& name,
				void* mipmapData = 0) _IRR_OVERRIDE_;

			//! False on a deferred context: the device, the loader and the process-wide shader
			//! compiler state belong to the immediate driver and are left alone by the destructor.
			bool OwnsDevice = true;
			//! The driver whose caches own every texture, buffer and material this one draws with:
			//! `this` on the immediate driver, the immediate driver on a deferred context. Resources
			//! are created against its Context and upload context so they outlive a deferred context,
			//! and the material registry is always read through it.
			CVulkanDriver* ResourceOwner = nullptr;
			//! Serialize the shared graphics queue (submit, present, upload) and the one-shot upload
			//! scope between the immediate driver and deferred contexts recording on other threads.
			//! Only the ResourceOwner's instances are ever locked.
			std::mutex QueueMutex;
			std::recursive_mutex UploadMutex;

			bool createInstance();
			//! Hooks the debug-utils messenger up to the log. No-op unless the instance was built
			//! with VK_EXT_debug_utils; never fatal, so it returns nothing.
			void createDebugMessenger();
			bool createSurface(HWND hwnd);
			//! Picks a discrete GPU with graphics+present support and the required extensions,
			//! falling back to the first suitable device.
			bool pickPhysicalDevice();
			bool createLogicalDevice();
			bool createSwapchain(const core::dimension2d<u32>& size);
			void destroySwapchain();
			//! Tears down and rebuilds the swapchain after a resize or an OUT_OF_DATE present.
			bool recreateSwapchain(const core::dimension2d<u32>& size);
			bool createFrameContexts();
			void destroyFrameContexts();
			//! Compiles the embedded SPIR-V (CVulkanDefaultShaders.h) into VkShaderModules and
			//! registers one material renderer per built-in E_MATERIAL_TYPE.
			bool createBuiltInMaterialRenderers();
			//! Set layouts for sets 0..4 plus the one pipeline layout every built-in draw uses. Sets
			//! 1..3 are empty: a layout has no holes, and those belong to user shaders.
			bool createDescriptorLayouts();
			//! 1x1 opaque white, bound wherever a material leaves a texture layer unset -- set 0 has
			//! a fixed two-binding layout that must be complete on every draw.
			bool createNullTexture();
			//! Sub-allocates from the current frame's uniform ring; returns the offset, or
			//! VK_WHOLE_SIZE when the ring is exhausted.
			VkDeviceSize allocateUniform(const void* data, size_t sizeBytes);
			//! Makes room for `bytes` of uniforms in one contiguous ring, growing it if needed, so a
			//! run of allocateUniform() calls whose descriptors share one VkBuffer cannot straddle a
			//! growth. Call once before writing a descriptor set's blocks.
			bool reserveUniforms(VkDeviceSize bytes);
			//! Retires the current ring and allocates one at least `needed` bytes big.
			bool growUniformRing(VkDeviceSize needed);
			//! Appends one more descriptor pool to the current frame. Called when the pools it
			//! already has are full.
			bool addDescriptorPool(SVulkanFrameContext& frame);
			//! One set from the current frame's pool, which beginScene() reset. VK_NULL_HANDLE
			//! (warned once) when the pool is full for this frame.
			VkDescriptorSet allocateDescriptorSet(VkDescriptorSetLayout layout);
			//! Begins dynamic rendering on the bound render target, or on the current swapchain
			//! image when none is set, with the clear values the caller asked for.
			void beginRendering(bool clearColor, bool clearDepth, SColor color);
			void endRendering();
			//! Puts the swapchain back in play after a render target pass, without clearing.
			void unbindRenderTarget();

			//! The registered built-in renderer for a material type, or 0 for an index that was never
			//! registered, holds a user shader, or holds a renderer this driver did not build.
			CVulkanMaterialRenderer* getNativeRenderer(s32 index) const;

			//! The registered user shader material for a type, or 0 when that index is a built-in.
			//! The two lookups are exclusive: a registration is one or the other.
			CVulkanUserMaterial* getUserMaterial(s32 index) const;

			//! The registered compute material for a type, or 0 for anything else.
			CVulkanComputeMaterial* getComputeMaterial(s32 index) const;

			//! Brings `buffer`'s hardware copy up to date (creating it on first use) and returns it.
			//! 0 (logged) when the buffer is empty or the allocation failed.
			CVulkanHardwareBuffer* prepareComputeBuffer(scene::IComputeBuffer* buffer);

			//! Runs the active compute material's OnSetConstants() with ActiveMaterialRendererIndex
			//! set, so the compute constant setters above write into that material's scratch.
			void runComputeCallback(CVulkanComputeMaterial* material);

			//! The shared tail of dispatchComputeShaderBound() and dispatchComputeShaderIndirect():
			//! translates the SRV/UAV slots into a binding table, brings every bound buffer up to
			//! date, then records and waits for one dispatch. `indirectArgs` null means a direct
			//! dispatch of `groupCount`.
			void dispatchBoundResources(const core::vector3d<u32>& groupCount,
				CVulkanHardwareBuffer* indirectArgs, u32 indirectOffset);

			//! The stencil half of a pipeline key that SMaterial cannot express, for the two shadow
			//! passes; everything else in the key still comes from the material.
			struct SVulkanStencilOverride
			{
				VkCompareOp CompareOp = VK_COMPARE_OP_ALWAYS;
				VkStencilOp FailOp = VK_STENCIL_OP_KEEP;
				VkStencilOp DepthFailOp = VK_STENCIL_OP_KEEP;
				VkStencilOp PassOp = VK_STENCIL_OP_KEEP;
			};

			//! Whether the bound depth attachment carries a stencil aspect at all.
			bool currentTargetHasStencil() const;

			//! Suspends the running dynamic rendering instance, so a barrier, copy, query reset or
			//! dispatch can be recorded on the frame's command buffer, and resumeRendering() reopens
			//! it on the same attachments without clearing. Both are no-ops outside a scene.
			void suspendRendering();
			void resumeRendering();

			//! What a user material's pipelines are created against, built once at registration.
			//! Sets the shader leaves unused get the empty layout: a VkPipelineLayout is indexed by
			//! set number and cannot carry a hole.
			struct SVulkanUserLayout
			{
				VkDescriptorSetLayout SetLayouts[MaxUserDescriptorSets] = {};
				VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
			};

			//! Turns CVulkanUserMaterial::getDescriptorSetLayoutBindings() into the layouts above,
			//! through LayoutCache so materials declaring the same bindings share one object.
			//! False (logged) when a layout could not be created.
			bool buildUserMaterialLayout(const CVulkanUserMaterial& material, SVulkanUserLayout& out);

			//! Compiles, reflects and registers one user material -- the single point every
			//! addHighLevelShaderMaterial*/addShaderMaterial* overload funnels into, once its sources
			//! are in memory. `stages` holds EVUS_COUNT entries. -1 (logged) on any failure, with
			//! nothing registered.
			//! `streamOutputLayout` is the vertexTypeOut of the public API: with a geometry stage, its
			//! attributes become the transform feedback layout of that stage (see CVulkanSpirvXfb.h).
			s32 registerUserShaderMaterial(const SVulkanUserShaderStageSource* stages,
				E_GPU_SHADING_LANGUAGE lang, IShaderConstantSetCallBack* callback,
				E_MATERIAL_TYPE baseMaterial, s32 userData, IVertexDescriptor* streamOutputLayout = nullptr);

			//! Transform feedback around one draw: bindDrawState() begins it after the pipeline is
			//! bound (a pipeline cannot be bound while it is active), the draw sites end it.
			void beginTransformFeedbackForDraw();
			void endTransformFeedbackForDraw();
			//! Makes what the bound stream-output target received readable as vertex data and its
			//! byte count readable as an indirect argument, then forgets the target.
			void releaseStreamOutputTarget();

			//! Everything a draw's pipeline needs from whichever renderer owns this material type.
			//! Resolved once and used both for the key and for pipeline creation, so the two cannot
			//! name different modules or a different layout.
			struct SVulkanDrawProgram
			{
				VkShaderModule Vertex = VK_NULL_HANDLE;
				VkShaderModule Fragment = VK_NULL_HANDLE;
				VkShaderModule Geometry = VK_NULL_HANDLE;
				VkShaderModule TessControl = VK_NULL_HANDLE;
				VkShaderModule TessEval = VK_NULL_HANDLE;
				VkPipelineLayout Layout = VK_NULL_HANDLE;
				//! Per stage: a user material compiled from D3D-style HLSL names each stage's entry
				//! point differently ("vsMain", "psMain", ...); the built-ins are all "main".
				SVulkanStageEntryPoints EntryPoints;
			};

			//! A user material's own five modules and layout, or the built-in pair (its second-UV
			//! variant on an EVT_2TCOORDS mesh, EMT_SOLID's for an unregistered type) plus the shared
			//! BuiltInPipelineLayout.
			void resolveDrawProgram(const SMaterial& material, IVertexDescriptor* descriptor,
				SVulkanDrawProgram& out) const;

			//! Every pipeline state SMaterial and the bound attachments determine. The vertex layout
			//! hash comes from `vertexInput` and the module/layout hashes from `program`, so caller
			//! and pipeline creation cannot disagree.
			SVulkanPipelineKey buildPipelineKeyFromMaterial(const SMaterial& material,
				const SVulkanDrawProgram& program, const SVulkanVertexInputState& vertexInput,
				VkPrimitiveTopology topology, const SVulkanStencilOverride* stencil = nullptr) const;

			//! Pipeline + descriptor sets for one draw: the counterpart of the D3D12 driver's
			//! bindDrawState(). False (already logged, or silent outside a scene) means "skip it".
			bool bindDrawState(const SMaterial& material, const core::matrix4& world,
				const core::matrix4& view, const core::matrix4& proj, IVertexDescriptor* descriptor,
				const SVulkanVertexInputState& vertexInput, VkPrimitiveTopology topology,
				const SVulkanStencilOverride* stencil = nullptr);

			//! Set 4: the five uniform blocks, each sub-allocated from the frame's ring. Bound
			//! against `layout`, which is the drawing pipeline's own -- set 4 is identical in all of
			//! them, so the sets stay compatible.
			bool bindDriverUniforms(const SMaterial& material, const core::matrix4& world,
				const core::matrix4& view, const core::matrix4& proj, VkPipelineLayout layout);
			//! Set 0: TextureLayer[0] and [1] as combined image samplers, NullTexture for the rest.
			//! Built-in materials only -- a user shader owns set 0 and gets bindUserMaterialDescriptors().
			bool bindMaterialTextures(const SMaterial& material, VkPipelineLayout layout);

			//! Sets 0..MaxUserDescriptorSets-1 of a user shader, one descriptor set per used set:
			//! every uniform block through the frame's ring, every sampler from the matching
			//! SMaterial::TextureLayer. A block declared by two stages is ONE descriptor with two
			//! scratch mirrors, so the first mirror found is the one uploaded.
			bool bindUserMaterialDescriptors(CVulkanUserMaterial& shader, const SMaterial& material,
				const SVulkanUserLayout& layout);

			//! Uploads `vertices` to the frame's immediate ring and draws them non-indexed.
			void drawImmediate(const S3DVertex* vertices, u32 vertexCount, VkPrimitiveTopology topology,
				const SMaterial& material, const core::matrix4& world, const core::matrix4& view,
				const core::matrix4& proj, const SVulkanStencilOverride* stencil = nullptr);
			//! The tail every 2D primitive shares: scissor from `clip`, ImmediateVertices drawn
			//! through build2DProjection() with identity world and view, then the scissor restored.
			void draw2DImmediate(VkPrimitiveTopology topology, const SMaterial& material,
				const core::rect<s32>* clip);
			//! The unlit, depth-test-free material every 2D primitive draws with.
			SMaterial build2DMaterial(bool alphaBlend, video::ITexture* texture) const;
			//! `clip` == 0 restores the whole render target.
			void setScissorFromClip(const core::rect<s32>* clip);

			//! Common tail of both setRenderTarget() overloads: binds `RenderTarget`, updates the
			//! current size/viewport and reopens dynamic rendering.
			bool activateRenderTarget(bool clearColor, bool clearDepth, SColor color);

			core::stringw Name;
			//! getVendorInfo(); filled from VkPhysicalDeviceProperties once the device is up.
			core::stringc VendorInfo = "Vulkan (native)";
			SIrrlichtCreationParameters Params;
			HWND Window = 0;

			VkInstance Instance = VK_NULL_HANDLE;
			VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
			//! Whether VK_EXT_debug_utils was actually enabled on Instance; gates the messenger.
			bool DebugUtilsEnabled = false;
			VkSurfaceKHR Surface = VK_NULL_HANDLE;
			SVulkanContext Context;

			VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
			VkFormat SwapchainFormat = VK_FORMAT_UNDEFINED;
			VkExtent2D SwapchainExtent = { 0, 0 };
			std::vector<VkImage> SwapchainImages;
			std::vector<VkImageView> SwapchainImageViews;
			//! Index returned by the last AcquireNextImageKHR; the image being rendered to.
			u32 CurrentImageIndex = 0;
			//! Whether the surface granted TRANSFER_SRC on its images, i.e. whether a screenshot can
			//! be copied out of them at all. Optional per the spec, so it has to be checked.
			bool SwapchainCanReadBack = false;

			VkImage DepthImage = VK_NULL_HANDLE;
			VkDeviceMemory DepthMemory = VK_NULL_HANDLE;
			VkImageView DepthImageView = VK_NULL_HANDLE;
			VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT;

			//! Frames in flight. Two is enough to overlap CPU recording with GPU execution without
			//! adding latency, the same count the D3D12 driver uses.
			static const u32 FrameCount = 2;
			SVulkanFrameContext Frames[FrameCount];
			u32 CurrentFrameIndex = 0;

			//! Dedicated pool for the one-shot upload command buffers, kept apart from the frame
			//! pools so an upload can happen outside beginScene()/endScene().
			VkCommandPool UploadPool = VK_NULL_HANDLE;

			CVulkanPipelineCache PipelineCache;
			CVulkanPipelineLayoutCache LayoutCache;
			//! The sampler every material texture binding uses, keyed on the SMaterialLayer (filter,
			//! anisotropy, wrap, LOD bias, min/max reduction, min LOD) and SMaterial::UseMipMaps --
			//! what the D3D drivers derive from the layer per draw. A texture's own sampler is only
			//! the fallback when the cache cannot create one.
			CVulkanSamplerCache Samplers;
			//! Owns the VkShaderModules the material renderers below borrow; outlives them.
			CVulkanShaderModuleCache ShaderModules;
			//! Parallel to CNullDriver::MaterialRenderers, so material.MaterialType indexes all
			//! three. A renderer this driver did not build leaves a null hole in both, as on the
			//! D3D12 side; a given index is a built-in or a user shader, never both.
			std::vector<CVulkanMaterialRenderer*> NativeRenderers;
			std::vector<CVulkanUserMaterial*> UserRenderers;
			//! Only the UserRenderers entries carry anything; a built-in draws with
			//! BuiltInPipelineLayout and needs no per-material layout.
			std::vector<SVulkanUserLayout> UserLayouts;
			//! Compute materials, same index convention. The one registry holds three kinds.
			std::vector<CVulkanComputeMaterial*> ComputeRenderers;

			//! The compute layouts, pipeline cache and descriptor pool; null until initDriver().
			CVulkanCompute* Compute = nullptr;
			//! The multi-slot bindings for dispatchComputeShaderBound(), valid until
			//! unbindComputeResources(). A slot holds a buffer or a texture, never both.
			struct SVulkanComputeSlot
			{
				scene::IComputeBuffer* Buffer = nullptr;
				ITexture* Texture = nullptr;
				bool empty() const { return !Buffer && !Texture; }
			};
			SVulkanComputeSlot ComputeSRV[EMCS_MAX_COMPUTE_SRV_SLOTS];
			SVulkanComputeSlot ComputeUAV[EMCS_MAX_COMPUTE_UAV_SLOTS];

			//! Occlusion query pool and per-node records; null until initDriver().
			CVulkanOcclusionQuery* Occlusion = nullptr;
			//! Bumped once per endScene(); the marker a query is stamped with, so a readback can tell
			//! a submitted frame from the one still being recorded (waiting on that one would hang).
			u64 FrameCounter = 1;

			//! The user material bindDrawState() is currently drawing, i.e. the one every
			//! IMaterialRendererServices method reads; -1 outside a draw. Equal to
			//! SMaterial::MaterialType, the two registries being one.
			s32 ActiveMaterialRendererIndex = -1;
			bool WarnedUserEntryPoints = false;
			bool WarnedUserDescriptorType = false;

			//! Whether createLogicalDevice() could enable the geometry stage, which is what
			//! queryFeature(EVDF_GEOMETRY_SHADER) reports: a user material may well declare one.
			bool HasGeometryShader = false;
			//! Whether occlusionQueryPrecise was enabled, i.e. whether a query counts samples exactly
			//! rather than merely non-zero when anything was visible.
			bool HasPreciseOcclusionQuery = false;
			bool WarnedNoStencil = false;

			//! Per-target blend overrides of the bound MRT set, from the IRenderTarget entries of the
			//! last setRenderTarget(array); copied into SVulkanPipelineKey::TargetOverrideMask and
			//! friends by buildPipelineKeyFromMaterial(). Bit 0 is never set: the first target follows
			//! the material, as on D3D11.
			struct SVulkanMrtBlend
			{
				u8 Mask = 0;
				bool Enable[8] = {};
				VkBlendFactor Src[8] = {};
				VkBlendFactor Dst[8] = {};
				VkColorComponentFlags Write[8] = {};
				void reset() { Mask = 0; }
			};
			SVulkanMrtBlend MrtBlend;
			bool WarnedNoIndependentBlend = false;
			//! One-time warnings for the D3D11.x material state a device lacks (const key builder).
			mutable bool WarnedNoLogicOp = false;
			mutable bool WarnedNoConservativeRaster = false;
			bool WarnedSrc1OnMrtSlot = false;
			//! setViewPorts(): the count baked into the pipeline key (1 without multiViewport).
			u32 ViewportCount = 1;

			//! Whether createLogicalDevice() enabled VK_EXT_transform_feedback; Context.HasTransformFeedback
			//! is that plus the resolved entry points.
			bool TransformFeedbackEnabled = false;
			bool WarnedNoTransformFeedback = false;
			//! The stream-output target bound by setStreamOutputBuffer(): every draw until it is unbound
			//! appends into it. StreamOutputHold keeps the hardware buffer alive meanwhile.
			CVulkanHardwareBuffer* StreamOutputTarget = nullptr;
			std::shared_ptr<video::IHardwareBuffer> StreamOutputHold;
			bool TransformFeedbackActive = false;

			// Set layouts, cached and owned by LayoutCache; copies only.
			VkDescriptorSetLayout MaterialTextureSetLayout = VK_NULL_HANDLE;	//!< set 0
			VkDescriptorSetLayout EmptySetLayout = VK_NULL_HANDLE;			//!< sets 1..3
			VkDescriptorSetLayout DriverUniformSetLayout = VK_NULL_HANDLE;	//!< set DriverDescriptorSet
			//! The single layout every built-in material's pipeline is created against.
			VkPipelineLayout BuiltInPipelineLayout = VK_NULL_HANDLE;

			//! EVT_STANDARD vertex input, built once: the 2D and immediate paths have no mesh
			//! buffer to resolve a layout from.
			SVulkanVertexInputState S3DVertexInput;
			//! Scratch for the CVulkanImmediateGeometry builders, kept to avoid a per-call malloc.
			core::array<S3DVertex> ImmediateVertices;

			//! Bound by setRenderTarget(); RenderTargetActive says whether rendering goes there
			//! rather than to the swapchain image.
			CVulkanRenderTarget* RenderTarget = nullptr;
			CVulkanDepthBufferPool* DepthPool = nullptr;
			bool RenderTargetActive = false;
			//! The swapchain extent, or the bound target's size. Drives the 2D projection.
			core::dimension2d<u32> CurrentRenderTargetSize;

			//! Stands in for every unset SMaterial::TextureLayer, see createNullTexture().
			CVulkanTexture* NullTexture = nullptr;

			//! Set between beginScene() and endScene(); drawing outside that is ignored with one
			//! logged warning, as a Vulkan command buffer that is not recording accepts nothing.
			bool SceneOpen = false;
			bool WarnedDrawOutsideScene = false;
			bool WarnedTextureLayout = false;
			bool RenderingActive = false;

			SMaterial Material;
			core::matrix4 Matrices[ETS_COUNT];
			core::rect<s32> ViewPort;
			//! setClipPlane()/enableClipPlane(); three, the count ClipPlanesCB declares.
			core::plane3df ClipPlanes[3];
			bool ClipPlaneEnabled[3] = { false, false, false };
		};

		//! Creates the Vulkan driver. Returns nullptr (logged) when the loader is absent or the
		//! device lacks dynamic rendering, so the caller can try another driver type.
		IVideoDriver* createVulkanDriver(const irr::SIrrlichtCreationParameters& params,
			io::IFileSystem* io, HWND window);

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
