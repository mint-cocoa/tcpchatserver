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

struct BufferInfo {
    bool in_use{false};                    // 버퍼 사용 중 여부
    uint16_t client_fd{0};                // 버퍼를 사용 중인 클라이언트의 파일 디스크립터
    std::chrono::steady_clock::time_point allocation_time{};  // 버퍼 할당 시간
    uint64_t bytes_used{0};               // 현재 사용 중인 바이트 수
    uint64_t total_uses{0};               // 총 사용 횟수
    uint32_t ref_count{0};               // 레퍼런스 카운트

    BufferInfo() = default;
    BufferInfo(const BufferInfo&) = delete;
    BufferInfo& operator=(const BufferInfo&) = delete;
    BufferInfo(BufferInfo&&) = delete;
    BufferInfo& operator=(BufferInfo&&) = delete;
};

class UringBuffer {
public:
 
   
    static constexpr unsigned IO_BUFFER_SIZE = 2048;
    // The number of IO buffers to pre-allocate
    static constexpr uint16_t NUM_IO_BUFFERS = 4096;


    // 생성자 및 소멸자
    explicit UringBuffer(io_uring* ring);
    ~UringBuffer();

    // 버퍼 관리 메서드
    void markBufferInUse(uint16_t idx, uint16_t client_fd);  // completion queue에서 사용된 버퍼 표시
    void releaseBuffer(uint16_t idx, uint8_t* buf_base_addr);                        // 버퍼 사용 완료 표시
    uint8_t* getBufferAddr(uint16_t idx, uint8_t* buf_base_addr);                   // 버퍼 주소 반환
    void updateBufferBytes(uint16_t idx, uint64_t bytes);   // 버퍼 사용량 업데이트
    void printBufferStatus(uint16_t highlight_idx = UINT16_MAX); // 버퍼 상태 출력
    
    // 버퍼 상태 조회 메서드
    bool isBufferInUse(uint16_t idx) const;
    uint16_t getBufferClient(uint16_t idx) const;
    uint64_t getBufferBytesUsed(uint16_t idx) const;
    double getBufferUsageTime(uint16_t idx) const;
    void printBufferStats() const;
    uint16_t findClientBuffer(uint16_t client_fd) const;
    
    // 버퍼 기본 주소 반환
    uint8_t* getBaseAddr() const { return buffer_base_addr_; }

    // 로그 관련 메서드
  
       

    // 레퍼런스 카운트 관련 메서드 추가
    void incrementRefCount(uint16_t idx);
    void decrementRefCount(uint16_t idx);
    uint32_t getRefCount(uint16_t idx) const { return idx < NUM_IO_BUFFERS ? buffers_[idx].ref_count : 0; }

    // 복사 및 이동 생성자/대입 연산자 삭제
    UringBuffer(const UringBuffer&) = delete;
    UringBuffer& operator=(const UringBuffer&) = delete;
    UringBuffer(UringBuffer&&) = delete;
    UringBuffer& operator=(UringBuffer&&) = delete;

private:
    // 초기화 메서드
    void initBufferRing();
    

    // 멤버 변수
    io_uring* ring_;                // io_uring 인스턴스 (소유권 없음)
    io_uring_buf_ring* buf_ring_;   // 버퍼 링
    uint8_t* buffer_base_addr_;     // 버퍼 메모리 시작 주소
    const unsigned ring_size_;      // 전체 버퍼 링 크기
    unsigned ring_mask_;           // 버퍼 링 마스크
    std::vector<BufferInfo> buffers_;                    // 버퍼 정보 배열
    std::unordered_map<uint16_t, uint16_t> client_buffers_;  // client_fd -> buffer_idx 매핑
}; 