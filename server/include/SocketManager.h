#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

class SocketManager {
public:
    SocketManager();
    ~SocketManager();
    
    int createListeningSocket(int port);
    void closeSocket(int fd);
    int getListeningSocket() const { return listening_socket_; }
    
private:
    int listening_socket_{-1};
    sockaddr_in client_addr_;
    socklen_t client_addr_len_;
}; 