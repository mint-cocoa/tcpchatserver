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
            // 메시지 파싱
            size_t pos = msg.find("msg_id:");
            if (pos != std::string::npos) {
                uint64_t msg_id = std::stoull(msg.substr(pos + 7));
                
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
        std::string message = "msg_id:" + std::to_string(msg_id) + 
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

int main(int argc, char* argv[]) {
    std::string address = "127.0.0.1";
    int port = 8080;
    int num_clients = 50;
    size_t msg_size = 512;
    int duration = 60;
    uint32_t messages_per_second = 2;  // 기본값: 클라이언트당 초당 2개 메시지

    // 커맨드 라인 인자 파싱
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-a" || arg == "--address") {
            if (i + 1 < argc) {
                std::string addr = argv[++i];
                size_t colon_pos = addr.find(':');
                if (colon_pos != std::string::npos) {
                    address = addr.substr(0, colon_pos);
                    port = std::stoi(addr.substr(colon_pos + 1));
                }
            }
        } else if (arg == "-c" || arg == "--clients") {
            if (i + 1 < argc) num_clients = std::stoi(argv[++i]);
        } else if (arg == "-s" || arg == "--size") {
            if (i + 1 < argc) msg_size = std::stoi(argv[++i]);
        } else if (arg == "-d" || arg == "--duration") {
            if (i + 1 < argc) duration = std::stoi(argv[++i]);
        } else if (arg == "-r" || arg == "--rate") {
            if (i + 1 < argc) messages_per_second = std::stoi(argv[++i]);
        }
    }

    std::cout << "벤치마크 설정:\n"
              << "서버 주소: " << address << ":" << port << "\n"
              << "클라이언트 수: " << num_clients << "\n"
              << "메시지 크기: " << msg_size << " bytes\n"
              << "테스트 시간: " << duration << " 초\n"
              << "클라이언트당 초당 메시지 수: " << messages_per_second << "\n"
              << std::endl;

    // 통계 및 제어 변수
    Stats stats;
    std::atomic<bool> stop_flag(false);
    std::vector<std::thread> client_threads;

    // 클라이언트 스레드 시작
    auto start_time = std::chrono::steady_clock::now();
    const int grace_period = 30;
    for (int i = 0; i < num_clients; i++) {
        client_threads.emplace_back(run_client,
                                  std::ref(address),
                                  port,
                                  msg_size,
                                  messages_per_second,
                                  std::ref(stop_flag),
                                  std::ref(stats),
                                  i + 1,
                                  grace_period);
    }

    // 진행 상황 모니터링
    for (int elapsed = 0; elapsed < duration; elapsed++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto current_sent = stats.messages_sent.load();
        auto current_received = stats.messages_received.load();
        size_t pending;
        {
            std::lock_guard<std::mutex> lock(stats.mutex);
            pending = stats.pending_messages.size();
        }
        
        std::cout << "\r진행 중: " << elapsed + 1 << "/" << duration 
                  << " 초, 전송: " << current_sent 
                  << ", 수신: " << current_received 
                  << ", 미응답: " << pending
                  << " (" << (current_sent / (elapsed + 1)) << " msg/s)"
                  << std::flush;
    }
    std::cout << std::endl;

    // 테스트 종료 - 메시지 전송만 중단
    stop_flag = true;  

    // 미응답 메시지 추적을 위한 추가 시간
    std::cout << "\n미응답 메시지 추적 중... " << std::flush;
    
    for (int i = 0; i < grace_period; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        size_t pending;
        {
            std::lock_guard<std::mutex> lock(stats.mutex);
            pending = stats.pending_messages.size();
        }
        std::cout << "\r미응답 메시지 추적 중... " << pending << " 개 남음 (" 
                  << (grace_period - i) << "초 남음)" << std::flush;
                  
        if (pending == 0) {
            std::cout << "\n모든 메시지 응답 완료!" << std::endl;
            break;
        }
    }
    std::cout << std::endl;

    // 모든 클라이언트에게 종료 신호 전달
    for (auto& thread : client_threads) {
        thread.join();
    }

    // 최종 결과 출력
    size_t pending_messages;
    std::unordered_map<size_t, TestMessage> unresponded_messages;
    {
        std::lock_guard<std::mutex> lock(stats.mutex);
        pending_messages = stats.pending_messages.size();
        if (pending_messages > 0) {
            unresponded_messages = stats.pending_messages;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
    auto total_sent = stats.messages_sent.load();
    auto total_received = stats.messages_received.load();
    auto total_latency = stats.total_latency_ms.load();

    std::cout << "\n벤치마크 결과:\n"
              << "총 실행 시간: " << total_time << " 초\n"
              << "총 전송 메시지: " << total_sent << "\n"
              << "총 수신 메시지: " << total_received << "\n"
              << "미응답 메시지: " << pending_messages << "\n"
              << "초당 전송량: " << (total_sent / total_time) << " messages/s\n"
              << "초당 수신량: " << (total_received / total_time) << " messages/s\n";

    if (total_received > 0) {
        std::cout << "평균 지연 시간: " << (total_latency / total_received) << " ms\n";
    }

    // 미응답 메시지 상세 정보 출력
    if (!unresponded_messages.empty()) {
        std::cout << "\n미응답 메시지 상세 정보:\n";
        for (const auto& [msg_id, msg] : unresponded_messages) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - msg.send_time).count();
            
            std::cout << "Message ID: " << msg_id 
                      << ", Client: " << msg.client_id
                      << ", 경과 시간: " << elapsed << "ms\n";
        }
    }

    return 0;
} 