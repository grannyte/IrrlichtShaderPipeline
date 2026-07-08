#include "CCommandBufferDriver.h"

#include "IImageLoader.h"
#include "IImageWriter.h"
#include <array>


using namespace irr;
using namespace irr::video;
bool CCommandBufferDriver::beginScene(bool backBuffer, bool zBuffer, SColor color, const SExposedVideoData& videoData, core::rect<s32>* sourceRect)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([backBuffer, zBuffer, color, videoData, sourceRect](IVideoDriver* driver) {
		driver->beginScene(backBuffer, zBuffer, color, videoData, sourceRect);
		});
	return true;
}

bool CCommandBufferDriver::endScene()
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([](IVideoDriver* driver) {
		driver->endScene();
		});
	return true;
}

bool CCommandBufferDriver::queryFeature(E_VIDEO_DRIVER_FEATURE feature) const
{
	return Driver->queryFeature(feature);
}

void CCommandBufferDriver::disableFeature(E_VIDEO_DRIVER_FEATURE feature, bool flag)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([feature, flag](IVideoDriver* driver) {
		driver->disableFeature(feature, flag);
		});
}

const io::IAttributes& CCommandBufferDriver::getDriverAttributes() const
{
	return Driver->getDriverAttributes();
}

bool CCommandBufferDriver::checkDriverReset()
{
	return Driver->checkDriverReset();
}

// State cached synchronously (like the prototype already did) so a caller
// reading it back mid-frame gets the correct value even though the actual
// driver call hasn't executed yet.
void CCommandBufferDriver::setTransform(E_TRANSFORMATION_STATE state, const core::matrix4& mat)
{
	Matrices[state] = mat;
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([state, matb = mat](IVideoDriver* driver) {
		driver->setTransform(state, matb);
		});
}

const core::matrix4& CCommandBufferDriver::getTransform(E_TRANSFORMATION_STATE state) const
{
	return Matrices[state];
}

u32 CCommandBufferDriver::getImageLoaderCount() const
{
	return Driver->getImageLoaderCount();
}

IImageLoader* CCommandBufferDriver::getImageLoader(u32 n)
{
	return Driver->getImageLoader(n);
}

u32 CCommandBufferDriver::getImageWriterCount() const
{
	return Driver->getImageWriterCount();
}

IImageWriter* CCommandBufferDriver::getImageWriter(u32 n)
{
	return Driver->getImageWriter(n);
}

// FIX: SMaterial holds raw ITexture* in TextureLayer[]. grab() every
// non-null one before pushing so the material's textures can't be deleted
// out from under the recorded call, drop() them all after replay.
void CCommandBufferDriver::setMaterial(const SMaterial& material)
{
	for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
	{
		if (material.TextureLayer[i].Texture)
			material.TextureLayer[i].Texture->grab();
	}

	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([materialb = material](IVideoDriver* driver) {
		driver->setMaterial(materialb);
		for (u32 i = 0; i < MATERIAL_MAX_TEXTURES; ++i)
		{
			if (materialb.TextureLayer[i].Texture)
				materialb.TextureLayer[i].Texture->drop();
		}
		});
}

ITexture* CCommandBufferDriver::getTexture(const io::path& filename)
{
	return Driver->getTexture(filename);
}

ITexture* CCommandBufferDriver::getTexture(io::IReadFile* file)
{
	return Driver->getTexture(file);
}

ITexture* CCommandBufferDriver::getTextureByIndex(u32 index)
{
	return Driver->getTextureByIndex(index);
}

ITexture* CCommandBufferDriver::getTexture(const core::array<io::path>& files, E_TEXTURE_TYPE Type)
{
	return Driver->getTexture(files, Type);
}

u32 CCommandBufferDriver::getTextureCount() const
{
	return Driver->getTextureCount();
}

// FIX: grab/drop the texture around the deferred call.
void CCommandBufferDriver::renameTexture(ITexture* texture, const io::path& newName)
{
	if (!texture) return;
	texture->grab();
	io::path newNameb = newName;
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([texture, newNameb](IVideoDriver* driver) {
		driver->renameTexture(texture, newNameb);
		texture->drop();
		});
}

ITexture* CCommandBufferDriver::addTexture(const core::dimension2d<u32>& size, const io::path& name, ECOLOR_FORMAT format)
{
	return Driver->addTexture(size, name, format);
}

ITexture* CCommandBufferDriver::addTexture(const io::path& name, IImage* image, void* mipmapData)
{
	return Driver->addTexture(name, image, mipmapData);
}

ITexture* CCommandBufferDriver::addRenderTargetTexture(const core::dimension2d<u32>& size, const io::path& name, const ECOLOR_FORMAT format)
{
	return Driver->addRenderTargetTexture(size, name, format);
}

// FIX: grab/drop the texture around the deferred call.
void CCommandBufferDriver::removeTexture(ITexture* texture)
{
	if (!texture) return;
	texture->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([texture](IVideoDriver* driver) {
		driver->removeTexture(texture);
		texture->drop();
		});
}

void CCommandBufferDriver::removeAllTextures()
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([](IVideoDriver* driver) {
		driver->removeAllTextures();
		});
}

std::shared_ptr<IHardwareBuffer> CCommandBufferDriver::createHardwareBuffer(scene::IIndexBuffer* indexBuffer)
{
	return Driver->createHardwareBuffer(indexBuffer);
}

