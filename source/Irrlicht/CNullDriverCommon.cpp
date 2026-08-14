#include "CNullDriverCommon.h"
#include "CCommandBufferDriver.h"
irr::video::IVideoDriver* irr::video::CNullDriverCommon::createDeferredContext()
{
	return new CCommandBufferDriver(this);
}

void irr::video::CNullDriverCommon::executeDeferredContext(IDeferredContext* context)
{
	if (context) context->execute(this);
}
