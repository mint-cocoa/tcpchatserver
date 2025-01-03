#include "UringBuffer.h"
#include "Logger.h"
#include <sys/mman.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <iostream>
#include "Utils.h"

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
    return (UringBuffer::IO_BUFFER_SIZE + sizeof(io_uring_buf)) * UringBuffer::NUM_IO_BUFFERS;
}

static uint8_t* get_buffer_base_addr(void* ring_addr) {
    return static_cast<uint8_t*>(ring_addr) + (sizeof(io_uring_buf) * UringBuffer::NUM_IO_BUFFERS);
}

uint8_t* UringBuffer::getBufferAddr(uint16_t idx, uint8_t* buf_base_addr) {
    return buf_base_addr + (idx << log2<UringBuffer::IO_BUFFER_SIZE>());
}

UringBuffer::UringBuffer(io_uring* ring) 
    : ring_(ring), buf_ring_(nullptr), buffer_base_addr_(nullptr), ring_size_(buffer_ring_size()),
      buffers_(NUM_IO_BUFFERS) {
    initBufferRing();
}

UringBuffer::~UringBuffer() {
    if (buf_ring_) {
        munmap(buf_ring_, ring_size_);
    }
}

void UringBuffer::initBufferRing() {
    void* ring_addr = mmap(nullptr, ring_size_, 
                          PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE,
                          -1, 0);
    if (ring_addr == MAP_FAILED) {
        throw std::runtime_error("Failed to mmap buffer ring");
    }

    io_uring_buf_reg reg{};
    reg.ring_addr = reinterpret_cast<__u64>(ring_addr);
    reg.ring_entries = NUM_IO_BUFFERS;
    reg.bgid = 1;  // Buffer group ID

    if (io_uring_register_buf_ring(ring_, &reg, 0) < 0) {
        munmap(ring_addr, ring_size_);
        throw std::runtime_error("Failed to register buffer ring");
    }

    buf_ring_ = static_cast<io_uring_buf_ring*>(ring_addr);
    io_uring_buf_ring_init(buf_ring_);

    buffer_base_addr_ = get_buffer_base_addr(ring_addr);

    // 버퍼 정보 초기화
    for (uint16_t i = 0; i < NUM_IO_BUFFERS; ++i) {
        buffers_[i].in_use = false;
        buffers_[i].client_fd = 0;
        buffers_[i].allocation_time = std::chrono::steady_clock::now();
        buffers_[i].bytes_used = 0;
        buffers_[i].total_uses = 0;
        buffers_[i].ref_count = 0;
        
        // 버퍼를 io_uring에 등록
        io_uring_buf_ring_add(buf_ring_, 
                             getBufferAddr(i, buffer_base_addr_), 
                             IO_BUFFER_SIZE, 
                             i,
                             io_uring_buf_ring_mask(NUM_IO_BUFFERS), 
                             i);
    }
    io_uring_buf_ring_advance(buf_ring_, NUM_IO_BUFFERS);
        
}


void UringBuffer::markBufferInUse(uint16_t idx, uint16_t client_fd) {
    if (idx >= NUM_IO_BUFFERS) return;
    
    buffers_[idx].in_use = true;
    buffers_[idx].client_fd = client_fd;
    buffers_[idx].allocation_time = std::chrono::steady_clock::now();
    buffers_[idx].total_uses++;
    
    LOG_DEBUG("[Buffer] Session buffer #", idx, " allocated -> client ", client_fd,
              " (total uses: ", buffers_[idx].total_uses, ")");
    client_buffers_[client_fd] = idx;

    printBufferStatus(idx);
}