std::shared_ptr<IHardwareBuffer> CCommandBufferDriver::createHardwareBuffer(scene::IVertexBuffer* vertexBuffer)
{
	return Driver->createHardwareBuffer(vertexBuffer);
}

// shared_ptr<ISceneNode> needs no manual lifetime handling -- capture by
// value is enough. The raw IMesh* still does, since IMesh is still
// IReferenceCounted on this branch.
void CCommandBufferDriver::addOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, const scene::IMesh* mesh)
{
	if (mesh) mesh->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([node, mesh](IVideoDriver* driver) {
		driver->addOcclusionQuery(node, mesh);
		if (mesh) mesh->drop();
		});
}

void CCommandBufferDriver::removeOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([node](IVideoDriver* driver) {
		driver->removeOcclusionQuery(node);
		});
}

void CCommandBufferDriver::removeAllOcclusionQueries()
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([](IVideoDriver* driver) {
		driver->removeAllOcclusionQueries();
		});
}

void CCommandBufferDriver::runOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool visible)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([node, visible](IVideoDriver* driver) {
		driver->runOcclusionQuery(node, visible);
		});
}

void CCommandBufferDriver::runAllOcclusionQueries(bool visible)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	// NOTE: prototype called driver->runAllOcclusionQueries() with no args
	// here, dropping the `visible` parameter on the floor. Preserved as a
	// pass-through fix since IVideoDriver::runAllOcclusionQueries does take
	// the flag.
	deferedcalls.push([visible](IVideoDriver* driver) {
		driver->runAllOcclusionQueries(visible);
		});
}

void CCommandBufferDriver::updateOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool block)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([node, block](IVideoDriver* driver) {
		driver->updateOcclusionQuery(node, block);
		});
}

void CCommandBufferDriver::updateAllOcclusionQueries(bool block)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([block](IVideoDriver* driver) {
		driver->updateAllOcclusionQueries(block);
		});
}

u32 CCommandBufferDriver::getOcclusionQueryResult(std::shared_ptr<irr::scene::ISceneNode> node) const
{
	return Driver->getOcclusionQueryResult(node);
}

// FIX: grab/drop the texture around the deferred call.
void CCommandBufferDriver::makeColorKeyTexture(video::ITexture* texture, video::SColor color, bool zeroTexels) const
{
	if (!texture) return;
	texture->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	const_cast<CCommandBufferDriver*>(this)->deferedcalls.push([texture, color, zeroTexels](IVideoDriver* driver) {
		driver->makeColorKeyTexture(texture, color, zeroTexels);
		texture->drop();
		});
}

void CCommandBufferDriver::makeColorKeyTexture(video::ITexture* texture, core::position2d<s32> colorKeyPixelPos, bool zeroTexels) const
{
	if (!texture) return;
	texture->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	const_cast<CCommandBufferDriver*>(this)->deferedcalls.push([texture, colorKeyPixelPos, zeroTexels](IVideoDriver* driver) {
		driver->makeColorKeyTexture(texture, colorKeyPixelPos, zeroTexels);
		texture->drop();
		});
}

// FIX (two bugs): grab/drop added, AND the prototype called
// driver->makeColorKeyTexture(...) instead of makeNormalMapTexture(...)
// inside this method -- a copy/paste bug from the overload above it.
void CCommandBufferDriver::makeNormalMapTexture(video::ITexture* texture, f32 amplitude) const
{
	if (!texture) return;
	texture->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	const_cast<CCommandBufferDriver*>(this)->deferedcalls.push([texture, amplitude](IVideoDriver* driver) {
		driver->makeNormalMapTexture(texture, amplitude);
		texture->drop();
		});
}

// FIX: CurrentRenderTarget is now actually updated (was declared, never
// written, in the prototype), and texture/depthStencil are grab()'d/drop()'d.
bool CCommandBufferDriver::setRenderTarget(video::ITexture* texture, bool clearBackBuffer, bool clearZBuffer, SColor color, video::ITexture* depthStencil)
{
	CurrentRenderTarget = texture;

	if (texture) texture->grab();
	if (depthStencil) depthStencil->grab();

	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([texture, clearBackBuffer, clearZBuffer, color, depthStencil](IVideoDriver* driver) {
		driver->setRenderTarget(texture, clearBackBuffer, clearZBuffer, color, depthStencil);
		if (texture) texture->drop();
		if (depthStencil) depthStencil->drop();
		});
	return true;
}

// NOTE: IRenderTarget's internal texture ownership wasn't inspected on this
// branch -- if it wraps raw ITexture* the same way SMaterial does, apply the
// same grab()-each-entry/drop()-each-entry pattern used above before
// shipping this to production.
bool CCommandBufferDriver::setRenderTarget(const core::array<video::IRenderTarget>& texture, const core::array<bool>& clearBackBuffer, bool clearZBuffer, SColor color, video::ITexture* depthStencil)
{
	if (depthStencil) depthStencil->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([textureb = texture, clearBackBufferb = clearBackBuffer, clearZBuffer, color, depthStencil](IVideoDriver* driver) {
		driver->setRenderTarget(textureb, clearBackBufferb, clearZBuffer, color, depthStencil);
		if (depthStencil) depthStencil->drop();
		});
	return true;
}

bool CCommandBufferDriver::setRenderTarget(E_RENDER_TARGET target, bool clearTarget, bool clearZBuffer, SColor color)
{
	if (ERT_FRAME_BUFFER == target)
		return setRenderTarget(static_cast<video::ITexture*>(0), clearTarget, clearZBuffer, color, 0);
	else
		return false;
}

