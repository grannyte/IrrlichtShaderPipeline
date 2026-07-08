#pragma once

// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h
//
#ifndef __I_DEFERRED_CONTEXT_H_INCLUDED__
#define __I_DEFERRED_CONTEXT_H_INCLUDED__

#include <cstddef>

namespace irr
{
	namespace video
	{
		class IVideoDriver;

		// Control surface for a deferred recording context. Deliberately
		// does NOT inherit IVideoDriver -- an earlier version of this design
		// did, which forced IVideoDriver to become a virtual base wherever
		// this interface was combined with a concrete driver base
		// (CNullDriverCommon). That broke every existing
		// static_cast<ConcreteDriver*>(IVideoDriver*) downcast across every
		// material renderer in every backend (D3D9/D3D11/D3D12/OpenGL),
		// since a virtual base can't be the source of a static_cast.
		//
		// Instead: a deferred context is still an ordinary IVideoDriver
		// (via CNullDriverCommon, non-virtually, same as any other driver).
		// This interface is obtained separately, via
		// IVideoDriver::getDeferredContextControl(), which returns nullptr
		// on every ordinary driver and `this` on an actual deferred context.
		// No diamond, no virtual base, no dynamic_cast anywhere.
		class IDeferredContext
		{
		public:
			virtual ~IDeferredContext() {}

			// Replays recorded commands against `driver`, or against
			// whatever driver this context was created from if none given.
			// This is the underlying mechanism -- callers should normally
			// go through IVideoDriver::executeDeferredContext(context)
			// instead, called on the REAL driver (mirrors D3D11's
			// ImmediateContext->ExecuteCommandList(commandList): the
			// immediate context consumes the recording, the recording
			// doesn't execute itself). execute() stays public here mainly
			// so executeDeferredContext()'s default implementation has
			// something to forward to.
			virtual void execute(IVideoDriver* driver = nullptr) = 0;

			// Clears out any not-yet-executed commands so this context can
			// be reused for the next frame/batch. Only safe to call once
			// execute() has fully drained the previous batch.
			virtual void beginRecording() = 0;

			virtual size_t pendingCommandCount() const = 0;

			// No-op on backends with nothing async to wait on. Meaningful
			// on backends with real GPU/CPU decoupling (e.g. a future
			// native D3D12 path) where it blocks on a fence.
			virtual void waitForCompletion() = 0;
		};
	}
}
#endif