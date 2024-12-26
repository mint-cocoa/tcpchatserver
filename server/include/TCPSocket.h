#pragma once
#include "SocketAddress.h"
#include <memory> // Add this line to include the memory header for std::shared_ptr

class TCPSocket;
using TCPSocketPtr = std::shared_ptr<TCPSocket>;

class TCPSocket: public std::enable_shared_from_this<TCPSocket>{
public:
    TCPSocket(TCPSocketPtr sharedPtr);
    ~TCPSocket();

    int Connect(const SocketAddress& inAddress);
    TCPSocketPtr Bind(const SocketAddress& inToAddress);
    TCPSocketPtr Listen(int inBacklog = 32);
    TCPSocketPtr Accept(SocketAddress& inFromAddress);
    int Send(const void* inData, int inLen);
    int Receive(void* inBuffer, int inLen);
    bool SetNonBlockingMode(bool shouldBeNonBlocking);
    int getFD() const { return mSocket; }
private:
    friend class SocketUtil;
    explicit TCPSocket(int inSocket) : mSocket(inSocket) {}
    int mSocket;
};