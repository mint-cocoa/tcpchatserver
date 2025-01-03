#include "ChatClient.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <sys/select.h>

ChatClient::ChatClient() : socket_(-1), running_(false) {}

ChatClient::~ChatClient() {
    disconnect();
}

bool ChatClient::connect(const std::string& host, int port) {
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) <= 0) {
        close(socket_);
        return false;
    }

    if (::connect(socket_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        close(socket_);
        return false;
    }

    running_ = true;

    // 메인 루프 시작
    mainLoop();
    return true;
}

void ChatClient::disconnect() {
    if (socket_ >= 0) {
        running_ = false;
        close(socket_);
        socket_ = -1;
    }
}

void ChatClient::mainLoop() {
    fd_set readfds;
    struct timeval tv;
    char buffer[1024];

    while (running_) {
        FD_ZERO(&readfds);
        FD_SET(socket_, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        // timeout 설정 (100ms)
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int maxfd = std::max(socket_, STDIN_FILENO) + 1;
        int activity = select(maxfd, &readfds, nullptr, nullptr, &tv);

        if (activity < 0) {
            if (errno != EINTR) {
                break;
            }
            continue;
        }

        // 소켓으로부터 데이터 수신
        if (FD_ISSET(socket_, &readfds)) {
            ChatMessage message;
            ssize_t bytesRead = recv(socket_, &message, sizeof(message), 0);
            if (bytesRead <= 0) {
                break;
            }
            
            if (bytesRead >= sizeof(ChatMessage)) {
                // 메시지 길이 검증
                if (message.length > sizeof(message.data)) {
                    std::string error_msg = "비정상 메시지 수신: type=" + std::to_string(static_cast<int>(message.type)) + 
                        ", length=" + std::to_string(message.length) + 
                        " (최대 허용=" + std::to_string(sizeof(message.data)) + ")";
                    // 에러 로그는 stderr에만 출력
                    std::cerr << error_msg << std::endl;
                    continue;
                }
                handleMessage(message);
            }
        }

        // 표준 입력 처리 (필요한 경우)
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (fgets(buffer, sizeof(buffer), stdin) != nullptr) {
                std::string input(buffer);
                if (!input.empty()) {
                    if (input.back() == '\n') {
                        input.pop_back();
                    }
                    sendChat(input);
                }
            }
        }
    }
    running_ = false;
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

bool ChatClient::sendMessage(MessageType type, const void* data, size_t length) {
    if (socket_ < 0 || !running_) {
        return false;
    }

    ChatMessage message{};
    message.type = type;
    message.length = static_cast<uint16_t>(length);
    
    if (data && length > 0) {
        if (length > sizeof(message.data)) {
            return false;
        }
        std::memcpy(message.data, data, length);
    }

    ssize_t bytesSent = send(socket_, &message, sizeof(message), 0);
    if (bytesSent != sizeof(message)) {
        return false;
    }
    
    return true;
}

void ChatClient::handleMessage(const ChatMessage& message) {
    std::string messageData(message.data, message.length);
    
    // 출력 버퍼링 비활성화
    std::cout.setf(std::ios::unitbuf);
    
    switch (message.type) {
        case MessageType::SERVER_CHAT: {
            std::cout << messageData << std::endl;
            std::cout.flush();
            break;
        }
            
        case MessageType::SERVER_NOTIFICATION: {
            // 세션 참여 메시지는 특별히 처리
            if (messageData.find("세션에 참여") != std::string::npos) {
                size_t pos = messageData.find("세션 ");
                if (pos != std::string::npos) {
                    std::string sessionStr = messageData.substr(pos);
                    std::cout << "joined session:" << sessionStr.substr(3, sessionStr.find(" ", 3) - 3) << std::endl;
                }
            } else {
                std::cout << messageData << std::endl;
            }
            std::cout.flush();
            break;
        }
            
        case MessageType::SERVER_ACK: {
            std::cout << messageData << std::endl;
            std::cout.flush();
            break;
        }
            
        case MessageType::SERVER_ERROR: {
            std::cerr << "ERROR: " << messageData << std::endl;
            std::cerr.flush();
            break;
        }
            
        default: {
            // 알 수 없는 메시지 타입일 경우 메시지 타입 번호도 출력
            std::string error_msg = "알 수 없는 메시지 타입: 0x" + 
                std::string(2 - std::to_string(static_cast<int>(message.type)).length(), '0') +
                std::to_string(static_cast<int>(message.type));
            std::cerr << error_msg << std::endl;
            std::cerr.flush();
            break;
        }
    }
}