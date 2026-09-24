#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <d2d1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kOutputWidth = 1920;
constexpr UINT kOutputHeight = 1080;
constexpr wchar_t kWindowClass[] = L"RearSilverDirectCompositionDiagnostic19";
constexpr wchar_t kWindowTitle[] = L"RearSilver Avatar - Diagnostic 19 - DirectComposition";
constexpr UINT kRenderFailureMessage = WM_APP + 19;

HWND g_window = nullptr;
std::atomic<bool> g_running{false};
std::atomic<bool> g_transformPending{true};
std::atomic<bool> g_transparentBackground{false};
std::atomic<UINT> g_clientWidth{1};
std::atomic<UINT> g_clientHeight{1};
std::atomic<LONG> g_pointerX{-1};
std::atomic<LONG> g_pointerY{-1};
std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;

void logMessage(const std::wstring &message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t prefix[80]{};
    swprintf_s(prefix, L"%02u:%02u:%02u.%03u | ", time.wHour, time.wMinute, time.wSecond,
               time.wMilliseconds);
    const std::wstring line = std::wstring(prefix) + message + L"\r\n";
    OutputDebugStringW(line.c_str());
    if (g_logFile != INVALID_HANDLE_VALUE) {
        const int byteCount = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                                   static_cast<int>(line.size()), nullptr, 0,
                                                   nullptr, nullptr);
        std::string bytes(static_cast<size_t>(byteCount), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                            bytes.data(), byteCount, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(g_logFile, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }
}

void startLog()
{
    wchar_t temp[MAX_PATH + 1]{};
    if (!GetTempPathW(MAX_PATH, temp))
        return;
    const std::wstring path = std::wstring(temp) + L"RearSilverAvatar-Diagnostic19.log";
    g_logFile = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    logMessage(L"Diagnostic 19 Milestone 1: fixed composition swap chain acquisition test");
    logMessage(L"Log path: " + path);
}

void check(HRESULT result)
{
    if (FAILED(result))
        throw result;
}

bool sameLuid(const LUID &a, const LUID &b)
{
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

bool sameAdapterIdentity(const DXGI_ADAPTER_DESC1 &a, const DXGI_ADAPTER_DESC1 &b)
{
    return a.VendorId == b.VendorId && a.DeviceId == b.DeviceId && a.SubSysId == b.SubSysId;
}

struct AdapterCandidate {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    DXGI_ADAPTER_DESC1 description{};
    UINT relatedOpen = 0;
    UINT relatedTotal = 0;
    bool viable = false;
    bool selfOpen = false;
};

ComPtr<IDXGIAdapter1> chooseInteroperableAdapter()
{
    ComPtr<ID3D11Device> defaultDevice;
    ComPtr<ID3D11DeviceContext> defaultContext;
    D3D_FEATURE_LEVEL defaultFeature{};
    LUID defaultLuid{};
    bool haveDefault = false;
    const HRESULT defaultResult = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        defaultDevice.GetAddressOf(), &defaultFeature, defaultContext.GetAddressOf());
    if (SUCCEEDED(defaultResult)) {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(defaultDevice.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&description))) {
            defaultLuid = description.AdapterLuid;
            haveDefault = true;
        }
    }

    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    std::vector<AdapterCandidate> candidates;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumerate = factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (enumerate == DXGI_ERROR_NOT_FOUND)
            break;
        check(enumerate);
        AdapterCandidate candidate;
        candidate.adapter = adapter;
        check(adapter->GetDesc1(&candidate.description));
        if ((candidate.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            D3D_FEATURE_LEVEL feature{};
            candidate.viable = SUCCEEDED(D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, candidate.device.GetAddressOf(), &feature,
                candidate.context.GetAddressOf()));
        }
        logMessage(L"Candidate " + std::to_wstring(index) + L": " +
                   candidate.description.Description + L"; LUID=" +
                   std::to_wstring(candidate.description.AdapterLuid.HighPart) + L":" +
                   std::to_wstring(candidate.description.AdapterLuid.LowPart) + L"; viable=" +
                   std::to_wstring(candidate.viable));
        candidates.push_back(std::move(candidate));
    }

    std::vector<std::vector<bool>> matrix(candidates.size(),
                                          std::vector<bool>(candidates.size(), false));
    for (size_t from = 0; from < candidates.size(); ++from) {
        if (!candidates[from].viable)
            continue;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 64;
        description.Height = 64;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIResource> resource;
        HANDLE sharedHandle = nullptr;
        if (SUCCEEDED(candidates[from].device->CreateTexture2D(&description, nullptr,
                                                               texture.GetAddressOf())) &&
            SUCCEEDED(texture.As(&resource)) &&
            SUCCEEDED(resource->GetSharedHandle(&sharedHandle))) {
            for (size_t to = 0; to < candidates.size(); ++to) {
                if (!candidates[to].viable)
                    continue;
                ComPtr<ID3D11Texture2D> opened;
                matrix[from][to] = SUCCEEDED(candidates[to].device->OpenSharedResource(
                    sharedHandle, IID_PPV_ARGS(&opened)));
                logMessage(L"Matrix " + std::to_wstring(from) + L" -> " +
                           std::to_wstring(to) + L" = " + std::to_wstring(matrix[from][to]));
            }
        }
    }

    int defaultIndex = -1;
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (haveDefault && sameLuid(candidates[index].description.AdapterLuid, defaultLuid))
            defaultIndex = static_cast<int>(index);
        if (!candidates[index].viable)
            continue;
        candidates[index].selfOpen = matrix[index][index];
        for (size_t target = 0; target < candidates.size(); ++target) {
            if (candidates[target].viable &&
                sameAdapterIdentity(candidates[index].description, candidates[target].description)) {
                ++candidates[index].relatedTotal;
                if (matrix[index][target])
                    ++candidates[index].relatedOpen;
            }
        }
    }

    int choice = defaultIndex >= 0 && candidates[defaultIndex].viable &&
                         candidates[defaultIndex].selfOpen
                     ? defaultIndex
                     : -1;
    if (choice >= 0) {
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (candidates[index].viable && candidates[index].selfOpen &&
                sameAdapterIdentity(candidates[index].description,
                                    candidates[choice].description) &&
                candidates[index].relatedOpen > candidates[choice].relatedOpen)
                choice = static_cast<int>(index);
        }
    } else {
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (candidates[index].viable && candidates[index].selfOpen &&
                (choice < 0 || candidates[index].relatedOpen > candidates[choice].relatedOpen))
                choice = static_cast<int>(index);
        }
    }
    if (choice < 0)
        throw E_FAIL;
    logMessage(L"Selected candidate " + std::to_wstring(choice) + L"; related-open=" +
               std::to_wstring(candidates[choice].relatedOpen) + L"/" +
               std::to_wstring(candidates[choice].relatedTotal));
    return candidates[choice].adapter;
}

