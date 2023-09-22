// Copyright (C) 2012 Patryk Nadrowski
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#ifndef __I_VERTEX_BUFFER_H_INCLUDED__
#define __I_VERTEX_BUFFER_H_INCLUDED__

#include "IReferenceCounted.h"
#include "irrArray.h"
#include "IHardwareBuffer.h"
#include "IBuffer.h"

namespace irr
{
namespace scene
{
	class IVertexBuffer :public IBuffer
	{
	public:
		IVertexBuffer() : IBuffer(EBT_VERTEX)
		{
		}

		virtual ~IVertexBuffer() override
		{
		}

		virtual s32 linear_reverse_search(const void* element) const = 0;

		virtual void fill(u32 used) = 0;

		virtual void addVertex(const void* vertex) = 0;

		virtual const void* getVertex(u32 id) const = 0;

		virtual void* getVertices() = 0;

		virtual u32 getVertexCount() const = 0;

		virtual u32 getVertexSize() const = 0;

		virtual void setVertex(u32 id, const void* vertex) = 0;
	};
}
}

#endif
