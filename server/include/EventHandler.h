#pragma once
#include <liburing.h>
#include "Context.h"
#include "BufferManager.h"
#include "IOUringManager.h"
#include <string>
#include <vector>

class EventHandler {
public:
    EventHandler(BufferManager& buffer_manager, IOUringManager& io_manager);
    
    void handleAccept(io_uring_cqe* cqe);
    void handleRead(io_uring_cqe* cqe, int client_fd);
    void handleWrite(io_uring_cqe* cqe, int client_fd, uint16_t buffer_idx);

private:
    BufferManager& buffer_manager_;
    IOUringManager& io_manager_;

    void processMessage(int client_fd, const ChatMessage* message, uint16_t buffer_idx);
    
    // 클라이언트 메시지 핸들러
    void handleJoinSession(int client_fd, const ChatMessage* message, uint16_t buffer_idx);
    void handleLeaveSession(int client_fd, const ChatMessage* message, uint16_t buffer_idx);
    void handleChatMessage(int client_fd, const ChatMessage* message, uint16_t buffer_idx);
    
    // 유틸리티 메서드
    void sendMessage(int client_fd, MessageType msg_type, const void* data, size_t length, uint16_t buffer_idx);
    void broadcastToSession(int32_t session_id, MessageType msg_type, const void* data, size_t length, uint16_t buffer_idx, int32_t exclude_fd = -1);
}; 