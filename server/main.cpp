#include "Listener.h"
#include "SessionManager.h"
#include "SocketManager.h"
#include "Utils.h"
#include "Logger.h"
#include <csignal>
#include <thread>

std::atomic<bool> running(true);

int main(int argc, char* argv[]) {
    if (argc != 3) {
        LOG_ERROR("Usage: ", argv[0], " <host> <port>");
        return 1;
    }

    try {
        const char* host = argv[1];
        int port = std::stoi(argv[2]);

        LOG_INFO("Starting server on ", host, ":", port);
        LOG_INFO("Hardware concurrency: ", std::thread::hardware_concurrency(), " cores");

        // 소켓 매니저 생성
        SocketManager socket_manager;

        // 세션 매니저 초기화 및 시작
        auto& session_manager = SessionManager::getInstance();
        session_manager.initialize();  // CPU 코어 수에 맞춰 자동으로 세션 생성
        session_manager.start();

        // 리스너 생성 및 시작
        Listener listener(port, socket_manager);
        listener.start();

        // 리스닝 소켓을 세션에 설정
        int listening_socket = socket_manager.getListeningSocket();
        for (size_t i = 0; i < session_manager.getOptimalThreadCount(); ++i) {
            auto session = session_manager.getSessionByIndex(i);
            if (session) {
                session->setListeningSocket(listening_socket);
            }
        }

        LOG_INFO("Server started successfully");

        // 메인 루프
        while (running) {
            listener.processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        LOG_INFO("Shutting down server...");
        
        // 정리
        listener.stop();
        session_manager.stop();
        
        LOG_INFO("Server shutdown complete");
        return 0;
    }
    catch (const std::exception& e) {
        LOG_FATAL("Fatal error: ", e.what());
        return 1;
    }
}
