#include "BufferManager.h"
#include <sys/mman.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <mutex>

template <unsigned N> constexpr bool is_power_of_two() {
    static_assert(N <= 32768, "N must be N <= 32768");
    return (N == 1 || N == 2 || N == 4 || N == 8 || N == 16 || N == 32 || N == 64 || N == 128 || N == 256 ||
            N == 512 || N == 1024 || N == 2048 || N == 4096 || N == 8192 || N == 16384 || N == 32768);
}

template <unsigned N> constexpr unsigned log2() {
    static_assert(is_power_of_two<N>(), "N must be a power of 2");
    unsigned val = N;
    unsigned ret = 0;
    while (val > 1) {
        val >>= 1;
        ret++;
    }
    return ret;
}

static constexpr unsigned buffer_ring_size() {
    return (BufferManager::IO_BUFFER_SIZE + sizeof(io_uring_buf)) * BufferManager::NUM_BUFFERS;
}

static uint8_t* get_buffer_base_addr(void* ring_addr) {
    return static_cast<uint8_t*>(ring_addr) + (sizeof(io_uring_buf) * BufferManager::NUM_BUFFERS);
}

uint8_t* BufferManager::getBufferAddr(uint16_t idx) {
    return buffer_base_addr_ + (idx << log2<BufferManager::IO_BUFFER_SIZE>());
}

BufferManager::BufferManager(io_uring* ring) 
    : ring_(ring), buf_ring_(nullptr), buffer_base_addr_(nullptr), ring_size_(buffer_ring_size()),
      buffers_(NUM_BUFFERS) {
    initBufferRing();
}

BufferManager::~BufferManager() {
    if (buf_ring_) {
        munmap(buf_ring_, ring_size_);
    }
}

void BufferManager::initBufferRing() {
    void* ring_addr = mmap(nullptr, ring_size_, 
                          PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE,
                          -1, 0);
    if (ring_addr == MAP_FAILED) {
        logError("Failed to mmap buffer ring");
        throw std::runtime_error("Failed to mmap buffer ring");
    }

    io_uring_buf_reg reg{};
    reg.ring_addr = reinterpret_cast<__u64>(ring_addr);
    reg.ring_entries = NUM_BUFFERS;
    reg.bgid = 1;  // Buffer group ID

    if (io_uring_register_buf_ring(ring_, &reg, 0) < 0) {
        munmap(ring_addr, ring_size_);
        logError("Failed to register buffer ring");
        throw std::runtime_error("Failed to register buffer ring");
    }

    buf_ring_ = static_cast<io_uring_buf_ring*>(ring_addr);
    io_uring_buf_ring_init(buf_ring_);

    buffer_base_addr_ = get_buffer_base_addr(ring_addr);

    // 버퍼 정보 초기화
    for (uint16_t i = 0; i < NUM_BUFFERS; ++i) {
        buffers_[i].in_use = false;
        buffers_[i].client_fd = 0;
        buffers_[i].allocation_time = std::chrono::steady_clock::now();
        buffers_[i].bytes_used = 0;
        buffers_[i].total_uses = 0;
        buffers_[i].ref_count = 0;
        
        // 버퍼를 io_uring에 등록
        io_uring_buf_ring_add(buf_ring_, 
                             getBufferAddr(i), 
                             IO_BUFFER_SIZE, 
                             i,
                             io_uring_buf_ring_mask(NUM_BUFFERS), 
                             i);
    }
    io_uring_buf_ring_advance(buf_ring_, NUM_BUFFERS);
    
    logInfo("Buffer ring initialized with " + std::to_string(NUM_BUFFERS) + " buffers");
}

void BufferManager::incrementRefCount(uint16_t idx) {
    if (idx >= NUM_BUFFERS) return;
    
    std::lock_guard<std::mutex> lock(buffers_[idx].ref_mutex);
    buffers_[idx].ref_count++;
    logDebug("Buffer " + std::to_string(idx) + " ref count increased to " + 
             std::to_string(buffers_[idx].ref_count));
}

