#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <deque>
#include <winver.h>
#include <tlhelp32.h>
#include <atomic>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;
static HWND windowHandle;
static HWND previewWindowHandle;
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;
static ComPtr<ID3D11Device> independentDevice;
static ComPtr<ID3D11DeviceContext> independentContext;
static ComPtr<IDXGISwapChain> swapChain;
static ComPtr<ID3D11RenderTargetView> target;
static ComPtr<ID3D11VertexShader> vertexShader;
static ComPtr<ID3D11PixelShader> pixelShader;
static ComPtr<ID3D11InputLayout> layout;
static ComPtr<ID3D11Buffer> vertices;
static ComPtr<ID3D11SamplerState> sampler;
static ComPtr<ID3D11BlendState> blending;
static ComPtr<ID3D11ShaderResourceView> avatar, controls;
static UINT imageWidth = 256, imageHeight = 256;
static constexpr int width = 960, height = 720;
static std::atomic<int> background{0};
static std::atomic<bool> focused{true}, menuVisible{true}, animate{true}, dialogOpen{false};
static std::mutex avatarMutex;
static std::atomic<bool> renderRunning{false};

enum : int {
	ID_LOAD_PNG = 1001,
	ID_BACKGROUND = 1002,
	ID_TOGGLE_MOTION = 1003,
	ID_COPY_LOG = 1004,
	ID_STATUS = 1005,
};
static std::wstring imageName = L"Built-in test shape";
static HANDLE logFile = INVALID_HANDLE_VALUE;
static std::wstring logPath;
static std::deque<std::wstring> logLines;
static ULONGLONG frames = 0, lastReport = 0, lastFrames = 0, lastBadge = 0;
static HRESULT lastPresent = S_OK;
static ComPtr<ID3D11ShaderResourceView> counterBadge;
struct CrossProcessResult {
	volatile LONG sequence;
	DWORD mapId;
	LONG openResult;
	DWORD width;
	DWORD height;
	DWORD format;
};
static HANDLE receiverResultMapping = nullptr;
static CrossProcessResult *receiverResult = nullptr;
static HANDLE receiverProcess = nullptr;
static LONG lastReceiverSequence = 0;

static std::wstring hexResult(HRESULT value)
{
	wchar_t text[24];
	swprintf_s(text, L"0x%08X", static_cast<unsigned>(value));
	return text;
}

static void eventLog(const std::wstring &message)
{
	SYSTEMTIME time{};
	GetLocalTime(&time);
	wchar_t stamp[80];
	swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u.%03u | ", time.wYear, time.wMonth, time.wDay, time.wHour,
		   time.wMinute, time.wSecond, time.wMilliseconds);
	std::wstring line = std::wstring(stamp) + message + L"\r\n";
	logLines.push_back(line);
	if (logLines.size() > 1500)
		logLines.pop_front();
	if (logFile != INVALID_HANDLE_VALUE) {
		int size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0,
					       nullptr, nullptr);
		std::string bytes(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), bytes.data(), size,
				    nullptr, nullptr);
		DWORD written;
		WriteFile(logFile, bytes.data(), size, &written, nullptr);
	}
}

static void startLog()
{
	wchar_t folder[MAX_PATH + 1]{};
	DWORD length = GetTempPathW(MAX_PATH, folder);
	if (length && length < MAX_PATH) {
		SYSTEMTIME t{};
		GetLocalTime(&t);
		wchar_t name[160];
		swprintf_s(name, L"RearSilverAvatar-%04u%02u%02u-%02u%02u%02u-%lu.log", t.wYear, t.wMonth,
			   t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
		logPath = std::wstring(folder) + name;
		logFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
				      FILE_ATTRIBUTE_NORMAL, nullptr);
	}
	eventLog(L"RearSilver Avatar main build; process=" + std::to_wstring(GetCurrentProcessId()) +
		 L"; dynamic D3D11 shared-resource interoperability selection");
	eventLog(logFile == INVALID_HANDLE_VALUE ? L"File logging unavailable; Copy event log still works"
						 : L"Log file: " + logPath);
}

static std::wstring windowsError(DWORD error)
{
	wchar_t *message = nullptr;
	DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
					      FORMAT_MESSAGE_IGNORE_INSERTS,
				      nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
	std::wstring text = length ? std::wstring(message, length) : L"No system description available";
	if (message)
		LocalFree(message);
	while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
		text.pop_back();
	return std::to_wstring(error) + L" (" + text + L")";
}

static void probePresentDetour()
{
    if (!swapChain) return;
    ULONG_PTR current = reinterpret_cast<ULONG_PTR>((*reinterpret_cast<void***>(swapChain.Get()))[8]);
    ULONG_PTR visited[5]{};
    eventLog(L"Present chain snapshot; hook loaded=" + std::to_wstring(GetModuleHandleW(L"graphics-hook64.dll") != nullptr) +
        L"; inspection only, not proof of callback execution");
    for (int hop=0; hop<5; ++hop) {
        if (!current) {eventLog(L"Chain stopped: null destination"); return;}
        for (int i=0; i<hop; ++i) if (visited[i]==current) {eventLog(L"Chain stopped: repeated address"); return;}
        visited[hop]=current;
        MEMORY_BASIC_INFORMATION page{};
        if (VirtualQuery(reinterpret_cast<void*>(current), &page, sizeof(page)) != sizeof(page)) {
            eventLog(L"Chain stopped: VirtualQuery failed: " + windowsError(GetLastError())); return;
        }
        std::wstring module=L"No loaded module identified";
        HMODULE owner=nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(current), &owner)) {
            wchar_t path[32768]{};
            DWORD n=GetModuleFileNameW(owner,path,static_cast<DWORD>(std::size(path)));
            if(n && n<std::size(path)) module.assign(path,n);
            FreeLibrary(owner); // Balance the reference acquired above.
        }
        wchar_t line[512];
        swprintf_s(line,L"Chain hop %d: address=%p; allocation=%p; state=0x%lX; type=0x%lX; protection=0x%lX",
            hop,reinterpret_cast<void*>(current),page.AllocationBase,page.State,page.Type,page.Protect);
        eventLog(line); eventLog(L"Chain module: " + module);
        if(page.State!=MEM_COMMIT || (page.Protect & (PAGE_GUARD|PAGE_NOACCESS))) {
            eventLog(L"Chain stopped: uncommitted or protected page"); return;
        }
        unsigned char bytes[16]{}; SIZE_T count=0;
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(current),bytes,sizeof(bytes),&count) || count!=sizeof(bytes)) {
            eventLog(L"Chain stopped: incomplete instruction read: " + windowsError(GetLastError())); return;
        }
        std::wstring dump;
        for(unsigned char byte:bytes) {wchar_t pair[4];swprintf_s(pair,L"%02X ",static_cast<unsigned>(byte));dump+=pair;}
        eventLog(L"Chain bytes: " + dump);
        ULONG_PTR next=0; LONG displacement=0;
        if(bytes[0]==0xE9) {
            memcpy(&displacement,bytes+1,sizeof(displacement));
            next=current+5+displacement;
            eventLog(L"Chain instruction: E9 relative jump");
        } else if(bytes[0]==0xFF && bytes[1]==0x25) {
            memcpy(&displacement,bytes+2,sizeof(displacement));
            ULONG_PTR pointer=current+6+displacement;
            MEMORY_BASIC_INFORMATION pointerPage{};
            if(VirtualQuery(reinterpret_cast<void*>(pointer),&pointerPage,sizeof(pointerPage))!=sizeof(pointerPage) ||
                pointerPage.State!=MEM_COMMIT || (pointerPage.Protect & (PAGE_GUARD|PAGE_NOACCESS))) {
                eventLog(L"Chain stopped: indirect pointer page unavailable or protected");return;
            }
            count=0;
            if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(pointer),&next,sizeof(next),&count) || count!=sizeof(next)) {
                eventLog(L"Chain stopped: incomplete indirect pointer read: " + windowsError(GetLastError()));return;
            }
            eventLog(L"Chain instruction: FF 25 indirect jump");
        } else if(bytes[0]==0x48 && bytes[1]==0xB8 && bytes[10]==0xFF && bytes[11]==0xE0) {
            memcpy(&next,bytes+2,sizeof(next));
            eventLog(L"Chain instruction: MOV RAX / JMP RAX");
        } else {
            eventLog(L"Chain stopped: instruction pattern not recognised; final handler not established");return;
        }
        swprintf_s(line,L"Chain destination=%p",reinterpret_cast<void*>(next));eventLog(line);
        current=next;
    }
    eventLog(L"Chain stopped: five-hop limit reached");
}

