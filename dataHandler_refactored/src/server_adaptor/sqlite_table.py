"""
SQLite Table Handlers Module
=============================
SQLite 테이블 생성 및 데이터 조작을 담당하는 핸들러 모듈

테이블 타입:
1. FailedMessageTable: 전송 실패 데이터 임시 저장
   - 구조: id (자동 증가), payload (JSON 텍스트)
   - 용도: 네트워크 장애 시 데이터 손실 방지
   
2. MainTable: 주요 차량 검지 데이터 저장
   - 구조: 차량 정보 필드들 (car_id, lane_no, speed 등)
   - 용도: 여러 엣지의 데이터를 하나의 엣지에서 통합하여 통계를 집계
   - 자동 정리: 24시간 이상 오래된 데이터 삭제

설계 패턴:
- BaseTable 추상 클래스
- 각 테이블 타입별 구현 클래스
- 테이블 생성, 삽입, 조회, 삭제 메서드 제공
"""

import json

from abc import ABC, abstractmethod
from collections import OrderedDict

from data.constants import FieldKey as fk

from utils.logger import get_logger


class BaseTable(ABC):
    """
    SQLite 테이블 핸들러 추상 기본 클래스
    
    Attributes:
        config: 테이블 설정 딕셔너리
        table: 테이블 이름
        logger: 로거 인스턴스
        
    추상 메서드:
        - create(): 테이블 생성
        - insert(): 데이터 삽입
        
    선택적 메서드:
        - fetch_one(): 데이터 조회 (기본: None 반환)
        - delete_by_id(): 데이터 삭제 (기본: None 반환)
    """
    
    def __init__(self, config):
        """
        BaseTable 초기화
        
        Args:
            config: 테이블 설정 딕셔너리
                - table: 테이블 이름 (필수)
        """
        self.config = config
        self.table = self.config["table"]
        self.logger = get_logger("sqlite")
        
    @abstractmethod
    def create(self, cur):
        """
        테이블 생성 (추상 메서드)
        
        Args:
            cur: SQLite 커서 또는 연결 객체
            
        구현 필수:
        - CREATE TABLE IF NOT EXISTS 쿼리
        - 인덱스 생성 (옵션)
        - 트리거 생성 (옵션)
        
        Raises:
            NotImplementedError: 구현되지 않은 경우
        """
        raise NotImplementedError
    
    @abstractmethod
    def insert(self, cur, data):
        """
        데이터 삽입 (추상 메서드)
        
        Args:
            cur: SQLite 커서
            data: 삽입할 데이터 딕셔너리
            
        구현 필수:
        - INSERT 쿼리 생성
        - 데이터 변환 (필요 시)
        - 쿼리 실행
        
        Raises:
            NotImplementedError: 구현되지 않은 경우
        """
        raise NotImplementedError
    
    def fetch_one(self, cur):
        """
        데이터 조회 (선택적 메서드)
        
        Args:
            cur: SQLite 커서
            
        Returns:
            None: 기본 구현은 None 반환
            
        주의:
        - 필요한 경우 서브클래스에서 오버라이드
        """
        return None
    
    def delete_by_id(self, cur, row_id):
        """
        ID로 데이터 삭제 (선택적 메서드)
        
        Args:
            cur: SQLite 커서
            row_id: 삭제할 행 ID
            
        Returns:
            None: 기본 구현은 None 반환
            
        주의:
        - 필요한 경우 서브클래스에서 오버라이드
        """
        return None


