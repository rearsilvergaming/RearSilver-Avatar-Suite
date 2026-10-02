#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include "websocket_server.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
std::string base64(const unsigned char *data, size_t size)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const unsigned value = (static_cast<unsigned>(data[i]) << 16) |
            (i + 1 < size ? static_cast<unsigned>(data[i + 1]) << 8 : 0) |
            (i + 2 < size ? static_cast<unsigned>(data[i + 2]) : 0);
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(i + 1 < size ? alphabet[(value >> 6) & 63] : '=');
        result.push_back(i + 2 < size ? alphabet[value & 63] : '=');
    }
    return result;
}

std::string websocketAccept(const std::string &key)
{
    const std::string source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 20> digest{};
    DWORD objectSize = 0, returned = 0;
    std::vector<unsigned char> object;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0)
        return {};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0) >= 0) {
        object.resize(objectSize);
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0) {
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(source.data())),
                           static_cast<ULONG>(source.size()), 0);
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        }
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return base64(digest.data(), digest.size());
}

bool sendAll(SOCKET socket, const char *data, size_t size)
{
    while (size) {
        const int sent = send(socket, data, static_cast<int>(size), 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool sendFrame(SOCKET socket, unsigned char opcode, const std::string &payload)
{
    std::vector<unsigned char> frame{static_cast<unsigned char>(0x80 | opcode)};
    if (payload.size() <= 125) frame.push_back(static_cast<unsigned char>(payload.size()));
    else if (payload.size() <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<unsigned char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<unsigned char>(payload.size() & 0xff));
    } else return false;
    frame.insert(frame.end(), payload.begin(), payload.end());
    return sendAll(socket, reinterpret_cast<const char *>(frame.data()), frame.size());
}

std::string headerValue(const std::string &request, const std::string &name)
{
    auto lower = [](std::string value) {
        for (char &character : value)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return value;
    };
    const std::string wanted = lower(name);
    size_t lineStart = request.find("\r\n");
    if (lineStart == std::string::npos) return {};
    lineStart += 2;
    while (lineStart < request.size()) {
        const size_t lineEnd = request.find("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd == lineStart) break;
        const size_t separator = request.find(':', lineStart);
        if (separator != std::string::npos && separator < lineEnd &&
            lower(request.substr(lineStart, separator - lineStart)) == wanted) {
            size_t valueStart = separator + 1;
            while (valueStart < lineEnd && (request[valueStart] == ' ' || request[valueStart] == '\t'))
                ++valueStart;
            size_t valueEnd = lineEnd;
            while (valueEnd > valueStart && (request[valueEnd - 1] == ' ' || request[valueEnd - 1] == '\t'))
                --valueEnd;
            return request.substr(valueStart, valueEnd - valueStart);
        }
        lineStart = lineEnd + 2;
    }
    return {};
}

bool allowedOrigin(const std::string &origin)
{
    if (origin.empty() || origin == "null" || origin == "https://app.rearsilver-avatar.test")
        return true;
    return origin.rfind("http://127.0.0.1", 0) == 0 ||
           origin.rfind("https://127.0.0.1", 0) == 0 ||
           origin.rfind("http://localhost", 0) == 0 ||
           origin.rfind("https://localhost", 0) == 0;
}
}

struct WebSocketServer::Impl {
    std::atomic<bool> active{false};
    unsigned short boundPort = 0;
    SOCKET listener = INVALID_SOCKET;
    Handler handler;
    std::thread thread;
    std::atomic<unsigned> clients{0};
    std::atomic<unsigned long long> messages{0};
    std::mutex clientMutex;
    std::mutex sendMutex;
    std::vector<SOCKET> clientSockets;
    std::vector<std::thread> clientThreads;

    void client(SOCKET socket)
    {
        std::string request;
        std::array<char, 2048> buffer{};
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0) { closesocket(socket); return; }
            request.append(buffer.data(), received);
        }
        const std::string key = headerValue(request, "Sec-WebSocket-Key");
        const std::string origin = headerValue(request, "Origin");
        const std::string accept = websocketAccept(key);
        if (key.empty() || accept.empty() || !allowedOrigin(origin)) {
            static constexpr char denied[] = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
            sendAll(socket, denied, sizeof(denied) - 1);
            closesocket(socket);
            return;
        }
        const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (!sendAll(socket, response.data(), response.size())) { closesocket(socket); return; }
        clients.fetch_add(1);

        while (active.load()) {
            unsigned char head[2]{};
            if (recv(socket, reinterpret_cast<char *>(head), 2, MSG_WAITALL) != 2) break;
            const unsigned char opcode = head[0] & 0x0f;
            uint64_t length = head[1] & 0x7f;
            if (length == 126) {
                unsigned char bytes[2]{};
                if (recv(socket, reinterpret_cast<char *>(bytes), 2, MSG_WAITALL) != 2) break;
                length = (static_cast<uint64_t>(bytes[0]) << 8) | bytes[1];
            } else if (length == 127) break;
            if (length > 65535) break;
            unsigned char mask[4]{};
            if ((head[1] & 0x80) == 0 || recv(socket, reinterpret_cast<char *>(mask), 4, MSG_WAITALL) != 4) break;
            std::string payload(static_cast<size_t>(length), '\0');
            if (length && recv(socket, payload.data(), static_cast<int>(length), MSG_WAITALL) != static_cast<int>(length)) break;
            for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= static_cast<char>(mask[i % 4]);
            if (opcode == 8) break;
            if (opcode == 9) {
                std::lock_guard<std::mutex> sendLock(sendMutex);
                if (!sendFrame(socket, 10, payload)) break;
                continue;
            }
            if (opcode != 1) continue;
            messages.fetch_add(1);
            const std::string reply = handler ? handler(payload) : "{\"ok\":false,\"code\":\"SERVER_UNAVAILABLE\",\"error\":\"Server unavailable\"}";
            {
                std::lock_guard<std::mutex> sendLock(sendMutex);
                if (!sendFrame(socket, 1, reply)) break;
            }
        }
        clients.fetch_sub(1);
        closesocket(socket);
        std::lock_guard<std::mutex> lock(clientMutex);
        for (SOCKET &entry : clientSockets) if (entry == socket) { entry = INVALID_SOCKET; break; }
    }

    void run()
    {
        while (active.load()) {
            SOCKET socket = accept(listener, nullptr, nullptr);
            if (socket == INVALID_SOCKET) break;
            std::lock_guard<std::mutex> lock(clientMutex);
            clientSockets.push_back(socket);
            clientThreads.emplace_back([this, socket] { client(socket); });
        }
    }
};