static void probeHookInfo()
{
	// Read-only snapshot of OBS 32.2.2's 648-byte hook_info ABI.
	const auto name = L"CaptureHook_HookInfo" + std::to_wstring(GetCurrentProcessId());
	HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
	if (!mapping) {
		eventLog(L"Hook info unavailable: " + windowsError(GetLastError()));
		return;
	}
	const void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 648);
	if (!view) {
		const DWORD error = GetLastError();
		CloseHandle(mapping);
		eventLog(L"Hook info map failed: " + windowsError(error));
		return;
	}
	unsigned char snapshot[648]{};
	memcpy(snapshot, view, sizeof(snapshot));
	UnmapViewOfFile(view);
	CloseHandle(mapping);
	auto u32 = [&](size_t offset) {
		DWORD value = 0;
		memcpy(&value, snapshot + offset, sizeof(value));
		return value;
	};
	eventLog(L"OBS hook-info snapshot (32.2.2 layout): version=" + std::to_wstring(u32(0)) + L"." +
		 std::to_wstring(u32(4)) + L"; type=" + std::to_wstring(u32(8)) + L"; window=" + hexResult(u32(12)) +
		 L"; format=" + std::to_wstring(u32(16)) + L"; size=" + std::to_wstring(u32(20)) + L"x" +
		 std::to_wstring(u32(24)) + L"; map id=" + std::to_wstring(u32(40)) + L"; map bytes=" +
		 std::to_wstring(u32(44)) + L"; force shmem=" + std::to_wstring(snapshot[65]));
	if (!swapChain)
		return;

	void *present = (*reinterpret_cast<void ***>(swapChain.Get()))[8];
	HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
	const auto expected = reinterpret_cast<ULONG_PTR>(dxgi) + u32(92);
	wchar_t addresses[256];
	swprintf_s(addresses, L"Present address comparison: actual=%p; OBS DXGI offset=0x%08X; expected=%p; match=%u",
		   present, u32(92), reinterpret_cast<void *>(expected),
		   static_cast<unsigned>(dxgi && u32(92) && reinterpret_cast<ULONG_PTR>(present) == expected));
	eventLog(addresses);

}

static void probeSharedTextureHandle()
{
	// OBS can abandon a failed shared-texture attempt within milliseconds. Run this
	// directly after Present and inspect each new texture mapping only once.
	static DWORD lastWindow = 0;
	static DWORD lastMapId = 0;
	const auto hookInfoName = L"CaptureHook_HookInfo" + std::to_wstring(GetCurrentProcessId());
	HANDLE hookInfoMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, hookInfoName.c_str());
	if (!hookInfoMapping)
		return;
	const void *hookInfoView = MapViewOfFile(hookInfoMapping, FILE_MAP_READ, 0, 0, 648);
	if (!hookInfoView) {
		CloseHandle(hookInfoMapping);
		return;
	}
	unsigned char snapshot[648]{};
	memcpy(snapshot, hookInfoView, sizeof(snapshot));
	UnmapViewOfFile(hookInfoView);
	CloseHandle(hookInfoMapping);
	auto u32 = [&](size_t offset) {
		DWORD value = 0;
		memcpy(&value, snapshot + offset, sizeof(value));
		return value;
	};
	const DWORD captureType = u32(8);
	const DWORD capturedWindow = u32(12);
	const DWORD mapId = u32(40);
	const DWORD mapBytes = u32(44);
	if (captureType != 1 || !capturedWindow || !mapId || mapBytes < sizeof(DWORD) ||
	    (capturedWindow == lastWindow && mapId == lastMapId))
		return;

	const auto textureMapName = L"CaptureHook_Texture_" + std::to_wstring(capturedWindow) + L"_" +
				    std::to_wstring(mapId);
	HANDLE textureMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, textureMapName.c_str());
	if (!textureMapping)
		return;
	const void *textureView = MapViewOfFile(textureMapping, FILE_MAP_READ, 0, 0, sizeof(DWORD));
	if (!textureView) {
		CloseHandle(textureMapping);
		return;
	}
	DWORD handleValue = 0;
	memcpy(&handleValue, textureView, sizeof(handleValue));
	UnmapViewOfFile(textureView);
	CloseHandle(textureMapping);
	if (!handleValue)
		return;
	lastWindow = capturedWindow;
	lastMapId = mapId;

	wchar_t identity[512];
	swprintf_s(identity,
		   L"Fast shared-texture probe: mapping=%s; window=%lu; map id=%lu; map bytes=%lu; handle=0x%08lX",
		   textureMapName.c_str(), capturedWindow, mapId, mapBytes, handleValue);
	eventLog(identity);

	ComPtr<ID3D11Texture2D> openedTexture;
	const HANDLE sharedHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handleValue));
	const HRESULT openResult = device->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&openedTexture));
	if (FAILED(openResult) || !openedTexture) {
		eventLog(L"Fast shared-texture result: prototype OpenSharedResource failed with " +
			 hexResult(openResult));
		return;
	}
	D3D11_TEXTURE2D_DESC description{};
	openedTexture->GetDesc(&description);
	wchar_t details[512];
	swprintf_s(details,
		   L"Fast shared-texture result: prototype OpenSharedResource succeeded; size=%ux%u; format=%u; "
		   L"mips=%u; array=%u; samples=%u; usage=%u; bind=0x%08X; misc=0x%08X",
		   description.Width, description.Height, static_cast<unsigned>(description.Format),
		   description.MipLevels, description.ArraySize, description.SampleDesc.Count,
		   static_cast<unsigned>(description.Usage), description.BindFlags, description.MiscFlags);
	eventLog(details);

	static bool independentCreationAttempted = false;
	if (!independentCreationAttempted) {
		independentCreationAttempted = true;
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> adapter;
		HRESULT independentResult = device.As(&dxgiDevice);
		if (SUCCEEDED(independentResult))
			independentResult = dxgiDevice->GetAdapter(adapter.GetAddressOf());
		D3D_FEATURE_LEVEL independentFeatureLevel{};
		if (SUCCEEDED(independentResult))
			independentResult = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
							D3D11_SDK_VERSION, independentDevice.GetAddressOf(),
							&independentFeatureLevel, independentContext.GetAddressOf());
		if (SUCCEEDED(independentResult))
			eventLog(L"Independent D3D11 device created on the originating adapter; feature level=" +
				 hexResult(static_cast<HRESULT>(independentFeatureLevel)));
		else
			eventLog(L"Independent D3D11 device creation failed with " + hexResult(independentResult));
	}
	if (!independentDevice)
		return;
	ComPtr<ID3D11Texture2D> independentlyOpenedTexture;
	const HRESULT independentOpenResult =
		independentDevice->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&independentlyOpenedTexture));
	if (FAILED(independentOpenResult) || !independentlyOpenedTexture) {
		eventLog(L"Independent-device shared-texture result: OpenSharedResource failed with " +
			 hexResult(independentOpenResult));
		return;
	}
	D3D11_TEXTURE2D_DESC independentDescription{};
	independentlyOpenedTexture->GetDesc(&independentDescription);
	wchar_t independentDetails[384];
	swprintf_s(independentDetails,
		   L"Independent-device shared-texture result: OpenSharedResource succeeded; size=%ux%u; format=%u; "
		   L"samples=%u; bind=0x%08X; misc=0x%08X",
		   independentDescription.Width, independentDescription.Height,
		   static_cast<unsigned>(independentDescription.Format), independentDescription.SampleDesc.Count,
		   independentDescription.BindFlags, independentDescription.MiscFlags);
	eventLog(independentDetails);
}

