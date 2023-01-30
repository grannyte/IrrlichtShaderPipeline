#include "CCommandBufferDriver.h"
#if 0
bool irr::video::CCommandBufferDriver::beginScene(bool backBuffer = true, bool zBuffer = true, SColor color = SColor(255, 0, 0, 0), const SExposedVideoData& videoData = SExposedVideoData(), core::rect<s32>* sourceRect = 0)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->beginScene(backBuffer, zBuffer, color, videoData, sourceRect);
		});
	return true;
}

bool irr::video::CCommandBufferDriver::endScene()
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->endScene();
		});
	return true;
}

bool irr::video::CCommandBufferDriver::queryFeature(E_VIDEO_DRIVER_FEATURE feature) const
{
	return Driver->queryFeature(feature);
}

void irr::video::CCommandBufferDriver::disableFeature(E_VIDEO_DRIVER_FEATURE feature, bool flag = true)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->disableFeature(feature, flag);
		});
}

const irr::io::IAttributes& irr::video::CCommandBufferDriver::getDriverAttributes() const
{
	return Driver->getDriverAttributes();
}

bool irr::video::CCommandBufferDriver::checkDriverReset()
{
	return Driver->checkDriverReset();
}

void irr::video::CCommandBufferDriver::setTransform(E_TRANSFORMATION_STATE state, const core::matrix4& mat)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->setTransform(state, mat);
		});
}

const irr::core::matrix4& irr::video::CCommandBufferDriver::getTransform(E_TRANSFORMATION_STATE state) const
{
	// TODO: insert return statement here
}

irr::u32 irr::video::CCommandBufferDriver::getImageLoaderCount() const
{
	return Driver->getImageLoaderCount();
};

irr::video::IImageLoader* irr::video::CCommandBufferDriver::getImageLoader(u32 n)
{
	return Driver->getImageLoader(n);
}

irr::u32 irr::video::CCommandBufferDriver::getImageWriterCount() const
{
	return Driver->getImageWriterCount();
}

irr::video::IImageWriter* irr::video::CCommandBufferDriver::getImageWriter(u32 n)
{
	return Driver->getImageWriter(n);
}

void irr::video::CCommandBufferDriver::setMaterial(const SMaterial& material)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->setMaterial(material);
		});
}

irr::video::ITexture* irr::video::CCommandBufferDriver::getTexture(const io::path& filename)
{
	return Driver->getTexture(filename);
}

irr::video::ITexture* irr::video::CCommandBufferDriver::getTexture(io::IReadFile* file)
{
	return Driver->getTexture(file);
}

irr::video::ITexture* irr::video::CCommandBufferDriver::getTextureByIndex(u32 index)
{
	return Driver->getTextureByIndex(index);
}

irr::video::ITexture* irr::video::CCommandBufferDriver::getTexture(const core::array<io::path>& files, E_TEXTURE_TYPE Type)
{
	return Driver->getTexture(files,Type);
}

irr::u32 irr::video::CCommandBufferDriver::getTextureCount() const
{
	return Driver->getTextureCount();
}

void irr::video::CCommandBufferDriver::renameTexture(ITexture* texture, const io::path& newName)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->renameTexture(texture,newName);
		});
}

irr::video::ITexture* irr::video::CCommandBufferDriver::addTexture(const core::dimension2d<u32>& size, const io::path& name, ECOLOR_FORMAT format = ECF_A8R8G8B8)
{
	return Driver->addTexture(size,name,format);
}

irr::video::ITexture* irr::video::CCommandBufferDriver::addTexture(const io::path& name, IImage* image, void* mipmapData = 0)
{
	return Driver->addTexture(name,image,mipmapData);
}

irr::video::ITexture* irr::video::CCommandBufferDriver::addRenderTargetTexture(const core::dimension2d<u32>& size, const io::path& name = "rt", const ECOLOR_FORMAT format = ECF_UNKNOWN)
{
	return Driver->addRenderTargetTexture(size,name,format);
}

