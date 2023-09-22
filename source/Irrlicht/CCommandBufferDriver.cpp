#include "CCommandBufferDriver.h"

#include "IImageLoader.h"
#include "IImageWriter.h"


bool irr::video::CCommandBufferDriver::beginScene(bool backBuffer, bool zBuffer, SColor color, const SExposedVideoData& videoData, core::rect<s32>* sourceRect )
{
	deferedcalls.push([backBuffer, zBuffer, color, videoData, sourceRect](IVideoDriver* driver) {
		driver->beginScene(backBuffer, zBuffer, color, videoData, sourceRect);
		});
	return true;
}

bool irr::video::CCommandBufferDriver::endScene()
{
	deferedcalls.push([](IVideoDriver* driver) {
		driver->endScene();
		});
	return true;
}

bool irr::video::CCommandBufferDriver::queryFeature(E_VIDEO_DRIVER_FEATURE feature) const
{
	return Driver->queryFeature(feature);
}

void irr::video::CCommandBufferDriver::disableFeature(E_VIDEO_DRIVER_FEATURE feature, bool flag)
{
	deferedcalls.push([feature, flag](IVideoDriver* driver) {
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
	Matrices[state] = mat;
	deferedcalls.push([state, matb = mat
	](IVideoDriver* driver) {
		driver->setTransform(state, matb);
		});
}

const irr::core::matrix4& irr::video::CCommandBufferDriver::getTransform(E_TRANSFORMATION_STATE state) const
{
	return Matrices[state];
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
	deferedcalls.push([materialb = material](IVideoDriver* driver) {
		driver->setMaterial(materialb);
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
	deferedcalls.push([texture, newNameb = newName
	](IVideoDriver* driver) {
		driver->renameTexture(texture, newNameb);
		});
}

irr::video::ITexture* irr::video::CCommandBufferDriver::addTexture(const core::dimension2d<u32>& size, const io::path& name, ECOLOR_FORMAT format)
{
	return Driver->addTexture(size,name,format);
}

irr::video::ITexture* irr::video::CCommandBufferDriver::addTexture(const io::path& name, IImage* image, void* mipmapData)
{
	return Driver->addTexture(name,image,mipmapData);
}

irr::video::ITexture* irr::video::CCommandBufferDriver::addRenderTargetTexture(const core::dimension2d<u32>& size, const io::path& name, const ECOLOR_FORMAT format)
{
	return Driver->addRenderTargetTexture(size,name,format);
}

void irr::video::CCommandBufferDriver::removeTexture(ITexture* texture)
{
	deferedcalls.push([texture	](IVideoDriver* driver) {
		driver->removeTexture(texture);
		});

}

void irr::video::CCommandBufferDriver::removeAllTextures()
{
	deferedcalls.push([](IVideoDriver* driver) {
		driver->removeAllTextures();
		});
}

void irr::video::CCommandBufferDriver::addOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, const scene::IMesh* mesh)
{
	deferedcalls.push([node, mesh](IVideoDriver* driver) {
		driver->addOcclusionQuery(node, mesh);
		});
}

void irr::video::CCommandBufferDriver::removeOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node)
{
	deferedcalls.push([node](IVideoDriver* driver) {
		driver->removeOcclusionQuery(node);
		});
}

void irr::video::CCommandBufferDriver::removeAllOcclusionQueries()
{
	deferedcalls.push([](IVideoDriver* driver) {
		driver->removeAllOcclusionQueries();
		});
}

void irr::video::CCommandBufferDriver::runOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool visible )
{
	deferedcalls.push([ node, visible](IVideoDriver* driver) {
		driver->runOcclusionQuery(node,visible);
		});
}

void irr::video::CCommandBufferDriver::runAllOcclusionQueries(bool visible)
{
	deferedcalls.push([ visible](IVideoDriver* driver) {
		driver->runAllOcclusionQueries();
		});
}

void irr::video::CCommandBufferDriver::updateOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool block)
{
	deferedcalls.push([ node, block](IVideoDriver* driver) {
		driver->updateOcclusionQuery(node,block);
		});
}