static void probeCaptureStartup()
{
	probePresentDetour();
	probeHookInfo();
	const std::wstring name = L"CaptureHook_KeepAlive" + std::to_wstring(GetCurrentProcessId());
	HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
	DWORD error = mutex ? ERROR_SUCCESS : GetLastError();
	if (mutex)
		CloseHandle(mutex);
	eventLog(L"Keepalive probe: name=" + name + L"; access=SYNCHRONIZE; open=" +
		 (mutex ? std::wstring(L"yes") : std::wstring(L"no")) + L"; error=" + windowsError(error));

	static std::wstring reportedPath;
	HMODULE hook = nullptr;
	if (!GetModuleHandleExW(0, L"graphics-hook64.dll", &hook)) {
		if (!reportedPath.empty()) {
			eventLog(L"Hook module no longer loaded");
			reportedPath.clear();
		}
		return;
	}
	wchar_t path[32768]{};
	DWORD count = GetModuleFileNameW(hook, path, static_cast<DWORD>(std::size(path)));
	error = (count == 0 || count >= std::size(path)) ? GetLastError() : ERROR_SUCCESS;
	FreeLibrary(hook);
	if (error != ERROR_SUCCESS || count == 0) {
		eventLog(L"Hook path lookup failed: " + windowsError(error));
		return;
	}
	if (reportedPath == path)
		return;
	reportedPath = path;
	eventLog(L"Loaded graphics-hook64.dll path: " + reportedPath);
	DWORD ignored = 0;
	DWORD size = GetFileVersionInfoSizeW(path, &ignored);
	if (!size) {
		eventLog(L"Hook file version unavailable: " + windowsError(GetLastError()));
		return;
	}
	std::vector<unsigned char> versionData(size);
	if (!GetFileVersionInfoW(path, 0, size, versionData.data())) {
		eventLog(L"Hook file version read failed: " + windowsError(GetLastError()));
		return;
	}
	VS_FIXEDFILEINFO *version = nullptr;
	UINT bytes = 0;
	if (!VerQueryValueW(versionData.data(), L"\\", reinterpret_cast<void **>(&version), &bytes) ||
	    bytes < sizeof(VS_FIXEDFILEINFO) || !version || version->dwSignature != 0xFEEF04BD) {
		eventLog(L"Hook file version resource has no valid fixed version information");
		return;
	}
	auto formatVersion = [](DWORD high, DWORD low) {
		return std::to_wstring(HIWORD(high)) + L"." + std::to_wstring(LOWORD(high)) + L"." +
		       std::to_wstring(HIWORD(low)) + L"." + std::to_wstring(LOWORD(low));
	};
	eventLog(L"Hook file version=" + formatVersion(version->dwFileVersionMS, version->dwFileVersionLS) +
		 L"; product version=" + formatVersion(version->dwProductVersionMS, version->dwProductVersionLS));
}

static void copyLog()
{
	probeCaptureStartup();
	eventLog(L"Copy event log requested (latest 1500 events)");
	std::wstring text = L"RearSilver Avatar capture diagnostic build 11\r\n";
	for (const auto &line : logLines)
		text += line;
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
	if (!memory) {
		MessageBoxW(windowHandle, L"Not enough memory to copy the log.", L"Event log", MB_OK);
		return;
	}
	void *destination = GlobalLock(memory);
	if (!destination) {
		GlobalFree(memory);
		return;
	}
	memcpy(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
	GlobalUnlock(memory);
	bool copied = false;
	if (OpenClipboard(windowHandle)) {
		if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory))
			copied = true;
		CloseClipboard();
	}
	if (!copied)
		GlobalFree(memory);
	MessageBoxW(windowHandle,
		    copied ? L"Event log copied. Paste it into our conversation with Ctrl+V."
			   : L"Could not access the clipboard. Please try Copy event log again.",
		    L"Event log", MB_OK | (copied ? MB_ICONINFORMATION : MB_ICONERROR));
}

struct Vertex {
	float x, y, u, v;
};
static void check(HRESULT result)
{
	if (FAILED(result)) {
		eventLog(L"Graphics/API failure: " + hexResult(result));
		throw result;
	}
}

static ComPtr<ID3D11ShaderResourceView> texture(const void *pixels, UINT w, UINT h)
{
	D3D11_TEXTURE2D_DESC description{};
	description.Width = w;
	description.Height = h;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_IMMUTABLE;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data{pixels, w * 4, 0};
	ComPtr<ID3D11Texture2D> resource;
	check(device->CreateTexture2D(&description, &data, &resource));
	ComPtr<ID3D11ShaderResourceView> view;
	check(device->CreateShaderResourceView(resource.Get(), nullptr, &view));
	return view;
}

