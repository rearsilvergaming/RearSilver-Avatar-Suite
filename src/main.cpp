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

#include "settings_window.h"
#include "audio_monitor.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"RearSilverAvatarWindow";
constexpr wchar_t kWindowTitle[] = L"RearSilver Avatar";
constexpr UINT kRenderFailureMessage = WM_APP + 1;
constexpr UINT kImageUploadFailureMessage = WM_APP + 2;
constexpr UINT kImageUploadSuccessMessage = WM_APP + 3;
constexpr UINT kBlinkStateMessage = WM_APP + 4;
constexpr UINT kOutputWidth = 1920;
constexpr UINT kOutputHeight = 1080;
constexpr UINT kOutputUiDpi = 192;

HWND g_mainWindow = nullptr;
std::atomic<bool> g_running{false};
std::atomic<bool> g_applicationActive{true};
std::atomic<bool> g_dialogOpen{false};
std::atomic<bool> g_motionEnabled{true};
std::atomic<int> g_backgroundMode{0};
std::atomic<UINT> g_clientWidth{960};
std::atomic<UINT> g_clientHeight{720};
std::atomic<bool> g_transformPending{true};

std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;

enum class ImageSlot : WPARAM {
    Primary = 0,
    Reaction = 1,
    PrimaryBlink = 2,
    ReactionBlink = 3,
};

struct PendingImage {
    std::vector<unsigned char> rgba;
    UINT width = 0;
    UINT height = 0;
    std::wstring path;
    ImageSlot slot = ImageSlot::Primary;
};

std::mutex g_pendingImageMutex;
std::deque<PendingImage> g_pendingImages;

std::mutex g_primaryImageStateMutex;
std::wstring g_primaryImagePath;
bool g_primaryImageLoaded = false;
std::wstring g_reactionImagePath;
bool g_reactionImageLoaded = false;
std::wstring g_primaryBlinkImagePath;
bool g_primaryBlinkImageLoaded = false;
std::wstring g_reactionBlinkImagePath;
bool g_reactionBlinkImageLoaded = false;
std::atomic<bool> g_previewReaction{false};
std::atomic<bool> g_microphoneReaction{false};
std::atomic<bool> g_reactionAvailable{false};
std::atomic<unsigned> g_reactionThreshold{180};
std::atomic<unsigned> g_releaseDelayMs{250};
ULONGLONG g_lastAboveThreshold = 0;
std::unique_ptr<AudioInputMonitor> g_audioMonitor;
std::wstring g_selectedMicrophoneId;
std::atomic<int> g_audioMonitorStatus{0};
std::atomic<unsigned> g_audioMonitorLevel{0};
std::atomic<bool> g_blinkEnabled{true};
std::atomic<unsigned> g_blinkMinimumMs{3000};
std::atomic<unsigned> g_blinkMaximumMs{6000};
std::atomic<unsigned> g_blinkDurationMs{150};

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
    RectF avatar;
    RectF settings;
};

UiLayout calculateUiLayout(UINT, UINT, UINT dpi)
{
    const float scale = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
    const float margin = 16.0f * scale;
    const float gap = 8.0f * scale;
    const float iconSize = 44.0f * scale;

    UiLayout result;
    result.avatar = {margin, margin, iconSize, iconSize};
    result.settings = {margin, margin + iconSize + gap, iconSize, iconSize};
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

std::wstring settingsFilePath()
{
    wchar_t localAppData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0)
        return {};
    const std::wstring directory = std::wstring(localAppData) + L"\\RearSilver Avatar";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\settings.ini";
}

void ensureUnicodeSettingsFile(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
        const wchar_t bom = 0xfeff;
        DWORD written = 0;
        WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    }
    CloseHandle(file);
}

std::wstring loadSetting(const wchar_t *key)
{
    const std::wstring path = settingsFilePath();
    if (path.empty())
        return {};
    wchar_t value[32768]{};
    GetPrivateProfileStringW(L"Avatar", key, L"", value,
                             static_cast<DWORD>(std::size(value)), path.c_str());
    return value;
}

void saveSetting(const wchar_t *key, const std::wstring &value)
{
    const std::wstring path = settingsFilePath();
    if (path.empty())
        return;
    ensureUnicodeSettingsFile(path);
    WritePrivateProfileStringW(L"Avatar", key, value.c_str(), path.c_str());
}

