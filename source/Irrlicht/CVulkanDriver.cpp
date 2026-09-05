// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanDriver.h"
#include "CVulkanDeferredContext.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CVulkanTexture.h"
#include "CVulkanHardwareBuffer.h"
#include "CVulkanShaderCompiler.h"
#include "IComputebuffer.h"
#include "CImage.h"
#include "IMeshBuffer.h"
#include "IVertexBuffer.h"
#include "IReadFile.h"
#include "IFileSystem.h"
#include "os.h"
#include <vector>
#include <string.h>
#include <math.h>

namespace irr
{
	namespace video
	{
		namespace
		{
			//! Instance extensions the driver cannot work without: a surface plus its Win32 flavour.
			const c8* const kRequiredInstanceExtensions[] =
			{
				VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef VK_USE_PLATFORM_WIN32_KHR
				VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
			};

			//! Device extensions. Dynamic rendering is required rather than optional -- see the
			//! header comment; the render-pass fallback is not implemented.
			const c8* const kRequiredDeviceExtensions[] =
			{
				VK_KHR_SWAPCHAIN_EXTENSION_NAME,
				VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
			};

			bool hasExtension(const std::vector<VkExtensionProperties>& available, const c8* name)
			{
				for (size_t i = 0; i < available.size(); ++i)
					if (strcmp(available[i].extensionName, name) == 0)
						return true;
				return false;
			}

#ifdef _IRR_VULKAN_DEBUG_LAYER_
			const c8* const kValidationLayerName = "VK_LAYER_KHRONOS_validation";

			bool hasLayer(const std::vector<VkLayerProperties>& available, const c8* name)
			{
				for (size_t i = 0; i < available.size(); ++i)
					if (strcmp(available[i].layerName, name) == 0)
						return true;
				return false;
			}