void irr::video::CCommandBufferDriver::updateAllOcclusionQueries(bool block)
{
	deferedcalls.push([ block](IVideoDriver* driver) {
		driver->updateAllOcclusionQueries(block);
		});
}

irr::u32 irr::video::CCommandBufferDriver::getOcclusionQueryResult(std::shared_ptr<irr::scene::ISceneNode> node) const
{
	return Driver->getOcclusionQueryResult(node);
}

void irr::video::CCommandBufferDriver::makeColorKeyTexture(video::ITexture* texture, video::SColor color, bool zeroTexels ) const
{
	deferedcalls.push([ texture, color, zeroTexels](IVideoDriver* driver) {
		driver->makeColorKeyTexture(texture, color, zeroTexels);
		});
}

void irr::video::CCommandBufferDriver::makeColorKeyTexture(video::ITexture* texture, core::position2d<s32> colorKeyPixelPos, bool zeroTexels) const
{
	deferedcalls.push([ texture, colorKeyPixelPos, zeroTexels](IVideoDriver* driver) {
		driver->makeColorKeyTexture(texture, colorKeyPixelPos, zeroTexels);
		});
}

void irr::video::CCommandBufferDriver::makeNormalMapTexture(video::ITexture* texture, f32 amplitude ) const
{
	deferedcalls.push([ texture, amplitude](IVideoDriver* driver) {
		driver->makeColorKeyTexture(texture, amplitude, amplitude);
		});
}

bool irr::video::CCommandBufferDriver::setRenderTarget(video::ITexture* texture, bool clearBackBuffer , bool clearZBuffer, SColor color, video::ITexture* depthStencil)
{
	deferedcalls.push([texture, clearBackBuffer, clearZBuffer, color, depthStencil](IVideoDriver* driver) {
		driver->setRenderTarget(texture, clearBackBuffer, clearZBuffer,color,depthStencil);
		});
	return true;
}

bool irr::video::CCommandBufferDriver::setRenderTarget(const core::array<video::IRenderTarget>& texture, const core::array<bool>& clearBackBuffer, bool clearZBuffer, SColor color, video::ITexture* depthStencil)
{
	deferedcalls.push([textureb = texture, clearBackBufferb = clearBackBuffer, clearZBuffer, color, depthStencil](IVideoDriver* driver) {
		driver->setRenderTarget(textureb, clearBackBufferb, clearZBuffer, color, depthStencil);
		});
	return true;
}

bool irr::video::CCommandBufferDriver::setRenderTarget(E_RENDER_TARGET target, bool clearTarget , bool clearZBuffer, SColor color)
{
		if (ERT_FRAME_BUFFER == target)
			return setRenderTarget(0, clearTarget, clearZBuffer, color, 0);
		else
			return false;

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
	CNullDriverCommon::setViewPort(area);
	deferedcalls.push([areab = area](IVideoDriver* driver) {
		driver->setViewPort(areab);
		});
}

void irr::video::CCommandBufferDriver::draw2DVertexPrimitiveList(const void* vertices, u32 vertexCount, const void* indexList, u32 primCount, E_VERTEX_TYPE vType, scene::E_PRIMITIVE_TYPE pType, E_INDEX_TYPE iType)
{
	deferedcalls.push([vertices, vertexCount, indexList, primCount, vType, pType, iType](IVideoDriver* driver) {
		driver->draw2DVertexPrimitiveList(vertices, vertexCount, indexList, primCount, vType, pType, iType);
		});
}

void irr::video::CCommandBufferDriver::draw3DLine(const core::vector3df& start, const core::vector3df& end, SColor color)
{
	deferedcalls.push([startb = start, endb =end, color](IVideoDriver* driver) {
		driver->draw3DLine(startb, endb, color);
		});
}

void irr::video::CCommandBufferDriver::draw3DTriangle(const core::triangle3df& triangle, SColor color )
{
	deferedcalls.push([triangleb = triangle, color](IVideoDriver* driver) {
		driver->draw3DTriangle(triangleb, color);
		});
}

