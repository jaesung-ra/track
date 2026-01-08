"""
Data Sender Module
==================
처리된 데이터를 서버로 전송하고 실패 시 재전송을 관리하는 모듈

주요 기능:
1. 데이터 전처리 (해싱, 경로 변환 등)
2. 이미지 파일 전송 (API)
3. 데이터베이스 전송 (VoltDB, Middleware 등)
4. 전송 실패 데이터 SQLite 저장 및 재전송

처리 흐름:
큐에서 데이터 수신 → 전처리 → 이미지 전송 → DB 전송 → 실패 시 SQLite 저장
→ FailedDataSender가 주기적으로 재전송 시도

전송 대상:
- API 서버: 이미지 파일
- VoltDB/Middleware: 검지 데이터
"""

import os
import time
import threading
import json

from collections import defaultdict

from utils.hash import hash_sha256, hash_md5
from utils.file import generate_time_path
from utils.images import ImagePost
from utils.config_parser import ConfigParser
from utils.logger import get_logger

from data.constants import DataType as dt
from data.constants import FieldKey as fk

class DataSender:
    """
    데이터 전송 클래스
    
    Class Attributes:
        camera_id: 카메라 고유 ID (클래스 변수, 모든 인스턴스 공유)
        lane_offset: 차로 번호 오프셋 (4K 차로 번호 보정용)
        _cid_lock: camera_id 설정 시 동기화 락
        
    Instance Attributes:
        logger: 로거 인스턴스
        failed_data_sender: 전송 실패 데이터 재전송 관리자
        to_server_q: 서버 전송 대기 큐
        sqlite: SQLite 어댑터 (실패 데이터 저장용)
        servers: 전송 대상 서버 리스트
        flag: 스레드 종료 플래그
        POLL_INTERVAL: 재전송 시도 주기 (초)
        config: 설정 정보
        image_server_url: 이미지 서버 URL
        all_server_names: 모든 서버 이름 집합
        send_function: 데이터 타입별 전송 함수 매핑
        
    주의사항:
        - camera_id가 None이면 모든 데이터를 SQLite에 저장
        - 전송 실패 시 sent_to 플래그로 재전송 방지
        - 이미지는 바이트 또는 파일 형태로 전송 가능
    """
    
    # 클래스 변수 (모든 인스턴스 공유)
    camera_id = None
    lane_offset = None
    _cid_lock = threading.Lock()
    
    @classmethod
    def set_camera_id(cls, cid):
        """
        카메라 ID 설정 (클래스 메서드)
        
        Args:
            cid: 카메라 고유 ID
            
        주의:
        - 스레드 안전성을 위해 락 사용
        - 한 번 설정되면 모든 Sender 인스턴스에 적용
        """
        with cls._cid_lock:
            cls.camera_id = cid
            
    @classmethod
    def set_lane_offset(cls, lane_offset):
        """
        차로 번호 오프셋 설정 (클래스 메서드)
        
        Args:
            lane_offset: 차로 번호 오프셋
            
        용도:
        - 4K 카메라의 차로 번호를 실제 도로 차로 번호로 변환
        - 예: 오프셋 2 → 4K의 차로 1 → 실제 차로 3
        """
        with cls._cid_lock:
            cls.lane_offset = lane_offset
            
    def __init__(self, to_server_q, servers, sqlite):
        """
        DataSender 초기화
        
        Args:
            to_server_q: 서버 전송 대기 큐
            servers: 전송 대상 서버 리스트 (VoltDB, Middleware 등)
            sqlite: SQLite 어댑터 (전송 실패 데이터 저장)
        """
        self.logger = get_logger("sender")
        
        # 전송 실패 데이터 재전송 관리자 생성
        self.failed_data_sender = FailedDataSender(to_server_q, sqlite)
        
        self.to_server_q = to_server_q
        self.sqlite = sqlite
        self.servers = servers
        
        # 모든 서버 연결
        for server in self.servers:
            server.connect()
            
        self.flag = threading.Event()
        self.POLL_INTERVAL = 30
        self.config = ConfigParser.get()
        
        # 이미지 서버 URL 구성
        self.image_server_url = f'http://{self.config["image_remote"]["host"]}:{self.config["image_remote"]["port"]}/edge_api/img'
        
        # 모든 서버 이름 집합 (필터링용)
        self.all_server_names = {s.name for s in self.servers}
        
        # 데이터 타입별 커스텀 전송 함수 매핑
        self.send_function = {
            dt.VEHICLE_2K:     self._send_2k,
            dt.VEHICLE_RAW_4K: self._send_raw_4k,
            dt.QUEUE_APPROACH: self._send_wait,
            dt.INCIDENT_START: self._send_abnormal
        }
        
    def stop(self):
        """스레드 종료 플래그 설정"""
        self.flag.set()

    def main_loop(self):
        """
        메인 전송 루프
        
        처리 흐름:
        1. 큐에서 데이터 수신
        2. camera_id 없으면 SQLite에 저장 후 스킵
        3. 전송 상태 딕셔너리 초기화
        4. 데이터 전처리 (해싱, 경로 변환 등)
        5. 데이터 타입별 전송 함수 호출
        6. 전송 실패 시 SQLite에 저장
        
        전송 상태 관리:
        - sent_to: {server_name: True/False}
        - True: 전송 성공
        - False: 전송 실패 또는 미시도
        
        주의:
        - camera_id가 None이면 카메라 초기화 대기 중
        - _prepared 플래그로 중복 전처리 방지
        """
        while True:
            try:
                # 큐에서 데이터 수신 (블로킹)
                data = self.to_server_q.get()
                dtype = data[fk.DATA_TYPE]
                
                # 카메라 ID가 설정되지 않은 경우 SQLite에 저장
                if self.camera_id is None:
                    self.sqlite.insert(data, dtype)
                    continue
                
                # 전송 상태 딕셔너리 초기화
                if data.get("sent_to") is None:
                    data["sent_to"] = {}
                                
                sent_to = data["sent_to"]
                
                # 데이터 전처리 (처음 한 번만)
                if data.get("_prepared") is None:
                    self._build_before_insert(data)
                    
                # 전송 대상 서버 필터링
                destination_servers = data.get("_send_to")
                if destination_servers is None:
                    # 지정되지 않은 경우 모든 서버로 전송
                    allowed_servers = self.all_server_names
                else:
                    # 지정된 서버로만 전송
                    allowed_servers = destination_servers
                    
                # 데이터 타입별 전송 함수 선택
                send_func = self.send_function.get(dtype, self._send_default)
                
                # 데이터 전송 및 SQLite 저장 필요 여부 반환
                sqlite_insert = send_func(data, allowed_servers)
                        
                # 전송 실패 시 SQLite에 저장
                if sqlite_insert:
                    self.logger.info("Inserting Data to SQLite!", extra={"data":data[fk.UK_PLAIN]})
                    self.sqlite.insert(data, dtype)
            except Exception as e:
                self.logger.critical("Something Went Wrong in Sender!", extra={"error":e})

    def _send_default(self, data, allowed_servers):
        """
        기본 전송 함수 (데이터베이스만 전송)
        
        Args:
            data: 전송할 데이터
            allowed_servers: 전송 허용 서버 목록
            
        Returns:
            bool: SQLite 저장 필요 여부
            - True: 하나 이상의 서버 전송 실패
            - False: 모든 서버 전송 성공
            
        처리:
        1. 각 서버에 대해 전송 시도
        2. 이미 전송 성공한 서버는 스킵
        3. 전송 결과를 sent_to에 기록
        4. 하나라도 실패하면 SQLite 저장 필요
        
        주의:
        - sent_to[server_name] = True면 재전송 안 함
        - 여러 서버 중 일부만 실패해도 SQLite 저장
        """
        sqlite_insert = False
        sent_to = data["sent_to"]
        dtype = data[fk.DATA_TYPE]
        
        for server in self.servers:
            # 허용되지 않은 서버는 스킵
            if server.name not in allowed_servers:
                continue
                
            server_name = server.name
            
            # 전송 상태 초기화
            if server_name not in sent_to:
                sent_to[server_name] = False
                
            # 이미 전송 성공한 경우 스킵
            if sent_to[server_name] is True:
                continue
                
            # 데이터 전송 시도
            self.logger.info("Sending Data to Server!", extra={
                "datatype": data[fk.DATA_TYPE], 
                "data": data[fk.UK_PLAIN], 
                "uk": data[fk.UK], 
                "server": f"{server.stype}|{server.host}:{server.port}"
            })
            
            result = server.insert(data, dtype)
            sent_to[server_name] = result
            
            # 전송 실패 로깅
            if not result:
                self.logger.info("Data Send Failed!", extra={
                    "datatype": data[fk.DATA_TYPE], 
                    "data": data[fk.UK_PLAIN], 
                    "uk": data[fk.UK], 
                    "server": f"{server.stype}|{server.host}:{server.port}"
                })
                sqlite_insert = True if not sqlite_insert else sqlite_insert
                
        return sqlite_insert

    def _send_2k(self, data, allowed_servers):
        """
        2K 차량 데이터 전송 (이미지 + 데이터베이스)
        
        Args:
            data: 2K 차량 데이터
            allowed_servers: 전송 허용 서버 목록
            
        Returns:
            bool: SQLite 저장 필요 여부
            
        처리 흐름:
        1. 이미지 파일 전송 (API 서버)
        2. 데이터베이스 전송
        3. 이미지 전송, 데이터베이스 전송 둘 중 하나라도 실패 시 SQLite 저장
        
        """
        sqlite_insert = False
        sent_to = data["sent_to"]
        
        # 이미지 전송 상태 확인
        if "API" not in sent_to:
            sent_to["API"] = False
            
        # 이미지 미전송 시 전송 시도
        if sent_to["API"] is False:
            file_name = fk.CAR_IMAGE_FILE_NAME 
            is_posted = ImagePost.image_post_file(
                            self.image_server_url,
                            data[fk.IMAGE_FILE], 
                            data[fk.IMAGE_PATH_NAME], 
                            data[file_name]
                        )
            sent_to["API"] = is_posted
            sqlite_insert = True if not is_posted else False
            
        # 데이터베이스 전송 (이미지 전송 결과와 AND 연산)
        return self._send_default(data, allowed_servers) or sqlite_insert

    def _send_raw_4k(self, data, allowed_servers):
        """
        4K 원본 데이터 전송 (차량 이미지 + 번호판 이미지 + 데이터베이스)
        
        Args:
            data: 4K 원본 데이터 (OCR 처리 완료)
            allowed_servers: 전송 허용 서버 목록
            
        Returns:
            bool: SQLite 저장 필요 여부
            
        처리 흐름:
        1. 차량 이미지 바이트 전송 (API 서버)
        2. 번호판 이미지 바이트 전송 (API 서버)
        3. 전송 성공 시 메모리에서 이미지 삭제
        4. 데이터베이스 전송
        5. 이미지 전송, 데이터베이스 전송 둘 중 하나라도 실패 시 SQLite 저장

        주의:
        - 이미지는 바이트 형태로 메모리에 있음
        - 전송 성공 시 메모리 절약을 위해 삭제
        - 두 이미지 모두 전송 성공해야 API 전송 성공
        """
        sqlite_insert = False
        sent_to = data["sent_to"]
        
        # 이미지 전송 상태 확인
        if "API" not in sent_to:
            sent_to["API"] = False
            
        # 이미지 미전송 시 전송 시도
        if sent_to["API"] is False:
            # 차량 이미지 전송
            car_posted = ImagePost.image_post_bytes(
                self.image_server_url, 
                data[fk.IMAGE_BYTES_4K], 
                data[fk.IMAGE_PATH_NAME], 
                data[fk.CAR_IMAGE_FILE_NAME]
            )
            
            # 번호판 이미지 전송
            plate_posted = ImagePost.image_post_bytes(
                self.image_server_url, 
                data[fk.IMAGE_BYTES_PLATE_4K], 
                data[fk.IMAGE_PATH_NAME], 
                data[fk.PLATE_IMAGE_FILE_NAME]
            )
            
            # 두 이미지 모두 전송 성공 시
            is_posted = car_posted and plate_posted
            
            if is_posted:
                # 메모리에서 이미지 바이트 삭제
                del data[fk.IMAGE_BYTES_4K]
                del data[fk.IMAGE_BYTES_PLATE_4K]
                
            sent_to["API"] = is_posted
            
            if is_posted is False:
                sqlite_insert = True
                
        # 데이터베이스 전송
        return self._send_default(data, allowed_servers) or sqlite_insert

    def _send_wait(self, data, allowed_servers):
        """
        대기행렬 데이터 전송 (이미지 + 데이터베이스)
        
        처리 방식은 _send_2k와 동일
        
        Args:
            data: 대기행렬 데이터
            allowed_servers: 전송 허용 서버 목록
            
        Returns:
            bool: SQLite 저장 필요 여부
        """
        sqlite_insert = False
        sent_to = data["sent_to"]
        
        if "API" not in sent_to:
            sent_to["API"] = False
            
        if sent_to["API"] is False:
            file_name = fk.IMAGE_FILE_NAME
            is_posted = ImagePost.image_post_file(
                            self.image_server_url, 
                            data[fk.IMAGE_FILE], 
                            data[fk.IMAGE_PATH_NAME], 
                            data[file_name]
            )
            sent_to["API"] = is_posted
            
            if is_posted is False:
                sqlite_insert = True
                
        return self._send_default(data, allowed_servers) or sqlite_insert
    
    def _send_abnormal(self, data, allowed_servers):
        """
        돌발상황 데이터 전송 (이미지 + 데이터베이스)
        
        처리 방식은 _send_2k와 동일
        
        Args:
            data: 돌발상황 데이터
            allowed_servers: 전송 허용 서버 목록
            
        Returns:
            bool: SQLite 저장 필요 여부
        """
        sqlite_insert = False
        sent_to = data["sent_to"]
        
        if "API" not in sent_to:
            sent_to["API"] = False
            
        if sent_to["API"] is False:
            file_name = fk.IMAGE_FILE_NAME
            is_posted = ImagePost.image_post_file(
                            self.image_server_url, 
                            data[fk.IMAGE_FILE], 
                            data[fk.IMAGE_PATH_NAME], 
                            data[file_name]
            )
            sent_to["API"] = is_posted
            
            if is_posted is False:
                sqlite_insert = True
                
        return self._send_default(data, allowed_servers) or sqlite_insert

    def _build_before_insert(self, data):
        """
        전송 전 데이터 전처리
        
        Args:
            data: 전처리할 데이터
            
        처리 항목:
        1. 카메라 ID 설정 (없는 경우)
        2. 4K 차로 번호 오프셋 적용
        3. 고유 키 해싱
        4. 이미지 경로 변환 (로컬 → 원격)
        5. _prepared 플래그 설정
        
        주의:
        - 한 번만 실행되도록 _prepared 플래그 사용
        - 해싱 후 원본 ID는 OBJ_ID에 백업
        """
        dtype = data[fk.DATA_TYPE]
        
        # 카메라 ID 설정 (없는 경우)
        if data.get(fk.SPOT_CAMR_ID) is None:
            data[fk.SPOT_CAMR_ID] = self.camera_id
        
        # 4K 데이터는 차로 번호 오프셋 적용
        if dtype == dt.VEHICLE_RAW_4K:
            data[fk.LANE_NO] = str(int(data[fk.LANE_NO]) + self.lane_offset)
            
        # 고유 키 해싱
        self._hash_data(data)
        
        # 이미지 경로 변환 (이미지가 있는 데이터 타입만)
        if dtype in {dt.VEHICLE_2K, dt.VEHICLE_RAW_4K, dt.MERGE, dt.INCIDENT_START, dt.QUEUE_APPROACH, dt.QUEUE_LANES}:
            # 차량 데이터는 차량 이미지 파일명 사용
            if dtype in {dt.VEHICLE_2K, dt.VEHICLE_RAW_4K, dt.MERGE}:
                data[fk.CAR_IMAGE_FILE_NAME] = data[fk.IMAGE_PATH_NAME] + "/" + data[fk.CAR_IMAGE_FILE_NAME]
                
                # 4K는 번호판 이미지도 있음
                if dtype == dt.VEHICLE_RAW_4K:
                    data[fk.PLATE_IMAGE_FILE_NAME] = data[fk.IMAGE_PATH_NAME] + data[fk.PLATE_IMAGE_FILE_NAME]
            else:
                # 대기행렬, 돌발상황은 일반 이미지 파일명 사용
                data[fk.IMAGE_FILE_NAME] = data[fk.IMAGE_PATH_NAME] + "/" + data[fk.IMAGE_FILE_NAME]
                
            # 로컬 경로를 원격 경로로 변환
            data[fk.IMAGE_PATH_NAME] = self._get_remote_dir(data)
            
        # 전처리 완료 플래그 설정
        data["_prepared"] = True
        
    def _hash_data(self, data):
        """
        데이터 고유 키 해싱
        
        Args:
            data: 해싱할 데이터
            
        처리:
        1. camera_id + unique_key_plain → SHA256 해싱
        2. 원본 차량 ID는 OBJ_ID에 백업
        3. 해시된 값으로 차량 ID 대체
        
        목적:
        - 전역적으로 고유한 ID 생성
        """
        dtype = data[fk.DATA_TYPE]
        
        # 고유 키 해싱 (camera_id + plain_key)
        data[fk.UK] = hash_sha256(self.camera_id + data[fk.UK_PLAIN])
        
        # 데이터 타입별 차량 ID 해싱
        if dtype == dt.VEHICLE_2K:
            data[fk.OBJ_ID] = data[fk.CAR_ID_2K]
            data[fk.CAR_ID_2K] = data[fk.UK]
            
        elif dtype == dt.MERGE:
            data[fk.OBJ_ID] = data[fk.CAR_ID]
            data[fk.CAR_ID] = data[fk.UK]
            
        elif dtype == dt.VEHICLE_RAW_4K:
            data[fk.OBJ_ID] = data[fk.CAR_ID_4K]
            data[fk.CAR_ID_4K] = data[fk.UK]
    
    def _get_remote_dir(self, data):
        """
        원격 이미지 저장 경로 생성 및 파일명 해싱
        
        Args:
            data: 이미지 정보가 포함된 데이터
            
        Returns:
            str: 원격 저장 경로
            
        처리:
        1. 이미지 파일명에서 생성 시각 추출
        2. 이미지 타입 결정 (차량:10, 번호판:20, 대기행렬:20, 돌발:30)
        3. 파일명 해싱 (MD5)
        4. 원격 경로 구성: {base}/{camera_id}/{year}/{month}/{day}/{image_type}/{file}
        
        파일명 형식:
        - 차량/번호판/돌발: {image_type}_{hash}.jpg
        - 대기행렬: {original_name}.jpg
        
        주의:
        - data[IMAGE_FILE]에 로컬 파일 경로 저장
        - 번호판 이미지 없으면 "N_PLATE"로 설정
        """
        dtype = data[fk.DATA_TYPE]
        
        # 이미지 파일명 추출 및 생성 시각 파싱
        if dtype in {dt.VEHICLE_2K, dt.VEHICLE_RAW_4K, dt.MERGE}:
            image_name = data[fk.CAR_IMAGE_FILE_NAME].split("/")[-1][:-4]
            data[fk.IMAGE_FILE] = data[fk.CAR_IMAGE_FILE_NAME]
        else:
            image_name = data[fk.IMAGE_FILE_NAME].split("/")[-1][:-4]
            data[fk.IMAGE_FILE] = data[fk.IMAGE_FILE_NAME]
            
        # 파일명에서 타임스탬프 추출 (마지막 언더스코어 뒤)
        image_created_time = image_name.split("_")[-1]
        
        # 데이터 타입별 이미지 타입 및 경로 설정
        if dtype in {dt.VEHICLE_2K, dt.MERGE}:
            image_type = 10  # 2K 차량 이미지
            rp = "car_image_path_2k"
            data[fk.CAR_IMAGE_FILE_NAME] = f"{image_type}_{hash_md5(data[fk.CAR_IMAGE_FILE_NAME])}.jpg"
            
        elif dtype == dt.VEHICLE_RAW_4K:
            image_type = 10  # 4K 차량 이미지
            rp = "car_image_path_4k"
            data[fk.CAR_IMAGE_FILE_NAME] = f"{image_type}_{hash_md5(data[fk.CAR_IMAGE_FILE_NAME])}.jpg"
            
            # 번호판 이미지 파일명 설정
            if data.get(fk.IMAGE_BYTES_PLATE_4K) is not None:
                data[fk.PLATE_IMAGE_FILE_NAME] = f"20_{hash_md5(data[fk.PLATE_NUM])}.jpg"
            else:
                data[fk.PLATE_IMAGE_FILE_NAME] = "N_PLATE"
                
        elif dtype == dt.INCIDENT_START:
            image_type = 30  # 돌발상황 이미지
            rp = "abnormal_image_path"
            data[fk.IMAGE_FILE_NAME] = f"{image_type}_{hash_md5(data[fk.IMAGE_FILE_NAME])}.jpg"
            
        else:
            image_type = 20  # 대기행렬 이미지
            rp = "queue_image_path"
            data[fk.IMAGE_FILE_NAME] = f"{image_name}.jpg"
        
        # 원격 경로 구성: {base}/{camera_id}/{year}/{month}/{day}/{image_type}/
        return os.path.join(
            self.config["image_remote"][rp], 
            self.camera_id, 
            generate_time_path(int(image_created_time), image_type)
        )


