// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CEmptySceneNode.h"
#include "ISceneManager.h"
#include "IVideoDriver.h"

namespace irr
{
namespace scene
{

//! constructor
CEmptySceneNode::CEmptySceneNode(std::shared_ptr<ISceneManager> mgr, irr::s32 id)
: ISceneNode( mgr, id)
{
	#ifdef _DEBUG
	setDebugName("CEmptySceneNode");
	#endif

	setAutomaticCulling(scene::EAC_OFF);
}


//! pre render event
void CEmptySceneNode::OnRegisterSceneNode()
{
	if (IsVisible)
		SceneManager.lock()->registerNodeForRendering(std::dynamic_pointer_cast<ISceneNode>(shared_from_this()));

	ISceneNode::OnRegisterSceneNode();
}


//! render
void CEmptySceneNode::render()
{
	Box.reset(0, 0, 0);
	Box.addInternalPoint(RelativeScale);
	Box.addInternalPoint(-RelativeScale);


		if (DebugDataVisible & scene::EDS_BBOX)
		{
			video::SMaterial m;
			m.Lighting = false;
			auto driver = SceneManager.lock()->getVideoDriver();
			driver->setMaterial(m);
			driver->setTransform(video::ETS_WORLD, AbsoluteTransformation);
			driver->draw3DBox(Box, video::SColor(255, 255, 255, 255));
		}
	


	// do nothing
}


//! returns the axis aligned bounding box of this node
const irr::core::aabbox3d<f32>& CEmptySceneNode::getBoundingBox() const
{
	return Box;
}


//! Creates a clone of this scene node and its children.
std::shared_ptr<ISceneNode> CEmptySceneNode::clone(std::shared_ptr<ISceneNode> newParent,
                                                   std::shared_ptr<ISceneManager> newManager)
{
	if (!newParent)
		newParent = Parent.lock();
	if (!newManager)
		newManager = SceneManager.lock();

	auto  nb = std::make_shared< CEmptySceneNode>(
		newManager, ID);
	newParent->addChild(nb);
	nb->cloneMembers(std::dynamic_pointer_cast<ISceneNode>(shared_from_this()), newManager);
	nb->Box = Box;
	return nb;
}


} // end namespace scene
} // end namespace irr
