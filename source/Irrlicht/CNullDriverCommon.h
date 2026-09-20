#pragma once

#include <vector>
#include "IVideoDriver.h"
#include "IFileSystem.h"
#include "IImagePresenter.h"
#include "IGPUProgrammingServices.h"
#include "irrArray.h"
#include "irrString.h"
#include "irrMap.h"
#include "IAttributes.h"
#include "IMesh.h"
#include "IMeshBuffer.h"
#include "IMeshSceneNode.h"
#include "CVertexDescriptor.h"
#include "CFPSCounter.h"
#include "S3DVertex.h"
#include "SLight.h"
#include "SExposedVideoData.h"

#ifdef _MSC_VER
#pragma warning( disable: 4996)
#endif

namespace irr
{
	namespace io
	{
		class IWriteFile;
		class IReadFile;
	} // end namespace io
	namespace video
	{
		class IImageLoader;
		class IImageWriter;

		class CNullDriverCommon : public IVideoDriver, public IGPUProgrammingServices
		{
		public:
			CNullDriverCommon(): ViewPort(0, 0, 0, 0),
				OverrideMaterial2DEnabled(false) {}


			//! sets a viewport
			void setViewPort(const core::rect<s32>& area)
			{
								ViewPort = area;
			}

			//! gets the area of the current viewport
			const core::rect<s32>& getViewPort() const
			{
				return ViewPort;
			}

			IVideoDriver* CNullDriverCommon::createDeferredContext();

			void CNullDriverCommon::executeDeferredContext(IDeferredContext* context);

				//! Create a named GPU timer. No-op here; EVDF_GPU_TIMER is off unless overridden.
				virtual void addGpuTimer(const core::stringc& name) override
				{
					if (GpuTimers.linear_search(name) == -1)
						GpuTimers.push_back(name);
				}

				//! Remove a GPU timer.
				virtual void removeGpuTimer(const core::stringc& name) override
				{
					const s32 index = GpuTimers.linear_search(name);
					if (index != -1)
						GpuTimers.erase(index);
				}

				//! Remove all GPU timers.
				virtual void removeAllGpuTimers() override { GpuTimers.clear(); }

				//! Start timing name. No-op here.
				virtual void beginGpuTimer(const core::stringc& name) override {}

				//! Stop timing name. No-op here.
				virtual void endGpuTimer(const core::stringc& name) override {}

				//! Update timer. No-op here.
				virtual void updateGpuTimer(const core::stringc& name, bool block = true) override {}

				//! Update all GPU timers. No-op here.
				virtual void updateAllGpuTimers(bool block = true) override {}

				//! Return timer result, in milliseconds of GPU time. Always 0 here.
				virtual f32 getGpuTimerResult(const core::stringc& name) const override { return 0.f; }
		protected:
			core::rect<s32> ViewPort;
			core::matrix4 TransformationMatrix;

				//! Names only: no real GPU resource is ever attached at this level.
				core::array<core::stringc> GpuTimers;

			SOverrideMaterial OverrideMaterial;
			SMaterial OverrideMaterial2D;
			SMaterial InitMaterial2D;
			bool OverrideMaterial2DEnabled;



			f32 FogStart;
			f32 FogEnd;
			f32 FogDensity;
			SColor FogColor;
			E_FOG_TYPE FogType;
			bool PixelFog;
			bool RangeFog;
		};
	}
}