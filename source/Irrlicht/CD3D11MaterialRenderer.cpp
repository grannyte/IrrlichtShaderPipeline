// Copyright (C) 2002-2009 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "CD3D11MaterialRenderer.h"
#include "IShaderConstantSetCallBack.h"
#include "IVideoDriver.h"
#include "os.h"
#include "irrString.h"
#include "IFileSystem.h"
#include "irrMap.h"
#include "CD3D11Driver.h"
#include "CD3D11CallBridge.h"

#include <d3dcompiler.h>

#include <ppl.h>

concurrency::critical_section mylock;
//this is a nasty hack not thread safe
const irr::core::stringc value = irr::core::stringc(irr::video::MATERIAL_MAX_TEXTURES);

class CShaderInclude : public ID3DInclude
{
public:
	CShaderInclude(irr::io::IFileSystem* fileSystem)
	{
		FileSystem = fileSystem;
	}

	STDMETHOD(Close(LPCVOID pData))
	{
		irr::c8* data = (irr::c8*)pData;

		if (data)
			delete[] data;

		return S_OK;
	}

	STDMETHOD(Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes))
	{
		irr::core::stringc lfilename = "media//shaders//";
		lfilename.append(pFileName);
		irr::io::IReadFile* file = FileSystem->createAndOpenFile(lfilename);

		if (!file)
		{
			irr::os::Printer::log("Could not open included shader program file",
				lfilename, irr::ELL_ERROR);
			return S_FALSE;
		}

		irr::u32 mBytes = file->getSize();
		(*pBytes) = mBytes;

		irr::c8* retInclude = new irr::c8[mBytes + 1];
		ZeroMemory(retInclude, mBytes + 1);
		ZeroMemory(retInclude, mBytes + 1);
		file->read(retInclude, mBytes);
		retInclude[mBytes] = '\0';

		(*ppData) = retInclude;

		file->drop();

		return S_OK;
	}

private:
	irr::io::IFileSystem* FileSystem;
};

namespace irr
{
	namespace video
	{
		//! Public constructor
		CD3D11MaterialRenderer::CD3D11MaterialRenderer(ID3D11Device* device, video::IVideoDriver* driver, CD3D11CallBridge* bridgeCalls, s32& outMaterialTypeNr,
			const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
			const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut, CD3D11VertexDescriptor* vertexTypeOut,
			IShaderConstantSetCallBack* callback, IMaterialRenderer* baseRenderer, s32 userData, io::IFileSystem* fileSystem, E_GPU_SHADING_LANGUAGE ShadingLang)
			: Driver(driver), Device(device), Context(NULL), BridgeCalls(bridgeCalls),
			BaseRenderer(baseRenderer), CallBack(callback), UserData(userData),
			includer(NULL), buffer(NULL), StreamOutputFormat(vertexTypeOut), Lang(ShadingLang)
		{
#ifdef _DEBUG
			setDebugName("CD3D11MaterialRenderer");
#endif

			outMaterialTypeNr = -1;

			for (u32 i = 0; i < EST_COUNT; ++i)
				shaders[i] = NULL;

			if (Device)
			{
				Device->AddRef();
				Device->GetImmediateContext(&Context);
			}

			if (BaseRenderer)
				BaseRenderer->grab();

			if (CallBack)
				CallBack->grab();

			if (fileSystem)
				includer = new CShaderInclude(fileSystem);

			if (!init(vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
				pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
				geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
				NULL, NULL, (E_HULL_SHADER_TYPE)0,  // hull shader
				NULL, NULL, (E_DOMAIN_SHADER_TYPE)0,  // domain shader
				NULL, NULL, (E_COMPUTE_SHADER_TYPE)0)) // compute shader
				return;

			outMaterialTypeNr = driver->addMaterialRenderer(this);
		}
		//! Public constructor
		CD3D11MaterialRenderer::CD3D11MaterialRenderer(ID3D11Device* device, video::IVideoDriver* driver, CD3D11CallBridge* bridgeCalls, s32& outMaterialTypeNr,
			const c8* computeShaderProgram, const c8* computeShaderEntryPointName, E_COMPUTE_SHADER_TYPE csCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut, CD3D11VertexDescriptor* vertexTypeOut,				// Only for DirectX 11
			IShaderConstantSetCallBack* callback, IMaterialRenderer* baseRenderer, s32 userData, io::IFileSystem* fileSystem, E_GPU_SHADING_LANGUAGE ShadingLang)
			: Driver(driver), Device(device), Context(NULL), BridgeCalls(bridgeCalls),
			BaseRenderer(baseRenderer), CallBack(callback), UserData(userData),
			includer(NULL), buffer(NULL), StreamOutputFormat(vertexTypeOut), Lang(ShadingLang)
		{
#ifdef _DEBUG
			setDebugName("CD3D11MaterialRenderer");
#endif

			outMaterialTypeNr = -1;

			for (u32 i = 0; i < EST_COUNT; ++i)
				shaders[i] = NULL;

			if (Device)
			{
				Device->AddRef();
				Device->GetImmediateContext(&Context);
			}

			if (BaseRenderer)
				BaseRenderer->grab();

			if (CallBack)
				CallBack->grab();

			if (fileSystem)
				includer = new CShaderInclude(fileSystem);

			if (!init(NULL, NULL, (E_VERTEX_SHADER_TYPE)0,
				NULL, NULL, (E_PIXEL_SHADER_TYPE)0,
				NULL, NULL, (E_GEOMETRY_SHADER_TYPE)0,
				NULL, NULL, (E_HULL_SHADER_TYPE)0,  // hull shader
				NULL, NULL, (E_DOMAIN_SHADER_TYPE)0,  // domain shader
				computeShaderProgram, computeShaderEntryPointName, (E_COMPUTE_SHADER_TYPE)csCompileTarget)) // compute shader
				return;

			outMaterialTypeNr = driver->addMaterialRenderer(this);
		}