void irr::video::CCommandBufferDriver::removeTexture(ITexture* texture)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->removeTexture(texture);
		});

}

void irr::video::CCommandBufferDriver::removeAllTextures()
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->removeAllTextures();
		});
}

irr::video::IHardwareBuffer* irr::video::CCommandBufferDriver::createHardwareBuffer(scene::IIndexBuffer* indexBuffer)
{
	return Driver->createHardwareBuffer(indexBuffer);
}

irr::video::IHardwareBuffer* irr::video::CCommandBufferDriver::createHardwareBuffer(scene::IVertexBuffer* vertexBuffer)
{
	return Driver->createHardwareBuffer(vertexBuffer);
}

void irr::video::CCommandBufferDriver::addOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, const scene::IMesh* mesh = 0)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->addOcclusionQuery(node, mesh);
		});
}

void irr::video::CCommandBufferDriver::removeOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->removeOcclusionQuery(node);
		});
}

void irr::video::CCommandBufferDriver::removeAllOcclusionQueries()
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->removeAllOcclusionQueries();
		});
}

void irr::video::CCommandBufferDriver::runOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool visible = false)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->runOcclusionQuery(node,visible);
		});
}

void irr::video::CCommandBufferDriver::runAllOcclusionQueries(bool visible = false)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->runAllOcclusionQueries();
		});
}

void irr::video::CCommandBufferDriver::updateOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool block = true)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->updateOcclusionQuery(node,block);
		});
}

void irr::video::CCommandBufferDriver::updateAllOcclusionQueries(bool block = true)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->updateAllOcclusionQueries();
		});
}

irr::u32 irr::video::CCommandBufferDriver::getOcclusionQueryResult(std::shared_ptr<irr::scene::ISceneNode> node) const
{
	return Driver->getOcclusionQueryResult(node);
}

void irr::video::CCommandBufferDriver::makeColorKeyTexture(video::ITexture* texture, video::SColor color, bool zeroTexels = false) const
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->makeColorKeyTexture(texture, color, zeroTexels);
		});
}

void irr::video::CCommandBufferDriver::makeColorKeyTexture(video::ITexture* texture, core::position2d<s32> colorKeyPixelPos, bool zeroTexels = false) const
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->makeColorKeyTexture(texture, colorKeyPixelPos, zeroTexels);
		});
}

void irr::video::CCommandBufferDriver::makeNormalMapTexture(video::ITexture* texture, f32 amplitude = 1.0f) const
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->makeColorKeyTexture(texture, amplitude, amplitude);
		});
}

bool irr::video::CCommandBufferDriver::setRenderTarget(video::ITexture* texture, bool clearBackBuffer = true, bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0), video::ITexture* depthStencil = 0)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->setRenderTarget(texture, clearBackBuffer, clearZBuffer,color,depthStencil);
		});
	return true;
}

bool irr::video::CCommandBufferDriver::setRenderTarget(const core::array<video::IRenderTarget>& texture, bool clearBackBuffer = true, bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0), video::ITexture* depthStencil = 0)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->setRenderTarget(texture, clearBackBuffer, clearZBuffer, color, depthStencil);
		});
	return true;
}

bool irr::video::CCommandBufferDriver::setRenderTarget(E_RENDER_TARGET target, bool clearTarget = true, bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0))
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->setRenderTarget(target, clearTarget, clearZBuffer, color);
		});
	return true;
}

bool irr::video::CCommandBufferDriver::setStreamOutputBuffer(scene::IVertexBuffer* buffer)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->setStreamOutputBuffer(buffer);
		});
	return true;
}

void irr::video::CCommandBufferDriver::setViewPort(const core::rect<s32>& area)
{
	deferedcalls.push([&](IVideoDriver* driver) {
		driver->setViewPort(area);
		});
}

const irr::core::rect<irr::s32>& irr::video::CCommandBufferDriver::getViewPort() const
{
	return Driver->getViewPort();
}