class DiagnosticRenderer {
public:
    void initialize(HWND window)
    {
        window_ = window;
        ComPtr<IDXGIAdapter1> adapter = chooseInteroperableAdapter();
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                           D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL actualLevel{};
        check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                device_.GetAddressOf(), &actualLevel, context_.GetAddressOf()));
        check(context_.As(&context1_));

        ComPtr<IDXGIFactory2> factory;
        check(adapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = kOutputWidth;
        description.Height = kOutputHeight;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        check(factory->CreateSwapChainForComposition(device_.Get(), &description, nullptr,
                                                      swapChain_.GetAddressOf()));

        ComPtr<ID3D11Texture2D> backBuffer;
        check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
        check(device_->CreateRenderTargetView(backBuffer.Get(), nullptr,
                                              target_.GetAddressOf()));

        ComPtr<IDXGIDevice> dxgiDevice;
        check(device_.As(&dxgiDevice));
        check(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&compositionDevice_)));
        check(compositionDevice_->CreateTargetForHwnd(window_, TRUE,
                                                       compositionTarget_.GetAddressOf()));
        check(compositionDevice_->CreateVisual(compositionVisual_.GetAddressOf()));
        check(compositionDevice_->CreateMatrixTransform(compositionTransform_.GetAddressOf()));
        check(compositionVisual_->SetContent(swapChain_.Get()));
        check(compositionVisual_->SetTransform(compositionTransform_.Get()));
        check(compositionTarget_->SetRoot(compositionVisual_.Get()));
        updateLocalTransform(g_clientWidth.load(), g_clientHeight.load());
        check(compositionDevice_->Commit());

        DXGI_SWAP_CHAIN_DESC legacyDescription{};
        check(swapChain_->GetDesc(&legacyDescription));
        logMessage(L"Composition swap chain created: " +
                   std::to_wstring(legacyDescription.BufferDesc.Width) + L"x" +
                   std::to_wstring(legacyDescription.BufferDesc.Height) + L"; buffers=" +
                   std::to_wstring(legacyDescription.BufferCount) + L"; OutputWindow=" +
                   std::to_wstring(reinterpret_cast<uintptr_t>(legacyDescription.OutputWindow)) +
                   L"; swapEffect=" + std::to_wstring(legacyDescription.SwapEffect));
    }

    void updateLocalTransform(UINT clientWidth, UINT clientHeight)
    {
        if (clientWidth == 0 || clientHeight == 0)
            return;
        const float scale = std::min(static_cast<float>(clientWidth) / kOutputWidth,
                                     static_cast<float>(clientHeight) / kOutputHeight);
        const float offsetX = (static_cast<float>(clientWidth) - kOutputWidth * scale) * 0.5f;
        const float offsetY = (static_cast<float>(clientHeight) - kOutputHeight * scale) * 0.5f;
        const D2D_MATRIX_3X2_F matrix{scale, 0.0f, 0.0f, scale, offsetX, offsetY};
        check(compositionTransform_->SetMatrix(matrix));
        check(compositionDevice_->Commit());
        logMessage(L"DComp aspect-fit transform: client=" + std::to_wstring(clientWidth) + L"x" +
                   std::to_wstring(clientHeight) + L"; scale=" + std::to_wstring(scale) +
                   L"; offset=" + std::to_wstring(offsetX) + L"," + std::to_wstring(offsetY));
    }

    void render()
    {
        const float pulse = 0.04f + 0.025f *
            (std::sin(static_cast<float>(GetTickCount64()) / 700.0f) + 1.0f);
        const bool transparent = g_transparentBackground.load();
        const float clear[] = {transparent ? 0.0f : pulse,
                               transparent ? 0.0f : pulse * 1.25f,
                               transparent ? 0.0f : pulse * 1.8f,
                               transparent ? 0.0f : 1.0f};
        context_->ClearRenderTargetView(target_.Get(), clear);

        const float seconds = static_cast<float>(GetTickCount64() % 6000) / 6000.0f;
        const LONG blockWidth = 240;
        const LONG blockHeight = 160;
        const LONG movingX = static_cast<LONG>(seconds * (kOutputWidth - blockWidth));
        const LONG movingY = static_cast<LONG>((0.5f + 0.35f *
            std::sin(seconds * 6.2831853f)) * (kOutputHeight - blockHeight));
        D3D11_RECT moving{movingX, movingY, movingX + blockWidth, movingY + blockHeight};
        const float cyan[] = {0.05f, 0.85f, 0.78f, 1.0f};
        context1_->ClearView(target_.Get(), cyan, &moving, 1);

        constexpr LONG markerSize = 34;
        const D3D11_RECT markers[] = {
            {0, 0, markerSize, markerSize},
            {static_cast<LONG>(kOutputWidth) - markerSize, 0,
             static_cast<LONG>(kOutputWidth), markerSize},
            {0, static_cast<LONG>(kOutputHeight) - markerSize, markerSize,
             static_cast<LONG>(kOutputHeight)},
            {static_cast<LONG>(kOutputWidth) - markerSize,
             static_cast<LONG>(kOutputHeight) - markerSize,
             static_cast<LONG>(kOutputWidth), static_cast<LONG>(kOutputHeight)}};
        const float red[] = {1.0f, 0.05f, 0.08f, 1.0f};
        context1_->ClearView(target_.Get(), red, markers,
                             static_cast<UINT>(std::size(markers)));

        const LONG pointerX = g_pointerX.load();
        const LONG pointerY = g_pointerY.load();
        if (pointerX >= 0 && pointerY >= 0 && pointerX < static_cast<LONG>(kOutputWidth) &&
            pointerY < static_cast<LONG>(kOutputHeight)) {
            constexpr LONG pointerSize = 24;
            const D3D11_RECT pointerMarker{
                std::max<LONG>(0, pointerX - pointerSize),
                std::max<LONG>(0, pointerY - pointerSize),
                std::min<LONG>(static_cast<LONG>(kOutputWidth), pointerX + pointerSize),
                std::min<LONG>(static_cast<LONG>(kOutputHeight), pointerY + pointerSize)};
            const float yellow[] = {1.0f, 0.82f, 0.05f, 1.0f};
            context1_->ClearView(target_.Get(), yellow, &pointerMarker, 1);
        }

        const HRESULT present = swapChain_->Present(1, 0);
        ++frames_;
        if (present != lastPresent_) {
            wchar_t result[80]{};
            swprintf_s(result, L"Present result changed to 0x%08X",
                       static_cast<unsigned>(present));
            logMessage(result);
            lastPresent_ = present;
        }
        check(present);

        const ULONGLONG now = GetTickCount64();
        if (now - lastReport_ >= 2000) {
            RECT client{};
            GetClientRect(window_, &client);
            const ULONGLONG elapsed = lastReport_ == 0 ? 0 : now - lastReport_;
            const ULONGLONG frameDelta = frames_ - lastFrames_;
            const double fps = elapsed == 0 ? 0.0 : frameDelta * 1000.0 / elapsed;
            wchar_t report[320]{};
            swprintf_s(report,
                       L"Frames=%llu; interval=%llu ms; fps=%.2f; Present=0x%08X; client=%ldx%ld; iconic=%u",
                       frames_, elapsed, fps, static_cast<unsigned>(present), client.right,
                       client.bottom, static_cast<unsigned>(IsIconic(window_) != FALSE));
            logMessage(report);
            lastFrames_ = frames_;
            lastReport_ = now;
        }
    }