		//! Public constructor
		CD3D11MaterialRenderer::CD3D11MaterialRenderer(ID3D11Device* device, video::IVideoDriver* driver, CD3D11CallBridge* bridgeCalls, s32& outMaterialTypeNr,
			const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
			const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			const c8* hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
			const c8* domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType, u32 verticesOut, CD3D11VertexDescriptor* vertexTypeOut,				// Only for DirectX 11
			IShaderConstantSetCallBack* callback, IMaterialRenderer* baseRenderer, s32 userData, io::IFileSystem* fileSystem, E_GPU_SHADING_LANGUAGE ShadingLang)
			: Driver(driver), Device(device), Context(NULL), BridgeCalls(bridgeCalls),
			BaseRenderer(baseRenderer), CallBack(callback), UserData(userData),
			includer(NULL), buffer(NULL), StreamOutputFormat(vertexTypeOut), Lang(ShadingLang)
		{
#ifdef _DEBUG
			setDebugName("CD3D11MaterialRenderer");
#endif

			outMaterialTypeNr = -1;

			for (u32 i = 0; i < EST_COUNT; ++i)
				shaders[i] = NULL;

			if (Device)
			{
				Device->AddRef();
				Device->GetImmediateContext(&Context);
			}

			if (BaseRenderer)
				BaseRenderer->grab();

			if (CallBack)
				CallBack->grab();

			if (fileSystem)
				includer = new CShaderInclude(fileSystem);

			if (!init(vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
				pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
				geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
				hullShaderProgram, hullShaderEntryPointName, hsCompileTarget,  // hull shader
				domainShaderProgram, domainShaderEntryPointName, dsCompileTarget,  // domain shader
				NULL, NULL, (E_COMPUTE_SHADER_TYPE)0)) // compute shader
				return;

			outMaterialTypeNr = driver->addMaterialRenderer(this);
		}

		CD3D11MaterialRenderer::CD3D11MaterialRenderer(ID3D11Device* device, video::IVideoDriver* driver, CD3D11CallBridge* bridgeCalls, IShaderConstantSetCallBack* callback, IMaterialRenderer* baseRenderer, io::IFileSystem* fileSystem, s32 userData)
			: Driver(driver), Device(device), Context(NULL), BridgeCalls(bridgeCalls), BaseRenderer(baseRenderer), CallBack(callback), UserData(userData),
			includer(NULL), buffer(NULL), StreamOutputFormat(NULL), Lang(EGSL_DEFAULT)
		{
#ifdef _DEBUG
			setDebugName("CD3D11MaterialRenderer");
#endif

			if (Device)
			{
				Device->AddRef();
				Device->GetImmediateContext(&Context);
			}

			if (BaseRenderer)
				BaseRenderer->grab();

			if (CallBack)
				CallBack->grab();

			for (u32 i = 0; i < EST_COUNT; ++i)
				shaders[i] = NULL;

			if (fileSystem)
				includer = new CShaderInclude(fileSystem);
		}

