#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d2d1.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"RearSilverAvatarWindow";
constexpr wchar_t kWindowTitle[] = L"RearSilver Avatar";
constexpr UINT kRenderFailureMessage = WM_APP + 1;
constexpr UINT kImageUploadFailureMessage = WM_APP + 2;
constexpr UINT kOutputWidth = 1920;
constexpr UINT kOutputHeight = 1080;
constexpr UINT kOutputUiDpi = 192;

HWND g_mainWindow = nullptr;
std::atomic<bool> g_running{false};
std::atomic<bool> g_applicationActive{true};
std::atomic<bool> g_dialogOpen{false};
std::atomic<bool> g_sidebarOpen{true};
std::atomic<bool> g_motionEnabled{true};
std::atomic<int> g_backgroundMode{0};
std::atomic<UINT> g_clientWidth{960};
std::atomic<UINT> g_clientHeight{720};
std::atomic<bool> g_transformPending{true};

std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;

struct PendingImage {
    std::vector<unsigned char> rgba;
    UINT width = 0;
    UINT height = 0;
    std::wstring path;
};

std::mutex g_pendingImageMutex;
PendingImage g_pendingImage;
bool g_hasPendingImage = false;

struct RectF {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;

    bool contains(float px, float py) const
    {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

struct UiLayout {
    RectF toggle;
    RectF panel;
    RectF loadPng;
    RectF background;
    RectF motion;
    bool sidebarOpen = false;
};

UiLayout calculateUiLayout(UINT clientWidth, UINT clientHeight, UINT dpi, bool sidebarOpen)
{
    const float scale = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
    const float margin = 16.0f * scale;
    const float toggleSize = 48.0f * scale;
    const float gap = 10.0f * scale;
    const float buttonHeight = 46.0f * scale;
    const float availableWidth = std::max(1.0f, static_cast<float>(clientWidth) - margin * 2.0f);
    const float panelWidth = std::min(300.0f * scale, std::max(220.0f * scale, availableWidth * 0.30f));

    UiLayout result;
    result.sidebarOpen = sidebarOpen;
    result.toggle = {margin, margin, toggleSize, toggleSize};
    if (!sidebarOpen)
        return result;

    const float panelTop = margin + toggleSize + gap;
    const float wantedHeight = gap * 4.0f + buttonHeight * 3.0f + 60.0f * scale;
    const float panelHeight = std::min(wantedHeight, std::max(1.0f, static_cast<float>(clientHeight) - panelTop - margin));
    result.panel = {margin, panelTop, panelWidth, panelHeight};
    const float buttonX = result.panel.x + gap;
    const float buttonWidth = std::max(1.0f, result.panel.width - gap * 2.0f);
    float buttonY = result.panel.y + 48.0f * scale;
    result.loadPng = {buttonX, buttonY, buttonWidth, buttonHeight};
    buttonY += buttonHeight + gap;
    result.background = {buttonX, buttonY, buttonWidth, buttonHeight};
    buttonY += buttonHeight + gap;
    result.motion = {buttonX, buttonY, buttonWidth, buttonHeight};
    return result;
}

void logMessage(const std::wstring &message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"%02u:%02u:%02u.%03u | ", time.wHour, time.wMinute, time.wSecond,
               time.wMilliseconds);
    const std::wstring line = std::wstring(stamp) + message + L"\r\n";
    OutputDebugStringW(line.c_str());
    if (g_logFile != INVALID_HANDLE_VALUE) {
        const int count = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                              nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), count,
                            nullptr, nullptr);
        DWORD written = 0;
        WriteFile(g_logFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

void startLog()
{
    wchar_t tempPath[MAX_PATH + 1]{};
    if (GetTempPathW(MAX_PATH, tempPath) == 0)
        return;
    const std::wstring path = std::wstring(tempPath) + L"RearSilverAvatar-baseline.log";
    g_logFile = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    logMessage(L"RearSilver Avatar first reconstructed baseline starting");
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
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                candidate.device.GetAddressOf(), &feature, candidate.context.GetAddressOf()));
        }
        logMessage(L"Adapter candidate " + std::to_wstring(index) + L": " +
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
        ComPtr<ID3D11Texture2D> sharedTexture;
        ComPtr<IDXGIResource> sharedResource;
        HANDLE sharedHandle = nullptr;
        if (SUCCEEDED(candidates[from].device->CreateTexture2D(&description, nullptr, &sharedTexture)) &&
            SUCCEEDED(sharedTexture.As(&sharedResource)) &&
            SUCCEEDED(sharedResource->GetSharedHandle(&sharedHandle))) {
            for (size_t to = 0; to < candidates.size(); ++to) {
                if (!candidates[to].viable)
                    continue;
                ComPtr<ID3D11Texture2D> opened;
                matrix[from][to] = SUCCEEDED(candidates[to].device->OpenSharedResource(
                    sharedHandle, IID_PPV_ARGS(&opened)));
                logMessage(L"Adapter matrix " + std::to_wstring(from) + L" -> " +
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
                sameAdapterIdentity(candidates[index].description, candidates[choice].description) &&
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

    logMessage(L"Selected adapter candidate " + std::to_wstring(choice) + L"; LUID=" +
               std::to_wstring(candidates[choice].description.AdapterLuid.HighPart) + L":" +
               std::to_wstring(candidates[choice].description.AdapterLuid.LowPart) +
               L"; related-open=" + std::to_wstring(candidates[choice].relatedOpen) + L"/" +
               std::to_wstring(candidates[choice].relatedTotal));
    return candidates[choice].adapter;
}

bool decodePng(const wchar_t *path, PendingImage &decoded)
{
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame));
    UINT width = 0;
    UINT height = 0;
    check(frame->GetSize(&width, &height));
    if (width == 0 || height == 0 || width > 8192 || height > 8192)
        return false;

    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom));
    decoded.rgba.resize(static_cast<size_t>(width) * height * 4);
    check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(decoded.rgba.size()),
                                decoded.rgba.data()));
    decoded.width = width;
    decoded.height = height;
    decoded.path = path;
    return true;
}

