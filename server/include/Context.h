#pragma once
#include <liburing.h>
#include <cstdint>

#pragma pack(push, 1)  // 1바이트 정렬 시작

enum class MessageType : uint8_t {
    // 서버 메시지 (0x00 ~ 0x0F)
    SERVER_ACK = 0x01,           // 일반적인 응답
    SERVER_ERROR = 0x02,         // 에러
    SERVER_CHAT = 0x03,          // 채팅 메시지
    SERVER_NOTIFICATION = 0x04,  // 시스템 알림
    
    // 클라이언트 메시지 (0x10 ~ 0x1F)
    CLIENT_JOIN = 0x11,          // 세션 참가
    CLIENT_LEAVE = 0x12,         // 세션 퇴장
    CLIENT_CHAT = 0x13,          // 채팅 메시지
    CLIENT_COMMAND = 0x14        // 명령어 (상태변경, 귓속말 등)
};

enum class OperationType : uint8_t {
    ACCEPT = 1,
    READ = 2,
    WRITE = 3,
    CLOSE = 4
};

// 서버 내부에서 사용하는 작업 컨텍스트
struct Operation {
    int32_t client_fd;        // 4 bytes
    OperationType op_type;    // 1 byte
    uint16_t buffer_idx;      // 2 bytes
};

// 실제 네트워크로 전송되는 메시지 구조체
struct ChatMessage {
    MessageType type;         // 1 byte
    uint16_t length;         // 2 bytes
    char data[512];          // 512 bytes
};

#pragma pack(pop)   // 정렬 설정 복원

static constexpr size_t MAX_MESSAGE_SIZE = 4096;  // 4KB