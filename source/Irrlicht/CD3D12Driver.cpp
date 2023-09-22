#include "CD3D12Driver.h"

irr::video::CD3D12Driver::CD3D12Driver(const irr::SIrrlichtCreationParameters& params, io::IFileSystem* io, HWND window)
	: CD3D11Driver(params, io, window),
	DeviceADV(NULL),
	Output(NULL)
{
#ifdef _DEBUG
	setDebugName("CD3D12Driver");
#endif
}

bool irr::video::CD3D12Driver::initDriver(HWND hwnd, bool pureSoftware)
{
	UINT createFactoryFlags = 0;

#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;

#endif
	(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&DXGIFactory)));
	IDXGIAdapter1* dxgiAdapter1;
	SIZE_T maxDedicatedVideoMemory = 0;

	for (UINT i = 0; DXGIFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
	{

		DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;

		dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);



		// Check to see if the adapter can create a D3D12 device without actually 

		// creating it. The adapter with the largest dedicated video memory

		// is favored.



	}
	D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&DeviceADV));

	//3d command queue description
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;
	// allocate array of command queues 
	for (int i = 0; i < 16; i++)
	{
		DeviceADV->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_CommandQueues[i]));
	}
	// compute command queue description
	D3D12_COMMAND_QUEUE_DESC computeDesc = {};
	computeDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
	computeDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	computeDesc.NodeMask = 0;
	// allocate array of command queues
	for (int i = 0; i < 16; i++)
	{
		DeviceADV->CreateCommandQueue(&computeDesc, IID_PPV_ARGS(&g_ComputeCommandQueues[i]));
	}

	// copy command queue description
	D3D12_COMMAND_QUEUE_DESC copyDesc = {};
	copyDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	copyDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	copyDesc.NodeMask = 0;
	// allocate array of command queues
	for (int i = 0; i < 16; i++)
	{
		DeviceADV->CreateCommandQueue(&copyDesc, IID_PPV_ARGS(&g_CopyCommandQueues[i]));
	}
	

	//SwapChain = CreateSwapChain(hwnd, g_CommandQueue, Params.WindowSize.Width, Params.WindowSize.Height, 3);

	//g_CurrentBackBufferIndex = SwapChain->GetCurrentBackBufferIndex();

	g_RTVDescriptorHeap = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3);

	g_RTVDescriptorSize = DeviceADV->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	// Device flags
	UINT deviceFlags = 0;
#ifdef _DEBUG
	deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	// feature levels variable for creating device
	
	D3D_FEATURE_LEVEL featureLevels[] =
	{
		          D3D_FEATURE_LEVEL_12_1,
		          D3D_FEATURE_LEVEL_12_0,
		          D3D_FEATURE_LEVEL_11_1,
		          D3D_FEATURE_LEVEL_11_0,
		          D3D_FEATURE_LEVEL_10_1,
		          D3D_FEATURE_LEVEL_10_0,
		          D3D_FEATURE_LEVEL_9_3,
		          D3D_FEATURE_LEVEL_9_2,
		          D3D_FEATURE_LEVEL_9_1
	};

	//create d3d11 on top of d3d12
	D3D11On12CreateDevice(DeviceADV, deviceFlags, featureLevels,9, reinterpret_cast<IUnknown**>(g_CommandQueues),1, 0
		, &Device, &Context, &FeatureLevel);
	Name+= "11 on 12 Feature:"; 
	Name +=
		(FeatureLevel == D3D_FEATURE_LEVEL_12_1) ? "12.1" :
		(FeatureLevel == D3D_FEATURE_LEVEL_12_0) ? "12.0" :
		(FeatureLevel == D3D_FEATURE_LEVEL_11_1) ? "11.1" :
		(FeatureLevel == D3D_FEATURE_LEVEL_11_0) ? "11.0" :
		(FeatureLevel == D3D_FEATURE_LEVEL_10_1) ? "10.1" : "10.0";

	HRESULT hr;
	BuildDriverInternal(hr, hwnd);
	return true;
}

irr::video::CD3D12Driver::~CD3D12Driver()
{
	//Flush(g_CommandQueue, Fence, g_FenceValue, g_FenceEvent);
}