void openPngPicker()
{
    wchar_t path[32768]{};
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = g_mainWindow;
    picker.lpstrFilter = L"PNG images\0*.png\0All files\0*.*\0";
    picker.lpstrFile = path;
    picker.nMaxFile = static_cast<DWORD>(std::size(path));
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    g_dialogOpen.store(true);
    const BOOL selected = GetOpenFileNameW(&picker);
    if (selected) {
        try {
            PendingImage decoded;
            if (!decodePng(path, decoded))
                throw E_INVALIDARG;
            {
                std::lock_guard<std::mutex> lock(g_pendingImageMutex);
                g_pendingImage = std::move(decoded);
                g_hasPendingImage = true;
            }
            logMessage(L"PNG decoded and queued for render-thread upload: " + std::wstring(path));
        } catch (...) {
            MessageBoxW(g_mainWindow,
                        L"Could not decode this image. Choose a valid PNG no larger than 8192 × 8192 pixels. The current avatar is unchanged.",
                        L"RearSilver Avatar — PNG loading", MB_OK | MB_ICONERROR);
        }
    }
    g_dialogOpen.store(false);
}

bool isProcessRunning(const wchar_t *name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

struct Vertex {
    float x;
    float y;
    float u;
    float v;
};

struct TextureAsset {
    ComPtr<ID3D11ShaderResourceView> view;
    UINT width = 1;
    UINT height = 1;
};

class Renderer {
public:
    void initialize(HWND window)
    {
        window_ = window;
        width_ = kOutputWidth;
        height_ = kOutputHeight;

        ComPtr<IDXGIAdapter1> selectedAdapter = chooseInteroperableAdapter();
        const D3D_FEATURE_LEVEL requestedLevels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL createdLevel{};
        check(D3D11CreateDevice(
            selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, requestedLevels,
            static_cast<UINT>(std::size(requestedLevels)), D3D11_SDK_VERSION,
            device_.GetAddressOf(), &createdLevel, context_.GetAddressOf()));

        ComPtr<IDXGIFactory2> factory;
        check(selectedAdapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width_;
        description.Height = height_;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        check(factory->CreateSwapChainForComposition(device_.Get(), &description, nullptr,
                                                      swapChain_.GetAddressOf()));
        createRenderTarget();

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

        createPipeline();
        createAssets();

        ComPtr<IDXGIDevice> activeDxgiDevice;
        ComPtr<IDXGIAdapter> activeAdapter;
        DXGI_ADAPTER_DESC activeDescription{};
        if (SUCCEEDED(device_.As(&activeDxgiDevice)) &&
            SUCCEEDED(activeDxgiDevice->GetAdapter(activeAdapter.GetAddressOf())) &&
            SUCCEEDED(activeAdapter->GetDesc(&activeDescription))) {
            logMessage(L"Active GPU: " + std::wstring(activeDescription.Description) + L"; LUID=" +
                       std::to_wstring(activeDescription.AdapterLuid.HighPart) + L":" +
                       std::to_wstring(activeDescription.AdapterLuid.LowPart));
        }
        logMessage(L"Fixed 1920 x 1080 composition swap chain attached through DirectComposition");
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
    }

    void uploadPendingImage()
    {
        PendingImage pending;
        {
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            if (!g_hasPendingImage)
                return;
            pending = std::move(g_pendingImage);
            g_pendingImage = {};
            g_hasPendingImage = false;
        }

        try {
            TextureAsset replacement = createTexture(pending.rgba.data(), pending.width, pending.height);
            avatar_ = std::move(replacement);
            logMessage(L"Avatar atomically replaced after GPU upload: " + pending.path);
        } catch (...) {
            PostMessageW(window_, kImageUploadFailureMessage, 0, 0);
        }
    }

    void render()
    {
        uploadPendingImage();

        float clear[4] = {0, 0, 0, 0};
        const int background = g_backgroundMode.load();
        if (background == 1) {
            clear[1] = 1.0f;
            clear[3] = 1.0f;
        } else if (background == 2) {
            clear[0] = 0.035f;
            clear[1] = 0.045f;
            clear[2] = 0.070f;
            clear[3] = 1.0f;
        }
        context_->ClearRenderTargetView(target_.Get(), clear);
        ID3D11RenderTargetView *target = target_.Get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
        context_->RSSetViewports(1, &viewport);
        bindPipeline();

        const float maxWidth = static_cast<float>(width_) * 0.68f;
        const float maxHeight = static_cast<float>(height_) * 0.68f;
        const float scale = std::min(maxWidth / avatar_.width, maxHeight / avatar_.height);
        const float avatarWidth = avatar_.width * scale;
        const float avatarHeight = avatar_.height * scale;
        const float bob = g_motionEnabled.load()
                              ? std::sin(static_cast<float>(GetTickCount64()) / 500.0f) *
                                    std::max(3.0f, static_cast<float>(height_) * 0.015f)
                              : 0.0f;
        draw(avatar_, (static_cast<float>(width_) - avatarWidth) * 0.5f,
             (static_cast<float>(height_) - avatarHeight) * 0.5f + bob, avatarWidth,
             avatarHeight);

        const bool overlayVisible = g_applicationActive.load() && !g_dialogOpen.load();
        if (overlayVisible)
            drawOverlay();

        ID3D11ShaderResourceView *empty = nullptr;
        context_->PSSetShaderResources(0, 1, &empty);
        check(swapChain_->Present(1, 0));
    }

private:
    TextureAsset createTexture(const void *pixels, UINT width, UINT height)
    {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixels, width * 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        check(device_->CreateTexture2D(&description, &initial, texture.GetAddressOf()));
        TextureAsset asset;
        check(device_->CreateShaderResourceView(texture.Get(), nullptr, asset.view.GetAddressOf()));
        asset.width = width;
        asset.height = height;
        return asset;
    }

    TextureAsset createSolid(unsigned char red, unsigned char green, unsigned char blue,
                             unsigned char alpha)
    {
        const unsigned char rgba[] = {red, green, blue, alpha};
        return createTexture(rgba, 1, 1);
    }

    TextureAsset createText(const wchar_t *text, UINT pixelHeight, COLORREF color)
    {
        constexpr int canvasWidth = 512;
        constexpr int canvasHeight = 80;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = canvasWidth;
        info.bmiHeader.biHeight = -canvasHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *pixels = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dc || !bitmap) {
            if (bitmap)
                DeleteObject(bitmap);
            if (dc)
                DeleteDC(dc);
            throw E_OUTOFMEMORY;
        }
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        HFONT font = CreateFontW(-static_cast<int>(pixelHeight), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkColor(dc, RGB(0, 0, 0));
        SetTextColor(dc, color);
        RECT measure{0, 0, canvasWidth, canvasHeight};
        DrawTextW(dc, text, -1, &measure, DT_CALCRECT | DT_SINGLELINE);
        const int textWidth = std::clamp(measure.right, 1L, static_cast<LONG>(canvasWidth));
        const int textHeight = std::clamp(measure.bottom, 1L, static_cast<LONG>(canvasHeight));
        RECT drawRect{0, 0, textWidth, textHeight};
        DrawTextW(dc, text, -1, &drawRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
        GdiFlush();

        std::vector<unsigned char> rgba(static_cast<size_t>(textWidth) * textHeight * 4);
        const auto *bgra = static_cast<const unsigned char *>(pixels);
        const unsigned char targetRed = GetRValue(color);
        const unsigned char targetGreen = GetGValue(color);
        const unsigned char targetBlue = GetBValue(color);
        for (int y = 0; y < textHeight; ++y) {
            for (int x = 0; x < textWidth; ++x) {
                const size_t source = static_cast<size_t>(y * canvasWidth + x) * 4;
                const size_t destination = static_cast<size_t>(y * textWidth + x) * 4;
                const unsigned char coverage = std::max({bgra[source], bgra[source + 1], bgra[source + 2]});
                rgba[destination] = targetRed;
                rgba[destination + 1] = targetGreen;
                rgba[destination + 2] = targetBlue;
                rgba[destination + 3] = coverage;
            }
        }
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(font);
        DeleteObject(bitmap);
        DeleteDC(dc);
        return createTexture(rgba.data(), static_cast<UINT>(textWidth), static_cast<UINT>(textHeight));
    }

    void createRenderTarget()
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
        check(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, target_.GetAddressOf()));
    }

    void createPipeline()
    {
        static constexpr char shader[] =
            "struct V{float2 p:POSITION;float2 uv:TEXCOORD0;};"
            "struct P{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};"
            "P vs(V i){P o;o.p=float4(i.p,0,1);o.uv=i.uv;return o;}"
            "Texture2D img:register(t0);SamplerState smp:register(s0);"
            "float4 ps(P i):SV_TARGET{return img.Sample(smp,i.uv);}";
        ComPtr<ID3DBlob> vertexBlob;
        ComPtr<ID3DBlob> pixelBlob;
        ComPtr<ID3DBlob> errors;
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0,
                         vertexBlob.GetAddressOf(), errors.GetAddressOf()));
        errors.Reset();
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0,
                         pixelBlob.GetAddressOf(), errors.GetAddressOf()));
        check(device_->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                          nullptr, vertexShader_.GetAddressOf()));
        check(device_->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(),
                                         nullptr, pixelShader_.GetAddressOf()));
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        check(device_->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)),
                                         vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                         inputLayout_.GetAddressOf()));

        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Vertex) * 6;
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(device_->CreateBuffer(&buffer, nullptr, vertexBuffer_.GetAddressOf()));

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device_->CreateSamplerState(&sampler, sampler_.GetAddressOf()));

        D3D11_BLEND_DESC blend{};
        auto &target = blend.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check(device_->CreateBlendState(&blend, blend_.GetAddressOf()));
    }

    void createAssets()
    {
        constexpr UINT size = 512;
        std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4, 0);
        auto paint = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b,
                         unsigned char a) {
            if (x < 0 || y < 0 || x >= static_cast<int>(size) || y >= static_cast<int>(size))
                return;
            const size_t index = static_cast<size_t>(y * size + x) * 4;
            pixels[index] = r;
            pixels[index + 1] = g;
            pixels[index + 2] = b;
            pixels[index + 3] = a;
        };
        for (int y = 0; y < static_cast<int>(size); ++y) {
            for (int x = 0; x < static_cast<int>(size); ++x) {
                const float dx = (x - 256.0f) / 180.0f;
                const float dy = (y - 285.0f) / 190.0f;
                const bool head = dx * dx + dy * dy <= 1.0f;
                const bool leftEar = y < 190 && x > 105 && x < 245 && y > 0.82f * x - 55;
                const bool rightEar = y < 190 && x > 267 && x < 407 && y > -0.82f * x + 365;
                if (head || leftEar || rightEar)
                    paint(x, y, 89, 181, 226, 255);
            }
        }
        avatar_ = createTexture(pixels.data(), size, size);
        white_ = createSolid(255, 255, 255, 255);
        panel_ = createSolid(17, 23, 38, 235);
        button_ = createSolid(40, 55, 82, 245);
        accent_ = createSolid(45, 205, 188, 255);
        titleText_ = createText(L"REARSILVER AVATAR", 28, RGB(244, 247, 255));
        loadText_ = createText(L"SELECT PNG", 24, RGB(244, 247, 255));
        backgroundText_ = createText(L"BACKGROUND", 24, RGB(244, 247, 255));
        motionText_ = createText(L"MOTION", 24, RGB(244, 247, 255));
    }

    void bindPipeline()
    {
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer *vertexBuffer = vertexBuffer_.Get();
        context_->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11SamplerState *sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        context_->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    }

    void draw(const TextureAsset &texture, float x, float y, float width, float height)
    {
        if (!texture.view || width <= 0 || height <= 0)
            return;
        const float left = 2.0f * x / width_ - 1.0f;
        const float right = 2.0f * (x + width) / width_ - 1.0f;
        const float top = 1.0f - 2.0f * y / height_;
        const float bottom = 1.0f - 2.0f * (y + height) / height_;
        const Vertex vertices[] = {
            {left, top, 0, 0}, {right, top, 1, 0}, {left, bottom, 0, 1},
            {left, bottom, 0, 1}, {right, top, 1, 0}, {right, bottom, 1, 1}};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context_->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, vertices, sizeof(vertices));
        context_->Unmap(vertexBuffer_.Get(), 0);
        ID3D11ShaderResourceView *view = texture.view.Get();
        context_->PSSetShaderResources(0, 1, &view);
        context_->Draw(6, 0);
    }

    void drawRect(const TextureAsset &texture, const RectF &rect)
    {
        draw(texture, rect.x, rect.y, rect.width, rect.height);
    }

    void drawLabel(const TextureAsset &label, const RectF &bounds, float maxHeight)
    {
        const float scale = std::min((bounds.width - 24.0f) / label.width,
                                     maxHeight / label.height);
        const float width = label.width * scale;
        const float height = label.height * scale;
        draw(label, bounds.x + (bounds.width - width) * 0.5f,
             bounds.y + (bounds.height - height) * 0.5f, width, height);
    }

    void drawOverlay()
    {
        const UiLayout ui = calculateUiLayout(width_, height_, kOutputUiDpi,
                                              g_sidebarOpen.load());
        drawRect(panel_, ui.toggle);
        const float inset = ui.toggle.width * 0.25f;
        const float lineHeight = std::max(2.0f, ui.toggle.height * 0.055f);
        for (int line = 0; line < 3; ++line) {
            RectF bar{ui.toggle.x + inset,
                      ui.toggle.y + inset + line * ui.toggle.height * 0.18f,
                      ui.toggle.width - inset * 2.0f, lineHeight};
            drawRect(accent_, bar);
        }
        if (!ui.sidebarOpen)
            return;

        drawRect(panel_, ui.panel);
        RectF titleBounds{ui.panel.x + 10.0f, ui.panel.y + 8.0f,
                          ui.panel.width - 20.0f, 32.0f};
        drawLabel(titleText_, titleBounds, 24.0f);
        drawRect(button_, ui.loadPng);
        drawRect(button_, ui.background);
        drawRect(button_, ui.motion);
        drawLabel(loadText_, ui.loadPng, ui.loadPng.height * 0.42f);
        drawLabel(backgroundText_, ui.background, ui.background.height * 0.42f);
        drawLabel(motionText_, ui.motion, ui.motion.height * 0.42f);

        const float marker = 10.0f;
        const RectF backgroundMarker{ui.background.x, ui.background.y, marker, ui.background.height};
        drawRect(g_backgroundMode.load() == 1 ? white_ : accent_, backgroundMarker);
        if (g_motionEnabled.load()) {
            const RectF motionMarker{ui.motion.x, ui.motion.y, marker, ui.motion.height};
            drawRect(accent_, motionMarker);
        }
    }

    HWND window_ = nullptr;
    UINT width_ = 1;
    UINT height_ = 1;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> compositionVisual_;
    ComPtr<IDCompositionMatrixTransform> compositionTransform_;
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
    TextureAsset avatar_;
    TextureAsset white_;
    TextureAsset panel_;
    TextureAsset button_;
    TextureAsset accent_;
    TextureAsset titleText_;
    TextureAsset loadText_;
    TextureAsset backgroundText_;
    TextureAsset motionText_;
};