static void updateControls()
{
	constexpr int w = 650, h = 150;
	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = w;
	info.bmiHeader.biHeight = -h;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
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
	HGDIOBJ old = SelectObject(dc, bitmap);
	RECT area{0, 0, w, h};
	HBRUSH brush = CreateSolidBrush(RGB(32, 32, 32));
	FillRect(dc, &area, brush);
	DeleteObject(brush);
	auto oldFont = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(245, 245, 245));
	std::wstring title = L"REARSILVER AVATAR — DIAGNOSTIC BUILD 11";
	TextOutW(dc, 12, 10, title.c_str(), static_cast<int>(title.size()));
	const wchar_t *labels[] = {L"Load PNG", L"Background", L"Toggle motion", L"Hide controls (F1)"};
	for (int i = 0; i < 4; ++i) {
		RECT r{12 + i * 158, 35, 160 + i * 158, 66};
		brush = CreateSolidBrush(RGB(65, 65, 65));
		FillRect(dc, &r, brush);
		DeleteObject(brush);
		DrawTextW(dc, labels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	const wchar_t *modes[] = {L"Transparent output (black local preview)", L"Green chroma key",
				  L"Solid dark background"};
	std::wstring status = std::wstring(modes[background.load()]) + L" | " +
			      (animate.load() ? L"Motion on" : L"Motion off");
	TextOutW(dc, 12, 78, status.c_str(), static_cast<int>(status.size()));
	RECT copyButton{12, 108, 220, 139};
	brush = CreateSolidBrush(RGB(65, 65, 65));
	FillRect(dc, &copyButton, brush);
	DeleteObject(brush);
	DrawTextW(dc, L"Copy event log (Ctrl+L)", -1, &copyButton, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	GdiFlush();
	auto bytes = static_cast<unsigned char *>(pixels);
	for (int i = 0; i < w * h; ++i) {
		std::swap(bytes[i * 4], bytes[i * 4 + 2]);
		bytes[i * 4 + 3] = 255;
	}
	std::vector<unsigned char> copy(bytes, bytes + w * h * 4);
	SelectObject(dc, oldFont);
	SelectObject(dc, old);
	DeleteObject(bitmap);
	DeleteDC(dc);
	controls = texture(copy.data(), w, h);
}

static void updateCounter()
{
	constexpr int w = 650, h = 28;
	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = w;
	info.bmiHeader.biHeight = -h;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	HDC dc = CreateCompatibleDC(nullptr);
	void *pixels = nullptr;
	HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
	if (!dc || !bitmap) {
		if (bitmap)
			DeleteObject(bitmap);
		if (dc)
			DeleteDC(dc);
		throw E_OUTOFMEMORY;
	}
	auto old = SelectObject(dc, bitmap);
	RECT rect{0, 0, w, h};
	HBRUSH brush = CreateSolidBrush(RGB(32, 32, 32));
	FillRect(dc, &rect, brush);
	DeleteObject(brush);
	auto font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, RGB(255, 255, 255));
	std::wstring label = L"Frame " + std::to_wstring(frames) + L" | Focus " +
			     (focused.load() ? L"yes" : L"no") + L" | Present " + hexResult(lastPresent);
	DrawTextW(dc, label.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	GdiFlush();
	auto bytes = static_cast<unsigned char *>(pixels);
	for (int i = 0; i < w * h; ++i) {
		std::swap(bytes[i * 4], bytes[i * 4 + 2]);
		bytes[i * 4 + 3] = 255;
	}
	std::vector<unsigned char> copy(bytes, bytes + w * h * 4);
	SelectObject(dc, font);
	SelectObject(dc, old);
	DeleteObject(bitmap);
	DeleteDC(dc);
	counterBadge = texture(copy.data(), w, h);
}

static void loadImage()
{
	wchar_t path[32768]{};
	OPENFILENAMEW picker{};
	picker.lStructSize = sizeof(picker);
	picker.hwndOwner = windowHandle;
	picker.lpstrFilter = L"PNG images\0*.png\0";
	picker.lpstrFile = path;
	picker.nMaxFile = 32768;
	picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	eventLog(L"Image picker opened; capture rendering continues on the render thread");
	dialogOpen = true;
	if (GetOpenFileNameW(&picker)) {
		try {
			ComPtr<IWICImagingFactory> factory;
			check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
					       IID_PPV_ARGS(&factory)));
			ComPtr<IWICBitmapDecoder> decoder;
			check(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
								 WICDecodeMetadataCacheOnLoad, &decoder));
			ComPtr<IWICBitmapFrameDecode> frame;
			check(decoder->GetFrame(0, &frame));
			UINT w, h;
			check(frame->GetSize(&w, &h));
			if (!w || !h || w > 8192 || h > 8192)
				throw E_INVALIDARG;
			ComPtr<IWICFormatConverter> converter;
			check(factory->CreateFormatConverter(&converter));
			check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
						    nullptr, 0, WICBitmapPaletteTypeCustom));
			std::vector<unsigned char> bytes(static_cast<size_t>(w) * h * 4);
			check(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(bytes.size()), bytes.data()));
			auto loaded = texture(bytes.data(), w, h);
			{
				std::lock_guard<std::mutex> lock(avatarMutex);
				avatar = loaded;
				imageWidth = w;
				imageHeight = h;
				imageName = path;
			}
			eventLog(L"PNG loaded: " + std::to_wstring(w) + L" x " + std::to_wstring(h));
		} catch (...) {
			MessageBoxW(
				windowHandle,
				L"Could not load this PNG. Please use a valid PNG up to 8192 × 8192 pixels. The previous image is unchanged.",
				L"Image loading", MB_OK | MB_ICONERROR);
		}
	}
	dialogOpen = false;
	focused.store(GetForegroundWindow() == windowHandle);
	menuVisible.store(focused.load());
	eventLog(L"Image picker closed");
}

static void resize()
{
	eventLog(L"Resize begin: " + std::to_wstring(width) + L" x " + std::to_wstring(height));
	context->OMSetRenderTargets(0, nullptr, nullptr);
	target.Reset();
	check(swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
	ComPtr<ID3D11Texture2D> buffer;
	check(swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)));
	check(device->CreateRenderTargetView(buffer.Get(), nullptr, &target));
	eventLog(L"Resize complete");
}

