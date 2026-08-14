#pragma once

// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h
//
#ifndef __C_D3D11_DEFERRED_CONTEXT_H_INCLUDED__
#define __C_D3D11_DEFERRED_CONTEXT_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "CD3D11Driver.h"

namespace irr
{
	namespace video
	{
		class CD3D11DeferredContext : public CD3D11Driver
		{
		public:
			// `immediate` is shared (Device is AddRef()'d), not owned --
			// caller guarantees it outlives this object.
			explicit CD3D11DeferredContext(CD3D11Driver* immediate);
			virtual ~CD3D11DeferredContext();

			// No backbuffer/Present concept on a deferred context -- both
			// no-ops, same reasoning as the Option-A design.
			virtual bool beginScene(bool backBuffer = true, bool zBuffer = true,
				SColor color = SColor(255, 0, 0, 0),
				const SExposedVideoData& videoData = SExposedVideoData(),
				core::rect<s32>* sourceRect = 0) override;
			virtual bool endScene() override;

			// IDeferredContext -- overridden because CD3D11Driver's own
			// implementation assumes Option-A semantics (restore a saved
			// immediate Context/BridgeCalls on the SAME instance), which
			// doesn't apply here: this object's Context/BridgeCalls are
			// permanently its own.
			virtual void execute(IVideoDriver* driver = nullptr) override;
			virtual void beginRecording() override;
			virtual void waitForCompletion() override;
			virtual IDeferredContext* getDeferredContextControl() override { return this; }

			// Recording into a deferred context's own deferred context
			// isn't a supported nesting -- fail loudly rather than silently
			// constructing something half-working.
			virtual IVideoDriver* createDeferredContext() override;

			// Same reasoning as getRendererFor() below -- this object's own table is never populated.
			virtual IVertexDescriptor* getVertexDescriptor(u32 id) const override
			{
				return ImmediateDriver->getVertexDescriptor(id);
			}
			virtual IVertexDescriptor* getVertexDescriptor(const core::stringc& pName) const override
			{
				return ImmediateDriver->getVertexDescriptor(pName);
			}
			virtual u32 getVertexDescriptorCount() const override
			{
				return ImmediateDriver->getVertexDescriptorCount();
			}
			virtual IVertexDescriptor* addVertexDescriptor(const core::stringc& pName) override
			{
				return ImmediateDriver->addVertexDescriptor(pName);
			}

			// Same table as getRendererFor() below -- this object's own count is always 0.
			virtual u32 getMaterialRendererCount() const override
			{
				return ImmediateDriver->getMaterialRendererCount();
			}
			virtual IMaterialRenderer* getMaterialRenderer(u32 idx) override
			{
				return ImmediateDriver->getMaterialRenderer(idx);
			}
			virtual const char* getMaterialRendererName(u32 idx) const override
			{
				return ImmediateDriver->getMaterialRendererName(idx);
			}

			// Registration must land in the SAME table getRendererFor() reads, or a material created
			// while recording gets an index only valid here and resolves to a different renderer.
			virtual s32 addMaterialRenderer(IMaterialRenderer* renderer, const char* name = 0) override
			{
				return ImmediateDriver->addMaterialRenderer(renderer, name);
			}
			virtual IGPUProgrammingServices* getGPUProgrammingServices() override
			{
				return ImmediateDriver->getGPUProgrammingServices();
			}

		protected:
			// Redirect to the immediate driver's authoritative table
			// instead of this object's own (never populated) one.
			virtual IMaterialRenderer* getRendererFor(u32 materialType) override
			{
				return ImmediateDriver->getMaterialRenderer(materialType);
			}

		private:
			CD3D11Driver* ImmediateDriver;
			ID3D11Query* CompletionQuery;
			void createCompletionQuery();
		};
	}
}
#endif // _IRR_COMPILE_WITH_DIRECT3D_11_
#endif