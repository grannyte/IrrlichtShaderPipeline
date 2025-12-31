// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CSceneNodeAnimatorCameraFPS.h"
#include "IVideoDriver.h"
#include "ISceneManager.h"
#include "Keycodes.h"
#include "ICursorControl.h"
#include "ICameraSceneNode.h"
#include "ISceneNodeAnimatorCollisionResponse.h"
#include "os.h"

namespace irr
{
namespace scene
{

//! constructor
CSceneNodeAnimatorCameraFPS::CSceneNodeAnimatorCameraFPS(gui::ICursorControl* cursorControl,
		f32 rotateSpeed, f32 moveSpeed, f32 jumpSpeed,
		SKeyMap* keyMapArray, u32 keyMapSize, bool noVerticalMovement, bool invertY)
: CursorControl(cursorControl), MaxVerticalAngle(88.0f),
	MoveSpeed(moveSpeed), RotateSpeed(rotateSpeed), JumpSpeed(jumpSpeed),
	MouseYDirection(invertY ? -1.0f : 1.0f),
	LastAnimationTime(0), firstUpdate(true), firstInput(true), NoVerticalMovement(noVerticalMovement)
{
	#ifdef _DEBUG
	setDebugName("CCameraSceneNodeAnimatorFPS");
	#endif

	if (CursorControl)
		CursorControl->grab();

	allKeysUp();

	// create key map
	if (!keyMapArray || !keyMapSize)
	{
		// create default key map
		KeyMap.push_back(SKeyMap(EKA_MOVE_FORWARD, irr::KEY_UP));
		KeyMap.push_back(SKeyMap(EKA_MOVE_BACKWARD, irr::KEY_DOWN));
		KeyMap.push_back(SKeyMap(EKA_STRAFE_LEFT, irr::KEY_LEFT));
		KeyMap.push_back(SKeyMap(EKA_STRAFE_RIGHT, irr::KEY_RIGHT));
		KeyMap.push_back(SKeyMap(EKA_JUMP_UP, irr::KEY_KEY_J));
	}
	else
	{
		// create custom key map
		setKeyMap(keyMapArray, keyMapSize);
	}
}


//! destructor
CSceneNodeAnimatorCameraFPS::~CSceneNodeAnimatorCameraFPS()
{
	if (CursorControl)
		CursorControl->drop();
}


//! It is possible to send mouse and key events to the camera. Most cameras
//! may ignore this input, but camera scene nodes which are created for
//! example with scene::ISceneManager::addMayaCameraSceneNode or
//! scene::ISceneManager::addFPSCameraSceneNode, may want to get this input
//! for changing their position, look at target or whatever.
bool CSceneNodeAnimatorCameraFPS::OnEvent(const SEvent& evt)
{
	switch(evt.EventType)
	{
	case EET_KEY_INPUT_EVENT:
		for (u32 i=0; i<KeyMap.size(); ++i)
		{
			if (KeyMap[i].KeyCode == evt.KeyInput.Key)
			{
				CursorKeys[KeyMap[i].Action] = evt.KeyInput.PressedDown;
				return true;
			}
		}
		break;

	case EET_MOUSE_INPUT_EVENT:
		if (evt.MouseInput.Event == EMIE_MOUSE_MOVED)
		{
			CursorPos = CursorControl->getRelativePosition();
			return true;
		}
		break;

	default:
		break;
	}

	return false;
}

#pragma float_control( precise,on , push )


void irr::scene::CSceneNodeAnimatorCameraFPS::animateNode(ISceneNode* node,
    u32 timeMs)
{
    if (!node || node->getType() != ESNT_CAMERA)
        return;

    auto camera = static_cast<ICameraSceneNode*>(node);

    if (firstUpdate)
    {
        // Initialize yaw/pitch from current orientation
        core::vector3df dir = camera->getTarget() - camera->getPosition();
        dir.normalize();
        auto ha = dir.getHorizontalAngle();
        YawAngle = ha.Y;
        PitchAngle = ha.X;

        // Center cursor
        if (CursorControl)
        {
            CursorControl->setPosition(0.5f, 0.5f);
            CenterCursor = CursorControl->getRelativePosition();
        }

        LastAnimationTime = timeMs;
        firstUpdate = false;
    }

    // Only animate if this camera is active & receiving input
    if (!camera->isInputReceiverEnabled() ||
        (camera->getSceneManager() &&
            camera->getSceneManager()->getActiveCamera().get() != camera))
    {
        firstInput = true;
        return;
    }

    if (firstInput)
    {
        allKeysUp();
        firstInput = false;
    }

    // Delta time
    f32 dt = (f32)(timeMs - LastAnimationTime);
    LastAnimationTime = timeMs;

    //
    // --- MOUSE LOOK (inverted X/Y) ---
    //
    if (CursorControl)
    {
        core::vector2df cur = CursorControl->getRelativePosition();
        core::vector2df delta = cur - CenterCursor;

        if (delta.X != 0.f || delta.Y != 0.f)
        {
            // arbitrary up
            core::vector3df up = camera->getUpVector();
            up.normalize();

            // Store the tentative new angles
            f32 newYaw = YawAngle + delta.X * RotateSpeed;
            f32 newPitch = PitchAngle - delta.Y * RotateSpeed * MouseYDirection;

            // Test if the new orientation would cause gimbal lock or flip
            core::quaternion qYaw, qPitch;
            qYaw.fromAngleAxis(core::DEGTORAD * newYaw, up);
            core::vector3df fwd = qYaw * core::vector3df(0, 0, 1);
            core::vector3df right = fwd.crossProduct(up).normalize();
            qPitch.fromAngleAxis(core::DEGTORAD * newPitch, right);
            core::vector3df testForward = qPitch * fwd;
            testForward.normalize();

            // Check angle between forward and up vector
            // Prevent looking too close to straight up or down (leave a safety margin)
            f32 dotProduct = testForward.dotProduct(up);
            f32 angle = core::RADTODEG * acos(core::clamp(dotProduct, -1.0f, 1.0f));

            // Allow looking in any direction as long as we're not within 5 degrees of straight up/down
            const f32 safetyMargin = 5.0f;
            if (angle > safetyMargin && angle < (180.0f - safetyMargin))
            {
                YawAngle = newYaw;
                PitchAngle = newPitch;
            }
            // If the new angle would be too extreme, only apply the yaw (horizontal rotation)
            else
            {
                YawAngle = newYaw;
                // Keep pitch at the limit by recalculating it
                // This allows smooth rotation along the horizon even when at pitch limits
            }

            // recenter cursor
            CursorControl->setPosition(0.5f, 0.5f);
            CenterCursor = CursorControl->getRelativePosition();
        }
    }

    //
    // --- REBUILD FORWARD FROM YAW/PITCH ---
    //
    core::vector3df up = camera->getUpVector();
    up.normalize();
    core::quaternion qYaw, qPitch;

    qYaw.fromAngleAxis(core::DEGTORAD * YawAngle, up);

    // base forward = (0,0,1) in local
    core::vector3df fwd = qYaw * core::vector3df(0, 0, 1);
    core::vector3df right = fwd.crossProduct(up).normalize();

    qPitch.fromAngleAxis(core::DEGTORAD * PitchAngle, right);
    core::vector3df finalForward = qPitch * fwd;

    //
    // --- MOVEMENT (strafe signs inverted) ---
    //
    core::vector3df pos = camera->getPosition();
    core::vector3df movF = finalForward;
    core::vector3df movR = movF.crossProduct(up).normalize();

    if (NoVerticalMovement)
    {
        movF -= up * movF.dotProduct(up);
        movF.normalize();
        movR -= up * movR.dotProduct(up);
        movR.normalize();
    }

    if (CursorKeys[EKA_MOVE_FORWARD])
        pos += movF * dt * MoveSpeed;
    if (CursorKeys[EKA_MOVE_BACKWARD])
        pos -= movF * dt * MoveSpeed;

    // now inverted: pressing LEFT adds +R, pressing RIGHT subtracts R
    if (CursorKeys[EKA_STRAFE_LEFT])
        pos += movR * dt * MoveSpeed;
    if (CursorKeys[EKA_STRAFE_RIGHT])
        pos -= movR * dt * MoveSpeed;

    if (CursorKeys[EKA_JUMP_UP])
    {
        for (auto* anim : camera->getAnimators())
            if (anim->getType() == ESNAT_COLLISION_RESPONSE)
            {
                auto* cr = static_cast<ISceneNodeAnimatorCollisionResponse*>(anim);
                if (!cr->isFalling())
                    cr->jump(JumpSpeed);
            }
    }

    // Write position and target
    camera->setPosition(pos);
    camera->setPosition(pos);
    irr::core::vector3df displacementCorrection(0);
    if (camera->getParent() == camera->getSceneManager()->getRootSceneNode())
        displacementCorrection = pos;
    camera->setTarget(displacementCorrection + finalForward.normalize() * std::max(pos.getLength(), 1.0f));
}

#pragma float_control( pop )


void CSceneNodeAnimatorCameraFPS::allKeysUp()
{
	for (u32 i=0; i<EKA_COUNT; ++i)
		CursorKeys[i] = false;
}


//! Sets the rotation speed
void CSceneNodeAnimatorCameraFPS::setRotateSpeed(f32 speed)
{
	RotateSpeed = speed;
}


//! Sets the movement speed
void CSceneNodeAnimatorCameraFPS::setMoveSpeed(f32 speed)
{
	MoveSpeed = speed;
}


//! Gets the rotation speed
f32 CSceneNodeAnimatorCameraFPS::getRotateSpeed() const
{
	return RotateSpeed;
}


// Gets the movement speed
f32 CSceneNodeAnimatorCameraFPS::getMoveSpeed() const
{
	return MoveSpeed;
}


//! Sets the keyboard mapping for this animator
void CSceneNodeAnimatorCameraFPS::setKeyMap(SKeyMap *map, u32 count)
{
	// clear the keymap
	KeyMap.clear();

	// add actions
	for (u32 i=0; i<count; ++i)
	{
		KeyMap.push_back(map[i]);
	}
}

void CSceneNodeAnimatorCameraFPS::setKeyMap(const core::array<SKeyMap>& keymap)
{
	KeyMap=keymap;
}

const core::array<SKeyMap>& CSceneNodeAnimatorCameraFPS::getKeyMap() const
{
	return KeyMap;
}


//! Sets whether vertical movement should be allowed.
void CSceneNodeAnimatorCameraFPS::setVerticalMovement(bool allow)
{
	NoVerticalMovement = !allow;
}


//! Sets whether the Y axis of the mouse should be inverted.
void CSceneNodeAnimatorCameraFPS::setInvertMouse(bool invert)
{
	if (invert)
		MouseYDirection = -1.0f;
	else
		MouseYDirection = 1.0f;
}


ISceneNodeAnimator* CSceneNodeAnimatorCameraFPS::createClone(std::shared_ptr<ISceneNode> node, std::shared_ptr<ISceneManager> newManager)
{
	CSceneNodeAnimatorCameraFPS * newAnimator =
		new CSceneNodeAnimatorCameraFPS(CursorControl,	RotateSpeed, MoveSpeed, JumpSpeed,
											0, 0, NoVerticalMovement);
	newAnimator->cloneMembers(this);
	newAnimator->setKeyMap(KeyMap);
	return newAnimator;
}


} // namespace scene
} // namespace irr