std::wstring fileNameFromPath(const std::wstring &path)
{
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

void sendPrimaryImageState()
{
    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
    if (g_primaryImagePath.empty()) {
        postAvatarSettingsMessage(L"avatar-image-default");
    } else if (g_primaryImageLoaded) {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Primary), g_primaryImagePath);
        postAvatarSettingsMessage(L"avatar-image-current\t" + fileNameFromPath(g_primaryImagePath));
    } else {
        postAvatarSettingsMessage(L"avatar-image-unavailable\t" + fileNameFromPath(g_primaryImagePath));
    }
}

void sendReactionImageState()
{
    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
    if (g_reactionImagePath.empty()) {
        postAvatarSettingsMessage(L"reaction-image-empty");
    } else if (g_reactionImageLoaded) {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Reaction), g_reactionImagePath);
        postAvatarSettingsMessage(L"reaction-image-current\t" + fileNameFromPath(g_reactionImagePath));
    } else {
        postAvatarSettingsMessage(L"reaction-image-unavailable\t" + fileNameFromPath(g_reactionImagePath));
    }
}

void sendBlinkImageState(ImageSlot slot)
{
    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
    const bool reaction = slot == ImageSlot::ReactionBlink;
    const std::wstring &path = reaction ? g_reactionBlinkImagePath : g_primaryBlinkImagePath;
    const bool loaded = reaction ? g_reactionBlinkImageLoaded : g_primaryBlinkImageLoaded;
    const wchar_t *prefix = reaction ? L"reaction-blink-image" : L"primary-blink-image";
    if (path.empty()) {
        postAvatarSettingsMessage(std::wstring(prefix) + L"-empty");
    } else if (loaded) {
        setAvatarSettingsPreviewImage(static_cast<unsigned>(slot), path);
        postAvatarSettingsMessage(std::wstring(prefix) + L"-current\t" + fileNameFromPath(path));
    } else {
        postAvatarSettingsMessage(std::wstring(prefix) + L"-unavailable\t" + fileNameFromPath(path));
    }
}

const wchar_t *imageMessagePrefix(ImageSlot slot)
{
    switch (slot) {
    case ImageSlot::Reaction: return L"reaction-image";
    case ImageSlot::PrimaryBlink: return L"primary-blink-image";
    case ImageSlot::ReactionBlink: return L"reaction-blink-image";
    default: return L"avatar-image";
    }
}

void sendBlinkSettings()
{
    postAvatarSettingsMessage(g_blinkEnabled.load() ? L"blink-enabled\t1" : L"blink-enabled\t0");
    postAvatarSettingsMessage(L"blink-minimum\t" + std::to_wstring(g_blinkMinimumMs.load()));
    postAvatarSettingsMessage(L"blink-maximum\t" + std::to_wstring(g_blinkMaximumMs.load()));
    postAvatarSettingsMessage(L"blink-duration\t" + std::to_wstring(g_blinkDurationMs.load()));
}

void sendMicrophoneState()
{
    const std::vector<AudioInputDevice> devices = enumerateAudioInputDevices();
    std::wstring message = L"microphone-devices\t" + g_selectedMicrophoneId;
    for (const AudioInputDevice &device : devices)
        message += L"\n" + device.id + L"\t" + device.name;
    postAvatarSettingsMessage(message);
    const int status = g_audioMonitorStatus.load();
    postAvatarSettingsMessage(status == 1 ? L"microphone-status\tListening"
                                          : status == 2 ? L"microphone-status\tInput unavailable"
                                                        : L"microphone-status\tConnecting…");
    postAvatarSettingsMessage(L"microphone-level\t" +
                              std::to_wstring(g_audioMonitorLevel.load()));
    postAvatarSettingsMessage(L"reaction-threshold\t" +
                              std::to_wstring(g_reactionThreshold.load()));
    postAvatarSettingsMessage(L"release-delay\t" +
                              std::to_wstring(g_releaseDelayMs.load()));
    postAvatarSettingsMessage(g_microphoneReaction.load() ? L"microphone-reaction-on"
                                                           : L"microphone-reaction-off");
}

