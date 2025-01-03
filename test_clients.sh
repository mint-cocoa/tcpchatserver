#!/bin/bash

# 색상 정의
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 기본 설정
CLIENT_COUNT=4
PORT=8080
HOST="localhost"
CLIENT_BINARY="./build/chat_client"
SESSION_COUNT=1 # 테스트할 세션 수

# 사용법 출력 함수
print_usage() {
    echo -e "${YELLOW}사용법:${NC} $0 [-n 클라이언트수] [-p 포트] [-h 호스트] [-s 세션수]"
    echo "  -n: 실행할 클라이언트 수 (기본값: 4)"
    echo "  -p: 서버 포트 (기본값: 8080)"
    echo "  -h: 서버 호스트 (기본값: localhost)"
    echo "  -s: 테스트할 세션 수 (기본값: 1)"
    exit 1
}

# 명령행 인자 파싱
while getopts "n:p:h:s:" opt; do
    case $opt in
        n) CLIENT_COUNT=$OPTARG ;;
        p) PORT=$OPTARG ;;
        h) HOST=$OPTARG ;;
        s) SESSION_COUNT=$OPTARG ;;
        ?) print_usage ;;
    esac
done

# 클라이언트 바이너리 존재 확인
if [ ! -f "$CLIENT_BINARY" ]; then
    echo -e "${RED}에러:${NC} 클라이언트 바이너리를 찾을 수 없습니다: $CLIENT_BINARY"
    echo "먼저 클라이언트를 빌드해주세요."
    exit 1
fi

echo -e "${GREEN}=== 클라이언트 테스트 시작 ===${NC}"
echo "호스트: $HOST"
echo "포트: $PORT"
echo "클라이언트 수: $CLIENT_COUNT"
echo "세션 수: $SESSION_COUNT"
echo

# 클라이언트 프로세스 ID 저장 배열
declare -a CLIENT_PIDS
declare -a NAMED_PIPES

# 이전 파이프 정리
for ((i=1; i<=CLIENT_COUNT; i++)); do
    rm -f "/tmp/client_pipe_$i"
done

# 각 클라이언트의 named pipe 생성
for ((i=1; i<=CLIENT_COUNT; i++)); do
    PIPE="/tmp/client_pipe_$i"
    mkfifo "$PIPE" 2>/dev/null
    NAMED_PIPES+=("$PIPE")
done

# 각 클라이언트 순차적으로 실행
for ((i=1; i<=CLIENT_COUNT; i++)); do
    PIPE="${NAMED_PIPES[$i-1]}"
    
    # 클라이언트 시작
    echo -e "${YELLOW}클라이언트 $i 시작${NC}"
    cat "$PIPE" | $CLIENT_BINARY $HOST $PORT &
    CLIENT_PID=$!
    CLIENT_PIDS+=($CLIENT_PID)
    
    # 클라이언트 초기화를 위한 대기
    sleep 3
    
    # 세션 명령어 전송을 위한 백그라운드 프로세스
    (
        # 세션 목록 확인
        echo -e "${BLUE}클라이언트 $i: 세션 목록 요청${NC}"
        echo "/list" > "$PIPE"
        sleep 2
        
        # 세션 참여
        SESSION_ID=1  # 모든 클라이언트를 세션 1에 참여시킴
        echo -e "${BLUE}클라이언트 $i -> 세션 $SESSION_ID 참여 시도${NC}"
        echo "/join $SESSION_ID" > "$PIPE"
        sleep 2
        
        # 세션 참여 확인
        echo "/list" > "$PIPE"
        sleep 1
        
        # 테스트 메시지 전송
        echo -e "${GREEN}클라이언트 $i: 메시지 전송 시작${NC}"
        echo "안녕하세요! 저는 클라이언트 $i 입니다." > "$PIPE"
        sleep 2
        echo "테스트 메시지입니다." > "$PIPE"
        sleep 2
    ) &
done

echo -e "\n${GREEN}모든 클라이언트가 시작되었습니다.${NC}"
echo "종료하려면 Ctrl+C를 누르세요..."

# SIGINT (Ctrl+C) 처리
cleanup() {
    echo -e "\n${YELLOW}테스트 종료 중...${NC}"
    
    # 모든 클라이언트 프로세스 종료
    for pid in "${CLIENT_PIDS[@]}"; do
        kill -SIGINT $pid 2>/dev/null
    done
    
    # named pipe 정리
    for pipe in "${NAMED_PIPES[@]}"; do
        rm -f "$pipe"
    done
    
    wait
    echo -e "${GREEN}테스트가 완료되었습니다.${NC}"
    exit 0
}

trap cleanup SIGINT
trap cleanup EXIT

# 모든 클라이언트 프로세스 대기
wait