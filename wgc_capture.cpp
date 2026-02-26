#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

// IDirect3DDxgiInterfaceAccess may not resolve from the interop header on all
// SDK versions, so declare it explicitly.
MIDL_INTERFACE("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")
IDirect3DDxgiInterfaceAccess : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **p) = 0;
};

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

static com_ptr<ID3D11Device> g_d3dDevice;
static com_ptr<ID3D11DeviceContext> g_d3dContext;
static IDirect3DDevice g_winrtDevice{nullptr};
static bool g_initialized = false;

static bool InitD3D() {
  if (g_initialized)
    return true;

  D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, featureLevels, ARRAYSIZE(featureLevels),
                                 D3D11_SDK_VERSION, g_d3dDevice.put(), nullptr,
                                 g_d3dContext.put());

  if (FAILED(hr))
    return false;

  com_ptr<IDXGIDevice> dxgiDevice;
  hr = g_d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
  if (FAILED(hr))
    return false;

  com_ptr<IInspectable> inspectable;
  hr =
      CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
  if (FAILED(hr))
    return false;

  g_winrtDevice = inspectable.as<IDirect3DDevice>();
  g_initialized = true;
  return true;
}

static GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd) {
  auto interopFactory = get_activation_factory<GraphicsCaptureItem,
                                               IGraphicsCaptureItemInterop>();
  GraphicsCaptureItem item{nullptr};
  check_hresult(interopFactory->CreateForWindow(
      hwnd, guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
      put_abi(item)));
  return item;
}

// Returns: 0 = success, >0 = error code
// When buffer is NULL, only populates outWidth/outHeight (query mode).
// When buffer is non-NULL, captures pixels into buffer as BGRA.
extern "C" __declspec(dllexport) int32_t __stdcall
WgcCaptureWindow(HWND hwnd, uint8_t *buffer, int32_t bufferSize,
                 int32_t *outWidth, int32_t *outHeight) {
  try {
    if (!InitD3D())
      return 1;

    auto item = CreateCaptureItemForWindow(hwnd);
    auto size = item.Size();

    int32_t w = size.Width;
    int32_t h = size.Height;

    if (outWidth)
      *outWidth = w;
    if (outHeight)
      *outHeight = h;

    if (buffer == nullptr)
      return 0;

    int32_t stride = w * 4;
    int32_t requiredSize = stride * h;
    if (bufferSize < requiredSize)
      return 2;

    auto framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        g_winrtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);

    auto session = framePool.CreateCaptureSession(item);

    try {
      session.IsBorderRequired(false);
    } catch (...) {
    }
    try {
      session.IsCursorCaptureEnabled(false);
    } catch (...) {
    }

    HANDLE frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Direct3D11CaptureFrame capturedFrame{nullptr};

    auto token = framePool.FrameArrived(
        [&](Direct3D11CaptureFramePool const &pool,
            winrt::Windows::Foundation::IInspectable const &) {
          capturedFrame = pool.TryGetNextFrame();
          SetEvent(frameEvent);
        });

    session.StartCapture();

    DWORD waitResult = WaitForSingleObject(frameEvent, 3000);
    CloseHandle(frameEvent);

    if (waitResult != WAIT_OBJECT_0 || capturedFrame == nullptr) {
      session.Close();
      framePool.Close();
      return 3;
    }

    auto surface = capturedFrame.Surface();
    auto access = surface.as<IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> frameTex;
    check_hresult(access->GetInterface(IID_PPV_ARGS(frameTex.put())));

    D3D11_TEXTURE2D_DESC desc;
    frameTex->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    com_ptr<ID3D11Texture2D> stagingTex;
    check_hresult(
        g_d3dDevice->CreateTexture2D(&desc, nullptr, stagingTex.put()));

    g_d3dContext->CopyResource(stagingTex.get(), frameTex.get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    check_hresult(
        g_d3dContext->Map(stagingTex.get(), 0, D3D11_MAP_READ, 0, &mapped));

    int32_t copyW = (int32_t)desc.Width < w ? (int32_t)desc.Width : w;
    int32_t copyH = (int32_t)desc.Height < h ? (int32_t)desc.Height : h;

    if (outWidth)
      *outWidth = copyW;
    if (outHeight)
      *outHeight = copyH;

    for (int32_t y = 0; y < copyH; y++) {
      uint8_t *srcRow = (uint8_t *)mapped.pData + y * mapped.RowPitch;
      uint8_t *dstRow = buffer + y * (copyW * 4);
      memcpy(dstRow, srcRow, copyW * 4);
    }

    g_d3dContext->Unmap(stagingTex.get(), 0);

    capturedFrame.Close();
    session.Close();
    framePool.Close();

    return 0;

  } catch (hresult_error const &) {
    return 4;
  } catch (...) {
    return 5;
  }
}

extern "C" __declspec(dllexport) int32_t __stdcall WgcIsSupported() {
  try {
    return GraphicsCaptureSession::IsSupported() ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

extern "C" __declspec(dllexport) void __stdcall WgcCleanup() {
  g_winrtDevice = nullptr;
  g_d3dContext = nullptr;
  g_d3dDevice = nullptr;
  g_initialized = false;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    break;
  case DLL_PROCESS_DETACH:
    WgcCleanup();
    winrt::uninit_apartment();
    break;
  }
  return TRUE;
}