void irr::video::CCommandBufferDriver::draw2DVertexPrimitiveList(const void* vertices, u32 vertexCount, const void* indexList, u32 primCount, E_VERTEX_TYPE vType = EVT_STANDARD, scene::E_PRIMITIVE_TYPE pType = scene::EPT_TRIANGLES, E_INDEX_TYPE iType = EIT_16BIT)
{
}

void irr::video::CCommandBufferDriver::draw3DLine(const core::vector3df& start, const core::vector3df& end, SColor color = SColor(255, 255, 255, 255))
{
}

void irr::video::CCommandBufferDriver::draw3DTriangle(const core::triangle3df& triangle, SColor color = SColor(255, 255, 255, 255))
{
}

void irr::video::CCommandBufferDriver::draw3DBox(const core::aabbox3d<f32>& box, SColor color = SColor(255, 255, 255, 255))
{
}

void irr::video::CCommandBufferDriver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos)
{
}

void irr::video::CCommandBufferDriver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos, const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect = 0, SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false)
{
}

void irr::video::CCommandBufferDriver::draw2DImageBatch(const video::ITexture* texture, const core::position2d<s32>& pos, const core::array<core::rect<s32>>& sourceRects, const core::array<s32>& indices, s32 kerningWidth = 0, const core::rect<s32>* clipRect = 0, SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false)
{
}

void irr::video::CCommandBufferDriver::draw2DImageBatch(const video::ITexture* texture, const core::array<core::position2d<s32>>& positions, const core::array<core::rect<s32>>& sourceRects, const core::rect<s32>* clipRect = 0, SColor color = SColor(255, 255, 255, 255), bool useAlphaChannelOfTexture = false)
{
}

void irr::video::CCommandBufferDriver::draw2DImage(const video::ITexture* texture, const core::rect<s32>& destRect, const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect = 0, const video::SColor* const colors = 0, bool useAlphaChannelOfTexture = false)
{
}

void irr::video::CCommandBufferDriver::draw2DRectangle(SColor color, const core::rect<s32>& pos, const core::rect<s32>* clip = 0)
{
}

void irr::video::CCommandBufferDriver::draw2DRectangle(const core::rect<s32>& pos, SColor colorLeftUp, SColor colorRightUp, SColor colorLeftDown, SColor colorRightDown, const core::rect<s32>* clip = 0)
{
}

void irr::video::CCommandBufferDriver::draw2DRectangleOutline(const core::recti& pos, SColor color = SColor(255, 255, 255, 255))
{
}

void irr::video::CCommandBufferDriver::draw2DLine(const core::position2d<s32>& start, const core::position2d<s32>& end, SColor color = SColor(255, 255, 255, 255))
{
}

void irr::video::CCommandBufferDriver::drawPixel(u32 x, u32 y, const SColor& color)
{
}

void irr::video::CCommandBufferDriver::draw2DPolygon(core::position2d<s32> center, f32 radius, video::SColor color = SColor(100, 255, 255, 255), s32 vertexCount = 10)
{
}

void irr::video::CCommandBufferDriver::drawStencilShadowVolume(const core::array<core::vector3df>& triangles, bool zfail = true, u32 debugDataVisible = 0)
{
}

void irr::video::CCommandBufferDriver::drawStencilShadow(bool clearStencilBuffer = false, video::SColor leftUpEdge = video::SColor(255, 0, 0, 0), video::SColor rightUpEdge = video::SColor(255, 0, 0, 0), video::SColor leftDownEdge = video::SColor(255, 0, 0, 0), video::SColor rightDownEdge = video::SColor(255, 0, 0, 0))
{
}

void irr::video::CCommandBufferDriver::drawMeshBuffer(const scene::IMeshBuffer* mb)
{
}

void irr::video::CCommandBufferDriver::drawMeshBufferNormals(const scene::IMeshBuffer* mb, f32 length = 10.f, SColor color = 0xffffffff)
{
}

void irr::video::CCommandBufferDriver::setFog(SColor color = SColor(0, 255, 255, 255), E_FOG_TYPE fogType = EFT_FOG_LINEAR, f32 start = 50.0f, f32 end = 100.0f, f32 density = 0.01f, bool pixelFog = false, bool rangeFog = false)
{
}