			//! Routes every message the layer or the loader emits into the engine log. Severity maps
			//! onto Irrlicht's levels; the message id name is prefixed so a VUID is greppable.
			VKAPI_ATTR VkBool32 VKAPI_CALL vulkanDebugCallback(
				VkDebugUtilsMessageSeverityFlagBitsEXT severity,
				VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
			{
				if (!data || !data->pMessage)
					return VK_FALSE;

				ELOG_LEVEL level = ELL_INFORMATION;
				if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
					level = ELL_ERROR;
				else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
					level = ELL_WARNING;

				core::stringc text("Vulkan: ");
				if (data->pMessageIdName)
				{
					text += data->pMessageIdName;
					text += ": ";
				}
				text += data->pMessage;
				os::Printer::log(text.c_str(), level);
				return VK_FALSE; // never abort the offending call
			}

			//! Shared by the pNext of vkCreateInstance (so instance creation itself is covered) and
			//! by the standalone messenger created right after.
			void fillDebugMessengerInfo(VkDebugUtilsMessengerCreateInfoEXT& info)
			{
				info = {};
				info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
				info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
					VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
				info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
					VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
					VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
				info.pfnUserCallback = vulkanDebugCallback;
			}
#endif

			//! Size of each frame's uniform ring. Per-draw World/View/Proj/lighting/fog blocks only,
			//! never mesh data; one draw costs about a kilobyte, so this covers ~4000 of them.
			const VkDeviceSize kUniformRingSize = 4 * 1024 * 1024;

			//! Descriptor sets a frame may hand out. Two per draw (set 0 textures, set 4 uniforms),
			//! so this is the same draw budget as the ring above.
			const u32 kDescriptorSetsPerFrame = 8192;

			//! Vertex streams one drawMeshBuffer() will bind; the device limit is checked too.
			const u32 kMaxVertexStreams = 16;

			// ---- Uniform block mirrors. Byte-for-byte images of the std140 blocks in
			// vulkan/shaders/common.glsl and ps_common.glsl, which were themselves written against
			// the D3D12 constant buffers: nothing is reflected here, so a change on one side has to
			// be mirrored on the other by hand.

			//! set 4 binding 1. View/Proj transposed on upload (see bindDriverUniforms()).
			struct SVulkanPerFrame
			{
				core::matrix4 View;
				core::matrix4 Proj;
				f32 CameraPosWorld[4];
			};

			//! set 4 binding 2. A disabled plane is (0,0,0,1): dot(p,1) == 1 > 0, never clipped.
			struct SVulkanClipPlanes
			{
				f32 Planes[3][4];
			};

			//! SLightGPU, 5 * vec4 = 80 bytes.
			struct SVulkanShaderLight
			{
				f32 Position[4];	//!< xyz world position, w unused
				f32 Diffuse[4];
				f32 Specular[4];
				f32 Ambient[4];
				f32 Atten[4];		//!< constant, linear, quadratic, unused
			};

			//! SLightMaterialGPU, 4 * vec4 = 64 bytes.
			struct SVulkanShaderLightMaterial
			{
				f32 Ambient[4];
				f32 Diffuse[4];
				f32 Specular[4];
				f32 Emissive[4];
			};

			//! set 4 binding 3, LightingCB: 640 + 64 + 16 = 720 bytes.
			struct SVulkanLightingConstants
			{
				SVulkanShaderLight Lights[8];
				SVulkanShaderLightMaterial Material;
				s32 LightCount;
				s32 EnableLighting;
				s32 ColorMaterialMode;
				s32 NormalizeNormalsFlag;
			};

			//! set 4 binding 4, FogCB: vec4 + 4 words + 2 words, padded to 48 bytes.
			struct SVulkanFogConstants
			{
				f32 Color[4];
				s32 Mode;
				f32 Start;
				f32 End;
				f32 Density;
				s32 EnableFog;
				f32 ParallaxHeightScale;
				f32 Pad[2];
			};

			void writeShaderColor(f32* dst, const video::SColorf& c)
			{
				dst[0] = c.r; dst[1] = c.g; dst[2] = c.b; dst[3] = c.a;
			}

			//! Strict parity with the D3D11/D3D12 mapping, fans included: D3D10+ has no fan either,
			//! and both of those drivers render one as a strip rather than dropping the mesh.
			VkPrimitiveTopology mapPrimitiveType(scene::E_PRIMITIVE_TYPE primitiveType)
			{
				switch (primitiveType)
				{
				case scene::EPT_POINT_SPRITES:
				case scene::EPT_POINTS:			return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
				case scene::EPT_LINE_STRIP:		return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
				case scene::EPT_LINE_LOOP:
				case scene::EPT_LINES:			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
				case scene::EPT_TRIANGLE_STRIP:	return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
				case scene::EPT_TRIANGLES:		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				case scene::EPT_TRIANGLE_FAN:	return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
				default:						return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
				}
			}

			//! `forAlphaChannel` picks the alpha counterpart of the *_COLOR factors, which Vulkan
			//! refuses on the alpha slot - same substitution the D3D12 driver makes.
			VkBlendFactor getVulkanBlendFactor(E_BLEND_FACTOR factor, bool forAlphaChannel)
			{
				switch (factor)
				{
				case EBF_ZERO: return VK_BLEND_FACTOR_ZERO;
				case EBF_ONE: return VK_BLEND_FACTOR_ONE;
				case EBF_DST_COLOR: return forAlphaChannel ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
				case EBF_ONE_MINUS_DST_COLOR: return forAlphaChannel ?
					VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
				case EBF_SRC_COLOR: return forAlphaChannel ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
				case EBF_ONE_MINUS_SRC_COLOR: return forAlphaChannel ?
					VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
				case EBF_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
				case EBF_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
				case EBF_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
				case EBF_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
				case EBF_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
				// Dual-source (dualSrcBlend feature): the fragment shader's Index 1 output, i.e. the
				// HLSL SV_Target1 with [[vk::location(0), vk::index(1)]]. Attachment 0 only.
				case EBF_SRC1_COLOR: return forAlphaChannel ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC1_COLOR;
				case EBF_ONE_MINUS_SRC1_COLOR: return forAlphaChannel ?
					VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
				case EBF_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
				case EBF_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
				default: return VK_BLEND_FACTOR_ONE;
				}
			}

			//! E_LOGIC_OP -> VkLogicOp; the two enumerations are ordered differently.
			VkLogicOp getVulkanLogicOp(u32 op)
			{
				switch (op)
				{
				case ELO_CLEAR: return VK_LOGIC_OP_CLEAR;
				case ELO_SET: return VK_LOGIC_OP_SET;
				case ELO_COPY: return VK_LOGIC_OP_COPY;
				case ELO_COPY_INVERTED: return VK_LOGIC_OP_COPY_INVERTED;
				case ELO_NOOP: return VK_LOGIC_OP_NO_OP;
				case ELO_INVERT: return VK_LOGIC_OP_INVERT;
				case ELO_AND: return VK_LOGIC_OP_AND;
				case ELO_NAND: return VK_LOGIC_OP_NAND;
				case ELO_OR: return VK_LOGIC_OP_OR;
				case ELO_NOR: return VK_LOGIC_OP_NOR;
				case ELO_XOR: return VK_LOGIC_OP_XOR;
				case ELO_EQUIV: return VK_LOGIC_OP_EQUIVALENT;
				case ELO_AND_REVERSE: return VK_LOGIC_OP_AND_REVERSE;
				case ELO_AND_INVERTED: return VK_LOGIC_OP_AND_INVERTED;
				case ELO_OR_REVERSE: return VK_LOGIC_OP_OR_REVERSE;
				case ELO_OR_INVERTED: return VK_LOGIC_OP_OR_INVERTED;
				default: return VK_LOGIC_OP_NO_OP;
				}
			}

			//! ECFN_DISABLED maps to ALWAYS; the caller also clears DepthTestEnable for it.
			VkCompareOp getVulkanCompareOp(E_COMPARISON_FUNC func)
			{
				switch (func)
				{
				case ECFN_LESSEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
				case ECFN_EQUAL: return VK_COMPARE_OP_EQUAL;
				case ECFN_LESS: return VK_COMPARE_OP_LESS;
				case ECFN_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
				case ECFN_GREATEREQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
				case ECFN_GREATER: return VK_COMPARE_OP_GREATER;
				case ECFN_NEVER: return VK_COMPARE_OP_NEVER;
				case ECFN_ALWAYS:
				case ECFN_DISABLED:
				default: return VK_COMPARE_OP_ALWAYS;
				}
			}

			//! EBO_MIN_FACTOR..EBO_MAX_ALPHA have no Vulkan equivalent and are approximated as
			//! MIN/MAX, exactly as the D3D12 driver approximates them.
			VkBlendOp getVulkanBlendOp(E_BLEND_OPERATION op)
			{
				static const VkBlendOp map[] =
				{
					VK_BLEND_OP_ADD,			// EBO_NONE, never reached
					VK_BLEND_OP_ADD,			// EBO_ADD
					VK_BLEND_OP_SUBTRACT,		// EBO_SUBTRACT
					VK_BLEND_OP_REVERSE_SUBTRACT,	// EBO_REVSUBTRACT
					VK_BLEND_OP_MIN,			// EBO_MIN
					VK_BLEND_OP_MAX,			// EBO_MAX
					VK_BLEND_OP_MIN,			// EBO_MIN_FACTOR
					VK_BLEND_OP_MAX,			// EBO_MAX_FACTOR
					VK_BLEND_OP_MIN,			// EBO_MIN_ALPHA
					VK_BLEND_OP_MAX,			// EBO_MAX_ALPHA
				};
				const s32 index = static_cast<s32>(op);
				return (index >= 0 && index < static_cast<s32>(sizeof(map) / sizeof(map[0]))) ?
					map[index] : VK_BLEND_OP_ADD;
			}

			//! The combined formats are the only ones a stencil pass can run against.
			bool depthFormatHasStencil(VkFormat format)
			{
				return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
					format == VK_FORMAT_D16_UNORM_S8_UINT;
			}

			//! The aspect mask a barrier or a view over a depth image has to name.
			VkImageAspectFlags depthAspectOf(VkFormat format)
			{
				return depthFormatHasStencil(format) ?
					(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
			}
		}

		CVulkanDriver::CVulkanDriver(const irr::SIrrlichtCreationParameters& params,
			io::IFileSystem* io, HWND window)
			: CNullDriver(io, params.WindowSize), Name(L"Vulkan"), Params(params), Window(window)
		{
#ifdef _DEBUG
			setDebugName("CVulkanDriver");
#endif
			ViewPort = core::rect<s32>(0, 0, params.WindowSize.Width, params.WindowSize.Height);
			CurrentRenderTargetSize = params.WindowSize;
			ResourceOwner = this;
			for (u32 i = 0; i < ETS_COUNT; ++i)
				Matrices[i].makeIdentity();
			// EVT_STANDARD, the layout every 2D/immediate draw uses; never changes afterwards.
			getS3DVertexInputState(S3DVertexInput);
		}

		CVulkanDriver::~CVulkanDriver()
		{
			if (Context.Device)
				vk::DeviceWaitIdle(Context.Device);

			// The tile pools the tiled textures still map into: freed while the device is alive.
			releaseTiledRecords();

			// A deferred context shares the device, the loader and the compiler state with the
			// immediate driver: it tears down what it created (frames, caches, its own pools) and
			// nothing of the owner's.
			if (!OwnsDevice)
			{
				NullTexture = nullptr; // borrowed from the owner, see CVulkanDeferredContext
				deleteAllTextures();   // empty by construction, kept for the CNullDriver invariant
				deleteMaterialRenders();
				NativeRenderers.clear();
				UserRenderers.clear();
				UserLayouts.clear();
				ComputeRenderers.clear();
				delete RenderTarget;
				RenderTarget = nullptr;
				delete DepthPool;
				DepthPool = nullptr;
				PipelineCache.clear();
				LayoutCache.clear();
				Samplers.clear(Context);
				destroyFrameContexts();
				if (UploadPool)
					vk::DestroyCommandPool(Context.Device, UploadPool, nullptr);
				UploadPool = VK_NULL_HANDLE;
				Context.Device = VK_NULL_HANDLE; // nothing below may touch the owner's device
				return;
			}

			// The texture cache belongs to CNullDriver, whose destructor runs after this body -- by
			// which point vkDestroyDevice below has already been called and every CVulkanTexture
			// would free its image against a dead device. Emptied here instead, and so is the
			// material registry: a CVulkanUserMaterial owns its VkShaderModules.
			deleteAllTextures();
			deleteMaterialRenders();
			NativeRenderers.clear();
			UserRenderers.clear();
			UserLayouts.clear();
			ComputeRenderers.clear();
			// After the compute materials (they own the pipelines built against Compute's cache) and
			// before the device. The query pool likewise.
			delete Compute;
			Compute = nullptr;
			delete Occlusion;
			Occlusion = nullptr;
			// Releases glslang's per-process pools and the DXC library handle; a later driver
			// re-acquires both on its first compile.
			CVulkanShaderCompiler::shutdown();

			// Order matters: pipelines before the layouts they embed, layouts before the modules.
			delete RenderTarget;
			RenderTarget = nullptr;
			delete DepthPool;
			DepthPool = nullptr;
			if (NullTexture)
				NullTexture->drop();
			NullTexture = nullptr;

			PipelineCache.clear();
			LayoutCache.clear();
			ShaderModules.clear();
			Samplers.clear(Context);
			destroyQueryResources();
			destroyFrameContexts();
			destroySwapchain();

			if (UploadPool)
				vk::DestroyCommandPool(Context.Device, UploadPool, nullptr);
			if (Context.Device)
				vk::DestroyDevice(Context.Device, nullptr);
			if (Surface)
				vk::DestroySurfaceKHR(Instance, Surface, nullptr);
			// After every other object: it is the only thing that can still report on their
			// destruction.
			if (DebugMessenger && vk::DestroyDebugUtilsMessengerEXT)
				vk::DestroyDebugUtilsMessengerEXT(Instance, DebugMessenger, nullptr);
			if (Instance)
				vk::DestroyInstance(Instance, nullptr);

			unloadVulkanLibrary();
		}

		bool CVulkanDriver::initDriver(HWND hwnd)
		{
			if (!loadVulkanLibrary())
				return false;
			if (!createInstance())
				return false;
			if (!loadInstanceFunctions(Instance))
				return false;
			createDebugMessenger();
			if (!createSurface(hwnd))
				return false;
			if (!pickPhysicalDevice())
				return false;
			if (!createLogicalDevice())
				return false;

			bool dynamicRendering = false;
			if (!loadDeviceFunctions(Context.Device, dynamicRendering))
				return false;
			Context.HasDynamicRendering = dynamicRendering;
			// The extension was enabled AND its entry points resolved; either alone is useless.
			Context.HasTransformFeedback = TransformFeedbackEnabled &&
				vk::CmdBindTransformFeedbackBuffersEXT && vk::CmdBeginTransformFeedbackEXT &&
				vk::CmdEndTransformFeedbackEXT && vk::CmdDrawIndirectByteCountEXT && vk::CmdFillBuffer;
			if (!dynamicRendering)
			{
				os::Printer::log("CVulkanDriver: VK_KHR_dynamic_rendering unavailable, "
					"this driver requires it", ELL_ERROR);
				return false;
			}

			vk::GetDeviceQueue(Context.Device, Context.GraphicsQueueFamily, 0, &Context.GraphicsQueue);

			VkCommandPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			poolInfo.queueFamilyIndex = Context.GraphicsQueueFamily;
			if (vulkanFailed("CVulkanDriver: upload command pool",
				vk::CreateCommandPool(Context.Device, &poolInfo, nullptr, &UploadPool)))
				return false;

			// A stencil aspect only when asked for, as on the D3D drivers: the shadow volume passes
			// need it, everything else pays its bandwidth for nothing. D24S8 first (the D3D default),
			// D32S8 where the device lacks it; one of the two is guaranteed by the spec.
			if (Params.Stencilbuffer)
			{
				const VkFormat candidates[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
				bool found = false;
				for (u32 i = 0; i < 2 && !found; ++i)
				{
					VkFormatProperties properties = {};
					vk::GetPhysicalDeviceFormatProperties(Context.PhysicalDevice, candidates[i], &properties);
					if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
					{
						DepthFormat = candidates[i];
						found = true;
					}
				}
				if (!found)
					os::Printer::log("CVulkanDriver: no depth/stencil format is supported, stencil "
						"shadows will be unavailable", ELL_WARNING);
			}

			if (!createSwapchain(Params.WindowSize))
				return false;
			if (!createFrameContexts())
				return false;

			// Compute and occlusion queries are optional halves: a failure only disables the
			// corresponding IVideoDriver entry points (queryFeature() says so), as on D3D12.
			Compute = new CVulkanCompute(Context);
			if (!Compute->init())
				os::Printer::log("CVulkanDriver: compute unavailable, dispatchComputeShader() will do "
					"nothing", ELL_WARNING);
			Occlusion = new CVulkanOcclusionQuery(Context);
			if (!Occlusion->create())
				os::Printer::log("CVulkanDriver: occlusion queries unavailable", ELL_WARNING);
			else
				Occlusion->setPreciseCounts(HasPreciseOcclusionQuery);

			CurrentRenderTargetSize = core::dimension2d<u32>(SwapchainExtent.width, SwapchainExtent.height);
			ViewPort = core::rect<s32>(0, 0, (s32)SwapchainExtent.width, (s32)SwapchainExtent.height);

			// The D3D11.x additions, see doc/d3d11-feature-api.md. Vulkan applies a logic op to any
			// integer or normalized attachment, so UNORM qualifies wherever the feature exists.
			DriverAttributes->setAttribute("LogicOpOnUnorm", Context.HasLogicOp);
			DriverAttributes->setAttribute("TiledResourcesTier", Context.HasSparseResidency ?
				(Context.HasSparseResidencyStrict && Context.HasShaderResourceResidency ? 2 : 1) : 0);
			DriverAttributes->setAttribute("ConservativeRasterizationTier", Context.HasConservativeRaster ? 1 : 0);

			// Depth images for render targets created without one of their own; the swapchain keeps
			// the dedicated DepthImage above instead.
			DepthPool = new CVulkanDepthBufferPool(Context, DepthFormat);
			// One target object reused by every setRenderTarget(): it owns nothing, so rebinding it
			// is just re-describing attachments.
			RenderTarget = new CVulkanRenderTarget(Context);

			// The built-in vertex layouts every CMeshBuffer looks up by index. Without these
			// getVertexDescriptor(0) hands back null and the geometry creator dereferences it.
			createVertexDescriptors();

			if (!createNullTexture())
				return false;
			if (!createDescriptorLayouts())
				return false;
			if (!createBuiltInMaterialRenderers())
				return false;

			VendorInfo = "Vulkan (native) - ";
			VendorInfo += Context.DeviceProperties.deviceName;

			os::Printer::log("Vulkan device", core::stringc(Context.DeviceProperties.deviceName).c_str(), ELL_INFORMATION);
			return true;
		}

		bool CVulkanDriver::createNullTexture()
		{
			// Built from a real IImage so it goes through CVulkanTexture's upload path and ends in
			// SHADER_READ_ONLY_OPTIMAL; an "empty" texture would still be in UNDEFINED.
			CImage* image = new CImage(ECF_A8R8G8B8, core::dimension2d<u32>(1, 1));
			image->fill(SColor(255, 255, 255, 255));

			NullTexture = new CVulkanTexture(Context, *this, image, 0, "irr_vulkan_null");
			image->drop();

			if (!NullTexture->hasDeviceResource())
			{
				os::Printer::log("CVulkanDriver: could not create the default 1x1 texture", ELL_ERROR);
				NullTexture->drop();
				NullTexture = nullptr;
				return false;
			}
			return true;
		}

		// One pipeline layout for all 24 built-in materials: they declare the same two sets, so
		// nothing is gained by giving each its own, and a shared one keeps the pipeline key's
		// PipelineLayoutHash constant across every draw.
		bool CVulkanDriver::createDescriptorLayouts()
		{
			// Both binding tables are fixed and hand-written; see CVulkanMaterialRenderer.h for why
			// they are not reflected out of the SPIR-V.
			u32 textureBindingCount = 0;
			const VkDescriptorSetLayoutBinding* textureBindings =
				getVulkanMaterialTextureBindings(textureBindingCount);
			MaterialTextureSetLayout = LayoutCache.getOrCreateDescriptorSetLayout(Context,
				textureBindings, textureBindingCount);

			u32 uniformBindingCount = 0;
			const VkDescriptorSetLayoutBinding* uniformBindings =
				getVulkanDriverUniformBindings(uniformBindingCount);
			DriverUniformSetLayout = LayoutCache.getOrCreateDescriptorSetLayout(Context,
				uniformBindings, uniformBindingCount);

			// Sets 1..3 are declared but empty: a VkPipelineLayout is indexed by set number and
			// cannot skip one, and those three belong to user shaders this backend does not serve.
			EmptySetLayout = LayoutCache.getOrCreateDescriptorSetLayout(Context, nullptr, 0);

			if (!MaterialTextureSetLayout || !DriverUniformSetLayout || !EmptySetLayout)
				return false;

			VkDescriptorSetLayout setLayouts[DriverDescriptorSet + 1];
			setLayouts[VulkanMaterialTextureSet] = MaterialTextureSetLayout;
			for (u32 i = 1; i < DriverDescriptorSet; ++i)
				setLayouts[i] = EmptySetLayout;
			setLayouts[DriverDescriptorSet] = DriverUniformSetLayout;

			BuiltInPipelineLayout = LayoutCache.getOrCreatePipelineLayout(Context, setLayouts,
				DriverDescriptorSet + 1);
			return BuiltInPipelineLayout != VK_NULL_HANDLE;
		}

		bool CVulkanDriver::createBuiltInMaterialRenderers()
		{
			// The factory hands back one reference per renderer, in E_MATERIAL_TYPE order, with the
			// SPIR-V modules borrowed from ShaderModules -- which therefore has to outlive them.
			std::vector<CVulkanMaterialRenderer*> renderers;
			if (!createVulkanBuiltInMaterialRenderers(Context, ShaderModules, renderers))
				return false;

			// Registration order is E_MATERIAL_TYPE order, so the index a renderer lands at must be
			// its own type -- SMaterial::MaterialType is used directly as the registry index.
			for (size_t i = 0; i < renderers.size(); ++i)
			{
				const s32 index = addMaterialRenderer(renderers[i], nullptr);
				renderers[i]->drop(); // addMaterialRenderer() grabbed it

				if (index != static_cast<s32>(i))
				{
					os::Printer::log("CVulkanDriver: built-in material renderer index out of step "
						"with E_MATERIAL_TYPE", ELL_ERROR);
					// Whatever is left in the vector was never registered; drop it here.
					for (size_t r = i + 1; r < renderers.size(); ++r)
						renderers[r]->drop();
					return false;
				}
			}
			return true;
		}

		s32 CVulkanDriver::addMaterialRenderer(IMaterialRenderer* renderer, const c8* name)
		{
			const s32 index = CNullDriver::addMaterialRenderer(renderer, name);
			if (index < 0)
				return index;

			// A renderer this driver did not build leaves a null hole in both tables: still reachable
			// through getMaterialRenderer(), but bindDrawState() can get no shader modules out of it.
			NativeRenderers.resize(MaterialRenderers.size(), nullptr);
			UserRenderers.resize(MaterialRenderers.size(), nullptr);
			ComputeRenderers.resize(MaterialRenderers.size(), nullptr);
			UserLayouts.resize(MaterialRenderers.size());
			NativeRenderers[index] = dynamic_cast<CVulkanMaterialRenderer*>(renderer);
			UserRenderers[index] = dynamic_cast<CVulkanUserMaterial*>(renderer);
			ComputeRenderers[index] = dynamic_cast<CVulkanComputeMaterial*>(renderer);
			return index;
		}

		// The three lookups read the ResourceOwner's tables: on a deferred context they are the
		// immediate driver's, always up to date, including a material registered after the context
		// was created.
		CVulkanComputeMaterial* CVulkanDriver::getComputeMaterial(s32 index) const
		{
			const std::vector<CVulkanComputeMaterial*>& table = ResourceOwner->ComputeRenderers;
			if (index < 0 || static_cast<size_t>(index) >= table.size())
				return nullptr;
			return table[index];
		}

		CVulkanMaterialRenderer* CVulkanDriver::getNativeRenderer(s32 index) const
		{
			const std::vector<CVulkanMaterialRenderer*>& table = ResourceOwner->NativeRenderers;
			if (index < 0 || static_cast<size_t>(index) >= table.size())
				return nullptr;
			return table[index];
		}

		CVulkanUserMaterial* CVulkanDriver::getUserMaterial(s32 index) const
		{
			const std::vector<CVulkanUserMaterial*>& table = ResourceOwner->UserRenderers;
			if (index < 0 || static_cast<size_t>(index) >= table.size())
				return nullptr;
			return table[index];
		}

		bool CVulkanDriver::createInstance()
		{
			u32 count = 0;
			vk::EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
			std::vector<VkExtensionProperties> available(count);
			if (count)
				vk::EnumerateInstanceExtensionProperties(nullptr, &count, available.data());

			std::vector<const c8*> extensions;
			for (size_t i = 0; i < sizeof(kRequiredInstanceExtensions) / sizeof(kRequiredInstanceExtensions[0]); ++i)
			{
				if (!hasExtension(available, kRequiredInstanceExtensions[i]))
				{
					os::Printer::log("CVulkanDriver: missing instance extension",
						kRequiredInstanceExtensions[i], ELL_ERROR);
					return false;
				}
				extensions.push_back(kRequiredInstanceExtensions[i]);
			}
			// The wide-gamut / HDR surface colour spaces (SIrrlichtCreationParameters::ColorSpace)
			// only exist with this optional extension; without it createSwapchain() falls back.
			if ((Params.ColorSpace == ESCS_SCRGB_LINEAR || Params.ColorSpace == ESCS_HDR10_ST2084) &&
				hasExtension(available, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
				extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);

			std::vector<const c8*> layers;
#ifdef _IRR_VULKAN_DEBUG_LAYER_
			u32 layerCount = 0;
			vk::EnumerateInstanceLayerProperties(&layerCount, nullptr);
			std::vector<VkLayerProperties> availableLayers(layerCount);
			if (layerCount)
				vk::EnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

			if (hasLayer(availableLayers, kValidationLayerName))
				layers.push_back(kValidationLayerName);
			else
				os::Printer::log("CVulkanDriver: VK_LAYER_KHRONOS_validation not installed, "
					"running without validation", ELL_INFORMATION);

			// The loader implements debug_utils even when no layer is present, so it is worth
			// asking for either way.
			DebugUtilsEnabled = hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			if (DebugUtilsEnabled)
				extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

			VkApplicationInfo appInfo = {};
			appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
			appInfo.pApplicationName = "Irrlicht";
			appInfo.pEngineName = "Irrlicht Engine";
			// 1.2 rather than 1.3: dynamic rendering is taken as an extension so the driver also
			// runs on 1.2 implementations that expose it.
			appInfo.apiVersion = VK_API_VERSION_1_2;

			VkInstanceCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			info.pApplicationInfo = &appInfo;
			info.enabledExtensionCount = static_cast<u32>(extensions.size());
			info.ppEnabledExtensionNames = extensions.data();
			info.enabledLayerCount = static_cast<u32>(layers.size());
			info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

#ifdef _IRR_VULKAN_DEBUG_LAYER_
			// Chained here as well as created standalone below: a messenger made after the fact
			// cannot report anything vkCreateInstance itself complained about.
			VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};
			if (DebugUtilsEnabled)
			{
				fillDebugMessengerInfo(messengerInfo);
				info.pNext = &messengerInfo;
			}
#endif

			return !vulkanFailed("CVulkanDriver: vkCreateInstance",
				vk::CreateInstance(&info, nullptr, &Instance));
		}

		void CVulkanDriver::createDebugMessenger()
		{
#ifdef _IRR_VULKAN_DEBUG_LAYER_
			if (!DebugUtilsEnabled || !vk::CreateDebugUtilsMessengerEXT)
				return;

			VkDebugUtilsMessengerCreateInfoEXT info = {};
			fillDebugMessengerInfo(info);
			// Not fatal: losing the messenger costs diagnostics, not correctness.
			vulkanFailed("CVulkanDriver: vkCreateDebugUtilsMessengerEXT",
				vk::CreateDebugUtilsMessengerEXT(Instance, &info, nullptr, &DebugMessenger));
#endif
		}

		bool CVulkanDriver::createSurface(HWND hwnd)
		{
#ifdef VK_USE_PLATFORM_WIN32_KHR
			VkWin32SurfaceCreateInfoKHR info = {};
			info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
			info.hinstance = (HINSTANCE)GetModuleHandle(nullptr);
			info.hwnd = hwnd;
			return !vulkanFailed("CVulkanDriver: vkCreateWin32SurfaceKHR",
				vk::CreateWin32SurfaceKHR(Instance, &info, nullptr, &Surface));
#else
			os::Printer::log("CVulkanDriver: no surface backend for this platform", ELL_ERROR);
			return false;
#endif
		}

		bool CVulkanDriver::pickPhysicalDevice()
		{
			u32 count = 0;
			vk::EnumeratePhysicalDevices(Instance, &count, nullptr);
			if (!count)
			{
				os::Printer::log("CVulkanDriver: no Vulkan physical device", ELL_ERROR);
				return false;
			}
			std::vector<VkPhysicalDevice> devices(count);
			vk::EnumeratePhysicalDevices(Instance, &count, devices.data());

			VkPhysicalDevice fallback = VK_NULL_HANDLE;
			u32 fallbackFamily = 0;

			for (size_t i = 0; i < devices.size(); ++i)
			{
				u32 extCount = 0;
				vk::EnumerateDeviceExtensionProperties(devices[i], nullptr, &extCount, nullptr);
				std::vector<VkExtensionProperties> available(extCount);
				if (extCount)
					vk::EnumerateDeviceExtensionProperties(devices[i], nullptr, &extCount, available.data());

				bool ok = true;
				for (size_t e = 0; e < sizeof(kRequiredDeviceExtensions) / sizeof(kRequiredDeviceExtensions[0]); ++e)
					ok = ok && hasExtension(available, kRequiredDeviceExtensions[e]);
				if (!ok)
					continue;

				u32 familyCount = 0;
				vk::GetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, nullptr);
				std::vector<VkQueueFamilyProperties> families(familyCount);
				vk::GetPhysicalDeviceQueueFamilyProperties(devices[i], &familyCount, families.data());

				for (u32 f = 0; f < familyCount; ++f)
				{
					if (!(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT))
						continue;
					VkBool32 present = VK_FALSE;
					vk::GetPhysicalDeviceSurfaceSupportKHR(devices[i], f, Surface, &present);
					if (!present)
						continue;

					VkPhysicalDeviceProperties props = {};
					vk::GetPhysicalDeviceProperties(devices[i], &props);
					// Prefer a discrete GPU, but keep the first workable device so an integrated
					// or software implementation is still used rather than failing outright.
					if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
					{
						Context.PhysicalDevice = devices[i];
						Context.GraphicsQueueFamily = f;
						Context.DeviceProperties = props;
						vk::GetPhysicalDeviceMemoryProperties(devices[i], &Context.MemoryProperties);
						return true;
					}
					if (fallback == VK_NULL_HANDLE)
					{
						fallback = devices[i];
						fallbackFamily = f;
					}
					break;
				}
			}

			if (fallback == VK_NULL_HANDLE)
			{
				os::Printer::log("CVulkanDriver: no device with graphics+present and the "
					"required extensions", ELL_ERROR);
				return false;
			}

			Context.PhysicalDevice = fallback;
			Context.GraphicsQueueFamily = fallbackFamily;
			vk::GetPhysicalDeviceProperties(fallback, &Context.DeviceProperties);
			vk::GetPhysicalDeviceMemoryProperties(fallback, &Context.MemoryProperties);
			return true;
		}

		bool CVulkanDriver::createLogicalDevice()
		{
			const float priority = 1.0f;
			VkDeviceQueueCreateInfo queueInfo = {};
			queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueInfo.queueFamilyIndex = Context.GraphicsQueueFamily;
			queueInfo.queueCount = 1;
			queueInfo.pQueuePriorities = &priority;

			VkPhysicalDeviceFeatures features = {};
			VkPhysicalDeviceFeatures supported = {};
			vk::GetPhysicalDeviceFeatures(Context.PhysicalDevice, &supported);
			features.samplerAnisotropy = supported.samplerAnisotropy;
			features.fillModeNonSolid = supported.fillModeNonSolid;   // SMaterial::Wireframe
			// Block-compressed textures (.dds DXT1-5 / BC6 / BC7): optional in Vulkan, and without it
			// sampling a BC image is undefined. Every desktop driver offers it.
			features.textureCompressionBC = supported.textureCompressionBC;
			if (!supported.textureCompressionBC)
				os::Printer::log("CVulkanDriver: the device has no BC texture compression, compressed "
					".dds textures will not load", ELL_WARNING);
			features.depthBiasClamp = supported.depthBiasClamp;
			// Both are user-shader territory: no built-in material declares either stage.
			features.geometryShader = supported.geometryShader;
			features.tessellationShader = supported.tessellationShader;
			HasGeometryShader = supported.geometryShader == VK_TRUE;
			// Exact sample counts for getOcclusionQueryResult(), which callers scale intensities by.
			features.occlusionQueryPrecise = supported.occlusionQueryPrecise;
			HasPreciseOcclusionQuery = supported.occlusionQueryPrecise == VK_TRUE;
			// Indirect draws/dispatches read their arguments from a buffer a compute shader wrote.
			features.drawIndirectFirstInstance = supported.drawIndirectFirstInstance;
			// D3D bounds-checks every buffer access: an append past capacity is dropped while the
			// counter still advances, which callers use to detect overflow. robustBufferAccess is
			// the Vulkan feature that gives the same guarantee (core, always supported).
			features.robustBufferAccess = supported.robustBufferAccess;
			// Per-attachment blend state on an MRT draw, what the IRenderTarget blend fields of
			// setRenderTarget(array) describe. Every desktop device has it; without it the pipeline
			// cache keeps one state for every attachment and the overrides are ignored.
			features.independentBlend = supported.independentBlend;
			Context.HasIndependentBlend = supported.independentBlend == VK_TRUE;
			// The D3D11.x material state (doc/d3d11-feature-api.md): every one optional per the spec,
			// present on every desktop device, reported through queryFeature() when absent.
			features.logicOp = supported.logicOp;
			Context.HasLogicOp = supported.logicOp == VK_TRUE;
			features.dualSrcBlend = supported.dualSrcBlend;
			Context.HasDualSrcBlend = supported.dualSrcBlend == VK_TRUE;
			features.multiViewport = supported.multiViewport;
			Context.HasMultiViewport = supported.multiViewport == VK_TRUE;
			features.pipelineStatisticsQuery = supported.pipelineStatisticsQuery;
			Context.HasPipelineStatistics = supported.pipelineStatisticsQuery == VK_TRUE;
			// Pixel-stage UAVs: stores and atomics from the fragment stage, and storage-image writes
			// without a declared format (HLSL RWTexture2D<float4> without [[vk::image_format]]).
			features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
			features.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
			Context.HasFragmentStores = supported.fragmentStoresAndAtomics == VK_TRUE;
			// Tiled textures: sparse binding plus 2D residency, and the two flags that make a
			// non-resident read defined (D3D tier 2) and visible to the shader.
			features.sparseBinding = supported.sparseBinding;
			features.sparseResidencyImage2D = supported.sparseResidencyImage2D;
			features.shaderResourceResidency = supported.shaderResourceResidency;
			Context.HasSparseResidency = supported.sparseBinding == VK_TRUE && supported.sparseResidencyImage2D == VK_TRUE;
			Context.HasShaderResourceResidency = supported.shaderResourceResidency == VK_TRUE;
			// vkQueueBindSparse needs a queue family with the sparse-binding bit; the tile mappings go
			// through the graphics queue, so that family has to carry it.
			if (Context.HasSparseResidency)
			{
				u32 familyCount = 0;
				vk::GetPhysicalDeviceQueueFamilyProperties(Context.PhysicalDevice, &familyCount, nullptr);
				std::vector<VkQueueFamilyProperties> families(familyCount);
				if (familyCount)
					vk::GetPhysicalDeviceQueueFamilyProperties(Context.PhysicalDevice, &familyCount, families.data());
				Context.HasSparseResidency = Context.GraphicsQueueFamily < familyCount &&
					(families[Context.GraphicsQueueFamily].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;
			}
			Context.HasSparseResidencyStrict = Context.DeviceProperties.sparseProperties.residencyNonResidentStrict == VK_TRUE;

			// Must be enabled explicitly even when the extension is present, or CmdBeginRendering
			// is undefined behaviour.
			VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering = {};
			dynamicRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
			dynamicRendering.dynamicRendering = VK_TRUE;

			// The optional extensions on top of the required list.
			const u32 requiredCount = sizeof(kRequiredDeviceExtensions) / sizeof(kRequiredDeviceExtensions[0]);
			std::vector<const c8*> extensions(kRequiredDeviceExtensions, kRequiredDeviceExtensions + requiredCount);
			u32 availableCount = 0;
			vk::EnumerateDeviceExtensionProperties(Context.PhysicalDevice, nullptr, &availableCount, nullptr);
			std::vector<VkExtensionProperties> available(availableCount);
			if (availableCount)
				vk::EnumerateDeviceExtensionProperties(Context.PhysicalDevice, nullptr, &availableCount, available.data());

			// VK_EXT_transform_feedback carries the stream-output path (setStreamOutputBuffer() and the
			// vertexTypeOut of a geometry material). Asked for when the device offers both the
			// extension and its feature; desktop drivers do, a mobile one may not.
			VkPhysicalDeviceTransformFeedbackFeaturesEXT transformFeedback = {};
			transformFeedback.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
			TransformFeedbackEnabled = false;
			if (hasExtension(available, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME) && vk::GetPhysicalDeviceFeatures2)
			{
				VkPhysicalDeviceTransformFeedbackFeaturesEXT query = {};
				query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
				VkPhysicalDeviceFeatures2 features2 = {};
				features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
				features2.pNext = &query;
				vk::GetPhysicalDeviceFeatures2(Context.PhysicalDevice, &features2);
				if (query.transformFeedback)
				{
					transformFeedback.transformFeedback = VK_TRUE;
					extensions.push_back(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
					TransformFeedbackEnabled = true;
				}
			}
			if (TransformFeedbackEnabled)
				dynamicRendering.pNext = &transformFeedback;
			else
				os::Printer::log("CVulkanDriver: VK_EXT_transform_feedback unavailable, stream output "
					"(setStreamOutputBuffer) is disabled on this device", ELL_INFORMATION);

			// The pNext chain grows from here; `tail` is the last structure appended.
			void** tail = TransformFeedbackEnabled ? &transformFeedback.pNext : &dynamicRendering.pNext;
			auto chain = [&tail](void* next, void** nextsPNext)
			{
				*tail = next;
				tail = nextsPNext;
			};

			// Extension-only features: present means usable.
			if (hasExtension(available, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME))
			{
				extensions.push_back(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
				Context.HasConservativeRaster = true;
			}
			if (hasExtension(available, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME))
			{
				extensions.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
				Context.HasStencilExport = true;
			}

			// Extensions with a feature structure that has to be queried and then enabled.
			VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock = {};
			interlock.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT;
			VkPhysicalDeviceConditionalRenderingFeaturesEXT conditional = {};
			conditional.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT;
			VkPhysicalDeviceSamplerFilterMinmaxProperties minmaxProperties = {};
			minmaxProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES;
			if (vk::GetPhysicalDeviceFeatures2)
			{
				VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlockQuery = {};
				interlockQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT;
				VkPhysicalDeviceConditionalRenderingFeaturesEXT conditionalQuery = {};
				conditionalQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT;
				interlockQuery.pNext = &conditionalQuery;
				VkPhysicalDeviceFeatures2 features2 = {};
				features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
				features2.pNext = &interlockQuery;
				vk::GetPhysicalDeviceFeatures2(Context.PhysicalDevice, &features2);

				// ROVs: pixel interlock is what RasterizerOrdered* compile to.
				if (hasExtension(available, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) &&
					interlockQuery.fragmentShaderPixelInterlock)
				{
					interlock.fragmentShaderPixelInterlock = VK_TRUE;
					extensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
					chain(&interlock, &interlock.pNext);
					Context.HasFragmentShaderInterlock = true;
				}
				// Predication.
				if (hasExtension(available, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME) &&
					conditionalQuery.conditionalRendering)
				{
					conditional.conditionalRendering = VK_TRUE;
					extensions.push_back(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
					chain(&conditional, &conditional.pNext);
					Context.HasConditionalRendering = true;
				}
			}
			// Min/max sampler reduction: a 1.2 property (the extension promoted into core); on a
			// 1.1 device the EXT extension carries the same structure.
			if (vk::GetPhysicalDeviceProperties2)
			{
				VkPhysicalDeviceProperties2 properties2 = {};
				properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
				properties2.pNext = &minmaxProperties;
				vk::GetPhysicalDeviceProperties2(Context.PhysicalDevice, &properties2);
				const bool core12 = Context.DeviceProperties.apiVersion >= VK_API_VERSION_1_2;
				const bool ext = hasExtension(available, VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
				if (minmaxProperties.filterMinmaxSingleComponentFormats && (core12 || ext))
				{
					if (!core12)
						extensions.push_back(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
					Context.HasSamplerFilterMinmax = true;
				}
			}

			VkDeviceCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			info.pNext = &dynamicRendering;
			info.queueCreateInfoCount = 1;
			info.pQueueCreateInfos = &queueInfo;
			info.pEnabledFeatures = &features;
			info.enabledExtensionCount = static_cast<u32>(extensions.size());
			info.ppEnabledExtensionNames = extensions.data();

			return !vulkanFailed("CVulkanDriver: vkCreateDevice",
				vk::CreateDevice(Context.PhysicalDevice, &info, nullptr, &Context.Device));
		}

		bool CVulkanDriver::createSwapchain(const core::dimension2d<u32>& size)
		{
			VkSurfaceCapabilitiesKHR caps = {};
			if (vulkanFailed("CVulkanDriver: surface capabilities",
				vk::GetPhysicalDeviceSurfaceCapabilitiesKHR(Context.PhysicalDevice, Surface, &caps)))
				return false;

			u32 formatCount = 0;
			vk::GetPhysicalDeviceSurfaceFormatsKHR(Context.PhysicalDevice, Surface, &formatCount, nullptr);
			if (!formatCount)
			{
				os::Printer::log("CVulkanDriver: surface exposes no format", ELL_ERROR);
				return false;
			}
			std::vector<VkSurfaceFormatKHR> formats(formatCount);
			vk::GetPhysicalDeviceSurfaceFormatsKHR(Context.PhysicalDevice, Surface, &formatCount, formats.data());

			// B8G8R8A8_UNORM matches Irrlicht's A8R8G8B8 byte order, so 2D blits need no swizzle. The
			// requested colour space (SIrrlichtCreationParameters::ColorSpace) asks for another surface
			// format first; one the surface lacks falls back to that default, warned once.
			auto findSurfaceFormat = [&](VkFormat format, VkColorSpaceKHR colorSpace, VkSurfaceFormatKHR& out) -> bool
			{
				for (size_t i = 0; i < formats.size(); ++i)
					if (formats[i].format == format && formats[i].colorSpace == colorSpace)
					{
						out = formats[i];
						return true;
					}
				return false;
			};
			VkFormat wantedFormat = VK_FORMAT_B8G8R8A8_UNORM;
			VkColorSpaceKHR wantedSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
			switch (Params.ColorSpace)
			{
			case ESCS_SRGB_LINEAR:  wantedFormat = VK_FORMAT_B8G8R8A8_SRGB; break;
			case ESCS_SCRGB_LINEAR: wantedFormat = VK_FORMAT_R16G16B16A16_SFLOAT; wantedSpace = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT; break;
			case ESCS_HDR10_ST2084: wantedFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32; wantedSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT; break;
			default: break;
			}
			VkSurfaceFormatKHR chosen = formats[0];
			SwapchainColorSpace = ESCS_SRGB_NONLINEAR;
			if (Params.ColorSpace != ESCS_SRGB_NONLINEAR && findSurfaceFormat(wantedFormat, wantedSpace, chosen))
				SwapchainColorSpace = Params.ColorSpace;
			else
			{
				if (Params.ColorSpace != ESCS_SRGB_NONLINEAR && !WarnedColorSpace)
				{
					os::Printer::log("CVulkanDriver: the surface offers no format for the requested colour space; "
						"the 8-bit sRGB default is used (ESCS_SRGB_NONLINEAR)", ELL_WARNING);
					WarnedColorSpace = true;
				}
				if (!findSurfaceFormat(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, chosen))
					chosen = formats[0];
			}
			DriverAttributes->setAttribute("SwapchainColorSpace", (s32)SwapchainColorSpace);

			SwapchainExtent.width = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent.width : size.Width;
			SwapchainExtent.height = caps.currentExtent.height != 0xFFFFFFFFu ? caps.currentExtent.height : size.Height;
			if (!SwapchainExtent.width || !SwapchainExtent.height)
			{
				// A minimised window reports a 0x0 surface, and a swapchain cannot be that size.
				os::Printer::log("CVulkanDriver: the window surface has no area (minimised?), "
					"no swapchain can be created", ELL_ERROR);
				return false;
			}

			u32 imageCount = caps.minImageCount + 1;
			if (caps.maxImageCount && imageCount > caps.maxImageCount)
				imageCount = caps.maxImageCount;

			VkSwapchainCreateInfoKHR info = {};
			info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
			info.surface = Surface;
			info.minImageCount = imageCount;
			info.imageFormat = chosen.format;
			info.imageColorSpace = chosen.colorSpace;
			info.imageExtent = SwapchainExtent;
			info.imageArrayLayers = 1;
			// TRANSFER_SRC is what createScreenShot() copies out of; it is optional per the spec, so
			// it is only asked for when the surface actually offers it.
			info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
				info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			info.imageUsage &= caps.supportedUsageFlags;
			SwapchainCanReadBack = (info.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
			info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.preTransform = caps.currentTransform;
			info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			// FIFO is the only mode guaranteed present, and matches vsync-on behaviour.
			info.presentMode = Params.Vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
			info.clipped = VK_TRUE;

			if (vulkanFailed("CVulkanDriver: vkCreateSwapchainKHR",
				vk::CreateSwapchainKHR(Context.Device, &info, nullptr, &Swapchain)))
			{
				// IMMEDIATE is not universally supported; retry with the guaranteed mode.
				info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
				if (vulkanFailed("CVulkanDriver: vkCreateSwapchainKHR (FIFO retry)",
					vk::CreateSwapchainKHR(Context.Device, &info, nullptr, &Swapchain)))
					return false;
			}

			SwapchainFormat = chosen.format;

			u32 count = 0;
			vk::GetSwapchainImagesKHR(Context.Device, Swapchain, &count, nullptr);
			SwapchainImages.resize(count);
			vk::GetSwapchainImagesKHR(Context.Device, Swapchain, &count, SwapchainImages.data());

			SwapchainImageViews.resize(count);
			for (u32 i = 0; i < count; ++i)
			{
				VkImageViewCreateInfo viewInfo = {};
				viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
				viewInfo.image = SwapchainImages[i];
				viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
				viewInfo.format = SwapchainFormat;
				viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				viewInfo.subresourceRange.levelCount = 1;
				viewInfo.subresourceRange.layerCount = 1;
				if (vulkanFailed("CVulkanDriver: swapchain image view",
					vk::CreateImageView(Context.Device, &viewInfo, nullptr, &SwapchainImageViews[i])))
					return false;
			}

			// Depth buffer, sized with the swapchain and recreated with it.
			VkImageCreateInfo depthInfo = {};
			depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			depthInfo.imageType = VK_IMAGE_TYPE_2D;
			depthInfo.format = DepthFormat;
			depthInfo.extent.width = SwapchainExtent.width;
			depthInfo.extent.height = SwapchainExtent.height;
			depthInfo.extent.depth = 1;
			depthInfo.mipLevels = 1;
			depthInfo.arrayLayers = 1;
			depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			if (vulkanFailed("CVulkanDriver: depth image",
				vk::CreateImage(Context.Device, &depthInfo, nullptr, &DepthImage)))
				return false;

			VkMemoryRequirements req = {};
			vk::GetImageMemoryRequirements(Context.Device, DepthImage, &req);
			VkMemoryAllocateInfo alloc = {};
			alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc.allocationSize = req.size;
			alloc.memoryTypeIndex = findMemoryTypeIndex(Context.MemoryProperties, req.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (alloc.memoryTypeIndex == 0xFFFFFFFFu ||
				vulkanFailed("CVulkanDriver: depth memory",
					vk::AllocateMemory(Context.Device, &alloc, nullptr, &DepthMemory)))
				return false;
			vk::BindImageMemory(Context.Device, DepthImage, DepthMemory, 0);

			VkImageViewCreateInfo depthView = {};
			depthView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			depthView.image = DepthImage;
			depthView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			depthView.format = DepthFormat;
			depthView.subresourceRange.aspectMask = depthAspectOf(DepthFormat);
			depthView.subresourceRange.levelCount = 1;
			depthView.subresourceRange.layerCount = 1;
			if (vulkanFailed("CVulkanDriver: depth image view",
				vk::CreateImageView(Context.Device, &depthView, nullptr, &DepthImageView)))
				return false;

			// Out of UNDEFINED once, here: beginRendering() names this image as a depth attachment
			// in DEPTH_STENCIL_ATTACHMENT_OPTIMAL on every frame, and CmdBeginRendering requires the
			// image to actually be in the layout the attachment declares. Unlike the swapchain
			// images, nothing else ever transitions it.
			VkCommandBuffer transition = beginUpload();
			if (transition == VK_NULL_HANDLE)
				return false;
			transitionImageLayout(transition, DepthImage, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, depthAspectOf(DepthFormat));
			endUploadAndWait(transition);
			return true;
		}

		void CVulkanDriver::destroySwapchain()
		{
			if (!Context.Device)
				return;

			if (DepthImageView)
				vk::DestroyImageView(Context.Device, DepthImageView, nullptr);
			if (DepthImage)
				vk::DestroyImage(Context.Device, DepthImage, nullptr);
			if (DepthMemory)
				vk::FreeMemory(Context.Device, DepthMemory, nullptr);
			DepthImageView = VK_NULL_HANDLE;
			DepthImage = VK_NULL_HANDLE;
			DepthMemory = VK_NULL_HANDLE;

			for (size_t i = 0; i < SwapchainImageViews.size(); ++i)
				if (SwapchainImageViews[i])
					vk::DestroyImageView(Context.Device, SwapchainImageViews[i], nullptr);
			SwapchainImageViews.clear();
			SwapchainImages.clear();

			if (Swapchain)
				vk::DestroySwapchainKHR(Context.Device, Swapchain, nullptr);
			Swapchain = VK_NULL_HANDLE;
		}

		bool CVulkanDriver::recreateSwapchain(const core::dimension2d<u32>& size)
		{
			vk::DeviceWaitIdle(Context.Device);
			destroySwapchain();
			return createSwapchain(size);
		}

		bool CVulkanDriver::createFrameContexts()
		{
			for (u32 i = 0; i < FrameCount; ++i)
			{
				SVulkanFrameContext& frame = Frames[i];

				VkCommandPoolCreateInfo poolInfo = {};
				poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
				poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
				poolInfo.queueFamilyIndex = Context.GraphicsQueueFamily;
				if (vulkanFailed("CVulkanDriver: frame command pool",
					vk::CreateCommandPool(Context.Device, &poolInfo, nullptr, &frame.CommandPool)))
					return false;

				VkCommandBufferAllocateInfo cbInfo = {};
				cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
				cbInfo.commandPool = frame.CommandPool;
				cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				cbInfo.commandBufferCount = 1;
				if (vulkanFailed("CVulkanDriver: frame command buffer",
					vk::AllocateCommandBuffers(Context.Device, &cbInfo, &frame.CommandBuffer)))
					return false;

				// Created signalled so the first beginScene() does not block on a frame that was
				// never submitted.
				VkFenceCreateInfo fenceInfo = {};
				fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
				fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
				if (vulkanFailed("CVulkanDriver: frame fence",
					vk::CreateFence(Context.Device, &fenceInfo, nullptr, &frame.Fence)))
					return false;

				VkSemaphoreCreateInfo semInfo = {};
				semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
				if (vulkanFailed("CVulkanDriver: image semaphore",
					vk::CreateSemaphore(Context.Device, &semInfo, nullptr, &frame.ImageAvailable)) ||
					vulkanFailed("CVulkanDriver: render semaphore",
						vk::CreateSemaphore(Context.Device, &semInfo, nullptr, &frame.RenderFinished)))
					return false;

				if (!addDescriptorPool(frame))
					return false;

				if (!createVulkanBuffer(Context, kUniformRingSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					frame.UniformRing, frame.UniformRingMemory))
					return false;
				frame.UniformRingCapacity = kUniformRingSize;
				if (vulkanFailed("CVulkanDriver: uniform ring map",
					vk::MapMemory(Context.Device, frame.UniformRingMemory, 0, kUniformRingSize, 0,
						reinterpret_cast<void**>(&frame.UniformRingMapped))))
					return false;

				frame.Immediate = new CVulkanImmediateRing(Context);
				if (!frame.Immediate->init())
					return false;
			}
			return true;
		}

		void CVulkanDriver::destroyFrameContexts()
		{
			if (!Context.Device)
				return;

			for (u32 i = 0; i < FrameCount; ++i)
			{
				SVulkanFrameContext& frame = Frames[i];
				delete frame.Immediate; // destroys its buffers; the caller already waited for idle
				frame.Immediate = nullptr;
				if (frame.UniformRingMapped)
					vk::UnmapMemory(Context.Device, frame.UniformRingMemory);
				if (frame.UniformRing)
					vk::DestroyBuffer(Context.Device, frame.UniformRing, nullptr);
				if (frame.UniformRingMemory)
					vk::FreeMemory(Context.Device, frame.UniformRingMemory, nullptr);
				for (size_t r = 0; r < frame.RetiredUniformBuffers.size(); ++r)
				{
					vk::DestroyBuffer(Context.Device, frame.RetiredUniformBuffers[r], nullptr);
					vk::FreeMemory(Context.Device, frame.RetiredUniformMemory[r], nullptr);
				}
				for (size_t p = 0; p < frame.DescriptorPools.size(); ++p)
					vk::DestroyDescriptorPool(Context.Device, frame.DescriptorPools[p], nullptr);
				if (frame.ImageAvailable)
					vk::DestroySemaphore(Context.Device, frame.ImageAvailable, nullptr);
				if (frame.RenderFinished)
					vk::DestroySemaphore(Context.Device, frame.RenderFinished, nullptr);
				if (frame.Fence)
					vk::DestroyFence(Context.Device, frame.Fence, nullptr);
				if (frame.CommandPool)
					vk::DestroyCommandPool(Context.Device, frame.CommandPool, nullptr);
				frame = SVulkanFrameContext();
			}
		}

		bool CVulkanDriver::addDescriptorPool(SVulkanFrameContext& frame)
		{
			// Plain UNIFORM_BUFFER, not the _DYNAMIC flavour: getVulkanDriverUniformBindings()
			// declares set 4 with that type, and a pool has to carry the types its sets ask for.
			// One draw takes one set of each kind, hence the two multipliers.
			VkDescriptorPoolSize sizes[4] = {};
			sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			sizes[0].descriptorCount = kDescriptorSetsPerFrame * EVDU_COUNT;
			sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			sizes[1].descriptorCount = kDescriptorSetsPerFrame * VulkanMaterialTextureBindingCount;
			// The pixel-stage UAVs of user materials (storage buffers and images), a few per set.
			sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			sizes[2].descriptorCount = kDescriptorSetsPerFrame * 2;
			sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			sizes[3].descriptorCount = kDescriptorSetsPerFrame * 2;

			VkDescriptorPoolCreateInfo dpInfo = {};
			dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			dpInfo.maxSets = kDescriptorSetsPerFrame;
			dpInfo.poolSizeCount = 4;
			dpInfo.pPoolSizes = sizes;

			VkDescriptorPool pool = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanDriver: descriptor pool",
				vk::CreateDescriptorPool(Context.Device, &dpInfo, nullptr, &pool)))
				return false;

			frame.DescriptorPools.push_back(pool);
			return true;
		}

		bool CVulkanDriver::growUniformRing(VkDeviceSize needed)
		{
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];

			VkDeviceSize capacity = frame.UniformRingCapacity ? frame.UniformRingCapacity * 2 : kUniformRingSize;
			while (capacity < needed)
				capacity *= 2;

			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			if (!createVulkanBuffer(Context, capacity, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				buffer, memory))
				return false;

			u8* mapped = nullptr;
			if (vulkanFailed("CVulkanDriver: uniform ring map",
				vk::MapMemory(Context.Device, memory, 0, capacity, 0, reinterpret_cast<void**>(&mapped))))
			{
				vk::DestroyBuffer(Context.Device, buffer, nullptr);
				vk::FreeMemory(Context.Device, memory, nullptr);
				return false;
			}

			// The old ring is still referenced by descriptor sets this frame already wrote, so it
			// only goes back at the next reset of this slot, once its fence has been waited on.
			if (frame.UniformRing)
			{
				vk::UnmapMemory(Context.Device, frame.UniformRingMemory);
				frame.RetiredUniformBuffers.push_back(frame.UniformRing);
				frame.RetiredUniformMemory.push_back(frame.UniformRingMemory);
			}

			frame.UniformRing = buffer;
			frame.UniformRingMemory = memory;
			frame.UniformRingMapped = mapped;
			frame.UniformRingCapacity = capacity;
			frame.UniformRingNext = 0;
			return true;
		}

		bool CVulkanDriver::reserveUniforms(VkDeviceSize bytes)
		{
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			const VkDeviceSize alignment = Context.DeviceProperties.limits.minUniformBufferOffsetAlignment;
			const VkDeviceSize start = (frame.UniformRingNext + alignment - 1) & ~(alignment - 1);
			if (start + bytes <= frame.UniformRingCapacity)
				return true;
			return growUniformRing(bytes);
		}

		VkDeviceSize CVulkanDriver::allocateUniform(const void* data, size_t sizeBytes)
		{
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			const VkDeviceSize alignment = Context.DeviceProperties.limits.minUniformBufferOffsetAlignment;
			VkDeviceSize offset = (frame.UniformRingNext + alignment - 1) & ~(alignment - 1);
			if (offset + sizeBytes > frame.UniformRingCapacity)
			{
				// Only reached when the caller did not reserve; growing here would move the ring out
				// from under descriptors written earlier in the same set.
				if (!growUniformRing(sizeBytes))
					return VK_WHOLE_SIZE;
				offset = 0;
			}
			memcpy(frame.UniformRingMapped + offset, data, sizeBytes);
			frame.UniformRingNext = offset + sizeBytes;
			return offset;
		}

		VkDescriptorSet CVulkanDriver::allocateDescriptorSet(VkDescriptorSetLayout layout)
		{
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];

			VkDescriptorSetAllocateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			info.descriptorSetCount = 1;
			info.pSetLayouts = &layout;

			// Walk the pools this frame already has, then add one. A pool that reports FRAGMENTED or
			// OUT_OF_POOL_MEMORY is spent for this frame but still valid, so it is simply skipped.
			for (;;)
			{
				if (frame.CurrentDescriptorPool >= frame.DescriptorPools.size() &&
					!addDescriptorPool(frame))
					return VK_NULL_HANDLE;

				info.descriptorPool = frame.DescriptorPools[frame.CurrentDescriptorPool];

				VkDescriptorSet set = VK_NULL_HANDLE;
				const VkResult result = vk::AllocateDescriptorSets(Context.Device, &info, &set);
				if (result == VK_SUCCESS)
					return set;
				if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
				{
					vulkanFailed("CVulkanDriver: vkAllocateDescriptorSets", result);
					return VK_NULL_HANDLE;
				}
				++frame.CurrentDescriptorPool;
			}
		}

		bool CVulkanDriver::beginScene(bool backBuffer, bool zBuffer, SColor color,
			const SExposedVideoData& videoData, core::rect<s32>* sourceRect)
		{
			CNullDriver::beginScene(backBuffer, zBuffer, color, videoData, sourceRect);

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			vk::WaitForFences(Context.Device, 1, &frame.Fence, VK_TRUE, UINT64_MAX);

			VkResult acquired = vk::AcquireNextImageKHR(Context.Device, Swapchain, UINT64_MAX,
				frame.ImageAvailable, VK_NULL_HANDLE, &CurrentImageIndex);
			if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
			{
				if (!recreateSwapchain(ScreenSize))
					return false;
				acquired = vk::AcquireNextImageKHR(Context.Device, Swapchain, UINT64_MAX,
					frame.ImageAvailable, VK_NULL_HANDLE, &CurrentImageIndex);
			}
			if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
			{
				vulkanFailed("CVulkanDriver: vkAcquireNextImageKHR", acquired);
				return false;
			}

			vk::ResetFences(Context.Device, 1, &frame.Fence);
			vk::ResetCommandPool(Context.Device, frame.CommandPool, 0);
			for (size_t p = 0; p < frame.DescriptorPools.size(); ++p)
				vk::ResetDescriptorPool(Context.Device, frame.DescriptorPools[p], 0);
			frame.CurrentDescriptorPool = 0;
			frame.UniformRingNext = 0;
			// The fence above proves the GPU is done with the sets that pointed into these.
			for (size_t r = 0; r < frame.RetiredUniformBuffers.size(); ++r)
			{
				vk::DestroyBuffer(Context.Device, frame.RetiredUniformBuffers[r], nullptr);
				vk::FreeMemory(Context.Device, frame.RetiredUniformMemory[r], nullptr);
			}
			frame.RetiredUniformBuffers.clear();
			frame.RetiredUniformMemory.clear();
			// true: the fence above proves the GPU is done with everything this slot recorded, so
			// buffers a previous frame retired on growth can go now.
			frame.Immediate->reset(true);

			// A render target from the previous frame does not survive it; the swapchain image is
			// what beginScene() clears and presents.
			RenderTargetActive = false;
			RenderTarget->reset();
			CurrentRenderTargetSize = core::dimension2d<u32>(SwapchainExtent.width, SwapchainExtent.height);

			VkCommandBufferBeginInfo begin = {};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			if (vulkanFailed("CVulkanDriver: vkBeginCommandBuffer",
				vk::BeginCommandBuffer(frame.CommandBuffer, &begin)))
				return false;

			// Timers / statistics: this slot's fence passed above, so its last results can be read,
			// and its query range reset here, outside any rendering instance, for this frame's use.
			if (StatsArmed && !QueryResourcesCreated)
				createQueryResources();
			if (QueryResourcesCreated)
			{
				harvestQueryFrame(CurrentFrameIndex);
				if (TimestampPool)
					vk::CmdResetQueryPool(frame.CommandBuffer, TimestampPool,
						CurrentFrameIndex * 2 * EMCS_MAX_TIMER_QUERIES, 2 * EMCS_MAX_TIMER_QUERIES);
				if (StatsPool)
				{
					vk::CmdResetQueryPool(frame.CommandBuffer, StatsPool, CurrentFrameIndex, 1);
					if (StatsArmed)
					{
						// Begun outside the rendering instance and ended outside it at endScene(): a
						// query may span rendering instances but not straddle one boundary.
						vk::CmdBeginQuery(frame.CommandBuffer, StatsPool, CurrentFrameIndex, 0);
						StatsOpen[CurrentFrameIndex] = true;
					}
				}
			}

			transitionImageLayout(frame.CommandBuffer, SwapchainImages[CurrentImageIndex],
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_ASPECT_COLOR_BIT);

			SceneOpen = true;
			beginRendering(backBuffer, zBuffer, color);
			return true;
		}

		void CVulkanDriver::beginRendering(bool clearColor, bool clearDepth, SColor color)
		{
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			// A new rendering instance starts with one viewport, whatever setViewPorts() had set.
			ViewportCount = 1;

			// A bound render target describes its own attachments; the swapchain path below is only
			// for the default target. Both end with the same viewport/scissor setup.
			if (RenderTargetActive && RenderTarget->isValid())
			{
				const VkRenderingInfo& targetInfo = RenderTarget->getRenderingInfo(clearColor, clearDepth, color);
				vk::CmdBeginRendering(frame.CommandBuffer, &targetInfo);
				RenderingActive = true;

				VkViewport targetViewport = {};
				targetViewport.width = static_cast<float>(RenderTarget->getSize().Width);
				targetViewport.height = static_cast<float>(RenderTarget->getSize().Height);
				targetViewport.maxDepth = 1.0f;
				vk::CmdSetViewport(frame.CommandBuffer, 0, 1, &targetViewport);

				VkRect2D targetScissor = {};
				targetScissor.extent.width = RenderTarget->getSize().Width;
				targetScissor.extent.height = RenderTarget->getSize().Height;
				vk::CmdSetScissor(frame.CommandBuffer, 0, 1, &targetScissor);
				// Kept in step with what was just recorded: setScissorFromClip() bounds every later
				// scissor by ViewPort, so a stale one would clip the whole target away.
				ViewPort = core::rect<s32>(0, 0, (s32)RenderTarget->getSize().Width,
					(s32)RenderTarget->getSize().Height);
				return;
			}

			VkRenderingAttachmentInfo colorAttachment = {};
			colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			colorAttachment.imageView = SwapchainImageViews[CurrentImageIndex];
			colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			colorAttachment.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			colorAttachment.clearValue.color.float32[0] = color.getRed() / 255.f;
			colorAttachment.clearValue.color.float32[1] = color.getGreen() / 255.f;
			colorAttachment.clearValue.color.float32[2] = color.getBlue() / 255.f;
			colorAttachment.clearValue.color.float32[3] = color.getAlpha() / 255.f;

			VkRenderingAttachmentInfo depthAttachment = {};
			depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			depthAttachment.imageView = DepthImageView;
			depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depthAttachment.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			// 0.0f, not 1.0f: this fork uses a reversed-Z convention, see the default
			// SMaterial::ZBuffer of ECFN_GREATER.
			depthAttachment.clearValue.depthStencil.depth = 0.0f;
			depthAttachment.clearValue.depthStencil.stencil = 0;

			VkRenderingInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
			info.renderArea.extent = SwapchainExtent;
			info.layerCount = 1;
			info.colorAttachmentCount = 1;
			info.pColorAttachments = &colorAttachment;
			info.pDepthAttachment = &depthAttachment;
			// Named as the stencil attachment too when the format carries one, or the shadow volume
			// pipelines (which declare a stencil format) would not match this instance.
			info.pStencilAttachment = depthFormatHasStencil(DepthFormat) ? &depthAttachment : nullptr;

			vk::CmdBeginRendering(frame.CommandBuffer, &info);
			RenderingActive = true;

			// Positive height: the y flip is done in the vertex shaders (see the GLSL sources under
			// vulkan/shaders), so flipping the viewport too would cancel it out and render upside
			// down. Depth needs nothing here -- the reversed-Z mapping is already in the projection
			// matrix, and Vulkan's [0,1] clip z matches D3D's.
			VkViewport viewport = {};
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = static_cast<float>(SwapchainExtent.width);
			viewport.height = static_cast<float>(SwapchainExtent.height);
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vk::CmdSetViewport(frame.CommandBuffer, 0, 1, &viewport);

			VkRect2D scissor = {};
			scissor.extent = SwapchainExtent;
			vk::CmdSetScissor(frame.CommandBuffer, 0, 1, &scissor);
			// Same reason as the render-target branch above.
			ViewPort = core::rect<s32>(0, 0, (s32)SwapchainExtent.width, (s32)SwapchainExtent.height);
		}

		void CVulkanDriver::endRendering()
		{
			if (!RenderingActive)
				return;
			// Conditional rendering must end inside the rendering instance it began in; a block
			// still open here (a suspend for compute, or endScene()) is closed with it.
			endPredicatedDraws();
			vk::CmdEndRendering(Frames[CurrentFrameIndex].CommandBuffer);
			RenderingActive = false;
		}

		bool CVulkanDriver::endScene()
		{
			CNullDriver::endScene();
			if (!SceneOpen)
				return false;

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			endRendering();

			// The statistics query opened by beginScene(), outside the rendering instance like it.
			if (StatsOpen[CurrentFrameIndex] && StatsPool)
				vk::CmdEndQuery(frame.CommandBuffer, StatsPool, CurrentFrameIndex);

			// A target still bound at endScene() has to be released before the present transition,
			// or its colour textures stay in the attachment layout and cannot be sampled next frame.
			if (RenderTargetActive)
			{
				RenderTarget->unbind(frame.CommandBuffer);
				RenderTargetActive = false;
			}

			transitionImageLayout(frame.CommandBuffer, SwapchainImages[CurrentImageIndex],
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				VK_IMAGE_ASPECT_COLOR_BIT);

			if (vulkanFailed("CVulkanDriver: vkEndCommandBuffer",
				vk::EndCommandBuffer(frame.CommandBuffer)))
				return false;

			VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			VkSubmitInfo submit = {};
			submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &frame.ImageAvailable;
			submit.pWaitDstStageMask = &waitStage;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &frame.CommandBuffer;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &frame.RenderFinished;
			VkPresentInfoKHR present = {};
			present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			present.waitSemaphoreCount = 1;
			present.pWaitSemaphores = &frame.RenderFinished;
			present.swapchainCount = 1;
			present.pSwapchains = &Swapchain;
			present.pImageIndices = &CurrentImageIndex;

			// The queue is shared with deferred contexts and uploads on other threads; a VkQueue
			// is externally synchronized.
			VkResult presented = VK_SUCCESS;
			{
				std::lock_guard<std::mutex> queueLock(ResourceOwner->QueueMutex);
				if (vulkanFailed("CVulkanDriver: vkQueueSubmit",
					vk::QueueSubmit(Context.GraphicsQueue, 1, &submit, frame.Fence)))
					return false;
				presented = vk::QueuePresentKHR(Context.GraphicsQueue, &present);
			}
			if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
				recreateSwapchain(ScreenSize);
			else if (presented != VK_SUCCESS)
				vulkanFailed("CVulkanDriver: vkQueuePresentKHR", presented);

			CurrentFrameIndex = (CurrentFrameIndex + 1) % FrameCount;
			// Everything recorded so far is now submitted: a query stamped with the old value may be
			// waited on, one stamped with the new value is still being recorded.
			++FrameCounter;
			SceneOpen = false;
			return true;
		}

		// An upload scope is begin -> record -> endUploadAndWait on one thread. The scope holds the
		// owner's UploadMutex for its whole length: the pool is not thread-safe and a deferred context
		// recording on a worker creates its buffers through the owner (see ResourceOwner). Recursive,
		// so a scope opened inside another on the same thread does not deadlock.
		VkCommandBuffer CVulkanDriver::beginUpload()
		{
			ResourceOwner->UploadMutex.lock();

			VkCommandBufferAllocateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			info.commandPool = UploadPool;
			info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			info.commandBufferCount = 1;

			VkCommandBuffer cb = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanDriver: upload command buffer",
				vk::AllocateCommandBuffers(Context.Device, &info, &cb)))
			{
				ResourceOwner->UploadMutex.unlock();
				return VK_NULL_HANDLE;
			}

			VkCommandBufferBeginInfo begin = {};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			if (vulkanFailed("CVulkanDriver: begin upload buffer", vk::BeginCommandBuffer(cb, &begin)))
			{
				vk::FreeCommandBuffers(Context.Device, UploadPool, 1, &cb);
				ResourceOwner->UploadMutex.unlock();
				return VK_NULL_HANDLE;
			}
			return cb;
		}

		void CVulkanDriver::endUploadAndWait(VkCommandBuffer commandBuffer)
		{
			if (!commandBuffer)
				return;

			if (!vulkanFailed("CVulkanDriver: end upload buffer", vk::EndCommandBuffer(commandBuffer)))
			{
				VkSubmitInfo submit = {};
				submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				submit.commandBufferCount = 1;
				submit.pCommandBuffers = &commandBuffer;

				// Blocking, like the D3D12 upload scope: the caller frees its staging buffer straight
				// after, so the copy must have completed. The queue lock covers submit and wait.
				std::lock_guard<std::mutex> queueLock(ResourceOwner->QueueMutex);
				if (!vulkanFailed("CVulkanDriver: submit upload",
					vk::QueueSubmit(Context.GraphicsQueue, 1, &submit, VK_NULL_HANDLE)))
					vk::QueueWaitIdle(Context.GraphicsQueue);
			}

			vk::FreeCommandBuffers(Context.Device, UploadPool, 1, &commandBuffer);
			ResourceOwner->UploadMutex.unlock();
		}

		void CVulkanDriver::OnResize(const core::dimension2d<u32>& size)
		{
			CNullDriver::OnResize(size);
			if (Context.Device && Swapchain)
				recreateSwapchain(size);

			CurrentRenderTargetSize = core::dimension2d<u32>(SwapchainExtent.width, SwapchainExtent.height);
			ViewPort = core::rect<s32>(0, 0, (s32)SwapchainExtent.width, (s32)SwapchainExtent.height);
		}

		core::dimension2du CVulkanDriver::getMaxTextureSize() const
		{
			const u32 limit = Context.DeviceProperties.limits.maxImageDimension2D;
			return core::dimension2du(limit ? limit : 2048, limit ? limit : 2048);
		}

		u32 CVulkanDriver::queryMultisampleLevels(ECOLOR_FORMAT format, u32 numSamples) const
		{
			if (numSamples <= 1)
				return 1;
			// Power of two only, and it has to be in both attachment masks: a draw writes colour
			// and depth at the same sample count.
			if (numSamples > 64 || (numSamples & (numSamples - 1)) != 0)
				return 0;

			const VkSampleCountFlags supported =
				Context.DeviceProperties.limits.framebufferColorSampleCounts &
				Context.DeviceProperties.limits.framebufferDepthSampleCounts;
			return (supported & numSamples) ? 1 : 0;
		}

		bool CVulkanDriver::setClipPlane(u32 index, const core::plane3df& plane, bool enable)
		{
			if (index > 2)
				return false;
			ClipPlanes[index] = plane;
			enableClipPlane(index, enable);
			return true;
		}

		void CVulkanDriver::enableClipPlane(u32 index, bool enable)
		{
			if (index > 2)
				return;
			ClipPlaneEnabled[index] = enable;
		}

		bool CVulkanDriver::queryFeature(E_VIDEO_DRIVER_FEATURE feature) const
		{
			// The disableFeature() mask, as CD3D11Driver honours it. NOT CNullDriver::queryFeature():
			// that one is an unconditional "false" which used to hide the whole table below.
			if (feature < 0 || feature >= EVDF_COUNT || !FeatureEnabled[feature])
				return false;

			switch (feature)
			{
			case EVDF_OCCLUSION_QUERY:
				return Occlusion && Occlusion->isValid();
			case EVDF_COMPUTING_SHADER_5_0:
			case EVDF_BOUND_COMPUTE_PIPELINE:
				return Compute && Compute->isReady();
			// Only when the swapchain depth buffer actually got a stencil aspect (asked for through
			// SIrrlichtCreationParameters::Stencilbuffer and offered by the device).
			case EVDF_STENCIL_BUFFER:
				return depthFormatHasStencil(DepthFormat);
			case EVDF_RENDER_TO_TARGET:
			case EVDF_HARDWARE_TL:
			case EVDF_MULTITEXTURE:
			case EVDF_BILINEAR_FILTER:
			case EVDF_MIP_MAP:
			case EVDF_MIP_MAP_AUTO_UPDATE:
			case EVDF_TEXTURE_NPOT:
			case EVDF_COLOR_MASK:
			case EVDF_BLEND_OPERATIONS:
				return true;
			case EVDF_TEXTURE_COMPRESSED_DXT:
			case EVDF_VERTEX_BUFFER_OBJECT:
			case EVDF_FRAMEBUFFER_OBJECT:
			case EVDF_MULTIPLE_RENDER_TARGETS:
			case EVDF_POLYGON_OFFSET:
			// alphaToCoverageEnable is core pipeline state; multisampled render targets resolve
			// into their sampled image at the end of the pass (see CVulkanTexture).
			case EVDF_ALPHA_TO_COVERAGE:
			case EVDF_TEXTURE_MULTISAMPLING:
				return true;
			// The per-target IRenderTarget blend/mask of setRenderTarget(array) needs independentBlend.
			case EVDF_MRT_BLEND:
			case EVDF_MRT_COLOR_MASK:
			case EVDF_MRT_BLEND_FUNC:
				return Context.HasIndependentBlend;
			// Only a user material reaches this stage, and only if the device offered it. Vulkan has
			// no shader models, so the three "GS 4.0/4.1/5.0" flags say exactly the same thing.
			// E_VIDEO_DRIVER_FEATURE has no tessellation entry, hence none here either -- the pair is
			// enabled on the device all the same, see createLogicalDevice().
			case EVDF_GEOMETRY_SHADER:
			case EVDF_GEOMETRY_SHADER_4_0:
			case EVDF_GEOMETRY_SHADER_4_1:
			case EVDF_GEOMETRY_SHADER_5_0:
				return HasGeometryShader;
			// Which source language a user material may be written in is a build-time question: the
			// two front ends are optional, and a build with neither takes pre-compiled SPIR-V only.
			case EVDF_HLSL:
#ifdef _IRR_COMPILE_WITH_VULKAN_DXC_
				return true;
#else
				return false;
#endif
			case EVDF_ARB_GLSL:
#ifdef _IRR_COMPILE_WITH_VULKAN_GLSLANG_
				return true;
#else
				return false;
#endif
			// --- The D3D11.x additions, see doc/d3d11-feature-api.md ---
			case EVDF_DUAL_SOURCE_BLEND:
				return Context.HasDualSrcBlend;
			case EVDF_LOGIC_OP:
				return Context.HasLogicOp;
			case EVDF_CONSERVATIVE_RASTERIZATION:
				return Context.HasConservativeRaster;
			case EVDF_PIXEL_SHADER_STENCIL_REF:
				return Context.HasStencilExport;
			case EVDF_RASTERIZER_ORDERED_VIEWS:
				return Context.HasFragmentShaderInterlock;
			case EVDF_MULTIPLE_VIEWPORTS:
				return Context.HasMultiViewport;
			case EVDF_MINMAX_FILTER:
				return Context.HasSamplerFilterMinmax;
			case EVDF_PREDICATION:
				return Context.HasConditionalRendering && Occlusion && Occlusion->isValid() &&
					vk::CmdBeginConditionalRenderingEXT != nullptr;
			case EVDF_TIMER_QUERY:
				return Context.DeviceProperties.limits.timestampComputeAndGraphics == VK_TRUE;
			case EVDF_TILED_RESOURCES:
				return Context.HasSparseResidency;
			case EVDF_PIXEL_SHADER_UAV:
				return Context.HasFragmentStores;
			case EVDF_NATIVE_DEFERRED_CONTEXT:
				return true;
			default:
				return false;
			}
		}

		void CVulkanDriver::setTransform(E_TRANSFORMATION_STATE state, const core::matrix4& mat)
		{
			Matrices[state] = mat;
		}

		void CVulkanDriver::setMaterial(const SMaterial& material)
		{
			Material = material;
			OverrideMaterial.apply(Material);
		}

		void CVulkanDriver::setViewPort(const core::rect<s32>& area)
		{
			// Clipped against the current target, and remembered: getViewPort() feeding back an
			// empty rect makes GUI hit-testing collapse.
			core::rect<s32> vp = area;
			vp.clipAgainst(core::rect<s32>(0, 0, (s32)CurrentRenderTargetSize.Width,
				(s32)CurrentRenderTargetSize.Height));
			if (vp.getWidth() <= 0 || vp.getHeight() <= 0)
				return;

			ViewPort = vp;
			// The one-viewport case: the pipelines drawn from here declare a single viewport again.
			ViewportCount = 1;
			if (!SceneOpen)
				return;

			// Positive height, same reason as beginRendering().
			VkViewport viewport = {};
			viewport.x = static_cast<float>(vp.UpperLeftCorner.X);
			viewport.y = static_cast<float>(vp.UpperLeftCorner.Y);
			viewport.width = static_cast<float>(vp.getWidth());
			viewport.height = static_cast<float>(vp.getHeight());
			viewport.minDepth = 0.0f;
			viewport.maxDepth = 1.0f;
			vk::CmdSetViewport(Frames[CurrentFrameIndex].CommandBuffer, 0, 1, &viewport);
			// D3D clips to the viewport on its own; Vulkan does not, it clips to the scissor. Both
			// have to move together or a smaller viewport would only rescale, not confine.
			setScissorFromClip(nullptr);
		}

		void CVulkanDriver::setViewPorts(const core::array<core::rect<s32> >& areas)
		{
			if (areas.empty())
				return;
			// The first entry goes through the ordinary path (getViewPort(), scissor 0); the full
			// array then replaces it. The count becomes pipeline state (SVulkanPipelineKey::
			// ViewportCount) until the next setViewPort() or render target change, both of which
			// go back to one, as the D3D drivers do.
			setViewPort(areas[0]);
			if (areas.size() < 2 || !Context.HasMultiViewport || !SceneOpen)
				return;

			const u32 maxViewports = Context.DeviceProperties.limits.maxViewports;
			const core::rect<s32> bounds(0, 0, (s32)CurrentRenderTargetSize.Width, (s32)CurrentRenderTargetSize.Height);
			VkViewport viewports[16];
			VkRect2D scissors[16];
			u32 count = 0;
			for (u32 i = 0; i < areas.size() && count < 16 && count < maxViewports; ++i)
			{
				core::rect<s32> vp = areas[i];
				vp.clipAgainst(bounds);
				if (vp.getWidth() <= 0 || vp.getHeight() <= 0)
					continue;
				VkViewport& v = viewports[count];
				v.x = static_cast<float>(vp.UpperLeftCorner.X);
				v.y = static_cast<float>(vp.UpperLeftCorner.Y);
				v.width = static_cast<float>(vp.getWidth());
				v.height = static_cast<float>(vp.getHeight());
				v.minDepth = 0.0f;
				v.maxDepth = 1.0f;
				VkRect2D& s = scissors[count];
				s.offset.x = vp.UpperLeftCorner.X;
				s.offset.y = vp.UpperLeftCorner.Y;
				s.extent.width = (u32)vp.getWidth();
				s.extent.height = (u32)vp.getHeight();
				++count;
			}
			if (count < 2)
				return;
			ViewportCount = count;
			vk::CmdSetViewport(Frames[CurrentFrameIndex].CommandBuffer, 0, count, viewports);
			vk::CmdSetScissor(Frames[CurrentFrameIndex].CommandBuffer, 0, count, scissors);
		}

		void CVulkanDriver::setScissorFromClip(const core::rect<s32>* clip)
		{
			if (!SceneOpen)
				return;

			// Always bounded by the current viewport, for the reason in setViewPort().
			core::rect<s32> area = ViewPort;
			if (clip)
				area.clipAgainst(*clip);
			area.clipAgainst(core::rect<s32>(0, 0, (s32)CurrentRenderTargetSize.Width,
				(s32)CurrentRenderTargetSize.Height));

			// Vulkan rejects a negative offset or an extent past the framebuffer, so an empty
			// intersection becomes a zero-sized rect rather than a wrapped-around one.
			VkRect2D scissor = {};
			scissor.offset.x = area.UpperLeftCorner.X > 0 ? area.UpperLeftCorner.X : 0;
			scissor.offset.y = area.UpperLeftCorner.Y > 0 ? area.UpperLeftCorner.Y : 0;
			scissor.extent.width = area.getWidth() > 0 ? (u32)area.getWidth() : 0;
			scissor.extent.height = area.getHeight() > 0 ? (u32)area.getHeight() : 0;
			vk::CmdSetScissor(Frames[CurrentFrameIndex].CommandBuffer, 0, 1, &scissor);
		}

		void CVulkanDriver::clearZBuffer()
		{
			if (!SceneOpen || !RenderingActive)
				return;

			VkClearAttachment clear = {};
			clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			// Stencil goes with it, as the D3D12 driver's ClearDepthStencilView() clears both.
			if (currentTargetHasStencil())
				clear.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
			clear.clearValue.depthStencil.depth = 0.0f; // reversed-Z, see beginRendering()
			clear.clearValue.depthStencil.stencil = 0;

			VkClearRect rect = {};
			rect.rect.extent.width = CurrentRenderTargetSize.Width;
			rect.rect.extent.height = CurrentRenderTargetSize.Height;
			rect.layerCount = 1;
			vk::CmdClearAttachments(Frames[CurrentFrameIndex].CommandBuffer, 1, &clear, 1, &rect);
		}

		void CVulkanDriver::resolveDrawProgram(const SMaterial& material,
			IVertexDescriptor* descriptor, SVulkanDrawProgram& out) const
		{
			// A user shader answers first: it owns every stage it declares plus its own pipeline
			// layout, built from its reflection at registration time.
			if (const CVulkanUserMaterial* user = getUserMaterial(material.MaterialType))
			{
				out.Vertex = user->getModule(EVUS_VERTEX);
				out.Fragment = user->getModule(EVUS_FRAGMENT);
				out.Geometry = user->getModule(EVUS_GEOMETRY);
				out.TessControl = user->getModule(EVUS_HULL);
				out.TessEval = user->getModule(EVUS_DOMAIN);
				out.Layout = ResourceOwner->UserLayouts[material.MaterialType].PipelineLayout;
				out.EntryPoints.Vertex = user->getEntryPoint(EVUS_VERTEX);
				out.EntryPoints.Fragment = user->getEntryPoint(EVUS_FRAGMENT);
				out.EntryPoints.Geometry = user->getEntryPoint(EVUS_GEOMETRY);
				out.EntryPoints.TessControl = user->getEntryPoint(EVUS_HULL);
				out.EntryPoints.TessEval = user->getEntryPoint(EVUS_DOMAIN);
				return;
			}

			// EVT_2TCOORDS meshes take the second-UV variant of the 12 multi-texture materials;
			// getVertexModule()/getFragmentModule() fall back on their own when there is none.
			const bool use2TCoords = descriptor && descriptor->getID() == EVT_2TCOORDS;
			const CVulkanMaterialRenderer* renderer = getNativeRenderer(material.MaterialType);

			out.Vertex = renderer ? renderer->getVertexModule(use2TCoords) : VK_NULL_HANDLE;
			out.Fragment = renderer ? renderer->getFragmentModule(use2TCoords) : VK_NULL_HANDLE;
			out.Layout = BuiltInPipelineLayout;

			// Unregistered or foreign MaterialType: draw it opaque with the solid shaders rather
			// than hand a null module to the pipeline cache.
			const CVulkanMaterialRenderer* solid = getNativeRenderer(EMT_SOLID);
			if (out.Vertex == VK_NULL_HANDLE && solid)
				out.Vertex = solid->getVertexModule(false);
			if (out.Fragment == VK_NULL_HANDLE && solid)
				out.Fragment = solid->getFragmentModule(false);
		}

		SVulkanPipelineKey CVulkanDriver::buildPipelineKeyFromMaterial(const SMaterial& material,
			const SVulkanDrawProgram& program, const SVulkanVertexInputState& vertexInput,
			VkPrimitiveTopology topology, const SVulkanStencilOverride* stencil) const
		{
			SVulkanPipelineKey key;

			// Baked at registration, whichever kind of renderer owns the type -- a user shader has
			// no blend state of its own and inherits its base material's. Only EMT_ONETEXTURE_BLEND
			// overrides it, just below.
			const CVulkanMaterialRenderer* renderer = getNativeRenderer(material.MaterialType);
			const CVulkanUserMaterial* user = getUserMaterial(material.MaterialType);
			E_MATERIAL_TYPE baseType = material.MaterialType;
			if (renderer)
			{
				key.BlendMode = renderer->BlendMode;
				key.CustomSrcColorFactor = renderer->CustomSrcColorFactor;
				key.CustomDstColorFactor = renderer->CustomDstColorFactor;
				key.CustomSrcAlphaFactor = renderer->CustomSrcAlphaFactor;
				key.CustomDstAlphaFactor = renderer->CustomDstAlphaFactor;
				key.CustomBlendOp = renderer->CustomBlendOp;
				baseType = renderer->BaseMaterialType;
			}
			else if (user)
			{
				key.BlendMode = user->BlendMode;
				key.CustomSrcColorFactor = user->CustomSrcColorFactor;
				key.CustomDstColorFactor = user->CustomDstColorFactor;
				key.CustomSrcAlphaFactor = user->CustomSrcAlphaFactor;
				key.CustomDstAlphaFactor = user->CustomDstAlphaFactor;
				key.CustomBlendOp = user->CustomBlendOp;
				baseType = user->BaseMaterialType;
			}

			key.VSHash = vulkanHandleHash(program.Vertex);
			key.PSHash = vulkanHandleHash(program.Fragment);
			key.GSHash = vulkanHandleHash(program.Geometry);
			key.HSHash = vulkanHandleHash(program.TessControl);
			key.DSHash = vulkanHandleHash(program.TessEval);
			key.PipelineLayoutHash = vulkanHandleHash(program.Layout);
			key.VertexLayoutHash = vertexInput.hash();

			// EMT_ONETEXTURE_BLEND encodes its factors per instance in MaterialTypeParam, so they
			// cannot be baked into the renderer and are decoded on every draw.
			if (material.MaterialType == EMT_ONETEXTURE_BLEND || baseType == EMT_ONETEXTURE_BLEND)
			{
				E_BLEND_FACTOR srcFact, dstFact;
				E_MODULATE_FUNC modulate;
				u32 alphaSource;
				unpack_textureBlendFunc(srcFact, dstFact, modulate, alphaSource, material.MaterialTypeParam);
				key.BlendMode = SVulkanPipelineKey::EBlendMode::Custom;
				key.CustomSrcColorFactor = getVulkanBlendFactor(srcFact, false);
				key.CustomDstColorFactor = getVulkanBlendFactor(dstFact, false);
				key.CustomSrcAlphaFactor = getVulkanBlendFactor(srcFact, true);
				key.CustomDstAlphaFactor = getVulkanBlendFactor(dstFact, true);
				// modulate and alphaSource are not applied, same limitation as the D3D12 backend.
			}

			// SMaterial::BlendOperation/BlendFactor, independent of MaterialType. BlendFactor == 0
			// is not "no factors packed but blending on": unpacking it yields ZERO everywhere, which
			// renders black, so the base blend above is kept in that case.
			if (material.BlendOperation != EBO_NONE && material.BlendFactor != 0.0f)
			{
				E_BLEND_FACTOR srcRGB, dstRGB, srcAlpha, dstAlpha;
				E_MODULATE_FUNC modulate;
				u32 alphaSource;
				unpack_textureBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha, modulate,
					alphaSource, material.BlendFactor);
				key.BlendMode = SVulkanPipelineKey::EBlendMode::Custom;
				key.CustomBlendOp = getVulkanBlendOp(material.BlendOperation);
				key.CustomSrcColorFactor = getVulkanBlendFactor(srcRGB, false);
				key.CustomDstColorFactor = getVulkanBlendFactor(dstRGB, false);
				key.CustomSrcAlphaFactor = getVulkanBlendFactor(srcAlpha, true);
				key.CustomDstAlphaFactor = getVulkanBlendFactor(dstAlpha, true);
			}

			// Attachment formats of what is bound right now: a pipeline whose formats disagree with
			// the running VkRenderingInfo is invalid, exactly as a D3D12 PSO's RTV formats are.
			if (RenderTargetActive && RenderTarget->isValid())
			{
				key.ColorAttachmentCount = RenderTarget->getColorAttachmentCount();
				for (u32 i = 0; i < 8; ++i)
					key.ColorFormats[i] = RenderTarget->getColorFormat(i);
				key.DepthFormat = RenderTarget->getDepthFormat();
				key.SampleCount = RenderTarget->getSampleCount();
			}
			else
			{
				key.ColorAttachmentCount = 1;
				key.ColorFormats[0] = SwapchainFormat;
				for (u32 i = 1; i < 8; ++i)
					key.ColorFormats[i] = VK_FORMAT_UNDEFINED;
				key.DepthFormat = DepthFormat;
				key.SampleCount = VK_SAMPLE_COUNT_1_BIT;
			}

			// No depth attachment means no depth test at all, not just a missing format.
			const bool hasDepth = key.DepthFormat != VK_FORMAT_UNDEFINED;
			key.DepthTestEnable = hasDepth && material.ZBuffer != ECFN_DISABLED;
			key.DepthWriteEnable = material.ZWriteEnable && key.DepthTestEnable;
			key.DepthCompareOp = getVulkanCompareOp(static_cast<E_COMPARISON_FUNC>(material.ZBuffer));

			// SMaterial allows both culling flags at once (nothing drawn); Vulkan's FRONT_AND_BACK
			// says exactly that, so unlike D3D12 no approximation is needed here.
			key.CullMode = VK_CULL_MODE_NONE;
			if (material.BackfaceCulling)
				key.CullMode |= VK_CULL_MODE_BACK_BIT;
			if (material.FrontfaceCulling)
				key.CullMode |= VK_CULL_MODE_FRONT_BIT;
			key.FrontFace = VK_FRONT_FACE_CLOCKWISE; // Irrlicht winds front faces clockwise

			key.PolygonMode = material.Wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
			key.Topology = topology;
			// SMaterial::PointCloud has no fill mode of its own here either: rasterizing the
			// vertices as points is the same approximation the D3D12 driver makes.
			if (material.PointCloud)
				key.Topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

			// The ECP_* bits do not line up numerically with VK_COLOR_COMPONENT_*.
			key.ColorWriteMask =
				((material.ColorMask & ECP_RED) ? VK_COLOR_COMPONENT_R_BIT : 0) |
				((material.ColorMask & ECP_GREEN) ? VK_COLOR_COMPONENT_G_BIT : 0) |
				((material.ColorMask & ECP_BLUE) ? VK_COLOR_COMPONENT_B_BIT : 0) |
				((material.ColorMask & ECP_ALPHA) ? VK_COLOR_COMPONENT_A_BIT : 0);

			// SMaterial::AntiAliasing & EAAM_ALPHA_TO_COVERAGE, as CD3D11Driver::setBasicRenderStates().
			key.AlphaToCoverage = (material.AntiAliasing & EAAM_ALPHA_TO_COVERAGE) != 0;

			// A stream-output geometry material never rasterizes (D3D11_SO_NO_RASTERIZED_STREAM).
			key.RasterizerDiscard = user && user->StreamOutput;

			// The D3D11.x material state, each behind its device feature (doc/d3d11-feature-api.md).
			key.SampleMask = material.SampleMask;
			if (material.LogicOp != ELO_NONE)
			{
				if (Context.HasLogicOp)
				{
					key.LogicOpEnable = true;
					key.LogicOp = getVulkanLogicOp(material.LogicOp);
				}
				else if (!WarnedNoLogicOp)
				{
					os::Printer::log("CVulkanDriver: SMaterial::LogicOp needs EVDF_LOGIC_OP, ignored", ELL_WARNING);
					WarnedNoLogicOp = true;
				}
			}
			if (material.ConservativeRaster)
			{
				if (Context.HasConservativeRaster)
					key.ConservativeRaster = true;
				else if (!WarnedNoConservativeRaster)
				{
					os::Printer::log("CVulkanDriver: SMaterial::ConservativeRaster needs EVDF_CONSERVATIVE_RASTERIZATION, ignored", ELL_WARNING);
					WarnedNoConservativeRaster = true;
				}
			}
			key.ViewportCount = ViewportCount;

			// The IRenderTarget overrides of the bound MRT set, slots 1 and up (slot 0 follows the
			// material, as on D3D11 where setBasicRenderStates() rewrites RenderTarget[0]).
			if (RenderTargetActive && MrtBlend.Mask && Context.HasIndependentBlend)
			{
				key.TargetOverrideMask = MrtBlend.Mask & 0xFEu;
				for (u32 i = 1; i < 8; ++i)
				{
					if (!(key.TargetOverrideMask & (1u << i)))
						continue;
					key.TargetBlendEnable[i] = MrtBlend.Enable[i];
					key.TargetSrcFactor[i] = MrtBlend.Src[i];
					key.TargetDstFactor[i] = MrtBlend.Dst[i];
					key.TargetWriteMask[i] = MrtBlend.Write[i];
				}
			}

			// Same sign convention as the D3D11/D3D12 drivers: EPO_BACK pushes away, EPO_FRONT pulls
			// nearer. Baked into the pipeline -- depth bias is not among the dynamic states.
			key.DepthBiasConstant = 0.0f;
			key.DepthBiasSlope = 0.0f;
			if (material.PolygonOffsetFactor)
			{
				const f32 factor = static_cast<f32>(material.PolygonOffsetFactor);
				key.DepthBiasSlope = (material.PolygonOffsetDirection == EPO_BACK) ? 1.0f : -1.0f;
				key.DepthBiasConstant = (material.PolygonOffsetDirection == EPO_BACK) ? factor : -factor;
			}

			// The shadow passes only. A stencil state against a depth-only attachment is refused by
			// the pipeline, so it is dropped rather than baked when the target carries none.
			if (stencil && hasDepth && depthFormatHasStencil(key.DepthFormat))
			{
				key.StencilTestEnable = true;
				key.StencilCompareOp = stencil->CompareOp;
				key.StencilFailOp = stencil->FailOp;
				key.StencilDepthFailOp = stencil->DepthFailOp;
				key.StencilPassOp = stencil->PassOp;
			}

			return key;
		}

		bool CVulkanDriver::bindDriverUniforms(const SMaterial& material, const core::matrix4& world,
			const core::matrix4& view, const core::matrix4& proj, VkPipelineLayout layout)
		{
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];

			// All five blocks up front, in one ring: the descriptor writes below name a single
			// VkBuffer, so a growth partway through would leave the earlier bindings in a buffer
			// this set no longer points at. Worst-case padding is one alignment per block.
			const VkDeviceSize alignment = Context.DeviceProperties.limits.minUniformBufferOffsetAlignment;
			if (!reserveUniforms(sizeof(f32) * 16 + sizeof(SVulkanPerFrame) + sizeof(SVulkanClipPlanes) +
				sizeof(SVulkanLightingConstants) + sizeof(SVulkanFogConstants) + EVDU_COUNT * alignment))
				return false;

			// Transposed on upload: core::matrix4 is row-major with a row-vector convention, while
			// std140 packs a mat4 column-major, so the transpose is what makes the shaders' "v * M"
			// reproduce v * M here. Identical bytes to what the D3D12 backend uploads.
			const core::matrix4 worldT = world.getTransposed();
			const VkDeviceSize worldOffset = allocateUniform(worldT.pointer(), sizeof(f32) * 16);

			SVulkanPerFrame perFrame;
			perFrame.View = view.getTransposed();
			perFrame.Proj = proj.getTransposed();
			{
				// World-space camera position: the origin of view space through the inverse view.
				core::matrix4 viewInv = view;
				viewInv.makeInverse();
				f32 origin[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
				viewInv.multiplyWith1x4Matrix(origin);
				perFrame.CameraPosWorld[0] = origin[0];
				perFrame.CameraPosWorld[1] = origin[1];
				perFrame.CameraPosWorld[2] = origin[2];
				perFrame.CameraPosWorld[3] = 0.0f;
			}
			const VkDeviceSize frameOffset = allocateUniform(&perFrame, sizeof(perFrame));

			SVulkanClipPlanes clipPlanes;
			for (u32 i = 0; i < 3; ++i)
			{
				if (ClipPlaneEnabled[i])
				{
					clipPlanes.Planes[i][0] = ClipPlanes[i].Normal.X;
					clipPlanes.Planes[i][1] = ClipPlanes[i].Normal.Y;
					clipPlanes.Planes[i][2] = ClipPlanes[i].Normal.Z;
					clipPlanes.Planes[i][3] = ClipPlanes[i].D;
				}
				else
				{
					// dot(worldPos1, (0,0,0,1)) == 1 > 0 for every point: nothing is discarded.
					clipPlanes.Planes[i][0] = 0.0f;
					clipPlanes.Planes[i][1] = 0.0f;
					clipPlanes.Planes[i][2] = 0.0f;
					clipPlanes.Planes[i][3] = 1.0f;
				}
			}
			const VkDeviceSize clipOffset = allocateUniform(&clipPlanes, sizeof(clipPlanes));

			// The lighting block carries data only: calcLighting() in common.glsl does the work, on
			// the vertex stage for the standard shaders and per pixel for the normal-map ones.
			SVulkanLightingConstants lighting;
			memset(&lighting, 0, sizeof(lighting));
			lighting.EnableLighting = material.Lighting ? 1 : 0;
			lighting.ColorMaterialMode = static_cast<s32>(material.ColorMaterial);
			lighting.NormalizeNormalsFlag = material.NormalizeNormals ? 1 : 0;
			// Filled even when lighting is off: the shader only reads it in the enabled branch.
			writeShaderColor(lighting.Material.Ambient, material.AmbientColor);
			writeShaderColor(lighting.Material.Diffuse, material.DiffuseColor);
			writeShaderColor(lighting.Material.Specular, material.SpecularColor);
			writeShaderColor(lighting.Material.Emissive, material.EmissiveColor);
			if (material.Lighting)
			{
				u32 count = getDynamicLightCount();
				if (count > 8)
					count = 8; // MAX_LIGHTS in common.glsl
				lighting.LightCount = static_cast<s32>(count);
				for (u32 i = 0; i < count; ++i)
				{
					const SLight& dl = getDynamicLight(i);
					SVulkanShaderLight& l = lighting.Lights[i];
					l.Position[0] = dl.Position.X;
					l.Position[1] = dl.Position.Y;
					l.Position[2] = dl.Position.Z;
					writeShaderColor(l.Diffuse, dl.DiffuseColor);
					writeShaderColor(l.Specular, dl.SpecularColor);
					writeShaderColor(l.Ambient, dl.AmbientColor);
					l.Atten[0] = dl.Attenuation.X;
					l.Atten[1] = dl.Attenuation.Y;
					l.Atten[2] = dl.Attenuation.Z;
				}
			}
			const VkDeviceSize lightingOffset = allocateUniform(&lighting, sizeof(lighting));

			SVulkanFogConstants fog;
			memset(&fog, 0, sizeof(fog));
			fog.EnableFog = material.FogEnable ? 1 : 0;
			writeShaderColor(fog.Color, video::SColorf(FogColor));
			// E_FOG_TYPE is assigned straight to FogMode, which does not line up with the FOGMODE_*
			// constants the shader switches on. Reproduced as-is for parity with D3D11/D3D12.
			fog.Mode = static_cast<s32>(FogType);
			fog.Start = FogStart;
			fog.End = FogEnd;
			fog.Density = FogDensity;
			// EMT_PARALLAX_MAP_*: MaterialTypeParam of zero means the documented 0.02 default.
			fog.ParallaxHeightScale = (material.MaterialTypeParam != 0.0f) ? material.MaterialTypeParam : 0.02f;
			const VkDeviceSize fogOffset = allocateUniform(&fog, sizeof(fog));

			// All five are tested together, after the fact: a partially written set would leave some
			// bindings pointing at the previous draw's data, which is worse than skipping the draw.
			if (worldOffset == VK_WHOLE_SIZE || frameOffset == VK_WHOLE_SIZE ||
				clipOffset == VK_WHOLE_SIZE || lightingOffset == VK_WHOLE_SIZE ||
				fogOffset == VK_WHOLE_SIZE)
				return false;

			VkDescriptorSet set = allocateDescriptorSet(DriverUniformSetLayout);
			if (set == VK_NULL_HANDLE)
				return false;

			// One VkWriteDescriptorSet per binding, all pointing into the frame's single ring buffer
			// at the offsets just allocated -- the Vulkan shape of five root CBVs.
			VkDescriptorBufferInfo buffers[EVDU_COUNT] = {};
			const VkDeviceSize offsets[EVDU_COUNT] =
				{ worldOffset, frameOffset, clipOffset, lightingOffset, fogOffset };
			const VkDeviceSize ranges[EVDU_COUNT] =
				{ sizeof(f32) * 16, sizeof(perFrame), sizeof(clipPlanes), sizeof(lighting), sizeof(fog) };

			VkWriteDescriptorSet writes[EVDU_COUNT] = {};
			for (u32 i = 0; i < EVDU_COUNT; ++i)
			{
				buffers[i].buffer = frame.UniformRing;
				buffers[i].offset = offsets[i];
				buffers[i].range = ranges[i];

				writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[i].dstSet = set;
				writes[i].dstBinding = i;
				writes[i].descriptorCount = 1;
				writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				writes[i].pBufferInfo = &buffers[i];
			}
			vk::UpdateDescriptorSets(Context.Device, EVDU_COUNT, writes, 0, nullptr);

			vk::CmdBindDescriptorSets(frame.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
				layout, DriverDescriptorSet, 1, &set, 0, nullptr);
			return true;
		}

		bool CVulkanDriver::bindMaterialTextures(const SMaterial& material, VkPipelineLayout layout)
		{
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];

			VkDescriptorSet set = allocateDescriptorSet(MaterialTextureSetLayout);
			if (set == VK_NULL_HANDLE)
				return false;

			VkDescriptorImageInfo images[VulkanMaterialTextureBindingCount] = {};
			VkWriteDescriptorSet writes[VulkanMaterialTextureBindingCount] = {};

			for (u32 i = 0; i < VulkanMaterialTextureBindingCount; ++i)
			{
				CVulkanTexture* texture = static_cast<CVulkanTexture*>(material.getTexture(i));
				// A texture that is not in the sampled layout cannot be transitioned here: a general
				// image barrier is illegal inside a dynamic rendering instance. Fall back to the
				// default texture rather than record an invalid descriptor.
				if (texture && texture->getImageLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
				{
					if (!WarnedTextureLayout)
					{
						os::Printer::log("CVulkanDriver: a material texture was not in the sampled "
							"layout at draw time, the default texture was used", ELL_WARNING);
						WarnedTextureLayout = true;
					}
					texture = nullptr;
				}
				if (!texture || texture->getImageView() == VK_NULL_HANDLE)
					texture = NullTexture; // set 0 has a fixed layout and must always be complete

				images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				images[i].imageView = texture->getImageView();
				// The layer's own settings (filter, wrap, bias, min/max, min LOD), as on the D3D drivers.
				images[i].sampler = Samplers.getOrCreate(Context, material.TextureLayer[i], material.UseMipMaps);
				if (images[i].sampler == VK_NULL_HANDLE)
					images[i].sampler = texture->getSampler();

				writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[i].dstSet = set;
				writes[i].dstBinding = i;
				writes[i].descriptorCount = 1;
				writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				writes[i].pImageInfo = &images[i];
			}
			vk::UpdateDescriptorSets(Context.Device, VulkanMaterialTextureBindingCount, writes, 0, nullptr);

			vk::CmdBindDescriptorSets(frame.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
				layout, VulkanMaterialTextureSet, 1, &set, 0, nullptr);
			return true;
		}

		// Per-draw upload of a user shader's uniform blocks, the counterpart of the D3D12 driver's
		// CBV table loop: OnSetConstants() has just written into the CPU scratch mirrors, and each
		// one is copied into the frame's ring here, one descriptor set per used set.
		bool CVulkanDriver::bindUserMaterialDescriptors(CVulkanUserMaterial& shader,
			const SMaterial& material, const SVulkanUserLayout& layout)
		{
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];

			for (u32 set = 0; set < MaxUserDescriptorSets; ++set)
			{
				if (!(shader.UsedSetMask & (1u << set)))
					continue;

				VkDescriptorSet descriptorSet = allocateDescriptorSet(layout.SetLayouts[set]);
				if (descriptorSet == VK_NULL_HANDLE)
					return false;

				// Reserved up front: the write structs hold pointers into these two, so a
				// reallocation partway through would leave the earlier writes dangling.
				const size_t bindingCount = shader.DescriptorBindings.size();
				std::vector<VkDescriptorBufferInfo> buffers;
				std::vector<VkDescriptorImageInfo> images;
				std::vector<VkWriteDescriptorSet> writes;
				buffers.reserve(bindingCount);
				images.reserve(bindingCount);
				writes.reserve(bindingCount);

				// Same reason as bindDriverUniforms(): every uniform binding in this set has to end
				// up in the ring that the writes below name, so the whole set's worth is reserved
				// before the first allocateUniform().
				const VkDeviceSize alignment = Context.DeviceProperties.limits.minUniformBufferOffsetAlignment;
				VkDeviceSize uniformBytes = 0;
				for (size_t i = 0; i < bindingCount; ++i)
				{
					const SVulkanUserDescriptorBinding& binding = shader.DescriptorBindings[i];
					if (binding.Set != set)
						continue;
					if (binding.Type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
						binding.Type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
						continue;

					// 16 is the padding block an unreflected binding falls back to below.
					size_t blockBytes = 16;
					for (u32 stage = 0; stage < EVUS_COUNT; ++stage)
					{
						const std::vector<SVulkanUserUniformBlock>& blocks = shader.Blocks[stage];
						for (size_t b = 0; b < blocks.size(); ++b)
							if (blocks[b].Set == set && blocks[b].Binding == binding.Binding &&
								blocks[b].Scratch.size() > blockBytes)
								blockBytes = blocks[b].Scratch.size();
					}
					uniformBytes += blockBytes + alignment;
				}
				if (uniformBytes && !reserveUniforms(uniformBytes))
					return false;

				for (size_t i = 0; i < bindingCount; ++i)
				{
					const SVulkanUserDescriptorBinding& binding = shader.DescriptorBindings[i];
					if (binding.Set != set)
						continue;

					VkWriteDescriptorSet write = {};
					write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					write.dstSet = descriptorSet;
					write.dstBinding = binding.Binding;
					write.descriptorCount = 1;
					write.descriptorType = binding.Type;

					if (binding.Type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
						binding.Type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
					{
						// One Vulkan descriptor, but one scratch mirror per stage that declared the
						// block: the mirrors are kept in step by the material's own setters, so the
						// first one found is as good as any.
						const SVulkanUserUniformBlock* block = nullptr;
						for (u32 stage = 0; stage < EVUS_COUNT && !block; ++stage)
						{
							const std::vector<SVulkanUserUniformBlock>& blocks = shader.Blocks[stage];
							for (size_t b = 0; b < blocks.size(); ++b)
								if (blocks[b].Set == set && blocks[b].Binding == binding.Binding)
								{
									block = &blocks[b];
									break;
								}
						}

						// A binding with no reflected block, or an empty one, still has to point at
						// real memory: a descriptor the shader reads and nobody wrote is undefined.
						const u8 padding[16] = {};
						const void* data = padding;
						size_t sizeBytes = sizeof(padding);
						if (block && block->Size && block->Scratch.size())
						{
							data = block->Scratch.data();
							sizeBytes = block->Scratch.size();
						}

						const VkDeviceSize offset = allocateUniform(data, sizeBytes);
						if (offset == VK_WHOLE_SIZE)
							return false;

						VkDescriptorBufferInfo info = {};
						info.buffer = frame.UniformRing;
						info.offset = offset;
						info.range = sizeBytes;
						buffers.push_back(info);
						write.pBufferInfo = &buffers.back();
					}
					else if (binding.Type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
					{
						// Binding number indexes SMaterial::TextureLayer, the same convention the
						// built-in set 0 uses; NullTexture keeps the set complete either way.
						CVulkanTexture* texture = static_cast<CVulkanTexture*>(material.getTexture(binding.Binding));
						if (texture && texture->getImageLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
							texture = nullptr; // see bindMaterialTextures(), already warned there
						if (!texture || texture->getImageView() == VK_NULL_HANDLE)
							texture = NullTexture;

						VkDescriptorImageInfo info = {};
						info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
						info.imageView = texture->getImageView();
						info.sampler = binding.Binding < MATERIAL_MAX_TEXTURES ?
							Samplers.getOrCreate(Context, material.TextureLayer[binding.Binding], material.UseMipMaps) :
							VK_NULL_HANDLE;
						if (info.sampler == VK_NULL_HANDLE)
							info.sampler = texture->getSampler();
						images.push_back(info);
						write.pImageInfo = &images.back();
					}
					else if (binding.Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
					{
						// Pixel-stage UAV slot s is binding 16 + s (the compiler's u-register shift).
						const u32 slot = binding.Binding >= 16 ? binding.Binding - 16 : ~0u;
						CVulkanHardwareBuffer* hw = (slot < MaxPixelUAVSlots && PixelUAV[slot].Buffer) ?
							prepareComputeBuffer(PixelUAV[slot].Buffer) : nullptr;
						if (!hw && !ensureNullStorageBuffer())
							continue;
						VkDescriptorBufferInfo info = {};
						info.buffer = hw ? hw->getBuffer() : NullStorageBuffer;
						info.offset = 0;
						info.range = hw ? hw->getSize() : NullStorageBufferSize;
						buffers.push_back(info);
						write.pBufferInfo = &buffers.back();
					}
					else if (binding.Type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
					{
						const u32 slot = binding.Binding >= 16 ? binding.Binding - 16 : ~0u;
						CVulkanTexture* texture = (slot < MaxPixelUAVSlots) ?
							static_cast<CVulkanTexture*>(PixelUAV[slot].Texture) : nullptr;
						// bindPixelShaderTexture() put it in GENERAL; anything else cannot be written
						// inside the rendering instance, so the binding is left unwritten (warned).
						if (!texture || texture->getImageLayout() != VK_IMAGE_LAYOUT_GENERAL ||
							texture->getImageView() == VK_NULL_HANDLE)
						{
							if (!WarnedUserDescriptorType)
							{
								os::Printer::log("CVulkanDriver: a user shader declares a storage image with no "
									"bindPixelShaderTexture() bound to its slot, left unwritten: ",
									binding.Name.c_str(), ELL_WARNING);
								WarnedUserDescriptorType = true;
							}
							continue;
						}
						VkDescriptorImageInfo info = {};
						info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
						info.imageView = texture->getImageView();
						images.push_back(info);
						write.pImageInfo = &images.back();
					}
					else
					{
						// Anything else has no driver-side source to fill it from; the set layout
						// still declares it, so the pipeline is valid.
						if (!WarnedUserDescriptorType)
						{
							os::Printer::log("CVulkanDriver: a user shader declares a descriptor type "
								"this driver does not fill, it is left unwritten: ",
								binding.Name.c_str(), ELL_WARNING);
							WarnedUserDescriptorType = true;
						}
						continue;
					}

					writes.push_back(write);
				}

				if (!writes.empty())
					vk::UpdateDescriptorSets(Context.Device, (u32)writes.size(), writes.data(), 0, nullptr);

				vk::CmdBindDescriptorSets(frame.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
					layout.PipelineLayout, set, 1, &descriptorSet, 0, nullptr);
			}
			return true;
		}

		bool CVulkanDriver::bindDrawState(const SMaterial& material, const core::matrix4& world,
			const core::matrix4& view, const core::matrix4& proj, IVertexDescriptor* descriptor,
			const SVulkanVertexInputState& vertexInput, VkPrimitiveTopology topology,
			const SVulkanStencilOverride* stencil)
		{
			// Outside beginScene()/endScene() the command buffer is not recording and swallows
			// nothing: refuse, and say so once, the way the D3D12 driver does.
			if (!SceneOpen || !RenderingActive)
			{
				if (!WarnedDrawOutsideScene)
				{
					os::Printer::log("CVulkanDriver: draw outside beginScene()/endScene() -- ignored."
						" A Vulkan command buffer that is not recording accepts nothing.", ELL_WARNING);
					WarnedDrawOutsideScene = true;
				}
				return false;
			}
			// An unmapped topology or a layout with no attribute would both make pipeline creation
			// fail; refuse quietly, the caller already logged what it could not translate.
			if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM || vertexInput.empty())
				return false;

			// Resolved once and handed to both the key and the cache: the pipeline the cache builds
			// has to be the one the key describes.
			SVulkanDrawProgram program;
			resolveDrawProgram(material, descriptor, program);
			if (program.Layout == VK_NULL_HANDLE)
				return false;

			const SVulkanPipelineKey key = buildPipelineKeyFromMaterial(material, program,
				vertexInput, topology, stencil);

			VkPipeline pipeline = PipelineCache.getOrCreate(Context, key, program.Vertex,
				program.Fragment, vertexInput.get(), program.Layout, program.Geometry,
				program.TessControl, program.TessEval, 3, &program.EntryPoints);
			if (pipeline == VK_NULL_HANDLE)
				return false;

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			vk::CmdBindPipeline(frame.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

			// The active material, exactly as on the D3D12 side: every IMaterialRendererServices
			// method reads ActiveMaterialRendererIndex, and OnSetConstants() below is the only place
			// they are meant to be called from. A built-in type resolves to a null user material, so
			// the callback block is simply skipped for it.
			ActiveMaterialRendererIndex = -1;
			if (material.MaterialType >= 0 &&
				static_cast<u32>(material.MaterialType) < getMaterialRendererCount())
				ActiveMaterialRendererIndex = material.MaterialType;

			CVulkanUserMaterial* shader = getUserMaterial(ActiveMaterialRendererIndex);
			if (shader && shader->CallBack)
			{
				shader->CallBack->OnSetMaterial(material);
				shader->CallBack->OnSetConstants(this, shader->UserData);
			}

			if (!bindDriverUniforms(material, world, view, proj, program.Layout))
				return false;

			// A user shader owns sets 0..3 outright, so its own descriptors go there instead of the
			// built-in two-texture set 0. The uploads must come after OnSetConstants() above: that
			// is what just filled the scratch mirrors.
			const bool bound = shader ?
				bindUserMaterialDescriptors(*shader, material, ResourceOwner->UserLayouts[ActiveMaterialRendererIndex]) :
				bindMaterialTextures(material, program.Layout);

			// Last, once nothing can fail any more: the draw site that follows ends it, and a begin
			// without its end would leave transform feedback active into the next pipeline bind.
			if (bound)
				beginTransformFeedbackForDraw();
			return bound;
		}

		void CVulkanDriver::beginTransformFeedbackForDraw()
		{
			if (!StreamOutputTarget || !Context.HasTransformFeedback || TransformFeedbackActive)
				return;

			VkBuffer buffer = StreamOutputTarget->getBuffer();
			VkBuffer counter = StreamOutputTarget->getCounterBuffer(true);
			if (buffer == VK_NULL_HANDLE || counter == VK_NULL_HANDLE)
				return;

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			const VkDeviceSize offset = 0;
			const VkDeviceSize size = VK_WHOLE_SIZE;
			vk::CmdBindTransformFeedbackBuffersEXT(frame.CommandBuffer, 0, 1, &buffer, &offset, &size);
			// The counter buffer is the D3D BufferFilledSize: begin resumes at the byte count the
			// previous draw ended on, so draws between two setStreamOutputBuffer() calls append.
			vk::CmdBeginTransformFeedbackEXT(frame.CommandBuffer, 0, 1, &counter, &offset);
			TransformFeedbackActive = true;
		}

		void CVulkanDriver::endTransformFeedbackForDraw()
		{
			if (!TransformFeedbackActive)
				return;
			VkBuffer counter = StreamOutputTarget ? StreamOutputTarget->getCounterBuffer(false) : VK_NULL_HANDLE;
			const VkDeviceSize offset = 0;
			vk::CmdEndTransformFeedbackEXT(Frames[CurrentFrameIndex].CommandBuffer, 0,
				counter ? 1 : 0, counter ? &counter : nullptr, counter ? &offset : nullptr);
			TransformFeedbackActive = false;
		}

		void CVulkanDriver::releaseStreamOutputTarget()
		{
			if (!StreamOutputTarget)
				return;
			endTransformFeedbackForDraw(); // never active between two draws; defensive

			if (SceneOpen && Context.HasTransformFeedback)
			{
				// What transform feedback wrote becomes vertex data, and the byte count an indirect
				// argument (or a host read). Buffer barriers are not allowed inside a dynamic
				// rendering instance, hence the suspend/resume around them.
				suspendRendering();
				VkBufferMemoryBarrier barriers[2] = {};
				barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
				barriers[0].srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
				barriers[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
					VK_ACCESS_SHADER_READ_BIT;
				barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barriers[0].buffer = StreamOutputTarget->getBuffer();
				barriers[0].size = VK_WHOLE_SIZE;
				barriers[1] = barriers[0];
				barriers[1].srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
				barriers[1].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT |
					VK_ACCESS_TRANSFER_READ_BIT;
				barriers[1].buffer = StreamOutputTarget->getCounterBuffer(false);
				vk::CmdPipelineBarrier(Frames[CurrentFrameIndex].CommandBuffer,
					VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
					VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
					VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
					0, 0, nullptr, barriers[1].buffer ? 2 : 1, barriers, 0, nullptr);
				resumeRendering();
			}

			// From here on a draw of this buffer takes the byte-count form, see drawMeshBuffer().
			StreamOutputTarget->setStreamOutputCaptured(true);
			StreamOutputTarget = nullptr;
			StreamOutputHold.reset();
		}

		bool CVulkanDriver::setStreamOutputBuffer(scene::IVertexBuffer* buffer)
		{
			if (!Context.HasTransformFeedback)
			{
				if (!WarnedNoTransformFeedback)
				{
					os::Printer::log("CVulkanDriver::setStreamOutputBuffer: VK_EXT_transform_feedback is "
						"not available on this device, stream output is disabled", ELL_WARNING);
					WarnedNoTransformFeedback = true;
				}
				return false;
			}

			// Whether replacing or merely unbinding, the previous target has to become readable.
			releaseStreamOutputTarget();
			if (!buffer)
				return true;

			auto hardware = buffer->getHardwareBuffer();
			if (!hardware || hardware->getDriverType() != EDT_VULKAN)
			{
				createHardwareBuffer(buffer);
				hardware = buffer->getHardwareBuffer();
			}
			if (!hardware || hardware->getDriverType() != EDT_VULKAN)
			{
				os::Printer::log("CVulkanDriver::setStreamOutputBuffer: the buffer has no Vulkan device "
					"buffer", ELL_ERROR);
				return false;
			}
			if (hardware->getType() != EHBT_STREAM_OUTPUT)
			{
				os::Printer::log("CVulkanDriver::setStreamOutputBuffer: the target must be declared with "
					"setBufferType(scene::EBT_STREAM) before its first use, as on the D3D drivers", ELL_ERROR);
				return false;
			}

			CVulkanHardwareBuffer* native = static_cast<CVulkanHardwareBuffer*>(hardware.get());
			VkBuffer counter = native->getCounterBuffer(true);
			if (native->getBuffer() == VK_NULL_HANDLE || counter == VK_NULL_HANDLE)
				return false;

			// The byte count starts over at zero -- SOSetTargets with a zero offset. On the GPU
			// timeline while a scene is open, since a draw recorded earlier this frame may still be
			// reading the old count; from the host otherwise.
			if (SceneOpen)
			{
				suspendRendering();
				VkCommandBuffer commandBuffer = Frames[CurrentFrameIndex].CommandBuffer;
				vk::CmdFillBuffer(commandBuffer, counter, 0, 4, 0);

				VkBufferMemoryBarrier barriers[2] = {};
				barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
				barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
				barriers[0].dstAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |
					VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT;
				barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barriers[0].buffer = counter;
				barriers[0].size = VK_WHOLE_SIZE;
				barriers[1] = barriers[0];
				barriers[1].srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
				barriers[1].dstAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT;
				barriers[1].buffer = native->getBuffer();
				vk::CmdPipelineBarrier(commandBuffer,
					VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
					VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, 0, 0, nullptr, 2, barriers, 0, nullptr);
				resumeRendering();
			}
			else
				native->setCounterValue(0);

			native->setStreamOutputCaptured(false);
			StreamOutputTarget = native;
			StreamOutputHold = hardware;
			return true;
		}

		void CVulkanDriver::drawMeshBuffer(const scene::IMeshBuffer* mb)
		{
			if (!mb || mb->getVertexBufferCount() == 0)
				return;

			// One binding per stream, indices matching the buffer indices the vertex descriptor
			// assigned. The floor Vulkan guarantees for maxVertexInputBindings is 16.
			const u32 maxStreams = Context.DeviceProperties.limits.maxVertexInputBindings;
			const u32 vbCount = mb->getVertexBufferCount();
			if (vbCount > maxStreams || vbCount > kMaxVertexStreams)
			{
				os::Printer::log("CVulkanDriver::drawMeshBuffer: too many vertex buffers for this "
					"device", ELL_WARNING);
				return;
			}

			IVertexDescriptor* descriptor = mb->getVertexDescriptor();
			VkBuffer vertexBuffers[kMaxVertexStreams] = {};
			VkDeviceSize vertexOffsets[kMaxVertexStreams] = {};
			u32 drawVertexCount = 0;
			u32 instanceVertexCount = 0;
			// Set when stream 0 was filled by transform feedback: the draw then takes its vertex
			// count from the captured byte count, not from the IVertexBuffer (the D3D11 DrawAuto).
			VkBuffer byteCountBuffer = VK_NULL_HANDLE;
			u32 byteCountStride = 0;

			for (u32 i = 0; i < vbCount; ++i)
			{
				scene::IVertexBuffer* streamVb = mb->getVertexBuffer(i);
				if (!streamVb)
					return;

				auto streamHardware = streamVb->getHardwareBuffer();
				if (streamHardware && streamHardware->getDriverType() == EDT_VULKAN &&
					streamHardware->isRequiredUpdate() && streamVb->getVertexCount() > 0)
					streamHardware->update(streamVb->getHardwareMappingHint(),
						streamVb->getVertexCount() * streamVb->getVertexSize(), streamVb->getVertices());

				const bool captured = i == 0 && Context.HasTransformFeedback && streamHardware &&
					streamHardware->getDriverType() == EDT_VULKAN &&
					static_cast<CVulkanHardwareBuffer*>(streamHardware.get())->hasStreamOutputCount();
				if (streamVb->getVertexCount() == 0 && !captured)
					return;

				if (!streamHardware || streamHardware->getDriverType() != EDT_VULKAN)
					streamHardware = createHardwareBuffer(streamVb);
				if (!streamHardware)
					return;

				CVulkanHardwareBuffer* nativeVb = static_cast<CVulkanHardwareBuffer*>(streamHardware.get());
				vertexBuffers[i] = nativeVb->getBuffer();
				if (vertexBuffers[i] == VK_NULL_HANDLE)
					return;
				if (captured)
				{
					byteCountBuffer = nativeVb->getCounterBuffer(false);
					byteCountStride = streamVb->getVertexSize();
				}

				// A per-instance stream gives the instance count, a per-vertex one the vertex count,
				// the same split as the D3D11/D3D12 drivers make.
				if (descriptor && descriptor->getInstanceDataStepRate(i) == EIDSR_PER_INSTANCE)
					instanceVertexCount = streamVb->getVertexCount();
				else
					drawVertexCount = streamVb->getVertexCount();
			}

			const VkPrimitiveTopology topology = mapPrimitiveType(mb->getPrimitiveType());
			if (topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)
			{
				os::Printer::log("CVulkanDriver::drawMeshBuffer: unsupported E_PRIMITIVE_TYPE", ELL_WARNING);
				return;
			}

			// The descriptor's own layout, or the EVT_STANDARD fallback: resolved once and used both
			// for the pipeline key and for pipeline creation, so the two cannot drift. A user material
			// sees the vertex colour in D3D byte order, see resolveVulkanVertexInputState().
			SVulkanVertexInputState vertexInput;
			resolveVulkanVertexInputState(descriptor, vertexInput, getUserMaterial(Material.MaterialType) != nullptr);

			if (!bindDrawState(Material, Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION],
				descriptor, vertexInput, topology))
				return;

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			vk::CmdBindVertexBuffers(frame.CommandBuffer, 0, vbCount, vertexBuffers, vertexOffsets);

			// No per-instance stream means a plain draw; Vulkan has only the instanced form, so the
			// count is forced to one rather than taking a different entry point.
			const u32 instanceCount = instanceVertexCount ? instanceVertexCount : 1;

			// A stream-output capture is drawn from its byte count: transform feedback writes
			// non-indexed vertices, so the index buffer, if any, is left aside.
			scene::IIndexBuffer* ib = mb->getIndexBuffer();
			if (byteCountBuffer != VK_NULL_HANDLE && byteCountStride)
			{
				vk::CmdDrawIndirectByteCountEXT(frame.CommandBuffer, instanceCount, 0, byteCountBuffer, 0, 0,
					byteCountStride);
				endTransformFeedbackForDraw();
				return;
			}

			// Indexed when the mesh has an index buffer, otherwise straight from the per-vertex
			// stream -- the same fork as every other backend takes.
			if (ib && ib->getIndexCount() > 0)
			{
				auto ibHardware = ib->getHardwareBuffer();
				if (!ibHardware || ibHardware->getDriverType() != EDT_VULKAN)
					ibHardware = createHardwareBuffer(ib);
				else if (ibHardware->isRequiredUpdate())
				{
					const u32 indexSize = (ib->getType() == EIT_32BIT) ? 4 : 2;
					ibHardware->update(ib->getHardwareMappingHint(),
						ib->getIndexCount() * indexSize, ib->getIndices());
				}
				if (!ibHardware)
					return;

				CVulkanHardwareBuffer* nativeIb = static_cast<CVulkanHardwareBuffer*>(ibHardware.get());
				if (nativeIb->getBuffer() == VK_NULL_HANDLE)
					return;
				vk::CmdBindIndexBuffer(frame.CommandBuffer, nativeIb->getBuffer(), 0,
					(ib->getType() == EIT_32BIT) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
				vk::CmdDrawIndexed(frame.CommandBuffer, ib->getIndexCount(), instanceCount, 0, 0, 0);
			}
			else
			{
				vk::CmdDraw(frame.CommandBuffer, drawVertexCount, instanceCount, 0, 0);
			}
			endTransformFeedbackForDraw();
		}

		void CVulkanDriver::drawImmediate(const S3DVertex* vertices, u32 vertexCount,
			VkPrimitiveTopology topology, const SMaterial& material, const core::matrix4& world,
			const core::matrix4& view, const core::matrix4& proj, const SVulkanStencilOverride* stencil)
		{
			if (!vertexCount || !SceneOpen || !RenderingActive)
				return;

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			const SVulkanTransientRange range =
				frame.Immediate->allocateVertices(vertices, vertexCount, sizeof(S3DVertex));
			if (!range.isValid())
				return;

			// No mesh buffer here, so no descriptor: the EVT_STANDARD layout built at construction.
			if (!bindDrawState(material, world, view, proj, nullptr, S3DVertexInput, topology, stencil))
				return;

			// The range carries its own VkBuffer: a ring that grew mid-frame is no longer the one
			// getBuffer() would return.
			vk::CmdBindVertexBuffers(frame.CommandBuffer, 0, 1, &range.Buffer, &range.Offset);
			vk::CmdDraw(frame.CommandBuffer, vertexCount, 1, 0, 0);
			endTransformFeedbackForDraw();
		}

		void CVulkanDriver::draw2DImmediate(VkPrimitiveTopology topology, const SMaterial& material,
			const core::rect<s32>* clip)
		{
			if (ImmediateVertices.size() == 0)
				return;

			setScissorFromClip(clip);
			// Identity world and view plus build2DProjection(): the builders emit screen pixels.
			drawImmediate(ImmediateVertices.pointer(), ImmediateVertices.size(), topology, material,
				core::IdentityMatrix, core::IdentityMatrix,
				CVulkanImmediateGeometry::build2DProjection(CurrentRenderTargetSize));
			if (clip)
				setScissorFromClip(nullptr);
		}

		// Depth off, no culling, unlit: the state every 2D primitive wants. EMT_SOLID and
		// EMT_TRANSPARENT_ALPHA_CHANNEL both draw with solid.frag, only the blend differs.
		SMaterial CVulkanDriver::build2DMaterial(bool alphaBlend, video::ITexture* texture) const
		{
			SMaterial m;
			m.Lighting = false;
			m.ZBuffer = ECFN_DISABLED;
			m.ZWriteEnable = false;
			m.BackfaceCulling = false;
			m.FrontfaceCulling = false;
			m.MaterialType = alphaBlend ? EMT_TRANSPARENT_ALPHA_CHANNEL : EMT_SOLID;
			m.setTexture(0, texture);
			return m;
		}

		void CVulkanDriver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos,
			const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect,
			SColor color, bool useAlphaChannelOfTexture)
		{
			if (!texture)
				return;

			const core::rect<s32> destRect(destPos.X, destPos.Y,
				destPos.X + sourceRect.getWidth(), destPos.Y + sourceRect.getHeight());
			// The builder clips geometrically and re-maps the UVs, so this is right with or without
			// the scissor set below.
			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build2DImageQuad(
				ImmediateVertices, destRect, sourceRect, texture->getSize(), color, clipRect);
			if (ImmediateVertices.size() == 0)
				return;

			const SMaterial m = build2DMaterial(useAlphaChannelOfTexture || color.getAlpha() < 255,
				const_cast<video::ITexture*>(texture));
			draw2DImmediate(topology, m, clipRect);
		}

		void CVulkanDriver::draw2DImage(const video::ITexture* texture, const core::rect<s32>& destRect,
			const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect,
			const video::SColor* const colors, bool useAlphaChannelOfTexture)
		{
			if (!texture)
				return;

			const SColor white(255, 255, 255, 255);
			const SColor cUL = colors ? colors[0] : white;
			const SColor cUR = colors ? colors[1] : white;
			const SColor cLL = colors ? colors[2] : white;
			const SColor cLR = colors ? colors[3] : white;

			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build2DImageQuad(
				ImmediateVertices, destRect, sourceRect, texture->getSize(), cUL, cUR, cLL, cLR, clipRect);

			const bool alphaBlend = useAlphaChannelOfTexture || cUL.getAlpha() < 255 ||
				cUR.getAlpha() < 255 || cLL.getAlpha() < 255 || cLR.getAlpha() < 255;
			const SMaterial m = build2DMaterial(alphaBlend, const_cast<video::ITexture*>(texture));
			draw2DImmediate(topology, m, clipRect);
		}

		// Both batches are one draw per image. Correct, not optimal -- gathering them into a single
		// vertex buffer is the obvious next step, and the same one the D3D12 backend has left open.
		void CVulkanDriver::draw2DImageBatch(const video::ITexture* texture,
			const core::position2d<s32>& pos, const core::array<core::rect<s32> >& sourceRects,
			const core::array<s32>& indices, s32 kerningWidth, const core::rect<s32>* clipRect,
			SColor color, bool useAlphaChannelOfTexture)
		{
			if (!texture)
				return;

			core::position2d<s32> targetPos = pos;
			for (u32 i = 0; i < indices.size(); ++i)
			{
				const s32 index = indices[i];
				if (index < 0 || static_cast<u32>(index) >= sourceRects.size())
					continue;
				const core::rect<s32>& source = sourceRects[index];
				draw2DImage(texture, targetPos, source, clipRect, color, useAlphaChannelOfTexture);
				targetPos.X += source.getWidth() + kerningWidth;
			}
		}

		void CVulkanDriver::draw2DImageBatch(const video::ITexture* texture,
			const core::array<core::position2d<s32> >& positions,
			const core::array<core::rect<s32> >& sourceRects, const core::rect<s32>* clipRect,
			SColor color, bool useAlphaChannelOfTexture)
		{
			if (!texture)
				return;

			const u32 count = positions.size() < sourceRects.size() ? positions.size() : sourceRects.size();
			for (u32 i = 0; i < count; ++i)
				draw2DImage(texture, positions[i], sourceRects[i], clipRect, color, useAlphaChannelOfTexture);
		}

		void CVulkanDriver::draw2DRectangle(SColor color, const core::rect<s32>& pos,
			const core::rect<s32>* clip)
		{
			// No texture: bindMaterialTextures() puts the white NullTexture on both layers, so the
			// fragment shader's texture * vertex colour reduces to the vertex colour.
			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build2DRectangle(
				ImmediateVertices, pos, color, color, color, color, clip);

			const SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			draw2DImmediate(topology, m, clip);
		}

		void CVulkanDriver::draw2DRectangle(const core::rect<s32>& pos, SColor colorLeftUp,
			SColor colorRightUp, SColor colorLeftDown, SColor colorRightDown, const core::rect<s32>* clip)
		{
			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build2DRectangle(
				ImmediateVertices, pos, colorLeftUp, colorRightUp, colorLeftDown, colorRightDown, clip);

			const bool alphaBlend = colorLeftUp.getAlpha() < 255 || colorRightUp.getAlpha() < 255 ||
				colorLeftDown.getAlpha() < 255 || colorRightDown.getAlpha() < 255;
			const SMaterial m = build2DMaterial(alphaBlend, nullptr);
			draw2DImmediate(topology, m, clip);
		}

		// A rectangle whose colour or clip array is shorter than `pos` falls back to opaque white and
		// to no clipping, rather than dropping the rectangle: same tolerance as the D3D12 batches.
		void CVulkanDriver::batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
			const irr::core::array<SColor>& color, const irr::core::array<core::rect<s32>>* clip)
		{
			for (u32 i = 0; i < pos.size(); ++i)
			{
				const SColor c = (i < color.size()) ? color[i] : SColor(255, 255, 255, 255);
				const core::rect<s32>* rect = (clip && i < clip->size()) ? &(*clip)[i] : nullptr;
				draw2DRectangle(c, pos[i], rect);
			}
		}

		void CVulkanDriver::batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
			irr::core::array<SColor>& colorLeftUp, irr::core::array<SColor>& colorRightUp,
			irr::core::array<SColor>& colorLeftDown, irr::core::array<SColor>& colorRightDown,
			const irr::core::array<core::rect<s32>>* clip)
		{
			const SColor white(255, 255, 255, 255);
			for (u32 i = 0; i < pos.size(); ++i)
			{
				const SColor cul = (i < colorLeftUp.size()) ? colorLeftUp[i] : white;
				const SColor cur = (i < colorRightUp.size()) ? colorRightUp[i] : white;
				const SColor cld = (i < colorLeftDown.size()) ? colorLeftDown[i] : white;
				const SColor crd = (i < colorRightDown.size()) ? colorRightDown[i] : white;
				const core::rect<s32>* rect = (clip && i < clip->size()) ? &(*clip)[i] : nullptr;
				draw2DRectangle(pos[i], cul, cur, cld, crd, rect);
			}
		}

		void CVulkanDriver::draw2DRectangleOutline(const core::recti& pos, SColor color)
		{
			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build2DRectangleOutline(
				ImmediateVertices, pos, color);

			const SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			draw2DImmediate(topology, m, nullptr);
		}

		void CVulkanDriver::draw2DLine(const core::position2d<s32>& start,
			const core::position2d<s32>& end, SColor color)
		{
			// Always an independent segment, never a strip, so the vertex stream matches the other
			// backends' -- see the line-list family in CVulkanImmediate.h.
			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build2DLine(
				ImmediateVertices, start, end, color);

			const SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			draw2DImmediate(topology, m, nullptr);
		}

		void CVulkanDriver::drawPixel(u32 x, u32 y, const SColor& color)
		{
			// A one-pixel rectangle, not a POINT_LIST: point size is a shader output in Vulkan and
			// the built-in vertex shaders do not write one. Same shape as the D3D12 driver's.
			const core::rect<s32> pixel((s32)x, (s32)y, (s32)x + 1, (s32)y + 1);
			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build2DRectangle(
				ImmediateVertices, pixel, color, color, color, color, nullptr);

			const SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			draw2DImmediate(topology, m, nullptr);
		}

		void CVulkanDriver::draw2DPolygon(core::position2d<s32> center, f32 radius,
			video::SColor color, s32 vertexCount)
		{
			if (vertexCount < 2)
				return;

			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build2DPolygon(
				ImmediateVertices, center, radius, color, vertexCount);

			const SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			draw2DImmediate(topology, m, nullptr);
		}

		void CVulkanDriver::draw2DVertexPrimitiveList(const void* vertices, u32 vertexCount,
			const void* indexList, u32 primitiveCount, E_VERTEX_TYPE vType,
			scene::E_PRIMITIVE_TYPE pType, E_INDEX_TYPE iType)
		{
			if (!vertices || !vertexCount || !primitiveCount)
				return;
			if (vType != EVT_STANDARD)
			{
				os::Printer::log("CVulkanDriver::draw2DVertexPrimitiveList: only EVT_STANDARD is "
					"supported", ELL_WARNING);
				return;
			}

			// How many vertices the requested primitive count spans; the immediate ring is
			// non-indexed, so an index list is resolved into that stream below.
			VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
			u32 verticesToDraw = 0;
			switch (pType)
			{
			case scene::EPT_POINTS:
				topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; verticesToDraw = primitiveCount; break;
			case scene::EPT_LINE_STRIP:
				topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; verticesToDraw = primitiveCount + 1; break;
			case scene::EPT_LINES:
				topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; verticesToDraw = primitiveCount * 2; break;
			case scene::EPT_TRIANGLE_STRIP:
				topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; verticesToDraw = primitiveCount + 2; break;
			case scene::EPT_TRIANGLES:
				topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; verticesToDraw = primitiveCount * 3; break;
			default:
				os::Printer::log("CVulkanDriver::draw2DVertexPrimitiveList: unsupported "
					"E_PRIMITIVE_TYPE (POINTS/LINES/LINE_STRIP/TRIANGLES/TRIANGLE_STRIP only)",
					ELL_WARNING);
				return;
			}

			const S3DVertex* source = static_cast<const S3DVertex*>(vertices);
			ImmediateVertices.set_used(0);

			if (indexList)
			{
				ImmediateVertices.reallocate(verticesToDraw);
				for (u32 i = 0; i < verticesToDraw; ++i)
				{
					const u32 index = (iType == EIT_32BIT) ?
						static_cast<const u32*>(indexList)[i] : static_cast<const u16*>(indexList)[i];
					if (index >= vertexCount)
						return; // an out-of-range index would read past the caller's array
					ImmediateVertices.push_back(source[index]);
				}
			}
			else
			{
				// Without an index list the primitive count is only as good as the array behind it.
				if (verticesToDraw > vertexCount)
					verticesToDraw = vertexCount;
				ImmediateVertices.reallocate(verticesToDraw);
				for (u32 i = 0; i < verticesToDraw; ++i)
					ImmediateVertices.push_back(source[i]);
			}

			// Textured from the current material's layer 0, as on D3D12: this entry point carries no
			// texture of its own and 2D callers set one through setMaterial().
			const SMaterial m = build2DMaterial(false, Material.getTexture(0));
			draw2DImmediate(topology, m, nullptr);
		}

		// --- 3D immediate primitives. All of them draw with the CURRENT material and transforms, the
		// contract IVideoDriver.h documents: the caller sets those before calling.

		void CVulkanDriver::draw3DLine(const core::vector3df& start, const core::vector3df& end,
			SColor color)
		{
			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build3DLine(
				ImmediateVertices, start, end, color);
			drawImmediate(ImmediateVertices.pointer(), ImmediateVertices.size(), topology, Material,
				Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION]);
		}

		void CVulkanDriver::draw3DTriangle(const core::triangle3df& triangle, SColor color)
		{
			// No builder for this one: three vertices in world space, nothing to clip or re-map.
			const S3DVertex vertices[3] =
			{
				S3DVertex(triangle.pointA.X, triangle.pointA.Y, triangle.pointA.Z, 0, 0, 0, color, 0, 0),
				S3DVertex(triangle.pointB.X, triangle.pointB.Y, triangle.pointB.Z, 0, 0, 0, color, 0, 0),
				S3DVertex(triangle.pointC.X, triangle.pointC.Y, triangle.pointC.Z, 0, 0, 0, color, 0, 0)
			};
			drawImmediate(vertices, 3, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, Material,
				Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION]);
		}

		void CVulkanDriver::draw3DBox(const core::aabbox3d<f32>& box, SColor color)
		{
			const VkPrimitiveTopology topology = CVulkanImmediateGeometry::build3DBox(
				ImmediateVertices, box, color);
			drawImmediate(ImmediateVertices.pointer(), ImmediateVertices.size(), topology, Material,
				Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION]);
		}

		void CVulkanDriver::drawMeshBufferNormals(const scene::IMeshBuffer* mb, f32 length, SColor color)
		{
			if (!mb || mb->getVertexBufferCount() == 0)
				return;
			scene::IVertexBuffer* vb = mb->getVertexBuffer(0);
			if (!vb || vb->getVertexCount() == 0)
				return;

			// Same reduced scope as the D3D12 backend's: stream 0 is read as S3DVertex.
			const S3DVertex* source = static_cast<const S3DVertex*>(vb->getVertices());
			ImmediateVertices.set_used(0);
			ImmediateVertices.reallocate(vb->getVertexCount() * 2);
			for (u32 i = 0; i < vb->getVertexCount(); ++i)
			{
				const core::vector3df& p = source[i].Pos;
				const core::vector3df n = p + source[i].Normal * length;
				ImmediateVertices.push_back(S3DVertex(p.X, p.Y, p.Z, 0, 0, 0, color, 0, 0));
				ImmediateVertices.push_back(S3DVertex(n.X, n.Y, n.Z, 0, 0, 0, color, 0, 0));
			}

			SMaterial m; // depth still tested normally; only lighting and textures are irrelevant
			m.Lighting = false;
			m.setTexture(0, nullptr);
			drawImmediate(ImmediateVertices.pointer(), ImmediateVertices.size(),
				VK_PRIMITIVE_TOPOLOGY_LINE_LIST, m, Matrices[ETS_WORLD], Matrices[ETS_VIEW],
				Matrices[ETS_PROJECTION]);
		}

		ITexture* CVulkanDriver::createDeviceDependentTexture(IImage* surface, const io::path& name,
			void* mipmapData)
		{
			if (!surface)
				return nullptr;

			CVulkanTexture* texture = new CVulkanTexture(Context, *this, surface, TextureCreationFlags, name);
			if (!texture->hasDeviceResource())
			{
				os::Printer::log("CVulkanDriver: could not create the Vulkan image", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}
			return texture;
		}

		// Both overloads keep the buffer on the source object, so a mesh drawn every frame uploads
		// once instead of rebuilding its VkBuffer per draw.
		std::shared_ptr<video::IHardwareBuffer> CVulkanDriver::createHardwareBuffer(scene::IIndexBuffer* indexBuffer)
		{
			if (!indexBuffer)
				return nullptr;

			auto existing = indexBuffer->getHardwareBuffer();
			if (existing && existing->getDriverType() == EDT_VULKAN && !existing->isRequiredUpdate())
				return existing;

			// Against the owner's context and upload path: a buffer created while a deferred
			// context recorded a mesh has to outlive that context.
			auto buffer = std::make_shared<CVulkanHardwareBuffer>(ResourceOwner->Context, *ResourceOwner, indexBuffer);
			indexBuffer->setHardwareBuffer(buffer);
			return buffer;
		}

		std::shared_ptr<video::IHardwareBuffer> CVulkanDriver::createHardwareBuffer(scene::IVertexBuffer* vertexBuffer)
		{
			if (!vertexBuffer)
				return nullptr;

			auto existing = vertexBuffer->getHardwareBuffer();
			if (existing && existing->getDriverType() == EDT_VULKAN && !existing->isRequiredUpdate())
				return existing;

			auto buffer = std::make_shared<CVulkanHardwareBuffer>(ResourceOwner->Context, *ResourceOwner, vertexBuffer);
			vertexBuffer->setHardwareBuffer(buffer);
			return buffer;
		}

		ITexture* CVulkanDriver::addRenderTargetTexture(const core::dimension2d<u32>& size,
			const io::path& name, const ECOLOR_FORMAT format)
		{
			const ECOLOR_FORMAT actual = (format == ECF_UNKNOWN) ? ECF_A8R8G8B8 : format;

			CVulkanTexture* texture = new CVulkanTexture(Context, *this, size, actual, true, name);
			if (!texture->hasDeviceResource())
			{
				os::Printer::log("CVulkanDriver::addRenderTargetTexture: image creation failed", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}

			// Same contract as CNullDriver::addTexture(): the cache grabs, we give ours back.
			CNullDriver::addTexture(texture);
			texture->drop();
			return texture;
		}

		void CVulkanDriver::unbindRenderTarget()
		{
			if (!RenderTargetActive)
				return;
			// Colour attachments go straight to the sampled layout: what was rendered is almost
			// always the next pass' input.
			RenderTarget->unbind(Frames[CurrentFrameIndex].CommandBuffer);
			RenderTargetActive = false;
			RenderTarget->reset();
		}

		bool CVulkanDriver::activateRenderTarget(bool clearColor, bool clearDepth, SColor color)
		{
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];

			if (RenderTargetActive)
			{
				// bind() records layout transitions, which a dynamic rendering instance forbids.
				RenderTarget->bind(frame.CommandBuffer);
				CurrentRenderTargetSize = RenderTarget->getSize();
			}
			else
			{
				CurrentRenderTargetSize = core::dimension2d<u32>(SwapchainExtent.width, SwapchainExtent.height);
			}

			// A different target resets the viewport to the full surface, as on every other backend.
			ViewPort = core::rect<s32>(0, 0, (s32)CurrentRenderTargetSize.Width,
				(s32)CurrentRenderTargetSize.Height);
			beginRendering(clearColor, clearDepth, color);
			return true;
		}

		// Unlike D3D12's OMSetRenderTargets, changing target here means ending one dynamic rendering
		// instance and starting another: the attachments are described afresh each time, and the
		// layout transitions the change needs are illegal inside a running instance.
		bool CVulkanDriver::setRenderTarget(video::ITexture* texture, bool clearBackBuffer,
			bool clearZBufferFlag, SColor color, video::ITexture* depthStencil)
		{
			if (!SceneOpen)
			{
				os::Printer::log("CVulkanDriver::setRenderTarget: only valid between beginScene() "
					"and endScene()", ELL_WARNING);
				return false;
			}

			// Each target owns a dynamic rendering instance: close the running one first.
			endRendering();
			unbindRenderTarget();
			MrtBlend.reset(); // per-target overrides belong to an MRT set only

			if (!texture)
			{
				// Back to the swapchain image, which is still in COLOR_ATTACHMENT_OPTIMAL.
				return activateRenderTarget(clearBackBuffer, clearZBufferFlag, color);
			}

			if (texture->getDriverType() != EDT_VULKAN)
			{
				os::Printer::log("CVulkanDriver::setRenderTarget: texture is not a Vulkan one", ELL_ERROR);
				activateRenderTarget(false, false, color);
				return false;
			}

			CVulkanTexture* colorTexture = static_cast<CVulkanTexture*>(texture);
			// An explicit depth texture is honoured when it carries a depth format; otherwise the
			// pool supplies one sized to this target.
			CVulkanTexture* explicitDepth = static_cast<CVulkanTexture*>(depthStencil);
			if (explicitDepth && !CVulkanTexture::isDepthFormat(explicitDepth->getVkFormat()))
				explicitDepth = nullptr;

			if (!RenderTarget->setTarget(colorTexture, explicitDepth, DepthPool))
			{
				activateRenderTarget(false, false, color);
				return false;
			}

			RenderTargetActive = true;
			return activateRenderTarget(clearBackBuffer, clearZBufferFlag, color);
		}

		bool CVulkanDriver::setRenderTarget(const core::array<video::IRenderTarget>& targets,
			const core::array<bool>& clearBackBuffer, bool clearZBufferFlag, SColor color,
			video::ITexture* depthStencil)
		{
			// No target: same contract as the single-target overload, back to the swapchain.
			if (targets.empty())
				return setRenderTarget(static_cast<video::ITexture*>(nullptr), true, clearZBufferFlag,
					color, depthStencil);

			if (!SceneOpen)
			{
				os::Printer::log("CVulkanDriver::setRenderTarget: only valid between beginScene() "
					"and endScene()", ELL_WARNING);
				return false;
			}

			endRendering();
			unbindRenderTarget();

			CVulkanTexture* colorTextures[CVulkanRenderTarget::MaxColorAttachments] = {};
			u32 count = 0;
			for (u32 i = 0; i < targets.size() && count < CVulkanRenderTarget::MaxColorAttachments; ++i)
			{
				video::ITexture* entry = targets[i].RenderTexture;
				if (!entry || entry->getDriverType() != EDT_VULKAN)
				{
					os::Printer::log("CVulkanDriver::setRenderTarget: unusable MRT entry, truncated "
						"at that point", ELL_WARNING);
					break;
				}
				colorTextures[count++] = static_cast<CVulkanTexture*>(entry);
			}

			if (!count)
			{
				os::Printer::log("CVulkanDriver::setRenderTarget: no usable MRT target", ELL_ERROR);
				activateRenderTarget(false, false, color);
				return false;
			}

			CVulkanTexture* explicitDepth = static_cast<CVulkanTexture*>(depthStencil);
			if (explicitDepth && !CVulkanTexture::isDepthFormat(explicitDepth->getVkFormat()))
				explicitDepth = nullptr;

			if (!RenderTarget->setTargets(colorTextures, count, explicitDepth, DepthPool))
			{
				activateRenderTarget(false, false, color);
				return false;
			}

			// The per-target blend and colour mask of the IRenderTarget entries, slots 1 and up:
			// baked into the pipelines drawn while this set is bound (see buildPipelineKeyFromMaterial).
			// Slot 0 follows the material, as CD3D11Driver::setBasicRenderStates() rewrites it.
			// Opt-in (setPerTargetBlend()): by default every target follows the material, as on D3D11.
			MrtBlend.reset();
			const u32 bound = RenderTarget->getColorAttachmentCount();
			for (u32 i = 1; PerTargetBlend && i < bound && i < 8; ++i)
			{
				const video::IRenderTarget& target = targets[i];
				MrtBlend.Mask |= static_cast<u8>(1u << i);
				MrtBlend.Enable[i] = target.BlendOp != EBO_NONE;
				// The dual-source factors exist on attachment 0 only.
				auto slotFactor = [&](E_BLEND_FACTOR factor) -> VkBlendFactor
				{
					if (factor >= EBF_SRC1_COLOR)
					{
						if (!WarnedSrc1OnMrtSlot)
							os::Printer::log("CVulkanDriver::setRenderTarget: EBF_SRC1_* is only valid on "
								"render target 0, using EBF_ONE on the other slots", ELL_WARNING);
						WarnedSrc1OnMrtSlot = true;
						return VK_BLEND_FACTOR_ONE;
					}
					return getVulkanBlendFactor(factor, false);
				};
				MrtBlend.Src[i] = slotFactor(target.BlendFuncSrc);
				MrtBlend.Dst[i] = slotFactor(target.BlendFuncDst);
				MrtBlend.Write[i] =
					((target.ColorMask & ECP_RED) ? VK_COLOR_COMPONENT_R_BIT : 0) |
					((target.ColorMask & ECP_GREEN) ? VK_COLOR_COMPONENT_G_BIT : 0) |
					((target.ColorMask & ECP_BLUE) ? VK_COLOR_COMPONENT_B_BIT : 0) |
					((target.ColorMask & ECP_ALPHA) ? VK_COLOR_COMPONENT_A_BIT : 0);
			}
			if (MrtBlend.Mask && !Context.HasIndependentBlend && !WarnedNoIndependentBlend)
			{
				os::Printer::log("CVulkanDriver::setRenderTarget: the device has no independentBlend, "
					"every MRT target blends like the material", ELL_WARNING);
				WarnedNoIndependentBlend = true;
			}

			// One VkRenderingInfo, one loadOp per attachment kind: the first flag decides for all.
			const bool clearColor = clearBackBuffer.empty() ? true : clearBackBuffer[0];
			RenderTargetActive = true;
			return activateRenderTarget(clearColor, clearZBufferFlag, color);
		}

		IImage* CVulkanDriver::createScreenShot(video::ECOLOR_FORMAT format, video::E_RENDER_TARGET target)
		{
			if (target != video::ERT_FRAME_BUFFER)
			{
				os::Printer::log("CVulkanDriver::createScreenShot: only ERT_FRAME_BUFFER is supported",
					ELL_WARNING);
				return nullptr;
			}
			if (format == video::ECF_UNKNOWN)
				format = video::ECF_A8R8G8B8;
			if (format != video::ECF_A8R8G8B8)
			{
				os::Printer::log("CVulkanDriver::createScreenShot: only ECF_A8R8G8B8 is supported",
					ELL_WARNING);
				return nullptr;
			}
			if (SceneOpen || SwapchainImages.empty())
			{
				os::Printer::log("CVulkanDriver::createScreenShot: call this after endScene()", ELL_WARNING);
				return nullptr;
			}
			if (!SwapchainCanReadBack)
			{
				os::Printer::log("CVulkanDriver::createScreenShot: the surface does not allow "
					"TRANSFER_SRC on its images", ELL_WARNING);
				return nullptr;
			}
			// The memcpy below assumes B,G,R,A bytes: the default surface format and its sRGB twin
			// (ESCS_SRGB_LINEAR); a 16-bit float or 10-bit swapchain has no 8-bit image to hand out.
			if (SwapchainFormat != VK_FORMAT_B8G8R8A8_UNORM && SwapchainFormat != VK_FORMAT_B8G8R8A8_SRGB)
			{
				os::Printer::log("CVulkanDriver::createScreenShot: only an 8-bit BGRA swapchain can be read back", ELL_WARNING);
				return nullptr;
			}

			// Tightly packed: vkCmdCopyImageToBuffer with a zero bufferRowLength uses the image
			// width, so there is no D3D-style row-pitch alignment to work around.
			const u32 width = SwapchainExtent.width;
			const u32 height = SwapchainExtent.height;
			const VkDeviceSize sizeBytes = static_cast<VkDeviceSize>(width) * height * 4;

			VkBuffer readback = VK_NULL_HANDLE;
			VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
			if (!createVulkanBuffer(Context, sizeBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				readback, readbackMemory))
				return nullptr;

			VkImage image = SwapchainImages[CurrentImageIndex];
			VkCommandBuffer cb = beginUpload();
			if (cb == VK_NULL_HANDLE)
			{
				vk::DestroyBuffer(Context.Device, readback, nullptr);
				vk::FreeMemory(Context.Device, readbackMemory, nullptr);
				return nullptr;
			}

			// The image was left in PRESENT_SRC by endScene(); put it back there afterwards so the
			// next acquire finds it where the swapchain expects.
			transitionImageLayout(cb, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

			VkBufferImageCopy copy = {};
			copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.imageSubresource.layerCount = 1;
			copy.imageExtent.width = width;
			copy.imageExtent.height = height;
			copy.imageExtent.depth = 1;
			vk::CmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);

			transitionImageLayout(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
			endUploadAndWait(cb);

			void* mapped = nullptr;
			if (vulkanFailed("CVulkanDriver::createScreenShot: map readback",
				vk::MapMemory(Context.Device, readbackMemory, 0, sizeBytes, 0, &mapped)))
			{
				vk::DestroyBuffer(Context.Device, readback, nullptr);
				vk::FreeMemory(Context.Device, readbackMemory, nullptr);
				return nullptr;
			}

			IImage* result = new CImage(format, core::dimension2d<u32>(width, height));
			u8* dst = static_cast<u8*>(result->lock());
			const u8* src = static_cast<const u8*>(mapped);
			const u32 dstPitch = result->getPitch();
			// The swapchain is B8G8R8A8_UNORM, i.e. B,G,R,A in memory, which is exactly what a
			// 0xAARRGGBB ECF_A8R8G8B8 texel looks like: no channel swap, unlike the D3D12 backend.
			for (u32 y = 0; y < height; ++y)
				memcpy(dst + y * dstPitch, src + static_cast<size_t>(y) * width * 4, width * 4);
			result->unlock();

			vk::UnmapMemory(Context.Device, readbackMemory);
			vk::DestroyBuffer(Context.Device, readback, nullptr);
			vk::FreeMemory(Context.Device, readbackMemory, nullptr);
			return result;
		}

		// ============================ User shader materials ============================

		namespace
		{
			//! One stage's source held as raw bytes with a trailing NUL, rather than a core::stringc:
			//! an EGSL_PCMP blob is SPIR-V words, full of zero bytes, and a string would truncate it
			//! at the first one. Empty means "no shader for this stage", per IGPUProgrammingServices.
			typedef std::vector<c8> SVulkanSourceBytes;

			bool readShaderSource(io::IReadFile* file, SVulkanSourceBytes& out)
			{
				out.clear();
				if (!file)
					return true;

				const long size = file->getSize();
				if (size <= 0)
					return true;

				out.resize(static_cast<size_t>(size) + 1, 0);
				if (file->read(out.data(), static_cast<u32>(size)) != static_cast<s32>(size))
				{
					out.clear();
					return false;
				}
				return true;
			}

			bool loadShaderSource(io::IFileSystem* fs, const io::path& filename, SVulkanSourceBytes& out)
			{
				out.clear();
				if (!fs || filename.size() == 0)
					return true;

				io::IReadFile* file = fs->createAndOpenFile(filename);
				if (!file)
				{
					os::Printer::log("CVulkanDriver: could not open the shader file ",
						filename.c_str(), ELL_ERROR);
					return false;
				}
				const bool ok = readShaderSource(file, out);
				file->drop();
				return ok;
			}

			//! The buffer minus its terminator; 0 length would mean "measure it", which is wrong for
			//! a blob that legitimately contains zero bytes.
			void fillStage(SVulkanUserShaderStageSource& stage, const SVulkanSourceBytes& bytes,
				const c8* entryPoint)
			{
				if (bytes.size() < 2)
					return;
				stage.Source = bytes.data();
				stage.SourceLength = static_cast<u32>(bytes.size() - 1);
				stage.EntryPoint = entryPoint;
			}

			//! In-memory sources arrive as a bare pointer, so their length has to be measured -- which
			//! makes this path text-only: a pre-compiled SPIR-V blob must come through a file.
			void fillStage(SVulkanUserShaderStageSource& stage, const c8* source, const c8* entryPoint)
			{
				if (!source || !source[0])
					return;
				stage.Source = source;
				stage.SourceLength = 0;
				stage.EntryPoint = entryPoint;
			}
		}

		bool CVulkanDriver::buildUserMaterialLayout(const CVulkanUserMaterial& material,
			SVulkanUserLayout& out)
		{
			std::vector<VkDescriptorSetLayoutBinding> bindings;
			for (u32 set = 0; set < MaxUserDescriptorSets; ++set)
			{
				material.getDescriptorSetLayoutBindings(set, bindings);
				// A set the shader declares nothing in still needs a layout: a VkPipelineLayout is
				// indexed by set number and cannot skip one, exactly as sets 1..3 of the built-ins.
				out.SetLayouts[set] = bindings.empty() ? EmptySetLayout :
					LayoutCache.getOrCreateDescriptorSetLayout(Context, bindings.data(),
						static_cast<u32>(bindings.size()));
				if (out.SetLayouts[set] == VK_NULL_HANDLE)
					return false;
			}

			VkDescriptorSetLayout setLayouts[DriverDescriptorSet + 1];
			for (u32 set = 0; set < MaxUserDescriptorSets; ++set)
				setLayouts[set] = out.SetLayouts[set];
			// The driver's own uniforms stay where the built-in shaders have them: a user shader
			// reaching into that set was already rejected at reflection time.
			setLayouts[DriverDescriptorSet] = DriverUniformSetLayout;

			out.PipelineLayout = LayoutCache.getOrCreatePipelineLayout(Context, setLayouts,
				DriverDescriptorSet + 1);
			return out.PipelineLayout != VK_NULL_HANDLE;
		}

		// The single registration path every add*ShaderMaterial* overload funnels into, once its
		// sources are in memory: the material compiles and reflects itself, the driver only builds the
		// layouts that reflection implies and puts it in the registry.
		s32 CVulkanDriver::registerUserShaderMaterial(const SVulkanUserShaderStageSource* stages,
			E_GPU_SHADING_LANGUAGE lang, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, s32 userData, IVertexDescriptor* streamOutputLayout)
		{
			CVulkanUserMaterial* material = new CVulkanUserMaterial();

			// vertexTypeOut with a geometry stage: the D3D stream-output declaration, which here
			// becomes the transform feedback layout patched into that stage's SPIR-V. Without the
			// extension the material still compiles and simply rasterizes.
			if (streamOutputLayout && stages[EVUS_GEOMETRY].Source)
			{
				if (Context.HasTransformFeedback)
				{
					streamOutputLayout->grab();
					material->StreamOutputLayout = streamOutputLayout;
				}
				else if (!WarnedNoTransformFeedback)
				{
					os::Printer::log("CVulkanDriver: a stream-output material was registered but "
						"VK_EXT_transform_feedback is not available; it will rasterize instead", ELL_WARNING);
					WarnedNoTransformFeedback = true;
				}
			}

			if (!material->compileFromSource(Context, lang, stages))
			{
				os::Printer::log("CVulkanDriver::addHighLevelShaderMaterial: the material was not "
					"registered, see the compiler diagnostics above", ELL_ERROR);
				material->drop();
				return -1;
			}

			// Must come after the compilation (there is nothing to reflect before it) and before
			// registration: without a pipeline layout the material would be selectable but
			// undrawable, since every pipeline of its is created against that layout.
			SVulkanUserLayout layout;
			if (!buildUserMaterialLayout(*material, layout))
			{
				os::Printer::log("CVulkanDriver::addHighLevelShaderMaterial: no pipeline layout could "
					"be built for this material", ELL_ERROR);
				material->drop();
				return -1;
			}

			if (callback)
			{
				callback->grab();
				material->CallBack = callback;
			}
			material->UserData = userData;

			// A user shader has no blend state of its own and inherits its base type's, the same way
			// the D3D12 side does: without this every user material would draw opaque whatever base
			// material it was created with.
			material->BaseMaterialType = baseMaterial;
			if (const CVulkanMaterialRenderer* base = getNativeRenderer(baseMaterial))
			{
				material->BlendMode = base->BlendMode;
				material->CustomSrcColorFactor = base->CustomSrcColorFactor;
				material->CustomDstColorFactor = base->CustomDstColorFactor;
				material->CustomSrcAlphaFactor = base->CustomSrcAlphaFactor;
				material->CustomDstAlphaFactor = base->CustomDstAlphaFactor;
				material->CustomBlendOp = base->CustomBlendOp;
			}

			const s32 materialType = addMaterialRenderer(material, nullptr);
			material->drop(); // addMaterialRenderer() grabbed it
			if (materialType < 0)
				return -1;

			UserLayouts[materialType] = layout;
			return materialType;
		}

		s32 CVulkanDriver::addHighLevelShaderMaterial(
			const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, const c8* pixelShaderProgram,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			return addHighLevelShaderMaterial(vertexShaderProgram, vertexShaderEntryPointName,
				vsCompileTarget, pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
				geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
				nullptr, nullptr, EHST_HS_5_0, nullptr, nullptr, EDST_DS_5_0,
				inType, outType, verticesOut, callback, baseMaterial, vertexTypeOut, userData,
				shadingLang);
		}

		// The one body: the five other overloads reduce to this one. inType/outType/verticesOut are
		// D3D9-era geometry shader declarations with no Vulkan counterpart -- the SPIR-V module
		// carries its own -- and vertexTypeOut only describes a stream-output the driver has none of.
		s32 CVulkanDriver::addHighLevelShaderMaterial(
			const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, const c8* pixelShaderProgram,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			const c8* hullShaderProgram, const c8* hullShaderEntryPointName,
			E_HULL_SHADER_TYPE hsCompileTarget,
			const c8* domainShaderProgram, const c8* domainShaderEntryPointName,
			E_DOMAIN_SHADER_TYPE dsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			SVulkanUserShaderStageSource stages[EVUS_COUNT];
			fillStage(stages[EVUS_VERTEX], vertexShaderProgram, vertexShaderEntryPointName);
			fillStage(stages[EVUS_FRAGMENT], pixelShaderProgram, pixelShaderEntryPointName);
			fillStage(stages[EVUS_GEOMETRY], geometryShaderProgram, geometryShaderEntryPointName);
			fillStage(stages[EVUS_HULL], hullShaderProgram, hullShaderEntryPointName);
			fillStage(stages[EVUS_DOMAIN], domainShaderProgram, domainShaderEntryPointName);

			return registerUserShaderMaterial(stages, shadingLang, callback, baseMaterial, userData, vertexTypeOut);
		}

		s32 CVulkanDriver::addHighLevelShaderMaterialFromFiles(
			const io::path& vertexShaderProgramFileName, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, const io::path& pixelShaderProgramFileName,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const io::path& geometryShaderProgramFileName, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			return addHighLevelShaderMaterialFromFiles(vertexShaderProgramFileName,
				vertexShaderEntryPointName, vsCompileTarget, pixelShaderProgramFileName,
				pixelShaderEntryPointName, psCompileTarget, geometryShaderProgramFileName,
				geometryShaderEntryPointName, gsCompileTarget, "", nullptr, EHST_HS_5_0,
				"", nullptr, EDST_DS_5_0, inType, outType, verticesOut, callback, baseMaterial,
				vertexTypeOut, userData, shadingLang);
		}

		s32 CVulkanDriver::addHighLevelShaderMaterialFromFiles(
			const io::path& vertexShaderProgramFile, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, const io::path& pixelShaderProgramFile,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const io::path& geometryShaderProgramFileName, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			const io::path& hullShaderProgram, const c8* hullShaderEntryPointName,
			E_HULL_SHADER_TYPE hsCompileTarget,
			const io::path& domainShaderProgram, const c8* domainShaderEntryPointName,
			E_DOMAIN_SHADER_TYPE dsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			SVulkanSourceBytes vs, ps, gs, hs, ds;
			if (!loadShaderSource(FileSystem, vertexShaderProgramFile, vs) ||
				!loadShaderSource(FileSystem, pixelShaderProgramFile, ps) ||
				!loadShaderSource(FileSystem, geometryShaderProgramFileName, gs) ||
				!loadShaderSource(FileSystem, hullShaderProgram, hs) ||
				!loadShaderSource(FileSystem, domainShaderProgram, ds))
				return -1;

			SVulkanUserShaderStageSource stages[EVUS_COUNT];
			fillStage(stages[EVUS_VERTEX], vs, vertexShaderEntryPointName);
			fillStage(stages[EVUS_FRAGMENT], ps, pixelShaderEntryPointName);
			fillStage(stages[EVUS_GEOMETRY], gs, geometryShaderEntryPointName);
			fillStage(stages[EVUS_HULL], hs, hullShaderEntryPointName);
			fillStage(stages[EVUS_DOMAIN], ds, domainShaderEntryPointName);

			return registerUserShaderMaterial(stages, shadingLang, callback, baseMaterial, userData, vertexTypeOut);
		}

		s32 CVulkanDriver::addHighLevelShaderMaterialFromFiles(
			io::IReadFile* vertexShaderProgram, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, io::IReadFile* pixelShaderProgram,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			io::IReadFile* geometryShaderProgram, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			return addHighLevelShaderMaterialFromFiles(vertexShaderProgram, vertexShaderEntryPointName,
				vsCompileTarget, pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
				geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
				nullptr, nullptr, EHST_HS_5_0, nullptr, nullptr, EDST_DS_5_0,
				inType, outType, verticesOut, callback, baseMaterial, vertexTypeOut, userData,
				shadingLang);
		}

		s32 CVulkanDriver::addHighLevelShaderMaterialFromFiles(
			io::IReadFile* vertexShaderProgram, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, io::IReadFile* pixelShaderProgram,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			io::IReadFile* geometryShaderProgram, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			io::IReadFile* hullShaderProgram, const c8* hullShaderEntryPointName,
			E_HULL_SHADER_TYPE hsCompileTarget,
			io::IReadFile* domainShaderProgram, const c8* domainShaderEntryPointName,
			E_DOMAIN_SHADER_TYPE dsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			SVulkanSourceBytes vs, ps, gs, hs, ds;
			if (!readShaderSource(vertexShaderProgram, vs) ||
				!readShaderSource(pixelShaderProgram, ps) ||
				!readShaderSource(geometryShaderProgram, gs) ||
				!readShaderSource(hullShaderProgram, hs) ||
				!readShaderSource(domainShaderProgram, ds))
				return -1;

			SVulkanUserShaderStageSource stages[EVUS_COUNT];
			fillStage(stages[EVUS_VERTEX], vs, vertexShaderEntryPointName);
			fillStage(stages[EVUS_FRAGMENT], ps, pixelShaderEntryPointName);
			fillStage(stages[EVUS_GEOMETRY], gs, geometryShaderEntryPointName);
			fillStage(stages[EVUS_HULL], hs, hullShaderEntryPointName);
			fillStage(stages[EVUS_DOMAIN], ds, domainShaderEntryPointName);

			return registerUserShaderMaterial(stages, shadingLang, callback, baseMaterial, userData, vertexTypeOut);
		}

		// The assembly-shader entry points. Vulkan consumes SPIR-V only, so there is no assembler to
		// run and the sources go to the high-level path in the default language: right for a
		// GLSL/HLSL pair, and anything else fails with the front end's own diagnostics rather than
		// silently returning -1.
		s32 CVulkanDriver::addShaderMaterial(const c8* vertexShaderProgram, const c8* pixelShaderProgram,
			IShaderConstantSetCallBack* callback, E_MATERIAL_TYPE baseMaterial, s32 userData)
		{
			return addHighLevelShaderMaterial(vertexShaderProgram, "main", EVST_VS_5_0,
				pixelShaderProgram, "main", EPST_PS_5_0, nullptr, "main", EGST_GS_4_0,
				scene::EPT_TRIANGLES, scene::EPT_TRIANGLE_STRIP, 0, callback, baseMaterial,
				nullptr, userData, EGSL_DEFAULT);
		}

		s32 CVulkanDriver::addShaderMaterialFromFiles(const io::path& vertexShaderProgramFileName,
			const io::path& pixelShaderProgramFileName, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, s32 userData)
		{
			return addHighLevelShaderMaterialFromFiles(vertexShaderProgramFileName, "main", EVST_VS_5_0,
				pixelShaderProgramFileName, "main", EPST_PS_5_0, "", "main", EGST_GS_4_0,
				scene::EPT_TRIANGLES, scene::EPT_TRIANGLE_STRIP, 0, callback, baseMaterial,
				nullptr, userData, EGSL_DEFAULT);
		}

		s32 CVulkanDriver::addShaderMaterialFromFiles(io::IReadFile* vertexShaderProgram,
			io::IReadFile* pixelShaderProgram, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, s32 userData)
		{
			return addHighLevelShaderMaterialFromFiles(vertexShaderProgram, "main", EVST_VS_5_0,
				pixelShaderProgram, "main", EPST_PS_5_0, (io::IReadFile*)nullptr, "main", EGST_GS_4_0,
				scene::EPT_TRIANGLES, scene::EPT_TRIANGLE_STRIP, 0, callback, baseMaterial,
				nullptr, userData, EGSL_DEFAULT);
		}

		// --- IMaterialRendererServices. ActiveMaterialRendererIndex is set by bindDrawState() around
		// the CallBack->OnSetConstants() call and is -1 everywhere else, so a call made outside that
		// window resolves to no material and fails cleanly. A built-in type resolves to none either:
		// only a user shader has reflection to look a name up in.

		namespace
		{
			bool setUserFloats(CVulkanUserMaterial* shader, s32 index, const f32* floats, int count,
				E_SHADER_TYPE stage)
			{
				return shader ? shader->setVariable(index, floats, count, stage) : false;
			}

			bool setUserInts(CVulkanUserMaterial* shader, s32 index, const s32* ints, int count,
				E_SHADER_TYPE stage)
			{
				return shader ? shader->setVariable(index, ints, count, stage) : false;
			}

			//! Wider-than-4-byte scalars: the typed setters above assume 4-byte elements, so a 64-bit
			//! type set through them would copy half its bytes.
			bool setUserRaw(CVulkanUserMaterial* shader, s32 index, const void* data, u32 byteCount,
				E_SHADER_TYPE stage)
			{
				return shader ? shader->setVariableRaw(index, data, byteCount, stage) : false;
			}
		}

		s32 CVulkanDriver::getVertexShaderConstantID(const c8* name)
		{
			CVulkanUserMaterial* shader = getUserMaterial(ActiveMaterialRendererIndex);
			return shader ? shader->getVariableID(name, EST_VERTEX_SHADER) : -1;
		}

		s32 CVulkanDriver::getPixelShaderConstantID(const c8* name)
		{
			CVulkanUserMaterial* shader = getUserMaterial(ActiveMaterialRendererIndex);
			return shader ? shader->getVariableID(name, EST_PIXEL_SHADER) : -1;
		}

		s32 CVulkanDriver::getGeometryShaderConstantID(const c8* name)
		{
			CVulkanUserMaterial* shader = getUserMaterial(ActiveMaterialRendererIndex);
			return shader ? shader->getVariableID(name, EST_GEOMETRY_SHADER) : -1;
		}

		s32 CVulkanDriver::getHullShaderConstantID(const c8* name)
		{
			CVulkanUserMaterial* shader = getUserMaterial(ActiveMaterialRendererIndex);
			return shader ? shader->getVariableID(name, EST_HULL_SHADER) : -1;
		}

		s32 CVulkanDriver::getDomainShaderConstantID(const c8* name)
		{
			CVulkanUserMaterial* shader = getUserMaterial(ActiveMaterialRendererIndex);
			return shader ? shader->getVariableID(name, EST_DOMAIN_SHADER) : -1;
		}

		s32 CVulkanDriver::getShaderConstantBufferID(const c8* name, E_SHADER_TYPE stage)
		{
			CVulkanUserMaterial* shader = getUserMaterial(ActiveMaterialRendererIndex);
			return shader ? shader->getConstantBufferID(name, stage) : -1;
		}

		bool CVulkanDriver::setShaderConstantBuffer(s32 id, const void* data, size_t dataSizeBytes,
			E_SHADER_TYPE stage)
		{
			CVulkanUserMaterial* shader = getUserMaterial(ActiveMaterialRendererIndex);
			return shader ? shader->setConstantBuffer(id, data, dataSizeBytes, stage) : false;
		}

		bool CVulkanDriver::setVertexShaderConstant(s32 index, const f32* floats, int count)
		{
			return setUserFloats(getUserMaterial(ActiveMaterialRendererIndex), index, floats, count,
				EST_VERTEX_SHADER);
		}

		bool CVulkanDriver::setVertexShaderConstant(s32 index, const s32* ints, int count)
		{
			return setUserInts(getUserMaterial(ActiveMaterialRendererIndex), index, ints, count,
				EST_VERTEX_SHADER);
		}

		bool CVulkanDriver::setPixelShaderConstant(s32 index, const f32* floats, int count)
		{
			return setUserFloats(getUserMaterial(ActiveMaterialRendererIndex), index, floats, count,
				EST_PIXEL_SHADER);
		}

		bool CVulkanDriver::setPixelShaderConstant(s32 index, const s32* ints, int count)
		{
			return setUserInts(getUserMaterial(ActiveMaterialRendererIndex), index, ints, count,
				EST_PIXEL_SHADER);
		}

		bool CVulkanDriver::setGeometryShaderConstant(s32 index, const f32* floats, int count)
		{
			return setUserFloats(getUserMaterial(ActiveMaterialRendererIndex), index, floats, count,
				EST_GEOMETRY_SHADER);
		}

		bool CVulkanDriver::setGeometryShaderConstant(s32 index, const s32* ints, int count)
		{
			return setUserInts(getUserMaterial(ActiveMaterialRendererIndex), index, ints, count,
				EST_GEOMETRY_SHADER);
		}

		bool CVulkanDriver::setHullShaderConstant(s32 index, const f32* floats, int count)
		{
			return setUserFloats(getUserMaterial(ActiveMaterialRendererIndex), index, floats, count,
				EST_HULL_SHADER);
		}

		bool CVulkanDriver::setHullShaderConstant(s32 index, const s32* ints, int count)
		{
			return setUserInts(getUserMaterial(ActiveMaterialRendererIndex), index, ints, count,
				EST_HULL_SHADER);
		}

		bool CVulkanDriver::setDomainShaderConstant(s32 index, const f32* floats, int count)
		{
			return setUserFloats(getUserMaterial(ActiveMaterialRendererIndex), index, floats, count,
				EST_DOMAIN_SHADER);
		}

		bool CVulkanDriver::setDomainShaderConstant(s32 index, const s32* ints, int count)
		{
			return setUserInts(getUserMaterial(ActiveMaterialRendererIndex), index, ints, count,
				EST_DOMAIN_SHADER);
		}

		bool CVulkanDriver::setVertexShaderConstant(s32 index, const u32* uints, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, uints,
				(u32)(count * sizeof(u32)), EST_VERTEX_SHADER);
		}

		bool CVulkanDriver::setVertexShaderConstant(s32 index, const f64* doubles, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, doubles,
				(u32)(count * sizeof(f64)), EST_VERTEX_SHADER);
		}

		bool CVulkanDriver::setVertexShaderConstant(s32 index, const s64* longs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, longs,
				(u32)(count * sizeof(s64)), EST_VERTEX_SHADER);
		}

		bool CVulkanDriver::setVertexShaderConstant(s32 index, const u64* ulongs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, ulongs,
				(u32)(count * sizeof(u64)), EST_VERTEX_SHADER);
		}

