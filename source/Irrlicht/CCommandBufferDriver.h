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

#include<queue>
#include<functional>

#include "CNullDriver.h"
#include "SIrrCreationParameters.h"
#include "IMaterialRendererServices.h"
#include "CNullDriverCommon.h"

namespace irr
{
	namespace video
	{

		class CCommandBufferDriver :
			public CNullDriverCommon
		{
		public:
			CCommandBufferDriver() :CurrentRenderTarget(0), Driver(0) {}
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

			virtual void execute(IVideoDriver* driver);
		private:
			mutable std::queue < std::function<void(IVideoDriver*)>> deferedcalls;
			IVideoDriver* Driver;

			// Inherited via IVideoDriver
			std::shared_ptr<IHardwareBuffer> createHardwareBuffer(scene::IComputeBuffer* computeBuffer) override;
			void dispatchComputeShader(const core::vector3d<u32>& groupCount, scene::IComputeBuffer* Src, scene::IComputeBuffer* Dst) override;
			protected:
				irr::video::ITexture* CurrentRenderTarget;
				core::matrix4 Matrices[ETS_COUNT];
		};
	}
}
#endif
#endif
