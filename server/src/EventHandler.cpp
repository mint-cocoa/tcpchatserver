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
        uint8_t* buf = buffer_manager_.getBufferAddr(bid);
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
    
    buffer_manager_.decrementRefCount(buffer_idx);
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
            buffer_manager_.releaseBuffer(buffer_idx);
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
    if (!session || session->getSessionId() < 0) {
        buffer_manager_.releaseBuffer(buffer_idx);
        return;
    }

    int32_t session_id = session->getSessionId();
    std::string notification = "User " + std::to_string(client_fd) + " left the session";
    
    SessionManager::getInstance().removeSession(client_fd);
    
    broadcastToSession(session_id, MessageType::SERVER_NOTIFICATION, notification.c_str(), notification.length(), buffer_idx);
    sendMessage(client_fd, MessageType::SERVER_ACK, "Successfully left the session", 27, buffer_idx);
}

void EventHandler::handleChatMessage(int client_fd, const ChatMessage* message, uint16_t buffer_idx) {
    auto session = SessionManager::getInstance().getSession(client_fd);
    if (!session || session->getSessionId() < 0) {
        buffer_manager_.releaseBuffer(buffer_idx);
        return;
    }
    
    auto clients = SessionManager::getInstance().getSessionClients(session->getSessionId());
    if (clients.size() <= 1) {
        buffer_manager_.releaseBuffer(buffer_idx);
        return;
    }
    
    for (int32_t target_fd : clients) {
        if (target_fd != client_fd) {
            buffer_manager_.incrementRefCount(buffer_idx);
            sendMessage(target_fd, MessageType::SERVER_CHAT, message->data, message->length, buffer_idx);
        }
    }
    
    buffer_manager_.releaseBuffer(buffer_idx);
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
    }
    catch (const std::exception& e) {
        std::cerr << "Error sending message: " << e.what() << std::endl;
        buffer_manager_.releaseBuffer(buffer_idx);
    }
}

void EventHandler::broadcastToSession(int32_t session_id, MessageType msg_type, const void* data, size_t length, uint16_t buffer_idx, int32_t exclude_fd) {
    auto clients = SessionManager::getInstance().getSessionClients(session_id);
    
    if (exclude_fd != -1) {
        clients.erase(exclude_fd);
    }
    
    if (clients.empty()) {
        buffer_manager_.releaseBuffer(buffer_idx);
        return;
    }
    
    for (int32_t target_fd : clients) {
        buffer_manager_.incrementRefCount(buffer_idx);
        sendMessage(target_fd, msg_type, data, length, buffer_idx);
    }
    
    buffer_manager_.releaseBuffer(buffer_idx);
} 