void UringBuffer::releaseBuffer(uint16_t idx, uint8_t* buf_base_addr) {
    if (idx >= NUM_IO_BUFFERS) {
        LOG_ERROR("[Buffer] Invalid buffer index ", idx, " release attempt");
        return;
    }
    
    if (!buffers_[idx].in_use) {
        LOG_WARN("[Buffer] Buffer #", idx, " already released");
        return;
    }
    
    if (buffers_[idx].ref_count > 0) {
        LOG_DEBUG("[Buffer] Buffer #", idx, " ref_count=", buffers_[idx].ref_count,
                 ", release pending");
        return;
    }
    
    uint16_t client_fd = buffers_[idx].client_fd;
    auto usage_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - buffers_[idx].allocation_time
    ).count();

    LOG_DEBUG("[Buffer] Session buffer #", idx, " released <- client ", client_fd,
             "\n\tBytes used: ", buffers_[idx].bytes_used,
             "\n\tUsage time: ", usage_time, "ms",
             "\n\tTotal uses: ", buffers_[idx].total_uses);

    buffers_[idx].in_use = false;
    client_buffers_.erase(buffers_[idx].client_fd);
    buffers_[idx].client_fd = 0;
    buffers_[idx].bytes_used = 0;
    
    io_uring_buf_ring_add(buf_ring_, getBufferAddr(idx, buf_base_addr), IO_BUFFER_SIZE, idx,
                         io_uring_buf_ring_mask(NUM_IO_BUFFERS), 0);
    io_uring_buf_ring_advance(buf_ring_, 1);

    printBufferStatus();
}

void UringBuffer::printBufferStatus(uint16_t highlight_idx) {
    size_t total_in_use = 0;
    size_t total_bytes_used = 0;
    
    for (size_t i = 0; i < NUM_IO_BUFFERS; ++i) {
        if (buffers_[i].in_use) {
            total_in_use++;
            total_bytes_used += buffers_[i].bytes_used;
        }
    }
    
    LOG_DEBUG("[Buffer Status]",
              "\n\tTotal buffers: ", NUM_IO_BUFFERS,
              "\n\tBuffers in use: ", total_in_use,
              "\n\tAvailable buffers: ", (NUM_IO_BUFFERS - total_in_use),
              "\n\tTotal bytes in use: ", total_bytes_used);

    if (highlight_idx != UINT16_MAX) {
        LOG_DEBUG("[Buffer #", highlight_idx, " Details]",
                 "\n\tIn use: ", (buffers_[highlight_idx].in_use ? "yes" : "no"),
                 "\n\tClient: ", buffers_[highlight_idx].client_fd,
                 "\n\tBytes used: ", buffers_[highlight_idx].bytes_used,
                 "\n\tTotal uses: ", buffers_[highlight_idx].total_uses,
                 "\n\tRef count: ", buffers_[highlight_idx].ref_count);
    }
}

void UringBuffer::updateBufferBytes(uint16_t idx, uint64_t bytes) {
    if (idx >= NUM_IO_BUFFERS) return;
    
    buffers_[idx].bytes_used = bytes;
    LOG_DEBUG("[Buffer] Buffer #", idx, " usage updated: ", 
              bytes, " bytes (client ", buffers_[idx].client_fd, ")");
}

void UringBuffer::incrementRefCount(uint16_t idx) {
    if (idx >= NUM_IO_BUFFERS) return;
    
    buffers_[idx].ref_count++;
    LOG_TRACE("[Buffer] Buffer #", idx, " ref_count increased: ", 
              buffers_[idx].ref_count);
}

void UringBuffer::decrementRefCount(uint16_t idx) {
    if (idx >= NUM_IO_BUFFERS) return;
    
    if (buffers_[idx].ref_count > 0) {
        buffers_[idx].ref_count--;
        LOG_TRACE("[Buffer] Buffer #", idx, " ref_count decreased: ", 
                  buffers_[idx].ref_count);
    }
}

bool UringBuffer::isBufferInUse(uint16_t idx) const {
    return idx < NUM_IO_BUFFERS && buffers_[idx].in_use;
}

uint16_t UringBuffer::getBufferClient(uint16_t idx) const {
    return idx < NUM_IO_BUFFERS ? buffers_[idx].client_fd : 0;
}

uint64_t UringBuffer::getBufferBytesUsed(uint16_t idx) const {
    return idx < NUM_IO_BUFFERS ? buffers_[idx].bytes_used : 0;
}


uint16_t UringBuffer::findClientBuffer(uint16_t client_fd) const {
    auto it = client_buffers_.find(client_fd);
    if (it != client_buffers_.end()) {
        LOG_TRACE("[Buffer] Found buffer #", it->second, " for client ", client_fd);
        return it->second;
    }
    LOG_DEBUG("[Buffer] No buffer found for client ", client_fd);
    return UINT16_MAX;
}