void irr::video::CCommandBufferDriver::getFog(SColor& color, E_FOG_TYPE& fogType, f32& start, f32& end, f32& density, bool& pixelFog, bool& rangeFog)
{
}

irr::video::ECOLOR_FORMAT irr::video::CCommandBufferDriver::getColorFormat() const
{
	return ECOLOR_FORMAT();
}

const irr::core::dimension2d<irr::u32>& irr::video::CCommandBufferDriver::getScreenSize() const
{
	// TODO: insert return statement here
}

const irr::core::dimension2d<irr::u32>& irr::video::CCommandBufferDriver::getCurrentRenderTargetSize() const
{
	// TODO: insert return statement here
}

irr::s32 irr::video::CCommandBufferDriver::getFPS() const
{
	return s32();
}

irr::u32 irr::video::CCommandBufferDriver::getPrimitiveCountDrawn(u32 mode = 0) const
{
	return u32();
}

void irr::video::CCommandBufferDriver::deleteAllDynamicLights()
{
}

irr::s32 irr::video::CCommandBufferDriver::addDynamicLight(const SLight& light)
{
	return s32();
}

irr::u32 irr::video::CCommandBufferDriver::getMaximalDynamicLightAmount() const
{
	return u32();
}

irr::u32 irr::video::CCommandBufferDriver::getDynamicLightCount() const
{
	return u32();
}

const irr::video::SLight& irr::video::CCommandBufferDriver::getDynamicLight(u32 idx) const
{
	// TODO: insert return statement here
}

void irr::video::CCommandBufferDriver::turnLightOn(s32 lightIndex, bool turnOn)
{
}

const wchar_t* irr::video::CCommandBufferDriver::getName() const
{
	return nullptr;
}

void irr::video::CCommandBufferDriver::addExternalImageLoader(IImageLoader* loader)
{
}

void irr::video::CCommandBufferDriver::addExternalImageWriter(IImageWriter* writer)
{
}

irr::u32 irr::video::CCommandBufferDriver::getMaximalPrimitiveCount() const
{
	return u32();
}

void irr::video::CCommandBufferDriver::setTextureCreationFlag(E_TEXTURE_CREATION_FLAG flag, bool enabled = true)
{
}