void BufferManager::decrementRefCount(uint16_t idx) {
    if (idx >= NUM_BUFFERS) return;
    
    std::lock_guard<std::mutex> lock(buffers_[idx].ref_mutex);
    if (buffers_[idx].ref_count > 0) {
        buffers_[idx].ref_count--;
        logDebug("Buffer " + std::to_string(idx) + " ref count decreased to " + 
                 std::to_string(buffers_[idx].ref_count));
        
        if (buffers_[idx].ref_count == 0 && buffers_[idx].in_use) {
            releaseBuffer(idx);
        }
    }
}

uint32_t BufferManager::getRefCount(uint16_t idx) const {
    if (idx >= NUM_BUFFERS) return 0;
    
    std::lock_guard<std::mutex> lock(buffers_[idx].ref_mutex);
    return buffers_[idx].ref_count;
}

void BufferManager::markBufferInUse(uint16_t idx, uint16_t client_fd) {
    if (idx >= NUM_BUFFERS) return;
    
    std::lock_guard<std::mutex> lock(buffers_[idx].ref_mutex);
    buffers_[idx].in_use = true;
    buffers_[idx].client_fd = client_fd;
    buffers_[idx].allocation_time = std::chrono::steady_clock::now();
    buffers_[idx].total_uses++;
    buffers_[idx].ref_count = 1;  // 초기 레퍼런스 카운트 설정
    
    client_buffers_[client_fd] = idx;
    
    logInfo("Buffer " + std::to_string(idx) + " marked in use by client " + 
            std::to_string(client_fd));
}

void BufferManager::releaseBuffer(uint16_t idx) {
    if (idx >= NUM_BUFFERS) return;
    
    std::lock_guard<std::mutex> lock(buffers_[idx].ref_mutex);
    if (buffers_[idx].in_use) {
        if (buffers_[idx].ref_count > 0) {
            logDebug("Buffer " + std::to_string(idx) + " release delayed, ref count: " + 
                     std::to_string(buffers_[idx].ref_count));
            return;
        }
        
        buffers_[idx].in_use = false;
        client_buffers_.erase(buffers_[idx].client_fd);
        buffers_[idx].client_fd = 0;
        buffers_[idx].bytes_used = 0;
        
        // 버퍼 링에 반환
        io_uring_buf_ring_add(buf_ring_, 
                            buffer_base_addr_ + (idx * IO_BUFFER_SIZE), 
                            IO_BUFFER_SIZE, 
                            idx, 
                            ring_mask_, 
                            idx);
        io_uring_buf_ring_advance(buf_ring_, 1);
        
        logInfo("Buffer " + std::to_string(idx) + " released");
    }
}

void BufferManager::updateBufferBytes(uint16_t idx, uint64_t bytes) {
    if (idx >= NUM_BUFFERS) {
        logError("Attempting to update invalid buffer index: " + std::to_string(idx));
        return;
    }

    if (!buffers_[idx].in_use) {
        logWarning("Attempting to update unused buffer: " + std::to_string(idx));
        return;
    }

    uint64_t old_bytes = buffers_[idx].bytes_used;
    buffers_[idx].bytes_used = bytes;
    
    // 사용량이 크게 변경된 경우에만 로그
    if (bytes > old_bytes + 1024 || bytes < old_bytes) {  // 1KB 이상 증가 또는 감소
        std::stringstream ss;
        ss << "Buffer " << idx << " usage updated: " << old_bytes << "B -> " << bytes << "B "
           << "(client: " << buffers_[idx].client_fd << ", "
           << "capacity: " << std::fixed << std::setprecision(1) 
           << (bytes * 100.0 / IO_BUFFER_SIZE) << "%)";
        logDebug(ss.str());
    }
        
    if (bytes > IO_BUFFER_SIZE) {
        std::stringstream ss;
        ss << "Buffer " << idx << " overflow: " << bytes << "/" << IO_BUFFER_SIZE 
           << "B (client: " << buffers_[idx].client_fd << ")";
        logWarning(ss.str());
    } else if (bytes > (IO_BUFFER_SIZE * 0.9)) {  // 90% 이상 용
        std::stringstream ss;
        ss << "Buffer " << idx << " near capacity: " << bytes << "/" << IO_BUFFER_SIZE 
           << "B (client: " << buffers_[idx].client_fd << ")";
        logWarning(ss.str());
    }
}

