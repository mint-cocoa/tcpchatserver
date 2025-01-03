#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "ChatClient.h"

struct TestMessage {
    uint64_t message_id;
    std::chrono::steady_clock::time_point send_time;
    size_t client_id;
};

struct Stats {
    std::atomic<uint64_t> messages_sent{0};
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> total_latency_ms{0};
    std::atomic<uint64_t> message_id_counter{0};
    std::mutex mutex;
    std::unordered_map<uint64_t, TestMessage> pending_messages;
};

// 속도 제어를 위한 RateLimiter 클래스 추가
class RateLimiter {
public:
    explicit RateLimiter(uint32_t messages_per_second) 
        : messages_per_second_(messages_per_second)
        , interval_ms_(1000.0 / messages_per_second)
        , last_send_time_(std::chrono::steady_clock::now()) {}

    void wait() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_send_time_).count();
        
        if (elapsed < interval_ms_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<int64_t>(interval_ms_ - elapsed)));
        }
        last_send_time_ = std::chrono::steady_clock::now();
    }

private:
    uint32_t messages_per_second_;
    double interval_ms_;
    std::chrono::steady_clock::time_point last_send_time_;
};

void print_usage(const char* program) {
    std::cout << "Echo 벤치마크\n\n"
              << "사용법:\n"
              << "  " << program << " [옵션들]\n\n"
              << "옵션들:\n"
              << "  -h, --help                도움말 출력\n"
              << "  -a, --address <주소>      대상 서버 주소 (기본값: 127.0.0.1:8080)\n"
              << "  -c, --clients <개수>      클라이언트 수 (기본값: 50)\n"
              << "  -s, --size <크기>         메시지 크기 (기본값: 512)\n"
              << "  -d, --duration <시간>     테스트 시간(초) (기본값: 60)\n"
              << "  -r, --rate <속도>         클라이언트당 초당 메시지 수 (기본값: 2)\n"
              << std::endl;
}

void run_client(const std::string& address, 
               int port,
               size_t msg_size,
               uint32_t messages_per_second,
               std::atomic<bool>& stop_flag,
               Stats& stats,
               int client_id,
               int grace_period) {
    ChatClient client;
    RateLimiter rate_limiter(messages_per_second);
    bool session_joined = false;
    
    // 메시지 수신 콜백 설정
    client.setMessageCallback([&stats, client_id](const std::string& msg) {
        try {
            // test_message_ 접두사 확인
            if (msg.find("test_message_") != 0) {
                return;  // test_message로 시작하지 않는 메시지는 무시
            }
            
            // 메시지 파싱 (접두사 제외)
            std::string content = msg.substr(13);  // "test_message_" 길이는 13
            size_t pos = content.find("msg_id:");
            if (pos != std::string::npos) {
                uint64_t msg_id = std::stoull(content.substr(pos + 7));
                
                std::lock_guard<std::mutex> lock(stats.mutex);
                auto it = stats.pending_messages.find(msg_id);
                if (it != stats.pending_messages.end()) {
                    // 지연 시간 계산
                    auto now = std::chrono::steady_clock::now();
                    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->second.send_time).count();
                    
                    stats.total_latency_ms += latency;
                    stats.messages_received++;
                    stats.pending_messages.erase(it);  // 처리된 메시지 삭제
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "메시지 파싱 오류: " << e.what() << std::endl;
        }
    });

    // 서버 연결
    if (!client.connect(address, port)) {
        std::cerr << "클라이언트 " << client_id << " 연결 실패" << std::endl;
        return;
    }

    // 세션 참여
    if (!client.joinSession(1)) {
        std::cerr << "클라이언트 " << client_id << " 세션 참여 실패" << std::endl;
        client.disconnect();
        return;
    }
    session_joined = true;

    while (!stop_flag) {
        rate_limiter.wait();
        
        // 새 메시지 ID 생성
        uint64_t msg_id = ++stats.message_id_counter;
        
        // 메시지 생성 및 전송
        std::string message = "test_message_msg_id:" + std::to_string(msg_id) + 
                            ",client:" + std::to_string(client_id) + 
                            ",data:" + std::string(msg_size - 50, 'a');

        if (client.sendChat(message)) {
            TestMessage test_msg{
                msg_id,
                std::chrono::steady_clock::now(),
                static_cast<size_t>(client_id)
            };
            
            {
                std::lock_guard<std::mutex> lock(stats.mutex);
                stats.pending_messages[msg_id] = test_msg;
            }
            stats.messages_sent++;
        }
    }

    // stop_flag가 true가 되면 메시지 전송은 중단하지만,
    // 수신 대기는 계속 유지 (receiveThread는 계속 동작)
    std::this_thread::sleep_for(std::chrono::seconds(grace_period));

    // grace period 이후에 정상적으로 종료
    if (session_joined) {
        client.leaveSession();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    client.disconnect();
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <host> <port>" << std::endl;
        return 1;
    }

    const char* host = argv[1];
    int port = std::stoi(argv[2]);
    
    try {
        ChatClient client;
        if (!client.connect(host, port)) {
            std::cerr << "Failed to connect to server" << std::endl;
            return 1;
        }

        // 세션 1에 조인
        if (!client.joinSession(1)) {
            std::cerr << "Failed to join session" << std::endl;
            return 1;
        }

        // 메시지 수신을 위한 루프
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
} 