		CD3D11MaterialRenderer::~CD3D11MaterialRenderer()
		{
			if (BaseRenderer)
				BaseRenderer->drop();

			if (Context)
				Context->Release();

			if (Device)
				Device->Release();

			if (CallBack)
				CallBack->drop();

			if (includer)
				delete includer;

			for (u32 i = 0; i < EST_COUNT; ++i)
				if (shaders[i])
					shaders[i]->Release();

			if (buffer)
				buffer->Release();
		}

		void CD3D11MaterialRenderer::addMacros(core::array<D3D_SHADER_MACRO>& macroArray)
		{
			D3D_SHADER_MACRO macro;
			// indicates how match textures are supported by the engine
			macro.Name = "MAX_TEXTURES";
			macro.Definition = value.c_str();
			macroArray.push_back(macro);

			// last macro has to be NULL
			macro.Definition = NULL;
			macro.Name = NULL;
			macroArray.push_back(macro);
		}

		bool CD3D11MaterialRenderer::createShader(const char* code,
			const char* entryPointName,
			const char* targetName, UINT flags, E_SHADER_TYPE type)
		{
			if (!code)
				return true;

			HRESULT hr = 0;
			ID3DBlob* shaderBuffer = 0;

			core::array<D3D_SHADER_MACRO> macroArray;

			size_t shadersize = (size_t)entryPointName;

			addMacros(macroArray);
			if (Lang != EGSL_PCMP)
			{
				if (!compileShader(code, strlen(code), "", &macroArray[0], includer, entryPointName, targetName, flags, 0, &shaderBuffer))
					return false;

				if (!shaderBuffer)
					return false;
				shadersize = shaderBuffer->GetBufferSize();
			}
			else
			{
				D3DCreateBlob(shadersize, &shaderBuffer);
				memcpy(shaderBuffer->GetBufferPointer(), code, shadersize);
			}

			shaders[type] = new SShader();
			if (Lang != EGSL_PCMP)
			{
				shaders[type]->Name = entryPointName;

				os::Printer::log(entryPointName, irr::ELL_DEBUG);

				shaders[type]->Code = code;
			}

			// Create the shader from the buffer.
			switch (type)
			{
			case EST_VERTEX_SHADER:
				hr = Device->CreateVertexShader(shaderBuffer->GetBufferPointer(), shaderBuffer->GetBufferSize(), NULL, (ID3D11VertexShader**)(&shaders[type]->shader));
				break;
			case EST_PIXEL_SHADER:
				hr = Device->CreatePixelShader(shaderBuffer->GetBufferPointer(), shaderBuffer->GetBufferSize(), NULL, (ID3D11PixelShader**)(&shaders[type]->shader));
				break;
			case EST_GEOMETRY_SHADER:
				hr = Device->CreateGeometryShader(shaderBuffer->GetBufferPointer(), shaderBuffer->GetBufferSize(), NULL, (ID3D11GeometryShader**)(&shaders[type]->shader));
				break;
			case EST_DOMAIN_SHADER:
				hr = Device->CreateDomainShader(shaderBuffer->GetBufferPointer(), shaderBuffer->GetBufferSize(), NULL, (ID3D11DomainShader**)(&shaders[type]->shader));
				break;
			case EST_HULL_SHADER:
				hr = Device->CreateHullShader(shaderBuffer->GetBufferPointer(), shaderBuffer->GetBufferSize(), NULL, (ID3D11HullShader**)(&shaders[type]->shader));
				break;
			case EST_COMPUTE_SHADER:
				hr = Device->CreateComputeShader(shaderBuffer->GetBufferPointer(), shaderBuffer->GetBufferSize(), NULL, (ID3D11ComputeShader**)(&shaders[type]->shader));
				break;
			case EST_STREAM_OUTPUT_SHADER:
			{
				irr::core::array<u32> strides;
				irr::u32 stridecount = 0;
				for (; stridecount < 3; ++stridecount)
				{
					strides.push_back(StreamOutputFormat->getVertexSize(stridecount));
				}
				//flags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
				hr = Device->CreateGeometryShaderWithStreamOutput(shaderBuffer->GetBufferPointer(), shaderBuffer->GetBufferSize(), &StreamOutputFormat->getOutputLayoutDescription()[0], StreamOutputFormat->getOutputLayoutDescription().size(), &strides[0], stridecount, D3D11_SO_NO_RASTERIZED_STREAM, 0, (ID3D11GeometryShader**)(&shaders[type]->shader));
				break;
			}
			}

			if (FAILED(hr))
			{
				core::stringc errorMsg = "Could not create ";

				switch (type)
				{
				case EST_VERTEX_SHADER:
					errorMsg += "vertexshader";
					break;
				case EST_PIXEL_SHADER:
					errorMsg += "pixelshader";
					break;
				case EST_GEOMETRY_SHADER:
					errorMsg += "geometryshader";
					break;
				case EST_DOMAIN_SHADER:
					errorMsg += "domainshader";
					break;
				case EST_HULL_SHADER:
					errorMsg += "hullshader";
					break;
				case EST_COMPUTE_SHADER:
					errorMsg += "computeshader";
					break;
				case EST_STREAM_OUTPUT_SHADER:
					errorMsg += "Streamshader";
					break;
				}

				logFormatError(hr, errorMsg);

				return false;
			}

			createResources(shaderBuffer, type);

			switch (type)
			{
			case EST_VERTEX_SHADER:
				buffer = shaderBuffer;
				buffer->AddRef();
				break;
			case EST_GEOMETRY_SHADER:
				if (!buffer)
				{
					buffer = shaderBuffer;
					buffer->AddRef();
				}
				break;
			case EST_STREAM_OUTPUT_SHADER:
				if (!buffer)
				{
					buffer = shaderBuffer;
					buffer->AddRef();
				}
				break;
			}

			// Release the vertex shader buffer and pixel shader buffer since they are no longer needed.
			shaderBuffer->Release();
			shaderBuffer = 0;

			return true;
		}

