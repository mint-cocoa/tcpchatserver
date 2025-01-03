#pragma once
#include <string>
#include <thread>
#include <atomic>

class ChatClient {
public:
    ChatClient();
    ~ChatClient();

    bool connect(const char* host, int port);
    void disconnect();
    void sendMessage(const std::string& message);

private:
    void receiveLoop();
    void handleMessage(const char* message, size_t length);

    int socket_fd_;
    std::thread receiveThread_;
    std::atomic<bool> should_stop_;
}; 