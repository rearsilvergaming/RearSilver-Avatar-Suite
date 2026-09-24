#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int kCaptureWidth = 960;
constexpr int kCaptureHeight = 720;
constexpr int kCaptureX = 548;
constexpr int kCaptureY = 104;
constexpr int kOpenDialogButton = 1001;
constexpr int kVisibilityButton = 1002;

HWND g_mainWindow{};
HWND g_captureWindow{};
HWND g_visibilityButton{};
std::atomic<bool> g_running{true};
std::atomic<bool> g_captureVisible{true};
std::atomic<unsigned long long> g_frameCount{};
std::atomic<long> g_lastPresent{S_OK};
std::mutex g_frameMutex;
std::mutex g_logMutex;
std::vector<std::uint32_t> g_previewFrame(kCaptureWidth * kCaptureHeight, 0xFF181B18u);
HANDLE g_log = INVALID_HANDLE_VALUE;

void logLine(const std::string &message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char line[2048]{};
    const int length = sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s\r\n", time.wYear,
                                 time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                                 time.wMilliseconds, message.c_str());
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD written{};
        WriteFile(g_log, line, static_cast<DWORD>(length), &written, nullptr);
        FlushFileBuffers(g_log);
    }
}

void startLog()
{
    wchar_t directory[MAX_PATH]{};
    GetTempPathW(MAX_PATH, directory);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t filename[200]{};
    swprintf_s(filename, L"RearSilverAvatar-architecture-diagnostic-%04u%02u%02u-%02u%02u%02u-%lu.log",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
               GetCurrentProcessId());
    g_log = CreateFileW((std::wstring(directory) + filename).c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    logLine("Disposable fixed-capture architecture diagnostic started; pid=" +
            std::to_string(GetCurrentProcessId()));
}

struct AdapterCandidate {
    IDXGIAdapter1 *adapter{};
    ID3D11Device *device{};
    ID3D11DeviceContext *context{};
    DXGI_ADAPTER_DESC1 description{};
    bool viable{};
    bool selfOpen{};
    UINT relatedOpen{};
    UINT relatedTotal{};
};

bool sameLuid(const LUID &left, const LUID &right)
{
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

bool sameAdapterIdentity(const DXGI_ADAPTER_DESC1 &left, const DXGI_ADAPTER_DESC1 &right)
{
    return left.VendorId == right.VendorId && left.DeviceId == right.DeviceId &&
           left.SubSysId == right.SubSysId;
}

IDXGIAdapter1 *chooseInteroperableAdapter()
{
    IDXGIFactory1 *factory{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        logLine("CreateDXGIFactory1 failed");
        return nullptr;
    }

    LUID defaultLuid{};
    bool haveDefault = false;
    ID3D11Device *defaultDevice{};
    ID3D11DeviceContext *defaultContext{};
    D3D_FEATURE_LEVEL defaultLevel{};
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, &defaultDevice, &defaultLevel,
                                    &defaultContext))) {
        IDXGIDevice *dxgiDevice{};
        IDXGIAdapter *adapter{};
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(defaultDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&description))) {
            defaultLuid = description.AdapterLuid;
            haveDefault = true;
        }
        if (adapter)
            adapter->Release();
        if (dxgiDevice)
            dxgiDevice->Release();
    }
    if (defaultContext)
        defaultContext->Release();
    if (defaultDevice)
        defaultDevice->Release();

    std::vector<AdapterCandidate> candidates;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1 *adapter{};
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        AdapterCandidate candidate{};
        candidate.adapter = adapter;
        adapter->GetDesc1(&candidate.description);
        const bool hardware = (candidate.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
        D3D_FEATURE_LEVEL level{};
        const HRESULT createResult = hardware
                                         ? D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                                             nullptr, 0, D3D11_SDK_VERSION,
                                                             &candidate.device, &level,
                                                             &candidate.context)
                                         : E_FAIL;
        candidate.viable = hardware && SUCCEEDED(createResult);
        char line[700]{};
        sprintf_s(line,
                  "Adapter %u: LUID=%ld:%lu vendor=%u device=%u subsystem=%u flags=0x%08X "
                  "viable=%u default=%u create=0x%08X",
                  index, candidate.description.AdapterLuid.HighPart,
                  candidate.description.AdapterLuid.LowPart, candidate.description.VendorId,
                  candidate.description.DeviceId, candidate.description.SubSysId,
                  candidate.description.Flags, candidate.viable,
                  haveDefault && sameLuid(candidate.description.AdapterLuid, defaultLuid),
                  static_cast<unsigned>(createResult));
        logLine(line);
        candidates.push_back(candidate);
    }

    std::vector<std::vector<bool>> matrix(candidates.size(),
                                          std::vector<bool>(candidates.size(), false));
    for (size_t from = 0; from < candidates.size(); ++from) {
        auto &source = candidates[from];
        if (!source.viable)
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
        ID3D11Texture2D *texture{};
        IDXGIResource *resource{};
        HANDLE sharedHandle{};
        if (SUCCEEDED(source.device->CreateTexture2D(&description, nullptr, &texture)) && texture &&
            SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(&resource))) &&
            SUCCEEDED(resource->GetSharedHandle(&sharedHandle))) {
            for (size_t to = 0; to < candidates.size(); ++to) {
                if (!candidates[to].viable)
                    continue;
                ID3D11Texture2D *opened{};
                const HRESULT openResult = candidates[to].device->OpenSharedResource(
                    sharedHandle, IID_PPV_ARGS(&opened));
                matrix[from][to] = SUCCEEDED(openResult) && opened;
                char line[300]{};
                sprintf_s(line,
                          "Adapter matrix: created-on=%zu opened-on=%zu related=%u "
                          "result=0x%08X success=%u",
                          from, to,
                          sameAdapterIdentity(source.description,
                                              candidates[to].description),
                          static_cast<unsigned>(openResult),
                          static_cast<unsigned>(matrix[from][to] ? 1u : 0u));
                logLine(line);
                if (opened)
                    opened->Release();
            }
        }
        if (resource)
            resource->Release();
        if (texture)
            texture->Release();
    }

    int defaultIndex = -1;
    for (size_t index = 0; index < candidates.size(); ++index) {
        if (haveDefault && sameLuid(candidates[index].description.AdapterLuid, defaultLuid)) {
            defaultIndex = static_cast<int>(index);
            break;
        }
    }
    for (size_t from = 0; from < candidates.size(); ++from) {
        auto &candidate = candidates[from];
        if (!candidate.viable)
            continue;
        candidate.selfOpen = matrix[from][from];
        for (size_t to = 0; to < candidates.size(); ++to) {
            if (candidates[to].viable &&
                sameAdapterIdentity(candidate.description, candidates[to].description)) {
                ++candidate.relatedTotal;
                if (matrix[from][to])
                    ++candidate.relatedOpen;
            }
        }
        char line[260]{};
        sprintf_s(line,
                  "Adapter score %zu: viable=%u self-open=%u related-open=%u/%u default=%u",
                  from, candidate.viable, candidate.selfOpen, candidate.relatedOpen,
                  candidate.relatedTotal, static_cast<int>(from) == defaultIndex);
        logLine(line);
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
                candidates[index].relatedOpen > candidates[choice].relatedOpen) {
                choice = static_cast<int>(index);
            }
        }
    } else {
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (candidates[index].viable && candidates[index].selfOpen &&
                (choice < 0 ||
                 candidates[index].relatedOpen > candidates[choice].relatedOpen)) {
                choice = static_cast<int>(index);
            }
        }
    }

    IDXGIAdapter1 *selected{};
    if (choice >= 0) {
        candidates[choice].adapter->AddRef();
        selected = candidates[choice].adapter;
        char line[400]{};
        sprintf_s(line,
                  "Selected adapter %d: LUID=%ld:%lu related-open=%u/%u default=%u",
                  choice, candidates[choice].description.AdapterLuid.HighPart,
                  candidates[choice].description.AdapterLuid.LowPart,
                  candidates[choice].relatedOpen, candidates[choice].relatedTotal,
                  choice == defaultIndex);
        logLine(line);
    }

    for (auto &candidate : candidates) {
        if (candidate.context)
            candidate.context->Release();
        if (candidate.device)
            candidate.device->Release();
        if (candidate.adapter)
            candidate.adapter->Release();
    }
    factory->Release();
    return selected;
}