		SShaderVariable* CD3D11MaterialRenderer::getVariable(SShader* sh, s32 id)
		{
			if (id < 0 || id >= (s32)sh->variableArray.size())
				return NULL;

			return sh->variableArray[id];
		}

		SShaderBuffer* CD3D11MaterialRenderer::getBuffer(E_SHADER_TYPE type, s32 id)
		{
			SShader* sh = shaders[type];

			if (!sh)
				return NULL;

			if (id < 0 || id >= (s32)sh->bufferArray.size())
				return NULL;

			return sh->bufferArray[id];
		}

		bool CD3D11MaterialRenderer::setVariable(s32 id, const f32* floats, int count, E_SHADER_TYPE type)
		{
			SShader* sh = shaders[type];

			if (!sh)
				return false;

			SShaderVariable* var = getVariable(sh, id);

			if (!var)
				return false;

			core::matrix4* m = NULL;
			// if it is a matrix transpose it
			if (var->classType == D3D_SVC_MATRIX_COLUMNS)
			{
				m = (core::matrix4*)floats;
				*m = m->getTransposed();
			}
			SShaderBuffer* buff = getBuffer(type, var->buffer);
			if (buff == NULL)
				return false;
			c8* byteData = (c8*)buff->cData;
			byteData += var->offset;

			if (count * sizeof(f32) > buff->size - var->offset)
			{
				os::Printer::log((irr::core::stringw(L"Failed to set shader constant ") + irr::core::stringw(var->name) + L" entity to large").c_str());
				return false;
			}

			if (m)
				memcpy(byteData, m, count * sizeof(f32));
			else
				memcpy(byteData, floats, count * sizeof(f32));

			return true;
		}

		bool CD3D11MaterialRenderer::setVariable(s32 id, const s32* ints, int count, E_SHADER_TYPE type)
		{
			SShader* sh = shaders[type];

			if (!sh)
				return false;

			SShaderVariable* var = getVariable(sh, id);

			if (!var)
				return false;

			SShaderBuffer* buff = getBuffer(type, var->buffer);
			if (buff == NULL)
				return false;
			c8* byteData = (c8*)buff->cData;
			byteData += var->offset;
			memcpy(byteData, ints, count * sizeof(s32));
			;

			return true;
		}

		bool CD3D11MaterialRenderer::setConstantBuffer(s32 id, const void* data, E_SHADER_TYPE type)
		{
			SShaderBuffer* buff = getBuffer(type, id);

			if (!buff)
				return false;

			D3D11_MAPPED_SUBRESOURCE mappedData;

			HRESULT hr = Context->Map(buff->data, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedData);

			if (FAILED(hr))
			{
				logFormatError(hr, "Could not map constant buffer in shader");
				return false;
			}

			memcpy(mappedData.pData, data, buff->size);

			Context->Unmap(buff->data, 0);

			return false;
		}