void irr::video::CCommandBufferDriver::draw3DBox(const core::aabbox3d<f32>& box, SColor color)
{
	deferedcalls.push([boxb = box, color](IVideoDriver* driver) {
		driver->draw3DBox(boxb, color);
		});
}

void irr::video::CCommandBufferDriver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos)
{
	deferedcalls.push([texture, destPosb = destPos](IVideoDriver* driver) {
		driver->draw2DImage(texture, destPosb);
		});
}

void irr::video::CCommandBufferDriver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos, const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect, SColor color, bool useAlphaChannelOfTexture)
{
	if (clipRect == nullptr)
	{

		deferedcalls.push([texture, destPosb = destPos, sourceRectb = sourceRect, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImage(texture, destPosb, sourceRectb, nullptr, color, useAlphaChannelOfTexture);
			});
	}
	else
	{
		deferedcalls.push([texture, destPosb = destPos, sourceRectb = sourceRect, clipRectb = *clipRect, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImage(texture, destPosb, sourceRectb, &clipRectb, color, useAlphaChannelOfTexture);
			});
	}
}

void irr::video::CCommandBufferDriver::draw2DImageBatch(const video::ITexture* texture, const core::position2d<s32>& pos, const core::array<core::rect<s32>>& sourceRects, const core::array<s32>& indices, s32 kerningWidth , const core::rect<s32>* clipRect , SColor color , bool useAlphaChannelOfTexture)
{
	if (clipRect == nullptr)
	{
		deferedcalls.push([texture, posb = pos, sourceRectsb = sourceRects, indicesb = indices, kerningWidth, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImageBatch(texture, posb, sourceRectsb, indicesb, kerningWidth, nullptr, color, useAlphaChannelOfTexture);
			});
	}
	else
	{
		deferedcalls.push([texture, posb = pos, sourceRectsb = sourceRects, indicesb = indices, kerningWidth, clipRectb = *clipRect, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImageBatch(texture, posb, sourceRectsb, indicesb, kerningWidth, &clipRectb, color, useAlphaChannelOfTexture);
			});
	}
}

void irr::video::CCommandBufferDriver::draw2DImageBatch(const video::ITexture* texture, const core::array<core::position2d<s32>>& positions, const core::array<core::rect<s32>>& sourceRects, const core::rect<s32>* clipRect, SColor color, bool useAlphaChannelOfTexture )
{
	if (clipRect == nullptr)
	{
		deferedcalls.push([texture, positionsb = positions, sourceRectsb = sourceRects, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImageBatch(texture, positionsb, sourceRectsb, nullptr, color, useAlphaChannelOfTexture);
			});
	}
	else
	{
		deferedcalls.push([texture, positionsb = positions, sourceRectsb = sourceRects, clipRectb = *clipRect, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImageBatch(texture, positionsb, sourceRectsb, &clipRectb, color, useAlphaChannelOfTexture);
			});
	}
}

void irr::video::CCommandBufferDriver::draw2DImage(const video::ITexture* texture, const core::rect<s32>& destRect, const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect, const video::SColor* const colors , bool useAlphaChannelOfTexture)
{
	if (clipRect == nullptr)
	{
		deferedcalls.push([texture, destRectb = destRect, sourceRectb = sourceRect, colors, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImage(texture, destRectb, sourceRectb, nullptr, colors, useAlphaChannelOfTexture);
			});
	}
	else
	{
		deferedcalls.push([texture, destRectb = destRect, sourceRectb = sourceRect, clipRectb = *clipRect, colors, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImage(texture, destRectb, sourceRectb, &clipRectb, colors, useAlphaChannelOfTexture);
			});
	}
}

void irr::video::CCommandBufferDriver::draw2DRectangle(SColor color, const core::rect<s32>& pos, const core::rect<s32>* clip)
{
	if (clip == nullptr)
	{
		deferedcalls.push([color, posb = pos](IVideoDriver* driver) {
			driver->draw2DRectangle(color, posb, nullptr);
			});
	}
	else
	{
		deferedcalls.push([color, posb = pos, clipb = *clip](IVideoDriver* driver) {
			driver->draw2DRectangle(color, posb, &clipb);
			});
	}
}