void generateFrame(std::vector<std::uint32_t> &pixels, unsigned long long frame)
{
    // These diagnostic colours use equal red/blue channels so the same CPU frame has the same
    // appearance in the RGBA DXGI backbuffer and the BGRA-oriented GDI preview.
    std::fill(pixels.begin(), pixels.end(), 0xFF181B18u);
    const double phase = static_cast<double>(frame % 240) / 240.0 * 6.283185307;
    const int bob = static_cast<int>(std::sin(phase) * 80.0);
    const int centerX = kCaptureWidth / 2;
    const int centerY = kCaptureHeight / 2 + bob;
    const int radius = 150;
    for (int y = std::max(0, centerY - radius); y < std::min(kCaptureHeight, centerY + radius); ++y) {
        for (int x = std::max(0, centerX - radius); x < std::min(kCaptureWidth, centerX + radius); ++x) {
            const int dx = x - centerX;
            const int dy = y - centerY;
            if (dx * dx + dy * dy < radius * radius)
                pixels[static_cast<size_t>(y) * kCaptureWidth + x] = 0xFF43AF43u;
        }
    }
    const int markerX = 30 + static_cast<int>((frame * 7) % (kCaptureWidth - 120));
    for (int y = 30; y < 90; ++y)
        for (int x = markerX; x < markerX + 90; ++x)
            pixels[static_cast<size_t>(y) * kCaptureWidth + x] = 0xFFFFB3FFu;
}