private:
    HWND window_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext1> context1_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> compositionVisual_;
    ComPtr<IDCompositionMatrixTransform> compositionTransform_;
    ULONGLONG frames_ = 0;
    ULONGLONG lastFrames_ = 0;
    ULONGLONG lastReport_ = 0;
    HRESULT lastPresent_ = S_OK;
};

void renderThreadMain()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        DiagnosticRenderer renderer;
        renderer.initialize(g_window);
        while (g_running.load()) {
            if (g_transformPending.exchange(false))
                renderer.updateLocalTransform(g_clientWidth.load(), g_clientHeight.load());
            renderer.render();
        }
    } catch (HRESULT result) {
        wchar_t message[128]{};
        swprintf_s(message, L"Render failure: 0x%08X", static_cast<unsigned>(result));
        logMessage(message);
        PostMessageW(g_window, kRenderFailureMessage, static_cast<WPARAM>(result), 0);
    } catch (...) {
        logMessage(L"Render failure: unknown exception");
        PostMessageW(g_window, kRenderFailureMessage, static_cast<WPARAM>(E_FAIL), 0);
    }
    if (SUCCEEDED(com))
        CoUninitialize();
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_SIZE: {
        const UINT width = LOWORD(lParam);
        const UINT height = HIWORD(lParam);
        g_clientWidth.store(width);
        g_clientHeight.store(height);
        if (width > 0 && height > 0)
            g_transformPending.store(true);
        logMessage(L"WM_SIZE client=" + std::to_wstring(LOWORD(lParam)) + L"x" +
                   std::to_wstring(HIWORD(lParam)) + L"; state=" +
                   std::to_wstring(wParam) + L"; swap chain intentionally unchanged");
        return 0;
    }
    case WM_MOUSEMOVE: {
        RECT client{};
        GetClientRect(window, &client);
        const float clientWidth = static_cast<float>(client.right);
        const float clientHeight = static_cast<float>(client.bottom);
        if (clientWidth <= 0 || clientHeight <= 0)
            return 0;
        const float scale = std::min(clientWidth / kOutputWidth, clientHeight / kOutputHeight);
        const float offsetX = (clientWidth - kOutputWidth * scale) * 0.5f;
        const float offsetY = (clientHeight - kOutputHeight * scale) * 0.5f;
        const LONG mouseX = static_cast<short>(LOWORD(lParam));
        const LONG mouseY = static_cast<short>(HIWORD(lParam));
        g_pointerX.store(static_cast<LONG>((mouseX - offsetX) / scale));
        g_pointerY.store(static_cast<LONG>((mouseY - offsetY) / scale));
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == 'T') {
            g_transparentBackground.store(!g_transparentBackground.load());
            logMessage(L"Transparent background=" +
                       std::to_wstring(g_transparentBackground.load()));
        }
        return 0;
    case kRenderFailureMessage: {
        g_running.store(false);
        wchar_t text[220]{};
        swprintf_s(text, L"Diagnostic 19 stopped after graphics error 0x%08X. Check the log in your Temp folder.",
                   static_cast<unsigned>(wParam));
        MessageBoxW(window, text, L"Diagnostic 19", MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    startLog();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
        return 1;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassExW(&windowClass)) {
        CoUninitialize();
        return 2;
    }

    g_window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1120, 720, nullptr, nullptr,
                               instance, nullptr);
    if (!g_window) {
        CoUninitialize();
        return 3;
    }
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);

    RECT client{};
    GetClientRect(g_window, &client);
    g_clientWidth.store(static_cast<UINT>(std::max<LONG>(1, client.right)));
    g_clientHeight.store(static_cast<UINT>(std::max<LONG>(1, client.bottom)));

    g_running.store(true);
    std::thread renderThread(renderThreadMain);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_running.store(false);
    if (renderThread.joinable())
        renderThread.join();
    if (g_logFile != INVALID_HANDLE_VALUE)
        CloseHandle(g_logFile);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
