/*
* (C) 2025 badasahog. All Rights Reserved
*
* The above copyright notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#undef _CRT_SECURE_NO_WARNINGS
#include <shellscalingapi.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#ifdef _DEBUG
#include <dxgidebug.h>
#endif

#include <cglm/cglm.h>
#include <cglm/struct.h>
#include <cglm/call.h>
#include <cglm/cam.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <stdalign.h>

#pragma comment(linker, "/DEFAULTLIB:D3d12.lib")
#pragma comment(linker, "/DEFAULTLIB:Shcore.lib")
#pragma comment(linker, "/DEFAULTLIB:DXGI.lib")
#pragma comment(linker, "/DEFAULTLIB:dxguid.lib")

__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
__declspec(dllexport) UINT D3D12SDKVersion = 612;
__declspec(dllexport) char* D3D12SDKPath = ".\\D3D12\\";

HANDLE ConsoleHandle;
ID3D12Device2* Device;

inline void THROW_ON_FAIL_IMPL(HRESULT hr, int line)
{
	if (hr == 0x887A0005)//device removed
	{
		THROW_ON_FAIL_IMPL(ID3D12Device2_GetDeviceRemovedReason(Device), line);
	}

	if (FAILED(hr))
	{
		LPWSTR messageBuffer;
		DWORD formattedErrorLength = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			hr,
			MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
			(LPWSTR)&messageBuffer,
			0,
			NULL
		);

		if (formattedErrorLength == 0)
			WriteConsoleA(ConsoleHandle, "an error occured, unable to retrieve error message\n", 51, NULL, NULL);
		else
		{
			WriteConsoleA(ConsoleHandle, "an error occured: ", 18, NULL, NULL);
			WriteConsoleW(ConsoleHandle, messageBuffer, formattedErrorLength, NULL, NULL);
			WriteConsoleA(ConsoleHandle, "\n", 1, NULL, NULL);
			LocalFree(messageBuffer);
		}

		char buffer[50];
		int stringlength = _snprintf_s(buffer, 50, _TRUNCATE, "error code: 0x%X\nlocation:line %i\n", hr, line);
		WriteConsoleA(ConsoleHandle, buffer, stringlength, NULL, NULL);

		RaiseException(0, EXCEPTION_NONCONTINUABLE, 0, NULL);
	}
}

#define THROW_ON_FAIL(x) THROW_ON_FAIL_IMPL(x, __LINE__)

#define THROW_ON_FALSE(x) if((x) == FALSE) THROW_ON_FAIL(HRESULT_FROM_WIN32(GetLastError()))

#define VALIDATE_HANDLE(x) if((x) == NULL || (x) == INVALID_HANDLE_VALUE) THROW_ON_FAIL(HRESULT_FROM_WIN32(GetLastError()))

inline void MEMCPY_VERIFY_IMPL(errno_t error, int line)
{
	if (error != 0)
	{
		char buffer[28];
		int stringlength = _snprintf_s(buffer, 28, _TRUNCATE, "memcpy failed on line %i\n", line);
		WriteConsoleA(ConsoleHandle, buffer, stringlength, NULL, NULL);
		RaiseException(0, EXCEPTION_NONCONTINUABLE, 0, NULL);
	}
}

#define MEMCPY_VERIFY(x) MEMCPY_VERIFY_IMPL(x, __LINE__)

#define OffsetPointer(x, offset) ((typeof(x))((char*)x + (offset)))

#define BUFFER_COUNT 3
#define WM_INIT (WM_USER + 1)

static const int MESHFILE_PROLOG = 'MSHL';
static const wchar_t* MESHFILE_NAME = L"Dragon_LOD0.bin";
static const wchar_t* MESH_SHADER_FILE = L"MeshletMS.cso";
static const wchar_t* PIXEL_SHADER_FILE = L"MeshletPS.cso";

static const bool bWarp = false;
static const LPCTSTR WindowClassName = L"DXSampleClass";

static const DXGI_FORMAT RTV_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
static const DXGI_FORMAT DEPTH_BUFFER_FORMAT = DXGI_FORMAT_D32_FLOAT;

//gpu aligned:
struct SceneConstantBuffer
{
	alignas(256) mat4 World;
	mat4 WorldView;
	mat4 WorldViewProj;
	uint32_t DrawMeshlets;
};

enum EType
{
	ATTRIBUTE_TYPE_POSITION,
	ATTRIBUTE_TYPE_NORMAL,
	ATTRIBUTE_TYPE_TEXCOORD,
	ATTRIBUTE_TYPE_TANGENT,
	ATTRIBUTE_TYPE_BITANGENT,
	ATTRIBUTE_TYPE_COUNT
};

struct Subset
{
	uint32_t Offset;
	uint32_t Count;
};

struct Meshlet
{
	uint32_t VertCount;
	uint32_t VertOffset;
	uint32_t PrimCount;
	uint32_t PrimOffset;
};

struct PackedTriangle
{
	uint32_t i0 : 10;
	uint32_t i1 : 10;
	uint32_t i2 : 10;
};

struct BoundingSphere
{
	vec3 Center;
	float Radius;
};

struct CullData
{
	float BoundingSphere[4]; // xyz = center, w = radius
	uint8_t NormalCone[4];  // xyz = axis, w = -cos(a + 90)
	float ApexOffset;     // apex = center - axis * offset
};

struct VertexBuffer
{
	const uint8_t* Verts;
	uint32_t Count;
	uint32_t Stride;
};

struct Mesh
{
	D3D12_INPUT_ELEMENT_DESC LayoutElems[ATTRIBUTE_TYPE_COUNT];
	D3D12_INPUT_LAYOUT_DESC LayoutDesc;

	struct VertexBuffer* VertexBuffers;
	uint32_t VertexBufferCount;

	uint32_t VertexCount;
	struct BoundingSphere BoundingSphere;
	const struct Subset* IndexSubsets;
	uint32_t IndexSubsetCount;

	const uint8_t* IndexBuffer;
	uint32_t IndexBufferSize;

	uint32_t IndexSize;
	uint32_t IndexCount;

	const struct Subset* MeshletSubsets;
	uint32_t MeshletSubsetCount;

	const struct Meshlet* Meshlets;
	uint32_t MeshletCount;

	const uint8_t* UniqueVertexIndices;
	uint32_t UniqueVertexIndexCount;

	const struct PackedTriangle* PrimitiveIndices;
	uint32_t PrimitiveIndexCount;

	const struct CullData* CullingData;
	uint32_t CullingDataCount;

	ID3D12Resource** VertexResources;
	D3D12_VERTEX_BUFFER_VIEW* VBViews;
	D3D12_INDEX_BUFFER_VIEW IBView;
	ID3D12Resource* IndexResource;
	ID3D12Resource* MeshletResource;
	ID3D12Resource* UniqueVertexIndexResource;
	ID3D12Resource* PrimitiveIndexResource;
	ID3D12Resource* CullDataResource;
	ID3D12Resource* MeshInfoResource;
};

struct SyncObjects
{
	UINT FrameIndex;
	UINT FrameCounter;
	HANDLE FenceEvent;
	ID3D12Fence* Fence[BUFFER_COUNT];
	UINT64 FenceValues[BUFFER_COUNT];
};

enum FileVersion
{
	FILE_VERSION_INITIAL = 0,
	CURRENT_FILE_VERSION = FILE_VERSION_INITIAL
};

struct ObjectInfo
{
	struct Mesh* MeshList;
	uint32_t MeshCount;
};

struct DxObjects
{
	IDXGISwapChain3* SwapChain;
	ID3D12Resource* RenderTargets[BUFFER_COUNT];
	ID3D12Resource* DepthStencil;
	ID3D12CommandAllocator* CommandAllocators[BUFFER_COUNT];
	ID3D12CommandQueue* CommandQueue;
	ID3D12RootSignature* RootSignature;
	ID3D12DescriptorHeap* RtvHeap;
	ID3D12DescriptorHeap* DsvHeap;
	ID3D12PipelineState* PipelineState;
	ID3D12Resource* ConstantBuffer;
	UINT RtvDescriptorSize;
	UINT DsvDescriptorSize;
	ID3D12GraphicsCommandList7* CommandList;
	UINT8* CbvDataBegin;
};

struct WindowProcPayload
{
	struct SyncObjects* SyncObjects;
	struct DxObjects* DxObjects;
	struct ObjectInfo* ObjectInfo;
	bool bTearingSupport;
};

LRESULT CALLBACK PreInitProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK IdleProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam);
void WaitForPreviousFrame(struct SyncObjects* SyncObjects, struct DxObjects* DxObjects);

int main()
{
	ConsoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);

	THROW_ON_FAIL(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE));

	HINSTANCE Instance = GetModuleHandleW(NULL);

	HICON Icon = LoadIconW(NULL, IDI_APPLICATION);
	HCURSOR Cursor = LoadCursorW(NULL, IDC_ARROW);

	WNDCLASSEXW WindowClass = { 0 };
	WindowClass.cbSize = sizeof(WNDCLASSEXW);
	WindowClass.style = CS_HREDRAW | CS_VREDRAW;
	WindowClass.lpfnWndProc = PreInitProc;
	WindowClass.cbClsExtra = 0;
	WindowClass.cbWndExtra = 0;
	WindowClass.hInstance = Instance;
	WindowClass.hIcon = Icon;
	WindowClass.hCursor = Cursor;
	WindowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 2);
	WindowClass.lpszMenuName = NULL;
	WindowClass.lpszClassName = WindowClassName;
	WindowClass.hIconSm = Icon;

	ATOM WindowClassAtom = RegisterClassExW(&WindowClass);
	if (WindowClassAtom == 0)
		THROW_ON_FAIL(HRESULT_FROM_WIN32(GetLastError()));

	RECT WindowRect = { 0 };
	WindowRect.left = 0;
	WindowRect.top = 0;
	WindowRect.right = 1280;
	WindowRect.bottom = 720;

	THROW_ON_FALSE(AdjustWindowRect(&WindowRect, WS_OVERLAPPEDWINDOW, FALSE));

	HWND Window = CreateWindowExW(
		0,
		WindowClass.lpszClassName,
		NULL,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		WindowRect.right - WindowRect.left,
		WindowRect.bottom - WindowRect.top,
		NULL,
		NULL,
		Instance,
		NULL);

	THROW_ON_FALSE(ShowWindow(Window, SW_SHOW));
	
#ifdef _DEBUG
	ID3D12Debug6* DebugController;

	{
		ID3D12Debug* DebugControllerV1;
		THROW_ON_FAIL(D3D12GetDebugInterface(&IID_ID3D12Debug, &DebugControllerV1));
		THROW_ON_FAIL(ID3D12Debug_QueryInterface(DebugControllerV1, &IID_ID3D12Debug6, &DebugController));
		ID3D12Debug_Release(DebugControllerV1);
	}

	ID3D12Debug6_EnableDebugLayer(DebugController);
	ID3D12Debug6_SetEnableSynchronizedCommandQueueValidation(DebugController, TRUE);
	ID3D12Debug6_SetGPUBasedValidationFlags(DebugController, D3D12_GPU_BASED_VALIDATION_FLAGS_DISABLE_STATE_TRACKING);
	ID3D12Debug6_SetEnableGPUBasedValidation(DebugController, TRUE);
#endif

	IDXGIFactory6* Factory;
#ifdef _DEBUG
	THROW_ON_FAIL(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, &IID_IDXGIFactory6, &Factory));
#else
	THROW_ON_FAIL(CreateDXGIFactory2(0, &IID_IDXGIFactory6, &Factory));
#endif

	IDXGIAdapter1* Adapter;

	if (bWarp)
	{
		THROW_ON_FAIL(IDXGIFactory6_EnumWarpAdapter(Factory, &IID_IDXGIAdapter1, &Adapter));
	}
	else
	{
		THROW_ON_FAIL(IDXGIFactory6_EnumAdapterByGpuPreference(Factory, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, &Adapter));
	}

	THROW_ON_FAIL(D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device2, &Device));

	THROW_ON_FAIL(IDXGIAdapter1_Release(Adapter));

#ifdef _DEBUG
	ID3D12InfoQueue* InfoQueue;
	THROW_ON_FAIL(ID3D12Device_QueryInterface(Device, &IID_ID3D12InfoQueue, &InfoQueue));

	THROW_ON_FAIL(ID3D12InfoQueue_SetBreakOnSeverity(InfoQueue, D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE));
	THROW_ON_FAIL(ID3D12InfoQueue_SetBreakOnSeverity(InfoQueue, D3D12_MESSAGE_SEVERITY_ERROR, TRUE));
	THROW_ON_FAIL(ID3D12InfoQueue_SetBreakOnSeverity(InfoQueue, D3D12_MESSAGE_SEVERITY_WARNING, TRUE));
#endif

	{
		D3D12_FEATURE_DATA_SHADER_MODEL ShaderModel = { D3D_SHADER_MODEL_6_5 };
		THROW_ON_FAIL(ID3D12Device2_CheckFeatureSupport(Device, D3D12_FEATURE_SHADER_MODEL, &ShaderModel, sizeof(ShaderModel)));
		if (ShaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_5)
		{
			WriteConsoleW(ConsoleHandle, L"Insufficient Shader Model Support", 33, NULL, NULL);
			return EXIT_FAILURE;
		}
	}

	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS7 Features = { 0 };
		THROW_ON_FAIL(ID3D12Device2_CheckFeatureSupport(Device, D3D12_FEATURE_D3D12_OPTIONS7, &Features, sizeof(Features)));
		if (Features.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
		{
			WriteConsoleW(ConsoleHandle, L"Insufficient Mesh Shader Support", 32, NULL, NULL);
			return EXIT_FAILURE;
		}
	}

	struct DxObjects DxObjects = { 0 };

	{
		D3D12_COMMAND_QUEUE_DESC QueueDesc = { 0 };
		QueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		QueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		THROW_ON_FAIL(ID3D12Device2_CreateCommandQueue(Device, &QueueDesc, &IID_ID3D12CommandQueue, &DxObjects.CommandQueue));
	}

	{
		DXGI_SWAP_CHAIN_DESC SwapChainDesc = { 0 };
		SwapChainDesc.BufferDesc.Width = 1;
		SwapChainDesc.BufferDesc.Height = 1;
		SwapChainDesc.BufferDesc.Format = RTV_FORMAT;
		SwapChainDesc.SampleDesc.Count = 1;
		SwapChainDesc.SampleDesc.Quality = 0;
		SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		SwapChainDesc.BufferCount = BUFFER_COUNT;
		SwapChainDesc.OutputWindow = Window;
		SwapChainDesc.Windowed = TRUE;
		SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		SwapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		THROW_ON_FAIL(IDXGIFactory6_CreateSwapChain(Factory, DxObjects.CommandQueue, &SwapChainDesc, &DxObjects.SwapChain));
	}
	
	THROW_ON_FAIL(IDXGIFactory6_MakeWindowAssociation(Factory, Window, DXGI_MWA_NO_ALT_ENTER));
	THROW_ON_FAIL(IDXGIFactory6_Release(Factory));


	struct SyncObjects SyncObjects = { 0 };

	SyncObjects.FrameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(DxObjects.SwapChain);

	{
		D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = { 0 };
		RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		RtvHeapDesc.NumDescriptors = BUFFER_COUNT;
		RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		THROW_ON_FAIL(ID3D12Device2_CreateDescriptorHeap(Device, &RtvHeapDesc, &IID_ID3D12DescriptorHeap, &DxObjects.RtvHeap));
	}

	DxObjects.RtvDescriptorSize = ID3D12Device2_GetDescriptorHandleIncrementSize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	
	{
		D3D12_DESCRIPTOR_HEAP_DESC DsvHeapDesc = { 0 };
		DsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		DsvHeapDesc.NumDescriptors = 1;
		DsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		THROW_ON_FAIL(ID3D12Device2_CreateDescriptorHeap(Device, &DsvHeapDesc, &IID_ID3D12DescriptorHeap, &DxObjects.DsvHeap));
	}

	DxObjects.DsvDescriptorSize = ID3D12Device2_GetDescriptorHandleIncrementSize(Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	for (UINT i = 0; i < BUFFER_COUNT; i++)
	{
		THROW_ON_FAIL(ID3D12Device2_CreateCommandAllocator(Device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, &DxObjects.CommandAllocators[i]));
	}

	const UINT64 CONSTANT_BUFFER_SIZE = sizeof(struct SceneConstantBuffer) * BUFFER_COUNT;

	{
		D3D12_HEAP_PROPERTIES ConstantBufferHeapProps = { 0 };
		ConstantBufferHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
		ConstantBufferHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		ConstantBufferHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		ConstantBufferHeapProps.CreationNodeMask = 1;
		ConstantBufferHeapProps.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC ConstantBufferDesc = { 0 };
		ConstantBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		ConstantBufferDesc.Alignment = 0;
		ConstantBufferDesc.Width = CONSTANT_BUFFER_SIZE;
		ConstantBufferDesc.Height = 1;
		ConstantBufferDesc.DepthOrArraySize = 1;
		ConstantBufferDesc.MipLevels = 1;
		ConstantBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
		ConstantBufferDesc.SampleDesc.Count = 1;
		ConstantBufferDesc.SampleDesc.Quality = 0;
		ConstantBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ConstantBufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(
			Device,
			&ConstantBufferHeapProps,
			D3D12_HEAP_FLAG_NONE,
			&ConstantBufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			NULL,
			&IID_ID3D12Resource,
			&DxObjects.ConstantBuffer));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(DxObjects.ConstantBuffer, L"constant buffer"));
#endif
	}

	THROW_ON_FAIL(ID3D12Resource_Map(DxObjects.ConstantBuffer, 0, NULL, &DxObjects.CbvDataBegin));

	{
		HANDLE MeshShaderFile = CreateFileW(MESH_SHADER_FILE, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		VALIDATE_HANDLE(MeshShaderFile);

		SIZE_T MeshShaderSize;
		THROW_ON_FALSE(GetFileSizeEx(MeshShaderFile, &MeshShaderSize));

		HANDLE MeshShaderFileMap = CreateFileMappingW(MeshShaderFile, NULL, PAGE_READONLY, 0, 0, NULL);
		VALIDATE_HANDLE(MeshShaderFileMap);

		const void* MeshShaderBytecode = MapViewOfFile(MeshShaderFileMap, FILE_MAP_READ, 0, 0, 0);


		HANDLE PixelShaderFile = CreateFileW(PIXEL_SHADER_FILE, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		VALIDATE_HANDLE(PixelShaderFile);

		SIZE_T PixelShaderSize;
		THROW_ON_FALSE(GetFileSizeEx(PixelShaderFile, &PixelShaderSize));

		HANDLE PixelShaderFileMap = CreateFileMappingW(PixelShaderFile, NULL, PAGE_READONLY, 0, 0, NULL);
		VALIDATE_HANDLE(PixelShaderFileMap);

		const void* PixelShaderBytecode = MapViewOfFile(PixelShaderFileMap, FILE_MAP_READ, 0, 0, 0);

		{
			D3D12_ROOT_PARAMETER rootParameters[6] = { 0 };
			rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;// b0
			rootParameters[0].Descriptor.RegisterSpace = 0;
			rootParameters[0].Descriptor.ShaderRegister = 0;
			rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;// b1
			rootParameters[1].Constants.Num32BitValues = 2;
			rootParameters[1].Constants.RegisterSpace = 0;
			rootParameters[1].Constants.ShaderRegister = 1;
			rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;

			rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;// t0
			rootParameters[2].Descriptor.RegisterSpace = 0;
			rootParameters[2].Descriptor.ShaderRegister = 0;
			rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;

			rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;// t1
			rootParameters[3].Descriptor.RegisterSpace = 0;
			rootParameters[3].Descriptor.ShaderRegister = 1;
			rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;

			rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;// t2
			rootParameters[4].Descriptor.RegisterSpace = 0;
			rootParameters[4].Descriptor.ShaderRegister = 2;
			rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;

			rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;// t3
			rootParameters[5].Descriptor.RegisterSpace = 0;
			rootParameters[5].Descriptor.ShaderRegister = 3;
			rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;

			D3D12_ROOT_SIGNATURE_DESC rootSigDesc = { 0 };
			rootSigDesc.NumParameters = ARRAYSIZE(rootParameters);
			rootSigDesc.pParameters = rootParameters;
			rootSigDesc.NumStaticSamplers = 0;
			rootSigDesc.pStaticSamplers = NULL;
			rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

			ID3D10Blob* Signature;
			THROW_ON_FAIL(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &Signature, NULL));

			THROW_ON_FAIL(ID3D12Device2_CreateRootSignature(Device, 0, ID3D10Blob_GetBufferPointer(Signature), ID3D10Blob_GetBufferSize(Signature), &IID_ID3D12RootSignature, &DxObjects.RootSignature));
			ID3D10Blob_Release(Signature);
		}

		struct
		{
			alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE ObjectTypepRootSignature;
			ID3D12RootSignature* pRootSignature;

			alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE ObjectTypePS;
			D3D12_SHADER_BYTECODE PS;

			alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE ObjectTypeMS;
			D3D12_SHADER_BYTECODE MS;

			alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE ObjectTypeDepthStencilState;
			D3D12_DEPTH_STENCIL_DESC DepthStencilState;

			alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE ObjectTypeDSVFormat;
			DXGI_FORMAT DSVFormat;

			alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE ObjectTypeRasterizerState;
			D3D12_RASTERIZER_DESC RasterizerState;

			alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE ObjectTypeRTVFormats;
			struct D3D12_RT_FORMAT_ARRAY RTVFormats;
		} PipelineStateObject = { 0 };

		PipelineStateObject.ObjectTypepRootSignature = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
		PipelineStateObject.pRootSignature = DxObjects.RootSignature;

		PipelineStateObject.ObjectTypePS = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
		PipelineStateObject.PS.pShaderBytecode = PixelShaderBytecode;
		PipelineStateObject.PS.BytecodeLength = PixelShaderSize;

		PipelineStateObject.ObjectTypeMS = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
		PipelineStateObject.MS.pShaderBytecode = MeshShaderBytecode;
		PipelineStateObject.MS.BytecodeLength = MeshShaderSize;

		PipelineStateObject.ObjectTypeDepthStencilState = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
		PipelineStateObject.DepthStencilState.DepthEnable = TRUE;
		PipelineStateObject.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		PipelineStateObject.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		PipelineStateObject.DepthStencilState.StencilEnable = FALSE;
		PipelineStateObject.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		PipelineStateObject.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
		PipelineStateObject.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		PipelineStateObject.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		PipelineStateObject.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		PipelineStateObject.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		PipelineStateObject.DepthStencilState.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		PipelineStateObject.DepthStencilState.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		PipelineStateObject.DepthStencilState.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		PipelineStateObject.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		PipelineStateObject.ObjectTypeDSVFormat = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT;
		PipelineStateObject.DSVFormat = DEPTH_BUFFER_FORMAT;
		
		PipelineStateObject.ObjectTypeRasterizerState = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
		PipelineStateObject.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		PipelineStateObject.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		PipelineStateObject.RasterizerState.FrontCounterClockwise = FALSE;
		PipelineStateObject.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		PipelineStateObject.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		PipelineStateObject.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		PipelineStateObject.RasterizerState.DepthClipEnable = TRUE;
		PipelineStateObject.RasterizerState.MultisampleEnable = FALSE;
		PipelineStateObject.RasterizerState.AntialiasedLineEnable = FALSE;
		PipelineStateObject.RasterizerState.ForcedSampleCount = 0;
		PipelineStateObject.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		PipelineStateObject.ObjectTypeRTVFormats = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
		PipelineStateObject.RTVFormats.RTFormats[0] = RTV_FORMAT;
		PipelineStateObject.RTVFormats.NumRenderTargets = 1;

		D3D12_PIPELINE_STATE_STREAM_DESC PsoStreamDesc = { 0 };
		PsoStreamDesc.SizeInBytes = sizeof(PipelineStateObject);
		PsoStreamDesc.pPipelineStateSubobjectStream = &PipelineStateObject;

		THROW_ON_FAIL(ID3D12Device2_CreatePipelineState(Device, &PsoStreamDesc, &IID_ID3D12PipelineState, &DxObjects.PipelineState));

		THROW_ON_FALSE(UnmapViewOfFile(MeshShaderBytecode));
		THROW_ON_FALSE(CloseHandle(MeshShaderFileMap));
		THROW_ON_FALSE(CloseHandle(MeshShaderFile));

		THROW_ON_FALSE(UnmapViewOfFile(PixelShaderBytecode));
		THROW_ON_FALSE(CloseHandle(PixelShaderFileMap));
		THROW_ON_FALSE(CloseHandle(PixelShaderFile));
	}

	THROW_ON_FAIL(ID3D12Device2_CreateCommandList(Device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, DxObjects.CommandAllocators[SyncObjects.FrameIndex], DxObjects.PipelineState, &IID_ID3D12GraphicsCommandList7, &DxObjects.CommandList));

	THROW_ON_FAIL(ID3D12GraphicsCommandList7_Close(DxObjects.CommandList));

	struct ObjectInfo ObjectInfo = { 0 };

	HANDLE AssetDataFile = CreateFileW(MESHFILE_NAME, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	VALIDATE_HANDLE(AssetDataFile);

	SIZE_T AssetDataSize;
	THROW_ON_FALSE(GetFileSizeEx(AssetDataFile, &AssetDataSize));

	HANDLE AssetDataFileMap = CreateFileMappingW(AssetDataFile, NULL, PAGE_READONLY, 0, 0, NULL);
	VALIDATE_HANDLE(AssetDataFileMap);

	const void* AssetDataBytecode = MapViewOfFile(AssetDataFileMap, FILE_MAP_READ, 0, 0, 0);

	{
		struct FileHeader
		{
			uint32_t Prolog;
			uint32_t Version;
			uint32_t MeshCount;
			uint32_t AccessorCount;
			uint32_t BufferViewCount;
			uint32_t BufferSize;
		};

		struct MeshHeader
		{
			uint32_t IndexBuffer;
			uint32_t IndexSubsets;
			uint32_t Attributes[ATTRIBUTE_TYPE_COUNT];
			uint32_t Meshlets;
			uint32_t MeshletSubsets;
			uint32_t UniqueVertexIndices;
			uint32_t PrimitiveIndices;
			uint32_t CullData;
		};

		struct BufferView
		{
			uint32_t Offset;
			uint32_t Size;
		};

		struct Accessor
		{
			uint32_t BufferView;
			uint32_t Offset;
			uint32_t Size;
			uint32_t Stride;
			uint32_t Count;
		};

		const void* readPointer = AssetDataBytecode;
		const struct FileHeader* header = readPointer;
		readPointer = OffsetPointer(readPointer, sizeof(struct FileHeader));

		if (header->Prolog != MESHFILE_PROLOG)
		{
			WriteConsoleW(ConsoleHandle, L"File Malformed", 14, NULL, NULL);
			return EXIT_FAILURE; // Incorrect file format.
		}

		if (header->Version != CURRENT_FILE_VERSION)
		{
			WriteConsoleW(ConsoleHandle, L"File Malformed", 14, NULL, NULL);
			return EXIT_FAILURE; // Version mismatch between export and import serialization code.
		}

		// Read mesh metdata
		const struct MeshHeader* meshes = readPointer;
		readPointer = OffsetPointer(readPointer, header->MeshCount * sizeof(meshes[0]));

		const struct Accessor* accessors = readPointer;
		readPointer = OffsetPointer(readPointer, header->AccessorCount * sizeof(accessors[0]));

		const struct BufferView* bufferViews = readPointer;
		readPointer = OffsetPointer(readPointer, header->BufferViewCount * sizeof(bufferViews[0]));

		const void* buffer = readPointer;

		ObjectInfo.MeshList = VirtualAlloc(
			NULL,
			sizeof(struct Mesh) * header->MeshCount + (sizeof(struct VertexBuffer) * 6) * header->MeshCount,
			MEM_COMMIT | MEM_RESERVE,
			PAGE_READWRITE
		);

		ObjectInfo.MeshList[0].VertexBuffers = OffsetPointer(ObjectInfo.MeshList, sizeof(struct Mesh) * header->MeshCount);

		for (int i = 1; i < header->MeshCount; i++)
		{
			ObjectInfo.MeshList[i].VertexBuffers = OffsetPointer(ObjectInfo.MeshList[i - 1].VertexBuffers, sizeof(struct VertexBuffer) * 6);
		}

		ObjectInfo.MeshCount = header->MeshCount;

		// Populate mesh data from binary data and metadata.

		const D3D12_INPUT_ELEMENT_DESC InputElementDescs[ATTRIBUTE_TYPE_COUNT] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 1 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 1 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 1 },
			{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 1 },
			{ "BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 1 }
		};

		for (int i = 0; i < header->MeshCount; i++)
		{
			ObjectInfo.MeshList[i].VertexBufferCount = 0;
			// Index data

			ObjectInfo.MeshList[i].IndexSize = accessors[meshes[i].IndexBuffer].Size;
			ObjectInfo.MeshList[i].IndexCount = accessors[meshes[i].IndexBuffer].Count;
			ObjectInfo.MeshList[i].IndexBuffer = OffsetPointer(buffer, bufferViews[accessors[meshes[i].IndexBuffer].BufferView].Offset);
			ObjectInfo.MeshList[i].IndexBufferSize = bufferViews[accessors[meshes[i].IndexBuffer].BufferView].Size;

			// Index Subset data
			ObjectInfo.MeshList[i].IndexSubsets = OffsetPointer(buffer, bufferViews[accessors[meshes[i].IndexSubsets].BufferView].Offset);
			ObjectInfo.MeshList[i].IndexSubsetCount = accessors[meshes[i].IndexSubsets].Count;

			// Vertex data & layout metadata

			// Determine the number of unique Buffer Views associated with the vertex attributes & copy vertex buffers.
			uint32_t vbMap[ATTRIBUTE_TYPE_COUNT];

			uint32_t vbMapSize = 0;

			ObjectInfo.MeshList[i].LayoutDesc.pInputElementDescs = ObjectInfo.MeshList[i].LayoutElems;
			ObjectInfo.MeshList[i].LayoutDesc.NumElements = 0;

			for (int j = 0; j < ATTRIBUTE_TYPE_COUNT; j++)
			{
				if (meshes[i].Attributes[j] == -1)
					continue;

				bool shouldContinue = false;
				for (int k = 0; k < vbMapSize; k++)
				{
					if (vbMap[k] == accessors[meshes[i].Attributes[j]].BufferView)
					{
						shouldContinue = true;
						break;
					}
				}

				if (shouldContinue) continue;

				// New buffer view encountered; add to list and copy vertex data
				vbMap[vbMapSize] = accessors[meshes[i].Attributes[j]].BufferView;
				vbMapSize++;

				struct VertexBuffer verts = { 0 };
				verts.Verts = OffsetPointer(buffer, bufferViews[accessors[meshes[i].Attributes[j]].BufferView].Offset);
				verts.Count = bufferViews[accessors[meshes[i].Attributes[j]].BufferView].Size;
				verts.Stride = accessors[meshes[i].Attributes[j]].Stride;

				ObjectInfo.MeshList[i].VertexBuffers[ObjectInfo.MeshList[i].VertexBufferCount] = verts;
				ObjectInfo.MeshList[i].VertexBufferCount++;
				ObjectInfo.MeshList[i].VertexCount = verts.Count / verts.Stride;
			}

			// Populate the vertex buffer metadata from accessors.
			for (int j = 0; j < ATTRIBUTE_TYPE_COUNT; j++)
			{
				if (meshes[i].Attributes[j] == -1)
					continue;

				// Determine which vertex buffer index holds this attribute's data

				D3D12_INPUT_ELEMENT_DESC desc = InputElementDescs[j];

				for (int k = 0; k < vbMapSize; k++)
				{
					if (vbMap[k] == accessors[meshes[i].Attributes[j]].BufferView)
					{
						desc.InputSlot = k;
						break;
					}
				}

				ObjectInfo.MeshList[i].LayoutElems[ObjectInfo.MeshList[i].LayoutDesc.NumElements] = desc;
				ObjectInfo.MeshList[i].LayoutDesc.NumElements++;
			}

			// Meshlet data
			ObjectInfo.MeshList[i].Meshlets = OffsetPointer(buffer, bufferViews[accessors[meshes[i].Meshlets].BufferView].Offset);
			ObjectInfo.MeshList[i].MeshletCount = accessors[meshes[i].Meshlets].Count;

			// Meshlet Subset data
			ObjectInfo.MeshList[i].MeshletSubsets = OffsetPointer(buffer, bufferViews[accessors[meshes[i].MeshletSubsets].BufferView].Offset);
			ObjectInfo.MeshList[i].MeshletSubsetCount = accessors[meshes[i].MeshletSubsets].Count;

			// Unique Vertex Index data
			ObjectInfo.MeshList[i].UniqueVertexIndices = OffsetPointer(buffer, bufferViews[accessors[meshes[i].UniqueVertexIndices].BufferView].Offset);
			ObjectInfo.MeshList[i].UniqueVertexIndexCount = bufferViews[accessors[meshes[i].UniqueVertexIndices].BufferView].Size;

			// Primitive Index data
			ObjectInfo.MeshList[i].PrimitiveIndices = OffsetPointer(buffer, bufferViews[accessors[meshes[i].PrimitiveIndices].BufferView].Offset);
			ObjectInfo.MeshList[i].PrimitiveIndexCount = accessors[meshes[i].PrimitiveIndices].Count;

			// Cull data
			ObjectInfo.MeshList[i].CullingData = OffsetPointer(buffer, bufferViews[accessors[meshes[i].CullData].BufferView].Offset);
			ObjectInfo.MeshList[i].CullingDataCount = accessors[meshes[i].CullData].Count;
		}

		struct BoundingSphere BoundingSphere = { 0 };

		// Build bounding spheres for each mesh
		for (int i = 0; i < header->MeshCount; i++)
		{
			uint32_t vbIndexPos = 0;

			// Find the index of the vertex buffer of the position attribute
			for (int j = 1; j < ObjectInfo.MeshList[i].LayoutDesc.NumElements; j++)
			{
				if (strcmp(ObjectInfo.MeshList[i].LayoutElems[j].SemanticName, "POSITION") == 0)
				{
					vbIndexPos = j;
					break;
				}
			}

			// Find the byte offset of the position attribute with its vertex buffer
			uint32_t positionOffset = 0;

			for (int j = 0; j < ObjectInfo.MeshList[i].LayoutDesc.NumElements; j++)
			{
				if (strcmp(ObjectInfo.MeshList[i].LayoutElems[j].SemanticName, "POSITION") == 0)
				{
					break;
				}

				if (ObjectInfo.MeshList[i].LayoutElems[j].InputSlot == vbIndexPos)
				{
					switch (ObjectInfo.MeshList[i].LayoutElems[j].Format)
					{
					case DXGI_FORMAT_R32G32B32A32_FLOAT:
						positionOffset += 16;
						break;
					case DXGI_FORMAT_R32G32B32_FLOAT:
						positionOffset += 12;
						break;
					case DXGI_FORMAT_R32G32_FLOAT:
						positionOffset += 8;
						break;
					case DXGI_FORMAT_R32_FLOAT:
						positionOffset += 4;
						break;
					default: DebugBreak();
					}
				}
			}

			const float* v0 = OffsetPointer(ObjectInfo.MeshList[i].VertexBuffers[vbIndexPos].Verts, positionOffset);

			//create from points
			{
				vec3 MinX, MaxX, MinY, MaxY, MinZ, MaxZ;

				glm_vec3_copy(v0, MinX);
				glm_vec3_copy(v0, MaxX);
				glm_vec3_copy(v0, MinY);
				glm_vec3_copy(v0, MaxY);
				glm_vec3_copy(v0, MinZ);
				glm_vec3_copy(v0, MaxZ);

				for (size_t i = 1; i < ObjectInfo.MeshList[i].VertexCount; i++)
				{
					vec3 Point;
					glm_vec3_copy(OffsetPointer(v0, i * ObjectInfo.MeshList[i].VertexBuffers[vbIndexPos].Stride), Point);

					if (Point[0] < MinX[0])
						glm_vec3_copy(Point, MinX);

					if (Point[0] > MaxX[0])
						glm_vec3_copy(Point, MaxX);

					if (Point[1] < MinY[1])
						glm_vec3_copy(Point, MinY);

					if (Point[1] > MaxY[1])
						glm_vec3_copy(Point, MaxY);

					if (Point[2] < MinZ[2])
						glm_vec3_copy(Point, MinZ);

					if (Point[2] > MaxZ[2])
						glm_vec3_copy(Point, MaxZ);
				}

				// Use the min/max pair that are farthest apart to form the initial sphere.

				vec3 DeltaX;
				glm_vec3_sub(MaxX, MinX, DeltaX);

				const float DistX = glm_vec3_distance(DeltaX, (vec3){ 0, 0, 0 });

				vec3 DeltaY;
				glm_vec3_sub(MaxY, MinY, DeltaY);

				const float DistY = glm_vec3_distance(DeltaY, (vec3) { 0, 0, 0 });

				vec3 DeltaZ;
				glm_vec3_sub(MaxZ, MinZ, DeltaZ);

				const float DistZ = glm_vec3_distance(DeltaZ, (vec3) { 0, 0, 0 });

				vec3 vCenter;
				float vRadius;

				if (DistX > DistY)
				{
					if (DistX > DistZ)
					{
						// Use min/max x.
						glm_vec3_lerp(MaxX, MinX, 0.5f, vCenter);

						vRadius = DistX * 0.5f;
					}
					else
					{
						// Use min/max z.
						glm_vec3_lerp(MaxZ, MinZ, 0.5f, vCenter);

						vRadius = DistZ * 0.5f;
					}
				}
				else // Y >= X
				{
					if (DistY > DistZ)
					{
						// Use min/max y.
						glm_vec3_lerp(MaxY, MinY, 0.5f, vCenter);

						vRadius = DistY * 0.5f;
					}
					else
					{
						// Use min/max z.
						glm_vec3_lerp(MaxZ, MinZ, 0.5f, vCenter);

						vRadius = DistZ * 0.5f;
					}
				}

				// Add any points not inside the sphere.
				for (size_t i = 0; i < ObjectInfo.MeshList[i].VertexCount; i++)
				{
					vec3 Point;
					glm_vec3_copy(OffsetPointer(v0, i * ObjectInfo.MeshList[i].VertexBuffers[vbIndexPos].Stride), Point);

					vec3 Delta;
					glm_vec3_sub(Point, vCenter, Delta);

					float Dist = glm_vec3_distance(Delta, (vec3) { 0, 0, 0 });

					if (Dist > vRadius)
					{
						// Adjust sphere to include the new point.
						vRadius = (vRadius + Dist) * 0.5f;
						vCenter[0] += (1.0f - vRadius / Dist) * Delta[0];
						vCenter[1] += (1.0f - vRadius / Dist) * Delta[1];
						vCenter[2] += (1.0f - vRadius / Dist) * Delta[2];
					}
				}

				glm_vec3_copy(vCenter, ObjectInfo.MeshList[i].BoundingSphere.Center);
				ObjectInfo.MeshList[i].BoundingSphere.Radius = vRadius;
			}

			if (i == 0)
			{
				BoundingSphere = ObjectInfo.MeshList[i].BoundingSphere;
			}
			else
			{
				vec3 Center1;
				glm_vec3_copy(BoundingSphere.Center, Center1);

				const float r1 = BoundingSphere.Radius;

				vec3 Center2;
				glm_vec3_copy(ObjectInfo.MeshList[i].BoundingSphere.Center, Center2);

				float r2 = ObjectInfo.MeshList[i].BoundingSphere.Radius;

				vec3 V;
				glm_vec3_sub(Center2, Center1, V);

				const float d = glm_vec3_distance(V, (vec3) { 0, 0, 0 });

				const bool flag = r1 + r2 >= d;

				if (flag && r1 - r2 >= d)
				{
					BoundingSphere = ObjectInfo.MeshList[i].BoundingSphere;
				}
				else if (flag && r2 - r1 >= d)
				{
					BoundingSphere = ObjectInfo.MeshList[i].BoundingSphere;
				}
				else
				{
					vec3 Dist;
					glm_vec3_broadcast(d, Dist);

					vec3 N;
					glm_vec3_div(V, Dist, N);

					const float t1 = fmin(-r1, d - r2);
					const float t2 = fmax(r1, d + r2);
					const float t_5 = (t2 - t1) * 0.5f;

					vec3 tempVec;
					glm_vec3_broadcast(t_5 + t1, tempVec);

					vec3 NxTempVec;
					glm_vec3_mul(N, tempVec, NxTempVec);

					vec3 NCenter;
					glm_vec3_add(Center1, NxTempVec, NCenter);

					glm_vec3_copy(NCenter, BoundingSphere.Center);
					BoundingSphere.Radius = t_5;
				}
			}
		}
	}

	D3D12_HEAP_PROPERTIES UploadHeap = { 0 };
	UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
	UploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	UploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	UploadHeap.CreationNodeMask = 1;
	UploadHeap.VisibleNodeMask = 1;

	D3D12_HEAP_PROPERTIES DefaultHeap = { 0 };
	DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	DefaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	DefaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	DefaultHeap.CreationNodeMask = 1;
	DefaultHeap.VisibleNodeMask = 1;

	ID3D12GraphicsCommandList7_Reset(DxObjects.CommandList, DxObjects.CommandAllocators[SyncObjects.FrameIndex], NULL);

	ID3D12Resource** UploadBuffers = VirtualAlloc(
		NULL,
		ObjectInfo.MeshCount * 7 * sizeof(ID3D12Resource*),
		MEM_COMMIT | MEM_RESERVE,
		PAGE_READWRITE
	);

	int UploadBufferCount = 0;

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		D3D12_RESOURCE_DESC indexDesc = { 0 };
		indexDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		indexDesc.Alignment = 0;
		indexDesc.Width = ObjectInfo.MeshList[i].IndexBufferSize;
		indexDesc.Height = 1;
		indexDesc.DepthOrArraySize = 1;
		indexDesc.MipLevels = 1;
		indexDesc.Format = DXGI_FORMAT_UNKNOWN;
		indexDesc.SampleDesc.Count = 1;
		indexDesc.SampleDesc.Quality = 0;
		indexDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		indexDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &UploadHeap, D3D12_HEAP_FLAG_NONE, &indexDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, &UploadBuffers[UploadBufferCount]));
		
#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(UploadBuffers[UploadBufferCount], L"Index Buffer Upload"));
#endif
		
		void* memory;
		ID3D12Resource_Map(UploadBuffers[UploadBufferCount], 0, NULL, &memory);
		MEMCPY_VERIFY(memcpy_s(memory, ObjectInfo.MeshList[i].IndexBufferSize, ObjectInfo.MeshList[i].IndexBuffer, ObjectInfo.MeshList[i].IndexBufferSize));
		ID3D12Resource_Unmap(UploadBuffers[UploadBufferCount], 0, NULL);

		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &DefaultHeap, D3D12_HEAP_FLAG_NONE, &indexDesc, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, &ObjectInfo.MeshList[i].IndexResource));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(ObjectInfo.MeshList[i].IndexResource, L"Index Buffer"));
#endif

		ID3D12GraphicsCommandList7_CopyResource(DxObjects.CommandList, ObjectInfo.MeshList[i].IndexResource, UploadBuffers[UploadBufferCount]);

		UploadBufferCount++;
	}

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		D3D12_RESOURCE_DESC meshletDesc = { 0 };
		meshletDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		meshletDesc.Alignment = 0;
		meshletDesc.Width = ObjectInfo.MeshList[i].MeshletCount * sizeof(ObjectInfo.MeshList[i].Meshlets[0]);
		meshletDesc.Height = 1;
		meshletDesc.DepthOrArraySize = 1;
		meshletDesc.MipLevels = 1;
		meshletDesc.Format = DXGI_FORMAT_UNKNOWN;
		meshletDesc.SampleDesc.Count = 1;
		meshletDesc.SampleDesc.Quality = 0;
		meshletDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		meshletDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &UploadHeap, D3D12_HEAP_FLAG_NONE, &meshletDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, &UploadBuffers[UploadBufferCount]));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(UploadBuffers[UploadBufferCount], L"Meshlet Resource Upload"));
#endif

		void* memory;
		ID3D12Resource_Map(UploadBuffers[UploadBufferCount], 0, NULL, &memory);
		MEMCPY_VERIFY(memcpy_s(memory, ObjectInfo.MeshList[i].MeshletCount * sizeof(ObjectInfo.MeshList[i].Meshlets[0]), ObjectInfo.MeshList[i].Meshlets, ObjectInfo.MeshList[i].MeshletCount * sizeof(ObjectInfo.MeshList[i].Meshlets[0])));
		ID3D12Resource_Unmap(UploadBuffers[UploadBufferCount], 0, NULL);
		
		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &DefaultHeap, D3D12_HEAP_FLAG_NONE, &meshletDesc, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, &ObjectInfo.MeshList[i].MeshletResource));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(ObjectInfo.MeshList[i].MeshletResource, L"Meshlet Resource"));
#endif

		ID3D12GraphicsCommandList7_CopyResource(DxObjects.CommandList, ObjectInfo.MeshList[i].MeshletResource, UploadBuffers[UploadBufferCount]);

		UploadBufferCount++;
	}

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		D3D12_RESOURCE_DESC cullDataDesc = { 0 };
		cullDataDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		cullDataDesc.Alignment = 0;
		cullDataDesc.Width = ObjectInfo.MeshList[i].CullingDataCount * sizeof(ObjectInfo.MeshList[i].CullingData[0]);
		cullDataDesc.Height = 1;
		cullDataDesc.DepthOrArraySize = 1;
		cullDataDesc.MipLevels = 1;
		cullDataDesc.Format = DXGI_FORMAT_UNKNOWN;
		cullDataDesc.SampleDesc.Count = 1;
		cullDataDesc.SampleDesc.Quality = 0;
		cullDataDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		cullDataDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &UploadHeap, D3D12_HEAP_FLAG_NONE, &cullDataDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, &UploadBuffers[UploadBufferCount]));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(UploadBuffers[UploadBufferCount], L"culling data Upload"));
#endif

		void* memory;
		ID3D12Resource_Map(UploadBuffers[UploadBufferCount], 0, NULL, &memory);
		MEMCPY_VERIFY(memcpy_s(memory, ObjectInfo.MeshList[i].CullingDataCount * sizeof(ObjectInfo.MeshList[i].CullingData[0]), ObjectInfo.MeshList[i].CullingData, ObjectInfo.MeshList[i].CullingDataCount * sizeof(ObjectInfo.MeshList[i].CullingData[0])));
		ID3D12Resource_Unmap(UploadBuffers[UploadBufferCount], 0, NULL);
		
		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &DefaultHeap, D3D12_HEAP_FLAG_NONE, &cullDataDesc, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, &ObjectInfo.MeshList[i].CullDataResource));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(ObjectInfo.MeshList[i].CullDataResource, L"culling data"));
#endif

		ID3D12GraphicsCommandList7_CopyResource(DxObjects.CommandList, ObjectInfo.MeshList[i].CullDataResource, UploadBuffers[UploadBufferCount]);

		UploadBufferCount++;
	}

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		D3D12_RESOURCE_DESC vertexIndexDesc = { 0 };
		vertexIndexDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		vertexIndexDesc.Alignment = 0;
		vertexIndexDesc.Width = ((ObjectInfo.MeshList[i].UniqueVertexIndexCount + 4 - 1) / 4) * 4;
		vertexIndexDesc.Height = 1;
		vertexIndexDesc.DepthOrArraySize = 1;
		vertexIndexDesc.MipLevels = 1;
		vertexIndexDesc.Format = DXGI_FORMAT_UNKNOWN;
		vertexIndexDesc.SampleDesc.Count = 1;
		vertexIndexDesc.SampleDesc.Quality = 0;
		vertexIndexDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		vertexIndexDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &UploadHeap, D3D12_HEAP_FLAG_NONE, &vertexIndexDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, &UploadBuffers[UploadBufferCount]));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(UploadBuffers[UploadBufferCount], L"unique vertex Upload"));
#endif

		void* memory;
		ID3D12Resource_Map(UploadBuffers[UploadBufferCount], 0, NULL, &memory);
		MEMCPY_VERIFY(memcpy_s(memory, ObjectInfo.MeshList[i].UniqueVertexIndexCount, ObjectInfo.MeshList[i].UniqueVertexIndices, ObjectInfo.MeshList[i].UniqueVertexIndexCount));
		ID3D12Resource_Unmap(UploadBuffers[UploadBufferCount], 0, NULL);
		
		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &DefaultHeap, D3D12_HEAP_FLAG_NONE, &vertexIndexDesc, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, &ObjectInfo.MeshList[i].UniqueVertexIndexResource));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(ObjectInfo.MeshList[i].UniqueVertexIndexResource, L"unique vertex"));
#endif

		ID3D12GraphicsCommandList7_CopyResource(DxObjects.CommandList, ObjectInfo.MeshList[i].UniqueVertexIndexResource, UploadBuffers[UploadBufferCount]);

		UploadBufferCount++;
	}

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		D3D12_RESOURCE_DESC primitiveDesc = { 0 };
		primitiveDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		primitiveDesc.Alignment = 0;
		primitiveDesc.Width = ObjectInfo.MeshList[i].PrimitiveIndexCount * sizeof(ObjectInfo.MeshList[i].PrimitiveIndices[0]);
		primitiveDesc.Height = 1;
		primitiveDesc.DepthOrArraySize = 1;
		primitiveDesc.MipLevels = 1;
		primitiveDesc.Format = DXGI_FORMAT_UNKNOWN;
		primitiveDesc.SampleDesc.Count = 1;
		primitiveDesc.SampleDesc.Quality = 0;
		primitiveDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		primitiveDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &UploadHeap, D3D12_HEAP_FLAG_NONE, &primitiveDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, &UploadBuffers[UploadBufferCount]));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(UploadBuffers[UploadBufferCount], L"primitive index Upload"));
#endif

		void* memory;
		ID3D12Resource_Map(UploadBuffers[UploadBufferCount], 0, NULL, &memory);
		MEMCPY_VERIFY(memcpy_s(memory, ObjectInfo.MeshList[i].PrimitiveIndexCount * sizeof(ObjectInfo.MeshList[i].PrimitiveIndices[0]), ObjectInfo.MeshList[i].PrimitiveIndices, ObjectInfo.MeshList[i].PrimitiveIndexCount * sizeof(ObjectInfo.MeshList[i].PrimitiveIndices[0])));
		ID3D12Resource_Unmap(UploadBuffers[UploadBufferCount], 0, NULL);
		
		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &DefaultHeap, D3D12_HEAP_FLAG_NONE, &primitiveDesc, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, &ObjectInfo.MeshList[i].PrimitiveIndexResource));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(ObjectInfo.MeshList[i].UniqueVertexIndexResource, L"unique vertex"));
#endif

		ID3D12GraphicsCommandList7_CopyResource(DxObjects.CommandList, ObjectInfo.MeshList[i].PrimitiveIndexResource, UploadBuffers[UploadBufferCount]);

		UploadBufferCount++;
	}

	ID3D12Resource* ObjectUploadBuffer = NULL;

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		struct ObjectInfo
		{
			uint32_t IndexSize;
			uint32_t MeshletCount;
			uint32_t LastMeshletVertCount;
			uint32_t LastMeshletPrimCount;
		};

		D3D12_RESOURCE_DESC meshInfoDesc = { 0 };
		meshInfoDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		meshInfoDesc.Alignment = 0;
		meshInfoDesc.Width = sizeof(struct ObjectInfo);
		meshInfoDesc.Height = 1;
		meshInfoDesc.DepthOrArraySize = 1;
		meshInfoDesc.MipLevels = 1;
		meshInfoDesc.Format = DXGI_FORMAT_UNKNOWN;
		meshInfoDesc.SampleDesc.Count = 1;
		meshInfoDesc.SampleDesc.Quality = 0;
		meshInfoDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		meshInfoDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &UploadHeap, D3D12_HEAP_FLAG_NONE, &meshInfoDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, &ObjectUploadBuffer));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(ObjectUploadBuffer, L"mesh info Upload"));
#endif

		struct ObjectInfo info = { 0 };
		info.IndexSize = ObjectInfo.MeshList[i].IndexSize;
		info.MeshletCount = ObjectInfo.MeshList[i].MeshletCount;
		info.LastMeshletVertCount = ObjectInfo.MeshList[i].Meshlets[ObjectInfo.MeshList[i].MeshletCount].VertCount;
		info.LastMeshletPrimCount = ObjectInfo.MeshList[i].Meshlets[ObjectInfo.MeshList[i].MeshletCount].PrimCount;

		void* memory;
		ID3D12Resource_Map(ObjectUploadBuffer, 0, NULL, &memory);
		MEMCPY_VERIFY(memcpy_s(memory, sizeof(struct ObjectInfo), &info, sizeof(struct ObjectInfo)));
		ID3D12Resource_Unmap(ObjectUploadBuffer, 0, NULL);
		
		THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &DefaultHeap, D3D12_HEAP_FLAG_NONE, &meshInfoDesc, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, &ObjectInfo.MeshList[i].MeshInfoResource));

#ifdef _DEBUG
		THROW_ON_FAIL(ID3D12Resource_SetName(ObjectInfo.MeshList[i].MeshInfoResource, L"mesh info"));
#endif

		ID3D12GraphicsCommandList7_CopyResource(DxObjects.CommandList, ObjectInfo.MeshList[i].MeshInfoResource, ObjectUploadBuffer);

		ObjectInfo.MeshList[i].IBView.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ObjectInfo.MeshList[i].IndexResource);
		ObjectInfo.MeshList[i].IBView.Format = ObjectInfo.MeshList[i].IndexSize == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
		ObjectInfo.MeshList[i].IBView.SizeInBytes = ObjectInfo.MeshList[i].IndexCount * ObjectInfo.MeshList[i].IndexSize;

		UploadBufferCount++;
	}

	{
		uint32_t AllocationSize = 0;

		for (int i = 0; i < ObjectInfo.MeshCount; i++)
		{
			AllocationSize += 
				sizeof(ID3D12Resource*) * ObjectInfo.MeshList[i].VertexBufferCount +//size of m_meshes[i].VertexResources
				sizeof(D3D12_VERTEX_BUFFER_VIEW) * ObjectInfo.MeshList[i].VertexBufferCount;//size of m_meshes[i].VBViews
		}

		//remains open for the duration of the program
		void* AllocatedPages = VirtualAlloc(
			NULL,
			AllocationSize,
			MEM_COMMIT | MEM_RESERVE,
			PAGE_READWRITE
		);

		void* AllocatorPointer = AllocatedPages;

		for (int i = 0; i < ObjectInfo.MeshCount; i++)
		{
			ObjectInfo.MeshList[i].VertexResources = AllocatorPointer;
			AllocatorPointer = OffsetPointer(AllocatorPointer, sizeof(ID3D12Resource*) * ObjectInfo.MeshList[i].VertexBufferCount);

			ObjectInfo.MeshList[i].VBViews = AllocatorPointer;
			AllocatorPointer = OffsetPointer(AllocatorPointer, sizeof(D3D12_VERTEX_BUFFER_VIEW) * ObjectInfo.MeshList[i].VertexBufferCount);
		}
	}

	uint32_t MaxVertexUploadSize = 0;

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		MaxVertexUploadSize = max(sizeof(ID3D12Resource**) * ObjectInfo.MeshList[i].VertexBufferCount, MaxVertexUploadSize);
	}

	ID3D12Resource** vertexUploads = VirtualAlloc(
		NULL,
		MaxVertexUploadSize,
		MEM_COMMIT | MEM_RESERVE,
		PAGE_READWRITE
	);

	int vertexUploadNum = 0;

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		for (int j = 0; j < ObjectInfo.MeshList[i].VertexBufferCount; j++)
		{
			D3D12_RESOURCE_DESC vertexDesc = { 0 };
			vertexDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			vertexDesc.Alignment = 0;
			vertexDesc.Width = ObjectInfo.MeshList[i].VertexBuffers[j].Count;
			vertexDesc.Height = 1;
			vertexDesc.DepthOrArraySize = 1;
			vertexDesc.MipLevels = 1;
			vertexDesc.Format = DXGI_FORMAT_UNKNOWN;
			vertexDesc.SampleDesc.Count = 1;
			vertexDesc.SampleDesc.Quality = 0;
			vertexDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			vertexDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &DefaultHeap, D3D12_HEAP_FLAG_NONE, &vertexDesc, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, &ObjectInfo.MeshList[i].VertexResources[j]));

#ifdef _DEBUG
			THROW_ON_FAIL(ID3D12Resource_SetName(ObjectInfo.MeshList[i].VertexResources[j], L"Vertex Resource"));
#endif
			ObjectInfo.MeshList[i].VBViews[j].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ObjectInfo.MeshList[i].VertexResources[j]);
			ObjectInfo.MeshList[i].VBViews[j].SizeInBytes = ObjectInfo.MeshList[i].VertexBuffers[j].Count;
			ObjectInfo.MeshList[i].VBViews[j].StrideInBytes = ObjectInfo.MeshList[i].VertexBuffers[j].Stride;

			THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(Device, &UploadHeap, D3D12_HEAP_FLAG_NONE, &vertexDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, &vertexUploads[vertexUploadNum]));

#ifdef _DEBUG
			THROW_ON_FAIL(ID3D12Resource_SetName(vertexUploads[vertexUploadNum], L"Vertex Upload Heap"));
#endif

			void* memory;
			ID3D12Resource_Map(vertexUploads[vertexUploadNum], 0, NULL, &memory);
			MEMCPY_VERIFY(memcpy_s(memory, ObjectInfo.MeshList[i].VertexBuffers[j].Count, ObjectInfo.MeshList[i].VertexBuffers[j].Verts, ObjectInfo.MeshList[i].VertexBuffers[j].Count));
			ID3D12Resource_Unmap(vertexUploads[vertexUploadNum], 0, NULL);

			ID3D12GraphicsCommandList7_CopyResource(DxObjects.CommandList, ObjectInfo.MeshList[i].VertexResources[j], vertexUploads[vertexUploadNum]);
			
			vertexUploadNum++;
		}
	}

	int ResourceBarrierCount = 0;
	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		ResourceBarrierCount += 6 + ObjectInfo.MeshList[i].VertexBufferCount;
	}

	D3D12_BUFFER_BARRIER* ResourceBarriers = VirtualAlloc(
		NULL,
		ResourceBarrierCount * sizeof(D3D12_BUFFER_BARRIER),
		MEM_COMMIT | MEM_RESERVE,
		PAGE_READWRITE
	);

	ResourceBarrierCount = 0;

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		ResourceBarriers[ResourceBarrierCount].SyncBefore = D3D12_BARRIER_SYNC_COPY;
		ResourceBarriers[ResourceBarrierCount].SyncAfter = D3D12_BARRIER_SYNC_DRAW;
		ResourceBarriers[ResourceBarrierCount].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
		ResourceBarriers[ResourceBarrierCount].AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
		ResourceBarriers[ResourceBarrierCount].pResource = ObjectInfo.MeshList[i].IndexResource;
		ResourceBarriers[ResourceBarrierCount].Offset = 0;
		ResourceBarriers[ResourceBarrierCount].Size = UINT64_MAX;
		ResourceBarrierCount++;

		ResourceBarriers[ResourceBarrierCount].SyncBefore = D3D12_BARRIER_SYNC_COPY;
		ResourceBarriers[ResourceBarrierCount].SyncAfter = D3D12_BARRIER_SYNC_DRAW;
		ResourceBarriers[ResourceBarrierCount].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
		ResourceBarriers[ResourceBarrierCount].AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
		ResourceBarriers[ResourceBarrierCount].pResource = ObjectInfo.MeshList[i].MeshletResource;
		ResourceBarriers[ResourceBarrierCount].Offset = 0;
		ResourceBarriers[ResourceBarrierCount].Size = UINT64_MAX;
		ResourceBarrierCount++;

		ResourceBarriers[ResourceBarrierCount].SyncBefore = D3D12_BARRIER_SYNC_COPY;
		ResourceBarriers[ResourceBarrierCount].SyncAfter = D3D12_BARRIER_SYNC_DRAW;
		ResourceBarriers[ResourceBarrierCount].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
		ResourceBarriers[ResourceBarrierCount].AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
		ResourceBarriers[ResourceBarrierCount].pResource = ObjectInfo.MeshList[i].CullDataResource;
		ResourceBarriers[ResourceBarrierCount].Offset = 0;
		ResourceBarriers[ResourceBarrierCount].Size = UINT64_MAX;
		ResourceBarrierCount++;

		ResourceBarriers[ResourceBarrierCount].SyncBefore = D3D12_BARRIER_SYNC_COPY;
		ResourceBarriers[ResourceBarrierCount].SyncAfter = D3D12_BARRIER_SYNC_DRAW;
		ResourceBarriers[ResourceBarrierCount].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
		ResourceBarriers[ResourceBarrierCount].AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
		ResourceBarriers[ResourceBarrierCount].pResource = ObjectInfo.MeshList[i].UniqueVertexIndexResource;
		ResourceBarriers[ResourceBarrierCount].Offset = 0;
		ResourceBarriers[ResourceBarrierCount].Size = UINT64_MAX;
		ResourceBarrierCount++;

		ResourceBarriers[ResourceBarrierCount].SyncBefore = D3D12_BARRIER_SYNC_COPY;
		ResourceBarriers[ResourceBarrierCount].SyncAfter = D3D12_BARRIER_SYNC_DRAW;
		ResourceBarriers[ResourceBarrierCount].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
		ResourceBarriers[ResourceBarrierCount].AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
		ResourceBarriers[ResourceBarrierCount].pResource = ObjectInfo.MeshList[i].PrimitiveIndexResource;
		ResourceBarriers[ResourceBarrierCount].Offset = 0;
		ResourceBarriers[ResourceBarrierCount].Size = UINT64_MAX;
		ResourceBarrierCount++;

		ResourceBarriers[ResourceBarrierCount].SyncBefore = D3D12_BARRIER_SYNC_COPY;
		ResourceBarriers[ResourceBarrierCount].SyncAfter = D3D12_BARRIER_SYNC_DRAW;
		ResourceBarriers[ResourceBarrierCount].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
		ResourceBarriers[ResourceBarrierCount].AccessAfter = D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
		ResourceBarriers[ResourceBarrierCount].pResource = ObjectInfo.MeshList[i].MeshInfoResource;
		ResourceBarriers[ResourceBarrierCount].Offset = 0;
		ResourceBarriers[ResourceBarrierCount].Size = UINT64_MAX;
		ResourceBarrierCount++;

		for (int j = 0; j < ObjectInfo.MeshList[i].VertexBufferCount; j++)
		{
			ResourceBarriers[ResourceBarrierCount].SyncBefore = D3D12_BARRIER_SYNC_COPY;
			ResourceBarriers[ResourceBarrierCount].SyncAfter = D3D12_BARRIER_SYNC_DRAW;
			ResourceBarriers[ResourceBarrierCount].AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
			ResourceBarriers[ResourceBarrierCount].AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
			ResourceBarriers[ResourceBarrierCount].pResource = ObjectInfo.MeshList[i].VertexResources[j];
			ResourceBarriers[ResourceBarrierCount].Offset = 0;
			ResourceBarriers[ResourceBarrierCount].Size = UINT64_MAX;
			ResourceBarrierCount++;
		}
	}

	{
		D3D12_BARRIER_GROUP ResourceBarrier = { 0 };
		ResourceBarrier.Type = D3D12_BARRIER_TYPE_BUFFER;
		ResourceBarrier.NumBarriers = ResourceBarrierCount;
		ResourceBarrier.pBufferBarriers = ResourceBarriers;
		ID3D12GraphicsCommandList7_Barrier(DxObjects.CommandList, 1, &ResourceBarrier);
	}

	THROW_ON_FALSE(VirtualFree(ResourceBarriers, 0, MEM_RELEASE));

	THROW_ON_FAIL(ID3D12GraphicsCommandList7_Close(DxObjects.CommandList));

	ID3D12CommandQueue_ExecuteCommandLists(DxObjects.CommandQueue, 1, &DxObjects.CommandList);

	{
		ID3D12Fence* Fence;
		THROW_ON_FAIL(ID3D12Device2_CreateFence(Device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, &Fence));

		ID3D12CommandQueue_Signal(DxObjects.CommandQueue, Fence, 1);

		if (ID3D12Fence_GetCompletedValue(Fence) != 1)
		{
			HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
			ID3D12Fence_SetEventOnCompletion(Fence, 1, event);

			THROW_ON_FALSE(WaitForSingleObjectEx(event, INFINITE, FALSE) == WAIT_OBJECT_0);
			THROW_ON_FALSE(CloseHandle(event));
		}

		ID3D12Fence_Release(Fence);
	}

	for (int i = 0; i < UploadBufferCount - 1; i++)
	{
		THROW_ON_FAIL(ID3D12Resource_Release(UploadBuffers[i]));
	}

	for (int i = 0; i < vertexUploadNum; i++)
	{
		THROW_ON_FAIL(ID3D12Resource_Release(vertexUploads[i]));
	}

	THROW_ON_FALSE(VirtualFree(vertexUploads, 0, MEM_RELEASE));

#ifdef _DEBUG
	// Mesh shader file expects a certain vertex layout; assert our mesh conforms to that layout.
	const D3D12_INPUT_ELEMENT_DESC InputElementDescs[2] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 1 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 1 },
	};

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		assert(ObjectInfo.MeshList[i].LayoutDesc.NumElements == 2);

		for (uint32_t j = 0; j < ARRAYSIZE(InputElementDescs); j++)
			assert(memcmp(&ObjectInfo.MeshList[i].LayoutElems[j], &InputElementDescs[j], sizeof(D3D12_INPUT_ELEMENT_DESC)) == 0);
	}

#endif

	for (int i = 0; i < BUFFER_COUNT; i++)
	{
		SyncObjects.FenceValues[i] = 0;
		THROW_ON_FAIL(ID3D12Device2_CreateFence(Device, SyncObjects.FenceValues[i], D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, &SyncObjects.Fence[i]));
	}
	
	{
		SyncObjects.FenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
		VALIDATE_HANDLE(SyncObjects.FenceEvent);

		WaitForPreviousFrame(&SyncObjects, &DxObjects);
	}

	THROW_ON_FALSE(SetWindowLongPtrW(Window, GWLP_WNDPROC, WindowProc) != 0);

	DispatchMessageW(&(MSG) {
		.hwnd = Window,
		.message = WM_INIT,
		.wParam = &(struct WindowProcPayload) {
			.SyncObjects = &SyncObjects,
			.DxObjects = &DxObjects,
			.ObjectInfo = &ObjectInfo
		},
		.lParam = 0
	});
	
	DispatchMessageW(&(MSG) {
		.hwnd = Window,
		.message = WM_SIZE,
		.wParam = SIZE_RESTORED,
		.lParam = MAKELONG(WindowRect.right - WindowRect.left, WindowRect.bottom - WindowRect.top)
	});

	MSG Message = { 0 };

	while (Message.message != WM_QUIT)
	{
		if (PeekMessageW(&Message, NULL, 0, 0, PM_REMOVE))
		{
			THROW_ON_FALSE(TranslateMessage(&Message));
			DispatchMessageW(&Message);
		}
	}

	WaitForPreviousFrame(&SyncObjects, &DxObjects);

	ID3D12Resource_Unmap(DxObjects.ConstantBuffer, 0, NULL);
	THROW_ON_FAIL(ID3D12Resource_Release(DxObjects.ConstantBuffer));

	THROW_ON_FAIL(ID3D12Resource_Release(ObjectUploadBuffer));

	for (int i = 0; i < ObjectInfo.MeshCount; i++)
	{
		for (int j = 0; j < ObjectInfo.MeshList[i].VertexBufferCount; j++)
		{
			THROW_ON_FAIL(ID3D12Resource_Release(ObjectInfo.MeshList[i].VertexResources[j]));
		}

		THROW_ON_FAIL(ID3D12Resource_Release(ObjectInfo.MeshList[i].IndexResource));
		THROW_ON_FAIL(ID3D12Resource_Release(ObjectInfo.MeshList[i].MeshletResource));
		THROW_ON_FAIL(ID3D12Resource_Release(ObjectInfo.MeshList[i].UniqueVertexIndexResource));
		THROW_ON_FAIL(ID3D12Resource_Release(ObjectInfo.MeshList[i].PrimitiveIndexResource));
		THROW_ON_FAIL(ID3D12Resource_Release(ObjectInfo.MeshList[i].CullDataResource));
		THROW_ON_FAIL(ID3D12Resource_Release(ObjectInfo.MeshList[i].MeshInfoResource));
	}

	THROW_ON_FAIL(ID3D12PipelineState_Release(DxObjects.PipelineState));

	THROW_ON_FAIL(ID3D12RootSignature_Release(DxObjects.RootSignature));

	for (int i = 0; i < BUFFER_COUNT; i++)
	{
		THROW_ON_FAIL(ID3D12Resource_Release(DxObjects.RenderTargets[i]));
	}

	THROW_ON_FAIL(ID3D12Resource_Release(DxObjects.DepthStencil));

	THROW_ON_FAIL(IDXGISwapChain3_Release(DxObjects.SwapChain));

	THROW_ON_FALSE(CloseHandle(SyncObjects.FenceEvent));

	for (int i = 0; i < BUFFER_COUNT; i++)
	{
		THROW_ON_FAIL(ID3D12Fence_Release(SyncObjects.Fence[i]));
	}

	THROW_ON_FAIL(ID3D12GraphicsCommandList7_Release(DxObjects.CommandList));

	for(int i = 0; i < ARRAYSIZE(DxObjects.CommandAllocators); i++)
	{
		THROW_ON_FAIL(ID3D12CommandAllocator_Release(DxObjects.CommandAllocators[i]));
	}

	THROW_ON_FAIL(ID3D12CommandQueue_Release(DxObjects.CommandQueue));

	THROW_ON_FAIL(ID3D12DescriptorHeap_Release(DxObjects.RtvHeap));
	THROW_ON_FAIL(ID3D12DescriptorHeap_Release(DxObjects.DsvHeap));

#ifdef _DEBUG
	THROW_ON_FAIL(ID3D12InfoQueue_Release(InfoQueue));
#endif

	THROW_ON_FAIL(ID3D12Device10_Release(Device));

#ifdef _DEBUG
	THROW_ON_FAIL(ID3D12Debug6_Release(DebugController));
#endif

	THROW_ON_FALSE(UnregisterClassW(WindowClassName, Instance));

	THROW_ON_FALSE(DestroyCursor(Cursor));
	THROW_ON_FALSE(DestroyIcon(Icon));

#ifdef _DEBUG
	{
		IDXGIDebug1* DxgiDebug;
		THROW_ON_FAIL(DXGIGetDebugInterface1(0, &IID_IDXGIDebug1, &DxgiDebug));
		THROW_ON_FAIL(IDXGIDebug1_ReportLiveObjects(DxgiDebug, DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
	}
#endif

	return 0;
}

LRESULT CALLBACK PreInitProc(HWND Window, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProcW(Window, message, wParam, lParam);
	}
	return 0;
}

LRESULT CALLBACK IdleProc(HWND Window, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_PAINT:
		Sleep(25);
		break;
	case WM_SIZE:
		if (!IsIconic(Window))
			THROW_ON_FALSE(SetWindowLongPtrW(Window, GWLP_WNDPROC, WindowProc) != 0);
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProcW(Window, message, wParam, lParam);
	}
	return 0;
}

LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam)
{
	static struct
	{
		bool w;
		bool a;
		bool s;
		bool d;
		bool left;
		bool right;
		bool up;
		bool down;
	} KeysPressed = { 0 };

	static struct
	{
		vec3 StartingPosition;
		vec3 Position;
		float Yaw;// Relative to the +z axis.
		float Pitch;// Relative to the xz plane.
		vec3 LookDirection;
		vec3 UpDirection;
		float MoveSpeed;// Speed at which the camera moves, in units per second.
		float TurnSpeed;// Speed at which the camera turns, in radians per second.
	} Camera =
	{
		.StartingPosition = { 0, 75, 150 },
		.Position = { 0, 75, 150 },
		.Yaw = M_PI,
		.Pitch = 0.0f,
		.LookDirection = { 0, 0, -1 },
		.UpDirection = { 0, 1, 0 },
		.MoveSpeed = 150.0f,
		.TurnSpeed = M_PI_2
	};

	static struct
	{
		UINT32 FrameCount;
		UINT32 FramesPerSecond;
		UINT32 FramesThisSecond;
		UINT64 SecondCounter;
		UINT64 ElapsedTicks;
		UINT64 TotalTicks;
		UINT64 LeftOverTicks;
		LARGE_INTEGER Frequency;
		LARGE_INTEGER LastTime;
		UINT64 MaxDelta;
	} Timer = { 0 };

	static struct SceneConstantBuffer ConstantBufferData = { 0 };
	
	static D3D12_VIEWPORT Viewport =
	{
		.TopLeftX = 0.0f,
		.TopLeftY = 0.0f,
		.Width = 0,//filled in later
		.Height = 0,//filled in later
		.MinDepth = D3D12_MIN_DEPTH,
		.MaxDepth = D3D12_MAX_DEPTH
	};

	static D3D12_RECT ScissorRect =
	{
		.left = 0,
		.top = 0,
		.right = 0,//filled in later
		.bottom = 0//filled in later
	};

	static struct SyncObjects* SyncObjects;
	static struct DxObjects* DxObjects;
	static struct ObjectInfo* ObjectInfo;

	static UINT WindowWidth = 0;
	static UINT WindowHeight = 0;

	static bool bVsync = true;
	static bool bFullScreen = false;

	static const unsigned long long TICKS_PER_SECOND = 10000000ULL;

	switch (Message)
	{
	case WM_INIT:
		QueryPerformanceFrequency(&Timer.Frequency);
		QueryPerformanceCounter(&Timer.LastTime);

		// Initialize max delta to 1/10 of a second.
		Timer.MaxDelta = Timer.Frequency.QuadPart / 10;

		SyncObjects = ((struct WindowProcPayload*)wParam)->SyncObjects;
		DxObjects = ((struct WindowProcPayload*)wParam)->DxObjects;
		ObjectInfo = ((struct WindowProcPayload*)wParam)->ObjectInfo;
		break;
	case WM_KEYDOWN:
		switch (wParam)
		{
		case 'V':
			bVsync = !bVsync;
			break;
		case 'W':
			KeysPressed.w = true;
			break;
		case 'A':
			KeysPressed.a = true;
			break;
		case 'S':
			KeysPressed.s = true;
			break;
		case 'D':
			KeysPressed.d = true;
			break;
		case 'T':
			ConstantBufferData.DrawMeshlets = !ConstantBufferData.DrawMeshlets;
			break;
		case VK_LEFT:
			KeysPressed.left = true;
			break;
		case VK_RIGHT:
			KeysPressed.right = true;
			break;
		case VK_UP:
			KeysPressed.up = true;
			break;
		case VK_DOWN:
			KeysPressed.down = true;
			break;
		case VK_ESCAPE:
			glm_vec3_copy(Camera.StartingPosition, Camera.Position);
			Camera.Yaw = M_PI;
			Camera.Pitch = 0.0f;
			glm_vec3_copy((vec3) { 0, 0, -1 }, Camera.LookDirection);
			break;
		}
		break;
	case WM_SYSKEYDOWN:
		if (wParam == VK_RETURN && (lParam & 0x60000000) == 0x20000000)
		{
			bFullScreen = !bFullScreen;

			if (bFullScreen)
			{
				THROW_ON_FALSE(SetWindowLongPtrW(Window, GWL_EXSTYLE, WS_EX_TOPMOST) != 0);
				THROW_ON_FALSE(SetWindowLongPtrW(Window, GWL_STYLE, 0) != 0);

				THROW_ON_FALSE(ShowWindow(Window, SW_SHOWMAXIMIZED));
			}
			else
			{
				THROW_ON_FALSE(SetWindowLongPtrW(Window, GWL_STYLE, WS_OVERLAPPEDWINDOW) != 0);
				THROW_ON_FALSE(SetWindowLongPtrW(Window, GWL_EXSTYLE, 0) != 0);

				THROW_ON_FALSE(ShowWindow(Window, SW_SHOWMAXIMIZED));
			}
		}
		break;
	case WM_KEYUP:
		switch (wParam)
		{
		case 'W':
			KeysPressed.w = false;
			break;
		case 'A':
			KeysPressed.a = false;
			break;
		case 'S':
			KeysPressed.s = false;
			break;
		case 'D':
			KeysPressed.d = false;
			break;
		case VK_LEFT:
			KeysPressed.left = false;
			break;
		case VK_RIGHT:
			KeysPressed.right = false;
			break;
		case VK_UP:
			KeysPressed.up = false;
			break;
		case VK_DOWN:
			KeysPressed.down = false;
			break;
		}
		break;
	case WM_SIZE:
		if (IsIconic(Window))
		{
			THROW_ON_FALSE(SetWindowLongPtrW(Window, GWLP_WNDPROC, IdleProc) != 0);
			break;
		}

		if (WindowWidth == LOWORD(lParam) && WindowHeight == HIWORD(lParam))
			break;

		WaitForPreviousFrame(SyncObjects, DxObjects);

		WindowWidth = LOWORD(lParam);
		WindowHeight = HIWORD(lParam);

		Viewport.Width = WindowWidth;
		Viewport.Height = WindowHeight;

		ScissorRect.right = WindowWidth;
		ScissorRect.bottom = WindowHeight;

		{
			for (int i = 0; i < BUFFER_COUNT; i++)
			{
				if (DxObjects->RenderTargets[i])
					THROW_ON_FAIL(ID3D12Resource_Release(DxObjects->RenderTargets[i]));
				SyncObjects->FenceValues[i] = SyncObjects->FenceValues[SyncObjects->FrameIndex] + 1;
			}

			DXGI_SWAP_CHAIN_DESC swapChainDesc = { 0 };
			THROW_ON_FAIL(IDXGISwapChain3_GetDesc(DxObjects->SwapChain, &swapChainDesc));
			THROW_ON_FAIL(IDXGISwapChain3_ResizeBuffers(DxObjects->SwapChain, BUFFER_COUNT, WindowWidth, WindowHeight, swapChainDesc.BufferDesc.Format, swapChainDesc.Flags));

			SyncObjects->FrameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(DxObjects->SwapChain);

			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = { 0 };
			ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(DxObjects->RtvHeap, &rtvHandle);

			for (int i = 0; i < BUFFER_COUNT; i++)
			{
				THROW_ON_FAIL(IDXGISwapChain3_GetBuffer(DxObjects->SwapChain, i, &IID_ID3D12Resource, &DxObjects->RenderTargets[i]));

				ID3D12Device_CreateRenderTargetView(Device, DxObjects->RenderTargets[i], NULL, rtvHandle);

				rtvHandle.ptr += DxObjects->RtvDescriptorSize;
			}

			//now the depth buffer:
			if(DxObjects->DepthStencil)
				THROW_ON_FAIL(ID3D12Resource_Release(DxObjects->DepthStencil));

			{
				D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = { 0 };
				depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
				depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
				depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;

				D3D12_CLEAR_VALUE depthOptimizedClearValue = { 0 };
				depthOptimizedClearValue.Format = DXGI_FORMAT_D32_FLOAT;
				depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
				depthOptimizedClearValue.DepthStencil.Stencil = 0;

				D3D12_HEAP_PROPERTIES depthStencilHeapProps = { 0 };
				depthStencilHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
				depthStencilHeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				depthStencilHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
				depthStencilHeapProps.CreationNodeMask = 1;
				depthStencilHeapProps.VisibleNodeMask = 1;

				D3D12_RESOURCE_DESC depthStencilTextureDesc = { 0 };
				depthStencilTextureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				depthStencilTextureDesc.Alignment = 0;
				depthStencilTextureDesc.Width = WindowWidth;
				depthStencilTextureDesc.Height = WindowHeight;
				depthStencilTextureDesc.DepthOrArraySize = 1;
				depthStencilTextureDesc.MipLevels = 0;
				depthStencilTextureDesc.Format = DEPTH_BUFFER_FORMAT;
				depthStencilTextureDesc.SampleDesc.Count = 1;
				depthStencilTextureDesc.SampleDesc.Quality = 0;
				depthStencilTextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
				depthStencilTextureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

				THROW_ON_FAIL(ID3D12Device2_CreateCommittedResource(
					Device,
					&depthStencilHeapProps,
					D3D12_HEAP_FLAG_NONE,
					&depthStencilTextureDesc,
					D3D12_RESOURCE_STATE_DEPTH_WRITE,
					&depthOptimizedClearValue,
					&IID_ID3D12Resource,
					&DxObjects->DepthStencil
				));

#ifdef _DEBUG
				THROW_ON_FAIL(ID3D12Resource_SetName(DxObjects->DepthStencil, L"depth stencil"));
#endif
			}

			D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorHandle;
			ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(DxObjects->DsvHeap, &CpuDescriptorHandle);

			D3D12_DEPTH_STENCIL_VIEW_DESC DepthStencilDesc = { 0 };
			DepthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
			DepthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
			DepthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;

			ID3D12Device_CreateDepthStencilView(Device, DxObjects->DepthStencil, &DepthStencilDesc, CpuDescriptorHandle);
		}
		break;
	case WM_PAINT:
	{
		WaitForPreviousFrame(SyncObjects, DxObjects);

		LARGE_INTEGER currentTime;
		QueryPerformanceCounter(&currentTime);

		UINT64 timeDelta = currentTime.QuadPart - Timer.LastTime.QuadPart;

		Timer.LastTime = currentTime;
		Timer.SecondCounter += timeDelta;

		// Clamp excessively large time deltas (e.g. after paused in the debugger).
		if (timeDelta > Timer.MaxDelta)
		{
			timeDelta = Timer.MaxDelta;
		}

		// Convert QPC units into a canonical tick format. This cannot overflow due to the previous clamp.
		timeDelta *= TICKS_PER_SECOND;
		timeDelta /= Timer.Frequency.QuadPart;

		UINT32 lastFrameCount = Timer.FrameCount;

		// Variable timestep update logic.
		Timer.ElapsedTicks = timeDelta;
		Timer.TotalTicks += timeDelta;
		Timer.LeftOverTicks = 0;
		Timer.FrameCount++;

		// Track the current framerate.
		if (Timer.FrameCount != lastFrameCount)
		{
			Timer.FramesThisSecond++;
		}

		if (Timer.SecondCounter >= Timer.Frequency.QuadPart)
		{
			Timer.FramesPerSecond = Timer.FramesThisSecond;
			Timer.FramesThisSecond = 0;
			Timer.SecondCounter %= Timer.Frequency.QuadPart;
		}
		
		if (SyncObjects->FrameCounter++ % 30 == 0)
		{
			// Update window text with FPS value.
			wchar_t FPS[64];
			_snwprintf_s(FPS, 64, _TRUNCATE, L"D3D12 Mesh Shader: %ufps\0", Timer.FramesPerSecond);

			THROW_ON_FALSE(SetWindowTextW(Window, FPS));
		}

		// Calculate the move vector in camera space.
		vec3 move = { 0, 0, 0 };

		if (KeysPressed.a)
			move[0] -= 1.0f;
		if (KeysPressed.d)
			move[0] += 1.0f;
		if (KeysPressed.w)
			move[2] -= 1.0f;
		if (KeysPressed.s)
			move[2] += 1.0f;

		if (fabs(move[0]) > 0.1f && fabs(move[2]) > 0.1f)
		{
			vec3 v3move;
			glm_vec3_copy(move, v3move);

			glm_vec3_normalize(v3move);

			move[0] = v3move[0];
			move[2] = v3move[2];
		}

		const float moveInterval = Camera.MoveSpeed * (((float)Timer.ElapsedTicks) / TICKS_PER_SECOND);
		const float RotateInterval = Camera.TurnSpeed * (((float)Timer.ElapsedTicks) / TICKS_PER_SECOND);

		if (KeysPressed.left)
			Camera.Yaw += RotateInterval;
		if (KeysPressed.right)
			Camera.Yaw -= RotateInterval;
		if (KeysPressed.up)
			Camera.Pitch += RotateInterval;
		if (KeysPressed.down)
			Camera.Pitch -= RotateInterval;
			
		// Prevent looking too far up or down.
		Camera.Pitch = fmin(Camera.Pitch, M_PI_4);
		Camera.Pitch = fmax(-M_PI_4, Camera.Pitch);

		// Move the camera in model space.
		const float x = move[0] * -cosf(Camera.Yaw) - move[2] * sinf(Camera.Yaw);
		const float z = move[0] * sinf(Camera.Yaw) - move[2] * cosf(Camera.Yaw);
		Camera.Position[0] += x * moveInterval;
		Camera.Position[2] += z * moveInterval;

		// Determine the look direction.
		const float r = cosf(Camera.Pitch);
		Camera.LookDirection[0] = r * sinf(Camera.Yaw);
		Camera.LookDirection[1] = sinf(Camera.Pitch);
		Camera.LookDirection[2] = r * cosf(Camera.Yaw);
		
		mat4 WorldM4;
		glm_mat4_identity(WorldM4);

		mat4 ViewM4;
		glm_look_rh(Camera.Position, Camera.LookDirection, Camera.UpDirection, ViewM4);

		//glmf_perspective
		mat4 ProjM4;
		glm_perspective(M_PI / 3.0f, (float)WindowWidth / (float)WindowHeight, 1.0f, 1000.0f, ProjM4);

		glm_mat4_transpose_to(WorldM4, ConstantBufferData.World);

		mat4 WorldxView;
		glm_mat4_mul(WorldM4, ViewM4, WorldxView);
		glm_mat4_transpose_to(WorldxView, ConstantBufferData.WorldView);

		mat4 WorldxViewxProj;
		glm_mat4_mul(ProjM4, WorldxView, WorldxViewxProj);
		glm_mat4_transpose_to(WorldxViewxProj, ConstantBufferData.WorldViewProj);

		MEMCPY_VERIFY(memcpy_s(DxObjects->CbvDataBegin + sizeof(struct SceneConstantBuffer) * SyncObjects->FrameIndex, sizeof(ConstantBufferData), &ConstantBufferData, sizeof(ConstantBufferData)));

		// Command list allocators can only be reset when the associated 
		// command lists have finished execution on the GPU; apps should use 
		// fences to determine GPU execution progress.
		THROW_ON_FAIL(ID3D12CommandAllocator_Reset(DxObjects->CommandAllocators[SyncObjects->FrameIndex]));

		// However, when ExecuteCommandList() is called on a particular command 
		// list, that command list can then be reset at any time and must be before 
		// re-recording.
		THROW_ON_FAIL(ID3D12GraphicsCommandList7_Reset(DxObjects->CommandList, DxObjects->CommandAllocators[SyncObjects->FrameIndex], DxObjects->PipelineState));

		// Set necessary state.
		ID3D12GraphicsCommandList7_SetGraphicsRootSignature(DxObjects->CommandList, DxObjects->RootSignature);
		ID3D12GraphicsCommandList7_RSSetViewports(DxObjects->CommandList, 1, &Viewport);
		ID3D12GraphicsCommandList7_RSSetScissorRects(DxObjects->CommandList, 1, &ScissorRect);

		// Indicate that the back buffer will be used as a render target.
		{
			D3D12_TEXTURE_BARRIER TextureBarrier = { 0 };
			TextureBarrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
			TextureBarrier.SyncAfter = D3D12_BARRIER_SYNC_RENDER_TARGET;
			TextureBarrier.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
			TextureBarrier.AccessAfter = D3D12_BARRIER_ACCESS_RENDER_TARGET;
			TextureBarrier.LayoutBefore = D3D12_BARRIER_LAYOUT_PRESENT;
			TextureBarrier.LayoutAfter = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
			TextureBarrier.pResource = DxObjects->RenderTargets[SyncObjects->FrameIndex];
			TextureBarrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

			D3D12_BARRIER_GROUP ResourceBarrier = { 0 };
			ResourceBarrier.Type = D3D12_BARRIER_TYPE_TEXTURE;
			ResourceBarrier.NumBarriers = 1;
			ResourceBarrier.pTextureBarriers = &TextureBarrier;
			ID3D12GraphicsCommandList7_Barrier(DxObjects->CommandList, 1, &ResourceBarrier);
		}

		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = { 0 };

		ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(DxObjects->RtvHeap, &rtvHandle);
		rtvHandle.ptr += SyncObjects->FrameIndex * DxObjects->RtvDescriptorSize;

		D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle;
		ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(DxObjects->DsvHeap, &dsvHandle);

		ID3D12GraphicsCommandList7_OMSetRenderTargets(DxObjects->CommandList, 1, &rtvHandle, FALSE, &dsvHandle);

		// Record commands.
		const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
		ID3D12GraphicsCommandList7_ClearRenderTargetView(DxObjects->CommandList, rtvHandle, clearColor, 0, NULL);
		ID3D12GraphicsCommandList7_ClearDepthStencilView(DxObjects->CommandList, dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

		ID3D12GraphicsCommandList7_SetGraphicsRootConstantBufferView(DxObjects->CommandList, 0, ID3D12Resource_GetGPUVirtualAddress(DxObjects->ConstantBuffer) + sizeof(struct SceneConstantBuffer) * SyncObjects->FrameIndex);

		for (int i = 0; i < ObjectInfo->MeshCount; i++)
		{
			ID3D12GraphicsCommandList7_SetGraphicsRoot32BitConstant(DxObjects->CommandList, 1, ObjectInfo->MeshList[i].IndexSize, 0);
			ID3D12GraphicsCommandList7_SetGraphicsRootShaderResourceView(DxObjects->CommandList, 2, ID3D12Resource_GetGPUVirtualAddress(ObjectInfo->MeshList[i].VertexResources[0]));
			ID3D12GraphicsCommandList7_SetGraphicsRootShaderResourceView(DxObjects->CommandList, 3, ID3D12Resource_GetGPUVirtualAddress(ObjectInfo->MeshList[i].MeshletResource));
			ID3D12GraphicsCommandList7_SetGraphicsRootShaderResourceView(DxObjects->CommandList, 4, ID3D12Resource_GetGPUVirtualAddress(ObjectInfo->MeshList[i].UniqueVertexIndexResource));
			ID3D12GraphicsCommandList7_SetGraphicsRootShaderResourceView(DxObjects->CommandList, 5, ID3D12Resource_GetGPUVirtualAddress(ObjectInfo->MeshList[i].PrimitiveIndexResource));

			for (int j = 0; j < ObjectInfo->MeshList[i].MeshletSubsetCount; j++)
			{
				ID3D12GraphicsCommandList7_SetGraphicsRoot32BitConstant(DxObjects->CommandList, 1, ObjectInfo->MeshList[i].MeshletSubsets[j].Offset, 1);
				ID3D12GraphicsCommandList7_DispatchMesh(DxObjects->CommandList, ObjectInfo->MeshList[i].MeshletSubsets[j].Count, 1, 1);
			}
		}

		{
			D3D12_TEXTURE_BARRIER TextureBarrier = { 0 };
			TextureBarrier.SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET;
			TextureBarrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
			TextureBarrier.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
			TextureBarrier.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
			TextureBarrier.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
			TextureBarrier.LayoutAfter = D3D12_BARRIER_LAYOUT_PRESENT;
			TextureBarrier.pResource = DxObjects->RenderTargets[SyncObjects->FrameIndex];
			TextureBarrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

			D3D12_BARRIER_GROUP ResourceBarrier = { 0 };
			ResourceBarrier.Type = D3D12_BARRIER_TYPE_TEXTURE;
			ResourceBarrier.NumBarriers = 1;
			ResourceBarrier.pTextureBarriers = &TextureBarrier;
			ID3D12GraphicsCommandList7_Barrier(DxObjects->CommandList, 1, &ResourceBarrier);
		}

		THROW_ON_FAIL(ID3D12GraphicsCommandList7_Close(DxObjects->CommandList));
		
		ID3D12CommandList* ppCommandLists[] = { DxObjects->CommandList };
		ID3D12CommandQueue_ExecuteCommandLists(DxObjects->CommandQueue, ARRAYSIZE(ppCommandLists), ppCommandLists);

		THROW_ON_FAIL(IDXGISwapChain3_Present(DxObjects->SwapChain, bVsync ? 1 : 0, bVsync ? 0 : DXGI_PRESENT_ALLOW_TEARING));

		THROW_ON_FAIL(ID3D12CommandQueue_Signal(DxObjects->CommandQueue, SyncObjects->Fence[SyncObjects->FrameIndex], SyncObjects->FenceValues[SyncObjects->FrameIndex]));

		SyncObjects->FrameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(DxObjects->SwapChain);

		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProcW(Window, Message, wParam, lParam);
	}

	return 0;
}

void WaitForPreviousFrame(struct SyncObjects* SyncObjects, struct DxObjects* DxObjects)
{
	SyncObjects->FrameIndex = IDXGISwapChain3_GetCurrentBackBufferIndex(DxObjects->SwapChain);
	THROW_ON_FAIL(ID3D12CommandQueue_Signal(DxObjects->CommandQueue, SyncObjects->Fence[SyncObjects->FrameIndex], ++SyncObjects->FenceValues[SyncObjects->FrameIndex]));

	if (ID3D12Fence_GetCompletedValue(SyncObjects->Fence[SyncObjects->FrameIndex]) < SyncObjects->FenceValues[SyncObjects->FrameIndex])
	{
		THROW_ON_FAIL(ID3D12Fence_SetEventOnCompletion(SyncObjects->Fence[SyncObjects->FrameIndex], SyncObjects->FenceValues[SyncObjects->FrameIndex], SyncObjects->FenceEvent));
		THROW_ON_FALSE(WaitForSingleObjectEx(SyncObjects->FenceEvent, INFINITE, false) == WAIT_OBJECT_0);
	}
}