class FailedDataSender:
    """
    전송 실패 데이터 재전송 관리 클래스
    
    Attributes:
        logger: 로거 인스턴스
        to_server_q: 서버 전송 큐
        sqlite: SQLite 어댑터
        flag: 스레드 종료 플래그
        POLL_INTERVAL: 재전송 시도 주기 (초)
        
    동작 방식:
    1. 주기적으로 SQLite에서 실패 데이터 조회
    2. 데이터를 서버 전송 큐로 재투입
    3. 전송 성공 시 SQLite에서 삭제
    
    주의사항:
    - camera_id가 설정된 후에만 동작
    - 재전송 실패 시 SQLite에 계속 유지
    - 무한 재시도 (성공할 때까지)
    """
    
    def __init__(self, to_server_q, sqlite):
        """
        FailedDataSender 초기화
        
        Args:
            to_server_q: 재전송할 데이터를 넣을 큐
            sqlite: SQLite 어댑터
        """
        self.logger = get_logger("sqlite")
        self.to_server_q = to_server_q
        self.sqlite = sqlite
        self.flag = threading.Event()
        
        # 재전송 시도 주기 (SQLite 설정에서 가져옴)
        self.POLL_INTERVAL = self.sqlite.interval
        
    def stop(self):
        """스레드 종료 플래그 설정"""
        self.flag.set()

    def main_loop(self):
        """
        재전송 메인 루프
        
        처리 흐름:
        1. POLL_INTERVAL마다 깨어남
        2. camera_id 설정 여부 확인
        3. SQLite에서 실패 데이터 조회
        4. 데이터를 defaultdict로 변환
        5. 서버 전송 큐로 재투입
        6. SQLite에서 삭제
        
        주의:
        - camera_id가 None이면 스킵 (초기화 대기)
        - 조회된 데이터는 즉시 삭제 (중복 재전송 방지)
        - 재전송 실패 시 Sender가 다시 SQLite에 저장
        """
        while not self.flag.is_set():
            # camera_id가 설정된 경우에만 재전송 시도
            if DataSender.camera_id is not None:
                try:
                    # SQLite에서 실패 데이터 조회 (제너레이터)
                    for row in iter(self.sqlite.get, None):
                        _id, payload = row
                        
                        # JSON 파싱
                        payload = json.loads(payload)
                        
                        self.logger.info("Fetched Data from SQLite!", extra={
                            "datatype": payload[fk.DATA_TYPE], 
                            "data": payload[fk.UK_PLAIN]
                        })
                        
                        # defaultdict로 변환 (안전한 키 접근)
                        payload = defaultdict(lambda: 'NULL', payload)
                        
                        # 서버 전송 큐로 재투입
                        self.to_server_q.put(payload)
                        
                        # SQLite에서 삭제
                        self.sqlite.delete_by_id(_id)
                        
                except Exception as e:
                    self.logger.critical("Accessing SQLite Went Wrong!", extra={"error":e})

            # 다음 시도까지 대기
            self.flag.wait(self.POLL_INTERVAL)