static void auditAdapters(const LUID &activeLuid)
{
	ComPtr<IDXGIFactory1> factory;
	HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	if (FAILED(result)) {
		eventLog(L"Adapter audit unavailable: CreateDXGIFactory1 returned " + hexResult(result));
		return;
	}

	for (UINT index = 0;; ++index) {
		ComPtr<IDXGIAdapter1> adapter;
		result = factory->EnumAdapters1(index, &adapter);
		if (result == DXGI_ERROR_NOT_FOUND)
			break;
		if (FAILED(result)) {
			eventLog(L"Adapter audit stopped at index " + std::to_wstring(index) + L": " + hexResult(result));
			break;
		}

		DXGI_ADAPTER_DESC1 description{};
		result = adapter->GetDesc1(&description);
		if (FAILED(result)) {
			eventLog(L"Adapter " + std::to_wstring(index) + L" description failed: " + hexResult(result));
			continue;
		}

		const bool active = description.AdapterLuid.HighPart == activeLuid.HighPart &&
				    description.AdapterLuid.LowPart == activeLuid.LowPart;
		eventLog(L"DXGI adapter[" + std::to_wstring(index) + L"]: " + description.Description +
			 L"; LUID=" + std::to_wstring(description.AdapterLuid.HighPart) + L":" +
			 std::to_wstring(description.AdapterLuid.LowPart) + L"; vendor=" +
			 std::to_wstring(description.VendorId) + L"; device=" + std::to_wstring(description.DeviceId) +
			 L"; software=" + std::to_wstring((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) +
			 L"; active-device=" + std::to_wstring(active));
	}
}

struct AdapterCandidate {
	ComPtr<IDXGIAdapter1> adapter;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	DXGI_ADAPTER_DESC1 description{};
	UINT outputs = 0;
	UINT relatedOpen = 0;
	UINT relatedTotal = 0;
	bool viable = false;
	bool selfOpen = false;
};

static bool sameAdapterIdentity(const DXGI_ADAPTER_DESC1 &a, const DXGI_ADAPTER_DESC1 &b)
{
	return a.VendorId == b.VendorId && a.DeviceId == b.DeviceId && a.SubSysId == b.SubSysId;
}

static bool sameLuid(const LUID &a, const LUID &b)
{
	return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

static ComPtr<IDXGIAdapter1> chooseInteroperableAdapter()
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
		if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
			break;
		AdapterCandidate candidate;
		candidate.adapter = adapter;
		check(adapter->GetDesc1(&candidate.description));
		for (UINT outputIndex = 0;; ++outputIndex) {
			ComPtr<IDXGIOutput> output;
			if (adapter->EnumOutputs(outputIndex, output.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
				break;
			++candidate.outputs;
		}
		if ((candidate.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
			D3D_FEATURE_LEVEL feature{};
			candidate.viable = SUCCEEDED(D3D11CreateDevice(
				adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
				candidate.device.GetAddressOf(), &feature, candidate.context.GetAddressOf()));
		}
		eventLog(L"Adapter candidate " + std::to_wstring(index) + L": " + candidate.description.Description +
			 L"; LUID=" + std::to_wstring(candidate.description.AdapterLuid.HighPart) + L":" +
			 std::to_wstring(candidate.description.AdapterLuid.LowPart) + L"; outputs=" +
			 std::to_wstring(candidate.outputs) + L"; viable=" + std::to_wstring(candidate.viable));
		candidates.push_back(std::move(candidate));
	}

	std::vector<std::vector<bool>> matrix(candidates.size(), std::vector<bool>(candidates.size(), false));
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
		if (SUCCEEDED(candidates[from].device->CreateTexture2D(&description, nullptr, &texture)) &&
		    SUCCEEDED(texture.As(&resource)) && SUCCEEDED(resource->GetSharedHandle(&sharedHandle))) {
			for (size_t to = 0; to < candidates.size(); ++to) {
				if (!candidates[to].viable)
					continue;
				ComPtr<ID3D11Texture2D> opened;
				matrix[from][to] = SUCCEEDED(candidates[to].device->OpenSharedResource(
					sharedHandle, IID_PPV_ARGS(&opened)));
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
		for (size_t targetIndex = 0; targetIndex < candidates.size(); ++targetIndex) {
			if (candidates[targetIndex].viable &&
			    sameAdapterIdentity(candidates[index].description, candidates[targetIndex].description)) {
				++candidates[index].relatedTotal;
				if (matrix[index][targetIndex])
					++candidates[index].relatedOpen;
			}
		}
	}

	int choice = defaultIndex >= 0 && candidates[defaultIndex].viable && candidates[defaultIndex].selfOpen
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
	eventLog(L"Selected adapter candidate " + std::to_wstring(choice) + L"; LUID=" +
		 std::to_wstring(candidates[choice].description.AdapterLuid.HighPart) + L":" +
		 std::to_wstring(candidates[choice].description.AdapterLuid.LowPart) + L"; shared-resource reach=" +
		 std::to_wstring(candidates[choice].relatedOpen) + L"/" +
		 std::to_wstring(candidates[choice].relatedTotal));
	return candidates[choice].adapter;
}

static void initialize()
{
	DXGI_SWAP_CHAIN_DESC description{};
	description.BufferDesc.Width = width;
	description.BufferDesc.Height = height;
	description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	description.SampleDesc.Count = 1;
	description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	description.BufferCount = 1;
	description.OutputWindow = previewWindowHandle;
	description.Windowed = TRUE;
	description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	ComPtr<IDXGIAdapter1> creationAdapter = chooseInteroperableAdapter();
	const D3D_FEATURE_LEVEL requestedLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
						       D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
	D3D_FEATURE_LEVEL createdLevel{};
	const UINT deviceFlags = 0;
	check(D3D11CreateDeviceAndSwapChain(creationAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, deviceFlags,
					    requestedLevels, static_cast<UINT>(std::size(requestedLevels)),
					    D3D11_SDK_VERSION, &description, &swapChain, &device, &createdLevel, &context));
	check(swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC gpu{};
	if (SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&gpu)))
		eventLog(L"GPU: " + std::wstring(gpu.Description) + L"; vendor=" + std::to_wstring(gpu.VendorId) +
			 L"; device=" + std::to_wstring(gpu.DeviceId) + L"; adapter LUID=" +
			 std::to_wstring(gpu.AdapterLuid.HighPart) + L":" + std::to_wstring(gpu.AdapterLuid.LowPart));
	if (gpu.Description[0])
		auditAdapters(gpu.AdapterLuid);
	eventLog(L"D3D11 feature level=" + hexResult(static_cast<HRESULT>(device->GetFeatureLevel())) +
		 L"; RGBA8; initial buffers=1; resized buffers=2; swap=DISCARD; device flags=0; "
		 L"Present interval=1; premultiplied output");
	const char *shader =
		"struct V {float2 p:POSITION;float2 uv:TEXCOORD0;};struct P {float4 p:SV_POSITION;float2 uv:TEXCOORD0;};P vs(V i){P o;o.p=float4(i.p,0,1);o.uv=i.uv;return o;}Texture2D img:register(t0);SamplerState smp:register(s0);float4 ps(P i):SV_TARGET{return img.Sample(smp,i.uv);}";
	ComPtr<ID3DBlob> vs, ps, errors;
	check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, &errors));
	check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, &errors));
	check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertexShader));
	check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixelShader));
	D3D11_INPUT_ELEMENT_DESC elements[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}};
	check(device->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &layout));
	D3D11_BUFFER_DESC buffer{};
	buffer.ByteWidth = sizeof(Vertex) * 6;
	buffer.Usage = D3D11_USAGE_DYNAMIC;
	buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	check(device->CreateBuffer(&buffer, nullptr, &vertices));
	D3D11_SAMPLER_DESC sample{};
	sample.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sample.MaxLOD = D3D11_FLOAT32_MAX;
	check(device->CreateSamplerState(&sample, &sampler));
	D3D11_BLEND_DESC blend{};
	auto &b = blend.RenderTarget[0];
	b.BlendEnable = TRUE;
	b.SrcBlend = D3D11_BLEND_SRC_ALPHA;
	b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	b.BlendOp = D3D11_BLEND_OP_ADD;
	b.SrcBlendAlpha = D3D11_BLEND_ONE;
	b.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	check(device->CreateBlendState(&blend, &blending));
	std::vector<unsigned char> shape(256 * 256 * 4);
	for (int y = 0; y < 256; ++y)
		for (int x = 0; x < 256; ++x) {
			int i = (y * 256 + x) * 4;
			float radius = std::sqrt(float((x - 128) * (x - 128) + (y - 128) * (y - 128)));
			shape[i] = 70;
			shape[i + 1] = 180;
			shape[i + 2] = 240;
			shape[i + 3] = static_cast<unsigned char>(std::clamp((110 - radius) / 4, 0.0f, 1.0f) * 255);
		}
	avatar = texture(shape.data(), 256, 256);
	resize();

}

