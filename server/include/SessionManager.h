#pragma once
#include "Session.h"
#include <unordered_set>
#include "IOUringManager.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include <set>

class SessionManager {
public:
    static SessionManager& getInstance();
    void addSession(int32_t clientFd);
    void removeSession(int32_t clientFd);
    std::shared_ptr<Session> getSession(int32_t clientFd);
    std::vector<int32_t> getSessionIds() const;
    void joinSession(int32_t clientFd, int32_t sessionId);
    void broadcastToSession(int32_t sessionId, const void* data, size_t length, IOUringManager& ioManager);
    std::unordered_set<int32_t> getSessionClients(int32_t sessionId);
    void createSession(int32_t sessionId);

private:
    SessionManager() = default;
    std::unordered_map<int32_t, std::shared_ptr<Session>> sessions;
    std::unordered_map<int32_t, std::unordered_set<int32_t>> sessionGroups;
};