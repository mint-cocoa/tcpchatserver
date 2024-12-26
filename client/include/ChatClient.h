#pragma once
#include "Context.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>

class ChatClient {
public:
    ChatClient();
    ~ChatClient();

    bool connect(const std::string& host, int port);
    void disconnect();
    
    // 기본 기능
    bool joinSession(int32_t sessionId);
    bool leaveSession();
    bool sendChat(const std::string& message);
    
    // 콜백 설정
    using MessageCallback = std::function<void(const std::string&)>;
    using NotificationCallback = std::function<void(const std::string&)>;
    
    void setMessageCallback(MessageCallback callback) { messageCallback_ = callback; }
    void setNotificationCallback(NotificationCallback callback) { notificationCallback_ = callback; }

private:
    int socket_;
    std::atomic<bool> running_;
    std::thread receiveThread_;
    
    MessageCallback messageCallback_;
    NotificationCallback notificationCallback_;
    
    void receiveLoop();
    bool sendMessage(MessageType type, const void* data, size_t length);
    void handleMessage(const ChatMessage& message);
}; 