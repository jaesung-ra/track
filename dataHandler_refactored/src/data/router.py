"""
Data Router Module
==================
Redis 데이터를 파싱하고 적절한 큐로 라우팅하는 모듈

주요 기능:
1. 데이터 타입별 파싱 및 변환
2. 목적지 큐 결정 (server/merge/ocr)
3. 전송 대상 서버 태그 설정
4. 고유 키(unique_key) 생성

지원 데이터 타입:
- 차량: 2K, 4K, RAW_4K, MERGE
- 보행자: PED_2K
- 통계/대기행렬: STATS, QUEUE
- 돌발상황: INCIDENT
- SQLite: SQLITE_ST/LT/RT
- 존재감지: PRESENCE_VH/WAIT/CROSS

특수 처리:
- 루원 사이트: 회전 방향별 카메라 ID/차로 변환
"""

import os
import json
import copy

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Union, Callable, Tuple
from collections import defaultdict

from utils.config_parser import ConfigParser
from utils.logger import get_logger

from data.constants import DataType as dt
from data.constants import FieldKey as fk

logger = get_logger("receiver")

# 루원 사이트 특수 설정 로드
luwon_cfg = ConfigParser.get()["special_site"]

@dataclass
class BuildResult:
    """
    라우팅 결과 데이터 클래스
    
    Attributes:
        to_server: 서버로 직접 전송할 데이터 리스트
        to_merge: 병합 처리가 필요한 데이터 리스트 (2K/4K 매칭용)
        to_ocr: OCR 처리가 필요한 데이터 리스트 (4K 원본 이미지)
        
    사용처:
        - receiver.py에서 각 큐로 데이터 분배
        - merger.py로 병합 데이터 전달
        - lp_detector.py로 OCR 데이터 전달
    """
    to_server : List[Dict[str, Any]] = field(default_factory = list)
    to_merge  : List[Dict[str, Any]] = field(default_factory = list)
    to_ocr    : List[Dict[str, Any]] = field(default_factory = list)


# ============================================================================
# CSV 파싱 매핑 테이블
# ============================================================================
# 용도: CSV 형식 데이터(2K, 4K, 보행자)를 JSON 딕셔너리로 변환
# 주의: 인덱스 순서가 실제 CSV 컬럼 순서와 일치해야 함
# ============================================================================

# DS(DeepStream)가 생성한 2K 차량 데이터 파싱
CSV_JSON_MAP_2K = [
    fk.CAR_ID_2K,           # 0: 차량 고유 ID
    fk.CAR_TYPE,            # 1: 차종 코드
    fk.LANE_NO,             # 2: 차로 번호
    fk.TURN_TYPE_CD,        # 3: 회전 유형 (11: 직진, 21: 좌회전, 31: 우회전)
    fk.TURN_TIME,           # 4: 회전 검지 시각
    fk.TURN_SPEED,          # 5: 회전 구간 속도
    fk.STOP_PASS_TIME,      # 6: 정지선 통과 시각 (매칭 기준)
    fk.STOP_PASS_SPEED,     # 7: 정지선 통과 속도
    fk.INTERVAL_SPEED,      # 8: 구간 평균 속도
    fk.FIRST_DET_TIME,      # 9: 최초 검지 시각
    fk.OBSERVE_TIME,        # 10: 관측 시각
    fk.IMAGE_PATH_NAME,     # 11: 이미지 파일 경로
    fk.CAR_IMAGE_FILE_NAME, # 12: 차량 이미지 파일명
]

# DS(DeepStream)가 생성한 보행자 데이터 파싱
CSV_JSON_MAP_PED = [
    fk.TRACE_ID,     # 0: 추적 ID
    fk.PED_DET_TIME, # 1: 보행자 검지 시각
    fk.PED_DIR,      # 2: 보행자 이동 방향
]

# DS(DeepStream)가 생성한 4K 원본 데이터 파싱 (OCR 처리 전)
CSV_JSON_MAP_RAW_4K = [
    fk.CAR_ID_4K,             # 0: 차량 고유 ID
    fk.STOP_PASS_TIME,        # 1: 정지선 통과 시각
    fk.LANE_NO,               # 2: 차로 번호
    fk.CAR_TYPE,              # 3: 차종 코드
    fk.IMAGE_PATH_NAME,       # 4: 이미지 파일 경로
]


