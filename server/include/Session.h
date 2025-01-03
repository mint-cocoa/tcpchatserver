#pragma once
#include "IOUring.h"
#include <set>
#include <memory>
#include <thread>
#include "Context.h"

class Session {
public:
    static constexpr unsigned CQE_BATCH_SIZE = 32;  // 한 번에 처리할 최대 이벤트 수
    
    explicit Session(int32_t id);
    ~Session();
    
    void processEvent(io_uring_cqe* cqe);  // 단일 이벤트 처리
    
    int32_t getSessionId() const { return session_id_; }
    IOUring* getIOUring() { return io_ring_.get(); }
    const std::set<int32_t>& getClients() const { return clients_; }
    
    void addClient(int32_t client_fd);
    void removeClient(int32_t client_fd) { clients_.erase(client_fd); }
    size_t getClientCount() const { return clients_.size(); }
    
    void setListeningSocket(int socket_fd);

private:
    void handleRead(io_uring_cqe* cqe, const Operation& ctx);
    void handleWrite(io_uring_cqe* cqe, const Operation& ctx);
    void handleClose(int client_fd);

    int32_t session_id_;
    std::unique_ptr<IOUring> io_ring_;
    std::set<int32_t> clients_;
}; 