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
import select

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

class ClientProcess:
    def __init__(self, client_id: int, host: str, port: int):
        self.client_id = client_id
        self.host = host
        self.port = port
        self.process = None
        self.session_id = None
        
    def start(self):
        try:
            # 클라이언트 바이너리 실행
            self.process = subprocess.Popen(
                ["./build/chat_client", self.host, str(self.port)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
                universal_newlines=True
            )
            return True
        except Exception as e:
            print(f"클라이언트 {self.client_id} 시작 실패: {str(e)}")
            return False

    def read_stderr(self):
        while self.is_alive():
            try:
                error = self.process.stderr.readline()
                if error:
                    print(f"클라이언트 {self.client_id} 에러: {error.strip()}")
            except:
                break
            
    def send_message(self, message: str):
        if self.process and self.process.poll() is None:
            try:
                # 메시지에 개행문자가 없는 경우에만 추가
                if not message.endswith('\n'):
                    message += '\n'
                
                # 바이트로 인코딩하여 전송
                self.process.stdin.write(message)
                self.process.stdin.flush()
                
                # 약간의 지연을 추가하여 버퍼가 비워질 시간을 줌
                time.sleep(0.001)
                return True
            except BrokenPipeError as e:
                print(f"클라이언트 {self.client_id} 파이프 에러: {str(e)}")
                return False
            except IOError as e:
                print(f"클라이언트 {self.client_id} IO 에러: {str(e)}")
                return False
            except Exception as e:
                print(f"클라이언트 {self.client_id} 알 수 없는 에러: {str(e)}")
                return False
        return False
        
    def is_alive(self):
        if self.process:
            ret = self.process.poll()
            if ret is not None:
                print(f"클라이언트 {self.client_id} 종료됨 (반환 코드: {ret})")
            return ret is None
        return False
        
    def stop(self):
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()

class ClientTester:
    def __init__(self, client_count: int, port: int, host: str,
                 min_interval: float, max_interval: float):
        self.client_count = client_count
        self.port = port
        self.host = host
        self.min_interval = min_interval
        self.max_interval = max_interval
        self.running = True
        self.clients: List[ClientProcess] = []
        self.stats = MessageStats()
        
    def print_colored(self, color: str, message: str):
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"{color}[{timestamp}] {message}{Colors.END}")
        
    def get_session_color(self, session_id: int) -> str:
        colors = [Colors.GREEN, Colors.YELLOW, Colors.BLUE, Colors.PURPLE, Colors.CYAN]
        return colors[session_id % len(colors)]
            
    def start_client(self, client_id: int) -> ClientProcess:
        self.print_colored(Colors.YELLOW, f"클라이언트 {client_id} 시작")
        client = ClientProcess(client_id, self.host, self.port)
        if client.start():
            return client
        return None

    def process_client_output(self, client: ClientProcess):
        try:
            # stdout 처리 (non-blocking으로 변경)
            while client.process.stdout in select.select([client.process.stdout], [], [], 0)[0]:
                output = client.process.stdout.readline()
                if not output:
                    return False
                if "joined session:" in output:
                    try:
                        session_id = int(output.split(":")[1].strip())
                        client.session_id = session_id
                        color = self.get_session_color(session_id)
                        self.print_colored(color, f"클라이언트 {client.client_id}가 세션 {session_id}에 참여했습니다.")
                    except (IndexError, ValueError) as e:
                        self.print_colored(Colors.RED, f"세션 ID 파싱 오류: {str(e)}, 원본 메시지: {output}")
                elif client.session_id is not None:
                    if "test_message_" in output:
                        try:
                            message_parts = output.split("test_message_")
                            if len(message_parts) >= 2:
                                message_id = int(message_parts[1].split()[0])
                                self.stats.record_received(client.session_id, message_id)
                                color = self.get_session_color(client.session_id)
                                self.print_colored(color, f"[세션 {client.session_id}] 메시지 수신 확인: {message_id}")
                        except (IndexError, ValueError) as e:
                            self.print_colored(Colors.RED, f"메시지 파싱 오류: {str(e)}, 원본 메시지: {output}")
                    
                    color = self.get_session_color(client.session_id)
                    self.print_colored(color, f"[세션 {client.session_id}] 클라이언트 {client.client_id}: {output}")
                else:
                    self.print_colored(Colors.YELLOW, f"클라이언트 {client.client_id}: {output}")
                
            # stderr 처리 (non-blocking으로 변경)
            while client.process.stderr in select.select([client.process.stderr], [], [], 0)[0]:
                error = client.process.stderr.readline()
                if error and error.strip():
                    self.print_colored(Colors.RED, f"클라이언트 {client.client_id} 에러: {error.strip()}")
            
            return True
            
        except Exception as e:
            self.print_colored(Colors.RED, f"출력 처리 중 오류 발생 (클라이언트 {client.client_id}): {str(e)}")
            return False

    def run_test(self):
        self.print_colored(Colors.GREEN, "=== 클라이언트 테스트 시작 ===")
        print(f"호스트: {self.host}")
        print(f"포트: {self.port}")
        print(f"클라이언트 수: {self.client_count}")
        print(f"메시지 전송 간격: {self.min_interval}초 ~ {self.max_interval}초")
        print()
        
        try:
            # 클라이언트 시작
            for i in range(1, self.client_count + 1):
                try:
                    client = self.start_client(i)
                    if client:
                        self.clients.append(client)
                        time.sleep(0.1)  # 연결 대기
                except Exception as e:
                    self.print_colored(Colors.RED, f"클라이언트 {i} 초기화 실패: {str(e)}")
                    continue

            message_counter = 0
            last_stats_time = time.time()

            # 메인 루프
            while self.running:
                # 각 클라이언트의 출력 처리
                for client in self.clients[:]:
                    if not client.is_alive():
                        self.clients.remove(client)
                        continue
                        
                    # 출력 처리
                    self.process_client_output(client)
                    
                    # 세션이 할당된 클라이언트에게 메시지 전송
                    if client.session_id:
                        message_id = self.stats.get_next_message_id()
                        timestamp = datetime.now().strftime("%H:%M:%S.%f")
                        message = f"test_message_{message_id} from client {client.client_id} at {timestamp}"
                        if client.send_message(message):
                            self.stats.record_sent(client.session_id, message_id)
                            message_counter += 1

                # 5초마다 통계 출력
                current_time = time.time()
                if current_time - last_stats_time >= 5:
                    stats = self.stats.get_stats()
                    print("\n=== 벤치마크 통계 ===")
                    print(f"총 전송 메시지: {stats['total_sent']}")
                    print(f"총 수신 메시지: {stats['total_received']}")
                    if stats['total_sent'] > 0:
                        print(f"수신율: {(stats['total_received'] / stats['total_sent']) * 100:.1f}%")
                    print(f"평균 지연시간: {stats['avg_latency']:.2f}ms")
                    print(f"최소 지연시간: {stats['min_latency']:.2f}ms")
                    print(f"최대 지연시간: {stats['max_latency']:.2f}ms")
                    print(f"중간값 지연시간: {stats['median_latency']:.2f}ms")
                    print("\n세션별 통계:")
                    for session_id, session_stats in stats['sessions'].items():
                        color = self.get_session_color(session_id)
                        print(f"{color}세션 {session_id}:")
                        print(f"  전송: {session_stats['sent']}")
                        print(f"  수신: {session_stats['received']}")
                        if session_stats['latencies']:
                            avg_latency = statistics.mean(session_stats['latencies'])
                            print(f"  평균 지연시간: {avg_latency:.2f}ms{Colors.END}")
                    print()
                    last_stats_time = current_time

                time.sleep(random.uniform(self.min_interval, self.max_interval))
                
        except KeyboardInterrupt:
            self.print_colored(Colors.YELLOW, "테스트 종료 중...")
        except Exception as e:
            self.print_colored(Colors.RED, f"테스트 실행 중 오류 발생: {str(e)}")
        finally:
            self.cleanup()

    def cleanup(self):
        self.running = False
        for client in self.clients:
            try:
                client.stop()
            except:
                pass
        self.print_colored(Colors.GREEN, "테스트가 종료되었습니다.")

def main():
    parser = argparse.ArgumentParser(description='TCP 서버 테스트 클라이언트')
    parser.add_argument('--host', default='127.0.0.1', help='서버 호스트')
    parser.add_argument('--port', type=int, default=8080, help='서버 포트')
    parser.add_argument('--clients', type=int, default=10, help='클라이언트 수')
    parser.add_argument('--min-interval', type=float, default=0.1, help='최소 메시지 전송 간격(초)')
    parser.add_argument('--max-interval', type=float, default=1.0, help='최대 메시지 전송 간격(초)')
    
    args = parser.parse_args()
    
    tester = ClientTester(
        client_count=args.clients,
        port=args.port,
        host=args.host,
        min_interval=args.min_interval,
        max_interval=args.max_interval
    )
    
    tester.run_test()

if __name__ == '__main__':
    main() 