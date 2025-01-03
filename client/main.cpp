#include "ChatClient.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

void printHelp() {
    std::cout << "\n사용 가능한 명령어:\n"
              << "/leave - 채팅방 나가기\n"
              << "/quit - 프로그램 종료\n"
              << "/help - 도움말 보기\n" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "사용법: " << argv[0] << " <서버IP> <포트>" << std::endl;
        return 1;
    }

    // 출력 버퍼링 비활성화
    std::cout.setf(std::ios::unitbuf);
    setvbuf(stdout, nullptr, _IONBF, 0);

    ChatClient client;
    std::atomic<bool> running(true);

    // 콜백 설정
    client.setMessageCallback([](const std::string& msg) {
        // 수신된 메시지를 즉시 출력 (버퍼링 없이)
        std::cout << msg << std::flush;
    });


    // 서버 연결
    if (!client.connect(argv[1], std::stoi(argv[2]))) {
        return 1;
    }

    std::cout << "채팅 클라이언트가 시작되었습니다.\n"
              << "명령어 목록을 보려면 /help를 입력하세요." << std::flush;

    std::string input;
    while (running && std::getline(std::cin, input)) {
        if (input.empty()) continue;

        if (input[0] == '/') {
            std::string cmd = input.substr(1);
            if (cmd == "quit") {
                running = false;
                break;
            } else if (cmd == "help") {
                printHelp();
            } else if (cmd == "leave") {
                client.leaveSession();
            } else {
                std::cout << "알 수 없는 명령어입니다. /help를 입력하여 도움말을 확인하세요." << std::flush;
            }
        } else {
            // test_message_ 형식으로 전송
            if (input.find("test_message_") == 0) {
                client.sendChat(input);
            } else {
                client.sendChat("test_message_" + input);
            }
        }
    }

    client.disconnect();
    return 0;
} 