struct CaptureState {
    bool requestedVisible{};
    bool effectivelyVisible{};
    bool parentMinimised{};
    const char *geometry{"unavailable"};
    LONG visibleWidth{};
    LONG visibleHeight{};
};

CaptureState captureState()
{
    CaptureState state{};
    state.requestedVisible = g_captureVisible.load();
    state.effectivelyVisible = IsWindowVisible(g_captureWindow) != FALSE;
    state.parentMinimised = IsIconic(g_mainWindow) != FALSE;
    if (state.parentMinimised) {
        state.geometry = "parent-minimised";
        return state;
    }
    if (!state.requestedVisible) {
        state.geometry = "explicitly-hidden";
        return state;
    }

    RECT parentClient{};
    RECT childInParent{};
    if (!GetClientRect(g_mainWindow, &parentClient) || !GetWindowRect(g_captureWindow, &childInParent))
        return state;
    MapWindowPoints(HWND_DESKTOP, g_mainWindow,
                    reinterpret_cast<POINT *>(&childInParent), 2);
    RECT intersection{};
    if (!IntersectRect(&intersection, &parentClient, &childInParent)) {
        state.geometry = "fully-clipped";
        return state;
    }
    state.visibleWidth = intersection.right - intersection.left;
    state.visibleHeight = intersection.bottom - intersection.top;
    if (EqualRect(&intersection, &childInParent))
        state.geometry = "fully-visible";
    else
        state.geometry = "partially-clipped";
    return state;
}

void logCaptureState(const char *reason)
{
    const CaptureState state = captureState();
    RECT parentClient{};
    GetClientRect(g_mainWindow, &parentClient);
    char line[420]{};
    sprintf_s(line,
              "Capture state (%s): geometry=%s requested-visible=%u effective-visible=%u "
              "parent-minimised=%u visible-region=%ldx%ld parent-client=%ldx%ld "
              "capture-buffer=960x720",
              reason, state.geometry, state.requestedVisible, state.effectivelyVisible,
              state.parentMinimised, state.visibleWidth, state.visibleHeight,
              parentClient.right - parentClient.left, parentClient.bottom - parentClient.top);
    logLine(line);
}

