// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#ifndef __C_EMPTY_SCENE_NODE_H_INCLUDED__
#define __C_EMPTY_SCENE_NODE_H_INCLUDED__

#include "ISceneNode.h"

namespace irr
{
namespace scene
{

	class CEmptySceneNode : public ISceneNode
	{
	public:

		//! constructor
		CEmptySceneNode(std::shared_ptr<ISceneManager> mgr, irr::s32 id);

		//! returns the axis aligned bounding box of this node
		const irr::core::aabbox3d<f32>& getBoundingBox() const _IRR_OVERRIDE_;

		//! This method is called just before the rendering process of the whole scene.
		void OnRegisterSceneNode() _IRR_OVERRIDE_;

		//! does nothing.
		void render() _IRR_OVERRIDE_;

		//! Returns type of the scene node
		ESCENE_NODE_TYPE getType() const _IRR_OVERRIDE_ { return ESNT_EMPTY; }

		//! Creates a clone of this scene node and its children.
		std::shared_ptr<ISceneNode> clone(std::shared_ptr<ISceneNode> newParent=0, std::shared_ptr<ISceneManager> newManager=0) _IRR_OVERRIDE_;

	private:

		core::aabbox3d<f32> Box;
	};

} // end namespace scene
} // end namespace irr

#endif