// FIX: no longer captures `buffer` by reference (was UB -- dangling
// reference to a stack parameter the instant this function returned).
bool CCommandBufferDriver::setStreamOutputBuffer(scene::IVertexBuffer* buffer)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([buffer](IVideoDriver* driver) {
		driver->setStreamOutputBuffer(buffer);
		});
	return true;
}

void CCommandBufferDriver::setViewPort(const core::rect<s32>& area)
{
	CNullDriverCommon::setViewPort(area);
	ViewPortCache = area;
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([areab = area](IVideoDriver* driver) {
		driver->setViewPort(areab);
		});
}

const core::rect<s32>& CCommandBufferDriver::getViewPort() const
{
	return ViewPortCache;
}

// NOTE: vertices/indexList are raw void* into caller-owned memory with no
// lifetime information available at this layer. If callers pass pointers
// into transient (e.g. stack or per-frame scratch) buffers, that memory
// must outlive execute() -- this driver has no way to enforce that. Safer
// callers should route through drawMeshBuffer (which does have lifetime
// tracking) rather than this raw path when deferring across frames.
void CCommandBufferDriver::draw2DVertexPrimitiveList(const void* vertices, u32 vertexCount, const void* indexList, u32 primCount, E_VERTEX_TYPE vType, scene::E_PRIMITIVE_TYPE pType, E_INDEX_TYPE iType)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([vertices, vertexCount, indexList, primCount, vType, pType, iType](IVideoDriver* driver) {
		driver->draw2DVertexPrimitiveList(vertices, vertexCount, indexList, primCount, vType, pType, iType);
		});
}

void CCommandBufferDriver::draw3DLine(const core::vector3df& start, const core::vector3df& end, SColor color)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([startb = start, endb = end, color](IVideoDriver* driver) {
		driver->draw3DLine(startb, endb, color);
		});
}

void CCommandBufferDriver::draw3DTriangle(const core::triangle3df& triangle, SColor color)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([triangleb = triangle, color](IVideoDriver* driver) {
		driver->draw3DTriangle(triangleb, color);
		});
}

void CCommandBufferDriver::draw3DBox(const core::aabbox3d<f32>& box, SColor color)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([boxb = box, color](IVideoDriver* driver) {
		driver->draw3DBox(boxb, color);
		});
}

// FIX: grab/drop the texture.
void CCommandBufferDriver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos)
{
	if (!texture) return;
	texture->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([texture, destPosb = destPos](IVideoDriver* driver) {
		driver->draw2DImage(texture, destPosb);
		texture->drop();
		});
}

// FIX: grab/drop the texture, in both the clipRect-null and clipRect-set paths.
void CCommandBufferDriver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos, const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect, SColor color, bool useAlphaChannelOfTexture)
{
	if (texture) texture->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	if (clipRect == nullptr)
	{
		deferedcalls.push([texture, destPosb = destPos, sourceRectb = sourceRect, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImage(texture, destPosb, sourceRectb, nullptr, color, useAlphaChannelOfTexture);
			if (texture) texture->drop();
			});
	}
	else
	{
		deferedcalls.push([texture, destPosb = destPos, sourceRectb = sourceRect, clipRectb = *clipRect, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImage(texture, destPosb, sourceRectb, &clipRectb, color, useAlphaChannelOfTexture);
			if (texture) texture->drop();
			});
	}
}

// FIX: grab/drop the texture, in both branches.
void CCommandBufferDriver::draw2DImageBatch(const video::ITexture* texture, const core::position2d<s32>& pos, const core::array<core::rect<s32>>& sourceRects, const core::array<s32>& indices, s32 kerningWidth, const core::rect<s32>* clipRect, SColor color, bool useAlphaChannelOfTexture)
{
	if (texture) texture->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	if (clipRect == nullptr)
	{
		deferedcalls.push([texture, posb = pos, sourceRectsb = sourceRects, indicesb = indices, kerningWidth, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImageBatch(texture, posb, sourceRectsb, indicesb, kerningWidth, nullptr, color, useAlphaChannelOfTexture);
			if (texture) texture->drop();
			});
	}
	else
	{
		deferedcalls.push([texture, posb = pos, sourceRectsb = sourceRects, indicesb = indices, kerningWidth, clipRectb = *clipRect, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImageBatch(texture, posb, sourceRectsb, indicesb, kerningWidth, &clipRectb, color, useAlphaChannelOfTexture);
			if (texture) texture->drop();
			});
	}
}

// FIX: grab/drop the texture, in both branches.
void CCommandBufferDriver::draw2DImageBatch(const video::ITexture* texture, const core::array<core::position2d<s32>>& positions, const core::array<core::rect<s32>>& sourceRects, const core::rect<s32>* clipRect, SColor color, bool useAlphaChannelOfTexture)
{
	if (texture) texture->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	if (clipRect == nullptr)
	{
		deferedcalls.push([texture, positionsb = positions, sourceRectsb = sourceRects, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImageBatch(texture, positionsb, sourceRectsb, nullptr, color, useAlphaChannelOfTexture);
			if (texture) texture->drop();
			});
	}
	else
	{
		deferedcalls.push([texture, positionsb = positions, sourceRectsb = sourceRects, clipRectb = *clipRect, color, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImageBatch(texture, positionsb, sourceRectsb, &clipRectb, color, useAlphaChannelOfTexture);
			if (texture) texture->drop();
			});
	}
}

