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
    
private:
    int listening_socket_;
    sockaddr_in client_addr_;
    socklen_t client_addr_len_;
}; 