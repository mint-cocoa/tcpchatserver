#pragma once
#include "IOUring.h"
#include "SocketManager.h"
#include "SessionManager.h"
#include <memory>
#include <unistd.h>  // for close()

class Listener {
public:
    explicit Listener(int port, SocketManager& socket_manager);
    ~Listener();

    void start();
    void processEvents();
    void stop();

private:
    int port_;
    bool running_;
    std::unique_ptr<IOUring> io_ring_;
    SocketManager& socket_manager_;
}; 