		s32 CD3D11MaterialRenderer::getConstantBufferID(const c8* name, E_SHADER_TYPE type)
		{
			SShader* sh = shaders[type];

			if (sh)
			{
				const u32 size = sh->bufferArray.size();

				for (u32 i = 0; i < size; ++i)
				{
					if (sh->bufferArray[i]->name == name)
						return i;
				}
			}

			core::stringc s = "HLSL buffer to get ID not found: '";
			s += name;
			s += "'. Available buffers are:";
			os::Printer::log(s.c_str(), ELL_WARNING);

			printBuffers(type);

			return -1;
		}

		s32 CD3D11MaterialRenderer::getVariableID(const c8* name, E_SHADER_TYPE type)
		{
			SShader* sh = shaders[type];

			if (sh)
			{
				const u32 size = sh->variableArray.size();

				for (u32 i = 0; i < size; ++i)
				{
					if (strcmp(sh->variableArray[i]->name.c_str(), name) == 0)
						return i;
				}
			}

			core::stringc s = "HLSL variable to get ID not found: '";
			s += name;
			s += "'. Available variables are:";
			os::Printer::log(s.c_str(), ELL_WARNING);

			printVariables(type);

			return -1;
		}

		bool CD3D11MaterialRenderer::OnRender(IMaterialRendererServices* service, IVertexDescriptor* vtxtype)
		{
			if (!Context)
				return false;

			//	if(BaseRenderer && (vtxtype == EVT_STANDARD || vtxtype == EVT_2TCOORDS || vtxtype == EVT_TANGENTS))
			//		BaseRenderer->OnRender(service, vtxtype);

			if (CallBack)
				CallBack->OnSetConstants(service, UserData);
			if (BaseRenderer)
			{
				if (shaders[EST_VERTEX_SHADER])
					shaders[EST_VERTEX_SHADER]->UnMapAll(Context);

				if (shaders[EST_PIXEL_SHADER])
					shaders[EST_PIXEL_SHADER]->UnMapAll(Context);

				if (shaders[EST_GEOMETRY_SHADER])
					shaders[EST_GEOMETRY_SHADER]->UnMapAll(Context);
				else if (shaders[EST_STREAM_OUTPUT_SHADER])
					shaders[EST_STREAM_OUTPUT_SHADER]->UnMapAll(Context);

				if (shaders[EST_HULL_SHADER])
					shaders[EST_HULL_SHADER]->UnMapAll(Context);

				if (shaders[EST_DOMAIN_SHADER])
					shaders[EST_DOMAIN_SHADER]->UnMapAll(Context);

				if (shaders[EST_COMPUTE_SHADER])
					shaders[EST_COMPUTE_SHADER]->UnMapAll(Context);
			}

			BridgeCalls->setVertexShader(shaders[EST_VERTEX_SHADER]);
			BridgeCalls->setPixelShader(shaders[EST_PIXEL_SHADER]);
			if (shaders[EST_GEOMETRY_SHADER])
				BridgeCalls->setGeometryShader(shaders[EST_GEOMETRY_SHADER]);
			else
				BridgeCalls->setGeometryShader(shaders[EST_STREAM_OUTPUT_SHADER]);

			BridgeCalls->setHullShader(shaders[EST_HULL_SHADER]);
			BridgeCalls->setDomainShader(shaders[EST_DOMAIN_SHADER]);
			BridgeCalls->setComputeShader(shaders[EST_COMPUTE_SHADER]);

			return true;
		}

		bool CD3D11MaterialRenderer::OnCompute(IMaterialRendererServices* service)
		{
			if (!Context)
				return false;

			//	if(BaseRenderer && (vtxtype == EVT_STANDARD || vtxtype == EVT_2TCOORDS || vtxtype == EVT_TANGENTS))
			//		BaseRenderer->OnRender(service, vtxtype);

			if (CallBack)
				CallBack->OnSetConstants(service, UserData);
			if (BaseRenderer)
			{
				if (shaders[EST_COMPUTE_SHADER])
					shaders[EST_COMPUTE_SHADER]->UnMapAll(Context);
			}
			BridgeCalls->setComputeShader(shaders[EST_COMPUTE_SHADER]);

			return true;
		}