void renderThread()
{
    IDXGIAdapter1 *adapter = chooseInteroperableAdapter();
    if (!adapter) {
        logLine("No interoperable adapter was selected");
        g_lastPresent.store(E_FAIL);
        InvalidateRect(g_mainWindow, nullptr, FALSE);
        return;
    }

    DXGI_SWAP_CHAIN_DESC swapDescription{};
    swapDescription.BufferDesc.Width = kCaptureWidth;
    swapDescription.BufferDesc.Height = kCaptureHeight;
    swapDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = 1;
    swapDescription.OutputWindow = g_captureWindow;
    swapDescription.Windowed = TRUE;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *swapChain{};
    ID3D11Device *device{};
    ID3D11DeviceContext *context{};
    D3D_FEATURE_LEVEL actualLevel{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                       D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    const HRESULT createResult = D3D11CreateDeviceAndSwapChain(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &swapDescription, &swapChain, &device, &actualLevel, &context);
    adapter->Release();
    if (FAILED(createResult)) {
        char line[160]{};
        sprintf_s(line, "D3D11CreateDeviceAndSwapChain failed=0x%08X",
                  static_cast<unsigned>(createResult));
        logLine(line);
        g_lastPresent.store(createResult);
        InvalidateRect(g_mainWindow, nullptr, FALSE);
        return;
    }

    const HRESULT resizeResult = swapChain->ResizeBuffers(
        2, kCaptureWidth, kCaptureHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(resizeResult)) {
        char line[160]{};
        sprintf_s(line, "ResizeBuffers(2, 960, 720, RGBA8) failed=0x%08X",
                  static_cast<unsigned>(resizeResult));
        logLine(line);
        g_lastPresent.store(resizeResult);
        InvalidateRect(g_mainWindow, nullptr, FALSE);
        context->Release();
        device->Release();
        swapChain->Release();
        return;
    }
    logLine("ResizeBuffers(2, 960, 720, RGBA8) succeeded");

    ID3D11Texture2D *backBuffer{};
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        logLine("GetBuffer failed");
        context->Release();
        device->Release();
        swapChain->Release();
        return;
    }
    logLine("Fixed capture swap chain created: 960x720; child starts visible; one DXGI swap chain");

    std::vector<std::uint32_t> frame(kCaptureWidth * kCaptureHeight);
    ULONGLONG lastReport = GetTickCount64();
    unsigned long long number = 0;
    auto nextFrame = std::chrono::steady_clock::now();
    while (g_running.load()) {
        nextFrame += std::chrono::milliseconds(16);
        generateFrame(frame, number);
        context->UpdateSubresource(backBuffer, 0, nullptr, frame.data(), kCaptureWidth * 4, 0);
        const HRESULT presentResult = swapChain->Present(1, 0);
        g_lastPresent.store(presentResult);
        g_frameCount.store(++number);
        {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            g_previewFrame = frame;
        }
        InvalidateRect(g_mainWindow, nullptr, FALSE);

        const ULONGLONG now = GetTickCount64();
        if (now - lastReport >= 2000) {
            char line[320]{};
            const CaptureState state = captureState();
            sprintf_s(line,
                      "Render progress: frame=%llu present=0x%08X geometry=%s "
                      "requested-visible=%u effective-visible=%u parent-minimised=%u OBS-hook=%u",
                      number, static_cast<unsigned>(presentResult), state.geometry,
                      state.requestedVisible, state.effectivelyVisible, state.parentMinimised,
                      GetModuleHandleW(L"graphics-hook64.dll") != nullptr);
            logLine(line);
            lastReport = now;
        }
        const auto current = std::chrono::steady_clock::now();
        if (nextFrame > current)
            std::this_thread::sleep_until(nextFrame);
        else
            nextFrame = current;
    }
    backBuffer->Release();
    context->Release();
    device->Release();
    swapChain->Release();
    logLine("Render thread stopped");
}

RECT aspectFit(RECT bounds)
{
    const int availableWidth = std::max(1L, bounds.right - bounds.left);
    const int availableHeight = std::max(1L, bounds.bottom - bounds.top);
    int width = availableWidth;
    int height = width * kCaptureHeight / kCaptureWidth;
    if (height > availableHeight) {
        height = availableHeight;
        width = height * kCaptureWidth / kCaptureHeight;
    }
    const int x = bounds.left + (availableWidth - width) / 2;
    const int y = bounds.top + (availableHeight - height) / 2;
    return {x, y, x + width, y + height};
}

