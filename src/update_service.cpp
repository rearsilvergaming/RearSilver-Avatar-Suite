#include <windows.h>
#include <winhttp.h>

#include "update_service.hpp"

#include <cctype>
#include <memory>

namespace {
constexpr size_t kMaximumManifestBytes = 128 * 1024;
using WinHttpHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

std::string channelSlug(const std::string &channel)
{
    std::string slug;
    for (const unsigned char character : channel) {
        if (std::isalnum(character)) slug.push_back(static_cast<char>(std::tolower(character)));
        else if (!slug.empty() && slug.back() != '-') slug.push_back('-');
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug;
}

bool crackHttpsUrl(const std::string &url, std::wstring &host, INTERNET_PORT &port,
                   std::wstring &path)
{
    if (url.empty()) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
                                          static_cast<int>(url.size()), nullptr, 0);
    if (count <= 0) return false;
    std::wstring wide(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
                             static_cast<int>(url.size()), wide.data(), count)) return false;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwExtraInfoLength) return false;
    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    port = parts.nPort;
    while (path.size() > 1 && path.back() == L'/') path.pop_back();
    return !host.empty();
}
}

UpdateFetchResult fetchUpdateManifest(const std::string &baseUrl,
                                      const std::string &channel,
                                      bool manual)
{
    UpdateFetchResult result;
    result.manual = manual;
    std::wstring host, path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!crackHttpsUrl(baseUrl, host, port, path)) {
        result.error = "The update service address is invalid.";
        return result;
    }
    if (path.empty()) path = L"/";
    if (path.back() != L'/') path.push_back(L'/');
    const std::string slug = channelSlug(channel);
    path += L"v1/updates/";
    path.append(slug.begin(), slug.end());
    path += L"/windows-x64";

    WinHttpHandle session(WinHttpOpen(L"RearSilver-Avatar-Suite-Update/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
    if (!session) { result.error = "Windows could not initialise the update connection."; return result; }
    WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 15000);
    WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), port, 0), WinHttpCloseHandle);
    if (!connection) { result.error = "The update service could not be reached."; return result; }
    WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE), WinHttpCloseHandle);
    if (!request || !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.get(), nullptr)) {
        result.error = "The update service did not respond.";
        return result;
    }
    DWORD status = 0, bytes = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (status == 503) {
        result.error = "No release has been published to this update channel yet.";
        return result;
    }
    if (status < 200 || status >= 300) {
        result.error = "The update service returned an error.";
        return result;
    }
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            result.error = "The update response could not be read.";
            return result;
        }
        if (!available) break;
        if (result.body.size() + available > kMaximumManifestBytes) {
            result.error = "The update response was unexpectedly large.";
            result.body.clear();
            return result;
        }
        const size_t offset = result.body.size();
        result.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), result.body.data() + offset, available, &read)) {
            result.error = "The update response could not be read.";
            result.body.clear();
            return result;
        }
        result.body.resize(offset + read);
    }
    if (result.body.empty()) {
        result.error = "The update service returned an empty response.";
        return result;
    }
    result.succeeded = true;
    return result;
}