		void CD3D11MaterialRenderer::OnSetMaterial(const video::SMaterial& material, const video::SMaterial& lastMaterial,
			bool resetAllRenderstates, video::IMaterialRendererServices* services)
		{
			if (material.MaterialType != lastMaterial.MaterialType || resetAllRenderstates)
			{
				if (BaseRenderer)
				{
					//os::Printer::log("Befor Base OnsetMaterial");
					BaseRenderer->OnSetMaterial(material, material, resetAllRenderstates, services);
					//os::Printer::log("After Base OnsetMaterial");
				}
				else
				{
					//os::Printer::log("Befor Base setBasicRenderStates");
					services->setBasicRenderStates(material, lastMaterial, resetAllRenderstates);
					//os::Printer::log("After Base setBasicRenderStates");
				}
			}

			//let callback know used material
			if (CallBack)
			{
				//os::Printer::log("Befor OnsetMaterial");
				CallBack->OnSetMaterial(material);
				//os::Printer::log("After OnsetMaterial");
			}
		}

		void CD3D11MaterialRenderer::OnUnsetMaterial()
		{
			if (BaseRenderer)
				BaseRenderer->OnUnsetMaterial();
		}

		//! get shader signature
		void* CD3D11MaterialRenderer::getShaderByteCode() const
		{
			return buffer ? buffer->GetBufferPointer() : NULL;
		}

		//! get shader signature size
		u32 CD3D11MaterialRenderer::getShaderByteCodeSize() const
		{
			return buffer ? buffer->GetBufferSize() : 0;
		}

		void CD3D11MaterialRenderer::printVariables(E_SHADER_TYPE type)
		{
			SShader* sh = shaders[type];

			if (!sh)
				return;

			core::stringc text = "";

			const u32 size = sh->variableArray.size();
			for (u32 i = 0; i < size; ++i)
			{
				text += "Name: ";
				text += sh->variableArray[i]->name;
				text += " Size: ";
				text += sh->variableArray[i]->size;
				text += " Offset: ";
				text += sh->variableArray[i]->offset;
				text += "\n";
			}
			os::Printer::log(text.c_str());
		}

		void CD3D11MaterialRenderer::printBuffers(E_SHADER_TYPE type)
		{
			SShader* sh = shaders[type];

			if (!sh)
				return;

			core::stringc text = "";

			const u32 size = sh->bufferArray.size();

			for (u32 i = 0; i < size; ++i)
			{
				text += "Name: ";
				text += sh->bufferArray[i]->name;
				text += " Size: ";
				text += sh->bufferArray[i]->size;
				text += "\n";
			}

			os::Printer::log(text.c_str());
		}

		bool CD3D11MaterialRenderer::init(const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName, E_VERTEX_SHADER_TYPE vsCompileTarget,
			const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName, E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			const c8* hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
			const c8* domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
			const c8* computeShaderProgram, const c8* computeShaderEntryPointName, E_COMPUTE_SHADER_TYPE csCompileTarget)
		{
			if (vsCompileTarget < 0 || vsCompileTarget > EVST_COUNT)
			{
				os::Printer::log("Invalid HLSL vertex shader compilation target.", ELL_ERROR);
				return false;
			}
			UINT flags = 0;
			if (vsCompileTarget >= EVST_VS_4_0 && psCompileTarget >= EPST_PS_4_0)
			{
				//flags |= D3DCOMPILE_ENABLE_STRICTNESS;
			}
			else
			{
				flags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
				vsCompileTarget = EVST_VS_5_0;
				psCompileTarget = EPST_PS_5_0;
			}

#ifdef _DEBUG
			// These values allow use of PIX and shader debuggers
			flags |= D3DCOMPILE_DEBUG;
			flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#else
			// These flags allow maximum performance
			flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
			if (Lang != EGSL_PCMP && pixelShaderEntryPointName && strcmp("pixelNoiseMain", pixelShaderEntryPointName))
			{
				flags |= D3DCOMPILE_AVOID_FLOW_CONTROL;
			}
			else
			{
				flags |= D3DCOMPILE_PREFER_FLOW_CONTROL;
			}

#endif

			if (!createShader(vertexShaderProgram, vertexShaderEntryPointName, VERTEX_SHADER_TYPE_NAMES[vsCompileTarget], flags, EST_VERTEX_SHADER))
				return false;

			if (!createShader(pixelShaderProgram, pixelShaderEntryPointName, PIXEL_SHADER_TYPE_NAMES[psCompileTarget], flags, EST_PIXEL_SHADER))
				return false;

			if (!StreamOutputFormat && !createShader(geometryShaderProgram, geometryShaderEntryPointName, GEOMETRY_SHADER_TYPE_NAMES[gsCompileTarget], flags, EST_GEOMETRY_SHADER))
				return false;

			if (!createShader(domainShaderProgram, domainShaderEntryPointName, DOMAIN_SHADER_TYPE_NAMES[dsCompileTarget], flags, EST_DOMAIN_SHADER))
				return false;

			if (!createShader(hullShaderProgram, hullShaderEntryPointName, HULL_SHADER_TYPE_NAMES[hsCompileTarget], flags, EST_HULL_SHADER))
				return false;

			if (!createShader(computeShaderProgram, computeShaderEntryPointName, COMPUTE_SHADER_TYPE_NAMES[csCompileTarget], flags, EST_COMPUTE_SHADER))
				return false;

			if (StreamOutputFormat && !createShader(geometryShaderProgram, geometryShaderEntryPointName, GEOMETRY_SHADER_TYPE_NAMES[gsCompileTarget], flags, EST_STREAM_OUTPUT_SHADER))
				return false;

			return true;
		}