void setMicrophoneReaction(bool active)
{
    active = active && g_reactionAvailable.load();
    if (g_microphoneReaction.exchange(active) != active)
        postAvatarSettingsMessage(active ? L"microphone-reaction-on"
                                         : L"microphone-reaction-off");
}

void processMicrophoneLevel(unsigned level)
{
    const unsigned threshold = g_reactionThreshold.load();
    const unsigned hysteresis = 30;
    const ULONGLONG now = GetTickCount64();
    if (level >= threshold) {
        g_lastAboveThreshold = now;
        setMicrophoneReaction(true);
    } else if (g_microphoneReaction.load() && level + hysteresis < threshold &&
               now - g_lastAboveThreshold >= g_releaseDelayMs.load()) {
        setMicrophoneReaction(false);
    }
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

void openPngPicker(HWND owner = nullptr, ImageSlot slot = ImageSlot::Primary)
{
    wchar_t path[32768]{};
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = owner && IsWindow(owner) ? owner : g_mainWindow;
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
            decoded.slot = slot;
            {
                std::lock_guard<std::mutex> lock(g_pendingImageMutex);
                g_pendingImages.push_back(std::move(decoded));
            }
            logMessage(L"PNG decoded and queued for render-thread upload: " + std::wstring(path));
            const wchar_t *fileName = wcsrchr(path, L'\\');
            postAvatarSettingsMessage(std::wstring(imageMessagePrefix(slot)) + L"-selected\t" +
                                      (fileName ? fileName + 1 : path));
        } catch (...) {
            postAvatarSettingsMessage(std::wstring(imageMessagePrefix(slot)) + L"-error");
            MessageBoxW(picker.hwndOwner,
                        L"Could not decode this image. Choose a valid PNG no larger than 8192 × 8192 pixels. The current avatar is unchanged.",
                        L"RearSilver Avatar — PNG loading", MB_OK | MB_ICONERROR);
        }
    } else if (CommDlgExtendedError() == 0) {
        postAvatarSettingsMessage(std::wstring(imageMessagePrefix(slot)) + L"-cancelled");
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
            if (g_pendingImages.empty())
                return;
            pending = std::move(g_pendingImages.front());
            g_pendingImages.pop_front();
        }

        try {
            TextureAsset replacement = createTexture(pending.rgba.data(), pending.width, pending.height);
            if (pending.slot == ImageSlot::Reaction) {
                reactionAvatar_ = std::move(replacement);
                reactionAvatarLoaded_ = true;
                g_reactionAvailable.store(true);
                {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_reactionImagePath = pending.path;
                    g_reactionImageLoaded = true;
                }
                saveSetting(L"ReactionImage", pending.path);
            } else if (pending.slot == ImageSlot::PrimaryBlink) {
                primaryBlinkAvatar_ = std::move(replacement);
                primaryBlinkAvatarLoaded_ = true;
                {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_primaryBlinkImagePath = pending.path;
                    g_primaryBlinkImageLoaded = true;
                }
                saveSetting(L"PrimaryBlinkImage", pending.path);
            } else if (pending.slot == ImageSlot::ReactionBlink) {
                reactionBlinkAvatar_ = std::move(replacement);
                reactionBlinkAvatarLoaded_ = true;
                {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_reactionBlinkImagePath = pending.path;
                    g_reactionBlinkImageLoaded = true;
                }
                saveSetting(L"ReactionBlinkImage", pending.path);
            } else {
                primaryAvatar_ = std::move(replacement);
                {
                    std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
                    g_primaryImagePath = pending.path;
                    g_primaryImageLoaded = true;
                }
                saveSetting(L"PrimaryImage", pending.path);
            }
            logMessage(L"Avatar atomically replaced after GPU upload: " + pending.path);
            PostMessageW(window_, kImageUploadSuccessMessage, static_cast<WPARAM>(pending.slot), 0);
        } catch (...) {
            PostMessageW(window_, kImageUploadFailureMessage, static_cast<WPARAM>(pending.slot), 0);
        }
    }

    unsigned nextBlinkDelay()
    {
        blinkRandomState_ = blinkRandomState_ * 1664525u + 1013904223u;
        const unsigned minimum = g_blinkMinimumMs.load();
        const unsigned maximum = std::max(minimum, g_blinkMaximumMs.load());
        return minimum + (maximum > minimum ? blinkRandomState_ % (maximum - minimum + 1) : 0);
    }

    void updateBlinkState(ULONGLONG now)
    {
        if (!g_blinkEnabled.load()) {
            if (blinking_) {
                blinking_ = false;
                PostMessageW(window_, kBlinkStateMessage, FALSE, 0);
            }
            nextBlinkAt_ = 0;
            return;
        }
        if (nextBlinkAt_ == 0)
            nextBlinkAt_ = now + nextBlinkDelay();
        if (!blinking_ && now >= nextBlinkAt_) {
            blinking_ = true;
            blinkEndsAt_ = now + g_blinkDurationMs.load();
            PostMessageW(window_, kBlinkStateMessage, TRUE, 0);
        } else if (blinking_ && now >= blinkEndsAt_) {
            blinking_ = false;
            nextBlinkAt_ = now + nextBlinkDelay();
            PostMessageW(window_, kBlinkStateMessage, FALSE, 0);
        }
    }

    void render()
    {
        uploadPendingImage();
        const ULONGLONG now = GetTickCount64();
        updateBlinkState(now);

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
        const bool reactionState =
            (g_previewReaction.load() || g_microphoneReaction.load()) && reactionAvatarLoaded_;
        const TextureAsset *activeAvatar = reactionState ? &reactionAvatar_ : &primaryAvatar_;
        if (blinking_) {
            if (reactionState && reactionBlinkAvatarLoaded_)
                activeAvatar = &reactionBlinkAvatar_;
            else if (!reactionState && primaryBlinkAvatarLoaded_)
                activeAvatar = &primaryBlinkAvatar_;
        }
        const float scale = std::min(maxWidth / activeAvatar->width, maxHeight / activeAvatar->height);
        const float avatarWidth = activeAvatar->width * scale;
        const float avatarHeight = activeAvatar->height * scale;
        const float bob = g_motionEnabled.load()
                              ? std::sin(static_cast<float>(GetTickCount64()) / 500.0f) *
                                    std::max(3.0f, static_cast<float>(height_) * 0.015f)
                              : 0.0f;
        draw(*activeAvatar, (static_cast<float>(width_) - avatarWidth) * 0.5f,
             (static_cast<float>(height_) - avatarHeight) * 0.5f + bob, avatarWidth,
             avatarHeight);

        const bool overlayVisible = g_applicationActive.load() && !g_dialogOpen.load() &&
                                    !isAvatarSettingsWindowVisible();
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
        primaryAvatar_ = createTexture(pixels.data(), size, size);
        control_ = createSolid(30, 36, 48, 255);
        border_ = createSolid(48, 59, 74, 255);
        accent_ = createSolid(0, 212, 255, 255);
        avatarIconText_ = createText(L"A", 30, RGB(230, 232, 235));
        settingsIconText_ = createText(L"S", 30, RGB(230, 232, 235));
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

    void drawOutlinedRect(const RectF &rect, const TextureAsset &fill, float borderWidth = 2.0f)
    {
        drawRect(border_, rect);
        const RectF inner{rect.x + borderWidth, rect.y + borderWidth,
                          std::max(1.0f, rect.width - borderWidth * 2.0f),
                          std::max(1.0f, rect.height - borderWidth * 2.0f)};
        drawRect(fill, inner);
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
        const UiLayout ui = calculateUiLayout(width_, height_, kOutputUiDpi);
        drawOutlinedRect(ui.avatar, control_, 2.0f);
        drawLabel(avatarIconText_, ui.avatar, ui.avatar.height * 0.42f);
        drawOutlinedRect(ui.settings, control_, 2.0f);
        drawLabel(settingsIconText_, ui.settings, ui.settings.height * 0.42f);
        const RectF activeEdge{ui.settings.x, ui.settings.y, 5.0f, ui.settings.height};
        drawRect(accent_, activeEdge);
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
    TextureAsset primaryAvatar_;
    TextureAsset reactionAvatar_;
    TextureAsset primaryBlinkAvatar_;
    TextureAsset reactionBlinkAvatar_;
    bool reactionAvatarLoaded_ = false;
    bool primaryBlinkAvatarLoaded_ = false;
    bool reactionBlinkAvatarLoaded_ = false;
    bool blinking_ = false;
    ULONGLONG nextBlinkAt_ = 0;
    ULONGLONG blinkEndsAt_ = 0;
    unsigned blinkRandomState_ = static_cast<unsigned>(GetTickCount());
    TextureAsset control_;
    TextureAsset border_;
    TextureAsset accent_;
    TextureAsset avatarIconText_;
    TextureAsset settingsIconText_;
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
    const UiLayout ui = calculateUiLayout(kOutputWidth, kOutputHeight, kOutputUiDpi);
    if (ui.settings.contains(outputX, outputY))
        showAvatarSettingsWindow(window);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_ACTIVATE:
        g_applicationActive.store(LOWORD(wParam) != WA_INACTIVE);
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
    case kAvatarSettingsChoosePngMessage:
        openPngPicker(reinterpret_cast<HWND>(lParam));
        return 0;
    case kAvatarSettingsChooseReactionPngMessage:
        openPngPicker(reinterpret_cast<HWND>(lParam), ImageSlot::Reaction);
        return 0;
    case kAvatarSettingsChoosePrimaryBlinkMessage:
        openPngPicker(reinterpret_cast<HWND>(lParam), ImageSlot::PrimaryBlink);
        return 0;
    case kAvatarSettingsChooseReactionBlinkMessage:
        openPngPicker(reinterpret_cast<HWND>(lParam), ImageSlot::ReactionBlink);
        return 0;
    case kAvatarSettingsPreviewReactionMessage:
        g_previewReaction.store(wParam != FALSE);
        postAvatarSettingsMessage(wParam != FALSE ? L"reaction-preview-on" : L"reaction-preview-off");
        return 0;
    case kAvatarSettingsReadyMessage:
        sendPrimaryImageState();
        sendReactionImageState();
        sendBlinkImageState(ImageSlot::PrimaryBlink);
        sendBlinkImageState(ImageSlot::ReactionBlink);
        sendMicrophoneState();
        sendBlinkSettings();
        postAvatarSettingsMessage(L"reaction-preview-off");
        return 0;
    case kAvatarSettingsSelectMicrophoneMessage: {
        std::unique_ptr<std::wstring> selected(reinterpret_cast<std::wstring *>(lParam));
        g_selectedMicrophoneId = selected && *selected != L"@default" ? *selected : L"";
        saveSetting(L"MicrophoneDevice", g_selectedMicrophoneId.empty() ? L"@default"
                                                                         : g_selectedMicrophoneId);
        if (!g_audioMonitor)
            g_audioMonitor = std::make_unique<AudioInputMonitor>();
        g_audioMonitorStatus.store(0);
        g_audioMonitorLevel.store(0);
        setMicrophoneReaction(false);
        postAvatarSettingsMessage(L"microphone-status\tConnecting…");
        postAvatarSettingsMessage(L"microphone-level\t0");
        g_audioMonitor->start(window, g_selectedMicrophoneId);
        return 0;
    }
    case kAvatarSettingsReactionThresholdMessage:
        g_reactionThreshold.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 1, 1000));
        saveSetting(L"ReactionThreshold", std::to_wstring(g_reactionThreshold.load()));
        processMicrophoneLevel(g_audioMonitorLevel.load());
        return 0;
    case kAvatarSettingsReleaseDelayMessage:
        g_releaseDelayMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 0, 5000));
        saveSetting(L"ReleaseDelayMs", std::to_wstring(g_releaseDelayMs.load()));
        return 0;
    case kAvatarSettingsBlinkEnabledMessage:
        g_blinkEnabled.store(wParam != FALSE);
        saveSetting(L"BlinkEnabled", wParam != FALSE ? L"1" : L"0");
        return 0;
    case kAvatarSettingsBlinkMinimumMessage: {
        const unsigned value = std::clamp<unsigned>(static_cast<unsigned>(wParam), 250, 30000);
        g_blinkMinimumMs.store(value);
        if (g_blinkMaximumMs.load() < value)
            g_blinkMaximumMs.store(value);
        saveSetting(L"BlinkMinimumMs", std::to_wstring(value));
        saveSetting(L"BlinkMaximumMs", std::to_wstring(g_blinkMaximumMs.load()));
        sendBlinkSettings();
        return 0;
    }
    case kAvatarSettingsBlinkMaximumMessage: {
        const unsigned value = std::clamp<unsigned>(static_cast<unsigned>(wParam), 250, 30000);
        g_blinkMaximumMs.store(value);
        if (g_blinkMinimumMs.load() > value)
            g_blinkMinimumMs.store(value);
        saveSetting(L"BlinkMaximumMs", std::to_wstring(value));
        saveSetting(L"BlinkMinimumMs", std::to_wstring(g_blinkMinimumMs.load()));
        sendBlinkSettings();
        return 0;
    }
    case kAvatarSettingsBlinkDurationMessage:
        g_blinkDurationMs.store(std::clamp<unsigned>(static_cast<unsigned>(wParam), 50, 1000));
        saveSetting(L"BlinkDurationMs", std::to_wstring(g_blinkDurationMs.load()));
        return 0;
    case kAudioMonitorLevelMessage:
        g_audioMonitorLevel.store(static_cast<unsigned>(wParam));
        processMicrophoneLevel(static_cast<unsigned>(wParam));
        postAvatarSettingsMessage(L"microphone-level\t" + std::to_wstring(wParam));
        return 0;
    case kAudioMonitorStatusMessage:
        g_audioMonitorStatus.store(static_cast<int>(wParam));
        postAvatarSettingsMessage(wParam == 1 ? L"microphone-status\tListening"
                                               : L"microphone-status\tInput unavailable");
        if (wParam != 1)
            postAvatarSettingsMessage(L"microphone-level\t0");
        if (wParam != 1)
            setMicrophoneReaction(false);
        return 0;
    case kBlinkStateMessage:
        postAvatarSettingsMessage(wParam != FALSE ? L"blink-state-on" : L"blink-state-off");
        return 0;
    case kImageUploadFailureMessage:
        if (static_cast<ImageSlot>(wParam) == ImageSlot::Reaction) {
            sendReactionImageState();
            postAvatarSettingsMessage(L"reaction-image-upload-error");
        } else if (static_cast<ImageSlot>(wParam) == ImageSlot::PrimaryBlink ||
                   static_cast<ImageSlot>(wParam) == ImageSlot::ReactionBlink) {
            const ImageSlot slot = static_cast<ImageSlot>(wParam);
            sendBlinkImageState(slot);
            postAvatarSettingsMessage(std::wstring(imageMessagePrefix(slot)) + L"-upload-error");
        } else {
            sendPrimaryImageState();
            postAvatarSettingsMessage(L"avatar-image-upload-error");
        }
        MessageBoxW(window,
                    L"The PNG decoded successfully, but its GPU texture could not be created. The current avatar is unchanged.",
                    L"RearSilver Avatar — PNG loading", MB_OK | MB_ICONERROR);
        return 0;
    case kImageUploadSuccessMessage:
        if (static_cast<ImageSlot>(wParam) == ImageSlot::Reaction) {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Reaction), g_reactionImagePath);
        } else if (static_cast<ImageSlot>(wParam) == ImageSlot::PrimaryBlink) {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            setAvatarSettingsPreviewImage(2, g_primaryBlinkImagePath);
        } else if (static_cast<ImageSlot>(wParam) == ImageSlot::ReactionBlink) {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            setAvatarSettingsPreviewImage(3, g_reactionBlinkImagePath);
        } else {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            setAvatarSettingsPreviewImage(static_cast<unsigned>(ImageSlot::Primary), g_primaryImagePath);
        }
        postAvatarSettingsMessage(std::wstring(imageMessagePrefix(static_cast<ImageSlot>(wParam))) +
                                  L"-uploaded");
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

    const std::wstring savedPrimaryImage = loadSetting(L"PrimaryImage");
    if (!savedPrimaryImage.empty()) {
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            g_primaryImagePath = savedPrimaryImage;
            g_primaryImageLoaded = false;
        }
        try {
            PendingImage decoded;
            if (!decodePng(savedPrimaryImage.c_str(), decoded))
                throw E_INVALIDARG;
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(decoded));
            logMessage(L"Saved primary avatar queued for startup restore: " + savedPrimaryImage);
        } catch (...) {
            logMessage(L"Saved primary avatar is unavailable; using built-in default: " + savedPrimaryImage);
        }
    }

    const std::wstring savedReactionImage = loadSetting(L"ReactionImage");
    if (!savedReactionImage.empty()) {
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            g_reactionImagePath = savedReactionImage;
            g_reactionImageLoaded = false;
        }
        try {
            PendingImage decoded;
            if (!decodePng(savedReactionImage.c_str(), decoded))
                throw E_INVALIDARG;
            decoded.slot = ImageSlot::Reaction;
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(decoded));
            logMessage(L"Saved reaction avatar queued for startup restore: " + savedReactionImage);
        } catch (...) {
            logMessage(L"Saved reaction avatar is unavailable: " + savedReactionImage);
        }
    }

    auto queueSavedBlinkImage = [](const wchar_t *settingKey, ImageSlot slot,
                                   std::wstring &statePath, bool &stateLoaded) {
        const std::wstring savedPath = loadSetting(settingKey);
        if (savedPath.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(g_primaryImageStateMutex);
            statePath = savedPath;
            stateLoaded = false;
        }
        try {
            PendingImage decoded;
            if (!decodePng(savedPath.c_str(), decoded))
                throw E_INVALIDARG;
            decoded.slot = slot;
            std::lock_guard<std::mutex> lock(g_pendingImageMutex);
            g_pendingImages.push_back(std::move(decoded));
            logMessage(L"Saved blink avatar queued for startup restore: " + savedPath);
        } catch (...) {
            logMessage(L"Saved blink avatar is unavailable: " + savedPath);
        }
    };
    queueSavedBlinkImage(L"PrimaryBlinkImage", ImageSlot::PrimaryBlink,
                         g_primaryBlinkImagePath, g_primaryBlinkImageLoaded);
    queueSavedBlinkImage(L"ReactionBlinkImage", ImageSlot::ReactionBlink,
                         g_reactionBlinkImagePath, g_reactionBlinkImageLoaded);

    const std::wstring savedMicrophone = loadSetting(L"MicrophoneDevice");
    g_selectedMicrophoneId = savedMicrophone.empty() || savedMicrophone == L"@default"
                                 ? L""
                                 : savedMicrophone;
    g_audioMonitor = std::make_unique<AudioInputMonitor>();

    const std::wstring savedThreshold = loadSetting(L"ReactionThreshold");
    if (!savedThreshold.empty())
        g_reactionThreshold.store(std::clamp<unsigned>(wcstoul(savedThreshold.c_str(), nullptr, 10),
                                                       1, 1000));
    const std::wstring savedRelease = loadSetting(L"ReleaseDelayMs");
    if (!savedRelease.empty())
        g_releaseDelayMs.store(std::clamp<unsigned>(wcstoul(savedRelease.c_str(), nullptr, 10),
                                                    0, 5000));
    const std::wstring savedBlinkEnabled = loadSetting(L"BlinkEnabled");
    if (!savedBlinkEnabled.empty())
        g_blinkEnabled.store(savedBlinkEnabled != L"0");
    const std::wstring savedBlinkMinimum = loadSetting(L"BlinkMinimumMs");
    if (!savedBlinkMinimum.empty())
        g_blinkMinimumMs.store(std::clamp<unsigned>(wcstoul(savedBlinkMinimum.c_str(), nullptr, 10),
                                                    250, 30000));
    const std::wstring savedBlinkMaximum = loadSetting(L"BlinkMaximumMs");
    if (!savedBlinkMaximum.empty())
        g_blinkMaximumMs.store(std::clamp<unsigned>(wcstoul(savedBlinkMaximum.c_str(), nullptr, 10),
                                                    g_blinkMinimumMs.load(), 30000));
    const std::wstring savedBlinkDuration = loadSetting(L"BlinkDurationMs");
    if (!savedBlinkDuration.empty())
        g_blinkDurationMs.store(std::clamp<unsigned>(wcstoul(savedBlinkDuration.c_str(), nullptr, 10),
                                                     50, 1000));
    g_audioMonitor->start(g_mainWindow, g_selectedMicrophoneId);

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
    if (g_audioMonitor)
        g_audioMonitor->stop();
    if (renderThread.joinable())
        renderThread.join();
    shutdownAvatarSettingsWindow();
    if (g_logFile != INVALID_HANDLE_VALUE)
        CloseHandle(g_logFile);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