class FailedMessageTable(BaseTable):
    """
    전송 실패 메시지 임시 저장 테이블 핸들러
    
    테이블 구조:
        - id: INTEGER PRIMARY KEY AUTOINCREMENT (자동 증가 고유 ID)
        - payload: TEXT NOT NULL (JSON 직렬화된 데이터)
        
    사용 용도:
        - 서버 전송 실패 시 데이터 손실 방지
        - 네트워크 복구 후 재전송
        
    데이터 흐름:
        전송 실패 → SQLite 저장 → 주기적 조회 → 재전송 시도 → 성공 시 삭제
        
    주의사항:
        - payload는 전체 데이터 딕셔너리를 JSON으로 저장
        - 재전송 순서는 삽입 순서 (FIFO)
        - 재전송 성공 시 즉시 삭제
    """
    
    def create(self, conn):
        """
        FailedMessageTable 생성
        
        Args:
            conn: SQLite 연결 객체
            
        테이블 스키마:
            CREATE TABLE IF NOT EXISTS "{table_name}" (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                payload TEXT NOT NULL
            )
            
        주의:
        - IF NOT EXISTS로 중복 생성 방지
        - id는 자동 증가로 삽입 순서 보장
        """
        cur = conn.cursor()
        create_query = f"""
                        CREATE TABLE IF NOT EXISTS "{self.table}" (
                            id INTEGER PRIMARY KEY AUTOINCREMENT,
                            payload TEXT NOT NULL
                        )
                        """
        cur.execute(create_query)
        
    def insert(self, cur, data):
        """
        전송 실패 데이터 삽입
        
        Args:
            cur: SQLite 커서
            data: 삽입할 데이터 딕셔너리
            
        처리:
        1. 데이터 딕셔너리를 JSON 문자열로 직렬화
        2. payload 컬럼에 삽입
        3. id는 자동 증가
        
        주의:
        - 전체 데이터를 그대로 저장 (필터링 없음)
        - JSON 직렬화 실패 시 예외 발생
        """
        insert_query = f"INSERT INTO \"{self.table}\" (payload) VALUES (?);"
        payload = json.dumps(data)
        cur.execute(insert_query, (payload,))
        self.logger.debug(f"Full Query:\nInsert Query: {insert_query}\nPayload: {(payload,)}")
        
    def fetch_one(self, cur):
        """
        가장 오래된 데이터 1개 조회
        
        Args:
            cur: SQLite 커서
            
        Returns:
            tuple or None: (id, payload) 또는 None
            - id: 행 고유 ID
            - payload: JSON 문자열
            
        처리:
        - ORDER BY id ASC: 가장 오래된 데이터 우선 (FIFO)
        - LIMIT 1: 1개만 조회
        
        주의:
        - 데이터가 없으면 None 반환
        - payload는 JSON 문자열 (파싱 필요)
        """
        fetch_query = f"SELECT id, payload FROM \"{self.table}\" ORDER BY id ASC LIMIT 1;"
        cur.execute(fetch_query)
        return cur.fetchone()
    
    def delete_by_id(self, cur, row_id):
        """
        ID로 데이터 삭제
        
        Args:
            cur: SQLite 커서
            row_id: 삭제할 행 ID
            
        처리:
        - WHERE id = ? 로 특정 행 삭제
        
        사용처:
        - 재전송 성공 후 호출
        
        주의:
        - row_id가 존재하지 않아도 에러 없음
        - 삭제 후 커밋은 호출자가 담당
        """
        delete_query = f"DELETE FROM \"{self.table}\" WHERE id = ?;"
        cur.execute(delete_query, (row_id, ))


