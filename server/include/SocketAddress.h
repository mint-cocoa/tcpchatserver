#pragma once
#include <sys/socket.h>
#include <cstring>
#include <memory>
#include <netinet/in.h>
class SocketAddress {
public:
    SocketAddress(uint32_t inAddress, uint16_t inPort)
    {
        GetAsSockAddrIn()->sin_family = AF_INET;
        GetAsSockAddrIn()->sin_addr.s_addr = htonl(inAddress);
        GetAsSockAddrIn()->sin_port = htons(inPort);
    }
    SocketAddress(const sockaddr& inSockAddr)
    {
        std::memcpy(&mSockAddr, &inSockAddr, sizeof(sockaddr));
    }
    size_t GetSize() const { return sizeof(sockaddr); }

private:
    friend class UDPSocket;
    friend class TCPSocket;
    friend class SocketUtil;
    friend class UringCore;
    sockaddr mSockAddr;
    sockaddr_in* GetAsSockAddrIn() { return reinterpret_cast<sockaddr_in*>(&mSockAddr); }
};

using SocketAddressPtr = std::shared_ptr<SocketAddress>;


