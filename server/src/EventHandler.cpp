#include "EventHandler.h"
#include "SessionManager.h"
#include <iostream>
#include <cstring>
#include <sstream>

EventHandler::EventHandler(BufferManager& buffer_manager, IOUringManager& io_manager)
    : buffer_manager_(buffer_manager), io_manager_(io_manager) {
}

void EventHandler::handleAccept(io_uring_cqe* cqe) {
    const int client_fd = cqe->res;
    if (client_fd >= 0) {
        std::cout << "New connection accepted: " << client_fd << std::endl;
        io_manager_.prepareRead(client_fd);
    } else {
        std::cerr << "Accept failed: " << client_fd << std::endl;
    }
}

void EventHandler::handleRead(io_uring_cqe* cqe, int client_fd) {
    bool closed = false;
    const int result = cqe->res;

    if (result <= 0) {
        if (result < 0) {
            std::cerr << "Read error on fd " << client_fd << ": " << result << std::endl;
        }
        
        // 연결 종료 전 세션 정리
        auto session = SessionManager::getInstance().getSession(client_fd);
        if (session && session->getSessionId() >= 0) {
            int32_t session_id = session->getSessionId();
            std::string notification = "User " + std::to_string(client_fd) + " disconnected";
            
            // 다른 클라이언트들에게 알림
            auto clients = SessionManager::getInstance().getSessionClients(session_id);
            for (int32_t target_fd : clients) {
                if (target_fd != client_fd) {
                    ChatMessage message{};
                    message.type = MessageType::SERVER_NOTIFICATION;
                    message.length = static_cast<uint16_t>(notification.length());
                    std::memcpy(message.data, notification.c_str(), notification.length());
                    io_manager_.prepareWrite(target_fd, &message, sizeof(ChatMessage), 0);
                }
            }
            
            // 세션에서 제거
            SessionManager::getInstance().removeSession(client_fd);
        }
        
        // 버퍼 정리
        uint16_t buffer_idx = buffer_manager_.findClientBuffer(client_fd);
        if (buffer_idx != UINT16_MAX) {
            buffer_manager_.releaseBuffer(buffer_idx, buffer_manager_.getBaseAddr());
        }
        
        io_manager_.prepareClose(client_fd);
        closed = true;
        return;
    }

    if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
        std::cerr << "No buffer was provided for read" << std::endl;
        io_manager_.prepareClose(client_fd);
        closed = true;
    } else {
        const uint16_t bid = cqe->flags >> 16;
        buffer_manager_.markBufferInUse(bid, client_fd);
        
        uint8_t* buf = buffer_manager_.getBufferAddr(bid, buffer_manager_.getBaseAddr());
        if (static_cast<size_t>(result) >= sizeof(ChatMessage)) {
            auto* message = reinterpret_cast<ChatMessage*>(buf);
            processMessage(client_fd, message, bid);
        }
    }

    if (!closed && !(cqe->flags & IORING_CQE_F_MORE)) {
        io_manager_.prepareRead(client_fd);
    }
}

void EventHandler::handleWrite(io_uring_cqe* cqe, int client_fd, uint16_t buffer_idx) {
    const int bytes_written = cqe->res;
    
    if (bytes_written <= 0) {
        std::cerr << "Write error on fd " << client_fd << ": " << bytes_written << std::endl;
    }
    
   //buffer_manager_.decrementRefCount(buffer_idx);
}

void EventHandler::processMessage(int client_fd, const ChatMessage* message, uint16_t buffer_idx) {
    switch (message->type) {
        case MessageType::CLIENT_JOIN:
            handleJoinSession(client_fd, message, buffer_idx);
            break;
        case MessageType::CLIENT_LEAVE:
            handleLeaveSession(client_fd, message, buffer_idx);
            break;
        case MessageType::CLIENT_CHAT:
            handleChatMessage(client_fd, message, buffer_idx);
            break;
        default:
            buffer_manager_.releaseBuffer(buffer_idx, buffer_manager_.getBaseAddr());
            break;
    }
}