# ============================================================================
# 메인 라우팅 함수
# ============================================================================
        
def route_data(redis_data, ltype, send_to):
    """
    Redis 데이터를 타입별로 파싱하고 라우팅
    
    Args:
        redis_data: Redis에서 수신한 원본 데이터 (CSV 또는 JSON)
        ltype: 데이터 타입 (DataType 클래스 참조)
        send_to: 전송 대상 서버 목록
        
    Returns:
        BuildResult: 라우팅된 데이터 (to_server, to_merge, to_ocr)
        
    처리 흐름:
    1. ltype에 해당하는 build 함수 검색
    2. build 함수로 데이터 파싱 및 변환
    3. 전송 대상 서버 태그 설정
    
    예외 처리:
    - 지원하지 않는 ltype: 빈 BuildResult 반환
    - build 실패: 빈 BuildResult 반환
    """
    try:
        # 데이터 타입에 맞는 build 함수 검색
        build_function = BUILD_FUNCTION_MAP[ltype]
    except KeyError:
        logger.error("Builder Function Does Not Exist!", extra={"datatype":ltype})
        return BuildResult()
    
    try:
        # 데이터 파싱 및 변환
        res = build_function(redis_data, ltype)
    except Exception as e:
        logger.error("Failed to Build Data!", extra={"datatype":ltype, "error":e})
        return BuildResult()
        
    # 전송 대상 서버 태그 설정
    res = _set_destination(res, send_to)
    return res


# ============================================================================
# 특수 사이트 처리 함수
# ============================================================================

def build_luwon(redis_data):
    """
    루원 사이트 특수 처리
    
    기능:
    - 회전 방향(11/21/31)에 따라 카메라 ID 변환
    - 차로 번호를 실제 도로 차로 번호로 매핑
    
    Args:
        redis_data: 2K 차량 데이터 (TURN_TYPE_CD 포함)
        
    Returns:
        BuildResult: 
        - to_server: [변환된 2K 데이터, 병합 테이블용 데이터]
        - to_merge: [원본 차로 번호 유지 데이터]
        
    처리 로직:
    1. 회전 방향 → 카메라 ID 매핑
    2. 차로 번호 변환:
       - 1개 차로: 고정 매핑
       - 2개 차로: lane_no 1,2 → 첫번째, 3,4 → 두번째
       - 3개 차로: lane_no 1,2 → 첫번째, 3 → 두번째, 4 → 세번째
       - 4개 이상: 1:1 매핑
    3. 병합 테이블용 복사본 생성 (번호판 없음으로 설정)
    
    주의:
    - 원본 데이터는 to_merge로 전달 (4K 매칭용)
    - 변환 데이터는 to_server로 전달 (실제 전송용)
    """
    # 회전 방향 코드 → 문자열 변환
    dir_map = {"11":"straight", "21":"left", "31":"right"}
    direction = dir_map[redis_data[fk.TURN_TYPE_CD]]
    
    # 회전 방향별 카메라 ID 설정
    redis_data[fk.SPOT_CAMR_ID] = luwon_cfg["dir"][direction]["cam_id"]
    
    # 원본 차로 번호 유지 (병합용)
    to_merge_q = copy.deepcopy(redis_data)
    
    # 차로 번호 변환 로직
    length = len(luwon_cfg["dir"][direction]["lane"])
    
    if length == 1:
        # 단일 차로: 고정 매핑
        redis_data[fk.LANE_NO] = str(luwon_cfg["dir"][direction]["lane"][0])
        
    elif length == 2:
        # 2개 차로: 1,2 → 첫번째, 3,4 → 두번째
        index = 0 if int(redis_data[fk.LANE_NO]) in (1, 2) else 1
        redis_data[fk.LANE_NO] = str(luwon_cfg["dir"][direction]["lane"][index])
        
    elif length == 3:
        # 3개 차로: 1,2 → 첫번째, 3 → 두번째, 4 → 세번째
        index = 0 if int(redis_data[fk.LANE_NO]) in (1, 2) else 1
        index = 2 if int(redis_data[fk.LANE_NO]) == 4 else index
        redis_data[fk.LANE_NO] = str(luwon_cfg["dir"][direction]["lane"][index])
        
    else:
        # 4개 이상 차로: 1:1 매핑
        redis_data[fk.LANE_NO] = str(luwon_cfg["dir"][direction]["lane"][int(redis_data[fk.LANE_NO])-1])
    
    # 병합 테이블용 데이터 생성
    to_merge_table = copy.deepcopy(redis_data)
    to_merge_table[fk.DATA_TYPE] = dt.MERGE
    to_merge_table[fk.PLATE_YN] = "N"  # 번호판 없음
    to_merge_table[fk.CAR_ID] = redis_data[fk.CAR_ID_2K]
    to_merge_table["_send_to"] = ["middleware"]
    
    return BuildResult(
        to_server = [redis_data, to_merge_table], 
        to_merge = [to_merge_q]
    )


