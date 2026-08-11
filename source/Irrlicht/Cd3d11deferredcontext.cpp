#include "CD3D11DeferredContext.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "os.h"

using namespace irr;
using namespace irr::video;

// ============================================================================
// Base-class constructor call uses `immediate`'s own Params/FileSystem --
// confirmed safe from CD3D11Driver.cpp: the CD3D11Driver(params, io, window)
// constructor body only stores members and does cheap clip-plane/matrix
// setup. It does NOT create a device, swapchain, or window -- that happens
// separately in initDriver()/BuildDriverInternal(), which this class never
// calls. So reusing the existing constructor here (rather than adding a new
// overload to CD3D11Driver) is safe and requires no changes to that class's
// constructor.
//
// `window` is passed as 0 -- confirmed unused by the constructor body (no
// member stores it there).
// ============================================================================

CD3D11DeferredContext::CD3D11DeferredContext(CD3D11Driver* immediate)
	: CD3D11Driver(immediate->Params, immediate->FileSystem, (HWND)0)
	, ImmediateDriver(immediate)
	, CompletionQuery(NULL)
{
	// Share the device -- no new device created.
	Device = immediate->Device;
	Device->AddRef();

	// initDriver()'s swapchain setup (skipped here) is the only other place this gets set; anything
	// reading it off a deferred context (e.g. CD3D11HardwareBuffer's device pointer) needs it too.
	ExposedData.D3D11.D3DDev11 = Device;

	HRESULT hr = Device->CreateDeferredContext(0, &Context);
	if (FAILED(hr))
	{
		os::Printer::log("CD3D11DeferredContext: CreateDeferredContext failed", ELL_ERROR);
		Context = NULL;
		return;
	}

	// Fresh CallBridge targeting the deferred context -- empty state cache,
	// so the first setBlendState/setRasterizerState/etc. during recording
	// always actually emits the D3D11 call. See CD3D11CallBridge's
	// explicitContext parameter (added for this purpose).
	BridgeCalls = new CD3D11CallBridge(Device, this, Context);

	// Device/adapter-capability fields -- describe the physical device, not
	// bound state, so copying (not sharing) is correct and sufficient.
	// NOTE: confirm these are declared `protected` (not `private`) in your
	// tree -- assumed based on being adjacent to Context/BridgeCalls, which
	// are confirmed protected. If any of these fail to compile due to
	// access level, add a small protected/public accessor to CD3D11Driver
	// for that specific field.
	DriverType = immediate->DriverType;
	ColorFormat = immediate->ColorFormat;
	D3DColorFormat = immediate->D3DColorFormat;
	DepthStencilFormat = immediate->DepthStencilFormat;
	VendorID = immediate->VendorID;
	VendorName = immediate->VendorName;
	MaxTextureUnits = immediate->MaxTextureUnits;
	MaxActiveLights = immediate->MaxActiveLights;
	AlphaToCoverageSupport = immediate->AlphaToCoverageSupport;

	// The base ctor derived this from Params.WindowSize, which OnResize never updates on this
	// object -- callers scaling by getScreenSize() (e.g. scissor rects) would drift after a resize.
	ScreenSize = immediate->getScreenSize();

	// Deliberately NOT called: initDriver(), BuildDriverInternal(),
	// createMaterialRenderers(). No swapchain, no backbuffer, no adapter
	// enumeration, no duplicate material renderer table -- getRendererFor()
	// forwards to `immediate` instead.
}

CD3D11DeferredContext::~CD3D11DeferredContext()
{
	if (CompletionQuery)
		CompletionQuery->Release();

	// CD3D11Driver's own destructor doesn't appear to delete BridgeCalls
	// (checked: no `delete BridgeCalls` found in it) -- clean up our own
	// explicitly rather than assume. Context itself IS released by the
	// base ~CD3D11Driver() (confirmed: `if (Context) { ... Context->Release(); }`),
	// so not repeated here.
	if (BridgeCalls)
	{
		delete BridgeCalls;
		BridgeCalls = NULL;
	}

	// Base ~CD3D11Driver() destructor runs next: deleteMaterialRenders()/
	// deleteVertexDescriptors()/deleteAllTextures()/etc. all safely no-op
	// on empty vectors (this object never populated them), and
	// `if (Device) Device->Release();` correctly balances the AddRef()
	// above without freeing early, since `immediate` still holds its own
	// reference.
}

bool CD3D11DeferredContext::beginScene(bool, bool, SColor, const SExposedVideoData&, core::rect<s32>*)
{
	return true;
}

bool CD3D11DeferredContext::endScene()
{
	return true;
}

void CD3D11DeferredContext::execute(IVideoDriver* driver)
{
	if (!Context)
	{
		os::Printer::log("CD3D11DeferredContext::execute: no deferred context (construction failed?)", ELL_ERROR);
		return;
	}

	CD3D11Driver* target = driver ? static_cast<CD3D11Driver*>(driver) : ImmediateDriver;

	ID3D11CommandList* commandList = NULL;
	HRESULT hr = Context->FinishCommandList(FALSE, &commandList);
	if (FAILED(hr))
	{
		os::Printer::log("CD3D11DeferredContext::execute: FinishCommandList failed", ELL_ERROR);
		return;
	}

	target->getContext()->ExecuteCommandList(commandList, FALSE);
	commandList->Release();

	// FinishCommandList reset THIS context to default state, and ExecuteCommandList left the target
	// holding whatever the list ended with -- neither went through a bridge, so both caches now lie.
	BridgeCalls->invalidateCache();
	target->getBridgeCalls()->invalidateCache();

	createCompletionQuery();
	if (CompletionQuery)
		target->getContext()->End(CompletionQuery);
}

void CD3D11DeferredContext::beginRecording()
{
	// FinishCommandList (in execute()) already resets the deferred D3D11
	// context to a clean recording state as a side effect. No-op, present
	// only to satisfy IDeferredContext's contract.
}

IVideoDriver* CD3D11DeferredContext::createDeferredContext()
{
	os::Printer::log("CD3D11DeferredContext::createDeferredContext: nested deferred contexts are not supported", ELL_ERROR);
	return NULL;
}

void CD3D11DeferredContext::createCompletionQuery()
{
	if (CompletionQuery)
	{
		CompletionQuery->Release();
		CompletionQuery = NULL;
	}
	if (!Device)
		return;

	D3D11_QUERY_DESC desc = {};
	desc.Query = D3D11_QUERY_EVENT;
	Device->CreateQuery(&desc, &CompletionQuery);
}

void CD3D11DeferredContext::waitForCompletion()
{
	if (!CompletionQuery || !ImmediateDriver)
		return;

	while (ImmediateDriver->getContext()->GetData(CompletionQuery, NULL, 0, 0) != S_OK)
	{
		// Busy-wait. Replace with Sleep(0)/SwitchToThread() between polls
		// if this shows up in profiling.
	}
}
#endif // _IRR_COMPILE_WITH_DIRECT3D_11_