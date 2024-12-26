#include "SessionManager.h"
#include <iostream>

SessionManager& SessionManager::getInstance() {
    static SessionManager instance;
    return instance;
}

void SessionManager::addSession(int32_t clientFd) {
    auto session = std::make_shared<Session>(clientFd);
    sessions[clientFd] = session;
    std::cout << "클라이언트 " << clientFd << " 세션 생성됨" << std::endl;
}

void SessionManager::removeSession(int32_t clientFd) {
    auto session = getSession(clientFd);
    if (session) {
        int32_t sessionId = session->getSessionId();
        if (sessionId >= 0) {
            auto it = sessionGroups.find(sessionId);
            if (it != sessionGroups.end()) {
                it->second.erase(clientFd);
                std::cout << "클라이언트 " << clientFd << "가 세션 " << sessionId << "에서 제거됨" << std::endl;
            }
        }
    }
    sessions.erase(clientFd);
}

std::shared_ptr<Session> SessionManager::getSession(int32_t clientFd) {
    auto it = sessions.find(clientFd);
    return (it != sessions.end()) ? it->second : nullptr;
}

std::vector<int32_t> SessionManager::getSessionIds() const {
    std::vector<int32_t> ids;
    for (const auto& pair : sessionGroups) {
        ids.push_back(pair.first);
    }
    return ids;
}

void SessionManager::joinSession(int32_t clientFd, int32_t sessionId) {
    auto session = getSession(clientFd);
    if (session) {
        // 클라이언트가 이미 세션에 속해 있는 경우, 이전 세션에서 제거
        int32_t oldSessionId = session->getSessionId();
        if (oldSessionId >= 0) {
            auto it = sessionGroups.find(oldSessionId);
            if (it != sessionGroups.end()) {
                it->second.erase(clientFd);
                std::cout << "클라이언트 " << clientFd << "가 세션 " << oldSessionId << "에서 제거됨" << std::endl;
            }
        }
        // 클라이언트를 새 세션에 추가
        session->setSessionId(sessionId);
        sessionGroups[sessionId].insert(clientFd);
        std::cout << "클라이언트 " << clientFd << "가 세션 " << sessionId << "에 참가됨" << std::endl;
    } else {
        // 클라이언트 세션이 없는 경우, 새 세션 생성
        auto newSession = std::make_shared<Session>(clientFd);
        newSession->setSessionId(sessionId);
        sessions[clientFd] = newSession;
        sessionGroups[sessionId].insert(clientFd);
        std::cout << "클라이언트 " << clientFd << "를 위한 새 세션 생성 및 세션 " << sessionId << "에 추가됨" << std::endl;
    }
}

// 새로운 메서드 추가: 세션 생성
void SessionManager::createSession(int32_t sessionId) {
    if (sessionGroups.find(sessionId) == sessionGroups.end()) {
        sessionGroups[sessionId] = std::unordered_set<int32_t>();
        std::cout << "새 세션 생성됨: " << sessionId << std::endl;
    }
}

std::unordered_set<int32_t> SessionManager::getSessionClients(int32_t sessionId) {
    auto it = sessionGroups.find(sessionId);
    if (it != sessionGroups.end()) {
        return it->second;
    }
    return std::unordered_set<int32_t>();
} 