// FIX (two bugs): grab/drop the texture, AND `colors` is a raw pointer to a
// (typically 4-entry) SColor array the caller may own on the stack -- the
// prototype captured that pointer raw, which dangles by execute() time
// exactly like the setStreamOutputBuffer bug. Copy the actual color values
// into the lambda instead of the pointer.
void CCommandBufferDriver::draw2DImage(const video::ITexture* texture, const core::rect<s32>& destRect, const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect, const video::SColor* const colors, bool useAlphaChannelOfTexture)
{
	if (texture) texture->grab();

	bool hasColors = (colors != nullptr);
	std::array<SColor, 4> colorsb{};
	if (hasColors)
	{
		for (int i = 0; i < 4; ++i)
			colorsb[i] = colors[i];
	}

	std::lock_guard<std::mutex> lock(QueueMutex);
	if (clipRect == nullptr)
	{
		deferedcalls.push([texture, destRectb = destRect, sourceRectb = sourceRect, hasColors, colorsb, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImage(texture, destRectb, sourceRectb, nullptr, hasColors ? colorsb.data() : nullptr, useAlphaChannelOfTexture);
			if (texture) texture->drop();
			});
	}
	else
	{
		deferedcalls.push([texture, destRectb = destRect, sourceRectb = sourceRect, clipRectb = *clipRect, hasColors, colorsb, useAlphaChannelOfTexture](IVideoDriver* driver) {
			driver->draw2DImage(texture, destRectb, sourceRectb, &clipRectb, hasColors ? colorsb.data() : nullptr, useAlphaChannelOfTexture);
			if (texture) texture->drop();
			});
	}
}

void CCommandBufferDriver::draw2DRectangle(SColor color, const core::rect<s32>& pos, const core::rect<s32>* clip)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
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

void CCommandBufferDriver::draw2DRectangle(const core::rect<s32>& pos, SColor colorLeftUp, SColor colorRightUp, SColor colorLeftDown, SColor colorRightDown, const core::rect<s32>* clip)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
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

void CCommandBufferDriver::draw2DRectangleOutline(const core::recti& pos, SColor color)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([posb = pos, color](IVideoDriver* driver) {
		driver->draw2DRectangleOutline(posb, color);
		});
}

void CCommandBufferDriver::draw2DLine(const core::position2d<s32>& start, const core::position2d<s32>& end, SColor color)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([startb = start, endb = end, color](IVideoDriver* driver) {
		driver->draw2DLine(startb, endb, color);
		});
}

void CCommandBufferDriver::drawPixel(u32 x, u32 y, const SColor& color)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([x, y, colorb = color](IVideoDriver* driver) {
		driver->drawPixel(x, y, colorb);
		});
}

void CCommandBufferDriver::draw2DPolygon(core::position2d<s32> center, f32 radius, video::SColor color, s32 vertexCount)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([centerb = center, radius, color, vertexCount](IVideoDriver* driver) {
		driver->draw2DPolygon(centerb, radius, color, vertexCount);
		});
}

void CCommandBufferDriver::drawStencilShadowVolume(const core::array<core::vector3df>& triangles, bool zfail, u32 debugDataVisible)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([trianglesb = triangles, zfail, debugDataVisible](IVideoDriver* driver) {
		driver->drawStencilShadowVolume(trianglesb, zfail, debugDataVisible);
		});
}

void CCommandBufferDriver::drawStencilShadow(bool clearStencilBuffer, video::SColor leftUpEdge, video::SColor rightUpEdge, video::SColor leftDownEdge, video::SColor rightDownEdge)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([clearStencilBuffer, leftUpEdge, rightUpEdge, leftDownEdge, rightDownEdge](IVideoDriver* driver) {
		driver->drawStencilShadow(clearStencilBuffer, leftUpEdge, rightUpEdge, leftDownEdge, rightDownEdge);
		});
}

// FIX: grab/drop the mesh buffer -- IMeshBuffer is still IReferenceCounted
// on this branch, and was previously captured with no lifetime protection.
void CCommandBufferDriver::drawMeshBuffer(const scene::IMeshBuffer* mb)
{
	if (!mb) return;
	mb->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([mb](IVideoDriver* driver) {
		driver->drawMeshBuffer(mb);
		mb->drop();
		});
}

void CCommandBufferDriver::drawMeshBufferNormals(const scene::IMeshBuffer* mb, f32 length, SColor color)
{
	if (!mb) return;
	mb->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([mb, length, color](IVideoDriver* driver) {
		driver->drawMeshBufferNormals(mb, length, color);
		mb->drop();
		});
}

void CCommandBufferDriver::setFog(SColor color, E_FOG_TYPE fogType, f32 start, f32 end, f32 density, bool pixelFog, bool rangeFog)
{
	FogColor = color;
	FogType = fogType;
	FogStart = start;
	FogEnd = end;
	FogDensity = density;
	PixelFog = pixelFog;
	RangeFog = rangeFog;
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([color, fogType, start, end, density, pixelFog, rangeFog](IVideoDriver* driver) {
		driver->setFog(color, fogType, start, end, density, pixelFog, rangeFog);
		});
}