# ============================================================================
# 유틸리티 함수
# ============================================================================
    
def _set_destination(res, send_to):
    """
    데이터에 전송 대상 서버 태그 설정
    
    Args:
        res: BuildResult 인스턴스
        send_to: 전송 대상 서버 목록 (예: ["voltdb", "middleware"])
        
    Returns:
        BuildResult: 태그가 설정된 데이터
        
    처리:
    - to_server 데이터에 "_send_to" 태그 추가
    - MERGE 타입은 기존 "_send_to" 유지 (루원 특수 처리)
    - to_ocr, to_merge 데이터에도 태그 추가
    
    주의:
    - send_to가 None이면 스킵
    - "_send_to"는 sender.py에서 서버 필터링에 사용됨
    """
    if send_to is None:
        return res
    
    # 서버 전송 데이터 태그 설정
    for d in res.to_server: 
        if d[fk.DATA_TYPE] == dt.MERGE:
            # MERGE 타입은 기존 태그 유지 (루원 특수 처리)
            if d.get("_send_to") is None:
                d["_send_to"] = list(send_to)
            else:
                continue
        else:
            d["_send_to"] = list(send_to)
            
    # OCR 및 병합 데이터 태그 설정
    for d in res.to_ocr:    d["_send_to"] = list(send_to) 
    for d in res.to_merge:  d["_send_to"] = list(send_to) 
        
    return res


# ============================================================================
# 데이터 타입별 빌드 함수
# ============================================================================

def _route_2k(redis_data, ltype):
    """
    2K 차량 데이터 파싱 및 라우팅
    
    입력 형식: CSV 문자열
    예: "12345,PCAR,1,11,1234567890,50,..."
    
    처리:
    1. CSV를 딕셔너리로 변환
    2. 고유 키 생성 (직렬화)
    3. 루원 사이트인 경우 특수 처리
    4. 일반 사이트: 2K + 병합 테이블 데이터 생성
    
    Returns:
        BuildResult:
        - to_server: [2K 데이터, 병합 테이블 데이터]
        - to_merge: [병합 대기 데이터]
    """
    # CSV 파싱
    redis_data = redis_data.split(',')
    redis_data = dict(zip(CSV_JSON_MAP_2K, redis_data))
    redis_data[fk.DATA_TYPE] = ltype
    
    # 고유 키 생성 (로깅 및 추적용)
    redis_data[fk.UK_PLAIN] = _serialize_2k(redis_data)
    
    logger.info("Received Data!", extra={"datatype":redis_data[fk.DATA_TYPE], "data":redis_data[fk.UK_PLAIN]})
    logger.debug(f"Full Data: [{dict(redis_data)}]")
    
    # 루원 사이트 특수 처리
    if luwon_cfg["enabled"]:
        result = build_luwon(redis_data)
        return result
    
    # 일반 사이트: 병합 테이블용 데이터 생성
    to_merge_table = copy.deepcopy(redis_data)
    to_merge_table[fk.DATA_TYPE] = dt.MERGE
    to_merge_table[fk.PLATE_YN] = "N"  # 번호판 정보 없음
    to_merge_table[fk.CAR_ID] = redis_data[fk.CAR_ID_2K]
    
    return BuildResult(
        to_server = [redis_data, to_merge_table], 
        to_merge = [copy.deepcopy(redis_data)]
    )
    
