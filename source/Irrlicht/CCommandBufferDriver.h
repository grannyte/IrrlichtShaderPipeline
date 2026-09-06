#pragma once
#include "IVideoDriver.h"

// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h
//
#ifndef __C_COMMAND_BUFFER_DRIVER__
#define __C_COMMAND_BUFFER_DRIVER__

#include "IrrCompileConfig.h"
#define _IRR_COMPILE_WITH_COMMAND_BUFFERS_

#ifdef _IRR_COMPILE_WITH_COMMAND_BUFFERS_

#ifdef _IRR_WINDOWS_
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <queue>
#include <functional>
#include <mutex>

#include "CNullDriver.h"
#include "SIrrCreationParameters.h"
#include "IMaterialRendererServices.h"
#include "CNullDriverCommon.h"
#include "IDeferredContext.h"

namespace irr
{
	namespace video
	{
		// CCommandBufferDriver records IVideoDriver calls instead of executing
		// them, and replays them later against a real driver via execute().
		//
		// Split in two categories, matching how the prototype was already
		// structured:
		//   - Resource creation / queries (getTexture, addTexture,
		//     createHardwareBuffer, getMaterialRenderer, ...) execute
		//     immediately against the wrapped Driver and return a real result.
		//   - State-setting / draw calls (setMaterial, drawMeshBuffer,
		//     setTransform, draw2D*, ...) are queued as
		//     std::function<void(IVideoDriver*)> and only run on execute().
		//
		// Any parameter that is still a raw IReferenceCounted*-derived pointer
		// (ITexture*, IMeshBuffer*, IMesh*) is grab()'d at record time and
		// drop()'d as the last step of its lambda, so the recorded command
		// keeps the object alive regardless of what the caller does with its
		// own reference between record and execute. Parameters already using
		// std::shared_ptr (ISceneNode, IHardwareBuffer) need no such handling
		// -- capturing the shared_ptr by value is sufficient.
		// CCommandBufferDriver is an ordinary IVideoDriver (via
		// CNullDriverCommon, non-virtual inheritance, unchanged from every
		// other driver in the codebase) that ALSO implements the small,
		// unrelated IDeferredContext control interface. There is no shared
		// base between the two, so this is plain multiple inheritance --
		// no virtual keyword needed, no diamond, no impact on downcasts
		// anywhere else in the codebase.
		class CCommandBufferDriver :
			public CNullDriverCommon,
			public IDeferredContext
		{
		public:
			// Driver is required at construction: it's both the target used
			// for synchronous passthrough calls, and the default execute()
			// target if none is supplied.
			explicit CCommandBufferDriver(IVideoDriver* immediateDriver)
				: CurrentRenderTarget(0), Driver(immediateDriver), CachedDynamicLightCount(0)
			{}

			void setImmediateDriver(IVideoDriver* immediateDriver) { Driver = immediateDriver; }

			// Inherited via IVideoDriver
			virtual bool beginScene(bool backBuffer = true, bool zBuffer = true, SColor color = SColor(255, 0, 0, 0), const SExposedVideoData& videoData = SExposedVideoData(), core::rect<s32>* sourceRect = 0) override;
			virtual bool endScene() override;
			virtual bool queryFeature(E_VIDEO_DRIVER_FEATURE feature) const override;
			virtual void disableFeature(E_VIDEO_DRIVER_FEATURE feature, bool flag = true) override;
			virtual const io::IAttributes& getDriverAttributes() const override;
			virtual bool checkDriverReset() override;
			virtual void setTransform(E_TRANSFORMATION_STATE state, const core::matrix4& mat) override;
			virtual const core::matrix4& getTransform(E_TRANSFORMATION_STATE state) const override;
			virtual u32 getImageLoaderCount() const override;
			virtual IImageLoader* getImageLoader(u32 n) override;
			virtual u32 getImageWriterCount() const override;
			virtual IImageWriter* getImageWriter(u32 n) override;
			virtual void setMaterial(const SMaterial& material) override;
			virtual ITexture* getTexture(const io::path& filename) override;
			virtual ITexture* getTexture(io::IReadFile* file) override;
			virtual ITexture* getTextureByIndex(u32 index) override;
			virtual ITexture* getTexture(const core::array<io::path>& files, E_TEXTURE_TYPE Type) override;
			virtual u32 getTextureCount() const override;
			virtual void renameTexture(ITexture* texture, const io::path& newName) override;
			virtual ITexture* addTexture(const core::dimension2d<u32>& size, const io::path& name, ECOLOR_FORMAT format = ECF_A8R8G8B8) override;
			virtual ITexture* addTexture(const io::path& name, IImage* image, void* mipmapData = 0) override;
			virtual ITexture* addRenderTargetTexture(const core::dimension2d<u32>& size, const io::path& name = "rt", const ECOLOR_FORMAT format = ECF_UNKNOWN) override;
			virtual void removeTexture(ITexture* texture) override;
			virtual void removeAllTextures() override;
			virtual std::shared_ptr<video::IHardwareBuffer> createHardwareBuffer(scene::IIndexBuffer* indexBuffer) override;
			virtual std::shared_ptr<video::IHardwareBuffer> createHardwareBuffer(scene::IVertexBuffer* vertexBuffer) override;
			virtual void addOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, const scene::IMesh* mesh = 0) override;
			virtual void removeOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node) override;
			virtual void removeAllOcclusionQueries() override;
			virtual void runOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool visible = false) override;
			virtual void runAllOcclusionQueries(bool visible = false) override;
			virtual void updateOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool block = true) override;
			virtual void updateAllOcclusionQueries(bool block = true) override;
			virtual u32 getOcclusionQueryResult(std::shared_ptr<irr::scene::ISceneNode> node) const override;
			virtual void makeColorKeyTexture(video::ITexture* texture, video::SColor color, bool zeroTexels = false) const override;
			virtual void makeColorKeyTexture(video::ITexture* texture, core::position2d<s32> colorKeyPixelPos, bool zeroTexels = false) const override;
			virtual void makeNormalMapTexture(video::ITexture* texture, f32 amplitude = 1.0f) const override;
			virtual bool setRenderTarget(video::ITexture* texture, bool clearBackBuffer = true, bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0), video::ITexture* depthStencil = 0) override;
			virtual bool setRenderTarget(const core::array<video::IRenderTarget>& texture, const core::array<bool>& clearBackBuffer, bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0), video::ITexture* depthStencil = 0) override;
			virtual bool setRenderTarget(E_RENDER_TARGET target, bool clearTarget = true, bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0)) override;
			virtual bool setStreamOutputBuffer(scene::IVertexBuffer* buffer) override;
			virtual void setViewPort(const core::rect<s32>& area) override;
			virtual void draw2DVertexPrimitiveList(const void* vertices, u32 vertexCount, const void* indexList, u32 primCount, E_VERTEX_TYPE vType = EVT_STANDARD, scene::E_PRIMITIVE_TYPE pType = scene::EPT_TRIANGLES, E_INDEX_TYPE iType = EIT_16BIT) override;
			virtual void draw3DLine(const core::vector3df& start, const core::vector3df& end, SColor color = SColor(255, 255, 255, 255)) override;
			virtual void draw3DTriangle(const core::triangle3df& triangle, SColor color = SColor(255, 255, 255, 255)) override;
			virtual void draw3DBox(const core::aabbox3d<f32>& box, SColor color = SColor(255, 255, 255, 255)) override;
			virtual void draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos) override;
			virtual void draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos, const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect = 0, SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false) override;
			virtual void draw2DImageBatch(const video::ITexture* texture, const core::position2d<s32>& pos, const core::array<core::rect<s32>>& sourceRects, const core::array<s32>& indices, s32 kerningWidth = 0, const core::rect<s32>* clipRect = 0, SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false) override;
			virtual void draw2DImageBatch(const video::ITexture* texture, const core::array<core::position2d<s32>>& positions, const core::array<core::rect<s32>>& sourceRects, const core::rect<s32>* clipRect = 0, SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false) override;
			virtual void draw2DImage(const video::ITexture* texture, const core::rect<s32>& destRect, const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect = 0, const video::SColor* const colors = 0, bool useAlphaChannelOfTexture = false) override;
			virtual void draw2DRectangle(SColor color, const core::rect<s32>& pos, const core::rect<s32>* clip = 0) override;
			virtual void draw2DRectangle(const core::rect<s32>& pos, SColor colorLeftUp, SColor colorRightUp, SColor colorLeftDown, SColor colorRightDown, const core::rect<s32>* clip = 0) override;
			virtual void draw2DRectangleOutline(const core::recti& pos, SColor color = SColor(255, 255, 255, 255)) override;
			virtual void draw2DLine(const core::position2d<s32>& start, const core::position2d<s32>& end, SColor color = SColor(255, 255, 255, 255)) override;
			virtual void drawPixel(u32 x, u32 y, const SColor& color) override;
			virtual void draw2DPolygon(core::position2d<s32> center, f32 radius, video::SColor color = SColor(100, 255, 255, 255), s32 vertexCount = 10) override;
			virtual void drawStencilShadowVolume(const core::array<core::vector3df>& triangles, bool zfail = true, u32 debugDataVisible = 0) override;
			virtual void drawStencilShadow(bool clearStencilBuffer = false, video::SColor leftUpEdge = video::SColor(255, 0, 0, 0), video::SColor rightUpEdge = video::SColor(255, 0, 0, 0), video::SColor leftDownEdge = video::SColor(255, 0, 0, 0), video::SColor rightDownEdge = video::SColor(255, 0, 0, 0)) override;
			virtual void drawMeshBuffer(const scene::IMeshBuffer* mb) override;
			virtual void drawMeshBufferNormals(const scene::IMeshBuffer* mb, f32 length = 10.f, SColor color = 0xffffffff) override;
			virtual void setFog(SColor color = SColor(0, 255, 255, 255), E_FOG_TYPE fogType = EFT_FOG_LINEAR, f32 start = 50.0f, f32 end = 100.0f, f32 density = 0.01f, bool pixelFog = false, bool rangeFog = false) override;
			virtual void getFog(SColor& color, E_FOG_TYPE& fogType, f32& start, f32& end, f32& density, bool& pixelFog, bool& rangeFog) override;
			virtual ECOLOR_FORMAT getColorFormat() const override;
			virtual const core::dimension2d<u32>& getScreenSize() const override;
			virtual const core::dimension2d<u32>& getCurrentRenderTargetSize() const override;
			virtual s32 getFPS() const override;
			virtual u32 getPrimitiveCountDrawn(u32 mode = 0) const override;
			virtual void deleteAllDynamicLights() override;
			virtual s32 addDynamicLight(const SLight& light) override;
			virtual u32 getMaximalDynamicLightAmount() const override;
			virtual u32 getDynamicLightCount() const override;
			virtual const SLight& getDynamicLight(u32 idx) const override;
			virtual void turnLightOn(s32 lightIndex, bool turnOn) override;
			virtual const wchar_t* getName() const override;
			virtual void addExternalImageLoader(IImageLoader* loader) override;
			virtual void addExternalImageWriter(IImageWriter* writer) override;
			virtual u32 getMaximalPrimitiveCount() const override;
			virtual void setTextureCreationFlag(E_TEXTURE_CREATION_FLAG flag, bool enabled = true) override;
			virtual bool getTextureCreationFlag(E_TEXTURE_CREATION_FLAG flag) const override;
			virtual IImage* createImageFromFile(const io::path& filename) override;
			virtual IImage* createImageFromFile(io::IReadFile* file) override;
			virtual bool writeImageToFile(IImage* image, const io::path& filename, u32 param = 0) override;
			virtual bool writeImageToFile(IImage* image, io::IWriteFile* file, u32 param = 0) override;
			virtual IImage* createImageFromData(ECOLOR_FORMAT format, const core::dimension2d<u32>& size, void* data, bool ownForeignMemory = false, bool deleteMemory = true) override;
			virtual IImage* createImage(ECOLOR_FORMAT format, const core::dimension2d<u32>& size) override;
			virtual _IRR_DEPRECATED_ IImage* createImage(ECOLOR_FORMAT format, IImage* imageToCopy) override;
			virtual _IRR_DEPRECATED_ IImage* createImage(IImage* imageToCopy, const core::position2d<s32>& pos, const core::dimension2d<u32>& size) override;
			virtual IImage* createImage(ITexture* texture, const core::position2d<s32>& pos, const core::dimension2d<u32>& size) override;
			virtual void OnResize(const core::dimension2d<u32>& size) override;
			virtual s32 addMaterialRenderer(IMaterialRenderer* renderer, const c8* name = 0) override;
			virtual IMaterialRenderer* getMaterialRenderer(u32 idx) override;
			virtual u32 getMaterialRendererCount() const override;
			virtual const c8* getMaterialRendererName(u32 idx) const override;
			virtual void setMaterialRendererName(s32 idx, const c8* name) override;
			virtual io::IAttributes* createAttributesFromMaterial(const video::SMaterial& material, io::SAttributeReadWriteOptions* options = 0) override;
			virtual void fillMaterialStructureFromAttributes(video::SMaterial& outMaterial, io::IAttributes* attributes) override;
			virtual const SExposedVideoData& getExposedVideoData() override;
			virtual E_DRIVER_TYPE getDriverType() const override;
			virtual IGPUProgrammingServices* getGPUProgrammingServices() override;
			virtual scene::IMeshManipulator* getMeshManipulator() override;
			virtual void clearZBuffer() override;
			virtual IImage* createScreenShot(video::ECOLOR_FORMAT format = video::ECF_UNKNOWN, video::E_RENDER_TARGET target = video::ERT_FRAME_BUFFER) override;
			virtual video::ITexture* findTexture(const io::path& filename) override;
			virtual bool setClipPlane(u32 index, const core::plane3df& plane, bool enable = false) override;
			virtual void enableClipPlane(u32 index, bool enable) override;
			virtual void setMinHardwareBufferVertexCount(u32 count) override;
			virtual SOverrideMaterial& getOverrideMaterial() override;
			virtual SMaterial& getMaterial2D() override;
			virtual void enableMaterial2D(bool enable = true) override;
			virtual core::stringc getVendorInfo() override;
			virtual void setAmbientLight(const SColorf& color) override;
			virtual void setAllowZWriteOnTransparent(bool flag) override;
			virtual core::dimension2du getMaxTextureSize() const override;
			virtual void convertColor(const void* sP, ECOLOR_FORMAT sF, s32 sN, void* dP, ECOLOR_FORMAT dF) const override;
			virtual IVertexDescriptor* addVertexDescriptor(const core::stringc& pName) override;
			virtual IVertexDescriptor* getVertexDescriptor(u32 id) const override;
			virtual IVertexDescriptor* getVertexDescriptor(const core::stringc& pName) const override;
			virtual u32 getVertexDescriptorCount() const override;

			// These four were missed by staying on CNullDriverCommon instead
			// of CNullDriver -- CNullDriver provides default bodies for them,
			// CNullDriverCommon doesn't. Overriding directly here (rather
			// than switching base classes) avoids inheriting CNullDriver's
			// own internal texture/material bookkeeping, which would diverge
			// from the wrapped Driver's real state.
			virtual const core::rect<s32>& getViewPort() const override;
			virtual void batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
				const irr::core::array<SColor>& color,
				const irr::core::array<core::rect<s32>>* clip = 0) override;
			virtual void batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
				irr::core::array<SColor>& colorLeftUp, irr::core::array<SColor>& colorRightUp,
				irr::core::array<SColor>& colorLeftDown, irr::core::array<SColor>& colorRightDown,
				const irr::core::array<core::rect<s32>>* clip = 0) override;

			// IGPUProgrammingServices -- all resource-creation calls that
			// hand back a synchronous material-type ID, so they pass through
			// to Driver's own IGPUProgrammingServices immediately, same
			// reasoning as addMaterialRenderer/addTexture elsewhere in this
			// class. None of these touch per-draw shader constants (those
			// live on IMaterialRendererServices, which this class doesn't
			// implement at all -- getGPUProgrammingServices() below
			// delegates to Driver, so OnSetConstants callbacks only ever run
			// against the real driver, inside the replayed drawMeshBuffer
			// lambda, which is what keeps constant-set ordering correct).
			virtual s32 addHighLevelShaderMaterial(
				const c8* vertexShaderProgram,
				const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget,
				const c8* pixelShaderProgram,
				const c8* pixelShaderEntryPointName,
				E_PIXEL_SHADER_TYPE psCompileTarget,
				const c8* geometryShaderProgram,
				const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0,
				IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID,
				IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) override;

			virtual s32 addHighLevelShaderMaterial(
				const c8* vertexShaderProgram,
				const c8* vertexShaderEntryPointName = 0,
				E_VERTEX_SHADER_TYPE vsCompileTarget = EVST_VS_1_1,
				const c8* pixelShaderProgram = 0,
				const c8* pixelShaderEntryPointName = 0,
				E_PIXEL_SHADER_TYPE psCompileTarget = EPST_PS_1_1,
				const c8* geometryShaderProgram = 0,
				const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				const c8* hullShaderProgram = 0,
				const c8* hullShaderEntryPointName = "main",
				E_HULL_SHADER_TYPE hsCompileTarget = EHST_HS_5_0,
				const c8* domainShaderProgram = 0,
				const c8* domainShaderEntryPointName = "main",
				E_DOMAIN_SHADER_TYPE dsCompileTarget = EDST_DS_5_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0,
				IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) override;

			virtual s32 addHighLevelShaderMaterialFromFiles(
				const io::path& vertexShaderProgramFileName,
				const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget,
				const io::path& pixelShaderProgramFileName,
				const c8* pixelShaderEntryPointName,
				E_PIXEL_SHADER_TYPE psCompileTarget,
				const io::path& geometryShaderProgramFileName,
				const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0,
				IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) override;

			virtual s32 addHighLevelShaderMaterialFromFiles(
				const io::path& vertexShaderProgramFile,
				const c8* vertexShaderEntryPointName = "main",
				E_VERTEX_SHADER_TYPE vsCompileTarget = EVST_VS_1_1,
				const io::path& pixelShaderProgramFile = "",
				const c8* pixelShaderEntryPointName = "main",
				E_PIXEL_SHADER_TYPE psCompileTarget = EPST_PS_1_1,
				const io::path& geometryShaderProgramFileName = "",
				const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				const io::path& hullShaderProgram = "",
				const c8* hullShaderEntryPointName = "main",
				E_HULL_SHADER_TYPE hsCompileTarget = EHST_HS_5_0,
				const io::path& domainShaderProgram = "",
				const c8* domainShaderEntryPointName = "main",
				E_DOMAIN_SHADER_TYPE dsCompileTarget = EDST_DS_5_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0,
				IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID, IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) override;

			virtual s32 addHighLevelShaderMaterialFromFiles(
				io::IReadFile* vertexShaderProgram,
				const c8* vertexShaderEntryPointName,
				E_VERTEX_SHADER_TYPE vsCompileTarget,
				io::IReadFile* pixelShaderProgram,
				const c8* pixelShaderEntryPointName,
				E_PIXEL_SHADER_TYPE psCompileTarget,
				io::IReadFile* geometryShaderProgram,
				const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0,
				IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID,
				IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0,
				E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) override;

			virtual s32 addHighLevelShaderMaterialFromFiles(
				io::IReadFile* vertexShaderProgram,
				const c8* vertexShaderEntryPointName = "main",
				E_VERTEX_SHADER_TYPE vsCompileTarget = EVST_VS_1_1,
				io::IReadFile* pixelShaderProgram = 0,
				const c8* pixelShaderEntryPointName = "main",
				E_PIXEL_SHADER_TYPE psCompileTarget = EPST_PS_1_1,
				io::IReadFile* geometryShaderProgram = 0,
				const c8* geometryShaderEntryPointName = "main",
				E_GEOMETRY_SHADER_TYPE gsCompileTarget = EGST_GS_4_0,
				io::IReadFile* hullShaderProgram = 0,
				const c8* hullShaderEntryPointName = "main",
				E_HULL_SHADER_TYPE hsCompileTarget = EHST_HS_5_0,
				io::IReadFile* domainShaderProgram = 0,
				const c8* domainShaderEntryPointName = "main",
				E_DOMAIN_SHADER_TYPE dsCompileTarget = EDST_DS_5_0,
				scene::E_PRIMITIVE_TYPE inType = scene::EPT_TRIANGLES,
				scene::E_PRIMITIVE_TYPE outType = scene::EPT_TRIANGLE_STRIP,
				u32 verticesOut = 0,
				IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID,
				IVertexDescriptor* vertexTypeOut = NULL,
				s32 userData = 0, E_GPU_SHADING_LANGUAGE shadingLang = EGSL_DEFAULT) override;

			virtual s32 addShaderMaterial(const c8* vertexShaderProgram = 0,
				const c8* pixelShaderProgram = 0,
				IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID,
				s32 userData = 0) override;

			virtual s32 addShaderMaterialFromFiles(io::IReadFile* vertexShaderProgram,
				io::IReadFile* pixelShaderProgram,
				IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID,
				s32 userData = 0) override;

			virtual s32 addShaderMaterialFromFiles(const io::path& vertexShaderProgramFileName,
				const io::path& pixelShaderProgramFileName,
				IShaderConstantSetCallBack* callback = 0,
				E_MATERIAL_TYPE baseMaterial = video::EMT_SOLID,
				s32 userData = 0) override;

			virtual s32 addComputeShader(const c8* computeShaderProgram,
				const c8* computeShaderEntryPointName = "main",
				E_COMPUTE_SHADER_TYPE csCompileTarget = ECST_CS_5_0,
				IShaderConstantSetCallBack* callback = 0,
				s32 userData = 0) override;

			virtual s32 addComputeShaderFromFile(const io::path& computeShaderProgramFileName,
				const c8* computeShaderEntryPointName = "main",
				E_COMPUTE_SHADER_TYPE csCompileTarget = ECST_CS_5_0,
				IShaderConstantSetCallBack* callback = 0,
				s32 userData = 0) override;

			// Drains the recorded queue against `driver`, or against the
			// immediate Driver supplied at construction if none is given.
			// Thread-safe: swaps the internal queue out under lock, then runs
			// it unlocked, so a producer can keep recording the next batch of
			// commands while this one replays.
			virtual void execute(IVideoDriver* driver = nullptr) override;

			// Discards any recorded-but-not-executed commands and resets this
			// recorder for reuse. NOTE: only safe to call once execute() has
			// fully drained the queue -- calling this on a non-empty queue
			// leaks every grab()'d resource inside the discarded lambdas,
			// since their drop() calls never run.
			void beginRecording() override;

			// Number of commands currently queued.
			size_t pendingCommandCount() const override;

			// Generic driver has nothing async to wait on.
			virtual void waitForCompletion() override {}

			// Lets a caller holding this object only as an IVideoDriver*
			// (the normal case while recording) reach the execute()/
			// beginRecording()/etc. control surface without a cast.
			virtual IDeferredContext* getDeferredContextControl() override { return this; }

			// Records commands for another driver to execute; it owns no render target itself.
			virtual core::dimension2d<u32> getRecordingSize() const override { return core::dimension2d<u32>(0, 0); }
			virtual ITexture* getRenderTarget() const override { return nullptr; }

		private:
			mutable std::mutex QueueMutex;
			std::queue<std::function<void(IVideoDriver*)>> deferedcalls;

			IVideoDriver* Driver;
			u32 CachedDynamicLightCount;

			// Inherited via IVideoDriver
			std::shared_ptr<IHardwareBuffer> createHardwareBuffer(scene::IComputeBuffer* computeBuffer) override;
			void dispatchComputeShader(const core::vector3d<u32>& groupCount, scene::IComputeBuffer* Src, scene::IComputeBuffer* Dst) override;

			void bindComputeBuffer(u32 slot, scene::IComputeBuffer* buffer, E_HARDWARE_BUFFER_TYPE binding) override;

			void bindComputeTexture(u32 slot, ITexture* texture, bool asUAV) override;

			void dispatchComputeShaderBound(const core::vector3d<u32>& groupCount) override;

			void unbindComputeResources() override;

			void computeBarrier(scene::IComputeBuffer* buffer) override;

			void computeBarrierAll() override;

			// Deferred: the copy is recorded in order with the dispatches that fill the buffer.
			bool beginComputeReadback(scene::IComputeBuffer* buffer, u32 slot) override;

			// Synchronous passthrough: a poll has nothing to record, and before the deferred
			// copy above has executed it simply reports "not ready".
			bool tryReadComputeBuffer(scene::IComputeBuffer* buffer, u32 slot, void* dst, u32 bytes, bool wait) override;

		protected:
			irr::video::ITexture* CurrentRenderTarget;
			core::matrix4 Matrices[ETS_COUNT];
			core::rect<s32> ViewPortCache;
		};
	}
}
#endif
#endif