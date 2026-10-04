#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle() = default;
    InternetHandle(const InternetHandle &) = delete;
    InternetHandle &operator=(const InternetHandle &) = delete;
};

bool decodeBase64(const std::wstring &encoded, std::string &decoded)
{
    DWORD size = 0;
    if (!CryptStringToBinaryW(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr))
        return false;
    std::vector<unsigned char> bytes(size);
    if (!CryptStringToBinaryW(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64, bytes.data(), &size, nullptr, nullptr))
        return false;
    decoded.assign(reinterpret_cast<const char *>(bytes.data()), size);
    return true;
}

int fail(const char *message)
{
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc != 3 || std::wstring(argv[1]) != L"--base64")
        return fail("Usage: RearSilver Avatar Suite Automation.exe --base64 <UTF-8 JSON command>");

    std::string command;
    if (!decodeBase64(argv[2], command) || command.empty())
        return fail("The Avatar Suite command is not valid Base64 data.");
    if (command.size() > 65535)
        return fail("The Avatar Suite command exceeds the 65,535-byte limit.");

    InternetHandle session;
    session.value = WinHttpOpen(L"RearSilver Avatar Suite Automation/1.0",
                                WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) return fail("Could not initialise the local WebSocket client.");

    InternetHandle connection;
    connection.value = WinHttpConnect(session.value, L"127.0.0.1", 17891, 0);
    if (!connection.value) return fail("Could not connect to the Avatar Suite automation port.");

    InternetHandle request;
    request.value = WinHttpOpenRequest(connection.value, L"GET", L"/", nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request.value) return fail("Could not create the Avatar Suite WebSocket request.");
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        return fail("Could not request a WebSocket connection to Avatar Suite.");
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr))
        return fail("Avatar Suite is not accepting local automation connections.");

    InternetHandle socket;
    socket.value = WinHttpWebSocketCompleteUpgrade(request.value, 0);
    if (!socket.value) return fail("Avatar Suite did not complete the WebSocket connection.");
    WinHttpCloseHandle(request.value);
    request.value = nullptr;

    const DWORD sendResult = WinHttpWebSocketSend(
        socket.value, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char *>(command.data()), static_cast<DWORD>(command.size()));
    if (sendResult != NO_ERROR) return fail("The command could not be sent to Avatar Suite.");

    std::string response;
    std::vector<char> buffer(8192);
    for (;;) {
        DWORD received = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE;
        const DWORD receiveResult = WinHttpWebSocketReceive(
            socket.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received, &type);
        if (receiveResult != NO_ERROR) return fail("Avatar Suite did not return an automation response.");
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            return fail("Avatar Suite closed the connection before returning a response.");
        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
            type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            return fail("Avatar Suite returned an unsupported automation response.");
        response.append(buffer.data(), received);
        if (response.size() > 65535) return fail("Avatar Suite returned an oversized automation response.");
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) break;
    }

    WinHttpWebSocketClose(socket.value, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                          nullptr, 0);
    std::cout << response << '\n';
    return response.find("\"ok\":true") != std::string::npos ? 0 : 2;
}