		bool CD3D11MaterialRenderer::compileShader(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude,
			LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2, ID3DBlob** ppCode)
		{
			ID3DBlob* ppErrorMsgs = 0;

			HRESULT hr = D3DCompile(pSrcData, SrcDataSize, pSourceName, pDefines, pInclude, pEntrypoint, pTarget, Flags1, Flags2, ppCode, &ppErrorMsgs);

			if (FAILED(hr))
			{
				core::stringc errorMsg = "Could not compile shader";

				if (ppErrorMsgs)
				{
					errorMsg += ": ";
					errorMsg += static_cast<const char*>(ppErrorMsgs->GetBufferPointer());

					ppErrorMsgs->Release();
				}

				logFormatError(hr, errorMsg);

				return false;
			}
#ifdef _DEBUG
			else if (ppErrorMsgs)
			{
				core::stringc errorMsg = "Shader compilation warning: ";
				errorMsg += static_cast<const char*>(ppErrorMsgs->GetBufferPointer());

				ppErrorMsgs->Release();
				ppErrorMsgs = NULL;

				os::Printer::log(errorMsg.c_str(), ELL_WARNING);
			}
#endif

			if (ppErrorMsgs)
				ppErrorMsgs->Release();

			return true;
		}

		SShaderBuffer* CD3D11MaterialRenderer::createConstantBuffer(D3D11_SHADER_BUFFER_DESC& bufferDesc, E_SHADER_TYPE  type)
		{
			SShaderBuffer* sBuffer = NULL;

			// take the same buffer from the other shader if it has the same name

			SShader* shader = shaders[type];

			if (shader == NULL)
				return NULL;

			bool found = false;

			for (u32 j = 0; j < shader->bufferArray.size(); ++j)
			{
				if (shader->bufferArray[j]->name == bufferDesc.Name)
				{
					sBuffer = shader->bufferArray[j];
					sBuffer->AddRef();
					found = true;
					break;
				}
			}

			// no buffer found so create a new one
			if (!sBuffer)
			{
				sBuffer = new SShaderBuffer();
				sBuffer->name = bufferDesc.Name;
				sBuffer->size = bufferDesc.Size;
			}

			if (!sBuffer->data)
			{
				D3D11_BUFFER_DESC cbDesc;
				cbDesc.ByteWidth = sBuffer->size;
				cbDesc.Usage = D3D11_USAGE_DYNAMIC;
				cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				cbDesc.MiscFlags = 0;
				cbDesc.StructureByteStride = 0;

				// Create the buffer.
				HRESULT hr = Device->CreateBuffer(&cbDesc, NULL, &sBuffer->data);

				if (FAILED(hr) || !sBuffer->data)
				{
					core::stringc error = "Could not create constant buffer \"";
					error += sBuffer->name;
					error += "\"";

					logFormatError(hr, error);

					delete sBuffer;

					return NULL;
				}

				sBuffer->cData = malloc(sBuffer->size);
			}

			return sBuffer;
		}

		void CD3D11MaterialRenderer::createResources(ID3DBlob* code, E_SHADER_TYPE type)
		{
			// D3DXCompile
			ID3D11ShaderReflection* pReflector = NULL;

			HRESULT hr = D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
				IID_ID3D11ShaderReflection, (void**)&pReflector);

			if (FAILED(hr))
			{
				logFormatError(hr, "Could not reflect shader");

				return;
			}

			D3D11_SHADER_DESC shaderDesc;
			ZeroMemory(&shaderDesc, sizeof(D3D11_SHADER_DESC));
			pReflector->GetDesc(&shaderDesc);

			SShader* sh = shaders[type];

