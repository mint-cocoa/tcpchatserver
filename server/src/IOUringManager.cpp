#include <stdexcept>
#include <iostream>
#include "IOUringManager.h"
#include <string.h>

IOUringManager::IOUringManager() : ring_initialized_(false) {
    initRing();
}

IOUringManager::~IOUringManager() {
    if (ring_initialized_) {
        io_uring_queue_exit(&ring_);
    }
}

void IOUringManager::initRing() {
    io_uring_params params{};
    memset(&params, 0, sizeof(params));
    int ret = io_uring_queue_init_params(NUM_ENTRIES, &ring_, &params);
    if (ret < 0) {
        throw std::runtime_error("Failed to initialize io_uring");
    }
    ring_initialized_ = true;
}

io_uring_sqe* IOUringManager::getSQE() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            throw std::runtime_error("Failed to get SQE");
        }
    }
    return sqe;
}

int IOUringManager::submitAndWait() {
    int ret = io_uring_submit_and_wait(&ring_, 1);
    if (ret < 0 && ret != -EINTR) {
        std::cerr << "io_uring_submit_and_wait failed: " << ret << std::endl;
        return ret;
    }
    return 0;
}

void IOUringManager::setContext(io_uring_sqe* sqe, OperationType type, int client_fd, uint16_t buffer_idx) {
    static_assert(8 == sizeof(__u64));  // user_data 크기 확인

    auto* buffer = reinterpret_cast<uint8_t*>(&sqe->user_data);

    // client_fd 쓰기 (4 bytes)
    *(reinterpret_cast<int32_t*>(buffer)) = client_fd;
    buffer += 4;
    // type 쓰기 (1 byte)
    *buffer = static_cast<uint8_t>(type);
    buffer += 1;
    // buffer_idx 쓰기 (2 bytes)
    *(reinterpret_cast<uint16_t*>(buffer)) = buffer_idx;
}

void IOUringManager::prepareAccept(int socket_fd, sockaddr* addr, socklen_t* addrlen) {
    io_uring_sqe* sqe = getSQE();
    setContext(sqe, OperationType::ACCEPT, -1, 0);
    const int flags = 0;
    io_uring_prep_multishot_accept(sqe, socket_fd, addr, addrlen, flags);
}

void IOUringManager::prepareRead(int client_fd) {
    io_uring_sqe* sqe = getSQE();
    setContext(sqe, OperationType::READ, client_fd , 0);
    io_uring_prep_recv_multishot(sqe, client_fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = 1;  // Buffer group ID
}

void IOUringManager::prepareWrite(int client_fd, const void* buf, unsigned len, uint16_t bid) {
    io_uring_sqe* sqe = getSQE();
    if (!sqe) return;

    io_uring_prep_write(sqe, client_fd, buf, len, 0);
    setContext(sqe, OperationType::WRITE, client_fd, bid);
    submitAndWait();
}

void IOUringManager::prepareClose(int client_fd) {
    io_uring_sqe* sqe = getSQE();
    setContext(sqe, OperationType::CLOSE, client_fd);
    io_uring_prep_close(sqe, client_fd);
}

unsigned IOUringManager::peekCQE(io_uring_cqe** cqes) {
    return io_uring_peek_batch_cqe(&ring_, cqes, CQE_BATCH_SIZE);
}

void IOUringManager::advanceCQ(unsigned count) {
    io_uring_cq_advance(&ring_, count);
}