def _route_raw_4k(redis_data, ltype):
    """
    4K 원본 데이터 파싱 및 OCR 큐로 라우팅
    
    입력 형식: CSV 문자열
    예: "67890,1234567890,1,PCAR,/images/path"
    
    처리:
    1. CSV를 딕셔너리로 변환
    2. 고유 키 생성
    3. OCR 처리를 위해 to_ocr로 라우팅
    
    Returns:
        BuildResult:
        - to_ocr: [OCR 처리 대기 데이터]
        
    주의:
    - 이 데이터는 lp_detector.py로 전달되어 번호판 검출/OCR 수행
    """
    # CSV 파싱
    redis_data = redis_data.split(',')
    redis_data = dict(zip(CSV_JSON_MAP_RAW_4K, redis_data))
    redis_data[fk.DATA_TYPE] = ltype
    
    # 고유 키 생성
    redis_data[fk.UK_PLAIN] = _serialize_raw_4k(redis_data)
    
    logger.info("Received Data!", extra={"datatype":redis_data[fk.DATA_TYPE], "data":redis_data[fk.UK_PLAIN]})
    logger.debug(f"Full Data: [{dict(redis_data)}]")
    
    # OCR 처리 큐로 라우팅
    return BuildResult(
        to_ocr = [redis_data]
    )
    
def _route_4k(redis_data, ltype):
    """
    4K 처리 완료 데이터 파싱 및 병합 큐로 라우팅
    
    입력 형식: JSON 문자열
    예: {"car_id_4k": "...", "plate_num": "12가3456", ...}
    
    처리:
    1. JSON 파싱
    2. 2K 데이터와 병합을 위해 to_merge로 라우팅
    
    Returns:
        BuildResult:
        - to_merge: [병합 대기 데이터]
        
    주의:
    - lp_detector.py에서 OCR 처리 완료 후 생성되는 데이터
    - merger.py에서 2K 데이터와 시각 기준으로 매칭됨
    """
    redis_data = json.loads(redis_data)
    redis_data[fk.DATA_TYPE] = ltype
    
    logger.info("Received Data!", extra={"datatype":redis_data[fk.DATA_TYPE], "data":redis_data[fk.UK_PLAIN]})
    logger.debug(f"Full Data: [{dict(redis_data)}]")

    # 병합 처리 큐로 라우팅
    return BuildResult(
        to_server = [copy.deepcopy(redis_data)],
        to_merge = [redis_data]
    )
    
def _route_sqlite_stats(redis_data, ltype):
    """
    SQLite 통계 데이터 파싱 및 필터링
    
    입력 형식: JSON 문자열
    
    처리:
    - SQLITE_ST: 직진(11) 데이터만 전송
    - SQLITE_LT: 좌회전(21) 데이터만 전송
    - SQLITE_RT: 우회전(31) 데이터만 전송
    
    Returns:
        BuildResult:
        - to_server: [필터링된 통계 데이터]
        
    주의:
    - "_prepared" 플래그 설정 (sender에서 추가 처리 생략)
    """
    to_server = []
    redis_data = json.loads(redis_data)
    redis_data[fk.DATA_TYPE] = ltype
    
    # 회전 유형별 필터링
    if ltype == dt.SQLITE_ST and redis_data[fk.TURN_TYPE_CD] == "11":
        to_server.append(redis_data)
    elif ltype == dt.SQLITE_LT and redis_data[fk.TURN_TYPE_CD] == "21":
        to_server.append(redis_data)
    elif ltype == dt.SQLITE_RT and redis_data[fk.TURN_TYPE_CD] == "31":
        to_server.append(redis_data)
    
    logger.info("Received Data!", extra={"datatype":redis_data[fk.DATA_TYPE], "data":redis_data[fk.UK_PLAIN]})
    logger.debug(f"Full Data: [{dict(redis_data)}]")
    
    # 이미 준비된 데이터임을 표시
    redis_data["_prepared"] = True
    
    return BuildResult(
        to_server = to_server
    )
    
