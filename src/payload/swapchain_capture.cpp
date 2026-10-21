#include "swapchain_capture.hpp"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include <MinHook.h>

using Microsoft::WRL::ComPtr;

namespace swapchain_capture {
namespace {

constexpr int kPresentIndex = 8;
constexpr int kPresent1Index = 22;
constexpr int kExecuteCommandListsIndex = 10;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                       ID3D12CommandList* const*);

PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ExecuteCommandListsFn g_execute_command_lists = nullptr;

// vkd3d-proton's IDXGISwapChain::GetDevice returns the ID3D12Device rather than the
// command queue (libs/vkd3d/swapchain.c, dxgi_vk_swap_chain_GetDevice), so the queue
// is taken from the game's own ExecuteCommandLists calls instead.
std::atomic<ID3D12CommandQueue*> g_queue{nullptr};

std::atomic<bool> g_pending{false};
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_done = false;
bool g_ok = false;
Frame g_frame;
std::string g_error;

using PixelFn = void (*)(const uint8_t* in, uint8_t* out);

PixelFn pixel_converter(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return [](const uint8_t* in, uint8_t* out) {
            out[0] = in[0];
            out[1] = in[1];
            out[2] = in[2];
        };
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return [](const uint8_t* in, uint8_t* out) {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
        };
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return [](const uint8_t* in, uint8_t* out) {
            uint32_t v;
            std::memcpy(&v, in, sizeof(v));
            out[0] = static_cast<uint8_t>(v >> 22);
            out[1] = static_cast<uint8_t>(v >> 12);
            out[2] = static_cast<uint8_t>(v >> 2);
        };
    default:
        return nullptr;
    }
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

bool read_back_buffer(IDXGISwapChain* swap, ID3D12CommandQueue* queue, Frame& out,
                      std::string& error)
{
    ComPtr<IDXGISwapChain3> swap3;
    if (FAILED(swap->QueryInterface(IID_PPV_ARGS(&swap3))))
    {
        error = "swapchain does not support IDXGISwapChain3";
        return false;
    }
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(swap3->GetBuffer(swap3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer))))
    {
        error = "back buffer is not a Direct3D 12 resource";
        return false;
    }
    ComPtr<ID3D12Device> device;
    buffer->GetDevice(IID_PPV_ARGS(&device));

    D3D12_RESOURCE_DESC desc = buffer->GetDesc();
    PixelFn convert = pixel_converter(desc.Format);
    if (!convert)
    {
        error = "unsupported back buffer format " + std::to_string(desc.Format);
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = total;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        error = "failed to create Direct3D 12 readback objects";
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = buffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    auto to_copy = transition(buffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto to_present = transition(buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                 D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(1, &to_copy);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    list->ResourceBarrier(1, &to_present);
    list->Close();

    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, event);
    DWORD wait = WaitForSingleObject(event, 2000);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0)
    {
        error = "timed out waiting for the back buffer copy";
        return false;
    }

    D3D12_RANGE read_range{0, static_cast<SIZE_T>(total)};
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, &read_range, &mapped)))
    {
        error = "Map(readback) failed";
        return false;
    }
    out.width = static_cast<unsigned>(desc.Width);
    out.height = desc.Height;
    out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
    const auto* base = static_cast<const uint8_t*>(mapped) + footprint.Offset;
    for (unsigned y = 0; y < out.height; ++y)
    {
        const uint8_t* row = base + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        uint8_t* dst_row = out.bgra.data() + static_cast<size_t>(y) * out.width * 4;
        for (unsigned x = 0; x < out.width; ++x)
        {
            convert(row + x * 4, dst_row + x * 4);
            dst_row[x * 4 + 3] = 255;
        }
    }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    return true;
}

void on_present(IDXGISwapChain* swap, UINT flags)
{
    ID3D12CommandQueue* queue = g_queue.load();
    if ((flags & DXGI_PRESENT_TEST) || !queue || !g_pending.exchange(false))
        return;
    Frame frame;
    std::string error;
    bool ok = read_back_buffer(swap, queue, frame, error);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ok = ok;
        g_frame = std::move(frame);
        g_error = std::move(error);
        g_done = true;
    }
    g_cv.notify_all();
}

HRESULT STDMETHODCALLTYPE present_detour(IDXGISwapChain* swap, UINT sync_interval, UINT flags)
{
    on_present(swap, flags);
    return g_present(swap, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE present1_detour(IDXGISwapChain1* swap, UINT sync_interval, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS* params)
{
    on_present(swap, flags);
    return g_present1(swap, sync_interval, flags, params);
}

void STDMETHODCALLTYPE execute_command_lists_detour(ID3D12CommandQueue* queue, UINT count,
                                                    ID3D12CommandList* const* lists)
{
    if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        g_queue.store(queue);
    g_execute_command_lists(queue, count, lists);
}

template <typename Fn>
bool hook(void* target, Fn detour, Fn* original)
{
    return MH_CreateHook(target, reinterpret_cast<void*>(detour),
                         reinterpret_cast<void**>(original)) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}

bool hook_vtables(HWND hwnd, std::string& error)
{
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        error = "D3D12CreateDevice failed";
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))))
    {
        error = "CreateCommandQueue failed";
        return false;
    }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        error = "CreateDXGIFactory1 failed";
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.Width = 64;
    swap_desc.Height = 64;
    swap_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &swap_desc, nullptr, nullptr,
                                               &swap)))
    {
        error = "CreateSwapChainForHwnd failed";
        return false;
    }

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        error = "MH_Initialize failed";
        return false;
    }
    void** swap_vtable = *reinterpret_cast<void***>(swap.Get());
    void** queue_vtable = *reinterpret_cast<void***>(queue.Get());
    if (!hook(queue_vtable[kExecuteCommandListsIndex], &execute_command_lists_detour,
              &g_execute_command_lists) ||
        !hook(swap_vtable[kPresentIndex], &present_detour, &g_present) ||
        !hook(swap_vtable[kPresent1Index], &present1_detour, &g_present1))
    {
        error = "failed to hook the Direct3D 12 swapchain";
        return false;
    }
    return true;
}

bool install(std::string& error)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"bedrock_mcp_swapchain_probe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        error = "CreateWindowEx failed";
        return false;
    }
    bool ok = hook_vtables(hwnd, error);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace

bool grab(Frame& out, std::string& error)
{
    static std::string install_error;
    static const bool installed = install(install_error);
    if (!installed)
    {
        error = install_error;
        return false;
    }

    std::unique_lock<std::mutex> lock(g_mutex);
    g_done = false;
    g_pending.store(true);
    if (!g_cv.wait_for(lock, std::chrono::seconds(3), [] { return g_done; }))
    {
        g_pending.store(false);
        error = g_queue.load() ? "timed out waiting for a presented frame"
                               : "no Direct3D 12 command queue seen; the game may not be "
                                 "rendering with Direct3D 12";
        return false;
    }
    if (!g_ok)
    {
        error = g_error;
        return false;
    }
    out = std::move(g_frame);
    return true;
}

} // namespace swapchain_capture