void CCommandBufferDriver::getFog(SColor& color, E_FOG_TYPE& fogType, f32& start, f32& end, f32& density, bool& pixelFog, bool& rangeFog)
{
	color = FogColor;
	fogType = FogType;
	start = FogStart;
	end = FogEnd;
	density = FogDensity;
	pixelFog = PixelFog;
	rangeFog = RangeFog;
}

ECOLOR_FORMAT CCommandBufferDriver::getColorFormat() const
{
	return Driver->getColorFormat();
}

const core::dimension2d<u32>& CCommandBufferDriver::getScreenSize() const
{
	return Driver->getScreenSize();
}

const core::dimension2d<u32>& CCommandBufferDriver::getCurrentRenderTargetSize() const
{
	if (CurrentRenderTarget)
		return CurrentRenderTarget->getSize();
	else
		return Driver->getCurrentRenderTargetSize();
}

s32 CCommandBufferDriver::getFPS() const
{
	return Driver->getFPS();
}

u32 CCommandBufferDriver::getPrimitiveCountDrawn(u32 mode) const
{
	return Driver->getPrimitiveCountDrawn(mode);
}

void CCommandBufferDriver::deleteAllDynamicLights()
{
	CachedDynamicLightCount = 0;
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([](IVideoDriver* driver) {
		driver->deleteAllDynamicLights();
		});
}

// FIX: no longer touches (possibly stale) Driver state synchronously; the
// returned index now comes from a locally tracked count that increments
// exactly once per call, matching how the deferred add will eventually play
// out, same pattern as Matrices/Fog* above.
s32 CCommandBufferDriver::addDynamicLight(const SLight& light)
{
	s32 index = static_cast<s32>(CachedDynamicLightCount++);
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([lightb = light](IVideoDriver* driver) {
		driver->addDynamicLight(lightb);
		});
	return index;
}

u32 CCommandBufferDriver::getMaximalDynamicLightAmount() const
{
	return Driver->getMaximalDynamicLightAmount();
}

// FIX: returns the locally tracked count rather than depending on Driver
// having already processed a deferred add.
u32 CCommandBufferDriver::getDynamicLightCount() const
{
	return CachedDynamicLightCount;
}

const SLight& CCommandBufferDriver::getDynamicLight(u32 idx) const
{
	return Driver->getDynamicLight(idx);
}

void CCommandBufferDriver::turnLightOn(s32 lightIndex, bool turnOn)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([lightIndex, turnOn](IVideoDriver* driver) {
		driver->turnLightOn(lightIndex, turnOn);
		});
}

const wchar_t* CCommandBufferDriver::getName() const
{
	return L"Irrlicht CommandBufferDriver";
}

void CCommandBufferDriver::addExternalImageLoader(IImageLoader* loader)
{
	if (!loader) return;
	loader->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([loader](IVideoDriver* driver) {
		driver->addExternalImageLoader(loader);
		loader->drop();
		});
}

void CCommandBufferDriver::addExternalImageWriter(IImageWriter* writer)
{
	if (!writer) return;
	writer->grab();
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([writer](IVideoDriver* driver) {
		driver->addExternalImageWriter(writer);
		writer->drop();
		});
}

u32 CCommandBufferDriver::getMaximalPrimitiveCount() const
{
	return Driver->getMaximalPrimitiveCount();
}

void CCommandBufferDriver::setTextureCreationFlag(E_TEXTURE_CREATION_FLAG flag, bool enabled)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([flag, enabled](IVideoDriver* driver) {
		driver->setTextureCreationFlag(flag, enabled);
		});
}

bool CCommandBufferDriver::getTextureCreationFlag(E_TEXTURE_CREATION_FLAG flag) const
{
	return Driver->getTextureCreationFlag(flag);
}

IImage* CCommandBufferDriver::createImageFromFile(const io::path& filename)
{
	return Driver->createImageFromFile(filename);
}

IImage* CCommandBufferDriver::createImageFromFile(io::IReadFile* file)
{
	return Driver->createImageFromFile(file);
}

bool CCommandBufferDriver::writeImageToFile(IImage* image, const io::path& filename, u32 param)
{
	return Driver->writeImageToFile(image, filename, param);
}

bool CCommandBufferDriver::writeImageToFile(IImage* image, io::IWriteFile* file, u32 param)
{
	return Driver->writeImageToFile(image, file, param);
}

IImage* CCommandBufferDriver::createImageFromData(ECOLOR_FORMAT format, const core::dimension2d<u32>& size, void* data, bool ownForeignMemory, bool deleteMemory)
{
	return Driver->createImageFromData(format, size, data, ownForeignMemory, deleteMemory);
}

IImage* CCommandBufferDriver::createImage(ECOLOR_FORMAT format, const core::dimension2d<u32>& size)
{
	return Driver->createImage(format, size);
}

IImage* CCommandBufferDriver::createImage(ECOLOR_FORMAT format, IImage* imageToCopy)
{
	return Driver->createImage(format, imageToCopy);
}

IImage* CCommandBufferDriver::createImage(IImage* imageToCopy, const core::position2d<s32>& pos, const core::dimension2d<u32>& size)
{
	return Driver->createImage(imageToCopy, pos, size);
}

IImage* CCommandBufferDriver::createImage(ITexture* texture, const core::position2d<s32>& pos, const core::dimension2d<u32>& size)
{
	return Driver->createImage(texture, pos, size);
}

void CCommandBufferDriver::OnResize(const core::dimension2d<u32>& size)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([sizeb = size](IVideoDriver* driver) {
		driver->OnResize(sizeb);
		});
}