def _route_stats(redis_data, ltype):
    """
    통계 데이터 파싱 및 라우팅
    
    입력 형식: JSON 문자열
    예: {
        "approach": {...},
        "turn_types": [...],
        "lanes": [...]
    }
    
    처리:
    1. JSON 파싱
    2. 각 통계 유형별로 데이터 분리
    3. 고유 키 생성
    4. 모든 데이터를 to_server로 라우팅
    
    Returns:
        BuildResult:
        - to_server: [통계 데이터 리스트]
        
    주의:
    - 하나의 Redis 메시지에 여러 통계 데이터 포함 가능
    - dict 또는 list 형태 모두 지원
    """
    to_server = []
    redis_data = json.loads(redis_data)
    redis_name = redis_data.keys()
    
    # 각 통계 유형별 처리
    for name in redis_name:
        data = redis_data[name]
        
        # 단일 통계 데이터 (dict)
        if isinstance(data, dict):
            data[fk.DATA_TYPE] = name + "_stats"
            data[fk.UK_PLAIN] = _serialize_stats(data)
            
            logger.info("Received Data!", extra={"datatype":data[fk.DATA_TYPE], "data":data[fk.UK_PLAIN]})
            logger.debug(f"Full Data: [{dict(data)}]")
            
            to_server.append(data)
            
        # 복수 통계 데이터 (list)
        elif isinstance(data, list):
            for item in data:
                item[fk.DATA_TYPE] = name + "_stats"
                item[fk.UK_PLAIN] = _serialize_stats(item)
                
                logger.info("Received Data!", extra={"datatype":item[fk.DATA_TYPE], "data":item[fk.UK_PLAIN]})
                logger.debug(f"Full Data: [{dict(item)}]")
                
                to_server.append(item)
                
    return BuildResult(
        to_server = to_server
    )

def _route_queue(redis_data, ltype):
    """
    대기행렬 데이터 파싱 및 라우팅
    
    입력 형식: JSON 문자열
    예: {
        "approach": {...},
        "lanes": [...]
    }
    
    처리 과정은 _route_stats와 동일
    
    Returns:
        BuildResult:
        - to_server: [대기행렬 데이터 리스트]
    """
    to_server = []
    redis_data = json.loads(redis_data)
    redis_name = redis_data.keys()
    
    # 각 대기행렬 유형별 처리
    for name in redis_name:
        data = redis_data[name]
        
        # 단일 대기행렬 데이터 (dict)
        if isinstance(data, dict):
            data[fk.DATA_TYPE] = name + "_queue"
            data[fk.UK_PLAIN] = _serialize_queue(data)
            
            logger.info("Received Data!", extra={"datatype":data[fk.DATA_TYPE], "data":data[fk.UK_PLAIN]})
            logger.debug(f"Full Data: [{dict(data)}]")
            
            to_server.append(data)
            
        # 복수 대기행렬 데이터 (list)
        elif isinstance(data, list):
            for item in data:
                item[fk.DATA_TYPE] = name + "_queue"
                item[fk.UK_PLAIN] = _serialize_queue(item)
                
                logger.info("Received Data!", extra={"datatype":item[fk.DATA_TYPE], "data":item[fk.UK_PLAIN]})
                logger.debug(f"Full Data: [{dict(item)}]")
                
                to_server.append(item)
                
    return BuildResult(
        to_server = to_server
    )
    
