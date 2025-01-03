#include "ChatClient.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <thread>
#include <atomic>
#include <sstream>
#include <iomanip>

ChatClient::ChatClient() : socket_fd_(-1), should_stop_(false) {
    std::cout.setf(std::ios::unitbuf);  // 출력 버퍼링 비활성화
}

ChatClient::~ChatClient() {
    disconnect();
}

bool ChatClient::connect(const char* host, int port) {
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ == -1) {
        std::cerr << "소켓 생성 실패" << std::endl;
        return false;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        std::cerr << "잘못된 서버 주소" << std::endl;
        close(socket_fd_);
        return false;
    }

    if (::connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "서버 연결 실패" << std::endl;
        close(socket_fd_);
        return false;
    }

    // 연결 성공 후 수신 스레드 시작
    receiveThread_ = std::thread(&ChatClient::receiveLoop, this);
    return true;
}

void ChatClient::disconnect() {
    should_stop_ = true;
    
    if (socket_fd_ != -1) {
        close(socket_fd_);
        socket_fd_ = -1;
    }

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
}

void ChatClient::sendMessage(const std::string& message) {
    if (socket_fd_ == -1) {
        std::cerr << "서버에 연결되어 있지 않음" << std::endl;
        return;
    }

    // 메시지 전송
    std::string msg = message + "\n";  // 개행 문자 추가
    if (send(socket_fd_, msg.c_str(), msg.length(), 0) < 0) {
        std::cerr << "메시지 전송 실패" << std::endl;
    }
}

void ChatClient::receiveLoop() {
    char buffer[1024];
    
    while (!should_stop_) {
        ssize_t bytes_received = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) {
            if (!should_stop_) {
                std::cerr << "서버와의 연결이 끊어짐" << std::endl;
            }
            break;
        }

        buffer[bytes_received] = '\0';
        handleMessage(buffer, bytes_received);
    }
}

void ChatClient::handleMessage(const char* message, size_t length) {
    // 메시지 출력
    std::cout << message;
    
    // 메시지가 개행으로 끝나지 않으면 개행 추가
    if (length > 0 && message[length-1] != '\n') {
        std::cout << std::endl;
    }
} 