void irr::video::CCommandBufferDriver::draw2DRectangle(const core::rect<s32>& pos, SColor colorLeftUp, SColor colorRightUp, SColor colorLeftDown, SColor colorRightDown, const core::rect<s32>* clip)
{
	if (clip == nullptr)
	{
		deferedcalls.push([posb = pos, colorLeftUp, colorRightUp, colorLeftDown, colorRightDown](IVideoDriver* driver) {
			driver->draw2DRectangle(posb, colorLeftUp, colorRightUp, colorLeftDown, colorRightDown, nullptr);
			});
	}
	else
	{
		deferedcalls.push([posb = pos, colorLeftUp, colorRightUp, colorLeftDown, colorRightDown, clipb = *clip](IVideoDriver* driver) {
			driver->draw2DRectangle(posb, colorLeftUp, colorRightUp, colorLeftDown, colorRightDown, &clipb);
			});
	}
}

void irr::video::CCommandBufferDriver::draw2DRectangleOutline(const core::recti& pos, SColor color)
{
	deferedcalls.push([posb = pos, color](IVideoDriver* driver) {
		driver->draw2DRectangleOutline(posb, color);
		});
}

void irr::video::CCommandBufferDriver::draw2DLine(const core::position2d<s32>& start, const core::position2d<s32>& end, SColor color)
{
	deferedcalls.push([startb = start, endb = end, color](IVideoDriver* driver) {
		driver->draw2DLine(startb, endb, color);
		});
}

void irr::video::CCommandBufferDriver::drawPixel(u32 x, u32 y, const SColor& color)
{
	deferedcalls.push([x, y, colorb = color](IVideoDriver* driver) {
		driver->drawPixel(x, y, colorb);
		});
}

void irr::video::CCommandBufferDriver::draw2DPolygon(core::position2d<s32> center, f32 radius, video::SColor color, s32 vertexCount)
{
	deferedcalls.push([centerb = center, radius, color, vertexCount](IVideoDriver* driver) {
		driver->draw2DPolygon(centerb, radius, color, vertexCount);
		});
}

void irr::video::CCommandBufferDriver::drawStencilShadowVolume(const core::array<core::vector3df>& triangles, bool zfail, u32 debugDataVisible)
{
	deferedcalls.push([trianglesb = triangles, zfail, debugDataVisible](IVideoDriver* driver) {
		driver->drawStencilShadowVolume(trianglesb, zfail, debugDataVisible);
		});
}

void irr::video::CCommandBufferDriver::drawStencilShadow(bool clearStencilBuffer, video::SColor leftUpEdge, video::SColor rightUpEdge, video::SColor leftDownEdge, video::SColor rightDownEdge)
{
	deferedcalls.push([clearStencilBuffer, leftUpEdge, rightUpEdge, leftDownEdge, rightDownEdge](IVideoDriver* driver) {
		driver->drawStencilShadow(clearStencilBuffer, leftUpEdge, rightUpEdge, leftDownEdge, rightDownEdge);
		});
}

void irr::video::CCommandBufferDriver::drawMeshBuffer(const scene::IMeshBuffer* mb)
{
	deferedcalls.push([mb](IVideoDriver* driver) {
		driver->drawMeshBuffer(mb);
		});
}

void irr::video::CCommandBufferDriver::drawMeshBufferNormals(const scene::IMeshBuffer* mb, f32 length, SColor color )
{
	deferedcalls.push([mb, length, color](IVideoDriver* driver) {
		driver->drawMeshBufferNormals(mb, length, color);
		});
}

void irr::video::CCommandBufferDriver::setFog(SColor color, E_FOG_TYPE fogType, f32 start, f32 end, f32 density , bool pixelFog, bool rangeFog)
{
	FogColor = color;
	FogType = fogType;
	FogStart = start;
	FogEnd = end;
	FogDensity = density;
	PixelFog = pixelFog;
	RangeFog = rangeFog;
	deferedcalls.push([color, fogType, start, end, density, pixelFog, rangeFog](IVideoDriver* driver) {
		driver->setFog(color, fogType, start, end, density, pixelFog, rangeFog);
		});
}

