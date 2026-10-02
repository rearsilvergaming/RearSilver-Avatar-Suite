#pragma once

#include <functional>
#include <memory>
#include <string>

class WebSocketServer {
public:
    using Handler = std::function<std::string(const std::string &)>;
    WebSocketServer();
    ~WebSocketServer();
    WebSocketServer(const WebSocketServer &) = delete;
    WebSocketServer &operator=(const WebSocketServer &) = delete;

    bool start(unsigned short port, Handler handler);
    void stop();
    bool running() const;
    unsigned short port() const;
    unsigned connectedClients() const;
    unsigned long long messagesReceived() const;
    void broadcastText(const std::string &payload);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