s32 CCommandBufferDriver::addMaterialRenderer(IMaterialRenderer* renderer, const c8* name)
{
	return Driver->addMaterialRenderer(renderer, name);
}

IMaterialRenderer* CCommandBufferDriver::getMaterialRenderer(u32 idx)
{
	return Driver->getMaterialRenderer(idx);
}

u32 CCommandBufferDriver::getMaterialRendererCount() const
{
	return Driver->getMaterialRendererCount();
}

const c8* CCommandBufferDriver::getMaterialRendererName(u32 idx) const
{
	return Driver->getMaterialRendererName(idx);
}

void CCommandBufferDriver::setMaterialRendererName(s32 idx, const c8* name)
{
	// NOTE: `name` is a raw c-string. If the caller passes a transient
	// buffer, capturing the pointer raw (as the prototype did) is unsafe for
	// the same reason the `colors` and `buffer` bugs above were. Copying
	// into a core::stringc makes the deferred call self-contained.
	core::stringc nameb(name);
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([idx, nameb](IVideoDriver* driver) {
		driver->setMaterialRendererName(idx, nameb.c_str());
		});
}

io::IAttributes* CCommandBufferDriver::createAttributesFromMaterial(const video::SMaterial& material, io::SAttributeReadWriteOptions* options)
{
	return Driver->createAttributesFromMaterial(material, options);
}

void CCommandBufferDriver::fillMaterialStructureFromAttributes(video::SMaterial& outMaterial, io::IAttributes* attributes)
{
	Driver->fillMaterialStructureFromAttributes(outMaterial, attributes);
}

const SExposedVideoData& CCommandBufferDriver::getExposedVideoData()
{
	return Driver->getExposedVideoData();
}

E_DRIVER_TYPE CCommandBufferDriver::getDriverType() const
{
	return Driver->getDriverType();
}

// FIX: delegate to the real driver instead of returning `this`.
// OnSetConstants is invoked by the driver processing setMaterial/
// drawMeshBuffer internally -- it's never invoked through this object at
// all, since setMaterial/drawMeshBuffer here just queue a lambda that calls
// Driver->setMaterial(...)/Driver->drawMeshBuffer(...) later. That callback
// chain only ever runs inside that lambda, against Driver, at execute()
// time -- so this needs to hand back Driver's services, not its own.
IGPUProgrammingServices* CCommandBufferDriver::getGPUProgrammingServices()
{
	return Driver->getGPUProgrammingServices();
}

scene::IMeshManipulator* CCommandBufferDriver::getMeshManipulator()
{
	return Driver->getMeshManipulator();
}

void CCommandBufferDriver::clearZBuffer()
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([](IVideoDriver* driver) {
		driver->clearZBuffer();
		});
}

IImage* CCommandBufferDriver::createScreenShot(video::ECOLOR_FORMAT format, video::E_RENDER_TARGET target)
{
	// A screenshot of a frame that hasn't executed yet is meaningless -- the
	// prototype returned nullptr here too. If you need this, it has to be a
	// synchronous call made *after* execute(), directly on Driver, not
	// through this class.
	return nullptr;
}

ITexture* CCommandBufferDriver::findTexture(const io::path& filename)
{
	return Driver->findTexture(filename);
}

bool CCommandBufferDriver::setClipPlane(u32 index, const core::plane3df& plane, bool enable)
{
	return Driver->setClipPlane(index, plane, enable);
}

void CCommandBufferDriver::enableClipPlane(u32 index, bool enable)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([index, enable](IVideoDriver* driver) {
		driver->enableClipPlane(index, enable);
		});
}

void CCommandBufferDriver::setMinHardwareBufferVertexCount(u32 count)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([count](IVideoDriver* driver) {
		driver->setMinHardwareBufferVertexCount(count);
		});
}

SOverrideMaterial& CCommandBufferDriver::getOverrideMaterial()
{
	return OverrideMaterial;
}

SMaterial& CCommandBufferDriver::getMaterial2D()
{
	return OverrideMaterial2D;
}

void CCommandBufferDriver::enableMaterial2D(bool enable)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([enable](IVideoDriver* driver) {
		driver->enableMaterial2D(enable);
		});
}

core::stringc CCommandBufferDriver::getVendorInfo()
{
	return Driver->getVendorInfo();
}

void CCommandBufferDriver::setAmbientLight(const SColorf& color)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([color](IVideoDriver* driver) {
		driver->setAmbientLight(color);
		});
}

void CCommandBufferDriver::setAllowZWriteOnTransparent(bool flag)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([flag](IVideoDriver* driver) {
		driver->setAllowZWriteOnTransparent(flag);
		});
}

core::dimension2du CCommandBufferDriver::getMaxTextureSize() const
{
	return Driver->getMaxTextureSize();
}

void CCommandBufferDriver::convertColor(const void* sP, ECOLOR_FORMAT sF, s32 sN, void* dP, ECOLOR_FORMAT dF) const
{
	Driver->convertColor(sP, sF, sN, dP, dF);
}

IVertexDescriptor* CCommandBufferDriver::addVertexDescriptor(const core::stringc& pName)
{
	return Driver->addVertexDescriptor(pName);
}

IVertexDescriptor* CCommandBufferDriver::getVertexDescriptor(u32 id) const
{
	return Driver->getVertexDescriptor(id);
}

