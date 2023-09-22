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
		protected:
			core::rect<s32> ViewPort;
			core::matrix4 TransformationMatrix;

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