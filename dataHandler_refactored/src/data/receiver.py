"""
Data Receiver Module
====================
Redis로부터 데이터를 수신하고 적절한 큐로 라우팅하는 모듈

주요 기능:
1. Redis 채널 구독 및 데이터 수신
2. 데이터 타입별 라우팅 (서버/병합/OCR)
3. defaultdict 변환으로 안전한 키 접근
4. 전송 대상 서버 태그 설정

데이터 흐름:
Redis → receiver → router → (to_server_q / to_merge_q / to_ocr_q)
"""

import json
import time
import threading

from collections import defaultdict
from data.router import route_data
from utils.logger import get_logger

class DataReceiver:
    """
    Redis 데이터 수신 및 라우팅 클래스
    
    Attributes:
        logger: 로거 인스턴스
        to_server_q: 서버 전송 큐
        to_merge_q: 병합 처리 큐 (2K/4K 병합용)
        to_ocr_q: OCR 처리 큐 (4K 원본 데이터용)
        redis_adaptor: Redis 연결 어댑터
        flag: 스레드 종료 플래그
        label: Redis 채널 라벨 (데이터 타입 구분)
        send_to: 데이터 전송 대상 서버 목록
        
    주의사항:
        - to_merge_q와 to_ocr_q는 선택적 (해당 기능 활성화 시만 전달)
        - Redis 연결 실패 시 자동 재연결 시도
        - 모든 데이터는 defaultdict로 변환하여 안전한 키 접근 보장
    """
    
    def __init__(self, to_server_q, redis_adaptor, to_merge_q = None, to_ocr_q = None):
        """
        DataReceiver 초기화
        
        Args:
            to_server_q: 서버 전송 큐 (필수)
            redis_adaptor: Redis 연결 어댑터 (필수)
            to_merge_q: 병합 처리 큐 (옵션, 2K/4K 병합 기능 활성화 시)
            to_ocr_q: OCR 처리 큐 (옵션, OCR 기능 활성화 시)
            
        설정:
        - label: Redis 채널 타입 (config.json에서 설정)
        - send_to: 전송 대상 서버 목록 (config.json에서 설정)
        """
        self.logger = get_logger("receiver")
        self.to_server_q = to_server_q
        self.to_merge_q = None
        self.to_ocr_q = None
        
        # 선택적 큐 설정 (해당 프로세서 활성화 시에만 전달됨)
        if to_merge_q:
            self.to_merge_q = to_merge_q
        if to_ocr_q:
            self.to_ocr_q = to_ocr_q
            
        # Redis 연결
        self.redis_adaptor = redis_adaptor
        self.redis_adaptor.connect()
        
        self.flag = threading.Event()
        
        # Redis 채널 라벨 (데이터 타입 구분)
        self.label = self.redis_adaptor.label
        
        # 전송 대상 서버 목록 (config.json에서 설정)
        self.send_to = self.redis_adaptor.send_to
        
    def stop(self):
        """스레드 종료 플래그 설정"""
        self.flag.set()
        
    def main_loop(self):
        """
        메인 수신 루프
        
        처리 흐름:
        1. Redis로부터 데이터 수신 (블로킹)
        2. router를 통해 데이터 타입별 분류 및 변환
        3. 각 큐로 데이터 전달:
           - to_server: 서버로 직접 전송할 데이터
           - to_merge: 2K/4K 병합이 필요한 데이터
           - to_ocr: OCR 처리가 필요한 데이터
        4. defaultdict 변환으로 안전한 키 접근 보장
        5. 서버 전송 대상 태그 설정
        
        데이터 변환:
        - dict → defaultdict(lambda: 'NULL')
        - 존재하지 않는 키 접근 시 'NULL' 반환
        - KeyError 방지로 안정성 향상
        
        주의사항:
        - redis_data가 None이면 스킵 (연결 끊김 등)
        - 큐가 None이면 해당 데이터는 버림
        - 예외 발생 시 로깅 후 계속 실행
        """
        while not self.flag.is_set():
            try:
                # Redis에서 데이터 수신 (블로킹)
                redis_data = self.redis_adaptor.get()
                
                if redis_data:
                    # 데이터 타입별 라우팅 및 변환
                    result = route_data(redis_data, self.label, self.send_to)
                    
                    # 서버 전송 데이터 처리
                    for data in result.to_server:
                        # defaultdict로 변환 (존재하지 않는 키 → 'NULL')
                        data = defaultdict(lambda: 'NULL', data)
                        self.to_server_q.put(data)
                        
                    # 병합 처리 데이터 (2K/4K 매칭용)
                    if self.to_merge_q:
                        for data in result.to_merge:
                            data = defaultdict(lambda: 'NULL', data)
                            self.to_merge_q.put(data)
                            
                    # OCR 처리 데이터 (4K 원본 이미지)
                    if self.to_ocr_q:
                        for data in result.to_ocr:
                            data = defaultdict(lambda: 'NULL', data)
                            self.to_ocr_q.put(data)
            except Exception as e:
                self.logger.critical("Something Went Wrong in Receiver!", extra={"error":e})