static void draw(ID3D11ShaderResourceView *view, float x, float y, float w, float h)
{
	float l = 2 * x / width - 1, r = 2 * (x + w) / width - 1, t = 1 - 2 * y / height, b = 1 - 2 * (y + h) / height;
	Vertex data[] = {{l, t, 0, 0}, {r, t, 1, 0}, {l, b, 0, 1}, {l, b, 0, 1}, {r, t, 1, 0}, {r, b, 1, 1}};
	D3D11_MAPPED_SUBRESOURCE mapped{};
	check(context->Map(vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	memcpy(mapped.pData, data, sizeof(data));
	context->Unmap(vertices.Get(), 0);
	context->PSSetShaderResources(0, 1, &view);
	context->Draw(6, 0);
}

static void launchCrossProcessReceiver()
{
	ComPtr<IDXGIDevice> dxgiDevice;
	ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC adapterDescription{};
	check(device.As(&dxgiDevice));
	check(dxgiDevice->GetAdapter(adapter.GetAddressOf()));
	check(adapter->GetDesc(&adapterDescription));
	const DWORD parentPid = GetCurrentProcessId();
	const auto mappingName = L"RearSilverAvatar_ReceiverResult_" + std::to_wstring(parentPid);
	receiverResultMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
						      sizeof(CrossProcessResult), mappingName.c_str());
	if (!receiverResultMapping) {
		eventLog(L"Cross-process receiver result mapping creation failed: " + windowsError(GetLastError()));
		return;
	}
	receiverResult = static_cast<CrossProcessResult *>(
		MapViewOfFile(receiverResultMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CrossProcessResult)));
	if (!receiverResult) {
		eventLog(L"Cross-process receiver result view failed: " + windowsError(GetLastError()));
		CloseHandle(receiverResultMapping);
		receiverResultMapping = nullptr;
		return;
	}
	ZeroMemory(receiverResult, sizeof(*receiverResult));
	wchar_t executable[32768]{};
	DWORD executableLength = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
	if (!executableLength || executableLength >= std::size(executable)) {
		eventLog(L"Cross-process receiver executable lookup failed: " + windowsError(GetLastError()));
		return;
	}
	std::wstring command = L"\"" + std::wstring(executable, executableLength) + L"\" --receiver " +
			       std::to_wstring(parentPid) + L" " +
			       std::to_wstring(adapterDescription.AdapterLuid.LowPart) + L" " +
			       std::to_wstring(adapterDescription.AdapterLuid.HighPart);
	std::vector<wchar_t> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back(L'\0');
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
			    &startup, &process)) {
		eventLog(L"Cross-process receiver launch failed: " + windowsError(GetLastError()));
		return;
	}
	CloseHandle(process.hThread);
	receiverProcess = process.hProcess;
	eventLog(L"Cross-process receiver launched; process=" + std::to_wstring(process.dwProcessId) +
		 L"; adapter LUID=" + std::to_wstring(adapterDescription.AdapterLuid.HighPart) + L":" +
		 std::to_wstring(adapterDescription.AdapterLuid.LowPart));
}

static void pollCrossProcessReceiver()
{
	if (!receiverResult)
		return;
	const LONG sequence = InterlockedCompareExchange(&receiverResult->sequence, 0, 0);
	if (!sequence || sequence == lastReceiverSequence)
		return;
	lastReceiverSequence = sequence;
	const HRESULT result = static_cast<HRESULT>(receiverResult->openResult);
	if (FAILED(result)) {
		eventLog(L"Cross-process receiver result: map id=" + std::to_wstring(receiverResult->mapId) +
			 L"; OpenSharedResource failed with " + hexResult(result));
	} else {
		eventLog(L"Cross-process receiver result: map id=" + std::to_wstring(receiverResult->mapId) +
			 L"; OpenSharedResource succeeded; size=" + std::to_wstring(receiverResult->width) + L"x" +
			 std::to_wstring(receiverResult->height) + L"; format=" +
			 std::to_wstring(receiverResult->format));
	}
}

static void render()
{
	ULONGLONG now = GetTickCount64();
	float clear[4] = {0, 0, 0, 0};
	if (background.load() == 1) {
		clear[1] = 1;
		clear[3] = 1;
	}
	if (background.load() == 2) {
		clear[0] = clear[1] = clear[2] = 0.12f;
		clear[3] = 1;
	}
	ComPtr<ID3D11ShaderResourceView> frameAvatar;
	UINT frameImageWidth = 1, frameImageHeight = 1;
	{
		std::lock_guard<std::mutex> lock(avatarMutex);
		frameAvatar = avatar;
		frameImageWidth = imageWidth;
		frameImageHeight = imageHeight;
	}
	float scale = std::min(width * 0.65f / frameImageWidth, height * 0.65f / frameImageHeight);
	float w = frameImageWidth * scale, h = frameImageHeight * scale;
	float motion = animate.load() ? std::sin(GetTickCount64() / 500.0f) * 12 : 0;
	auto drawSurface = [&](ID3D11RenderTargetView *surface, const D3D11_VIEWPORT &viewport,
			       const float surfaceClear[4]) {
		context->ClearRenderTargetView(surface, surfaceClear);
		context->OMSetRenderTargets(1, &surface, nullptr);
		context->RSSetViewports(1, &viewport);
		UINT stride = sizeof(Vertex), offset = 0;
		ID3D11Buffer *vb = vertices.Get();
		context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		context->IASetInputLayout(layout.Get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vertexShader.Get(), nullptr, 0);
		context->PSSetShader(pixelShader.Get(), nullptr, 0);
		ID3D11SamplerState *smp = sampler.Get();
		context->PSSetSamplers(0, 1, &smp);
		context->OMSetBlendState(blending.Get(), nullptr, 0xffffffff);
		draw(frameAvatar.Get(), (width - w) / 2, (height - h) / 2 + motion, w, h);
	};
	D3D11_VIEWPORT captureViewport{0, 0, float(width), float(height), 0, 1};
	drawSurface(target.Get(), captureViewport, clear);
	HRESULT present = swapChain->Present(1, 0);
	++frames;
	if (present != lastPresent) {
		eventLog(L"Present result changed: " + hexResult(present));
		lastPresent = present;
	}
	if (FAILED(present))
		eventLog(L"Device removal reason: " + hexResult(device->GetDeviceRemovedReason()));
	check(present);
	now = GetTickCount64();
	if (now - lastReport >= 2000) {
		eventLog(L"Render progress: frame=" + std::to_wstring(frames) + L"; frames since report=" +
			 std::to_wstring(frames - lastFrames) + L"; interval ms=" +
			 std::to_wstring(lastReport ? now - lastReport : 0) + L"; focus=" + std::to_wstring(focused.load()) +
			 L"; minimized=" + std::to_wstring(IsIconic(windowHandle) != 0) + L"; controls=" +
			 std::to_wstring((focused.load() || dialogOpen.load()) && menuVisible.load()) + L"; motion=" +
			 std::to_wstring(animate.load()) + L"; background=" + std::to_wstring(background.load()) + L"; Present=" +
			 hexResult(present) + L"; graphics-hook64 loaded=" +
			 std::to_wstring(GetModuleHandleW(L"graphics-hook64.dll") != nullptr));
		lastFrames = frames;
		lastReport = now;
	}
}