IVertexDescriptor* CCommandBufferDriver::getVertexDescriptor(const core::stringc& pName) const
{
	return Driver->getVertexDescriptor(pName);
}

u32 CCommandBufferDriver::getVertexDescriptorCount() const
{
	return Driver->getVertexDescriptorCount();
}

// FIX: defaults to the wired-up Driver instead of an unconditionally-null
// member; swaps the queue out under lock so recording can continue on
// another thread while replay runs unlocked.
void CCommandBufferDriver::execute(IVideoDriver* driver)
{
	IVideoDriver* target = driver ? driver : Driver;
	// In a debug build you may want:
	// IRR_ASSERT(target != nullptr && "CCommandBufferDriver::execute: no driver supplied at construction or call site");

	std::queue<std::function<void(IVideoDriver*)>> local;
	{
		std::lock_guard<std::mutex> lock(QueueMutex);
		std::swap(local, deferedcalls);
	}
	while (!local.empty())
	{
		local.front()(target);
		local.pop();
	}
}

void CCommandBufferDriver::beginRecording()
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	std::queue<std::function<void(IVideoDriver*)>> empty;
	std::swap(deferedcalls, empty);
}

size_t CCommandBufferDriver::pendingCommandCount() const
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	return deferedcalls.size();
}

std::shared_ptr<IHardwareBuffer> CCommandBufferDriver::createHardwareBuffer(scene::IComputeBuffer* computeBuffer)
{
	return Driver->createHardwareBuffer(computeBuffer);
}

void CCommandBufferDriver::dispatchComputeShader(const core::vector3d<u32>& groupCount, scene::IComputeBuffer* Src, scene::IComputeBuffer* Dst)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	deferedcalls.push([groupCount, Src, Dst](IVideoDriver* driver) {
		driver->dispatchComputeShader(groupCount, Src, Dst);
		});
}

// --- Newly added: methods only defaulted in CNullDriver, not CNullDriverCommon ---

void CCommandBufferDriver::batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
	const irr::core::array<SColor>& color,
	const irr::core::array<core::rect<s32>>* clip)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	if (clip == nullptr)
	{
		deferedcalls.push([posb = pos, colorb = color](IVideoDriver* driver) {
			driver->batchDraw2DRectangles(posb, colorb, nullptr);
			});
	}
	else
	{
		deferedcalls.push([posb = pos, colorb = color, clipb = *clip](IVideoDriver* driver) {
			driver->batchDraw2DRectangles(posb, colorb, &clipb);
			});
	}
}

// NOTE: the corner-color parameters are non-const references on this
// overload (matches IVideoDriver.h exactly, unusual as that is), so the
// lambda captures copies and must be `mutable` to pass them back out as
// non-const lvalue refs at replay time.
void CCommandBufferDriver::batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
	irr::core::array<SColor>& colorLeftUp, irr::core::array<SColor>& colorRightUp,
	irr::core::array<SColor>& colorLeftDown, irr::core::array<SColor>& colorRightDown,
	const irr::core::array<core::rect<s32>>* clip)
{
	std::lock_guard<std::mutex> lock(QueueMutex);
	if (clip == nullptr)
	{
		deferedcalls.push([posb = pos, colorLeftUpb = colorLeftUp, colorRightUpb = colorRightUp,
			colorLeftDownb = colorLeftDown, colorRightDownb = colorRightDown]
			(IVideoDriver* driver) mutable {
				driver->batchDraw2DRectangles(posb, colorLeftUpb, colorRightUpb, colorLeftDownb, colorRightDownb, nullptr);
			});
	}
	else
	{
		deferedcalls.push([posb = pos, colorLeftUpb = colorLeftUp, colorRightUpb = colorRightUp,
			colorLeftDownb = colorLeftDown, colorRightDownb = colorRightDown, clipb = *clip]
			(IVideoDriver* driver) mutable {
				driver->batchDraw2DRectangles(posb, colorLeftUpb, colorRightUpb, colorLeftDownb, colorRightDownb, &clipb);
			});
	}
}

// --- IGPUProgrammingServices: immediate passthrough, same reasoning as
// addMaterialRenderer/addTexture -- these hand back a synchronous ID the
// caller needs right away, and they register into Driver's own material
// renderer table, so they must run against Driver directly rather than
// being deferred or (worse) registered into some bookkeeping local to this
// object that would drift out of sync with Driver's real state. ---