def _route_ped(redis_data, ltype):
    """
    보행자 데이터 파싱 및 라우팅
    
    입력 형식: CSV 문자열
    예: "trace_id,1234567890,direction_code"
    
    처리:
    1. CSV를 딕셔너리로 변환
    2. 고유 키 생성
    3. to_server로 직접 전송
    
    Returns:
        BuildResult:
        - to_server: [보행자 데이터]
    """
    # CSV 파싱
    redis_data = redis_data.split(',')
    redis_data = dict(zip(CSV_JSON_MAP_PED, redis_data))
    redis_data[fk.DATA_TYPE] = ltype
    
    # 고유 키 생성
    redis_data[fk.UK_PLAIN] = _serialize_ped(redis_data)
    
    logger.info("Received Data!", extra={"datatype":redis_data[fk.DATA_TYPE], "data":redis_data[fk.UK_PLAIN]})
    logger.debug(f"Full Data: [{dict(redis_data)}]")
    
    return BuildResult(
        to_server = [redis_data]
    )

def _route_abnormal(redis_data, ltype):
    """
    돌발상황 데이터 파싱 및 라우팅
    
    입력 형식: JSON 문자열
    예: {"start": {...}} 또는 {"end": {...}}
    
    처리:
    1. JSON 파싱
    2. start/end 모드 구분
    3. 데이터 타입 설정 (abn_start 또는 abn_end)
    4. 고유 키 생성
    
    Returns:
        BuildResult:
        - to_server: [돌발상황 데이터]
        
    주의:
    - start: 돌발상황 시작 이벤트
    - end: 돌발상황 종료 이벤트
    """
    redis_data = json.loads(redis_data)
    
    # start 또는 end 모드 추출
    (mode, payload), = redis_data.items()
    
    # 데이터 타입 설정
    payload[fk.DATA_TYPE] = ltype + "_start" if mode == "start" else ltype + "_end"
    
    # 고유 키 생성
    payload[fk.UK_PLAIN] = _serialize_abnormal(payload)
    
    logger.info("Received Data!", extra={"datatype":payload[fk.DATA_TYPE], "data":payload[fk.UK_PLAIN]})
    logger.debug(f"Full Data: [{dict(payload)}]")
    
    return BuildResult(
        to_server = [payload]
    )

def _route_presence(redis_data, ltype):
    """
    존재 감지 데이터 파싱 및 라우팅
    
    입력 형식: 정수 문자열
    예: "0" 또는 "1"
    
    처리:
    1. 정수로 변환
    2. 간단한 페이로드 생성
    3. to_server로 직접 전송
    
    Returns:
        BuildResult:
        - to_server: [존재 감지 데이터]
        
    주의:
    - 0: 없음
    - 1: 있음
    """
    state = int(redis_data.strip())
    
    payload = {
        fk.DATA_TYPE: ltype,
        fk.PRESENCE_STATE: state,
        fk.UK_PLAIN: state
    }
    
    logger.info("Received Data!", extra={"datatype":payload[fk.DATA_TYPE], "data":payload[fk.UK_PLAIN]})
    
    return BuildResult(
        to_server = [payload]
    )


# ============================================================================
# 빌드 함수 매핑 테이블
# ============================================================================
# 용도: 데이터 타입에 따른 빌드 함수 선택
# 주의: 새로운 데이터 타입 추가 시 여기에 등록 필요
# ============================================================================
BUILD_FUNCTION_MAP = {
    dt.VEHICLE_2K     : _route_2k,
    dt.VEHICLE_RAW_4K : _route_raw_4k,
    dt.VEHICLE_4K     : _route_4k,
    dt.PED_2K         : _route_ped,
    dt.STATS          : _route_stats,
    dt.QUEUE          : _route_queue,
    dt.INCIDENT       : _route_abnormal,
    dt.SQLITE_ST      : _route_sqlite_stats,
    dt.SQLITE_LT      : _route_sqlite_stats,
    dt.SQLITE_RT      : _route_sqlite_stats,
    dt.PRESENCE_VH    : _route_presence,
    dt.PRESENCE_WAIT  : _route_presence,
    dt.PRESENCE_CROSS : _route_presence,
}


# ============================================================================
# 고유 키 직렬화 함수
# ============================================================================
# 용도: 데이터를 고유하게 식별할 수 있는 문자열 생성
# 사용처: 로깅, 추적, 중복 검사
# 주의: 필드 순서와 구분자(,) 일관성 유지 필요
# ============================================================================