void irr::video::CCommandBufferDriver::getFog(SColor& color, E_FOG_TYPE& fogType, f32& start, f32& end, f32& density, bool& pixelFog, bool& rangeFog)
{
	color = FogColor;
	fogType = FogType;
	start = FogStart;
	end = FogEnd;
	density = FogDensity;
	pixelFog = PixelFog;
	rangeFog = RangeFog; 
}

irr::video::ECOLOR_FORMAT irr::video::CCommandBufferDriver::getColorFormat() const
{
	return Driver->getColorFormat();
}

const irr::core::dimension2d<irr::u32>& irr::video::CCommandBufferDriver::getScreenSize() const
{
	return Driver->getScreenSize();
}

const irr::core::dimension2d<irr::u32>& irr::video::CCommandBufferDriver::getCurrentRenderTargetSize() const
{
	if( CurrentRenderTarget )
		return CurrentRenderTarget->getSize();
	else
		return Driver->getCurrentRenderTargetSize();
}

irr::s32 irr::video::CCommandBufferDriver::getFPS() const
{
	return Driver->getFPS();
}

irr::u32 irr::video::CCommandBufferDriver::getPrimitiveCountDrawn(u32 mode) const
{
	return Driver->getPrimitiveCountDrawn(mode);
}

void irr::video::CCommandBufferDriver::deleteAllDynamicLights()
{
	deferedcalls.push([](IVideoDriver* driver) {
		driver->deleteAllDynamicLights();
		});
}

irr::s32 irr::video::CCommandBufferDriver::addDynamicLight(const SLight& light)
{
	deferedcalls.push([lightb = light](IVideoDriver* driver) {
		driver->addDynamicLight(lightb);
		});
	return Driver->getDynamicLightCount();
}

irr::u32 irr::video::CCommandBufferDriver::getMaximalDynamicLightAmount() const
{
	return Driver->getMaximalDynamicLightAmount();
}

irr::u32 irr::video::CCommandBufferDriver::getDynamicLightCount() const
{
	return Driver->getDynamicLightCount();
}

const irr::video::SLight& irr::video::CCommandBufferDriver::getDynamicLight(u32 idx) const
{
	return Driver->getDynamicLight(idx);
}

void irr::video::CCommandBufferDriver::turnLightOn(s32 lightIndex, bool turnOn)
{
	deferedcalls.push([lightIndex, turnOn](IVideoDriver* driver) {
		driver->turnLightOn(lightIndex, turnOn);
		});
}

const wchar_t* irr::video::CCommandBufferDriver::getName() const
{
	return L"Irrlicht CommandBufferDevice";
}

void irr::video::CCommandBufferDriver::addExternalImageLoader(IImageLoader* loader)
{
	loader->grab();
	deferedcalls.push([loader](IVideoDriver* driver) {
		driver->addExternalImageLoader(loader);
		loader->drop();
		});
}

void irr::video::CCommandBufferDriver::addExternalImageWriter(IImageWriter* writer)
{
	writer->grab();
	deferedcalls.push([writer](IVideoDriver* driver) {
		driver->addExternalImageWriter(writer);
		writer->drop();
		});

}

irr::u32 irr::video::CCommandBufferDriver::getMaximalPrimitiveCount() const
{
	return Driver->getMaximalPrimitiveCount();
}

void irr::video::CCommandBufferDriver::setTextureCreationFlag(E_TEXTURE_CREATION_FLAG flag, bool enabled)
{
	deferedcalls.push([flag, enabled](IVideoDriver* driver) {
		driver->setTextureCreationFlag(flag, enabled);
		});
}

