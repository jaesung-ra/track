"""
VoltDB Adaptor Module
=====================
VoltDB 데이터베이스 연결 및 데이터 전송을 담당하는 어댑터

주요 기능:
1. VoltDB REST API를 통한 데이터 삽입
2. 카메라 정보 자동 조회 및 설정
3. 차로 정보 조회 및 오프셋 설정
4. 테이블 스키마 자동 탐색
5. 연결 재시도 및 에러 처리

데이터 타입별 테이블 매핑:
- 차량: 2K, 4K, MERGE
- 보행자: PED_2K
- 통계: APPROACH, TURN_TYPES, LANES, VEHICLE_TYPES
- 대기행렬: APPROACH, LANES
- 돌발상황: INCIDENT_START, INCIDENT_END

특수 처리:
- MERGE, INCIDENT_END: UPSERT 사용 (중복 시 업데이트)
- 기타: INSERT 사용
"""

import requests
import json
import time
import threading
import re
import os
import urllib.parse as up

from server_adaptor.server_adaptor import ServerAdaptor

from data.sender import DataSender
from data.constants import Tables as ta
from data.constants import DataType as dt
from data.constants import FieldKey as fk

from utils.logger import get_logger


class VoltDBAdaptor(ServerAdaptor):
    """
    VoltDB 데이터베이스 어댑터 클래스
    
    Class Attributes:
        table_column_map: 테이블별 컬럼 리스트 매핑 (클래스 변수, 공유)
        
    Instance Attributes:
        logger: 로거 인스턴스
        stype: 서버 타입 ("volt")
        host: VoltDB 서버 호스트
        port: VoltDB 서버 포트
        name: 어댑터 이름
        base_url: VoltDB REST API 기본 URL
        session: HTTP 세션 객체
        connected: 연결 상태 플래그
        type_table_map: 데이터 타입 → 테이블명 매핑
        
    연결 초기화:
    1. 백그라운드 스레드로 연결 재시도
    2. 테이블 스키마 자동 조회
    3. 카메라 ID 자동 설정
    4. 차로 오프셋 자동 설정
    
    주의사항:
    - REST API 사용으로 네트워크 오버헤드 있음
    - 삽입 실패 시 최대 3회 재시도
    - 카메라 ID는 Edge 서버 IP로 조회
    """
    
    # 클래스 변수: 테이블별 컬럼 매핑 (모든 인스턴스 공유)
    table_column_map = None
    
    def __init__(self, config):
        """
        VoltDBAdaptor 초기화
        
        Args:
            config: VoltDB 설정 딕셔너리
                - ip: VoltDB 서버 IP
                - port: VoltDB 서버 포트 (기본: 8080)
                - name: 어댑터 이름
                
        데이터 타입 → 테이블명 매핑:
        - VEHICLE_2K → soitgrtmdtinfo_2K
        - MERGE → soitgrtmdtinfo
        - PED_2K → soitgcwdtinfo
        - 통계/대기행렬 → 각 테이블
        - 돌발상황 → soitgunacevet
        """
        self.logger = get_logger("volt")
        self.stype = "volt"
        self.host = config["ip"]
        self.port = config["port"]
        self.name = config["name"]
        
        # REST API 기본 URL 구성
        self.base_url = f"http://{self.host}:{self.port}/api/1.0/"
        
        self.session = None
        self.connected = False
        
        # 데이터 타입 → 테이블명 매핑
        self.type_table_map = {
            dt.VEHICLE_2K     : ta.TABLE_2K,               dt.VEHICLE_4K          : ta.TABLE_4K,
            dt.MERGE          : ta.TABLE_MERGE,            dt.PED_2K              : ta.TABLE_PED,
            dt.STATS_APPROACH : ta.TABLE_STATS_APPROACH,   dt.STATS_TURN_TYPES    : ta.TABLE_STATS_TURNTYPE,
            dt.STATS_LANES    : ta.TABLE_STATS_LANE,       dt.STATS_VEHICLE_TYPES : ta.TABLE_STATS_VEHICLE_TYPE,
            dt.QUEUE_APPROACH : ta.TABLE_QUEUE_APPROACH,   dt.QUEUE_LANES         : ta.TABLE_QUEUE_LANE,
            dt.INCIDENT_START : ta.TABLE_ABNORMAL,         dt.INCIDENT_END        : ta.TABLE_ABNORMAL
        }

    def connect(self):
        """
        VoltDB 연결 (백그라운드 스레드 시작)
        
        처리:
        - 연결 재시도 스레드 시작
        - 데몬 스레드로 메인 프로세스 종료 시 자동 종료
        
        백그라운드 작업:
        1. VoltDB 연결 시도
        2. 테이블 스키마 조회
        3. 카메라 ID 조회 및 설정
        4. 차로 정보 조회 및 오프셋 설정
        
        주의:
        - 비블로킹 방식 (즉시 리턴)
        - 실제 연결은 백그라운드에서 진행
        - 연결 실패 시 10초마다 재시도
        """
        self._t = threading.Thread(target=self._retry_loop, name="VOLT CON-RET", daemon=True)
        self._t.start()

    def disconnect(self):
        """
        VoltDB 연결 해제
        
        처리:
        - HTTP 세션 종료
        
        주의:
        - 백그라운드 스레드는 데몬이므로 자동 종료
        """
        self.session.close()

    def insert(self, data, dtype, remote_dir = None):
        """
        VoltDB에 데이터 삽입
        
        Args:
            data: 삽입할 데이터 딕셔너리
            dtype: 데이터 타입
            remote_dir: 원격 디렉토리 (현재 미사용)
            
        Returns:
            bool: 삽입 성공 여부
            - True: 삽입 성공
            - False: 연결 안 됨 또는 3회 재시도 실패
            
        처리 흐름:
        1. 연결 상태 확인
        2. 데이터 타입에 맞는 테이블 선택
        3. 테이블 스키마에 맞춰 컬럼과 값 생성
        4. INSERT 또는 UPSERT 쿼리 생성
        5. 최대 3회 재시도하며 실행
        
        쿼리 유형:
        - MERGE, INCIDENT_END: UPSERT (중복 시 업데이트)
        - 기타: INSERT (중복 시 에러)
        
        주의사항:
        - 연결 안 되면 즉시 False 반환
        - 각 재시도 사이 0.1초 대기
        - data에 없는 컬럼은 NULL로 삽입
        """
        # 연결 상태 확인
        if not self.connected:
            return False
            
        # 데이터 타입에 맞는 테이블 선택
        table = self.type_table_map[dtype]
        
        # 테이블 스키마에서 컬럼 리스트 가져오기
        columns = VoltDBAdaptor.table_column_map[table]
        
        # SQL 쿼리 생성
        columns_str = ', '.join(columns)
        
        # 값 생성: data에 있으면 해당 값, 없으면 NULL
        formatted_values = ', '.join([f"'{data[col]}'" if col in data else 'NULL' for col in columns])
        
        # 쿼리 유형 결정
        command = "UPSERT" if dtype == dt.MERGE or dtype == dt.INCIDENT_END else "INSERT"
        
        # 최종 쿼리 (이스케이프 처리)
        query = "\"" + command + f" INTO {table} ({columns_str}) VALUES ({formatted_values});" + "\""
        
        # 최대 3회 재시도
        for i in range(0, 3):
            response = self._execute(query, mode="insert")
            
            if response:
                self.logger.info("Volt Insert Success!", extra={
                    "datatype": dtype, 
                    "data": data[fk.UK_PLAIN], 
                    "uk": data[fk.UK_PLAIN]
                })
                return True
            else:
                self.logger.error(f"Volt Insert Failed {i+1} Times!", extra={
                    "datatype": dtype, 
                    "data": data[fk.UK_PLAIN], 
                    "uk": data[fk.UK], 
                    "server": f"{self.stype}|{self.host}:{self.port}"
                })
                
            # 재시도 대기
            time.sleep(0.1)
            
        return False

    def select(self, key=None):
        """
        VoltDB에서 데이터 조회
        
        Args:
            key: WHERE 절 조건 (옵션)
                예: "WHERE edge_sys_2k_ip = '192.168.1.1'"
                
        Returns:
            list: 조회 결과 행 리스트
            
        처리:
        1. SELECT 쿼리 생성
        2. key가 있으면 WHERE 절 추가
        3. 쿼리 실행
        
        사용처:
        - _get_intersection_info(): 카메라 정보 조회
        
        주의:
        - 에러 발생 시 None 반환
        - soitgcamrinfo 테이블 고정 조회
        """
        try:
            # SELECT 쿼리 생성
            query = f"SELECT * FROM soitgcamrinfo "
            
            # WHERE 절 추가
            if key is not None:
                query += key
                
            # 쿼리 이스케이프
            query = "\"" + query + "\""
            
            return self._execute(query, mode="select")
        except Exception as e:
            print(f"voltdb select fail: {e}")
            
    def _retry_loop(self):
        """
        연결 재시도 및 초기화 루프 (백그라운드 스레드)
        
        처리 단계:
        1. VoltDB 연결 시도
        2. 테이블 스키마 조회
        3. 카메라 ID 조회 및 설정
        4. 차로 오프셋 조회 및 설정
        
        재시도 조건:
        - connected == False: VoltDB 미연결
        - DataSender.camera_id == None: 카메라 ID 미설정
        
        재시도 주기:
        - 10초마다 재시도
        
        주의사항:
        - 모든 단계 완료될 때까지 반복
        - 각 단계 실패 시 에러 로깅
        - camera_id 없으면 데이터 전송 불가
        """
        # 연결 또는 카메라 ID 설정 완료될 때까지 반복
        while self.connected is False or DataSender.camera_id is None:
            # 1. VoltDB 연결 시도
            if self.connected is False:
                try:
                    # HTTP 세션 생성
                    if self.session is None:
                        self.session = requests.Session()
                        
                    # 테이블 스키마 조회 (클래스 변수에 저장)
                    if VoltDBAdaptor.table_column_map is None:
                        VoltDBAdaptor.table_column_map = self._get_column_names()
                        
                    self.connected = True
                except Exception as e:
                    self.logger.error("Volt Connect Failed!", extra={
                        "server": f"{self.stype}|{self.host}:{self.port}", 
                        "error": e
                    })
                    self.logger.error("Retrying Every 10 Seconds to Reconnect")
                    
            # 2. 카메라 ID 조회 및 설정
            if DataSender.camera_id is None:
                try:
                    # Edge 서버 IP로 카메라 정보 조회
                    info = self._get_intersection_info()
                    
                    if info:
                        camera_id = info[2]  # 세 번째 컬럼이 camera_id
                        
                        if camera_id:
                            # DataSender 클래스 변수에 설정
                            DataSender.set_camera_id(camera_id)
                            self.logger.info(f"Volt CAM_ID Retrieved! CAM_ID: [{camera_id}]")
                    else:
                        self.logger.critical("NO CAM_ID FOR THIS EDGE'S IP IN VOLT!!")
                except Exception as e:
                    self.logger.error("Volt CAM_ID Fetch Failed!", extra={
                        "server": f"{self.stype}|{self.host}:{self.port}", 
                        "error": e
                    })
                    self.logger.error("Retrying Every 10 Seconds to Fetch CAM_ID")
            
            # 3. 차로 오프셋 조회 및 설정
            if DataSender.lane_offset is None:
                camera_id = DataSender.camera_id
                try:
                    if camera_id is not None:
                        self._get_lane_info(camera_id)
                except Exception as e:
                    self.logger.error("Volt LANE_INFO Fetch Failed!", extra={
                        "server": f"{self.stype}|{self.host}:{self.port}", 
                        "error": e
                    })
                    self.logger.error("Retrying Every 10 Seconds to Fetch LANE_INFO")
                    
            # 미완료 작업이 있으면 10초 대기 후 재시도
            if self.connected is False or DataSender.camera_id is None:
                time.sleep(10)
    
    def _get_lane_info(self, camera_id):
        """
        차로 정보 조회 및 오프셋 설정
        
        Args:
            camera_id: 카메라 고유 ID
            
        처리:
        1. SOITGLANEINFO 테이블에서 4K 검지 가능 차로 조회
        2. 가장 작은 차로 번호 조회
        3. lane_offset = lane_no - 1 계산
        4. DataSender에 설정
        
        예시:
        - 4K 차로가 3번부터 시작 → offset = 2
        - 4K 차로 1 → 실제 차로 3 (1 + 2)
        
        주의:
        - 4K 차로가 없으면 offset = 0
        - VHNO_4K_DTTN_YN = 'Y': 4K 번호판 검지 가능
        """
        # 4K 번호판 검지 가능 차로 중 최소 번호 조회
        query = f"\"SELECT LANE_NO FROM SOITGLANEINFO WHERE SPOT_CAMR_ID = \'{camera_id}\' and VHNO_4K_DTTN_YN = \'Y\' ORDER BY LANE_NO ASC LIMIT 1;\""
        rows = self._execute(query, mode="select")
        
        # 차로 번호 추출 (없으면 0)
        lane_no = int(rows[0][0]) if rows else 0
        
        # 오프셋 계산 (음수 방지)
        lane_no = max(0, lane_no - 1)
        
        # DataSender에 설정
        DataSender.set_lane_offset(lane_no)
        
        self.logger.info(f"Volt LANE_INFO Retrived! LANE_INFO: [{lane_no}]")
        
    def _get_intersection_info(self):
        """
        Edge 서버 IP로 교차로 정보 조회
        
        Returns:
            tuple or False: 교차로 정보 행 또는 False
            
        처리:
        1. eth0 인터페이스의 IPv4 주소 추출
        2. edge_sys_2k_ip 컬럼으로 조회
        3. 첫 번째 행 반환
        
        IPv4 추출:
        - 명령: ip addr show eth0
        - 정규식: (?<=inet )(.*)(?=\/)
        - 예: "inet 192.168.1.100/24" → "192.168.1.100"
        
        주의:
        - eth0 인터페이스 없으면 에러
        - IPv4 없으면 에러
        - 조회 결과 없으면 False 반환
        """
        # eth0 인터페이스의 IPv4 주소 추출
        ipv4 = re.search(
            re.compile(r'(?<=inet )(.*)(?=\/)', re.M), 
            os.popen('ip addr show eth0').read()
        ).groups()[0]
        
        # WHERE 절 생성
        query_key = f"WHERE edge_sys_2k_ip = '{ipv4}'"

        # 교차로 정보 조회
        intersection_info = self.select(key=query_key)
        
        if intersection_info:
            return intersection_info[0]  # 첫 번째 행 반환
        else:
            return False
    
    def _call_procedure(self, proc, params=None, sql=None):
        """
        VoltDB Stored Procedure 호출
        
        Args:
            proc: 프로시저 이름
                - "@AdHoc": SQL 직접 실행
                - "@SystemCatalog": 시스템 카탈로그 조회
            params: 프로시저 파라미터 (옵션)
            sql: SQL 쿼리 (proc="@AdHoc" 경우)
            
        Returns:
            dict: JSON 응답 결과
            
        URL 형식:
        - @AdHoc: /api/1.0/?Procedure=@AdHoc&Parameters=["{sql}"]
        - 기타: /api/1.0/?Procedure={proc}&Parameters={params}
        
        주의사항:
        - timeout=(0.5, 0.5): 연결 0.5초, 읽기 0.5초
        - 타임아웃 시 RequestException 발생
        - HTTP 상태 에러 시 raise_for_status() 예외 발생
        """
        if proc == "@AdHoc":
            # SQL 직접 실행
            url = f"{self.base_url}?Procedure=@AdHoc&Parameters=[{sql}]"
        else:
            # 프로시저 호출
            url = f"{self.base_url}?Procedure={proc}&Parameters=[\"COLUMNS\"]"
        
        # HTTP GET 요청
        resp = self.session.get(url, timeout=(0.5, 0.5))
        
        # HTTP 상태 에러 체크
        resp.raise_for_status()
        
        return resp.json()
    
    def _execute(self, query, mode):
        """
        SQL 쿼리 실행
        
        Args:
            query: 실행할 SQL 쿼리 (이스케이프 처리됨)
            mode: 실행 모드
                - "insert": 삽입 (True/False 반환)
                - "select": 조회 (데이터 행 반환)
                
        Returns:
            mode="insert": bool (성공/실패)
            mode="select": list (조회 결과 행)
            
        처리:
        1. @AdHoc 프로시저로 SQL 실행
        2. 응답 status 확인 (1: 성공)
        3. mode에 따라 결과 반환
        
        응답 구조:
        {
            "status": 1,  // 1: 성공, 기타: 실패
            "results": [
                {
                    "data": [[row1], [row2], ...]
                }
            ]
        }
        
        주의사항:
        - 네트워크 에러 시 False 반환
        - status != 1 시 에러 로깅 및 False 반환
        """
        try:
            # @AdHoc 프로시저로 SQL 실행
            data = self._call_procedure("@AdHoc", sql=query)
        except requests.RequestException as e:
            self.logger.error("Volt Request Failed!", extra={"error": e})
            return False
        
        # 쿼리 실행 상태 확인
        if data["status"] != 1:
            self.logger.error("Volt Query Failed!", extra={"error": data})
            return False
            
        # 모드에 따라 결과 반환
        if mode == "select":
            # 조회: 데이터 행 반환
            return data["results"][0]["data"]
            
        # 삽입: 성공 반환
        return True

    def _get_column_names(self):
        """
        VoltDB 테이블 스키마 조회
        
        Returns:
            dict: 테이블명 → 컬럼 리스트 매핑
            예: {
                "soitgrtmdtinfo_2K": ["spot_camr_id", "lane_no", ...],
                "soitgcwdtinfo": ["trce_id", "dttn_unix_tm", ...],
                ...
            }
            
        처리:
        1. @SystemCatalog("COLUMNS") 호출로 모든 컬럼 정보 조회
        2. 관심 테이블 목록 정의
        3. 테이블명을 소문자로 통일 (VoltDB는 대문자 저장)
        4. 각 테이블의 컬럼 리스트 생성
        
        응답 구조:
        [
            [schema, catalog, TABLE_NAME, COLUMN_NAME, ...],
            ...
        ]
        - row[2]: 테이블명 (대문자)
        - row[3]: 컬럼명
        
        주의사항:
        - VoltDB는 테이블명을 대문자로 저장
        - 컬럼명은 소문자로 변환하여 저장
        - 관심 테이블만 필터링
        """
        # 관심 테이블 목록
        tables = (
            ta.TABLE_2K,                 ta.TABLE_4K,             ta.TABLE_MERGE,
            ta.TABLE_PED,                ta.TABLE_STATS_APPROACH, ta.TABLE_STATS_TURNTYPE,
            ta.TABLE_STATS_VEHICLE_TYPE, ta.TABLE_STATS_LANE,     ta.TABLE_QUEUE_APPROACH,
            ta.TABLE_QUEUE_LANE,         ta.TABLE_ABNORMAL
        )
        
        # 테이블별 컬럼 리스트 초기화
        column_map = {t : [] for t in tables}
        
        # 대문자 → 원본 테이블명 매핑
        upper_to_orig = {t.upper(): t for t in tables}

        # 시스템 카탈로그에서 컬럼 정보 조회
        cat = self._call_procedure("@SystemCatalog", ["COLUMNS"])
        data = cat.get("results", [{}])[0].get("data", [])
        
        # 각 컬럼 정보 처리
        for row in data:
            table_upper = row[2]  # 테이블명 (대문자)
            col_name = row[3]     # 컬럼명
            
            # 관심 테이블인 경우 컬럼 추가
            if table_upper in upper_to_orig:
                column_map[upper_to_orig[table_upper]].append(col_name.lower())
                
        return column_map