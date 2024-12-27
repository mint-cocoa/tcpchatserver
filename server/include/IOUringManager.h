#pragma once
#include <liburing.h>
#include <memory>
#include "Context.h"
class IOUringManager {
public:
    static constexpr unsigned NUM_ENTRIES = 256;
    static constexpr unsigned CQE_BATCH_SIZE = 8;
    
    IOUringManager();
    ~IOUringManager();
    
    io_uring_sqe* getSQE();
    int submitAndWait();
    void initRing();

    void prepareAccept(int socket_fd, sockaddr* addr, socklen_t* addrlen);
    void prepareRead(int client_fd);
    void prepareWrite(int client_fd, const void* buf, unsigned len, uint16_t bid);
    void prepareClose(int client_fd);
    
    unsigned peekCQE(io_uring_cqe** cqes);
    void advanceCQ(unsigned count);

    io_uring* getRing() { return &ring_; }

    void setContext(io_uring_sqe* sqe, OperationType op_type, int client_fd, uint16_t buffer_id = 0);

private:
    io_uring ring_;
    bool ring_initialized_;
}; 