bool irr::video::CCommandBufferDriver::getTextureCreationFlag(E_TEXTURE_CREATION_FLAG flag) const
{
	return Driver->getTextureCreationFlag(flag);
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImageFromFile(const io::path& filename)
{
	return Driver->createImageFromFile(filename);
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImageFromFile(io::IReadFile* file)
{
	return Driver->createImageFromFile(file);
}

bool irr::video::CCommandBufferDriver::writeImageToFile(IImage* image, const io::path& filename, u32 param)
{
	return Driver->writeImageToFile(image, filename, param);
}

bool irr::video::CCommandBufferDriver::writeImageToFile(IImage* image, io::IWriteFile* file, u32 param)
{
	return Driver->writeImageToFile(image, file, param);
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImageFromData(ECOLOR_FORMAT format, const core::dimension2d<u32>& size, void* data, bool ownForeignMemory, bool deleteMemory)
{
	return Driver->createImageFromData(format, size, data, ownForeignMemory, deleteMemory);
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImage(ECOLOR_FORMAT format, const core::dimension2d<u32>& size)
{
	return Driver->createImage(format, size);
}

_IRR_DEPRECATED_ irr::video::IImage* irr::video::CCommandBufferDriver::createImage(ECOLOR_FORMAT format, IImage* imageToCopy)
{
	return Driver->createImage(format, imageToCopy);
}

_IRR_DEPRECATED_ irr::video::IImage* irr::video::CCommandBufferDriver::createImage(IImage* imageToCopy, const core::position2d<s32>& pos, const core::dimension2d<u32>& size)
{
	return Driver->createImage(imageToCopy, pos, size);
}

irr::video::IImage* irr::video::CCommandBufferDriver::createImage(ITexture* texture, const core::position2d<s32>& pos, const core::dimension2d<u32>& size)
{
	return Driver->createImage(texture, pos, size);
}

void irr::video::CCommandBufferDriver::OnResize(const core::dimension2d<u32>& size)
{
	deferedcalls.push([sizeb = size](IVideoDriver* driver) {
		driver->OnResize(sizeb);
		});
}

irr::s32 irr::video::CCommandBufferDriver::addMaterialRenderer(IMaterialRenderer* renderer, const c8* name)
{
	return Driver->addMaterialRenderer(renderer, name);
}

irr::video::IMaterialRenderer* irr::video::CCommandBufferDriver::getMaterialRenderer(u32 idx)
{
	return Driver->getMaterialRenderer(idx);
}

irr::u32 irr::video::CCommandBufferDriver::getMaterialRendererCount() const
{
	return Driver->getMaterialRendererCount();
}

const irr::c8* irr::video::CCommandBufferDriver::getMaterialRendererName(u32 idx) const
{
	return Driver->getMaterialRendererName(idx);
}

void irr::video::CCommandBufferDriver::setMaterialRendererName(s32 idx, const c8* name)
{
	deferedcalls.push([idx, name](IVideoDriver* driver) {
		driver->setMaterialRendererName(idx, name);
		});
}

irr::io::IAttributes* irr::video::CCommandBufferDriver::createAttributesFromMaterial(const video::SMaterial& material, io::SAttributeReadWriteOptions* options)
{
	return Driver->createAttributesFromMaterial(material, options);
}

void irr::video::CCommandBufferDriver::fillMaterialStructureFromAttributes(video::SMaterial& outMaterial, io::IAttributes* attributes)
{
		Driver->fillMaterialStructureFromAttributes(outMaterial, attributes);
}

const irr::video::SExposedVideoData& irr::video::CCommandBufferDriver::getExposedVideoData()
{
	return Driver->getExposedVideoData();
}

irr::video::E_DRIVER_TYPE irr::video::CCommandBufferDriver::getDriverType() const
{
	return Driver->getDriverType();
}

irr::video::IGPUProgrammingServices* irr::video::CCommandBufferDriver::getGPUProgrammingServices()
{
	return this;
}

irr::scene::IMeshManipulator* irr::video::CCommandBufferDriver::getMeshManipulator()
{
	return Driver->getMeshManipulator();
}

void irr::video::CCommandBufferDriver::clearZBuffer()
{
	deferedcalls.push([](IVideoDriver* driver) {
		driver->clearZBuffer();
		});
}

irr::video::IImage* irr::video::CCommandBufferDriver::createScreenShot(video::ECOLOR_FORMAT format, video::E_RENDER_TARGET target)
{
	return nullptr;
}

irr::video::ITexture* irr::video::CCommandBufferDriver::findTexture(const io::path& filename)
{
	return Driver->findTexture(filename);
}

bool irr::video::CCommandBufferDriver::setClipPlane(u32 index, const core::plane3df& plane, bool enable)
{
	return Driver->setClipPlane(index, plane, enable);
}

void irr::video::CCommandBufferDriver::enableClipPlane(u32 index, bool enable)
{
	deferedcalls.push([index, enable](IVideoDriver* driver) {
		driver->enableClipPlane(index, enable);
		});
}

void irr::video::CCommandBufferDriver::setMinHardwareBufferVertexCount(u32 count)
{
	deferedcalls.push([count](IVideoDriver* driver) {
		driver->setMinHardwareBufferVertexCount(count);
		});
}

irr::video::SOverrideMaterial& irr::video::CCommandBufferDriver::getOverrideMaterial()
{
	return OverrideMaterial;
}

irr::video::SMaterial& irr::video::CCommandBufferDriver::getMaterial2D()
{
	return OverrideMaterial2D;
}

void irr::video::CCommandBufferDriver::enableMaterial2D(bool enable)
{
	deferedcalls.push([enable](IVideoDriver* driver) {
		driver->enableMaterial2D(enable);
		});
}

irr::core::stringc irr::video::CCommandBufferDriver::getVendorInfo()
{
	return Driver->getVendorInfo();
}

void irr::video::CCommandBufferDriver::setAmbientLight(const SColorf& color)
{
	deferedcalls.push([color](IVideoDriver* driver) {
		driver->setAmbientLight(color);
		});
}

void irr::video::CCommandBufferDriver::setAllowZWriteOnTransparent(bool flag)
{
	deferedcalls.push([flag](IVideoDriver* driver) {
		driver->setAllowZWriteOnTransparent(flag);
		});
}

irr::core::dimension2du irr::video::CCommandBufferDriver::getMaxTextureSize() const
{
	return Driver->getMaxTextureSize();
}

void irr::video::CCommandBufferDriver::convertColor(const void* sP, ECOLOR_FORMAT sF, s32 sN, void* dP, ECOLOR_FORMAT dF) const
{
		Driver->convertColor(sP, sF, sN, dP, dF);
}

irr::video::IVertexDescriptor* irr::video::CCommandBufferDriver::addVertexDescriptor(const core::stringc& pName)
{
	return Driver->addVertexDescriptor(pName);
}

irr::video::IVertexDescriptor* irr::video::CCommandBufferDriver::getVertexDescriptor(u32 id) const
{
	return Driver->getVertexDescriptor(id);
}

irr::video::IVertexDescriptor* irr::video::CCommandBufferDriver::getVertexDescriptor(const core::stringc& pName) const
{
	return Driver->getVertexDescriptor(pName);
}

irr::u32 irr::video::CCommandBufferDriver::getVertexDescriptorCount() const
{
	return Driver->getVertexDescriptorCount();
}

void irr::video::CCommandBufferDriver::execute(IVideoDriver* driver)
{
	//pop the queue of defered calls and execute them
	while (!deferedcalls.empty())
	{
		deferedcalls.front()(driver);
		deferedcalls.pop();
	}
}
std::shared_ptr<irr::video::IHardwareBuffer> irr::video::CCommandBufferDriver::createHardwareBuffer(scene::IComputeBuffer* computeBuffer)
{
	return Driver->createHardwareBuffer(computeBuffer);
}
void irr::video::CCommandBufferDriver::dispatchComputeShader(const core::vector3d<u32>& groupCount, scene::IComputeBuffer* Src, scene::IComputeBuffer* Dst)
{
	deferedcalls.push([groupCount, Src, Dst](IVideoDriver* driver) {
		driver->dispatchComputeShader(groupCount, Src, Dst);
		});
}