def _serialize_2k(data):
    """
    2K 차량 데이터 고유 키 생성
    
    키 구성:
    car_id,stop_pass_time,car_type,lane_no,turn_time,stop_pass_speed,image_file
    
    예: "12345,1234567890,PCAR,1,1234567885,50,image.jpg"
    """
    try:
        key = (
            str(data[fk.CAR_ID_2K]) + ","
            + str(data[fk.STOP_PASS_TIME]) + ","
            + str(data[fk.CAR_TYPE]) + ","
            + str(data[fk.LANE_NO]) + ","
            + str(data[fk.TURN_TIME]) + ","
            + str(data[fk.STOP_PASS_SPEED]) + ","
            + str(data[fk.CAR_IMAGE_FILE_NAME])
        )
        return key
    except Exception as e:
        logger.error(f"Failed to Serialize 2K Data: [{e}]")
        
def _serialize_raw_4k(data):
    """
    4K 원본 데이터 고유 키 생성
    
    키 구성:
    car_id_4k,stop_pass_time,car_type,lane_no
    
    예: "67890,1234567890,PCAR,1"
    """
    try:
        key = (
            str(data[fk.CAR_ID_4K]) + ","
            + str(data[fk.STOP_PASS_TIME]) + ","
            + str(data[fk.CAR_TYPE]) + ","
            + str(data[fk.LANE_NO])
        )
        return key
    except Exception as e:
        logger.error(f"Failed to Serialize 4K Data: [{e}]")
        
def _serialize_ped(data):
    """
    보행자 데이터 고유 키 생성
    
    키 구성:
    trace_id,detection_time
    
    예: "trace123,1234567890"
    """
    try:
        key = (
            str(data[fk.TRACE_ID]) + ","
            + str(data[fk.PED_DET_TIME])
        )
        return key
    except Exception as e:
        logger.error(f"Failed to Serialize Ped Data: [{e}]")
        
def _serialize_stats(data):
    """
    통계 데이터 고유 키 생성
    
    기본 키 구성:
    hr_type_cd,start_time,end_time
    
    선택적 추가 (있는 경우):
    - turn_type_cd: 회전 유형
    - car_type: 차종
    - lane_no: 차로 번호
    
    예: "1,1234560000,1234563600,11,PCAR,1"
    """
    try:
        key = (
            str(data[fk.HR_TYPE_CD]) + ","
            + str(data[fk.STAT_START_TIME]) + ","
            + str(data[fk.STAT_END_TIME])
        )
        # 선택적 필드 추가
        if data.get(fk.TURN_TYPE_CD):
            key += "," + str(data[fk.TURN_TYPE_CD])
        if data.get(fk.CAR_TYPE):
            key += "," + str(data[fk.CAR_TYPE])
        if data.get(fk.LANE_NO):
            key += "," + str(data[fk.LANE_NO])
        return key
    except Exception as e:
        logger.error(f"Failed to Serialize Stats Data: [{e}]")
        
def _serialize_queue(data):
    """
    대기행렬 데이터 고유 키 생성
    
    기본 키 구성:
    start_time,end_time
    
    선택적 추가 (있는 경우):
    - car_type: 차종
    - lane_no: 차로 번호
    
    예: "1234560000,1234563600,PCAR,1"
    """
    try:
        key = (
            str(data[fk.STAT_START_TIME]) + ","
            + str(data[fk.STAT_END_TIME])
        )
        # 선택적 필드 추가
        if data.get(fk.CAR_TYPE):
            key += "," + str(data[fk.CAR_TYPE])
        if data.get(fk.LANE_NO):
            key += "," + str(data[fk.LANE_NO])
        return key
    except Exception as e:
        logger.error(f"Failed to Serialize Queue Data: [{e}]")
        
def _serialize_abnormal(data):
    """
    돌발상황 데이터 고유 키 생성
    
    키 구성:
    trace_id,start_time
    
    예: "trace456,1234567890"
    """
    try:
        key = (
            str(data[fk.TRACE_ID]) + ","
            + str(data[fk.ABNORMAL_START_TIME])
        )
        return key
    except Exception as e:
        logger.error(f"Failed to Serialize Abnormal Data: [{e}]")