static bool isProcessRunning(const wchar_t *name)
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

static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
	switch (message) {
	case WM_ACTIVATEAPP:
		focused.store(w != 0);
		if (!dialogOpen.load())
			menuVisible.store(focused.load());
		eventLog(L"Application focus=" + std::to_wstring(focused.load()));
		return 0;
	case WM_ENTERSIZEMOVE:
		eventLog(L"Window move/resize started; modal message loop may pause rendering");
		return 0;
	case WM_EXITSIZEMOVE:
		eventLog(L"Window move/resize ended");
		return 0;
	case WM_SIZE:
		if (w != SIZE_MINIMIZED) {
			const int clientWidth = LOWORD(l), clientHeight = HIWORD(l);
			const int margin = 16, gap = 10, buttonWidth = 145, buttonHeight = 34;
			MoveWindow(GetDlgItem(hwnd, ID_LOAD_PNG), margin, 14, buttonWidth, buttonHeight, TRUE);
			MoveWindow(GetDlgItem(hwnd, ID_BACKGROUND), margin + buttonWidth + gap, 14, buttonWidth,
				   buttonHeight, TRUE);
			MoveWindow(GetDlgItem(hwnd, ID_TOGGLE_MOTION), margin + (buttonWidth + gap) * 2, 14,
				   buttonWidth, buttonHeight, TRUE);
			MoveWindow(GetDlgItem(hwnd, ID_COPY_LOG), margin + (buttonWidth + gap) * 3, 14, buttonWidth,
				   buttonHeight, TRUE);
			MoveWindow(GetDlgItem(hwnd, ID_STATUS), margin, 56, std::max(200, clientWidth - margin * 2), 24,
				   TRUE);
			if (previewWindowHandle) {
				const int availableWidth = std::max(1, clientWidth - margin * 2);
				const int availableHeight = std::max(1, clientHeight - 88 - margin);
				int renderWidth = availableWidth;
				int renderHeight = renderWidth * height / width;
				if (renderHeight > availableHeight) {
					renderHeight = availableHeight;
					renderWidth = renderHeight * width / height;
				}
				const int renderX = margin + (availableWidth - renderWidth) / 2;
				const int renderY = 88 + (availableHeight - renderHeight) / 2;
				MoveWindow(previewWindowHandle, renderX, renderY, std::max(1, renderWidth),
					   std::max(1, renderHeight), TRUE);
			}
		}
		return 0;
	case WM_GETMINMAXINFO:
		reinterpret_cast<MINMAXINFO *>(l)->ptMinTrackSize = {710, 360};
		return 0;
	case WM_KEYDOWN:
		if (w == 'O' && (GetKeyState(VK_CONTROL) & 0x8000))
			loadImage();
		if (w == 'L' && (GetKeyState(VK_CONTROL) & 0x8000))
			copyLog();
		return 0;
	case WM_COMMAND: {
		const int command = LOWORD(w);
		if (command == ID_LOAD_PNG)
			loadImage();
		else if (command == ID_BACKGROUND)
			background.store((background.load() + 1) % 3);
		else if (command == ID_TOGGLE_MOTION)
			animate.store(!animate.load());
		else if (command == ID_COPY_LOG)
			copyLog();
		const wchar_t *modes[] = {L"Transparent", L"Green chroma key", L"Solid dark"};
		std::wstring status = L"Capture: 960×720 | Background: " + std::wstring(modes[background.load()]) +
				      L" | Motion: " + (animate.load() ? L"On" : L"Off");
		SetWindowTextW(GetDlgItem(hwnd, ID_STATUS), status.c_str());
		return 0;
	}
	case WM_DESTROY:
		eventLog(L"Window destroyed");
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, message, w, l);
}

static LRESULT CALLBACK previewProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
	return DefWindowProcW(hwnd, message, w, l);
}