bool BufferManager::isBufferInUse(uint16_t idx) const {
    return idx < NUM_BUFFERS && buffers_[idx].in_use;
}

uint16_t BufferManager::getBufferClient(uint16_t idx) const {
    return idx < NUM_BUFFERS ? buffers_[idx].client_fd : 0;
}

uint64_t BufferManager::getBufferBytesUsed(uint16_t idx) const {
    return idx < NUM_BUFFERS ? buffers_[idx].bytes_used : 0;
}

double BufferManager::getBufferUsageTime(uint16_t idx) const {
    if (idx >= NUM_BUFFERS || !buffers_[idx].in_use) {
        return 0.0;
    }
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - buffers_[idx].allocation_time
    ).count();
    return duration / 1000.0;  // 초 단위로 변환
}

uint16_t BufferManager::findClientBuffer(uint16_t client_fd) const {
    auto it = client_buffers_.find(client_fd);
    return it != client_buffers_.end() ? it->second : UINT16_MAX;
}

void BufferManager::printBufferStatus(uint16_t highlight_idx) {
    int used_count = std::count_if(buffers_.begin(), buffers_.end(), 
                                 [](const BufferInfo& info) { return info.in_use; });
    
    std::stringstream ss;
    ss << "Buffer Status Update - Using: " << used_count << "/" << NUM_BUFFERS;
    logInfo(ss.str());
    
    // 하이라이트된 버퍼 정보 출력
    if (highlight_idx < NUM_BUFFERS) {
        const auto& info = buffers_[highlight_idx];
        std::stringstream buf_ss;
        buf_ss << "Buffer " << highlight_idx << " details:"
               << " Client=" << info.client_fd
               << " Time=" << std::fixed << std::setprecision(2) << getBufferUsageTime(highlight_idx) << "s"
               << " Bytes=" << info.bytes_used << "/" << IO_BUFFER_SIZE
               << " Uses=" << info.total_uses;
        logInfo(buf_ss.str());
    }

    // 활성 버퍼들의 상태를 로그로 출력
    std::stringstream active_ss;
    active_ss << "Active buffers: ";
    bool first = true;
    for (uint16_t i = 0; i < NUM_BUFFERS; ++i) {
        if (buffers_[i].in_use) {
            if (!first) active_ss << ", ";
            active_ss << i << "(" << buffers_[i].client_fd << ")";
            first = false;
        }
    }
    logDebug(active_ss.str());
}

void BufferManager::printBufferStats() const {
    std::stringstream ss;
    int used_count = std::count_if(buffers_.begin(), buffers_.end(), 
                                 [](const BufferInfo& info) { return info.in_use; });
    uint64_t total_bytes = 0;
    uint64_t total_uses = 0;
    
    for (const auto& info : buffers_) {
        total_bytes += info.bytes_used;
        total_uses += info.total_uses;
    }
    
    ss << "\n=== Buffer Pool Statistics ===\n"
       << "Total buffers: " << NUM_BUFFERS << "\n"
       << "In use: " << used_count << "\n"
       << "Total memory: " << (NUM_BUFFERS * IO_BUFFER_SIZE / 1024) << "KB\n"
       << "Used memory: " << (total_bytes / 1024) << "KB\n"
       << "Total allocations: " << total_uses;
    
    logInfo(ss.str());
    
    // 활성 버퍼 상세 정보
    for (uint16_t i = 0; i < NUM_BUFFERS; ++i) {
        const auto& info = buffers_[i];
        if (info.in_use) {
            std::stringstream buf_ss;
            buf_ss << "Buffer " << i << ": "
                  << "client " << info.client_fd
                  << ", time " << std::fixed << std::setprecision(2) 
                  << getBufferUsageTime(i) << "s"
                  << ", bytes " << info.bytes_used
                  << ", uses " << info.total_uses;
            logDebug(buf_ss.str());
        }
    }
}