			for (u32 i = 0; i < shaderDesc.BoundResources; ++i)
			{
				D3D11_SHADER_INPUT_BIND_DESC resourceDesc;
				ZeroMemory(&resourceDesc, sizeof(D3D11_SHADER_INPUT_BIND_DESC));
				pReflector->GetResourceBindingDesc(i, &resourceDesc);
				switch (resourceDesc.Type)
				{
				case D3D_SIT_CBUFFER:
				{
					ID3D11ShaderReflectionConstantBuffer* reflectionBuffer = pReflector->GetConstantBufferByName(resourceDesc.Name);

					D3D11_SHADER_BUFFER_DESC bufferDesc;
					ZeroMemory(&bufferDesc, sizeof(D3D11_SHADER_BUFFER_DESC));
					reflectionBuffer->GetDesc(&bufferDesc);

					SShaderBuffer* sBuffer = createConstantBuffer(bufferDesc, type);

					if (sBuffer)
					{
						irr::core::stringc bufferstring = sBuffer->name;
						bufferstring += " Bind Point : ";
						bufferstring += resourceDesc.BindPoint;

						os::Printer::log(bufferstring.c_str(), irr::ELL_DEBUG);
						sBuffer->bindPoint = resourceDesc.BindPoint;
						sh->bufferArray.push_back(sBuffer);

						// add vars to shader
						for (u32 j = 0; j < bufferDesc.Variables; j++)
						{
							ID3D11ShaderReflectionVariable* var = reflectionBuffer->GetVariableByIndex(j);

							D3D11_SHADER_VARIABLE_DESC varDesc;
							ZeroMemory(&varDesc, sizeof(D3D11_SHADER_VARIABLE_DESC));
							var->GetDesc(&varDesc);

							D3D11_SHADER_TYPE_DESC typeDesc;
							ZeroMemory(&typeDesc, sizeof(D3D11_SHADER_TYPE_DESC));
							var->GetType()->GetDesc(&typeDesc);

							SShaderVariable* sv = new SShaderVariable();
							sv->name = varDesc.Name;
							sv->buffer = sh->bufferArray.size() - 1;
							sv->offset = varDesc.StartOffset;
							sv->size = varDesc.Size;
							sv->baseType = typeDesc.Type;
							sv->classType = typeDesc.Class;

							sh->variableArray.push_back(sv);
						}
					}

					break;
				}
				case D3D_SIT_TBUFFER:
				{
					// same as cbuffer?
					break;
				}
				case D3D_SIT_SAMPLER:
				{
					for (u32 j = resourceDesc.BindPoint; j < resourceDesc.BindPoint + resourceDesc.BindCount; ++j)
						sh->samplersUsed += 1 << j;
					//irr::core::stringc BindingTexture = "sampler bind count : ";
					//BindingTexture += resourceDesc.BindCount;
					//BindingTexture += " sampler Bind Point : ";
					//BindingTexture += resourceDesc.BindPoint;
					//BindingTexture += " On shader type : ";
					//BindingTexture += type;
					//BindingTexture += " Sampler Used ";
					//BindingTexture += sh->samplersUsed;
					//os::Printer::log(BindingTexture.c_str());
					break;
				}
				case D3D_SIT_TEXTURE:
				{
					for (u32 j = resourceDesc.BindPoint; j < resourceDesc.BindPoint + resourceDesc.BindCount; ++j)
						sh->texturesUsed |= 1 << j;
					//irr::core::stringc BindingTexture = "Texture bind count : ";
					//BindingTexture += resourceDesc.BindCount;
					//BindingTexture += " Texture Bind Point : ";
					//BindingTexture += resourceDesc.BindPoint;
					//BindingTexture += " On shader type : ";
					//BindingTexture += type;
					//BindingTexture += " Texture Used ";
					//BindingTexture += sh->texturesUsed;
					//os::Printer::log(BindingTexture.c_str());

					break;
				}
				default:
				{
					//irr::core::stringc BindingTexture = "Unknown bind count : ";
					//BindingTexture += resourceDesc.BindCount;
					//BindingTexture += " Unknown Bind Point : ";
					//BindingTexture += resourceDesc.BindPoint;
					//BindingTexture += " Unknown Bind Name : ";
					//BindingTexture += resourceDesc.Name;
					//BindingTexture += " On shader type : ";
					//BindingTexture += type;
					//os::Printer::log(BindingTexture.c_str());
				}
				break;
				}
			}

			pReflector->Release();
		}
	} // end namespace video
} // end namespace irr

#endif