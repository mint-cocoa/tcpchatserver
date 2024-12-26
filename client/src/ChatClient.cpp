#include "ChatClient.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

ChatClient::ChatClient() : socket_(-1), running_(false) {}

ChatClient::~ChatClient() {
    disconnect();
}

bool ChatClient::connect(const std::string& host, int port) {
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        if (notificationCallback_) notificationCallback_("소켓 생성 실패");
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) <= 0) {
        if (notificationCallback_) notificationCallback_("잘못된 IP 주소");
        close(socket_);
        return false;
    }

    if (::connect(socket_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        if (notificationCallback_) notificationCallback_("서버 연결 실패");
        close(socket_);
        return false;
    }

    running_ = true;
    receiveThread_ = std::thread(&ChatClient::receiveLoop, this);
    
    if (notificationCallback_) notificationCallback_("서버에 연결되었습니다");
    return true;
}

void ChatClient::disconnect() {
    if (socket_ >= 0) {
        running_ = false;
        close(socket_);
        if (receiveThread_.joinable()) {
            receiveThread_.join();
        }
        socket_ = -1;
        if (notificationCallback_) notificationCallback_("서버와의 연결이 종료되었습니다");
    }
}

bool ChatClient::joinSession(int32_t sessionId) {
    return sendMessage(MessageType::CLIENT_JOIN, &sessionId, sizeof(sessionId));
}

bool ChatClient::leaveSession() {
    return sendMessage(MessageType::CLIENT_LEAVE, nullptr, 0);
}

bool ChatClient::sendChat(const std::string& message) {
    return sendMessage(MessageType::CLIENT_CHAT, message.c_str(), message.length());
}

void ChatClient::receiveLoop() {
    ChatMessage message;
    while (running_) {
        ssize_t bytesRead = recv(socket_, &message, sizeof(message), 0);
        if (bytesRead <= 0) {
            if (running_ && notificationCallback_) {
                notificationCallback_("서버와의 연결이 끊어졌습니다");
            }
            break;
        }
        
        if (bytesRead >= sizeof(ChatMessage)) {
            handleMessage(message);
        }
    }
    running_ = false;
}

bool ChatClient::sendMessage(MessageType type, const void* data, size_t length) {
    if (socket_ < 0 || !running_) {
        if (notificationCallback_) notificationCallback_("서버에 연결되어 있지 않습니다");
        return false;
    }

    ChatMessage message{};
    message.type = type;
    message.length = static_cast<uint16_t>(length);
    
    if (data && length > 0) {
        if (length > sizeof(message.data)) {
            if (notificationCallback_) notificationCallback_("메시지가 너무 깁니다");
            return false;
        }
        std::memcpy(message.data, data, length);
    }

    ssize_t bytesSent = send(socket_, &message, sizeof(message), 0);
    if (bytesSent != sizeof(message)) {
        if (notificationCallback_) notificationCallback_("메시지 전송 실패");
        return false;
    }
    
    return true;
}

void ChatClient::handleMessage(const ChatMessage& message) {
    std::string messageData(message.data, message.length);
    
    switch (message.type) {
        case MessageType::SERVER_CHAT:
            if (messageCallback_) messageCallback_(messageData);
            break;
            
        case MessageType::SERVER_NOTIFICATION:
        case MessageType::SERVER_ACK:
            if (notificationCallback_) notificationCallback_(messageData);
            break;
            
        default:
            if (notificationCallback_) notificationCallback_("알 수 없는 메시지 타입");
            break;
    }
}