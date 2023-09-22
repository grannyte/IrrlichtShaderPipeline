// Copyright (C) 2012 Patryk Nadrowski
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#ifndef __I_INDEX_BUFFER_H_INCLUDED__
#define __I_INDEX_BUFFER_H_INCLUDED__

#include "IReferenceCounted.h"
#include "irrArray.h"
#include "IHardwareBuffer.h"

namespace irr
{
namespace video
{
	enum E_INDEX_TYPE
	{
		EIT_16BIT = 0,
		EIT_32BIT
	};
}
namespace scene
{
	class IIndexBuffer :public IBuffer
	{
	public:
		IIndexBuffer() : IBuffer(EBT_INDEX)
		{
		}

		virtual ~IIndexBuffer() override
		{
		}

		virtual u32 getLast() = 0;

		virtual video::E_INDEX_TYPE getType() const = 0;

		virtual void setType(video::E_INDEX_TYPE type) = 0;

		virtual void addIndex(const u32& index) = 0;

		virtual u32 getIndex(u32 id) const = 0;

		virtual void* getIndices() = 0;

		virtual u32 getIndexCount() const = 0;

		virtual u32 getIndexSize() const = 0;

		virtual void setIndex(u32 id, u32 index) = 0;
	};
}
}

#endif
