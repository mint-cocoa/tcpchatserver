#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

class Session {
public:
    explicit Session(int32_t clientFd) : clientFd(clientFd), sessionId(-1) {}
    
    int32_t getClientFd() const { return clientFd; }
    int32_t getSessionId() const { return sessionId; }
    std::string getSessionName() const { return sessionName; }
    
    void setSessionId(int32_t id) { sessionId = id; }
    void setSessionName(const std::string& name) { sessionName = name; }
    
    void setData(const std::string& key, const std::string& value) {
        sessionData[key] = value;
    }
    
    std::string getData(const std::string& key) const {
        auto it = sessionData.find(key);
        return (it != sessionData.end()) ? it->second : "";
    }
    
private:
    int32_t clientFd;
    int32_t sessionId;
    std::string sessionName;
    std::unordered_map<std::string, std::string> sessionData;
}; 