class MainTable(BaseTable):
    """
    주요 차량 검지 데이터 백업 테이블 핸들러
    
    테이블 구조:
        - row_id: INTEGER PRIMARY KEY AUTOINCREMENT
        - 차량 정보 필드들 (car_id, lane_no, speed 등)
        - timestamp: 삽입 시각 (자동 생성)
        
    인덱스:
        - idx_timestamp: 타임스탬프 기준 조회 최적화
        - idx_id: 차량 ID 기준 조회 최적화
        - idx_dir_out: 회전 유형 기준 조회 최적화
        - idx_lane: 차로 번호 기준 조회 최적화
        - idx_label: 차종 기준 조회 최적화
        
    자동 정리:
        - 트리거로 24시간(86400초) 이상 오래된 데이터 자동 삭제
        - 삽입 시마다 실행
        
    사용 용도:
        - 로컬 데이터 백업
        - 오프라인 분석
        - 데이터 복구
    """
    
    def create(self, conn):
        """
        MainTable 생성 (테이블 + 인덱스 + 트리거)
        
        Args:
            conn: SQLite 연결 객체
            
        생성 항목:
        1. 테이블: 차량 검지 데이터 필드
        2. 인덱스: 5개 (조회 성능 최적화)
        3. 트리거: 자동 데이터 정리
        
        트리거 동작:
        - AFTER INSERT: 데이터 삽입 후 실행
        - 24시간 이상 오래된 데이터 삭제
        - 매 삽입마다 실행되므로 성능 영향 있음
        
        주의:
        - executescript() 사용으로 여러 쿼리 한번에 실행
        - IF NOT EXISTS로 중복 생성 방지
        - timestamp는 Unix timestamp (초 단위)
        """
        create_script = f"""
                         CREATE TABLE IF NOT EXISTS "{self.table}" (
                             row_id INTEGER PRIMARY KEY AUTOINCREMENT,
                             "{fk.CAR_ID_2K}"       INTEGER,
                             "{fk.TURN_TIME}"       INTEGER,
                             "{fk.STOP_PASS_TIME}"  INTEGER,
                             "{fk.FIRST_DET_TIME}"  INTEGER,
                             "{fk.CAR_TYPE}"        TEXT,
                             "{fk.LANE_NO}"         INTEGER,
                             "{fk.TURN_TYPE_CD}"    INTEGER,
                             "{fk.TURN_SPEED}"      REAL,
                             "{fk.STOP_PASS_SPEED}" REAL,
                             "{fk.INTERVAL_SPEED}"  REAL,
                             "{fk.OBSERVE_TIME}"    INTEGER,
                             "{fk.IMAGE_PATH_NAME}" TEXT,
                             timestamp INTEGER DEFAULT (strftime('%s', 'now'))
                         );
                         CREATE INDEX IF NOT EXISTS idx_timestamp ON "{self.table}"(timestamp);
                         CREATE INDEX IF NOT EXISTS idx_id        ON "{self.table}"("{fk.CAR_ID_2K}");
                         CREATE INDEX IF NOT EXISTS idx_dir_out   ON "{self.table}"("{fk.TURN_TYPE_CD}");
                         CREATE INDEX IF NOT EXISTS idx_lane      ON "{self.table}"("{fk.LANE_NO}");
                         CREATE INDEX IF NOT EXISTS idx_label     ON "{self.table}"("{fk.CAR_TYPE}");
 
                         CREATE TRIGGER IF NOT EXISTS cleanup_{self.table}
                         AFTER INSERT ON "{self.table}"
                         BEGIN
                             DELETE FROM "{self.table}"
                             WHERE timestamp < (strftime('%s','now') - 86400);
                         END;
                         """
        conn.executescript(create_script)
        
    def insert(self, cur, data):
        """
        차량 검지 데이터 삽입
        
        Args:
            cur: SQLite 커서
            data: 삽입할 데이터 딕셔너리
            
        처리:
        1. 필요한 필드만 추출
        2. 데이터 타입 변환 (int, float, str)
        3. INSERT 쿼리 동적 생성
        4. 쿼리 실행
        
        데이터 변환:
        - OBJ_ID → CAR_ID_2K (해싱 전 원본 ID 저장)
        - 문자열은 str(), 숫자는 int()/float() 변환
        
        주의:
        - timestamp는 자동 생성 (DEFAULT)
        - data에 없는 필드는 NULL
        - 변환 실패 시 예외 발생
        """
        # 삽입할 필드와 값 매핑
        vals = {
            fk.CAR_ID_2K       : int  (data[fk.OBJ_ID]),          # 원본 ID 저장
            fk.TURN_TIME       : int  (data[fk.TURN_TIME]),
            fk.STOP_PASS_TIME  : int  (data[fk.STOP_PASS_TIME]),
            fk.FIRST_DET_TIME  : int  (data[fk.FIRST_DET_TIME]),
            fk.CAR_TYPE        : str  (data[fk.CAR_TYPE]),
            fk.LANE_NO         : int  (data[fk.LANE_NO]),
            fk.TURN_TYPE_CD    : int  (data[fk.TURN_TYPE_CD]),
            fk.TURN_SPEED      : float(data[fk.TURN_SPEED]),
            fk.STOP_PASS_SPEED : float(data[fk.STOP_PASS_SPEED]),
            fk.INTERVAL_SPEED  : float(data[fk.INTERVAL_SPEED]),
            fk.OBSERVE_TIME    : int  (data[fk.OBSERVE_TIME]),
            fk.IMAGE_PATH_NAME : str  (data[fk.IMAGE_PATH_NAME]),
        }
        
        # 동적 INSERT 쿼리 생성
        cols = list(vals.keys())
        quoted_cols = ",".join(f"\"{c}\"" for c in cols)        # 컬럼명 이스케이프
        placeholder = ",".join("?" for _ in cols)               # 플레이스홀더 생성
        insert_query = f"INSERT INTO \"{self.table}\" ({quoted_cols}) VALUES ({placeholder});"
        
        # 값 리스트 생성
        payload = list(vals.values())
        
        # 쿼리 실행
        cur.execute(insert_query, payload)
        self.logger.debug(f"Full Query:\nInsert Query: {insert_query}\nPayload: {payload}")