#pragma once

// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h
//
#ifndef __I_D3D11_MATERIAL_RENDERER_SERVICES_H_INCLUDED__
#define __I_D3D11_MATERIAL_RENDERER_SERVICES_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "IMaterialRendererServices.h"

struct ID3D11DeviceContext;

namespace irr
{
	namespace video
	{
		class CD3D11CallBridge;

		// D3D11-specific extension of IMaterialRendererServices. Exists so
		// CD3D11MaterialRenderer::OnRender/OnCompute can resolve the
		// CallBridge/Context to use FRESH, per call, via whichever object
		// is passed in as `service` -- instead of a CallBridge/Context
		// captured once at construction time (createMaterialRenderers()).
		//
		// This is the fix for a real bug: the old code called
		// `this->BridgeCalls->setVertexShader(...)` and
		// `shader->UnMapAll(this->Context)` directly, using members
		// permanently bound to whichever context was "immediate" when
		// createMaterialRenderers() ran. Any deferred-recording context
		// (same instance with swapped Context/BridgeCalls, or a genuinely
		// separate instance later) would be silently ignored for shader
		// binding, since these captured members never track what the
		// CALLING driver/context actually is right now.
		//
		// CD3D11Driver (and any future deferred-context implementation)
		// implements this by returning ITS OWN current BridgeCalls/Context
		// -- which for CD3D11Driver's Option-A recording swap, IS the
		// deferred one while recording, and the immediate one otherwise.
		class ID3D11MaterialRendererServices : public IMaterialRendererServices
		{
		public:
			virtual CD3D11CallBridge* getBridgeCalls() = 0;
			virtual ID3D11DeviceContext* getContext() = 0;
		};
	}
}
#endif // _IRR_COMPILE_WITH_DIRECT3D_11_
#endif