WebSocketServer::WebSocketServer() : impl_(std::make_unique<Impl>()) {}
WebSocketServer::~WebSocketServer() { stop(); }

bool WebSocketServer::start(unsigned short port, Handler handler)
{
    if (impl_->active.load()) return true;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    impl_->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listener == INVALID_SOCKET) { WSACleanup(); return false; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    InetPtonW(AF_INET, L"127.0.0.1", &address.sin_addr);
    int exclusive = 1;
    setsockopt(impl_->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
    if (bind(impl_->listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(impl_->listener, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(impl_->listener); impl_->listener = INVALID_SOCKET; WSACleanup(); return false;
    }
    impl_->boundPort = port;
    impl_->handler = std::move(handler);
    impl_->active.store(true);
    impl_->thread = std::thread([this] { impl_->run(); });
    return true;
}

void WebSocketServer::stop()
{
    if (!impl_->active.exchange(false)) return;
    if (impl_->listener != INVALID_SOCKET) {
        shutdown(impl_->listener, SD_BOTH);
        closesocket(impl_->listener);
        impl_->listener = INVALID_SOCKET;
    }
    if (impl_->thread.joinable()) impl_->thread.join();
    {
        std::lock_guard<std::mutex> lock(impl_->clientMutex);
        for (SOCKET socket : impl_->clientSockets) if (socket != INVALID_SOCKET) {
            shutdown(socket, SD_BOTH);
        }
    }
    for (std::thread &thread : impl_->clientThreads) if (thread.joinable()) thread.join();
    impl_->clientThreads.clear();
    impl_->clientSockets.clear();
    WSACleanup();
}

bool WebSocketServer::running() const { return impl_->active.load(); }
unsigned short WebSocketServer::port() const { return impl_->boundPort; }
unsigned WebSocketServer::connectedClients() const { return impl_->clients.load(); }
unsigned long long WebSocketServer::messagesReceived() const { return impl_->messages.load(); }

void WebSocketServer::broadcastText(const std::string &payload)
{
    if (!impl_->active.load()) return;
    std::lock_guard<std::mutex> clientsLock(impl_->clientMutex);
    std::lock_guard<std::mutex> sendLock(impl_->sendMutex);
    for (SOCKET socket : impl_->clientSockets)
        if (socket != INVALID_SOCKET) sendFrame(socket, 1, payload);
}
