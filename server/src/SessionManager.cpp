#include "SessionManager.h"
#include "Utils.h"
#include "Logger.h"
#include <stdexcept>
#include <thread>
#include <sstream>
#include <iomanip>

SessionManager::SessionManager() {
    num_worker_threads_ = getOptimalThreadCount();
}

SessionManager::~SessionManager() {
    stop();
}

size_t SessionManager::getOptimalThreadCount() const {
    size_t hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 2;
    return hw_threads - 1;
}

void SessionManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    LOG_INFO("[SessionManager] Initializing with ", num_worker_threads_, 
             " sessions (one per worker thread)");
    
    for (size_t i = 0; i < num_worker_threads_; ++i) {
        int32_t session_id = next_session_id_++;
        auto session = std::make_shared<Session>(session_id);
        sessions_[session_id] = session;
        LOG_DEBUG("[SessionManager] Created session ", session_id);
    }

    thread_sessions_.resize(num_worker_threads_);
    distributeSessionsToThreads();
}

void SessionManager::distributeSessionsToThreads() {
    size_t thread_idx = 0;
    for (const auto& [session_id, session] : sessions_) {
        if (thread_idx >= num_worker_threads_) break;
        thread_sessions_[thread_idx].push_back(session);
        thread_idx++;
    }
}

void SessionManager::start() {
    should_stop_ = false;
    
    LOG_INFO("[SessionManager] Starting ", num_worker_threads_, " worker threads");
    
    for (size_t i = 0; i < num_worker_threads_; ++i) {
        worker_threads_.emplace_back(&SessionManager::workerThread, this, i);
        LOG_DEBUG("[SessionManager] Started worker thread ", i);
    }
}

void SessionManager::stop() {
    should_stop_ = true;
    
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    worker_threads_.clear();
    thread_sessions_.clear();
    sessions_.clear();
    client_sessions_.clear();
    
    LOG_INFO("[SessionManager] All threads stopped");
}

void SessionManager::workerThread(size_t thread_id) {
    LOG_INFO("[SessionManager] Worker thread ", thread_id, " started");
    
    while (!should_stop_) {
        for (auto& session : thread_sessions_[thread_id]) {
            if (!session || !session->getIOUring()) continue;

            io_uring_cqe* cqes[Session::CQE_BATCH_SIZE];
            unsigned num_cqes = session->getIOUring()->peekCQE(cqes);
            
            if (num_cqes == 0) {
                const int result = session->getIOUring()->submitAndWait();
                if (result == -EINTR) continue;
                if (result < 0) {
                    LOG_ERROR("[Session ", session->getSessionId(), 
                             "] io_uring_submit_and_wait failed: ", result);
                    continue;
                }
                num_cqes = session->getIOUring()->peekCQE(cqes);
            }
            
            for (unsigned i = 0; i < num_cqes; ++i) {
                session->processEvent(cqes[i]);
            }
            
            if (num_cqes > 0) {
                session->getIOUring()->advanceCQ(num_cqes);
            }
        }
        
        if (thread_sessions_[thread_id].empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    LOG_INFO("[SessionManager] Worker thread ", thread_id, " stopped");
}

int32_t SessionManager::getNextAvailableSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    int32_t selected_session = -1;
    size_t min_clients = SIZE_MAX;
    
    // 가장 적은 클라이언트를 가진 세션 선택
    for (const auto& [session_id, session] : sessions_) {
        size_t client_count = session->getClientCount();
        if (client_count < min_clients) {
            min_clients = client_count;
            selected_session = session_id;
        }
    }
    
    if (selected_session == -1) {
        throw std::runtime_error("No available sessions");
    }
    
    return selected_session;
}

void SessionManager::joinSession(int32_t client_fd, int32_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (client_sessions_.find(client_fd) != client_sessions_.end()) {
        throw std::runtime_error("Client already in a session");
    }
    
    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        throw std::runtime_error("Invalid session ID");
    }
    
    session_it->second->addClient(client_fd);
    client_sessions_[client_fd] = session_id;
    
    LOG_INFO("[SessionManager] Client ", client_fd, " joined session ", session_id,
             " (current clients: ", session_it->second->getClientCount(), ")");
}

void SessionManager::removeSession(int32_t client_fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = client_sessions_.find(client_fd);
    if (it == client_sessions_.end()) {
        return;
    }
    
    int32_t session_id = it->second;
    auto session_it = sessions_.find(session_id);
    if (session_it != sessions_.end()) {
        session_it->second->removeClient(client_fd);
        
        if (session_it->second->getClientCount() == 0) {
            LOG_DEBUG("[SessionManager] Removing empty session ", session_id);
            sessions_.erase(session_it);
        }
    }
    
    client_sessions_.erase(it);
    LOG_INFO("[SessionManager] Removed client ", client_fd, " from session ", session_id);
}

std::shared_ptr<Session> SessionManager::getSession(int32_t client_fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = client_sessions_.find(client_fd);
    if (it == client_sessions_.end()) {
        return nullptr;
    }
    
    auto session_it = sessions_.find(it->second);
    if (session_it == sessions_.end()) {
        return nullptr;
    }
    
    return session_it->second;
}

const std::set<int32_t>& SessionManager::getSessionClients(int32_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        static const std::set<int32_t> empty_set;
        return empty_set;
    }
    
    return it->second->getClients();
}

IOUring* SessionManager::getSessionIOUring(int32_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return nullptr;
    }
    
    return it->second->getIOUring();
}

std::shared_ptr<Session> SessionManager::getSessionByIndex(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (index >= sessions_.size()) {
        return nullptr;
    }
    
    auto it = sessions_.begin();
    std::advance(it, index);
    return it->second;
} 