void renderThreadMain()
{
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        Renderer renderer;
        renderer.initialize(g_mainWindow);
        while (g_running.load()) {
            if (g_transformPending.exchange(false)) {
                const UINT latestWidth = g_clientWidth.load();
                const UINT latestHeight = g_clientHeight.load();
                if (latestWidth > 0 && latestHeight > 0)
                    renderer.updateLocalTransform(latestWidth, latestHeight);
            }
            renderer.render();
        }
    } catch (HRESULT error) {
        logMessage(L"Render thread failed with HRESULT 0x" + std::to_wstring(static_cast<unsigned>(error)));
        PostMessageW(g_mainWindow, kRenderFailureMessage, static_cast<WPARAM>(error), 0);
    } catch (...) {
        PostMessageW(g_mainWindow, kRenderFailureMessage, static_cast<WPARAM>(E_FAIL), 0);
    }
    if (SUCCEEDED(com))
        CoUninitialize();
}

void handlePointerRelease(HWND window, float x, float y)
{
    if (!g_applicationActive.load() || g_dialogOpen.load())
        return;
    RECT client{};
    GetClientRect(window, &client);
    const float clientWidth = static_cast<float>(client.right);
    const float clientHeight = static_cast<float>(client.bottom);
    if (clientWidth <= 0.0f || clientHeight <= 0.0f)
        return;
    const float scale = std::min(clientWidth / kOutputWidth, clientHeight / kOutputHeight);
    const float offsetX = (clientWidth - kOutputWidth * scale) * 0.5f;
    const float offsetY = (clientHeight - kOutputHeight * scale) * 0.5f;
    const float outputX = (x - offsetX) / scale;
    const float outputY = (y - offsetY) / scale;
    if (outputX < 0.0f || outputY < 0.0f || outputX >= kOutputWidth || outputY >= kOutputHeight)
        return;
    const UiLayout ui = calculateUiLayout(kOutputWidth, kOutputHeight, kOutputUiDpi,
                                          g_sidebarOpen.load());
    if (ui.toggle.contains(outputX, outputY)) {
        g_sidebarOpen.store(!g_sidebarOpen.load());
        return;
    }
    if (!ui.sidebarOpen)
        return;
    if (ui.loadPng.contains(outputX, outputY)) {
        openPngPicker();
    } else if (ui.background.contains(outputX, outputY)) {
        g_backgroundMode.store((g_backgroundMode.load() + 1) % 3);
    } else if (ui.motion.contains(outputX, outputY)) {
        g_motionEnabled.store(!g_motionEnabled.load());
    }
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_ACTIVATEAPP:
        g_applicationActive.store(wParam != 0);
        return 0;
    case WM_DPICHANGED: {
        const RECT *suggested = reinterpret_cast<const RECT *>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_SIZE: {
        const UINT width = LOWORD(lParam);
        const UINT height = HIWORD(lParam);
        g_clientWidth.store(width);
        g_clientHeight.store(height);
        if (width > 0 && height > 0) {
            g_transformPending.store(true);
            RedrawWindow(window, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        handlePointerRelease(window, static_cast<float>(GET_X_LPARAM(lParam)),
                             static_cast<float>(GET_Y_LPARAM(lParam)));
        return 0;
    case WM_KEYDOWN:
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000))
            openPngPicker();
        return 0;
    case kImageUploadFailureMessage:
        MessageBoxW(window,
                    L"The PNG decoded successfully, but its GPU texture could not be created. The current avatar is unchanged.",
                    L"RearSilver Avatar — PNG loading", MB_OK | MB_ICONERROR);
        return 0;
    case kRenderFailureMessage: {
        g_running.store(false);
        wchar_t messageText[256]{};
        swprintf_s(messageText,
                   L"RearSilver Avatar encountered a graphics error (0x%08X) and must close.",
                   static_cast<unsigned>(wParam));
        MessageBoxW(window, messageText, L"RearSilver Avatar", MB_OK | MB_ICONERROR);
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
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    if (!RegisterClassExW(&windowClass)) {
        CoUninitialize();
        return 2;
    }

    g_mainWindow = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 980, 780, nullptr, nullptr,
                                   instance, nullptr);
    if (!g_mainWindow) {
        CoUninitialize();
        return 3;
    }
    RECT client{};
    GetClientRect(g_mainWindow, &client);
    g_clientWidth.store(std::max<LONG>(1, client.right));
    g_clientHeight.store(std::max<LONG>(1, client.bottom));

    ShowWindow(g_mainWindow, showCommand);
    UpdateWindow(g_mainWindow);
    g_running.store(true);
    std::thread renderThread(renderThreadMain);

    if (isProcessRunning(L"RTSS.exe")) {
        g_dialogOpen.store(true);
        MessageBoxW(g_mainWindow,
                    L"RivaTuner Statistics Server is running. If OBS Game Capture is blank or frozen, open RTSS Setup and enable ‘Use Microsoft Detours API hooking’.",
                    L"RearSilver Avatar — OBS compatibility", MB_OK | MB_ICONINFORMATION);
        g_dialogOpen.store(false);
    }

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