void EventHandler::handleJoinSession(int client_fd, const ChatMessage* message, uint16_t buffer_idx) {
    const int32_t* session_id_ptr = reinterpret_cast<const int32_t*>(message->data);
    int32_t session_id = *session_id_ptr;
    
    SessionManager::getInstance().joinSession(client_fd, session_id);

    std::string join_message = "Successfully joined session " + std::to_string(session_id);
    sendMessage(client_fd, MessageType::SERVER_ACK, join_message.c_str(), join_message.length(), buffer_idx);
    
}

void EventHandler::handleLeaveSession(int client_fd, const ChatMessage* message, uint16_t buffer_idx) {
    auto session = SessionManager::getInstance().getSession(client_fd);
    int32_t session_id = session->getSessionId();
    SessionManager::getInstance().removeSession(client_fd);
}

void EventHandler::handleChatMessage(int client_fd, const ChatMessage* message, uint16_t buffer_idx) {
    auto session = SessionManager::getInstance().getSession(client_fd);
    if (!session || session->getSessionId() < 0) {
        buffer_manager_.releaseBuffer(buffer_idx, buffer_manager_.getBaseAddr());
        return;
    }
    
    auto clients = SessionManager::getInstance().getSessionClients(session->getSessionId());
    if (clients.size() <= 1) {
        buffer_manager_.releaseBuffer(buffer_idx, buffer_manager_.getBaseAddr());
        return;
    }

    broadcastToSession(session->getSessionId(), MessageType::SERVER_CHAT, message->data, message->length, buffer_idx);
    
    
}

void EventHandler::sendMessage(int client_fd, MessageType msg_type, const void* data, size_t length, uint16_t buffer_idx) {
    try {
        ChatMessage message{};
        message.type = msg_type;
        message.length = static_cast<uint16_t>(length);
        
        if (data && length > 0) {
            if (length > sizeof(message.data)) {
                throw std::runtime_error("메시지 데이터가 너무 큽니다");
            }
            std::memcpy(message.data, data, std::min(length, sizeof(message.data)));
        }
        
        io_manager_.prepareWrite(client_fd, &message, sizeof(ChatMessage), buffer_idx);
        total_messages_++;
        logMessageStats();
    }
    catch (const std::exception& e) {
        std::cerr << "Error sending message: " << e.what() << std::endl;
        buffer_manager_.releaseBuffer(buffer_idx, buffer_manager_.getBaseAddr());
    }
}

void EventHandler::broadcastToSession(int32_t session_id, MessageType msg_type, const void* data, size_t length, uint16_t buffer_idx, int32_t exclude_fd) {
    auto clients = SessionManager::getInstance().getSessionClients(session_id);
    
    if (exclude_fd != -1) {
        clients.erase(exclude_fd);
    }
    
    if (clients.empty()) {
        buffer_manager_.releaseBuffer(buffer_idx, buffer_manager_.getBaseAddr());
        return;
    }
    
    total_broadcasts_++;
    size_t broadcast_count = clients.size();
    
    for (int32_t target_fd : clients) {
        //buffer_manager_.incrementRefCount(buffer_idx);
        sendMessage(target_fd, msg_type, data, length, buffer_idx);
    }
    
    std::cout << "[브로드캐스트] 세션 " << session_id << "에 메시지 전송: " 
              << broadcast_count << "명의 클라이언트에게 전달 (누적: " << total_broadcasts_ 
              << "개의 브로드캐스트)" << std::endl;
    
    buffer_manager_.releaseBuffer(buffer_idx, buffer_manager_.getBaseAddr());
}

void EventHandler::logMessageStats() const {
    static uint64_t last_log_messages = 0;
    static uint64_t last_log_broadcasts = 0;
    
    uint64_t current_messages = total_messages_.load();
    uint64_t current_broadcasts = total_broadcasts_.load();
    
    // 변화가 있을 때만 로그 출력
    if (current_messages > last_log_messages || current_broadcasts > last_log_broadcasts) {
        std::cout << "[메시지 통계] 총 전송: " << current_messages 
                  << ", 총 브로드캐스트: " << current_broadcasts << std::endl;
                  
        last_log_messages = current_messages;
        last_log_broadcasts = current_broadcasts;
    }
} 