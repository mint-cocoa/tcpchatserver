#pragma once
#include "Session.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <queue>
#include <condition_variable>

class SessionManager {
public:
    static SessionManager& getInstance() {
        static SessionManager instance;
        return instance;
    }

    void initialize();
    void start();
    void stop();
    
    int32_t getNextAvailableSession();
    void joinSession(int32_t client_fd, int32_t session_id);
    void removeSession(int32_t client_fd);
    std::shared_ptr<Session> getSession(int32_t client_fd);
    std::shared_ptr<Session> getSessionByIndex(size_t index);
    const std::set<int32_t>& getSessionClients(int32_t session_id);
    IOUring* getSessionIOUring(int32_t session_id);
    size_t getOptimalThreadCount() const;

private:
    SessionManager();
    ~SessionManager();
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    void workerThread(size_t thread_id);
    void distributeSessionsToThreads();

    std::unordered_map<int32_t, std::shared_ptr<Session>> sessions_;  // session_id -> Session
    std::unordered_map<int32_t, int32_t> client_sessions_;           // client_fd -> session_id
    
    // 쓰레드 관리
    std::vector<std::thread> worker_threads_;
    std::vector<std::vector<std::shared_ptr<Session>>> thread_sessions_;  // 각 쓰레드가 담당할 세션들
    std::mutex mutex_;
    std::atomic<bool> should_stop_{false};
    size_t next_session_id_{0};
    size_t num_worker_threads_{0};
};