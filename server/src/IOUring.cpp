#include <stdexcept>
#include <iostream>
#include "IOUring.h"
#include "SessionManager.h"
#include "Logger.h"
#include <string.h>
#include <sstream>
#include <iomanip>

IOUring::IOUring() : ring_initialized_(false) {
    initRing();
    buffer_manager_ = std::make_unique<UringBuffer>(&ring_);
}

IOUring::~IOUring() {
    if (ring_initialized_) {
        io_uring_queue_exit(&ring_);
    }
}

void IOUring::initRing() {
    io_uring_params params{};
    memset(&params, 0, sizeof(params));
    int ret = io_uring_queue_init_params(NUM_SUBMISSION_QUEUE_ENTRIES, &ring_, &params);
    if (ret < 0) {
        LOG_FATAL("Failed to initialize io_uring: ", ret);
        throw std::runtime_error("Failed to initialize io_uring");
    }
    LOG_INFO("io_uring initialized successfully");
    ring_initialized_ = true;
}

io_uring_sqe* IOUring::getSQE() {
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

int IOUring::submitAndWait() {
    int ret = io_uring_submit_and_wait(&ring_, NUM_WAIT_ENTRIES);
    if (ret < 0 && ret != -EINTR) {
        LOG_ERROR("io_uring_submit_and_wait failed: ", ret);
        return ret;
    }
    return 0;
}

void IOUring::setContext(io_uring_sqe* sqe, OperationType type, int client_fd, uint16_t buffer_idx) {
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

void IOUring::prepareAccept(int socket_fd) {
    io_uring_sqe* sqe = getSQE();
    setContext(sqe, OperationType::ACCEPT, -1, 0);
    const int flags = 0;
    io_uring_prep_multishot_accept(sqe, socket_fd, nullptr, 0, flags);
}

void IOUring::prepareRead(int client_fd) {
    io_uring_sqe* sqe = getSQE();
    setContext(sqe, OperationType::READ, client_fd, 0);
    io_uring_prep_recv_multishot(sqe, client_fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = 1;  // Buffer group ID
    
}

void IOUring::prepareWrite(int client_fd, const void* buf, unsigned len, uint16_t bid) {
    io_uring_sqe* sqe = getSQE();
    if (!sqe) return;

    io_uring_prep_write(sqe, client_fd, buf, len, 0);
    setContext(sqe, OperationType::WRITE, client_fd, bid);

}

void IOUring::prepareClose(int client_fd) {
    io_uring_sqe* sqe = getSQE();
    setContext(sqe, OperationType::CLOSE, client_fd);
    io_uring_prep_close(sqe, client_fd);
}

void IOUring::handleAccept(io_uring_cqe* cqe) {
    const int client_fd = cqe->res;
    if (client_fd >= 0) {
        LOG_DEBUG("Accepting new connection: fd=", client_fd);
        prepareRead(client_fd);
    } else {
        LOG_ERROR("Accept failed: ", client_fd);
    }
}

void IOUring::handleRead(io_uring_cqe* cqe, int client_fd) {
    bool closed = false;
    const int result = cqe->res;

    LOG_TRACE("Handling read from client ", client_fd, ", result: ", result);

    if (result <= 0) {
        if (result < 0) {
            LOG_ERROR("Read error on fd ", client_fd, ": ", result);
        }
        
        auto session = SessionManager::getInstance().getSession(client_fd);
        if (session && session->getSessionId() >= 0) {
            SessionManager::getInstance().removeSession(client_fd);
        }
        
        uint16_t buffer_idx = buffer_manager_->findClientBuffer(client_fd);
        if (buffer_idx != UINT16_MAX) {
            releaseBuffer(buffer_idx);
        }
        
        prepareClose(client_fd);
        closed = true;
        return;
    }

    if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
        std::cerr << "No buffer was provided for read" << std::endl;
        prepareClose(client_fd);
        closed = true;
    } else {
        const uint16_t bid = cqe->flags >> 16;
        buffer_manager_->markBufferInUse(bid, client_fd);
        
        uint8_t* buf = buffer_manager_->getBufferAddr(bid, buffer_manager_->getBaseAddr());
        auto* message = reinterpret_cast<ChatMessage*>(buf);
        
        // 메시지 검증
        uint8_t msg_type = static_cast<uint8_t>(message->type);
        if (msg_type < 0x10 || msg_type > 0x14) {
            std::cerr << "[ERROR] Invalid message type from client " << client_fd 
                      << ": 0x" << std::hex << static_cast<int>(msg_type) << std::dec << std::endl;
            releaseBuffer(bid);
        } else if (message->length > sizeof(message->data)) {
            std::cerr << "[ERROR] Message too long from client " << client_fd 
                      << ": " << message->length << " bytes" << std::endl;
            releaseBuffer(bid);
        } else if (message->length == 0) {
            std::cerr << "[ERROR] Empty message from client " << client_fd << std::endl;
            releaseBuffer(bid);
        } else {
            processMessage(client_fd, message, bid);
        }
    }

    if (!closed && !(cqe->flags & IORING_CQE_F_MORE)) {
        prepareRead(client_fd);
    }
}

void IOUring::handleWrite(io_uring_cqe* cqe, int client_fd, uint16_t buffer_idx) {
    const int bytes_written = cqe->res;
    
    if (bytes_written <= 0) {
        std::cerr << "Write error on fd " << client_fd << ": " << bytes_written << std::endl;
    }
    
    handleWriteComplete(client_fd, buffer_idx, bytes_written);
}

void IOUring::processMessage(int client_fd, const ChatMessage* message, uint16_t buffer_idx) {
    LOG_DEBUG("Processing message type ", static_cast<int>(message->type), 
              " from client ", client_fd);
              
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
            LOG_ERROR("Unknown message type: ", static_cast<int>(message->type));
            releaseBuffer(buffer_idx);
            break;
    }
}

void IOUring::handleJoinSession(int client_fd, const ChatMessage* message, uint16_t buffer_idx) {
    LOG_DEBUG("Processing JOIN request from client ", client_fd);
    
    if (!message || message->length < sizeof(int32_t)) {
        LOG_ERROR("Invalid JOIN message format");
        releaseBuffer(buffer_idx);
        return;
    }

    const int32_t* session_id_ptr = reinterpret_cast<const int32_t*>(message->data);
    int32_t session_id = *session_id_ptr;
    
    LOG_DEBUG("Client ", client_fd, " requesting to join session ", session_id);
    
    try {
        SessionManager::getInstance().joinSession(client_fd, session_id);
        
        std::string join_message = "Successfully joined session " + std::to_string(session_id);
        sendMessage(client_fd, MessageType::SERVER_ACK, join_message.c_str(), join_message.length(), buffer_idx);
        
        LOG_DEBUG("Client ", client_fd, " successfully joined session ", session_id);
    }
    catch (const std::exception& e) {
        LOG_ERROR("Error joining session: ", e.what());
        std::string error_message = "Failed to join session: ";
        error_message += e.what();
        sendMessage(client_fd, MessageType::SERVER_ERROR, error_message.c_str(), error_message.length(), buffer_idx);
    }
}

void IOUring::handleLeaveSession(int client_fd, const ChatMessage* /* message */, uint16_t /* buffer_idx */) {
    auto session = SessionManager::getInstance().getSession(client_fd);
    if (session) {
        SessionManager::getInstance().removeSession(client_fd);
        LOG_INFO("Client ", client_fd, " left session ", session->getSessionId());
    }
}

void IOUring::handleChatMessage(int client_fd, const ChatMessage* message, uint16_t buffer_idx) {
    auto session = SessionManager::getInstance().getSession(client_fd);
    if (!session || session->getSessionId() < 0) {
        LOG_WARN("Client ", client_fd, " not in any session");
        decrementBufferRefCount(buffer_idx);
        return;
    }
    
    if (!message || message->length == 0 || message->length > MAX_MESSAGE_SIZE) {
        LOG_WARN("Invalid message length from client ", client_fd);
        decrementBufferRefCount(buffer_idx);
        return;
    }

    std::string filtered_data;
    filtered_data.reserve(message->length);

    for (size_t i = 0; i < message->length; ++i) {
        char c = message->data[i];
        if ((c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t' || 
            static_cast<unsigned char>(c) >= 128) {
            filtered_data += c;
        }
    }

    if (filtered_data.empty()) {
        LOG_ERROR("Invalid message content from client ", client_fd);
        decrementBufferRefCount(buffer_idx);
        return;
    }
    
    auto clients = SessionManager::getInstance().getSessionClients(session->getSessionId());
    if (clients.empty()) {
        LOG_DEBUG("No clients in session ", session->getSessionId());
        decrementBufferRefCount(buffer_idx);
        return;
    }

    LOG_TRACE("Broadcasting to ", clients.size(), " clients in session ", session->getSessionId());
    broadcastToSession(session->getSessionId(), MessageType::SERVER_CHAT, 
                      filtered_data.c_str(), filtered_data.length(), buffer_idx, client_fd);
}

void IOUring::sendMessage(int client_fd, MessageType msg_type, const void* data, size_t length, uint16_t buffer_idx) {
    try {
        ChatMessage message{};
        message.type = msg_type;
        message.length = static_cast<uint16_t>(length);
        
        if (data && length > 0) {
            if (length > sizeof(message.data)) {
                decrementBufferRefCount(buffer_idx);
                throw std::runtime_error("메시지 크기 초과");
            }
            memcpy(message.data, data, std::min(length, sizeof(message.data)));
        }
        
        prepareWrite(client_fd, &message, sizeof(ChatMessage), buffer_idx);
        total_messages_++;
    }
    catch (const std::exception& e) {
        std::cerr << "[ERROR] Send failed: " << e.what() << std::endl;
        decrementBufferRefCount(buffer_idx);
        throw;
    }
}

void IOUring::broadcastToSession(int32_t session_id, MessageType msg_type, const void* data, size_t length, uint16_t buffer_idx, int32_t /* exclude_fd */) {
    try {
        auto clients = SessionManager::getInstance().getSessionClients(session_id);
        
        if (clients.empty()) {
            decrementBufferRefCount(buffer_idx);
            return;
        }
        
        size_t broadcast_count = clients.size();
        for (size_t i = 0; i < broadcast_count; i++) {
            buffer_manager_->incrementRefCount(buffer_idx);
        }
        
        for (int32_t target_fd : clients) {
            try {
                sendMessage(target_fd, msg_type, data, length, buffer_idx);
            } catch (const std::exception& e) {
                decrementBufferRefCount(buffer_idx);
                broadcast_count--;
            }
        }
        
        total_broadcasts_++;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Broadcast failed: ", e.what());
        decrementBufferRefCount(buffer_idx);
    }
}

void IOUring::decrementBufferRefCount(uint16_t buffer_idx) {
    buffer_manager_->decrementRefCount(buffer_idx);
}

void IOUring::handleWriteComplete(int32_t client_fd, uint16_t buffer_idx, int32_t bytes_written) {
    if (bytes_written < 0) {
        std::cerr << "[ERROR] Write failed for client " << client_fd << ": " << bytes_written << std::endl;
    }

    decrementBufferRefCount(buffer_idx);
    
    if (buffer_manager_->getRefCount(buffer_idx) == 0) {
        releaseBuffer(buffer_idx);
    }
}

// 주기적인 통계 로깅을 위한 상수 추가
static constexpr uint64_t LOG_INTERVAL = 1000;  // 1000개 메시지마다 로깅

void IOUring::logMessageStats() const {
    static uint64_t last_log_messages = 0;
    uint64_t current_messages = total_messages_.load();
    uint64_t current_broadcasts = total_broadcasts_.load();
    
    if (current_messages / LOG_INTERVAL > last_log_messages / LOG_INTERVAL) {
        LOG_INFO("Stats - Messages: ", current_messages, ", Broadcasts: ", current_broadcasts);
        last_log_messages = current_messages;
    }
}

unsigned IOUring::peekCQE(io_uring_cqe** cqes) {
    return io_uring_peek_batch_cqe(&ring_, cqes, CQE_BATCH_SIZE);
}

void IOUring::advanceCQ(unsigned count) {
    io_uring_cq_advance(&ring_, count);
}