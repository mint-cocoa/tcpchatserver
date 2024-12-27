#!/usr/bin/env python3
import subprocess
import time
import signal
import sys
import argparse
from typing import List, Dict
import os
import threading
import queue
import random
from datetime import datetime
from collections import defaultdict
import statistics

class Colors:
    RED = '\033[91m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    PURPLE = '\033[95m'
    CYAN = '\033[96m'
    END = '\033[0m'

class MessageStats:
    def __init__(self):
        self.sent_messages = defaultdict(list)  # session_id -> [(timestamp, message_id)]
        self.received_messages = defaultdict(list)  # session_id -> [(timestamp, message_id)]
        self.latencies = defaultdict(list)  # session_id -> [latency in ms]
        self.lock = threading.Lock()
        self.message_counter = 0
        
    def get_next_message_id(self):
        with self.lock:
            self.message_counter += 1
            return self.message_counter
            
    def record_sent(self, session_id: int, message_id: int):
        with self.lock:
            self.sent_messages[session_id].append((time.time(), message_id))
            
    def record_received(self, session_id: int, message_id: int):
        with self.lock:
            self.received_messages[session_id].append((time.time(), message_id))
            # 해당 메시지의 송신 시간 찾기
            for sent_time, sent_id in self.sent_messages[session_id]:
                if sent_id == message_id:
                    latency = (time.time() - sent_time) * 1000  # ms로 변환
                    self.latencies[session_id].append(latency)
                    break
                    
    def get_stats(self):
        with self.lock:
            stats = {}
            total_sent = sum(len(msgs) for msgs in self.sent_messages.values())
            total_received = sum(len(msgs) for msgs in self.received_messages.values())
            
            all_latencies = []
            for session_latencies in self.latencies.values():
                all_latencies.extend(session_latencies)
                
            if all_latencies:
                stats['avg_latency'] = statistics.mean(all_latencies)
                stats['min_latency'] = min(all_latencies)
                stats['max_latency'] = max(all_latencies)
                stats['median_latency'] = statistics.median(all_latencies)
            else:
                stats['avg_latency'] = 0
                stats['min_latency'] = 0
                stats['max_latency'] = 0
                stats['median_latency'] = 0
                
            stats['total_sent'] = total_sent
            stats['total_received'] = total_received
            stats['sessions'] = {}
            
            for session_id in self.sent_messages.keys():
                session_stats = {
                    'sent': len(self.sent_messages[session_id]),
                    'received': len(self.received_messages[session_id]),
                    'latencies': self.latencies[session_id]
                }
                stats['sessions'][session_id] = session_stats
                
            return stats

class ClientTester:
    def __init__(self, client_count: int, port: int, host: str, session_count: int,
                 min_interval: float, max_interval: float):
        self.client_count = client_count
        self.port = port
        self.host = host
        self.session_count = session_count
        self.min_interval = min_interval
        self.max_interval = max_interval
        self.client_binary = "./cmake-build-debug/client"
        self.processes: List[subprocess.Popen] = []
        self.client_sessions: Dict[int, int] = {}  # client_id -> session_id
        self.running = True
        self.message_queue = queue.Queue()
        self.session_messages: Dict[int, List[str]] = {i: [] for i in range(1, session_count + 1)}
        self.message_lock = threading.Lock()
        self.stats = MessageStats()
        self.start_time = None
        
    def print_colored(self, color: str, message: str):
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"{color}[{timestamp}] {message}{Colors.END}")
        
    def check_binary(self):
        if not os.path.exists(self.client_binary):
            self.print_colored(Colors.RED, f"에러: 클라이언트 바이너리를 찾을 수 없습니다: {self.client_binary}")
            self.print_colored(Colors.RED, "먼저 클라이언트를 빌드해주세요.")
            sys.exit(1)

    def get_session_color(self, session_id: int) -> str:
        colors = [Colors.GREEN, Colors.YELLOW, Colors.BLUE, Colors.PURPLE, Colors.CYAN]
        return colors[session_id % len(colors)]
            
    def start_client(self, client_id: int) -> subprocess.Popen:
        self.print_colored(Colors.YELLOW, f"클라이언트 {client_id} 시작")
        try:
            # 환경 변수 설정으로 버퍼링 비활성화
            env = os.environ.copy()
            env['PYTHONUNBUFFERED'] = '1'
            
            process = subprocess.Popen(
                [self.client_binary, self.host, str(self.port)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=0,  # 버퍼링 비활성화
                universal_newlines=True,
                env=env
            )
            time.sleep(0.1)
            if process.poll() is not None:
                raise RuntimeError("프로세스가 시작 직후 종료됨")
            return process
        except Exception as e:
            self.print_colored(Colors.RED, f"클라이언트 {client_id} 시작 실패: {str(e)}")
            raise

    def send_command(self, process: subprocess.Popen, command: str, client_id: int = None):
        if process.stdin and process.poll() is None:
            try:
                if client_id:
                    session_id = self.client_sessions.get(client_id)
                    color = self.get_session_color(session_id) if session_id else Colors.PURPLE
                    self.print_colored(color, f"[세션 {session_id}] 클라이언트 {client_id} 전송: {command}")
                process.stdin.write(command + "\n")
                process.stdin.flush()
            except (BrokenPipeError, IOError) as e:
                self.print_colored(Colors.RED, f"클라이언트 {client_id} 통신 오류: {str(e)}")
                return False
            return True
        return False

    def read_output(self, process: subprocess.Popen, client_id: int):
        session_id = self.client_sessions.get(client_id)
        while self.running and process.poll() is None:
            try:
                output = process.stdout.readline()
                if not output and process.poll() is not None:
                    break
                    
                if output:
                    output = output.strip()
                    if session_id:
                        if "test_message_" in output:
                            try:
                                message_parts = output.split("test_message_")
                                if len(message_parts) >= 2:
                                    message_id = int(message_parts[1].split()[0])
                                    self.stats.record_received(session_id, message_id)
                                    color = self.get_session_color(session_id)
                                    self.print_colored(color, f"[세션 {session_id}] 메시지 수신 확인: {message_id}")
                            except (IndexError, ValueError) as e:
                                self.print_colored(Colors.RED, f"메시지 파싱 오류: {str(e)}")
                        
                        color = self.get_session_color(session_id)
                        with self.message_lock:
                            self.session_messages[session_id].append(
                                f"[클라이언트 {client_id}] {output}"
                            )
                            if len(self.session_messages[session_id]) > 100:
                                self.session_messages[session_id].pop(0)
                        self.print_colored(color, f"[세션 {session_id}] 클라이언트 {client_id}: {output}")
                    else:
                        print(f"클라이언트 {client_id}: {output}")
                
                error = process.stderr.readline()
                if error:
                    error = error.strip()
                    if error:
                        self.print_colored(Colors.RED, f"클라이언트 {client_id} 에러: {error}")
                        
            except Exception as e:
                self.print_colored(Colors.RED, f"출력 읽기 오류 (클라이언트 {client_id}): {str(e)}")
                break
                
        self.print_colored(Colors.YELLOW, f"클라이언트 {client_id} 출력 모니터링 종료")

    def message_sender(self):
        while self.running:
            client_id = random.randint(1, self.client_count)
            if client_id not in range(1, len(self.processes) + 1):
                continue

            process = self.processes[client_id - 1]
            if process.poll() is not None:
                continue

            # 메시지 ID 생성 및 전송
            message_id = self.stats.get_next_message_id()
            session_id = self.client_sessions.get(client_id)
            if session_id:
                message = f"test_message_{message_id}"
                if self.send_command(process, message, client_id):
                    self.stats.record_sent(session_id, message_id)
            
            time.sleep(random.uniform(self.min_interval, self.max_interval))

    def run_test(self):
        self.check_binary()
        
        self.print_colored(Colors.GREEN, "=== 클라이언트 테스트 시작 ===")
        print(f"호스트: {self.host}")
        print(f"포트: {self.port}")
        print(f"클라이언트 수: {self.client_count}")
        print(f"세션 수: {self.session_count}")
        print(f"메시지 전송 간격: {self.min_interval}초 ~ {self.max_interval}초")
        print()
        
        try:
            # 벤치마크 통계 출력 스레드 시작
            benchmark_thread = threading.Thread(target=self.print_benchmark_stats, daemon=True)
            benchmark_thread.start()

            # 클라이언트 시작 및 세션 참여
            for i in range(1, self.client_count + 1):
                try:
                    process = self.start_client(i)
                    self.processes.append(process)
                    time.sleep(0.1)
                    
                    # 세션 참여
                    session_id = (i % self.session_count) + 1
                    self.client_sessions[i] = session_id
                    self.print_colored(Colors.BLUE, f"클라이언트 {i} -> 세션 {session_id} 참여")
                    if not self.send_command(process, f"/join {session_id}", i):
                        continue
                    time.sleep(0.1)

                    # 출력 모니터링 스레드 시작
                    threading.Thread(
                        target=self.read_output,
                        args=(process, i),
                        daemon=True
                    ).start()
                except Exception as e:
                    self.print_colored(Colors.RED, f"클라이언트 {i} 설정 실패: {str(e)}")
                    continue

            if not self.processes:
                raise RuntimeError("모든 클라이언트 시작 실패")

            self.start_time = time.time()
            
            # 메시지 전송 스레드 시작
            sender_thread = threading.Thread(target=self.message_sender, daemon=True)
            sender_thread.start()
            
            # 세션 통계 출력 스레드 시작
            stats_thread = threading.Thread(target=self.print_session_stats, daemon=True)
            stats_thread.start()
            
            self.print_colored(Colors.GREEN, f"\n{len(self.processes)}개의 클라이언트가 시작되었습니다.")
            print("종료하려면 Ctrl+C를 누르세요...")
            
            while True:
                active_processes = 0
                for i, process in enumerate(self.processes, 1):
                    if process.poll() is None:
                        active_processes += 1
                    elif process in self.processes:
                        self.print_colored(Colors.RED, f"클라이언트 {i}가 종료되었습니다.")
                
                if active_processes == 0:
                    self.print_colored(Colors.RED, "모든 클라이언트가 종료되었습니다.")
                    return
                
                time.sleep(1)
                
        except KeyboardInterrupt:
            self.print_colored(Colors.YELLOW, "\n테스트를 종료합니다...")
        except Exception as e:
            self.print_colored(Colors.RED, f"테스트 중 오류 발생: {str(e)}")
        finally:
            self.running = False
            self.cleanup()
            
    def cleanup(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
        
        self.print_colored(Colors.GREEN, "테스트가 완료되었습니다.")

    def print_session_stats(self):
        while self.running:
            time.sleep(5)
            with self.message_lock:
                print("\n=== 세션별 메시지 통계 ===")
                for session_id in range(1, self.session_count + 1):
                    color = self.get_session_color(session_id)
                    msg_count = len(self.session_messages[session_id])
                    clients = [cid for cid, sid in self.client_sessions.items() if sid == session_id]
                    self.print_colored(color, 
                        f"세션 {session_id}: {len(clients)}명 접속 중, "
                        f"최근 메시지 수: {msg_count}"
                    )
                print("========================\n")

    def print_benchmark_stats(self):
        while self.running:
            time.sleep(5)
            if not self.start_time:
                continue
                
            stats = self.stats.get_stats()
            elapsed_time = time.time() - self.start_time
            
            print("\n=== 벤치마크 통계 ===")
            print(f"실행 시간: {elapsed_time:.1f}초")
            print(f"전송된 총 메시지: {stats['total_sent']}")
            print(f"수신된 총 메시지: {stats['total_received']}")
            
            if elapsed_time > 0:
                tps_sent = stats['total_sent'] / elapsed_time
                tps_received = stats['total_received'] / elapsed_time
                print(f"초당 전송 메시지 (TPS): {tps_sent:.1f}")
                print(f"초당 수신 메시지 (TPS): {tps_received:.1f}")
            
            print(f"\n지연 시간 통계:")
            print(f"  평균: {stats['avg_latency']:.2f}ms")
            print(f"  최소: {stats['min_latency']:.2f}ms")
            print(f"  최대: {stats['max_latency']:.2f}ms")
            print(f"  중간값: {stats['median_latency']:.2f}ms")
            
            print("\n세션별 통계:")
            for session_id, session_stats in stats['sessions'].items():
                print(f"세션 {session_id}:")
                print(f"  전송: {session_stats['sent']}")
                print(f"  수신: {session_stats['received']}")
                if session_stats['latencies']:
                    avg_latency = statistics.mean(session_stats['latencies'])
                    print(f"  평균 지연: {avg_latency:.2f}ms")
            print("=====================")

def main():
    parser = argparse.ArgumentParser(description="클라이언트 테스트 스크립트")
    parser.add_argument("-n", "--num-clients", type=int, default=4, help="클라이언트 수 (기본값: 4)")
    parser.add_argument("-p", "--port", type=int, default=8080, help="서버 포트 (기본값: 8080)")
    parser.add_argument("-H", "--host", default="127.0.0.1", help="서버 호스트 (기본값: localhost)")
    parser.add_argument("-s", "--sessions", type=int, default=1, help="세션 수 (기본값: 2)")
    parser.add_argument("--min-interval", type=float, default=1.0,
                      help="최소 메시지 전송 간격(초) (기본값: 1.0)")
    parser.add_argument("--max-interval", type=float, default=3.0,
                      help="최대 메시지 전송 간격(초) (기본값: 3.0)")
    
    args = parser.parse_args()
    
    if args.min_interval > args.max_interval:
        print("에러: 최소 간격이 최대 간격보다 큽니다.")
        sys.exit(1)
    
    tester = ClientTester(
        client_count=args.num_clients,
        port=args.port,
        host=args.host,
        session_count=args.sessions,
        min_interval=args.min_interval,
        max_interval=args.max_interval
    )
    tester.run_test()

if __name__ == "__main__":
    main() 