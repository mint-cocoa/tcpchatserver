#include "SocketManager.h"
#include "IOUringManager.h"
#include "BufferManager.h"
#include "EventHandler.h"
#include "SessionManager.h"
#include <iostream>
#include <csignal>
#include <unordered_map>
#include <unordered_set>
#include <cstring>

static bool running = true;

// 클라이언트 상태 관리를 위한 구조체
struct ClientInfo {
    int32_t sessionId = -1;
    std::string status = "online";
    bool isTyping = false;
    std::unordered_set<uint64_t> readMessages{};
};

// 전역 클라이언트 정보 관리
static std::unordered_map<int32_t, ClientInfo> clientInfos;

void signal_handler(int) {
    running = false;
}

Operation get_context(io_uring_cqe* cqe) {
    Operation ctx{};
    auto* buffer = reinterpret_cast<uint8_t*>(&cqe->user_data);
    
    ctx.client_fd = *(reinterpret_cast<int32_t*>(buffer));
    buffer += 4;
    ctx.op_type = static_cast<OperationType>(*buffer);
    buffer += 1;
    ctx.buffer_idx = *(reinterpret_cast<uint16_t*>(buffer));
    
    return ctx;
}

int main() {
    try {
        signal(SIGINT, signal_handler);

        // 서버 초기화
        SocketManager socket_manager;
        IOUringManager io_uring_manager;
        BufferManager buffer_manager(io_uring_manager.getRing());
        EventHandler event_handler(buffer_manager, io_uring_manager);
    
        // 테스트용 세션 생성
        auto& sessionManager = SessionManager::getInstance();
    
        // 리스닝 소켓 생성
        const int port = 8080;
        int listen_fd = socket_manager.createListeningSocket(port);
        if (listen_fd < 0) {
            std::cerr << "Failed to create listening socket" << std::endl;
            return 1;
        }

        std::cout << "Server started on port " << port << std::endl;
        std::cout << "Press Ctrl+C to stop the server" << std::endl;

        // Accept 준비
        sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);
        io_uring_manager.prepareAccept(listen_fd, 
                                    reinterpret_cast<sockaddr*>(&client_addr), 
                                    &client_addr_len);

        // 이벤트 루프 실행
        io_uring_cqe* cqes[IOUringManager::CQE_BATCH_SIZE];
        
        while (running) {
            int ret = io_uring_manager.submitAndWait();
            if (ret < 0) continue;

            unsigned num_cqes = io_uring_manager.peekCQE(cqes);
            for (unsigned i = 0; i < num_cqes; ++i) {
                io_uring_cqe* cqe = cqes[i];
                const auto ctx = get_context(cqe);

                switch (ctx.op_type) {
                    case OperationType::ACCEPT: {
                        event_handler.handleAccept(cqe);
                        // 새 클라이언트 정보 초기화
                        if (cqe->res >= 0) {
                            clientInfos[cqe->res] = ClientInfo{};
                        }
                        break;
                    }
                        
                    case OperationType::READ:
                        event_handler.handleRead(cqe, ctx.client_fd);
                        break;
                        
                    case OperationType::WRITE:
                        event_handler.handleWrite(cqe, ctx.client_fd, ctx.buffer_idx);
                        break;
                        
                    case OperationType::CLOSE: {
                        auto& info = clientInfos[ctx.client_fd];
                        if (info.sessionId >= 0) {
                            // 세션에서 클라이언트 제거
                            SessionManager::getInstance().removeSession(ctx.client_fd);
                            
                            // 다른 클라이언트들에게 퇴장 알림
        
                        }
                        
                        // 클라이언트 정보 제거
                        clientInfos.erase(ctx.client_fd);
                        socket_manager.closeSocket(ctx.client_fd);
                        break;
                    }
                }
            }
            io_uring_manager.advanceCQ(num_cqes);
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
