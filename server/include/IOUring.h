#pragma once
#include <liburing.h>
#include <memory>
#include <atomic>
#include "UringBuffer.h"
#include "Context.h"
#include <vector>
#include <mutex>

class IOUring {
public:
    static constexpr unsigned NUM_SUBMISSION_QUEUE_ENTRIES = 2048;
    static constexpr unsigned CQE_BATCH_SIZE = 256;
    static constexpr unsigned NUM_WAIT_ENTRIES = 1;
    IOUring();
    ~IOUring();

    // IO 준비 메서드
    void prepareAccept(int socket_fd);
    void prepareRead(int client_fd);
    void prepareWrite(int client_fd, const void* buf, unsigned len, uint16_t bid);
    void prepareClose(int client_fd);
    
    // IO 이벤트 처리 메서드
    void handleAccept(io_uring_cqe* cqe);
    void handleRead(io_uring_cqe* cqe, int client_fd);
    void handleWrite(io_uring_cqe* cqe, int client_fd, uint16_t buffer_idx);
    
    // 메시지 처리 메서드
    void processMessage(int client_fd, const ChatMessage* message, uint16_t buffer_idx);
    void handleJoinSession(int client_fd, const ChatMessage* message, uint16_t buffer_idx);
    void handleLeaveSession(int client_fd, const ChatMessage* message, uint16_t buffer_idx);
    void handleChatMessage(int client_fd, const ChatMessage* message, uint16_t buffer_idx);
    
    // 메시지 전송 메서드
    void sendMessage(int client_fd, MessageType msg_type, const void* data, size_t length, uint16_t buffer_idx);
    void broadcastToSession(int32_t session_id, MessageType msg_type, const void* data, size_t length, uint16_t buffer_idx, int32_t exclude_fd = -1);
    
    unsigned peekCQE(io_uring_cqe** cqes);
    void advanceCQ(unsigned count);
    int submitAndWait();

    // Non-blocking submit
    int submit() {
        return io_uring_submit(&ring_);
    }

    // Buffer management methods
    void incrementRefCount(uint16_t idx) { buffer_manager_->incrementRefCount(idx); }
    void decrementRefCount(uint16_t idx) { buffer_manager_->decrementRefCount(idx); }
    uint32_t getRefCount(uint16_t idx) const { return buffer_manager_->getRefCount(idx); }
    void markBufferInUse(uint16_t idx, uint16_t client_fd) { buffer_manager_->markBufferInUse(idx, client_fd); }
    void releaseBuffer(uint16_t idx) { buffer_manager_->releaseBuffer(idx, buffer_manager_->getBaseAddr()); }
    void updateBufferBytes(uint16_t idx, uint64_t bytes) { buffer_manager_->updateBufferBytes(idx, bytes); }
    bool isBufferInUse(uint16_t idx) const { return buffer_manager_->isBufferInUse(idx); }
    uint16_t getBufferClient(uint16_t idx) const { return buffer_manager_->getBufferClient(idx); }
    uint64_t getBufferBytesUsed(uint16_t idx) const { return buffer_manager_->getBufferBytesUsed(idx); }
    double getBufferUsageTime(uint16_t idx) const { return buffer_manager_->getBufferUsageTime(idx); }
    uint16_t findClientBuffer(uint16_t client_fd) const { return buffer_manager_->findClientBuffer(client_fd); }
    void printBufferStatus(uint16_t highlight_idx = UINT16_MAX) { buffer_manager_->printBufferStatus(highlight_idx); }
    void printBufferStats() const { buffer_manager_->printBufferStats(); }

    void handleWriteComplete(int32_t client_fd, uint16_t buffer_idx, int32_t bytes_written);

private:
    void initRing();
    io_uring_sqe* getSQE();
    void setContext(io_uring_sqe* sqe, OperationType type, int client_fd = -1, uint16_t buffer_idx = 0);
    void logMessageStats() const;

    io_uring ring_;
    bool ring_initialized_;
    std::unique_ptr<UringBuffer> buffer_manager_;
    std::atomic<uint64_t> total_broadcasts_{0};
    std::atomic<uint64_t> total_messages_{0};
    
    void decrementBufferRefCount(uint16_t buffer_idx);
}; 
