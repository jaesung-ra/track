"""
SQLite Adaptor Module
=====================
로컬 SQLite 데이터베이스를 통한 데이터 저장 및 조회를 담당하는 어댑터

주요 기능:
1. 전송 실패 데이터 임시 저장 (FailedMessageTable)
2. 주요 데이터 로컬 저장 (MainTable)
3. 스레드 안전 데이터 접근
4. 주기적인 데이터 조회 및 재전송

사용 용도:
- 네트워크 장애 시 데이터 손실 방지
- 서버 다운타임 동안 데이터 버퍼링
- 여러 엣지의 데이터를 하나의 엣지에서 통합하여 통계를 집계

테이블 타입:
- failed_messages: 전송 실패한 데이터 임시 저장
- main_table: 주요 데이터 영구 저장 (24시간 자동 정리)
"""

import sqlite3
import threading
import json

from utils.logger import get_logger

from server_adaptor.server_adaptor import ServerAdaptor
from server_adaptor.sqlite_table import MainTable, FailedMessageTable

from utils.logger import get_logger

from data.constants import FieldKey as fk


class SqliteAdaptor(ServerAdaptor):
    """
    SQLite 데이터베이스 어댑터 클래스
    
    Attributes:
        stype: 서버 타입 ("sqlite")
        host: None (로컬 파일 기반)
        port: None (로컬 파일 기반)
        database: SQLite 데이터베이스 파일 경로
        table: 테이블 이름
        name: 어댑터 이름
        interval: 재전송 시도 주기 (초)
        table_handler: 테이블 핸들러 (MainTable 또는 FailedMessageTable)
        logger: 로거 인스턴스
        connection: SQLite 연결 객체
        cursor: SQLite 커서 객체
        _lock: 스레드 동기화 락
        
    스레드 안전성:
    - 모든 데이터베이스 작업은 _lock으로 보호됨
    - check_same_thread=False로 멀티스레드 접근 허용
    
    주의사항:
    - SQLite는 동시 쓰기에 제한이 있으므로 락 필수
    - 데이터베이스 파일은 자동 생성됨
    """
    
    def __init__(self, config):
        """
        SqliteAdaptor 초기화
        
        Args:
            config: SQLite 설정 딕셔너리
                - database: 데이터베이스 파일 경로 (예: "./data.db")
                - table: 테이블 이름
                - name: 어댑터 이름 (옵션)
                - interval: 재전송 시도 주기 (옵션, FailedMessageTable용)
                
        테이블 타입 선택:
        - "failed_messages": 전송 실패 데이터 임시 저장
        - "main_table": 주요 데이터 영구 저장
        """
        self.stype = "sqlite"
        self.host = None   # 로컬 파일
        self.port = None   # 로컬 파일
        self.database = config["database"]
        self.table = config["table"]
        self.name = config.get("name")
        self.interval = config.get("interval")
        
        # 테이블 타입에 따라 핸들러 생성
        self.table_handler = None
        if self.table == "failed_messages":
            # 전송 실패 데이터 테이블
            self.table_handler = FailedMessageTable(config)
        elif self.table == "main_table":
            # 주요 데이터 백업 테이블
            self.table_handler = MainTable(config)
        
        self.logger = get_logger("sqlite")
        
        self.connection = None
        self.cursor = None
        
        # 스레드 동기화 락
        self._lock = threading.Lock()

    def connect(self):
        """
        SQLite 데이터베이스 연결 및 테이블 생성
        
        처리:
        1. 이미 연결되어 있으면 스킵
        2. 데이터베이스 파일 연결
        3. check_same_thread=False로 멀티스레드 허용
        4. 테이블 생성 (없는 경우)
        5. 변경사항 커밋
        
        주의사항:
        - check_same_thread=False: 여러 스레드에서 접근 가능
        - 하지만 동시 쓰기는 _lock으로 보호 필요
        - 연결 실패 시 조용히 리턴 (에러 로그 없음)
        """
        # 이미 연결된 경우 스킵
        if self.connection:
            return
        
        try:
            # 데이터베이스 연결
            self.connection = sqlite3.connect(
                self.database, 
                check_same_thread=False  # 멀티스레드 접근 허용
            )
            
            # 테이블 생성 (없는 경우)
            self.table_handler.create(self.connection)
            
            # 변경사항 커밋
            self.connection.commit()
        except Exception as e:
            # 에러 발생 시 조용히 리턴
            return
        
    def insert(self, data, dtype): 
        """
        데이터 삽입
        
        Args:
            data: 삽입할 데이터 딕셔너리
            dtype: 데이터 타입 (현재 미사용, 호환성 유지용)
            
        Returns:
            bool: 삽입 성공 여부
            - True: 삽입 성공
            - False: 삽입 실패
            
        처리:
        1. 커서 생성
        2. 락 획득 (스레드 안전성)
        3. 테이블 핸들러를 통해 삽입
        4. 커밋
        5. 락 해제
        6. 성공/실패 로깅
        
        주의사항:
        - _lock으로 동시 쓰기 방지
        - 커밋 전까지는 다른 스레드에서 조회 불가
        - 에러 발생 시 자동 롤백 (SQLite 기본 동작)
        """
        try:
            cursor = self.connection.cursor()
            
            # 스레드 안전 삽입
            with self._lock:
                self.table_handler.insert(cursor, data)
                self.connection.commit()
                
            self.logger.info("SQLite Insert Success!", extra={
                "datatype": data[fk.DATA_TYPE], 
                "data": data[fk.UK_PLAIN]
            })
            return True
        except Exception as e:
            self.logger.error("SQLite Insert Failed!", extra={
                "datatype": data[fk.DATA_TYPE], 
                "data": data[fk.UK_PLAIN], 
                "error": e
            })
            return False
                
    def disconnect(self):
        """
        SQLite 연결 해제
        
        처리:
        - 연결 종료
        - 연결 객체 None으로 설정
        
        주의사항:
        - 커밋되지 않은 변경사항은 손실됨
        - 진행 중인 트랜잭션은 롤백됨
        """
        if self.connection:
            self.connection.close()
            self.connection = None

    def get(self):
        """
        데이터 조회 (1개)
        
        Returns:
            tuple or None: (id, payload) 튜플 또는 None
            - id: 행 고유 ID
            - payload: JSON 문자열 (FailedMessageTable) 또는 데이터 튜플
            
        처리:
        1. 커서 생성
        2. 락 획득
        3. 테이블 핸들러를 통해 조회
        4. 락 해제
        
        주의사항:
        - 가장 오래된 데이터 1개만 조회 (FIFO)
        - FailedMessageTable에서만 사용
        - MainTable은 None 반환
        """
        cursor = self.connection.cursor()
        
        # 스레드 안전 조회
        with self._lock:
            result = self.table_handler.fetch_one(cursor)
            
        return result
            
    def delete_by_id(self, id):
        """
        ID로 데이터 삭제
        
        Args:
            id: 삭제할 행의 고유 ID
            
        처리:
        1. 커서 생성
        2. 락 획득
        3. 테이블 핸들러를 통해 삭제
        4. 커밋
        5. 락 해제
        
        사용처:
        - FailedDataSender가 재전송 성공 후 호출
        
        주의사항:
        - ID가 존재하지 않아도 에러 없음
        - 삭제 후 즉시 커밋됨
        """
        cursor = self.connection.cursor()
        
        # 스레드 안전 삭제
        with self._lock:
            self.table_handler.delete_by_id(cursor, id)
            self.connection.commit()