		bool CVulkanDriver::setPixelShaderConstant(s32 index, const u32* uints, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, uints,
				(u32)(count * sizeof(u32)), EST_PIXEL_SHADER);
		}

		bool CVulkanDriver::setPixelShaderConstant(s32 index, const f64* doubles, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, doubles,
				(u32)(count * sizeof(f64)), EST_PIXEL_SHADER);
		}

		bool CVulkanDriver::setPixelShaderConstant(s32 index, const s64* longs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, longs,
				(u32)(count * sizeof(s64)), EST_PIXEL_SHADER);
		}

		bool CVulkanDriver::setPixelShaderConstant(s32 index, const u64* ulongs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, ulongs,
				(u32)(count * sizeof(u64)), EST_PIXEL_SHADER);
		}

		bool CVulkanDriver::setGeometryShaderConstant(s32 index, const u32* uints, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, uints,
				(u32)(count * sizeof(u32)), EST_GEOMETRY_SHADER);
		}

		bool CVulkanDriver::setGeometryShaderConstant(s32 index, const f64* doubles, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, doubles,
				(u32)(count * sizeof(f64)), EST_GEOMETRY_SHADER);
		}

		bool CVulkanDriver::setGeometryShaderConstant(s32 index, const s64* longs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, longs,
				(u32)(count * sizeof(s64)), EST_GEOMETRY_SHADER);
		}

		bool CVulkanDriver::setGeometryShaderConstant(s32 index, const u64* ulongs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, ulongs,
				(u32)(count * sizeof(u64)), EST_GEOMETRY_SHADER);
		}

		bool CVulkanDriver::setHullShaderConstant(s32 index, const u32* uints, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, uints,
				(u32)(count * sizeof(u32)), EST_HULL_SHADER);
		}

		bool CVulkanDriver::setHullShaderConstant(s32 index, const f64* doubles, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, doubles,
				(u32)(count * sizeof(f64)), EST_HULL_SHADER);
		}

		bool CVulkanDriver::setHullShaderConstant(s32 index, const s64* longs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, longs,
				(u32)(count * sizeof(s64)), EST_HULL_SHADER);
		}

		bool CVulkanDriver::setHullShaderConstant(s32 index, const u64* ulongs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, ulongs,
				(u32)(count * sizeof(u64)), EST_HULL_SHADER);
		}

		bool CVulkanDriver::setDomainShaderConstant(s32 index, const u32* uints, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, uints,
				(u32)(count * sizeof(u32)), EST_DOMAIN_SHADER);
		}

		bool CVulkanDriver::setDomainShaderConstant(s32 index, const f64* doubles, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, doubles,
				(u32)(count * sizeof(f64)), EST_DOMAIN_SHADER);
		}

		bool CVulkanDriver::setDomainShaderConstant(s32 index, const s64* longs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, longs,
				(u32)(count * sizeof(s64)), EST_DOMAIN_SHADER);
		}

		bool CVulkanDriver::setDomainShaderConstant(s32 index, const u64* ulongs, int count)
		{
			return setUserRaw(getUserMaterial(ActiveMaterialRendererIndex), index, ulongs,
				(u32)(count * sizeof(u64)), EST_DOMAIN_SHADER);
		}

		void CVulkanDriver::setVertexShaderConstant(const f32* data, s32 startRegister, s32 constantAmount)
		{
			os::Printer::log("CVulkanDriver::setVertexShaderConstant(register): SPIR-V has no register "
				"file to write into, use the by-name variant", ELL_ERROR);
		}

		void CVulkanDriver::setPixelShaderConstant(const f32* data, s32 startRegister, s32 constantAmount)
		{
			os::Printer::log("CVulkanDriver::setPixelShaderConstant(register): SPIR-V has no register "
				"file to write into, use the by-name variant", ELL_ERROR);
		}

		// ============================ rendering instance control ============================

		void CVulkanDriver::suspendRendering()
		{
			if (SceneOpen && RenderingActive)
				endRendering();
		}

		void CVulkanDriver::resumeRendering()
		{
			if (!SceneOpen || RenderingActive)
				return;

			// Same attachments, nothing cleared: the pass simply continues where it left off.
			const core::rect<s32> viewport = ViewPort;
			beginRendering(false, false, SColor(0, 0, 0, 0));
			// beginRendering() resets the viewport to the whole target; put the caller's back.
			if (viewport != ViewPort)
				setViewPort(viewport);
		}

		bool CVulkanDriver::currentTargetHasStencil() const
		{
			const VkFormat format = (RenderTargetActive && RenderTarget->isValid()) ?
				RenderTarget->getDepthFormat() : DepthFormat;
			return depthFormatHasStencil(format);
		}

		// ================================ stencil shadows ================================

		// Two passes over the same volume, the technique CD3D12Driver::drawStencilShadowVolume() uses:
		// with zfail the depth-FAILING back faces increment and the depth-failing front faces
		// decrement (Carmack's reverse); with zpass the depth-passing front faces increment and the
		// back faces decrement. Depth is tested with GREATER, this fork's reversed-Z convention, and
		// never written; colour is masked off. The stencil reference is 0 in every pipeline.
		void CVulkanDriver::drawStencilShadowVolume(const core::array<core::vector3df>& triangles,
			bool zfail, u32 debugDataVisible)
		{
			const u32 count = triangles.size();
			if (!count || !SceneOpen || !RenderingActive)
				return;

			if (!currentTargetHasStencil())
			{
				if (!WarnedNoStencil)
				{
					os::Printer::log("CVulkanDriver: stencil shadows need a stencil buffer, set "
						"SIrrlichtCreationParameters::Stencilbuffer", ELL_WARNING);
					WarnedNoStencil = true;
				}
				return;
			}

			ImmediateVertices.set_used(count);
			for (u32 i = 0; i < count; ++i)
				ImmediateVertices[i] = S3DVertex(triangles[i].X, triangles[i].Y, triangles[i].Z,
					0, 0, 0, SColor(255, 255, 255, 255), 0, 0);

			SMaterial material;
			material.MaterialType = EMT_SOLID;
			material.Lighting = false;
			material.ZWriteEnable = false; // stencil marking must never modify the depth buffer
			material.ZBuffer = ECFN_GREATER;
			material.ColorMask = ECP_NONE;

			SVulkanStencilOverride increment;
			SVulkanStencilOverride decrement;
			if (zfail)
			{
				increment.DepthFailOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
				decrement.DepthFailOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
			}
			else
			{
				increment.PassOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
				decrement.PassOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
			}

			// zfail increments on the BACK faces (front culled), zpass on the FRONT ones.
			material.FrontfaceCulling = zfail;
			material.BackfaceCulling = !zfail;
			drawImmediate(ImmediateVertices.pointer(), count, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
				material, Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION], &increment);

			material.FrontfaceCulling = !zfail;
			material.BackfaceCulling = zfail;
			drawImmediate(ImmediateVertices.pointer(), count, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
				material, Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION], &decrement);
		}

		void CVulkanDriver::drawStencilShadow(bool clearStencilBuffer, video::SColor leftUpEdge,
			video::SColor rightUpEdge, video::SColor leftDownEdge, video::SColor rightDownEdge)
		{
			if (!SceneOpen || !RenderingActive || !currentTargetHasStencil())
				return;

			// A full-target quad, alpha blended, drawn only where the volume passes left the stencil
			// non-zero: the corner alphas are the shadow's opacity.
			const core::rect<s32> full(0, 0, (s32)CurrentRenderTargetSize.Width, (s32)CurrentRenderTargetSize.Height);
			CVulkanImmediateGeometry::build2DRectangle(ImmediateVertices, full, leftUpEdge, rightUpEdge,
				leftDownEdge, rightDownEdge, nullptr);

			SVulkanStencilOverride test;
			test.CompareOp = VK_COMPARE_OP_NOT_EQUAL; // against the baked reference of 0

			setScissorFromClip(nullptr);
			drawImmediate(ImmediateVertices.pointer(), ImmediateVertices.size(),
				VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, build2DMaterial(true, nullptr),
				core::IdentityMatrix, core::IdentityMatrix,
				CVulkanImmediateGeometry::build2DProjection(CurrentRenderTargetSize), &test);

			if (clearStencilBuffer)
			{
				VkClearAttachment clear = {};
				clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
				clear.clearValue.depthStencil.stencil = 0;

				VkClearRect rect = {};
				rect.rect.extent.width = CurrentRenderTargetSize.Width;
				rect.rect.extent.height = CurrentRenderTargetSize.Height;
				rect.layerCount = 1;
				vk::CmdClearAttachments(Frames[CurrentFrameIndex].CommandBuffer, 1, &clear, 1, &rect);
			}
		}

		// ================================ occlusion queries ================================

		void CVulkanDriver::addOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, const scene::IMesh* mesh)
		{
			if (Occlusion && Occlusion->isValid())
				Occlusion->addQuery(node, mesh);
		}

		void CVulkanDriver::removeOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node)
		{
			if (Occlusion)
				Occlusion->removeQuery(node);
		}

		void CVulkanDriver::removeAllOcclusionQueries()
		{
			if (Occlusion)
				Occlusion->removeAll();
		}

		void CVulkanDriver::runOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool visible)
		{
			if (!node || !Occlusion || !Occlusion->isValid() || !SceneOpen || !RenderingActive)
				return;

			const std::vector<core::vector3df>* positions = Occlusion->getPositions(node);
			if (!positions || positions->empty())
				return;

			const u32 count = (u32)positions->size();
			ImmediateVertices.set_used(count);
			for (u32 i = 0; i < count; ++i)
				ImmediateVertices[i] = S3DVertex((*positions)[i].X, (*positions)[i].Y, (*positions)[i].Z,
					0, 0, 0, SColor(255, 255, 255, 255), 0, 0);

			// Dedicated material, not the driver's current state, for the reasons the D3D12 driver
			// spells out: no colour or depth writes, and GREATEREQUAL so fragments landing exactly on
			// the depth the node itself already wrote still count.
			SMaterial occlusionMaterial;
			occlusionMaterial.Lighting = false;
			occlusionMaterial.AntiAliasing = 0;
			occlusionMaterial.ColorMask = ECP_NONE;
			occlusionMaterial.GouraudShading = false;
			occlusionMaterial.ZWriteEnable = false;
			occlusionMaterial.ZBuffer = ECFN_GREATEREQUAL;
			const SMaterial& drawMaterial = visible ? Material : occlusionMaterial;

			// A Vulkan query slot has to be reset before every use, outside the rendering instance,
			// and a reset discards whatever the slot held. So: a result still pending from an earlier
			// frame is read first (that frame was submitted, waiting is finite), and a slot already
			// recorded THIS frame is not reset at all -- the volume is drawn without a query, since
			// the pending submission would never come back.
			const u64 pending = Occlusion->getPendingFrame(node);
			const bool runQuery = (pending != FrameCounter);
			if (pending && runQuery)
				Occlusion->updateResult(node, true);

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			const u32 slot = Occlusion->getSlot(node);
			if (runQuery)
			{
				suspendRendering();
				Occlusion->resetQuery(frame.CommandBuffer, slot);
				resumeRendering();
				if (!RenderingActive)
					return;
			}

			const SVulkanTransientRange range =
				frame.Immediate->allocateVertices(ImmediateVertices.pointer(), count, sizeof(S3DVertex));
			if (!range.isValid())
				return;

			// Through the ordinary draw path, so whatever that binds (lighting, fog, a user shader's
			// constants when `visible`) is bound here too.
			if (!bindDrawState(drawMaterial, node->getAbsoluteTransformation(), Matrices[ETS_VIEW],
				Matrices[ETS_PROJECTION], nullptr, S3DVertexInput, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST))
				return;

			vk::CmdBindVertexBuffers(frame.CommandBuffer, 0, 1, &range.Buffer, &range.Offset);
			if (runQuery)
				Occlusion->beginQuery(frame.CommandBuffer, slot);
			vk::CmdDraw(frame.CommandBuffer, count, 1, 0, 0);
			endTransformFeedbackForDraw();
			if (runQuery)
			{
				Occlusion->endQuery(frame.CommandBuffer, slot);
				Occlusion->markPending(node, FrameCounter);
			}
		}

		// ============================ timers, statistics, predication ============================

		bool CVulkanDriver::createQueryResources()
		{
			if (QueryResourcesCreated)
				return true;
			if (!Context.Device)
				return false;

			VkQueryPoolCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			info.queryType = VK_QUERY_TYPE_TIMESTAMP;
			info.queryCount = FrameCount * 2 * EMCS_MAX_TIMER_QUERIES;
			if (Context.DeviceProperties.limits.timestampComputeAndGraphics &&
				vulkanFailed("CVulkanDriver: timestamp query pool",
					vk::CreateQueryPool(Context.Device, &info, nullptr, &TimestampPool)))
				TimestampPool = VK_NULL_HANDLE;

			if (Context.HasPipelineStatistics)
			{
				// In SPipelineStatistics order; the result array follows the bit order of the flags.
				VkQueryPoolCreateInfo stats = {};
				stats.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
				stats.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
				stats.queryCount = FrameCount;
				stats.pipelineStatistics =
					VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
					VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
					VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
					VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
					VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
					VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
					VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
					VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
				if (vulkanFailed("CVulkanDriver: pipeline statistics query pool",
					vk::CreateQueryPool(Context.Device, &stats, nullptr, &StatsPool)))
					StatsPool = VK_NULL_HANDLE;
			}

			// The predication buffer: one 32-bit occlusion result per slot, filled by
			// vkCmdCopyQueryPoolResults and read by the conditional rendering commands.
			if (Context.HasConditionalRendering && Occlusion && Occlusion->isValid())
			{
				if (!createVulkanBuffer(Context, CVulkanOcclusionQuery::QueryCapacity * sizeof(u32),
					VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT,
					VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, PredicationBuffer, PredicationMemory))
				{
					PredicationBuffer = VK_NULL_HANDLE;
					PredicationMemory = VK_NULL_HANDLE;
				}
			}

			// A fresh pool's slots are in an undefined state: reset them all once. Nothing has used
			// them yet, so no submission can be affected. Outside any rendering instance.
			if (SceneOpen)
			{
				suspendRendering();
				VkCommandBuffer cmd = Frames[CurrentFrameIndex].CommandBuffer;
				if (TimestampPool)
					vk::CmdResetQueryPool(cmd, TimestampPool, 0, info.queryCount);
				if (StatsPool)
					vk::CmdResetQueryPool(cmd, StatsPool, 0, FrameCount);
				resumeRendering();
			}
			else
			{
				VkCommandBuffer cmd = beginUpload();
				if (cmd)
				{
					if (TimestampPool)
						vk::CmdResetQueryPool(cmd, TimestampPool, 0, info.queryCount);
					if (StatsPool)
						vk::CmdResetQueryPool(cmd, StatsPool, 0, FrameCount);
					endUploadAndWait(cmd);
				}
			}

			for (u32 f = 0; f < FrameCount; ++f)
			{
				StatsOpen[f] = false;
				for (u32 i = 0; i < EMCS_MAX_TIMER_QUERIES; ++i)
					TimerUsed[f][i] = TimerEnded[f][i] = false;
			}
			QueryResourcesCreated = true;
			return TimestampPool != VK_NULL_HANDLE || StatsPool != VK_NULL_HANDLE || PredicationBuffer != VK_NULL_HANDLE;
		}

		void CVulkanDriver::destroyQueryResources()
		{
			if (!Context.Device)
				return;
			if (TimestampPool)
				vk::DestroyQueryPool(Context.Device, TimestampPool, nullptr);
			if (StatsPool)
				vk::DestroyQueryPool(Context.Device, StatsPool, nullptr);
			if (PredicationBuffer)
				vk::DestroyBuffer(Context.Device, PredicationBuffer, nullptr);
			if (PredicationMemory)
				vk::FreeMemory(Context.Device, PredicationMemory, nullptr);
			TimestampPool = StatsPool = VK_NULL_HANDLE;
			PredicationBuffer = VK_NULL_HANDLE;
			PredicationMemory = VK_NULL_HANDLE;
			QueryResourcesCreated = false;
			// The pixel-stage UAV placeholder shares this teardown point (device-owned, like the rest).
			if (NullStorageBuffer)
				vk::DestroyBuffer(Context.Device, NullStorageBuffer, nullptr);
			if (NullStorageMemory)
				vk::FreeMemory(Context.Device, NullStorageMemory, nullptr);
			NullStorageBuffer = VK_NULL_HANDLE;
			NullStorageMemory = VK_NULL_HANDLE;
		}

		void CVulkanDriver::harvestQueryFrame(u32 frame)
		{
			const f64 period = Context.DeviceProperties.limits.timestampPeriod; // nanoseconds per tick
			const u32 base = frame * 2 * EMCS_MAX_TIMER_QUERIES;
			for (u32 id = 0; id < EMCS_MAX_TIMER_QUERIES; ++id)
			{
				if (!TimerUsed[frame][id])
					continue;
				if (TimerEnded[frame][id] && TimestampPool)
				{
					// The fence of this frame has passed, so WAIT never blocks; it only guards the
					// availability the spec makes conditional on the pool state.
					u64 stamps[2] = { 0, 0 };
					if (vk::GetQueryPoolResults(Context.Device, TimestampPool, base + 2 * id, 2, sizeof(stamps),
						stamps, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS &&
						stamps[1] >= stamps[0])
					{
						TimerResults[id] = static_cast<u64>(static_cast<f64>(stamps[1] - stamps[0]) * period);
						TimerResultValid[id] = true;
					}
				}
				TimerUsed[frame][id] = TimerEnded[frame][id] = false;
			}

			if (StatsOpen[frame] && StatsPool)
			{
				u64 values[8] = {};
				if (vk::GetQueryPoolResults(Context.Device, StatsPool, frame, 1, sizeof(values), values,
					sizeof(values), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
				{
					LastStats.VerticesIn = values[0];
					LastStats.PrimitivesIn = values[1];
					LastStats.VertexShaderInvocations = values[2];
					LastStats.GeometryShaderInvocations = values[3];
					LastStats.GeometryShaderPrimitives = values[4];
					LastStats.RasterizedPrimitives = values[5];
					LastStats.PixelShaderInvocations = values[6];
					LastStats.ComputeShaderInvocations = values[7];
					StatsValid = true;
				}
				StatsOpen[frame] = false;
			}
		}

		void CVulkanDriver::beginTimer(u32 id)
		{
			if (!SceneOpen || id >= EMCS_MAX_TIMER_QUERIES)
				return;
			if (!QueryResourcesCreated && !createQueryResources())
				return;
			if (!TimestampPool || !RenderingActive)
				return;
			if (TimerUsed[CurrentFrameIndex][id])
			{
				if (!WarnedTimerReuse)
					os::Printer::log("CVulkanDriver::beginTimer: a timer id was begun twice in one frame, "
						"the second pair is ignored", ELL_WARNING);
				WarnedTimerReuse = true;
				return;
			}
			vk::CmdWriteTimestamp(Frames[CurrentFrameIndex].CommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				TimestampPool, CurrentFrameIndex * 2 * EMCS_MAX_TIMER_QUERIES + 2 * id);
			TimerUsed[CurrentFrameIndex][id] = true;
		}

		void CVulkanDriver::endTimer(u32 id)
		{
			if (!SceneOpen || id >= EMCS_MAX_TIMER_QUERIES || !TimestampPool)
				return;
			if (!TimerUsed[CurrentFrameIndex][id] || TimerEnded[CurrentFrameIndex][id])
				return;
			vk::CmdWriteTimestamp(Frames[CurrentFrameIndex].CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				TimestampPool, CurrentFrameIndex * 2 * EMCS_MAX_TIMER_QUERIES + 2 * id + 1);
			TimerEnded[CurrentFrameIndex][id] = true;
		}

		bool CVulkanDriver::getTimerResult(u32 id, u64& nanoseconds) const
		{
			if (id >= EMCS_MAX_TIMER_QUERIES || !TimerResultValid[id])
				return false;
			nanoseconds = TimerResults[id];
			return true;
		}

		bool CVulkanDriver::getPipelineStatistics(SPipelineStatistics& out) const
		{
			// Arms the per-frame query; the pool is created at the next beginScene().
			StatsArmed = true;
			if (!StatsValid)
				return false;
			out = LastStats;
			return true;
		}

		void CVulkanDriver::beginPredicatedDraws(std::shared_ptr<scene::ISceneNode> node)
		{
			if (!SceneOpen || !RenderingActive || !node || !Occlusion || !Occlusion->isValid() ||
				!Context.HasConditionalRendering || !vk::CmdBeginConditionalRenderingEXT || PredicationActive)
				return;
			if (!QueryResourcesCreated && !createQueryResources())
				return;
			// A node whose query never ran draws normally: there is no result to decide on, and a
			// copy waiting on an unrecorded slot would never complete.
			const u32 slot = Occlusion->getSlot(node);
			if (slot == ~0u || !Occlusion->hasRun(node) || !PredicationBuffer)
				return;

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			const VkDeviceSize offset = static_cast<VkDeviceSize>(slot) * sizeof(u32);

			// The copy and its barriers cannot be recorded inside the rendering instance; the
			// conditional block itself begins inside it, after the resume.
			suspendRendering();
			VkBufferMemoryBarrier toCopy = {};
			toCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			toCopy.srcAccessMask = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT;
			toCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			toCopy.srcQueueFamilyIndex = toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			toCopy.buffer = PredicationBuffer;
			toCopy.offset = offset;
			toCopy.size = sizeof(u32);
			vk::CmdPipelineBarrier(frame.CommandBuffer, VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &toCopy, 0, nullptr);
			// The most recent result the GPU produced for this slot: last frame's, or this frame's
			// when the query already ran earlier in the command buffer (WAIT orders the two).
			vk::CmdCopyQueryPoolResults(frame.CommandBuffer, Occlusion->getPool(), slot, 1,
				PredicationBuffer, offset, sizeof(u32), VK_QUERY_RESULT_WAIT_BIT);
			VkBufferMemoryBarrier toRead = toCopy;
			toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			toRead.dstAccessMask = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT;
			vk::CmdPipelineBarrier(frame.CommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT, 0, 0, nullptr, 1, &toRead, 0, nullptr);
			resumeRendering();
			if (!RenderingActive)
				return;

			// Draws are discarded while the 32-bit value is zero: nothing visible last time.
			VkConditionalRenderingBeginInfoEXT info = {};
			info.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
			info.buffer = PredicationBuffer;
			info.offset = offset;
			vk::CmdBeginConditionalRenderingEXT(frame.CommandBuffer, &info);
			PredicationActive = true;
		}

		void CVulkanDriver::endPredicatedDraws()
		{
			if (!PredicationActive)
				return;
			if (vk::CmdEndConditionalRenderingEXT)
				vk::CmdEndConditionalRenderingEXT(Frames[CurrentFrameIndex].CommandBuffer);
			PredicationActive = false;
		}

		// ---------------- Tiled resources (sparse images) ----------------

		//! A tile pool as blocks of VkDeviceMemory, one memory type (chosen at creation from a probe
		//! sparse image, device local). resize() appends a block; shrinking frees whole trailing blocks
		//! nothing maps into any more, a block straddling the new size staying allocated for the next
		//! growth. A run of pool tiles is split at block boundaries by locate().
		class CVulkanTilePool : public CTilePoolBase
		{
		public:
			CVulkanTilePool(const SVulkanContext& context, u32 memoryTypeIndex)
				: Context(context), MemoryTypeIndex(memoryTypeIndex) {}

			virtual ~CVulkanTilePool()
			{
				// Every bind into this pool was waited for, and nothing maps into it any more.
				for (size_t i = 0; i < Chunks.size(); ++i)
					vk::FreeMemory(Context.Device, Chunks[i].Memory, nullptr);
			}

			bool addChunk(u32 tileCount)
			{
				VkMemoryAllocateInfo info = {};
				info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
				info.allocationSize = static_cast<VkDeviceSize>(tileCount) * TILED_RESOURCE_TILE_BYTES;
				info.memoryTypeIndex = MemoryTypeIndex;
				SChunk chunk;
				chunk.FirstTile = TileCount;
				chunk.TileCount = tileCount;
				if (vulkanFailed("CVulkanDriver: tile pool vkAllocateMemory",
					vk::AllocateMemory(Context.Device, &info, nullptr, &chunk.Memory)))
					return false;
				Chunks.push_back(chunk);
				TileCount += tileCount;
				MappedCount.resize(TileCount, 0);
				return true;
			}

			virtual bool resize(u32 tileCount) _IRR_OVERRIDE_
			{
				if (tileCount == 0)
					return false;
				if (tileCount > TileCount)
				{
					const u32 capacity = Chunks.empty() ? 0 : Chunks.back().FirstTile + Chunks.back().TileCount;
					TileCount = core::min_(tileCount, capacity);
					MappedCount.resize(TileCount, 0);
					return tileCount == TileCount || addChunk(tileCount - TileCount);
				}
				if (!canShrinkTo(tileCount))
				{
					os::Printer::log("ITilePool::resize: a tile past the new size is still mapped", ELL_ERROR);
					return false;
				}
				while (!Chunks.empty() && Chunks.back().FirstTile >= tileCount)
				{
					vk::FreeMemory(Context.Device, Chunks.back().Memory, nullptr);
					Chunks.pop_back();
				}
				TileCount = tileCount;
				MappedCount.resize(TileCount);
				return true;
			}

			//! The block holding pool tile `tile`, the byte offset of the tile in it, and how many
			//! tiles from it on stay in the same block.
			bool locate(u32 tile, VkDeviceMemory& memory, VkDeviceSize& offset, u32& run) const
			{
				if (tile >= TileCount)
					return false;
				for (size_t i = 0; i < Chunks.size(); ++i)
				{
					const SChunk& c = Chunks[i];
					if (tile >= c.FirstTile && tile < c.FirstTile + c.TileCount)
					{
						memory = c.Memory;
						offset = static_cast<VkDeviceSize>(tile - c.FirstTile) * TILED_RESOURCE_TILE_BYTES;
						run = core::min_(c.FirstTile + c.TileCount, TileCount) - tile;
						return true;
					}
				}
				return false;
			}

			const u32 MemoryTypeIndex;

		private:
			struct SChunk
			{
				VkDeviceMemory Memory = VK_NULL_HANDLE;
				u32 FirstTile = 0;
				u32 TileCount = 0;
			};
			std::vector<SChunk> Chunks;
			const SVulkanContext& Context;
		};

		ITilePool* CVulkanDriver::createTilePool(u32 tileCount)
		{
			if (!queryFeature(EVDF_TILED_RESOURCES) || tileCount == 0)
			{
				os::Printer::log("CVulkanDriver::createTilePool: tiled resources unavailable or empty pool", ELL_ERROR);
				return nullptr;
			}

			// The memory types a sparse colour image accepts, asked of a throwaway probe image: the
			// pool is created before any texture maps into it.
			VkImageCreateInfo probeInfo = {};
			probeInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			probeInfo.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
			probeInfo.imageType = VK_IMAGE_TYPE_2D;
			probeInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
			probeInfo.extent = { 256, 256, 1 };
			probeInfo.mipLevels = 1;
			probeInfo.arrayLayers = 1;
			probeInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			probeInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			probeInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			probeInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			probeInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImage probe = VK_NULL_HANDLE;
			if (vulkanFailed("CVulkanDriver::createTilePool: probe image", vk::CreateImage(Context.Device, &probeInfo, nullptr, &probe)))
				return nullptr;
			VkMemoryRequirements requirements = {};
			vk::GetImageMemoryRequirements(Context.Device, probe, &requirements);
			vk::DestroyImage(Context.Device, probe, nullptr);

			u32 memoryType = findMemoryTypeIndex(Context.MemoryProperties, requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (memoryType == 0xffffffffu)
				memoryType = findMemoryTypeIndex(Context.MemoryProperties, requirements.memoryTypeBits, 0);
			if (memoryType == 0xffffffffu)
			{
				os::Printer::log("CVulkanDriver::createTilePool: no memory type for sparse images", ELL_ERROR);
				return nullptr;
			}

			CVulkanTilePool* pool = new CVulkanTilePool(Context, memoryType);
			if (!pool->addChunk(tileCount))
			{
				pool->drop();
				return nullptr;
			}
			return pool;
		}

		bool CVulkanDriver::queryTileShape(CVulkanTexture* texture, STiledTextureRecord& out) const
		{
			u32 count = 0;
			vk::GetImageSparseMemoryRequirements(Context.Device, texture->getImage(), &count, nullptr);
			std::vector<VkSparseImageMemoryRequirements> requirements(count);
			if (count)
				vk::GetImageSparseMemoryRequirements(Context.Device, texture->getImage(), &count, requirements.data());
			const VkSparseImageMemoryRequirements* colour = nullptr;
			for (u32 i = 0; i < count; ++i)
				if (requirements[i].formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)
					colour = &requirements[i];
			if (!colour || colour->formatProperties.imageGranularity.width == 0)
			{
				os::Printer::log("CVulkanDriver::addTiledTexture: no sparse requirements for the colour aspect", ELL_ERROR);
				return false;
			}
			VkMemoryRequirements memory = {};
			vk::GetImageMemoryRequirements(Context.Device, texture->getImage(), &memory);
			if (memory.alignment != TILED_RESOURCE_TILE_BYTES)
			{
				os::Printer::log("CVulkanDriver::addTiledTexture: the device uses a non-standard sparse block size", ELL_ERROR);
				return false;
			}

			const VkExtent3D g = colour->formatProperties.imageGranularity;
			out.Size = texture->getSize();
			out.MipLevels = texture->getMipLevelCount();
			out.ArraySlices = texture->getLayerCount();
			out.MemoryTypeBits = memory.memoryTypeBits;
			out.Shape.TexelsWide = g.width;
			out.Shape.TexelsHigh = g.height;
			out.Shape.TexelsDeep = g.depth;
			out.Shape.TilesWide = out.tilesWide(0);
			out.Shape.TilesHigh = out.tilesHigh(0);
			out.Shape.TilesDeep = 1;
			out.Shape.MipTailStart = core::min_(colour->imageMipTailFirstLod, out.MipLevels);
			out.Shape.MipTailTiles = static_cast<u32>(colour->imageMipTailSize / TILED_RESOURCE_TILE_BYTES);
			out.MipTailOffset = colour->imageMipTailOffset;
			out.MipTailStride = colour->imageMipTailStride;
			out.SingleMipTail = (colour->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0;

			u32 standardTiles = 0;
			for (u32 mip = 0; mip < out.Shape.MipTailStart; ++mip)
				standardTiles += out.tilesWide(mip) * out.tilesHigh(mip);
			out.Shape.TotalTiles = standardTiles * out.ArraySlices +
				(out.SingleMipTail ? out.Shape.MipTailTiles : out.Shape.MipTailTiles * out.ArraySlices);
			return true;
		}

		ITexture* CVulkanDriver::addTiledTexture(const core::dimension2d<u32>& size, const io::path& name,
			ECOLOR_FORMAT format, u32 mipLevels, u32 arraySlices, bool isRenderTarget)
		{
			if (!queryFeature(EVDF_TILED_RESOURCES))
			{
				os::Printer::log("CVulkanDriver::addTiledTexture: EVDF_TILED_RESOURCES unavailable", name, ELL_ERROR);
				return nullptr;
			}
			CVulkanTexture* texture = new CVulkanTexture(Context, *ResourceOwner, size,
				(format == ECF_UNKNOWN) ? ECF_A8R8G8B8 : format, mipLevels, arraySlices, isRenderTarget, name, STiledTextureTag());
			STiledTextureRecord record;
			if (!texture->hasDeviceResource() || !queryTileShape(texture, record))
			{
				texture->drop();
				return nullptr;
			}
			CNullDriver::addTexture(texture);
			texture->drop();
			TiledTextures[texture] = record;
			return texture;
		}

		bool CVulkanDriver::getTileShape(const ITexture* texture, STileShape& out) const
		{
			std::map<const ITexture*, STiledTextureRecord>::const_iterator it = TiledTextures.find(texture);
			if (it == TiledTextures.end())
				return false;
			out = it->second.Shape;
			return true;
		}

		bool CVulkanDriver::updateTileMappings(ITexture* texture, const STileRegion* regions, u32 regionCount,
			ITilePool* pool, const u32* poolTileIndices)
		{
			std::map<const ITexture*, STiledTextureRecord>::iterator it = TiledTextures.find(texture);
			if (it == TiledTextures.end())
			{
				os::Printer::log("CVulkanDriver::updateTileMappings: not a tiled texture of this driver", ELL_ERROR);
				return false;
			}
			STiledTextureRecord& record = it->second;
			CVulkanTilePool* vkPool = static_cast<CVulkanTilePool*>(pool);
			if (!record.validate(regions, regionCount, vkPool, poolTileIndices, "CVulkanDriver::updateTileMappings"))
				return false;
			if (vkPool && !((record.MemoryTypeBits >> vkPool->MemoryTypeIndex) & 1))
			{
				os::Printer::log("CVulkanDriver::updateTileMappings: the pool's memory type does not fit this image", ELL_ERROR);
				return false;
			}

			CVulkanTexture* vkTexture = static_cast<CVulkanTexture*>(texture);
			std::vector<VkSparseImageMemoryBind> imageBinds;
			std::vector<VkSparseMemoryBind> tailBinds;
			for (u32 i = 0; i < regionCount; ++i)
			{
				const STileRegion& r = regions[i];
				const bool tail = record.isMipTail(r.MipLevel);
				if (tail && record.SingleMipTail && r.ArraySlice != 0)
				{
					os::Printer::log("CVulkanDriver::updateTileMappings: this image has one mip tail for every layer; "
						"address it through ArraySlice 0", ELL_ERROR);
					return false;
				}
				const u32 mipWidth = core::max_(1u, record.Size.Width >> r.MipLevel);
				const u32 mipHeight = core::max_(1u, record.Size.Height >> r.MipLevel);
				u32 n = 0;
				for (u32 y = 0; y < r.Height; ++y)
					for (u32 x = 0; x < r.Width; ++x, ++n)
					{
						VkDeviceMemory memory = VK_NULL_HANDLE;
						VkDeviceSize offset = 0;
						u32 run = 0;
						if (vkPool && !vkPool->locate(poolTileIndices[i] + n, memory, offset, run))
							return false;
						if (tail)
						{
							VkSparseMemoryBind bind = {};
							bind.resourceOffset = record.MipTailOffset +
								(record.SingleMipTail ? 0 : record.MipTailStride * r.ArraySlice) +
								static_cast<VkDeviceSize>(r.X + x) * TILED_RESOURCE_TILE_BYTES;
							bind.size = TILED_RESOURCE_TILE_BYTES;
							bind.memory = memory;
							bind.memoryOffset = offset;
							tailBinds.push_back(bind);
						}
						else
						{
							VkSparseImageMemoryBind bind = {};
							bind.subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
							bind.subresource.mipLevel = r.MipLevel;
							bind.subresource.arrayLayer = r.ArraySlice;
							bind.offset.x = static_cast<s32>((r.X + x) * record.Shape.TexelsWide);
							bind.offset.y = static_cast<s32>((r.Y + y) * record.Shape.TexelsHigh);
							bind.offset.z = 0;
							// The last tile of a row/column is clipped to the image, as the spec requires.
							bind.extent.width = core::min_(record.Shape.TexelsWide, mipWidth - (u32)bind.offset.x);
							bind.extent.height = core::min_(record.Shape.TexelsHigh, mipHeight - (u32)bind.offset.y);
							bind.extent.depth = 1;
							bind.memory = memory;
							bind.memoryOffset = offset;
							imageBinds.push_back(bind);
						}
					}
			}

			VkSparseImageMemoryBindInfo imageInfo = {};
			imageInfo.image = vkTexture->getImage();
			imageInfo.bindCount = static_cast<u32>(imageBinds.size());
			imageInfo.pBinds = imageBinds.data();
			VkSparseImageOpaqueMemoryBindInfo tailInfo = {};
			tailInfo.image = vkTexture->getImage();
			tailInfo.bindCount = static_cast<u32>(tailBinds.size());
			tailInfo.pBinds = tailBinds.data();
			VkBindSparseInfo bindInfo = {};
			bindInfo.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
			bindInfo.imageBindCount = imageBinds.empty() ? 0 : 1;
			bindInfo.pImageBinds = &imageInfo;
			bindInfo.imageOpaqueBindCount = tailBinds.empty() ? 0 : 1;
			bindInfo.pImageOpaqueBinds = &tailInfo;

			{
				// Waited for, like an upload: the caller may copy into the tiles straight after, and a
				// pool block freed later must not be named by a bind still in flight.
				std::lock_guard<std::mutex> queueLock(ResourceOwner->QueueMutex);
				if (vulkanFailed("CVulkanDriver::updateTileMappings: vkQueueBindSparse",
					vk::QueueBindSparse(Context.GraphicsQueue, 1, &bindInfo, VK_NULL_HANDLE)))
					return false;
				vk::QueueWaitIdle(Context.GraphicsQueue);
			}

			for (u32 i = 0; i < regionCount; ++i)
			{
				const STileRegion& r = regions[i];
				const u32 subresource = r.MipLevel + r.ArraySlice * record.MipLevels;
				u32 n = 0;
				for (u32 y = 0; y < r.Height; ++y)
					for (u32 x = 0; x < r.Width; ++x, ++n)
						record.account(STiledTextureRecord::tileKey(subresource, r.X + x, r.Y + y, 0),
							vkPool, vkPool ? poolTileIndices[i] + n : 0);
			}
			return true;
		}

		bool CVulkanDriver::updateTiles(ITexture* texture, const STileRegion& region, const void* data)
		{
			std::map<const ITexture*, STiledTextureRecord>::iterator it = TiledTextures.find(texture);
			if (it == TiledTextures.end() || !data)
			{
				os::Printer::log("CVulkanDriver::updateTiles: not a tiled texture of this driver, or no data", ELL_ERROR);
				return false;
			}
			const STiledTextureRecord& record = it->second;
			if (!record.validate(&region, 1, nullptr, nullptr, "CVulkanDriver::updateTiles"))
				return false;
			if (record.isMipTail(region.MipLevel))
			{
				os::Printer::log("CVulkanDriver::updateTiles: packed mips have no per-tile layout; write them through lock()", ELL_ERROR);
				return false;
			}

			const VkDeviceSize bytes = static_cast<VkDeviceSize>(region.getTileCount()) * TILED_RESOURCE_TILE_BYTES;
			VkBuffer staging = VK_NULL_HANDLE;
			VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
			if (!createVulkanBuffer(Context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMemory))
				return false;
			void* mapped = nullptr;
			if (vulkanFailed("vkMapMemory", vk::MapMemory(Context.Device, stagingMemory, 0, bytes, 0, &mapped)))
			{
				vk::DestroyBuffer(Context.Device, staging, nullptr);
				vk::FreeMemory(Context.Device, stagingMemory, nullptr);
				return false;
			}
			memcpy(mapped, data, static_cast<size_t>(bytes));
			vk::UnmapMemory(Context.Device, stagingMemory);

			CVulkanTexture* vkTexture = static_cast<CVulkanTexture*>(texture);
			const u32 mipWidth = core::max_(1u, record.Size.Width >> region.MipLevel);
			const u32 mipHeight = core::max_(1u, record.Size.Height >> region.MipLevel);
			// One copy per tile: the source holds the tiles back to back, each a tightly packed
			// TexelsWide x TexelsHigh block (the last column/row of tiles reads only its visible part).
			std::vector<VkBufferImageCopy> copies;
			u32 n = 0;
			for (u32 y = 0; y < region.Height; ++y)
				for (u32 x = 0; x < region.Width; ++x, ++n)
				{
					VkBufferImageCopy copy = {};
					copy.bufferOffset = static_cast<VkDeviceSize>(n) * TILED_RESOURCE_TILE_BYTES;
					copy.bufferRowLength = record.Shape.TexelsWide;
					copy.bufferImageHeight = record.Shape.TexelsHigh;
					copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					copy.imageSubresource.mipLevel = region.MipLevel;
					copy.imageSubresource.baseArrayLayer = region.ArraySlice;
					copy.imageSubresource.layerCount = 1;
					copy.imageOffset.x = static_cast<s32>((region.X + x) * record.Shape.TexelsWide);
					copy.imageOffset.y = static_cast<s32>((region.Y + y) * record.Shape.TexelsHigh);
					copy.imageExtent.width = core::min_(record.Shape.TexelsWide, mipWidth - (u32)copy.imageOffset.x);
					copy.imageExtent.height = core::min_(record.Shape.TexelsHigh, mipHeight - (u32)copy.imageOffset.y);
					copy.imageExtent.depth = 1;
					copies.push_back(copy);
				}

			VkCommandBuffer commandBuffer = ResourceOwner->beginUpload();
			bool ok = commandBuffer != VK_NULL_HANDLE;
			if (ok)
			{
				const VkImageLayout before = vkTexture->getImageLayout();
				vkTexture->transitionTo(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				vk::CmdCopyBufferToImage(commandBuffer, staging, vkTexture->getImage(),
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<u32>(copies.size()), copies.data());
				vkTexture->transitionTo(commandBuffer, before == VK_IMAGE_LAYOUT_UNDEFINED ?
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : before);
				ResourceOwner->endUploadAndWait(commandBuffer);
			}
			vk::DestroyBuffer(Context.Device, staging, nullptr);
			vk::FreeMemory(Context.Device, stagingMemory, nullptr);
			return ok;
		}

		void CVulkanDriver::removeTexture(ITexture* texture)
		{
			std::map<const ITexture*, STiledTextureRecord>::iterator it = TiledTextures.find(texture);
			if (it != TiledTextures.end())
			{
				it->second.release();
				TiledTextures.erase(it);
			}
			CNullDriver::removeTexture(texture);
		}

		void CVulkanDriver::removeAllTextures()
		{
			releaseTiledRecords();
			CNullDriver::removeAllTextures();
		}

		void CVulkanDriver::releaseTiledRecords()
		{
			for (std::map<const ITexture*, STiledTextureRecord>::iterator it = TiledTextures.begin(); it != TiledTextures.end(); ++it)
				it->second.release();
			TiledTextures.clear();
		}

		void CVulkanDriver::runAllOcclusionQueries(bool visible)
		{
			if (!Occlusion)
				return;
			const std::vector<std::shared_ptr<scene::ISceneNode>> nodes = Occlusion->getNodes();
			for (size_t i = 0; i < nodes.size(); ++i)
				runOcclusionQuery(nodes[i], visible);
		}

		void CVulkanDriver::updateOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool block)
		{
			if (!node || !Occlusion)
				return;
			// A query recorded in the frame still being recorded has not been submitted: blocking on
			// it would hang, so such a call degrades to a poll ("update might not occur").
			const bool submitted = Occlusion->getPendingFrame(node) != FrameCounter;
			Occlusion->updateResult(node, block && submitted);
		}

		void CVulkanDriver::updateAllOcclusionQueries(bool block)
		{
			if (!Occlusion)
				return;
			const std::vector<std::shared_ptr<scene::ISceneNode>> nodes = Occlusion->getNodes();
			for (size_t i = 0; i < nodes.size(); ++i)
				updateOcclusionQuery(nodes[i], block);
		}

		u32 CVulkanDriver::getOcclusionQueryResult(std::shared_ptr<scene::ISceneNode> node) const
		{
			return Occlusion ? Occlusion->getResult(node) : ~0u;
		}

		// ==================================== compute ====================================

		std::shared_ptr<video::IHardwareBuffer> CVulkanDriver::createHardwareBuffer(scene::IComputeBuffer* computeBuffer)
		{
			if (!computeBuffer)
				return nullptr;

			auto existing = computeBuffer->getHardwareBuffer();
			if (existing && existing->getDriverType() == EDT_VULKAN && !existing->isRequiredUpdate())
				return existing;

			const u32 size = computeBuffer->getStructureCount() * computeBuffer->getStructureStride();
			if (!size)
			{
				os::Printer::log("CVulkanDriver::createHardwareBuffer: empty compute buffer", ELL_ERROR);
				return nullptr;
			}

			// Device local whatever the hint (see CVulkanHardwareBuffer::wantsDeviceLocal()): a
			// compute buffer is a GPU write target. The flags decide the extra bind points -- vertex
			// for a per-instance stream, indirect for draw/dispatch arguments.
			auto buffer = std::make_shared<CVulkanHardwareBuffer>(ResourceOwner->Context, *ResourceOwner,
				size, EHBT_COMPUTE, EHBA_DEFAULT, computeBuffer->getBufferFlags(),
				computeBuffer->getStructureStride(), computeBuffer->getBufferPointer());
			if (buffer->getBuffer() == VK_NULL_HANDLE)
				return nullptr;

			computeBuffer->setHardwareBuffer(buffer);
			return buffer;
		}

		CVulkanHardwareBuffer* CVulkanDriver::prepareComputeBuffer(scene::IComputeBuffer* buffer)
		{
			if (!buffer || buffer->getStructureCount() == 0)
				return nullptr;

			// Same sequence as the D3D11/D3D12 dispatches: created on first use, re-uploaded when the
			// CPU copy was marked dirty since.
			auto hardware = buffer->getHardwareBuffer();
			if (!hardware || hardware->getDriverType() != EDT_VULKAN)
				hardware = createHardwareBuffer(buffer);
			else if (hardware->isRequiredUpdate())
				hardware->update(buffer->getHardwareMappingHint(),
					buffer->getStructureCount() * buffer->getStructureStride(), buffer->getBufferPointer());
			if (!hardware)
				return nullptr;

			CVulkanHardwareBuffer* native = static_cast<CVulkanHardwareBuffer*>(hardware.get());
			return (native->getBuffer() != VK_NULL_HANDLE) ? native : nullptr;
		}

		void CVulkanDriver::runComputeCallback(CVulkanComputeMaterial* material)
		{
			// Same convention as bindDrawState(): the index is set before the callback so
			// getComputeShaderConstantID()/setComputeShaderConstant() know which material is active.
			ActiveMaterialRendererIndex = Material.MaterialType;
			if (material && material->CallBack)
			{
				material->CallBack->OnSetMaterial(Material);
				material->CallBack->OnSetConstants(this, material->UserData);
			}
		}

		// Synchronous and on its own command buffer, as the D3D12 dispatch is: IVideoDriver's compute
		// entry points may be called outside beginScene()/endScene(), and the caller reads the result
		// back right after through IComputeBuffer::downloadFromGPU().
		void CVulkanDriver::dispatchComputeShader(const core::vector3d<u32>& groupCount,
			scene::IComputeBuffer* Src, scene::IComputeBuffer* Dst)
		{
			if (!Src || !Dst || Src->getStructureCount() == 0 || Dst->getStructureCount() == 0)
				return;

			CVulkanComputeMaterial* material = getComputeMaterial(Material.MaterialType);
			if (!material)
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShader: the active material has no "
					"compute shader", ELL_ERROR);
				return;
			}
			if (!Compute || !Compute->isReady())
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShader: compute is unavailable", ELL_ERROR);
				return;
			}

			CVulkanHardwareBuffer* src = prepareComputeBuffer(Src);
			CVulkanHardwareBuffer* dst = prepareComputeBuffer(Dst);
			if (!src || !dst)
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShader: Src/Dst have no device buffer",
					ELL_ERROR);
				return;
			}

			runComputeCallback(material);

			VkCommandBuffer cmd = beginUpload();
			if (cmd != VK_NULL_HANDLE)
			{
				CVulkanCompute::barrierBeforeDispatch(cmd, src->getBuffer(), dst->getBuffer());
				if (Compute->dispatch(cmd, material, src, dst, groupCount))
					CVulkanCompute::barrierAfterDispatch(cmd, dst->getBuffer());
				endUploadAndWait(cmd);
			}
			ActiveMaterialRendererIndex = -1;
		}

		void CVulkanDriver::dispatchComputeShaderToTexture(const core::vector3d<u32>& groupCount,
			scene::IComputeBuffer* Src, ITexture* Dst)
		{
			if (!Src || !Dst || Src->getStructureCount() == 0)
				return;

			if (Dst->getDriverType() != EDT_VULKAN || !Dst->isUnorderedAccess())
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShaderToTexture: Dst is not a UAV texture "
					"of this driver (see addUAVTexture())", ELL_ERROR);
				return;
			}

			CVulkanComputeMaterial* material = getComputeMaterial(Material.MaterialType);
			if (!material)
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShaderToTexture: the active material has "
					"no compute shader", ELL_ERROR);
				return;
			}
			if (!Compute || !Compute->isReady())
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShaderToTexture: compute is unavailable",
					ELL_ERROR);
				return;
			}

			CVulkanHardwareBuffer* src = prepareComputeBuffer(Src);
			CVulkanTexture* dstTexture = static_cast<CVulkanTexture*>(Dst);
			if (!src || !dstTexture->hasDeviceResource())
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShaderToTexture: Src has no device buffer",
					ELL_ERROR);
				return;
			}

			runComputeCallback(material);

			VkCommandBuffer cmd = beginUpload();
			if (cmd != VK_NULL_HANDLE)
			{
				CVulkanCompute::barrierBeforeDispatch(cmd, src->getBuffer(), VK_NULL_HANDLE);
				Compute->dispatchToTexture(cmd, material, src, dstTexture, groupCount);
				// Left ready to be sampled by the next draw, since endUploadAndWait() blocks until
				// the dispatch has actually finished.
				CVulkanCompute::barrierImageToShaderRead(cmd, dstTexture);
				endUploadAndWait(cmd);
			}
			ActiveMaterialRendererIndex = -1;
		}

		void CVulkanDriver::bindComputeBuffer(u32 slot, scene::IComputeBuffer* buffer, E_HARDWARE_BUFFER_TYPE binding)
		{
			const bool asUAV = (binding == EHBT_COMPUTE);
			const u32 maxSlot = asUAV ? (u32)EMCS_MAX_COMPUTE_UAV_SLOTS : (u32)EMCS_MAX_COMPUTE_SRV_SLOTS;
			if (slot >= maxSlot)
			{
				os::Printer::log("CVulkanDriver::bindComputeBuffer: slot out of range", ELL_ERROR);
				return;
			}

			SVulkanComputeSlot& target = asUAV ? ComputeUAV[slot] : ComputeSRV[slot];
			target.Buffer = buffer;
			target.Texture = nullptr;
		}

		void CVulkanDriver::bindComputeTexture(u32 slot, ITexture* texture, bool asUAV)
		{
			const u32 maxSlot = asUAV ? (u32)EMCS_MAX_COMPUTE_UAV_SLOTS : (u32)EMCS_MAX_COMPUTE_SRV_SLOTS;
			if (slot >= maxSlot)
			{
				os::Printer::log("CVulkanDriver::bindComputeTexture: slot out of range", ELL_ERROR);
				return;
			}
			if (texture && texture->getDriverType() != EDT_VULKAN)
			{
				os::Printer::log("CVulkanDriver::bindComputeTexture: texture is not a Vulkan one", ELL_ERROR);
				return;
			}
			if (asUAV && texture && !texture->isUnorderedAccess())
			{
				os::Printer::log("CVulkanDriver::bindComputeTexture: texture has no storage usage - use "
					"addUAVTexture()", ELL_ERROR);
				return;
			}

			SVulkanComputeSlot& target = asUAV ? ComputeUAV[slot] : ComputeSRV[slot];
			target.Buffer = nullptr;
			target.Texture = texture;
		}

		void CVulkanDriver::dispatchComputeShaderBound(const core::vector3d<u32>& groupCount)
		{
			dispatchBoundResources(groupCount, nullptr, 0);
		}

		void CVulkanDriver::dispatchComputeShaderIndirect(scene::IComputeBuffer* argBuffer, u32 byteOffset)
		{
			if (!argBuffer)
				return;

			CVulkanHardwareBuffer* args = prepareComputeBuffer(argBuffer);
			if (!args)
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShaderIndirect: args buffer has no device "
					"buffer", ELL_ERROR);
				return;
			}
			if (!(args->getFlags() & EHBF_DRAW_INDIRECT_ARGS))
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShaderIndirect: the args buffer needs "
					"EHBF_DRAW_INDIRECT_ARGS", ELL_ERROR);
				return;
			}
			if (byteOffset % 4 != 0 || byteOffset + 12 > args->getSize())
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShaderIndirect: byteOffset must be a "
					"multiple of 4 and leave room for three u32", ELL_ERROR);
				return;
			}

			dispatchBoundResources(core::vector3d<u32>(1, 1, 1), args, byteOffset);
		}

		void CVulkanDriver::dispatchBoundResources(const core::vector3d<u32>& groupCount,
			CVulkanHardwareBuffer* indirectArgs, u32 indirectOffset)
		{
			CVulkanComputeMaterial* material = getComputeMaterial(Material.MaterialType);
			if (!material)
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShaderBound: the active material has no "
					"compute shader", ELL_ERROR);
				return;
			}
			if (!Compute || !Compute->isReady())
			{
				os::Printer::log("CVulkanDriver::dispatchComputeShaderBound: compute is unavailable", ELL_ERROR);
				return;
			}

			// Slots -> binding numbers, per the convention in CVulkanCompute.h. Every bound buffer is
			// brought up to date here, which is also where a dirty CPU copy gets uploaded.
			SVulkanComputeResources resources;
			std::vector<CVulkanHardwareBuffer*> readBuffers;
			std::vector<CVulkanHardwareBuffer*> writeBuffers;
			std::vector<CVulkanTexture*> writeTextures;

			for (u32 s = 0; s < EMCS_MAX_COMPUTE_SRV_SLOTS; ++s)
			{
				const SVulkanComputeSlot& slot = ComputeSRV[s];
				if (slot.Buffer)
				{
					CVulkanHardwareBuffer* buffer = prepareComputeBuffer(slot.Buffer);
					if (!buffer)
					{
						os::Printer::log("CVulkanDriver::dispatchComputeShaderBound: an SRV buffer has no "
							"device buffer", ELL_ERROR);
						return;
					}
					resources.setBuffer(VulkanComputeSrvBindingBase + s, buffer);
					readBuffers.push_back(buffer);
				}
				else if (slot.Texture)
					resources.setTexture(VulkanComputeSrvBindingBase + s, static_cast<CVulkanTexture*>(slot.Texture));
			}

			for (u32 u = 0; u < EMCS_MAX_COMPUTE_UAV_SLOTS; ++u)
			{
				const SVulkanComputeSlot& slot = ComputeUAV[u];
				if (slot.Buffer)
				{
					CVulkanHardwareBuffer* buffer = prepareComputeBuffer(slot.Buffer);
					if (!buffer)
					{
						os::Printer::log("CVulkanDriver::dispatchComputeShaderBound: a UAV buffer has no "
							"device buffer", ELL_ERROR);
						return;
					}
					resources.setBuffer(VulkanComputeUavBindingBase + u, buffer);
					writeBuffers.push_back(buffer);
				}
				else if (slot.Texture)
				{
					CVulkanTexture* texture = static_cast<CVulkanTexture*>(slot.Texture);
					resources.setTexture(VulkanComputeUavBindingBase + u, texture);
					writeTextures.push_back(texture);
				}
			}

			runComputeCallback(material);

			VkCommandBuffer cmd = beginUpload();
			if (cmd != VK_NULL_HANDLE)
			{
				// Whatever produced each buffer -- an upload, a previous dispatch, the host -- is made
				// visible to this one; the argument buffer of an indirect dispatch as well.
				for (size_t i = 0; i < readBuffers.size(); ++i)
					CVulkanCompute::barrierBeforeDispatch(cmd, readBuffers[i]->getBuffer(), VK_NULL_HANDLE);
				for (size_t i = 0; i < writeBuffers.size(); ++i)
					CVulkanCompute::barrierBeforeDispatch(cmd, VK_NULL_HANDLE, writeBuffers[i]->getBuffer());
				if (indirectArgs)
					CVulkanCompute::bufferBarrier(cmd, indirectArgs->getBuffer(), 0, VK_WHOLE_SIZE,
						VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
						VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
						VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
						VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT);

				const bool dispatched = Compute->dispatchBound(cmd, material, resources, groupCount,
					indirectArgs ? indirectArgs->getBuffer() : VK_NULL_HANDLE, indirectOffset);

				if (dispatched)
				{
					for (size_t i = 0; i < writeBuffers.size(); ++i)
						CVulkanCompute::barrierAfterDispatch(cmd, writeBuffers[i]->getBuffer());
					// Back to the sampled layout so a draw can read the result; the next dispatch
					// that binds one as a UAV moves it to GENERAL again by itself.
					for (size_t i = 0; i < writeTextures.size(); ++i)
						CVulkanCompute::barrierImageToShaderRead(cmd, writeTextures[i]);
				}
				endUploadAndWait(cmd);
			}
			ActiveMaterialRendererIndex = -1;
		}

		void CVulkanDriver::unbindComputeResources()
		{
			for (u32 i = 0; i < EMCS_MAX_COMPUTE_SRV_SLOTS; ++i)
				ComputeSRV[i] = SVulkanComputeSlot();
			for (u32 i = 0; i < EMCS_MAX_COMPUTE_UAV_SLOTS; ++i)
				ComputeUAV[i] = SVulkanComputeSlot();
		}

		// ================================ pixel-stage UAVs ================================

		bool CVulkanDriver::ensureNullStorageBuffer()
		{
			if (NullStorageBuffer != VK_NULL_HANDLE)
				return true;
			// What a declared-but-unbound storage buffer points at, so the descriptor is valid.
			return createVulkanBuffer(Context, NullStorageBufferSize,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NullStorageBuffer, NullStorageMemory);
		}

		void CVulkanDriver::transitionPixelUAVTexture(CVulkanTexture* texture, VkImageLayout layout)
		{
			if (!texture || texture->getImageLayout() == layout)
				return;
			// An image barrier is illegal inside a rendering instance; outside a scene the one-shot
			// upload buffer carries it.
			if (SceneOpen)
			{
				suspendRendering();
				texture->transitionTo(Frames[CurrentFrameIndex].CommandBuffer, layout);
				resumeRendering();
			}
			else
			{
				VkCommandBuffer cmd = beginUpload();
				if (!cmd)
					return;
				texture->transitionTo(cmd, layout);
				endUploadAndWait(cmd);
			}
		}

		bool CVulkanDriver::bindPixelShaderBuffer(u32 slot, scene::IComputeBuffer* buffer)
		{
			if (slot >= MaxPixelUAVSlots)
			{
				os::Printer::log("CVulkanDriver::bindPixelShaderBuffer: slot out of range", ELL_ERROR);
				return false;
			}
			// D3D11 parity: the UAV slots start after the bound colour attachments.
			const u32 targets = RenderTargetActive ? RenderTarget->getColorAttachmentCount() : 1;
			if (buffer && slot < targets)
			{
				os::Printer::log("CVulkanDriver::bindPixelShaderBuffer: the slot must be at or above the "
					"number of bound render targets", ELL_ERROR);
				return false;
			}
			if (buffer && !prepareComputeBuffer(buffer))
			{
				os::Printer::log("CVulkanDriver::bindPixelShaderBuffer: the buffer has no device buffer", ELL_ERROR);
				return false;
			}
			PixelUAV[slot].Buffer = buffer;
			PixelUAV[slot].Texture = nullptr;
			return true;
		}

		bool CVulkanDriver::bindPixelShaderTexture(u32 slot, ITexture* texture)
		{
			if (slot >= MaxPixelUAVSlots)
			{
				os::Printer::log("CVulkanDriver::bindPixelShaderTexture: slot out of range", ELL_ERROR);
				return false;
			}
			const u32 targets = RenderTargetActive ? RenderTarget->getColorAttachmentCount() : 1;
			if (texture && slot < targets)
			{
				os::Printer::log("CVulkanDriver::bindPixelShaderTexture: the slot must be at or above the "
					"number of bound render targets", ELL_ERROR);
				return false;
			}
			if (texture && (texture->getDriverType() != EDT_VULKAN || !texture->isUnorderedAccess()))
			{
				os::Printer::log("CVulkanDriver::bindPixelShaderTexture: not an addUAVTexture() texture", ELL_ERROR);
				return false;
			}
			// Storage access needs the GENERAL layout; back to the sampled one at unbind.
			transitionPixelUAVTexture(static_cast<CVulkanTexture*>(texture), VK_IMAGE_LAYOUT_GENERAL);
			PixelUAV[slot].Buffer = nullptr;
			PixelUAV[slot].Texture = texture;
			return true;
		}

		void CVulkanDriver::unbindPixelShaderResources()
		{
			bool anyBuffer = false;
			for (u32 i = 0; i < MaxPixelUAVSlots; ++i)
			{
				if (PixelUAV[i].Texture)
					transitionPixelUAVTexture(static_cast<CVulkanTexture*>(PixelUAV[i].Texture),
						VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				anyBuffer = anyBuffer || PixelUAV[i].Buffer != nullptr;
				PixelUAV[i] = SVulkanComputeSlot();
			}
			// What the fragment shaders wrote becomes visible to whatever reads the buffers next in
			// this frame (a vertex fetch, a compute dispatch, a copy).
			if (anyBuffer && SceneOpen)
			{
				suspendRendering();
				VkMemoryBarrier barrier = {};
				barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
				barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
					VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
				vk::CmdPipelineBarrier(Frames[CurrentFrameIndex].CommandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
				resumeRendering();
			}
		}

		// Every dispatch here is submitted and waited on with full barriers around it, so there is
		// no GPU hazard left to order. What remains is the D3D11 meaning of the call: the buffer
		// stops being a UAV, so the next dispatch may read it through an SRV slot.
		void CVulkanDriver::computeBarrier(scene::IComputeBuffer* buffer)
		{
			if (!buffer)
				return;
			for (u32 i = 0; i < EMCS_MAX_COMPUTE_UAV_SLOTS; ++i)
				if (ComputeUAV[i].Buffer == buffer)
					ComputeUAV[i] = SVulkanComputeSlot();
		}

		void CVulkanDriver::computeBarrierAll()
		{
			for (u32 i = 0; i < EMCS_MAX_COMPUTE_UAV_SLOTS; ++i)
				ComputeUAV[i] = SVulkanComputeSlot();
		}

		// The counter lives in a small host-visible buffer of the append buffer's own (see
		// CVulkanHardwareBuffer::getCounterBuffer()), bound by every dispatch next to the buffer it
		// counts for. Copying it out is a 4-byte buffer copy on the upload command buffer.
		void CVulkanDriver::copyStructureCount(scene::IComputeBuffer* dst, u32 dstByteOffset,
			scene::IComputeBuffer* appendBuffer)
		{
			if (!dst || !appendBuffer)
				return;

			CVulkanHardwareBuffer* dstHardware = prepareComputeBuffer(dst);
			CVulkanHardwareBuffer* srcHardware = prepareComputeBuffer(appendBuffer);
			if (!dstHardware || !srcHardware)
			{
				os::Printer::log("CVulkanDriver::copyStructureCount: needs a real source and destination "
					"buffer", ELL_ERROR);
				return;
			}
			if (!(srcHardware->getFlags() & (EHBF_COMPUTE_APPEND | EHBF_COMPUTE_CONSUME)))
			{
				os::Printer::log("CVulkanDriver::copyStructureCount: source has no hidden counter - create "
					"it with EHBF_COMPUTE_APPEND/CONSUME", ELL_ERROR);
				return;
			}
			if (dstByteOffset + sizeof(u32) > dstHardware->getSize())
			{
				os::Printer::log("CVulkanDriver::copyStructureCount: dstByteOffset past the end of dst", ELL_ERROR);
				return;
			}

			VkBuffer counter = srcHardware->getCounterBuffer(true);
			if (counter == VK_NULL_HANDLE)
				return;

			VkCommandBuffer cmd = beginUpload();
			if (cmd == VK_NULL_HANDLE)
				return;

			CVulkanCompute::bufferBarrier(cmd, counter, 0, VK_WHOLE_SIZE,
				VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			CVulkanCompute::bufferBarrier(cmd, dstHardware->getBuffer(), 0, VK_WHOLE_SIZE,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
				VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT);

			VkBufferCopy region = {};
			region.srcOffset = 0;
			region.dstOffset = dstByteOffset;
			region.size = sizeof(u32);
			vk::CmdCopyBuffer(cmd, counter, dstHardware->getBuffer(), 1, &region);

			// Every consumer of the count: the next dispatch (as a param), an indirect dispatch (as
			// its arguments), the read-back copy, and the host itself.
			CVulkanCompute::bufferBarrier(cmd, dstHardware->getBuffer(), 0, VK_WHOLE_SIZE,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
				VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
				VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT);
			endUploadAndWait(cmd);
		}

		// Applied at once rather than on the next bind as D3D11 does: every dispatch here has been
		// waited on, so nothing can still be counting into it, and an unbound buffer is no problem.
		void CVulkanDriver::resetStructureCount(scene::IComputeBuffer* appendBuffer, u32 value)
		{
			if (!appendBuffer)
				return;

			CVulkanHardwareBuffer* hardware = prepareComputeBuffer(appendBuffer);
			if (!hardware)
			{
				os::Printer::log("CVulkanDriver::resetStructureCount: buffer has no device buffer", ELL_WARNING);
				return;
			}
			if (!hardware->setCounterValue(value))
				os::Printer::log("CVulkanDriver::resetStructureCount: no counter could be created", ELL_ERROR);
		}

		bool CVulkanDriver::beginComputeReadback(scene::IComputeBuffer* buffer, u32 slot)
		{
			if (!buffer || slot >= EMCS_MAX_READBACK_SLOTS)
				return false;

			CVulkanHardwareBuffer* hardware = prepareComputeBuffer(buffer);
			if (!hardware)
				return false;
			return hardware->beginAsyncReadback(slot);
		}

		bool CVulkanDriver::tryReadComputeBuffer(scene::IComputeBuffer* buffer, u32 slot, void* dst,
			u32 bytes, bool wait)
		{
			if (!buffer || slot >= EMCS_MAX_READBACK_SLOTS || !buffer->getHardwareBuffer())
				return false;
			if (buffer->getHardwareBuffer()->getDriverType() != EDT_VULKAN)
				return false;

			// Not prepareComputeBuffer(): a poll must never trigger an upload of a dirty CPU copy.
			CVulkanHardwareBuffer* hardware = static_cast<CVulkanHardwareBuffer*>(buffer->getHardwareBuffer().get());
			return hardware->tryAsyncReadback(slot, dst, bytes, wait);
		}

		void CVulkanDriver::drawMeshBufferInstancedIndirect(const scene::IMeshBuffer* mb,
			scene::IComputeBuffer* instanceBuffer, u32 instanceStride,
			scene::IComputeBuffer* argBuffer, u32 byteOffset)
		{
			if (!mb || !instanceBuffer || !argBuffer || !instanceStride)
				return;
			if (!SceneOpen || !RenderingActive)
				return; // bindDrawState() would only warn; nothing can be recorded

			IVertexDescriptor* descriptor = mb->getVertexDescriptor();
			const u32 vbCount = mb->getVertexBufferCount();
			if (!descriptor || vbCount == 0 || vbCount > kMaxVertexStreams ||
				vbCount > Context.DeviceProperties.limits.maxVertexInputBindings)
				return;

			CVulkanHardwareBuffer* instances = prepareComputeBuffer(instanceBuffer);
			CVulkanHardwareBuffer* args = prepareComputeBuffer(argBuffer);
			if (!instances || !args)
			{
				os::Printer::log("CVulkanDriver::drawMeshBufferInstancedIndirect: instance or args buffer "
					"has no device buffer", ELL_ERROR);
				return;
			}
			if (!(instances->getFlags() & EHBF_VERTEX_ADDITIONAL_BIND))
			{
				os::Printer::log("CVulkanDriver::drawMeshBufferInstancedIndirect: the instance buffer "
					"needs EHBF_VERTEX_ADDITIONAL_BIND to be fetched as vertex data", ELL_ERROR);
				return;
			}
			if (!(args->getFlags() & EHBF_DRAW_INDIRECT_ARGS) || byteOffset % 4 != 0 ||
				byteOffset + 5 * sizeof(u32) > args->getSize())
			{
				os::Printer::log("CVulkanDriver::drawMeshBufferInstancedIndirect: the args buffer needs "
					"EHBF_DRAW_INDIRECT_ARGS and five u32 at byteOffset", ELL_ERROR);
				return;
			}
			if (!vk::CmdDrawIndexedIndirect)
				return;

			scene::IIndexBuffer* ib = mb->getIndexBuffer();
			if (!ib || ib->getIndexCount() == 0)
			{
				os::Printer::log("CVulkanDriver::drawMeshBufferInstancedIndirect: an indexed mesh buffer "
					"is required", ELL_ERROR);
				return;
			}

			// The per-instance stream comes from `instanceBuffer`, every other one from the mesh, as
			// in drawMeshBuffer(). The stride is baked into the pipeline from the descriptor, so the
			// caller's has to agree with it.
			VkBuffer vertexBuffers[kMaxVertexStreams] = {};
			VkDeviceSize vertexOffsets[kMaxVertexStreams] = {};
			for (u32 i = 0; i < vbCount; ++i)
			{
				if (descriptor->getInstanceDataStepRate(i) == EIDSR_PER_INSTANCE)
				{
					if (descriptor->getVertexSize(i) != instanceStride)
						os::Printer::log("CVulkanDriver::drawMeshBufferInstancedIndirect: instanceStride "
							"differs from the descriptor's per-instance vertex size, the descriptor's is "
							"used", ELL_WARNING);
					vertexBuffers[i] = instances->getBuffer();
					continue;
				}

				scene::IVertexBuffer* streamVb = mb->getVertexBuffer(i);
				if (!streamVb || streamVb->getVertexCount() == 0)
					return;

				auto streamHardware = streamVb->getHardwareBuffer();
				if (!streamHardware || streamHardware->getDriverType() != EDT_VULKAN)
					streamHardware = createHardwareBuffer(streamVb);
				else if (streamHardware->isRequiredUpdate())
					streamHardware->update(streamVb->getHardwareMappingHint(),
						streamVb->getVertexCount() * streamVb->getVertexSize(), streamVb->getVertices());
				if (!streamHardware)
					return;

				vertexBuffers[i] = static_cast<CVulkanHardwareBuffer*>(streamHardware.get())->getBuffer();
				if (vertexBuffers[i] == VK_NULL_HANDLE)
					return;
			}

			auto ibHardware = ib->getHardwareBuffer();
			if (!ibHardware || ibHardware->getDriverType() != EDT_VULKAN)
				ibHardware = createHardwareBuffer(ib);
			else if (ibHardware->isRequiredUpdate())
			{
				const u32 indexSize = (ib->getType() == EIT_32BIT) ? 4 : 2;
				ibHardware->update(ib->getHardwareMappingHint(), ib->getIndexCount() * indexSize, ib->getIndices());
			}
			if (!ibHardware)
				return;
			CVulkanHardwareBuffer* nativeIb = static_cast<CVulkanHardwareBuffer*>(ibHardware.get());
			if (nativeIb->getBuffer() == VK_NULL_HANDLE)
				return;

			const VkPrimitiveTopology topology = mapPrimitiveType(mb->getPrimitiveType());
			SVulkanVertexInputState vertexInput;
			resolveVulkanVertexInputState(descriptor, vertexInput, getUserMaterial(Material.MaterialType) != nullptr);
			if (!bindDrawState(Material, Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION],
				descriptor, vertexInput, topology))
				return;

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];
			vk::CmdBindVertexBuffers(frame.CommandBuffer, 0, vbCount, vertexBuffers, vertexOffsets);
			vk::CmdBindIndexBuffer(frame.CommandBuffer, nativeIb->getBuffer(), 0,
				(ib->getType() == EIT_32BIT) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
			// The five D3D arguments (IndexCountPerInstance, InstanceCount, StartIndexLocation,
			// BaseVertexLocation, StartInstanceLocation) are VkDrawIndexedIndirectCommand in order.
			vk::CmdDrawIndexedIndirect(frame.CommandBuffer, args->getBuffer(), byteOffset, 1, 0);
			endTransformFeedbackForDraw();
		}

		s32 CVulkanDriver::addComputeShader(const c8* computeShaderProgram,
			const c8* computeShaderEntryPointName, E_COMPUTE_SHADER_TYPE csCompileTarget,
			IShaderConstantSetCallBack* callback, s32 userData)
		{
			if (!Compute || !Compute->isReady())
			{
				os::Printer::log("CVulkanDriver::addComputeShader: compute is unavailable on this device",
					ELL_ERROR);
				return -1;
			}

			// The material compiles and reflects itself; the driver only registers it -- the same
			// split as registerUserShaderMaterial(). The source is text in the build's default
			// language: a bare pointer cannot carry a SPIR-V blob's length, see the file overload.
			CVulkanComputeMaterial* material = new CVulkanComputeMaterial();
			material->Name = "compute shader";
			// An in-memory source has no directory of its own; its includes resolve against the
			// working directory and media/shaders/, as on the D3D drivers.
			if (!material->compileFromSource(Context, EGSL_DEFAULT, computeShaderProgram, 0,
				computeShaderEntryPointName, FileSystem, nullptr))
			{
				material->drop();
				return -1;
			}

			if (callback)
			{
				callback->grab();
				material->CallBack = callback;
			}
			material->UserData = userData;

			const s32 materialType = addMaterialRenderer(material, nullptr);
			material->drop(); // addMaterialRenderer() grabbed it
			return materialType;
		}

		s32 CVulkanDriver::addComputeShaderFromFile(const io::path& computeShaderProgramFileName,
			const c8* computeShaderEntryPointName, E_COMPUTE_SHADER_TYPE csCompileTarget,
			IShaderConstantSetCallBack* callback, s32 userData)
		{
			if (!Compute || !Compute->isReady())
			{
				os::Printer::log("CVulkanDriver::addComputeShaderFromFile: compute is unavailable on this "
					"device", ELL_ERROR);
				return -1;
			}

			SVulkanSourceBytes bytes;
			if (!loadShaderSource(FileSystem, computeShaderProgramFileName, bytes) || bytes.size() < 2)
				return -1;

			// A file starting with the SPIR-V magic word is a pre-compiled module and goes in as
			// such, length included (it contains zero bytes); anything else is source text.
			E_GPU_SHADING_LANGUAGE lang = EGSL_DEFAULT;
			u32 length = 0;
			if (bytes.size() > SpirvHeaderSize)
			{
				u32 magic = 0;
				memcpy(&magic, bytes.data(), sizeof(magic));
				if (magic == SpirvMagicWord)
				{
					lang = EGSL_PCMP;
					length = static_cast<u32>(bytes.size() - 1);
				}
			}

			CVulkanComputeMaterial* material = new CVulkanComputeMaterial();
			material->Name = computeShaderProgramFileName;
			// Includes resolve against the file's own directory first.
			const io::path includeDirectory = FileSystem ? FileSystem->getFileDir(computeShaderProgramFileName) : io::path();
			if (!material->compileFromSource(Context, lang, bytes.data(), length, computeShaderEntryPointName,
				FileSystem, includeDirectory.size() ? includeDirectory.c_str() : nullptr))
			{
				material->drop();
				return -1;
			}

			if (callback)
			{
				callback->grab();
				material->CallBack = callback;
			}
			material->UserData = userData;

			const s32 materialType = addMaterialRenderer(material, nullptr);
			material->drop();
			return materialType;
		}

		s32 CVulkanDriver::getComputeShaderConstantID(const c8* name)
		{
			CVulkanComputeMaterial* material = getComputeMaterial(ActiveMaterialRendererIndex);
			return (material && name) ? material->getVariableID(name) : -1;
		}

		bool CVulkanDriver::setComputeShaderConstant(s32 index, const f32* floats, int count)
		{
			CVulkanComputeMaterial* material = getComputeMaterial(ActiveMaterialRendererIndex);
			return material ? material->setVariable(index, floats, count) : false;
		}

		bool CVulkanDriver::setComputeShaderConstant(s32 index, const s32* ints, int count)
		{
			CVulkanComputeMaterial* material = getComputeMaterial(ActiveMaterialRendererIndex);
			return material ? material->setVariable(index, ints, count) : false;
		}

		bool CVulkanDriver::setComputeShaderConstant(s32 index, const u32* uints, int count)
		{
			CVulkanComputeMaterial* material = getComputeMaterial(ActiveMaterialRendererIndex);
			return material ? material->setVariableRaw(index, uints, (u32)(count * sizeof(u32))) : false;
		}

		bool CVulkanDriver::setComputeShaderConstant(s32 index, const f64* doubles, int count)
		{
			CVulkanComputeMaterial* material = getComputeMaterial(ActiveMaterialRendererIndex);
			return material ? material->setVariableRaw(index, doubles, (u32)(count * sizeof(f64))) : false;
		}

		bool CVulkanDriver::setComputeShaderConstant(s32 index, const s64* longs, int count)
		{
			CVulkanComputeMaterial* material = getComputeMaterial(ActiveMaterialRendererIndex);
			return material ? material->setVariableRaw(index, longs, (u32)(count * sizeof(s64))) : false;
		}

		bool CVulkanDriver::setComputeShaderConstant(s32 index, const u64* ulongs, int count)
		{
			CVulkanComputeMaterial* material = getComputeMaterial(ActiveMaterialRendererIndex);
			return material ? material->setVariableRaw(index, ulongs, (u32)(count * sizeof(u64))) : false;
		}

		// ============================ textures and render targets ============================

		ITexture* CVulkanDriver::addUAVTexture(const core::dimension2d<u32>& size, const io::path& name,
			const ECOLOR_FORMAT format)
		{
			CVulkanTexture* texture = new CVulkanTexture(Context, *this, size, format, false, name, 1, true);
			if (!texture->hasDeviceResource() || !texture->isUnorderedAccess())
			{
				os::Printer::log("CVulkanDriver::addUAVTexture: image creation failed", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}

			CNullDriver::addTexture(texture);
			texture->drop();
			return texture;
		}

		// The array path of CNullDriver::getTexture(files, type): the slices are already textures of
		// this driver, and the base adds the result to its cache itself.
		ITexture* CVulkanDriver::createDeviceDependentTexture(const core::array<ITexture*>& surfaces,
			const E_TEXTURE_TYPE Type, const io::path& name, void* mipmapData)
		{
			CVulkanTexture* texture = new CVulkanTexture(Context, *this, surfaces, Type, name);
			if (!texture->hasDeviceResource())
			{
				os::Printer::log("CVulkanDriver: could not create the array texture", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}
			return texture;
		}

		ITexture* CVulkanDriver::addRenderTargetTexture(const core::dimension2d<u32>& size,
			const io::path& name, const ECOLOR_FORMAT format, u32 sampleCount, u32 sampleQuality,
			u32 arraySlices)
		{
			const ECOLOR_FORMAT actual = (format == ECF_UNKNOWN) ? ECF_A8R8G8B8 : format;

			// The same two rules as CD3D12Driver::addRenderTargetTexture(): an array is never
			// multisampled (its slices are written one at a time by full-screen blits), and a count
			// the device cannot rasterize into colour and depth at once falls back to one sample.
			u32 samples = (sampleCount > 1) ? sampleCount : 1;
			if (samples > 1 && arraySlices > 1)
			{
				os::Printer::log("CVulkanDriver::addRenderTargetTexture: an array target cannot be "
					"multisampled, a single-sample array is created", name, ELL_WARNING);
				samples = 1;
			}
			if (samples > 1 && queryMultisampleLevels(actual, samples) == 0)
			{
				os::Printer::log("CVulkanDriver::addRenderTargetTexture: the device does not support "
					"this sample count, a single-sample target is created", name, ELL_WARNING);
				samples = 1;
			}

			CVulkanTexture* texture = new CVulkanTexture(Context, *this, size, actual, true, name,
				arraySlices ? arraySlices : 1, false, samples);
			if (!texture->hasDeviceResource())
			{
				os::Printer::log("CVulkanDriver::addRenderTargetTexture: image creation failed", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}

			CNullDriver::addTexture(texture);
			texture->drop();
			return texture;
		}

		// One slice, no depth: slices are written by a full-screen blit, never depth-tested -- the
		// same choice CD3D11Driver::setRenderTargetSlice() makes.
		bool CVulkanDriver::setRenderTargetSlice(video::ITexture* texture, u32 arraySlice,
			bool clearTarget, SColor color)
		{
			if (!texture || texture->getDriverType() != EDT_VULKAN || !texture->isRenderTarget())
			{
				os::Printer::log("CVulkanDriver::setRenderTargetSlice: not a Vulkan render target", ELL_ERROR);
				return false;
			}
			if (!SceneOpen)
			{
				os::Printer::log("CVulkanDriver::setRenderTargetSlice: only valid between beginScene() "
					"and endScene()", ELL_WARNING);
				return false;
			}

			CVulkanTexture* colorTexture = static_cast<CVulkanTexture*>(texture);
			if (arraySlice >= colorTexture->getLayerCount())
			{
				os::Printer::log("CVulkanDriver::setRenderTargetSlice: slice out of range", ELL_ERROR);
				return false;
			}

			endRendering();
			unbindRenderTarget();
			MrtBlend.reset();

			if (!RenderTarget->setTarget(colorTexture, nullptr, nullptr, arraySlice))
			{
				activateRenderTarget(false, false, color);
				return false;
			}

			RenderTargetActive = true;
			return activateRenderTarget(clearTarget, false, color);
		}

		bool CVulkanDriver::setRenderTarget(E_RENDER_TARGET target, bool clearTarget, bool clearZBuffer,
			SColor color)
		{
			if (target == ERT_FRAME_BUFFER)
				return setRenderTarget(static_cast<video::ITexture*>(nullptr), clearTarget, clearZBuffer, color);

			os::Printer::log("CVulkanDriver::setRenderTarget: only ERT_FRAME_BUFFER is supported (no "
				"stereo/aux buffers)", ELL_WARNING);
			return false;
		}

		bool CVulkanDriver::copyTexture(ITexture* dest, ITexture* source, u32 destSlice)
		{
			if (!dest || !source || dest == source)
				return false;

			if (dest->getDriverType() != EDT_VULKAN || source->getDriverType() != EDT_VULKAN)
			{
				os::Printer::log("CVulkanDriver::copyTexture: both textures must belong to this driver", ELL_ERROR);
				return false;
			}

			CVulkanTexture* d = static_cast<CVulkanTexture*>(dest);
			CVulkanTexture* s = static_cast<CVulkanTexture*>(source);
			if (!d->hasDeviceResource() || !s->hasDeviceResource())
				return false;

			// vkCmdCopyImage moves texels without conversion: the VkFormats have to agree (not just
			// the ECOLOR_FORMATs, ECF_R8G8B8 being promoted on upload) and so do the sizes.
			if (d->getSize() != s->getSize() || d->getVkFormat() != s->getVkFormat())
			{
				os::Printer::log("CVulkanDriver::copyTexture: size or format mismatch", ELL_ERROR);
				return false;
			}
			if (destSlice >= d->getLayerCount())
			{
				os::Printer::log("CVulkanDriver::copyTexture: destination slice out of range", ELL_ERROR);
				return false;
			}

			// Inside a scene the copy joins the frame's command buffer, in order with the draws that
			// produced the source; the rendering instance has to be suspended around it, transfer
			// commands being illegal inside one. Outside a scene it goes on a one-shot buffer.
			const bool onFrame = SceneOpen;
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			if (onFrame)
			{
				suspendRendering();
				cmd = Frames[CurrentFrameIndex].CommandBuffer;
			}
			else
			{
				cmd = beginUpload();
				if (cmd == VK_NULL_HANDLE)
					return false;
			}

			const VkImageLayout sourcePrevious = s->getImageLayout();
			const VkImageLayout destPrevious = d->getImageLayout();
			s->transitionTo(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			d->transitionTo(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

			// Every level both carry; layer 0 of the source into `destSlice`.
			const u32 levels = core::min_(s->getMipLevelCount(), d->getMipLevelCount());
			std::vector<VkImageCopy> regions(levels);
			for (u32 level = 0; level < levels; ++level)
			{
				VkImageCopy& region = regions[level];
				region = VkImageCopy();
				region.srcSubresource.aspectMask = s->getAspectMask();
				region.srcSubresource.mipLevel = level;
				region.srcSubresource.layerCount = 1;
				region.dstSubresource.aspectMask = d->getAspectMask();
				region.dstSubresource.mipLevel = level;
				region.dstSubresource.baseArrayLayer = destSlice;
				region.dstSubresource.layerCount = 1;
				region.extent.width = core::max_(1u, s->getSize().Width >> level);
				region.extent.height = core::max_(1u, s->getSize().Height >> level);
				region.extent.depth = 1;
			}
			vk::CmdCopyImage(cmd, s->getImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				d->getImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions.data());

			// Both go back where they were; nothing may transition back to UNDEFINED, so a texture
			// that was never written lands in the sampled layout instead.
			s->transitionTo(cmd, (sourcePrevious == VK_IMAGE_LAYOUT_UNDEFINED) ?
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : sourcePrevious);
			d->transitionTo(cmd, (destPrevious == VK_IMAGE_LAYOUT_UNDEFINED) ?
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : destPrevious);

			if (onFrame)
				resumeRendering();
			else
				endUploadAndWait(cmd);
			return true;
		}

		IVideoDriver* CVulkanDriver::createDeferredContext()
		{
			CVulkanDeferredContext* deferred = new CVulkanDeferredContext(this);
			// Same rule as the D3D drivers: never hand back a context whose first draw would fail.
			if (!deferred->isReady())
			{
				os::Printer::log("CVulkanDriver::createDeferredContext: the deferred context could not "
					"be built, see the messages above", ELL_ERROR);
				deferred->drop();
				return nullptr;
			}
			return deferred;
		}

		void CVulkanDriver::executeDeferredContext(IDeferredContext* context)
		{
			if (!context)
			{
				os::Printer::log("CVulkanDriver::executeDeferredContext: null context", ELL_ERROR);
				return;
			}
			context->execute(this);
		}

		IVideoDriver* createVulkanDriver(const irr::SIrrlichtCreationParameters& params,
			io::IFileSystem* io, HWND window)
		{
			CVulkanDriver* driver = new CVulkanDriver(params, io, window);
			if (!driver->initDriver(window))
			{
				driver->drop();
				driver = nullptr;
			}
			return driver;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
