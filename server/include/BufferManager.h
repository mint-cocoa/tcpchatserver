#pragma once

#include <vector>
#include <cstdint>
#include <liburing.h>
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <mutex>
#include <iomanip>

// 로그 레벨 정의
enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

class BufferLogger {
public:
    static BufferLogger& getInstance() {
        static BufferLogger instance;
        return instance;
    }

    void log(LogLevel level, const std::string& message, bool console = true) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count() % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S")
           << "." << std::setfill('0') << std::setw(3) << now_ms
           << " [" << getLevelString(level) << "] "
           << message << std::endl;

        if (console) {
            std::cout << getColorCode(level) << ss.str() << "\033[0m";
        }
        
        if (log_file_.is_open()) {
            log_file_ << ss.str();
            log_file_.flush();
        }
    }

    void setLogFile(const std::string& filename) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_file_.is_open()) {
            log_file_.close();
        }
        log_file_.open(filename, std::ios::app);
    }

private:
    BufferLogger() {
        setLogFile("buffer_manager.log");
    }
    
    ~BufferLogger() {
        if (log_file_.is_open()) {
            log_file_.close();
        }
    }

    static std::string getLevelString(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO: return "INFO";
            case LogLevel::WARNING: return "WARN";
            case LogLevel::ERROR: return "ERROR";
            default: return "UNKNOWN";
        }
    }

    static std::string getColorCode(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "\033[36m";   // Cyan
            case LogLevel::INFO: return "\033[32m";    // Green
            case LogLevel::WARNING: return "\033[33m";  // Yellow
            case LogLevel::ERROR: return "\033[31m";   // Red
            default: return "\033[0m";                 // Reset
        }
    }

    std::ofstream log_file_;
    std::mutex log_mutex_;
};

struct BufferInfo {
    bool in_use{false};                    // 버퍼 사용 중 여부
    uint16_t client_fd{0};                // 버퍼를 사용 중인 클라이언트의 파일 디스크립터
    std::chrono::steady_clock::time_point allocation_time{};  // 버퍼 할당 시간
    uint64_t bytes_used{0};               // 현재 사용 중인 바이트 수
    uint64_t total_uses{0};               // 총 사용 횟수
    uint32_t ref_count{0};               // 레퍼런스 카운트
    mutable std::mutex ref_mutex;         // 레퍼런스 카운트 보호용 뮤텍스

    BufferInfo() = default;
    BufferInfo(const BufferInfo&) = delete;
    BufferInfo& operator=(const BufferInfo&) = delete;
    BufferInfo(BufferInfo&&) = delete;
    BufferInfo& operator=(BufferInfo&&) = delete;
};

class BufferManager {
public:
    // 상수 정의
    static constexpr uint32_t IO_BUFFER_SIZE = 8192;  // 8KB
    static constexpr uint16_t NUM_BUFFERS = 256;      // 버퍼 개수

    // 생성자 및 소멸자
    explicit BufferManager(io_uring* ring);
    ~BufferManager();

    // 버퍼 관리 메서드
    void markBufferInUse(uint16_t idx, uint16_t client_fd);  // completion queue에서 사용된 버퍼 표시
    void releaseBuffer(uint16_t idx);                        // 버퍼 사용 완료 표시
    uint8_t* getBufferAddr(uint16_t idx);                   // 버퍼 주소 반환
    void updateBufferBytes(uint16_t idx, uint64_t bytes);   // 버퍼 사용량 업데이트
    void printBufferStatus(uint16_t highlight_idx = UINT16_MAX); // 버퍼 상태 출력
    
    // 버퍼 상태 조회 메서드
    bool isBufferInUse(uint16_t idx) const;
    uint16_t getBufferClient(uint16_t idx) const;
    uint64_t getBufferBytesUsed(uint16_t idx) const;
    double getBufferUsageTime(uint16_t idx) const;
    void printBufferStats() const;
    uint16_t findClientBuffer(uint16_t client_fd) const;

    // 로그 관련 메서드
    void setLogFile(const std::string& filename) {
        BufferLogger::getInstance().setLogFile(filename);
    }

    // 레퍼런스 카운트 관련 메서드 추가
    void incrementRefCount(uint16_t idx);
    void decrementRefCount(uint16_t idx);
    uint32_t getRefCount(uint16_t idx) const;

    // 복사 및 이동 생성자/대입 연산자 삭제
    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;
    BufferManager(BufferManager&&) = delete;
    BufferManager& operator=(BufferManager&&) = delete;

private:
    // 초기화 메서드
    void initBufferRing();
    
    // 로그 헬퍼 메서드
    void logDebug(const std::string& message) const {
        BufferLogger::getInstance().log(LogLevel::DEBUG, message);
    }
    
    void logInfo(const std::string& message) const {
        BufferLogger::getInstance().log(LogLevel::INFO, message);
    }
    
    void logWarning(const std::string& message) const {
        BufferLogger::getInstance().log(LogLevel::WARNING, message);
    }
    
    void logError(const std::string& message) const {
        BufferLogger::getInstance().log(LogLevel::ERROR, message);
    }
    
    // 멤버 변수
    io_uring* ring_;                // io_uring 인스턴스 (소유권 없음)
    io_uring_buf_ring* buf_ring_;   // 버퍼 링
    uint8_t* buffer_base_addr_;     // 버퍼 메모리 시작 주소
    const unsigned ring_size_;      // 전체 버퍼 링 크기
    unsigned ring_mask_;           // 버퍼 링 마스크
    std::vector<BufferInfo> buffers_;                    // 버퍼 정보 배열
    std::unordered_map<uint16_t, uint16_t> client_buffers_;  // client_fd -> buffer_idx 매핑
}; 