static int runCrossProcessReceiver(DWORD parentPid, DWORD luidLow, LONG luidHigh)
{
	const auto resultName = L"RearSilverAvatar_ReceiverResult_" + std::to_wstring(parentPid);
	HANDLE resultMapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, resultName.c_str());
	if (!resultMapping)
		return 2;
	auto *result = static_cast<CrossProcessResult *>(
		MapViewOfFile(resultMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CrossProcessResult)));
	if (!result) {
		CloseHandle(resultMapping);
		return 3;
	}
	HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
	if (!parent) {
		UnmapViewOfFile(result);
		CloseHandle(resultMapping);
		return 4;
	}
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		CloseHandle(parent);
		UnmapViewOfFile(result);
		CloseHandle(resultMapping);
		return 5;
	}
	ComPtr<IDXGIAdapter1> selectedAdapter;
	for (UINT index = 0;; ++index) {
		ComPtr<IDXGIAdapter1> candidate;
		if (factory->EnumAdapters1(index, candidate.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 description{};
		if (SUCCEEDED(candidate->GetDesc1(&description)) &&
		    description.AdapterLuid.LowPart == luidLow && description.AdapterLuid.HighPart == luidHigh) {
			selectedAdapter = candidate;
			break;
		}
	}
	ComPtr<ID3D11Device> receiverDevice;
	ComPtr<ID3D11DeviceContext> receiverContext;
	D3D_FEATURE_LEVEL featureLevel{};
	if (!selectedAdapter ||
	    FAILED(D3D11CreateDevice(selectedAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
				     D3D11_SDK_VERSION, receiverDevice.GetAddressOf(), &featureLevel,
				     receiverContext.GetAddressOf()))) {
		CloseHandle(parent);
		UnmapViewOfFile(result);
		CloseHandle(resultMapping);
		return 6;
	}
	DWORD lastMapId = 0;
	while (WaitForSingleObject(parent, 0) == WAIT_TIMEOUT) {
		const auto hookInfoName = L"CaptureHook_HookInfo" + std::to_wstring(parentPid);
		HANDLE hookMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, hookInfoName.c_str());
		if (!hookMapping) {
			Sleep(1);
			continue;
		}
		const void *hookView = MapViewOfFile(hookMapping, FILE_MAP_READ, 0, 0, 648);
		unsigned char snapshot[648]{};
		if (hookView) {
			memcpy(snapshot, hookView, sizeof(snapshot));
			UnmapViewOfFile(hookView);
		}
		CloseHandle(hookMapping);
		if (!hookView) {
			Sleep(1);
			continue;
		}
		auto u32 = [&](size_t offset) {
			DWORD value = 0;
			memcpy(&value, snapshot + offset, sizeof(value));
			return value;
		};
		const DWORD captureType = u32(8);
		const DWORD capturedWindow = u32(12);
		const DWORD mapId = u32(40);
		if (captureType != 1 || !capturedWindow || !mapId || mapId == lastMapId) {
			Sleep(1);
			continue;
		}
		const auto textureName = L"CaptureHook_Texture_" + std::to_wstring(capturedWindow) + L"_" +
					 std::to_wstring(mapId);
		HANDLE textureMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, textureName.c_str());
		if (!textureMapping) {
			Sleep(1);
			continue;
		}
		const void *textureView = MapViewOfFile(textureMapping, FILE_MAP_READ, 0, 0, sizeof(DWORD));
		DWORD handleValue = 0;
		if (textureView) {
			memcpy(&handleValue, textureView, sizeof(handleValue));
			UnmapViewOfFile(textureView);
		}
		CloseHandle(textureMapping);
		if (!handleValue) {
			Sleep(1);
			continue;
		}
		lastMapId = mapId;
		ComPtr<ID3D11Texture2D> openedTexture;
		const HRESULT openResult = receiverDevice->OpenSharedResource(
			reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handleValue)), IID_PPV_ARGS(&openedTexture));
		D3D11_TEXTURE2D_DESC textureDescription{};
		if (SUCCEEDED(openResult) && openedTexture)
			openedTexture->GetDesc(&textureDescription);
		result->mapId = mapId;
		result->openResult = static_cast<LONG>(openResult);
		result->width = textureDescription.Width;
		result->height = textureDescription.Height;
		result->format = static_cast<DWORD>(textureDescription.Format);
		MemoryBarrier();
		InterlockedIncrement(&result->sequence);
		Sleep(1);
	}
	CloseHandle(parent);
	UnmapViewOfFile(result);
	CloseHandle(resultMapping);
	return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
	startLog();
	HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(com))
		return 1;
	int result = 0;
	try {
		WNDCLASSW wc{};
		wc.hInstance = instance;
		wc.lpfnWndProc = windowProc;
		wc.lpszClassName = L"RearSilverAvatarCapturePrototype";
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		if (!RegisterClassW(&wc))
			throw HRESULT_FROM_WIN32(GetLastError());
		WNDCLASSW previewClass = wc;
		previewClass.lpfnWndProc = previewProc;
		previewClass.lpszClassName = L"RearSilverAvatarPreview";
		previewClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		if (!RegisterClassW(&previewClass))
			throw HRESULT_FROM_WIN32(GetLastError());
		windowHandle = CreateWindowW(wc.lpszClassName,
					     L"RearSilver Avatar",
					     WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 850, nullptr,
					     nullptr, instance, nullptr);
		if (!windowHandle)
			throw HRESULT_FROM_WIN32(GetLastError());
		CreateWindowW(L"BUTTON", L"Load PNG", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
			      windowHandle, reinterpret_cast<HMENU>(ID_LOAD_PNG), instance, nullptr);
		CreateWindowW(L"BUTTON", L"Background", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
			      windowHandle, reinterpret_cast<HMENU>(ID_BACKGROUND), instance, nullptr);
		CreateWindowW(L"BUTTON", L"Toggle motion", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
			      windowHandle, reinterpret_cast<HMENU>(ID_TOGGLE_MOTION), instance, nullptr);
		CreateWindowW(L"BUTTON", L"Copy event log", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
			      windowHandle, reinterpret_cast<HMENU>(ID_COPY_LOG), instance, nullptr);
		CreateWindowW(L"STATIC", L"Capture: 960×720 | Background: Transparent | Motion: On",
			      WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, windowHandle,
			      reinterpret_cast<HMENU>(ID_STATUS), instance, nullptr);
		previewWindowHandle = CreateWindowW(previewClass.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 16, 88,
						  1048, 720, windowHandle, nullptr, instance, nullptr);
		if (!previewWindowHandle)
			throw HRESULT_FROM_WIN32(GetLastError());
		if (isProcessRunning(L"RTSS.exe"))
			MessageBoxW(windowHandle,
				    L"RivaTuner Statistics Server is running. If OBS Game Capture is blank or frozen, "
				    L"open RTSS Setup and enable ‘Use Microsoft Detours API hooking’.",
				    L"RearSilver Avatar – OBS compatibility", MB_OK | MB_ICONINFORMATION);
		initialize();
		ShowWindow(windowHandle, show);
		renderRunning.store(true);
		std::thread renderThread([] {
			try {
				while (renderRunning.load()) {
					render();
					if (IsIconic(windowHandle))
						Sleep(16);
				}
			} catch (...) {
				renderRunning.store(false);
				PostMessageW(windowHandle, WM_CLOSE, 0, 0);
			}
		});
		bool running = true;
		while (running) {
			MSG msg{};
			const BOOL messageResult = GetMessageW(&msg, nullptr, 0, 0);
			if (messageResult <= 0) {
				running = false;
				break;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		renderRunning.store(false);
		renderThread.join();
	} catch (HRESULT error) {
		wchar_t text[200];
		swprintf_s(text,
			   L"The capture prototype encountered a graphics error (0x%08X). Please report this code.",
			   static_cast<unsigned>(error));
		MessageBoxW(nullptr, text, L"RearSilver capture prototype", MB_OK | MB_ICONERROR);
		result = 1;
	} catch (...) {
		MessageBoxW(
			nullptr,
			L"The prototype could not continue. Please report what happened immediately before this message.",
			L"RearSilver capture prototype", MB_OK | MB_ICONERROR);
		result = 1;
	}
	if (windowHandle && IsWindow(windowHandle))
		DestroyWindow(windowHandle);
	if (receiverProcess)
		CloseHandle(receiverProcess);
	if (receiverResult)
		UnmapViewOfFile(receiverResult);
	if (receiverResultMapping)
		CloseHandle(receiverResultMapping);
	eventLog(L"Application exit=" + std::to_wstring(result));
	if (logFile != INVALID_HANDLE_VALUE) {
		FlushFileBuffers(logFile);
		CloseHandle(logFile);
	}
	CoUninitialize();
	return result;
}