s32 CCommandBufferDriver::addHighLevelShaderMaterial(
	const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
	const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
	const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
	scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut,
	IShaderConstantSetCallBack* callback,
	E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut, s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
{
	return Driver->getGPUProgrammingServices()->addHighLevelShaderMaterial(
		vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
		pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
		geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
		inType, outType, verticesOut, callback, baseMaterial, vertexTypeOut, userData, shadingLang);
}

s32 CCommandBufferDriver::addHighLevelShaderMaterial(
	const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
	const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
	const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
	const c8* hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
	const c8* domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
	scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut,
	IShaderConstantSetCallBack* callback,
	E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut, s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
{
	return Driver->getGPUProgrammingServices()->addHighLevelShaderMaterial(
		vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
		pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
		geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
		hullShaderProgram, hullShaderEntryPointName, hsCompileTarget,
		domainShaderProgram, domainShaderEntryPointName, dsCompileTarget,
		inType, outType, verticesOut, callback, baseMaterial, vertexTypeOut, userData, shadingLang);
}

s32 CCommandBufferDriver::addHighLevelShaderMaterialFromFiles(
	const io::path& vertexShaderProgramFileName, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
	const io::path& pixelShaderProgramFileName, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
	const io::path& geometryShaderProgramFileName, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
	scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut,
	IShaderConstantSetCallBack* callback,
	E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut, s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
{
	return Driver->getGPUProgrammingServices()->addHighLevelShaderMaterialFromFiles(
		vertexShaderProgramFileName, vertexShaderEntryPointName, vsCompileTarget,
		pixelShaderProgramFileName, pixelShaderEntryPointName, psCompileTarget,
		geometryShaderProgramFileName, geometryShaderEntryPointName, gsCompileTarget,
		inType, outType, verticesOut, callback, baseMaterial, vertexTypeOut, userData, shadingLang);
}

s32 CCommandBufferDriver::addHighLevelShaderMaterialFromFiles(
	const io::path& vertexShaderProgramFile, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
	const io::path& pixelShaderProgramFile, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
	const io::path& geometryShaderProgramFileName, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
	const io::path& hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
	const io::path& domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
	scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut,
	IShaderConstantSetCallBack* callback,
	E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut, s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
{
	return Driver->getGPUProgrammingServices()->addHighLevelShaderMaterialFromFiles(
		vertexShaderProgramFile, vertexShaderEntryPointName, vsCompileTarget,
		pixelShaderProgramFile, pixelShaderEntryPointName, psCompileTarget,
		geometryShaderProgramFileName, geometryShaderEntryPointName, gsCompileTarget,
		hullShaderProgram, hullShaderEntryPointName, hsCompileTarget,
		domainShaderProgram, domainShaderEntryPointName, dsCompileTarget,
		inType, outType, verticesOut, callback, baseMaterial, vertexTypeOut, userData, shadingLang);
}

s32 CCommandBufferDriver::addHighLevelShaderMaterialFromFiles(
	io::IReadFile* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
	io::IReadFile* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
	io::IReadFile* geometryShaderProgram, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
	scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut,
	IShaderConstantSetCallBack* callback,
	E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut, s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
{
	return Driver->getGPUProgrammingServices()->addHighLevelShaderMaterialFromFiles(
		vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
		pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
		geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
		inType, outType, verticesOut, callback, baseMaterial, vertexTypeOut, userData, shadingLang);
}

s32 CCommandBufferDriver::addHighLevelShaderMaterialFromFiles(
	io::IReadFile* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
	io::IReadFile* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
	io::IReadFile* geometryShaderProgram, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
	io::IReadFile* hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
	io::IReadFile* domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
	scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut,
	IShaderConstantSetCallBack* callback,
	E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut, s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
{
	return Driver->getGPUProgrammingServices()->addHighLevelShaderMaterialFromFiles(
		vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
		pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
		geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
		hullShaderProgram, hullShaderEntryPointName, hsCompileTarget,
		domainShaderProgram, domainShaderEntryPointName, dsCompileTarget,
		inType, outType, verticesOut, callback, baseMaterial, vertexTypeOut, userData, shadingLang);
}

s32 CCommandBufferDriver::addShaderMaterial(const c8* vertexShaderProgram, const c8* pixelShaderProgram,
	IShaderConstantSetCallBack* callback, E_MATERIAL_TYPE baseMaterial, s32 userData)
{
	return Driver->getGPUProgrammingServices()->addShaderMaterial(
		vertexShaderProgram, pixelShaderProgram, callback, baseMaterial, userData);
}

s32 CCommandBufferDriver::addShaderMaterialFromFiles(io::IReadFile* vertexShaderProgram, io::IReadFile* pixelShaderProgram,
	IShaderConstantSetCallBack* callback, E_MATERIAL_TYPE baseMaterial, s32 userData)
{
	return Driver->getGPUProgrammingServices()->addShaderMaterialFromFiles(
		vertexShaderProgram, pixelShaderProgram, callback, baseMaterial, userData);
}

s32 CCommandBufferDriver::addShaderMaterialFromFiles(const io::path& vertexShaderProgramFileName, const io::path& pixelShaderProgramFileName,
	IShaderConstantSetCallBack* callback, E_MATERIAL_TYPE baseMaterial, s32 userData)
{
	return Driver->getGPUProgrammingServices()->addShaderMaterialFromFiles(
		vertexShaderProgramFileName, pixelShaderProgramFileName, callback, baseMaterial, userData);
}

s32 CCommandBufferDriver::addComputeShader(const c8* computeShaderProgram, const c8* computeShaderEntryPointName,
	E_COMPUTE_SHADER_TYPE csCompileTarget, IShaderConstantSetCallBack* callback, s32 userData)
{
	return Driver->getGPUProgrammingServices()->addComputeShader(
		computeShaderProgram, computeShaderEntryPointName, csCompileTarget, callback, userData);
}

s32 CCommandBufferDriver::addComputeShaderFromFile(const io::path& computeShaderProgramFileName, const c8* computeShaderEntryPointName,
	E_COMPUTE_SHADER_TYPE csCompileTarget, IShaderConstantSetCallBack* callback, s32 userData)
{
	return Driver->getGPUProgrammingServices()->addComputeShaderFromFile(
		computeShaderProgramFileName, computeShaderEntryPointName, csCompileTarget, callback, userData);
}