void paintMainWindow(HWND window)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 20, 20));
    RECT heading{16, 10, client.right - 16, 38};
    DrawTextW(dc, L"Architecture diagnostic: fixed 960x720 OBS surface + GDI preview", -1,
              &heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t status[400]{};
    const CaptureState state = captureState();
    const wchar_t *geometry = L"Unavailable";
    if (strcmp(state.geometry, "fully-visible") == 0)
        geometry = L"Fully visible";
    else if (strcmp(state.geometry, "partially-clipped") == 0)
        geometry = L"Partially clipped";
    else if (strcmp(state.geometry, "fully-clipped") == 0)
        geometry = L"Fully clipped";
    else if (strcmp(state.geometry, "explicitly-hidden") == 0)
        geometry = L"Explicitly hidden";
    else if (strcmp(state.geometry, "parent-minimised") == 0)
        geometry = L"Parent minimised";
    swprintf_s(status,
               L"Capture child: %s | Requested: %s | Frame: %llu | Present: 0x%08X | Parent: %ld x %ld",
               geometry, state.requestedVisible ? L"visible" : L"hidden", g_frameCount.load(),
               static_cast<unsigned>(g_lastPresent.load()), client.right - client.left,
               client.bottom - client.top);
    RECT statusRect{16, 72, client.right - 16, 98};
    DrawTextW(dc, status, -1, &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT previewBounds{16, 112, std::min<LONG>(516, client.right - 16), client.bottom - 16};
    if (previewBounds.right > previewBounds.left && previewBounds.bottom > previewBounds.top) {
        HBRUSH background = CreateSolidBrush(RGB(35, 38, 48));
        FillRect(dc, &previewBounds, background);
        DeleteObject(background);
        const RECT destination = aspectFit(previewBounds);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = kCaptureWidth;
        info.bmiHeader.biHeight = -kCaptureHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        std::lock_guard<std::mutex> lock(g_frameMutex);
        StretchDIBits(dc, destination.left, destination.top,
                      destination.right - destination.left, destination.bottom - destination.top, 0,
                      0, kCaptureWidth, kCaptureHeight, g_previewFrame.data(), &info,
                      DIB_RGB_COLORS, SRCCOPY);
        FrameRect(dc, &destination, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }
    EndPaint(window, &paint);
}

void runFilePicker(HWND owner)
{
    wchar_t filename[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"PNG images (*.png)\0*.png\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = filename;
    dialog.nMaxFile = ARRAYSIZE(filename);
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    logLine("File picker opened; render thread should continue presenting");
    const BOOL selected = GetOpenFileNameW(&dialog);
    logLine(selected ? "File picker closed with a selection" : "File picker closed or cancelled");
}

LRESULT CALLBACK captureWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK mainWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == kOpenDialogButton) {
            runFilePicker(window);
            return 0;
        }
        if (LOWORD(wParam) == kVisibilityButton) {
            const bool show = !g_captureVisible.load();
            ShowWindow(g_captureWindow, show ? SW_SHOWNA : SW_HIDE);
            g_captureVisible.store(show);
            SetWindowTextW(g_visibilityButton,
                           show ? L"Hide capture child" : L"Show capture child");
            logCaptureState(show ? "explicit show" : "explicit hide");
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        break;
    case WM_SIZE: {
        const UINT type = static_cast<UINT>(wParam);
        char line[220]{};
        sprintf_s(line, "Parent WM_SIZE: type=%u client=%ux%u; capture child remains 960x720",
                  type, LOWORD(lParam), HIWORD(lParam));
        logLine(line);
        logCaptureState(type == SIZE_MINIMIZED ? "parent minimised" : "parent resized/restored");
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_PAINT:
        paintMainWindow(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    startLog();
    SetProcessDPIAware();

    WNDCLASSEXW mainClass{sizeof(WNDCLASSEXW)};
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = mainWindowProcedure;
    mainClass.hInstance = instance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    mainClass.lpszClassName = L"RearSilverArchitectureDiagnosticMain";
    if (!RegisterClassExW(&mainClass))
        return 1;

    WNDCLASSEXW captureClass{sizeof(WNDCLASSEXW)};
    captureClass.lpfnWndProc = captureWindowProcedure;
    captureClass.hInstance = instance;
    captureClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    captureClass.lpszClassName = L"RearSilverArchitectureDiagnosticCapture";
    if (!RegisterClassExW(&captureClass))
        return 2;

    g_mainWindow = CreateWindowExW(
        0, mainClass.lpszClassName,
        L"RearSilver Avatar - Architecture Diagnostic - Fixed 960x720 Capture",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1560, 900, nullptr, nullptr, instance, nullptr);
    if (!g_mainWindow)
        return 3;

    CreateWindowExW(0, L"BUTTON", L"Open file picker test", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    16, 42, 190, 28, g_mainWindow,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpenDialogButton)), instance,
                    nullptr);
    g_visibilityButton = CreateWindowExW(
        0, L"BUTTON", L"Hide capture child", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 218, 42, 190,
        28, g_mainWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVisibilityButton)), instance,
        nullptr);

    g_captureWindow = CreateWindowExW(
        0, captureClass.lpszClassName, L"Fixed 960x720 capture child", WS_CHILD | WS_VISIBLE,
        kCaptureX, kCaptureY, kCaptureWidth, kCaptureHeight, g_mainWindow, nullptr, instance, nullptr);
    if (!g_captureWindow)
        return 4;

    ShowWindow(g_mainWindow, showCommand);
    UpdateWindow(g_mainWindow);
    std::thread renderer(renderThread);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_running.store(false);
    if (renderer.joinable())
        renderer.join();
    if (g_log != INVALID_HANDLE_VALUE)
        CloseHandle(g_log);
    return static_cast<int>(message.wParam);
}
