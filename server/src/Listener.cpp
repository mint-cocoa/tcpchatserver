#include "Listener.h"
#include "SessionManager.h"
#include "Logger.h"
#include <stdexcept>
#include "Context.h"

namespace {
    Operation getContext(io_uring_cqe* cqe) {
        Operation ctx{};
        auto* buffer = reinterpret_cast<uint8_t*>(&cqe->user_data);
        
        ctx.client_fd = *(reinterpret_cast<int32_t*>(buffer));
        buffer += 4;
        ctx.op_type = static_cast<OperationType>(*buffer);
        buffer += 1;
        ctx.buffer_idx = *(reinterpret_cast<uint16_t*>(buffer));
        
        return ctx;
    }
}

Listener::Listener(int port, SocketManager& socket_manager)
    : port_(port), running_(false), socket_manager_(socket_manager) {
    io_ring_ = std::make_unique<IOUring>();
    LOG_INFO("[Listener] Created with dedicated IOUring");
}

Listener::~Listener() {
    stop();
}

void Listener::start() {
    if (running_) {
        return;
    }

    int listening_socket = socket_manager_.createListeningSocket(port_);
    if (listening_socket < 0) {
        throw std::runtime_error("Failed to create listening socket");
    }
    LOG_INFO("[Listener] Server listening on port ", port_);

    running_ = true;
    io_ring_->prepareAccept(listening_socket);
}

void Listener::processEvents() {
    while (running_) {
        io_uring_cqe* cqes[IOUring::CQE_BATCH_SIZE];
        unsigned num_cqes = io_ring_->peekCQE(cqes);
        
        if (num_cqes == 0) {
            const int result = io_ring_->submitAndWait();
            if (result == -EINTR) continue;
            if (result < 0) {
                LOG_ERROR("[Listener] io_uring_submit_and_wait failed: ", result);
                continue;
            }
            num_cqes = io_ring_->peekCQE(cqes);
        }
        
        for (unsigned i = 0; i < num_cqes; ++i) {
            io_uring_cqe* cqe = cqes[i];
            const auto ctx = getContext(cqe);
            
            LOG_TRACE("[Listener] Processing event type: ", static_cast<int>(ctx.op_type));
            
            if (ctx.op_type == OperationType::ACCEPT) {
                const auto client_fd = cqe->res;
                if (client_fd < 0) {
                    LOG_ERROR("[Listener] Accept failed with error: ", client_fd);
                    continue;
                }
                
                LOG_DEBUG("[Listener] Accepted new connection: fd=", client_fd);
                
                try {
                    // 항상 세션 1에 할당
                    int32_t session_id = SessionManager::getInstance().getNextAvailableSession();
                    LOG_DEBUG("[Listener] Selected session ", session_id, " for client ", client_fd);
                    
                    // 클라이언트를 세션에 추가
                    SessionManager::getInstance().joinSession(client_fd, session_id);
                    
                    LOG_INFO("[Listener] Successfully assigned client ", client_fd, " to session ", session_id);
                }
                catch (const std::exception& e) {
                    LOG_ERROR("[Listener] Failed to assign client to session: ", e.what());
                    close(client_fd);  // 세션 할당 실패 시 연결 종료
                }
            } else {
                LOG_DEBUG("[Listener] Ignoring non-accept event type: ", static_cast<int>(ctx.op_type));
            }
        }
        
        if (num_cqes > 0) {
            io_ring_->advanceCQ(num_cqes);
            LOG_TRACE("[Listener] Processed ", num_cqes, " events");
        }
    }
}

void Listener::stop() {
    if (!running_) return;
    running_ = false;
    io_ring_.reset();
    LOG_INFO("[Listener] Server stopped");
} 