bool irr::video::CCommandBufferDriver::getTextureCreationFlag(E_TEXTURE_CREATION_FLAG flag) const
{
	return false;
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImageFromFile(const io::path& filename)
{
	return nullptr;
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImageFromFile(io::IReadFile* file)
{
	return nullptr;
}

bool irr::video::CCommandBufferDriver::writeImageToFile(IImage* image, const io::path& filename, u32 param = 0)
{
	return false;
}

bool irr::video::CCommandBufferDriver::writeImageToFile(IImage* image, io::IWriteFile* file, u32 param = 0)
{
	return false;
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImageFromData(ECOLOR_FORMAT format, const core::dimension2d<u32>& size, void* data, bool ownForeignMemory = false, bool deleteMemory = true)
{
	return nullptr;
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImage(ECOLOR_FORMAT format, const core::dimension2d<u32>& size)
{
	return nullptr;
}

_IRR_DEPRECATED_ irr::video::IImage* irr::video::CCommandBufferDriver::createImage(ECOLOR_FORMAT format, IImage* imageToCopy)
{
	return nullptr;
}

_IRR_DEPRECATED_ irr::video::IImage* irr::video::CCommandBufferDriver::createImage(IImage* imageToCopy, const core::position2d<s32>& pos, const core::dimension2d<u32>& size)
{
	return nullptr;
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImage(ITexture* texture, const core::position2d<s32>& pos, const core::dimension2d<u32>& size)
{
	return nullptr;
}

void irr::video::CCommandBufferDriver::OnResize(const core::dimension2d<u32>& size)
{
}

irr::s32 irr::video::CCommandBufferDriver::addMaterialRenderer(IMaterialRenderer* renderer, const c8* name = 0)
{
	return s32();
}

irr::video::IMaterialRenderer* irr::video::CCommandBufferDriver::getMaterialRenderer(u32 idx)
{
	return nullptr;
}

irr::u32 irr::video::CCommandBufferDriver::getMaterialRendererCount() const
{
	return u32();
}

const irr::c8* irr::video::CCommandBufferDriver::getMaterialRendererName(u32 idx) const
{
	return nullptr;
}

void irr::video::CCommandBufferDriver::setMaterialRendererName(s32 idx, const c8* name)
{
}

irr::io::IAttributes* irr::video::CCommandBufferDriver::createAttributesFromMaterial(const video::SMaterial& material, io::SAttributeReadWriteOptions* options = 0)
{
	return nullptr;
}

void irr::video::CCommandBufferDriver::fillMaterialStructureFromAttributes(video::SMaterial& outMaterial, io::IAttributes* attributes)
{
}

const irr::video::SExposedVideoData& irr::video::CCommandBufferDriver::getExposedVideoData()
{
	// TODO: insert return statement here
}

irr::video::E_DRIVER_TYPE irr::video::CCommandBufferDriver::getDriverType() const
{
	return E_DRIVER_TYPE();
}

irr::video::IGPUProgrammingServices* irr::video::CCommandBufferDriver::getGPUProgrammingServices()
{
	return nullptr;
}

irr::scene::IMeshManipulator* irr::video::CCommandBufferDriver::getMeshManipulator()
{
	return nullptr;
}

void irr::video::CCommandBufferDriver::clearZBuffer()
{
}

irr::video::IImage* irr::video::CCommandBufferDriver::createScreenShot(video::ECOLOR_FORMAT format = video::ECF_UNKNOWN, video::E_RENDER_TARGET target = video::ERT_FRAME_BUFFER)
{
	return nullptr;
}

irr::video::ITexture* irr::video::CCommandBufferDriver::findTexture(const io::path& filename)
{
	return nullptr;
}

bool irr::video::CCommandBufferDriver::setClipPlane(u32 index, const core::plane3df& plane, bool enable = false)
{
	return false;
}

void irr::video::CCommandBufferDriver::enableClipPlane(u32 index, bool enable)
{
}

void irr::video::CCommandBufferDriver::setMinHardwareBufferVertexCount(u32 count)
{
}

irr::video::SOverrideMaterial& irr::video::CCommandBufferDriver::getOverrideMaterial()
{
	// TODO: insert return statement here
}

irr::video::SMaterial& irr::video::CCommandBufferDriver::getMaterial2D()
{
	// TODO: insert return statement here
}

void irr::video::CCommandBufferDriver::enableMaterial2D(bool enable = true)
{
}

irr::core::stringc irr::video::CCommandBufferDriver::getVendorInfo()
{
	return core::stringc();
}

void irr::video::CCommandBufferDriver::setAmbientLight(const SColorf& color)
{
}

void irr::video::CCommandBufferDriver::setAllowZWriteOnTransparent(bool flag)
{
}

irr::core::dimension2du irr::video::CCommandBufferDriver::getMaxTextureSize() const
{
	return core::dimension2du();
}

void irr::video::CCommandBufferDriver::convertColor(const void* sP, ECOLOR_FORMAT sF, s32 sN, void* dP, ECOLOR_FORMAT dF) const
{
}

irr::video::IVertexDescriptor* irr::video::CCommandBufferDriver::addVertexDescriptor(const core::stringc& pName)
{
	return nullptr;
}

irr::video::IVertexDescriptor* irr::video::CCommandBufferDriver::getVertexDescriptor(u32 id) const
{
	return nullptr;
}

irr::video::IVertexDescriptor* irr::video::CCommandBufferDriver::getVertexDescriptor(const core::stringc& pName) const
{
	return nullptr;
}

irr::u32 irr::video::CCommandBufferDriver::getVertexDescriptorCount() const
{
	return u32();
}

void irr::video::CCommandBufferDriver::execute(IVideoDriver* driver)
{
}
#endif