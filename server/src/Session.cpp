#include "Session.h"
#include "Context.h"
#include "Utils.h"
#include "Logger.h"

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

Session::Session(int32_t id) : session_id_(id) {
    io_ring_ = std::make_unique<IOUring>();
    LOG_INFO("[Session ", id, "] Created with dedicated IOUring");
}

Session::~Session() = default;

void Session::processEvent(io_uring_cqe* cqe) {
    if (!cqe) return;
    
    const auto ctx = getContext(cqe);
    LOG_TRACE("[Session ", session_id_, "] Event: type=", static_cast<int>(ctx.op_type), 
              ", client=", ctx.client_fd, ", buffer=", ctx.buffer_idx);
    
    switch (ctx.op_type) {
        case OperationType::READ:
            if (cqe->res <= 0) {
                LOG_INFO("[Session ", session_id_, "] Client ", ctx.client_fd, 
                        " disconnected (res=", cqe->res, ")");
                handleClose(ctx.client_fd);
            } else {
                LOG_DEBUG("[Session ", session_id_, "] Read complete: ", cqe->res, 
                         " bytes (client=", ctx.client_fd, ", buffer=", ctx.buffer_idx, ")");
                io_ring_->handleRead(cqe, ctx.client_fd);
                
                if (clients_.find(ctx.client_fd) != clients_.end()) {
                    io_ring_->prepareRead(ctx.client_fd);
                    LOG_TRACE("[Session ", session_id_, "] Prepared next read (client=", ctx.client_fd, ")");
                }
            }
            break;
            
        case OperationType::WRITE:
            if (cqe->res < 0) {
                LOG_ERROR("[Session ", session_id_, "] Write failed (client=", ctx.client_fd,
                         ", buffer=", ctx.buffer_idx, ", error=", cqe->res, ")");
            } else {
                LOG_DEBUG("[Session ", session_id_, "] Write complete: ", cqe->res,
                         " bytes (client=", ctx.client_fd, ", buffer=", ctx.buffer_idx, ")");
            }
            io_ring_->handleWrite(cqe, ctx.client_fd, ctx.buffer_idx);
            break;
            
        case OperationType::CLOSE:
            LOG_DEBUG("[Session ", session_id_, "] Processing close (client=", ctx.client_fd, ")");
            break;
            
        case OperationType::ACCEPT:
            LOG_DEBUG("[Session ", session_id_, "] Ignoring ACCEPT event (handled by Listener)");
            break;
            
        default:
            LOG_ERROR("[Session ", session_id_, "] Unknown event type: ", static_cast<int>(ctx.op_type),
                     " (client=", ctx.client_fd, ", buffer=", ctx.buffer_idx, ")");
            break;
    }
}

void Session::handleClose(int client_fd) {
    removeClient(client_fd);
    io_ring_->prepareClose(client_fd);
    LOG_INFO("[Session ", session_id_, "] Closed client ", client_fd);
}

void Session::addClient(int32_t client_fd) {
    clients_.insert(client_fd);
    std::string session_msg = "joined session:" + std::to_string(session_id_);
    io_ring_->prepareRead(client_fd);   
    io_ring_->sendMessage(client_fd, MessageType::SERVER_NOTIFICATION, 
                         session_msg.c_str(), session_msg.length(), 0);
    io_ring_->submit();
    
    LOG_INFO("[Session ", session_id_, "] Added client ", client_fd, " and submitted read request");
}

void Session::setListeningSocket(int socket_fd) {
    io_ring_->prepareAccept(socket_fd);
    LOG_INFO("[Session ", session_id_, "] Started listening on socket ", socket_fd);
}
