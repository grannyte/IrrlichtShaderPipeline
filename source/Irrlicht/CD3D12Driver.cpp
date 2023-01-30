#include "CD3D12Driver.h"

void irr::video::CD3D12Driver::setBasicRenderStates(const SMaterial & material, const SMaterial & lastMaterial, bool resetAllRenderstates)
{
}

irr::s32 irr::video::CD3D12Driver::getVertexShaderConstantID(const c8 * name)
{
	return s32();
}

bool irr::video::CD3D12Driver::setVertexShaderConstant(s32 index, const f32 * floats, int count)
{
	return false;
}

bool irr::video::CD3D12Driver::setVertexShaderConstant(s32 index, const s32 * ints, int count)
{
	return false;
}

void irr::video::CD3D12Driver::setVertexShaderConstant(const f32 * data, s32 startRegister, s32 constantAmount)
{
}

void irr::video::CD3D12Driver::setPixelShaderConstant(const f32 * data, s32 startRegister, s32 constantAmount)
{
}

irr::s32 irr::video::CD3D12Driver::getPixelShaderConstantID(const c8 * name)
{
	return s32();
}

bool irr::video::CD3D12Driver::setPixelShaderConstant(s32 index, const f32 * floats, int count)
{
	return false;
}

bool irr::video::CD3D12Driver::setPixelShaderConstant(s32 index, const s32 * ints, int count)
{
	return false;
}

irr::video::IVideoDriver * irr::video::CD3D12Driver::getVideoDriver()
{
	return nullptr;
}
