#pragma once

#include "CD3D11Driver.h"

namespace irr
{
	namespace video
	{
		class CD3D11DeferedDriver : public CD3D11Driver
		{
		public:
			CD3D11DeferedDriver(const SIrrlichtCreationParameters& params, io::IFileSystem* io, CD3D11Driver* currentDriver) :
				CD3D11Driver(params, io, (HWND)currentDriver->getExposedVideoData().D3D11.HWnd), m_currentDriver(currentDriver)
			{
			}

			bool initDriver(HWND hwnd, bool pureSoftware)
			{
				Device = m_currentDriver->getExposedVideoData().D3D11.D3DDev11;
				Device->AddRef();

				Device->CreateDeferredContext(0, &Context);
			}
		private:
			CD3D11Driver* m_currentDriver;
		};
	}
}

