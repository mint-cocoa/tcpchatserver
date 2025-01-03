#include "SocketManager.h"
#include "Logger.h"
#include <cstring>

SocketManager::SocketManager() : listening_socket_(-1), client_addr_len_(sizeof(client_addr_)) {
    memset(&client_addr_, 0, sizeof(client_addr_));
}

SocketManager::~SocketManager() {
    if (listening_socket_ >= 0) {
        closeSocket(listening_socket_);
    }
}

int SocketManager::createListeningSocket(int port) {
    listening_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listening_socket_ < 0) {
        LOG_ERROR("Failed to create socket");
        return -1;
    }

    int enable = 1;
    if (setsockopt(listening_socket_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        LOG_ERROR("setsockopt(SO_REUSEADDR) failed");
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listening_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Bind failed");
        return -1;
    }

    if (listen(listening_socket_, SOMAXCONN) < 0) {
        LOG_ERROR("Listen failed");
        return -1;
    }

    LOG_INFO("Successfully created listening socket on port ", port);
    return listening_socket_;
}

void SocketManager::closeSocket(int fd) {
    close(fd);
    LOG_DEBUG("Closed socket fd=", fd);
} 