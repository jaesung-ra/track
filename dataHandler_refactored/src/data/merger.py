"""
Data Merger Module
==================
2K와 4K 해상도 차량 검지 데이터를 병합하는 모듈

주요 기능:
1. 2K와 4K 데이터를 차로/차종별로 분류하여 관리
2. 정지선 통과 시각 기준으로 데이터 매칭
3. 매칭 성공 시 병합 데이터 생성 및 전송
4. 오래된 데이터 자동 정리

병합 조건:
- 같은 차로 (lane_no)
- 같은 차종 (car_type)
- 정지선 통과 시각 차이 1초 이내

특수 처리:
- 루원 사이트: 차로 번호 변환 후 추가 데이터 전송
"""

import queue
import bisect
import threading
import time
import copy

from collections import defaultdict

from data.router import build_luwon
from data.constants import DataType as dt
from data.constants import FieldKey as fk

from utils.config_parser import ConfigParser
from utils.logger import get_logger

class Merger:
    """
    2K와 4K 데이터 병합 클래스
    
    Attributes:
        logger: 로거 인스턴스
        to_server_q: 서버 전송 큐
        to_merge_q_2k: 2K 데이터 병합 대기 큐
        to_merge_q_4k: 4K 데이터 병합 대기 큐
        compare_2k: 2K 데이터 버킷 (key: (lane_no, car_type))
        compare_4k: 4K 데이터 버킷 (key: (lane_no, car_type))
        flag: 스레드 종료 플래그
        luwon_cfg: 루원 사이트 특수 설정
        
    데이터 구조:
        compare_2k/4k = {
            (lane_no, car_type): [
                {..., stop_pass_time: 1234567890},
                {..., stop_pass_time: 1234567891},
                ...
            ]
        }
        - 각 버킷은 시각 순으로 정렬된 리스트
        
    주의사항:
        - 60초 이상 오래된 데이터는 자동 삭제
        - 유턴(turn_type_cd=41) 데이터는 병합 대상에서 제외
        - 매칭된 데이터는 버킷에서 즉시 제거
    """
    
    def __init__ (self, to_server_q, to_merge_q_2k, to_merge_q_4k):
        """
        Merger 초기화
        
        Args:
            to_server_q: 병합 완료 데이터 전송 큐
            to_merge_q_2k: 2K 데이터 수신 큐
            to_merge_q_4k: 4K 데이터 수신 큐
        """
        self.logger = get_logger("merger")
        self.to_server_q = to_server_q
        self.to_merge_q_2k = to_merge_q_2k
        self.to_merge_q_4k = to_merge_q_4k

        # 차로/차종별 데이터 버킷 (시각 순 정렬 유지)
        self.compare_2k = defaultdict(list)
        self.compare_4k = defaultdict(list)
        
        self.flag = threading.Event()
        
        # 루원 사이트 특수 처리 설정
        self.luwon_cfg = ConfigParser.get()["special_site"]["enabled"]
            
    def stop(self):
        """스레드 종료 플래그 설정"""
        self.flag.set()
    
    def main_loop(self):
        """
        메인 처리 루프
        
        처리 흐름:
        1. 2K 큐에서 데이터 수신 (블로킹)
        2. 해당 데이터를 적절한 버킷에 삽입 (정렬 유지)
        3. 2K 큐의 모든 대기 데이터 처리 (논블로킹)
        4. 4K 큐의 모든 대기 데이터 처리 (논블로킹)
        5. 2K와 4K 데이터 비교 및 병합
        
        주의:
        - 2K 큐는 블로킹으로 대기 (메인 트리거)
        - 4K 큐는 논블로킹으로 모두 소진
        - 병합은 매 루프마다 수행
        """
        while not self.flag.is_set():
            try:
                # 2K 큐에서 데이터 수신 (블로킹)
                record = self.to_merge_q_2k.get()
                
                # (차로, 차종) 키로 버킷 선택
                key = (record[fk.LANE_NO], record[fk.CAR_TYPE])
                bucket = self.compare_2k[key]
                
                # 시각 순으로 정렬된 위치에 삽입
                self._insert_sorted(bucket, record)

                # 대기 중인 모든 2K 데이터 처리
                self._drain_queue(self.to_merge_q_2k, self.compare_2k)
                
                # 대기 중인 모든 4K 데이터 처리
                self._drain_queue(self.to_merge_q_4k, self.compare_4k)
                
                # 2K와 4K 데이터 매칭 및 병합
                self._compare()
            except Exception as e:
                self.logger.critical("Something Went Wrong in Merger!", extra={"error":e})
            
    def _insert_sorted(self, bucket, record):
        """
        버킷에 데이터를 시각 순으로 정렬된 위치에 삽입
        
        Args:
            bucket: 데이터를 삽입할 리스트
            record: 삽입할 데이터
            
        동작:
        - bisect_left를 사용하여 O(log n) 시간에 삽입 위치 탐색
        - 정지선 통과 시각(STOP_PASS_TIME) 기준 정렬
        
        주의:
        - bucket은 항상 정렬 상태 유지
        - 같은 시각의 데이터는 앞쪽에 삽입
        """
        timestamp = int(record[fk.STOP_PASS_TIME])
        timelist = [int(x.get(fk.STOP_PASS_TIME, 0)) for x in bucket]
        index = bisect.bisect_left(timelist, timestamp)
        bucket.insert(index, record)
        
    def _drain_queue(self, data_queue_merge, destination_map):
        """
        큐의 모든 데이터를 꺼내서 버킷에 저장
        
        Args:
            data_queue_merge: 데이터를 꺼낼 큐
            destination_map: 데이터를 저장할 버킷 맵
            
        처리:
        1. 큐가 빌 때까지 논블로킹으로 데이터 수신
        2. 유턴(turn_type_cd=41) 데이터는 제외
        3. (차로, 차종) 키로 버킷 선택
        4. 시각 순으로 정렬된 위치에 삽입
        
        주의:
        - get_nowait() 사용으로 큐가 비면 즉시 종료
        - 유턴 데이터는 병합 대상이 아니므로 버림
        """
        while True:
            try:
                record = data_queue_merge.get_nowait()
            except queue.Empty:
                break
                
            # 유턴(41) 데이터는 병합 대상에서 제외
            if record.get(fk.TURN_TYPE_CD) == "41":
                continue
            
            # (차로, 차종) 키로 버킷 선택
            key = (record[fk.LANE_NO], record[fk.CAR_TYPE])
            bucket = destination_map[key]
            
            # 시각 순으로 삽입
            self._insert_sorted(bucket, record)
                
    def _remove_old_data(self, bucket_map):
        """
        60초 이상 오래된 데이터 삭제
        
        Args:
            bucket_map: 정리할 버킷 맵
            
        처리:
        1. 현재 시각 기준 60초 이전 cutoff 시각 계산
        2. 각 버킷에서 cutoff 이전 데이터 삭제
        3. 빈 버킷은 맵에서 제거
        
        최적화:
        - bisect_left로 삭제 범위를 O(log n)에 탐색
        - 슬라이싱으로 일괄 삭제
        """
        current_time = int(time.time())
        cutoff = current_time - 60  # 60초 이전 데이터 삭제
        
        for key in list(bucket_map.keys()):
            bucket = bucket_map[key]
            
            # 빈 버킷 제거
            if not bucket:
                bucket_map.pop(key, None)
                
            # cutoff 이전 데이터 인덱스 찾기
            times = [int(x.get(fk.STOP_PASS_TIME, 0)) for x in bucket]
            k = bisect.bisect_left(times, cutoff)
            
            # cutoff 이전 데이터 일괄 삭제
            if k > 0 :
                del bucket[:k]
                
            # 삭제 후 빈 버킷 제거
            if not bucket:
                bucket_map.pop(key, None)
                
    def _compare(self):
        """
        2K와 4K 데이터 비교 및 병합
        
        처리 흐름:
        1. 오래된 데이터 정리
        2. 공통 키(차로, 차종) 추출
        3. 각 키별로 2K와 4K 데이터 투 포인터로 비교
        4. 시각 차이 1초 이내면 병합
        5. 병합 성공 데이터는 버킷에서 제거
        
        병합 조건:
        - |t2 - t4| <= 1 (정지선 통과 시각 차이 1초 이내)
        - 같은 차로, 같은 차종
        
        병합 데이터 구성:
        - 2K 데이터 기본 복사
        - DATA_TYPE을 MERGE로 변경
        - 4K의 번호판 정보 추가
        
        특수 처리 (루원 사이트):
        - 회전 방향별 카메라 ID와 차로 번호 변환
        - 변환된 데이터 추가 전송
        
        투 포인터 알고리즘:
        - i: 2K 데이터 인덱스
        - j: 4K 데이터 인덱스
        - 시각 비교하며 양쪽 포인터 이동
        """
        # 버킷이 비어있으면 스킵
        if not self.compare_2k or not self.compare_4k:
            return
        
        # 오래된 데이터 삭제
        self._remove_old_data(self.compare_2k)
        self._remove_old_data(self.compare_4k)
        
        # 2K와 4K에 모두 존재하는 키 추출
        common_keys = set(self.compare_2k.keys()) & set(self.compare_4k.keys())
        
        for key in tuple(common_keys):
            data_2k = self.compare_2k.get(key)
            data_4k = self.compare_4k.get(key)
            
            if not data_2k or not data_4k:
                continue
            
            # 투 포인터 초기화
            i = j = 0 
            matched_2k = []  # 매칭된 2K 데이터 인덱스
            matched_4k = []  # 매칭된 4K 데이터 인덱스
            
            # 투 포인터 알고리즘으로 매칭
            while i < len(data_2k) and j < len(data_4k):
                item_2k = data_2k[i]
                item_4k = data_4k[j]
                
                t2 = int(item_2k[fk.STOP_PASS_TIME])
                t4 = int(item_4k[fk.STOP_PASS_TIME])
                
                # 시각 차이 1초 이내: 매칭 성공
                if abs(t2 - t4) <= 1:
                    # 2K 데이터 기반으로 병합 데이터 생성
                    merged = copy.deepcopy(item_2k)
                    merged[fk.DATA_TYPE] = dt.MERGE
                    merged[fk.CAR_ID] = item_2k[fk.CAR_ID_2K]
                    
                    # 4K의 번호판 정보 추가
                    merged[fk.PLATE_IMAGE_FILE_NAME] = item_4k[fk.PLATE_IMAGE_FILE_NAME]
                    merged[fk.PLATE_NUM] = item_4k[fk.PLATE_NUM]
                    
                    # 루원 사이트 특수 처리
                    if self.luwon_cfg:
                        # 차로 번호 변환 및 추가 데이터 생성
                        res = build_luwon(merged)
                        merged = res.to_server[0]
                        
                        # 4K 데이터도 변환된 정보로 업데이트하여 전송
                        item_4k[fk.SPOT_CAMR_ID] = merged[fk.SPOT_CAMR_ID]
                        item_4k[fk.LANE_NO] = merged[fk.LANE_NO]
                        self.to_server_q.put(item_4k)
                        
                    self.logger.info(f"Merge Success! 2K data: [{item_2k[fk.UK_PLAIN]}], 4K data: [{item_4k[fk.UK_PLAIN]}]")
                    self.logger.debug(f"2k data: {dict(item_2k)}")
                    self.logger.debug(f"4k data: {dict(item_4k)}")
                    self.logger.debug(f"merged data: {dict(merged)}")
                    
                    # 병합 데이터 전송
                    self.to_server_q.put(merged)
                        
                    # 매칭된 데이터 인덱스 기록
                    matched_2k.append(i)
                    matched_4k.append(j)
                    
                    # 양쪽 포인터 이동
                    i += 1
                    j += 1
                    
                # 2K가 더 이른 시각: 2K 포인터만 이동
                elif t2 < t4 - 1:
                    i += 1
                    
                # 4K가 더 이른 시각: 4K 포인터만 이동
                else:
                    j += 1
            
            # 매칭된 데이터를 버킷에서 제거 (역순으로 제거)
            for index in reversed(matched_2k):
                del data_2k[index]
            for index in reversed(matched_4k):
                del data_4k[index]
            
            # 빈 버킷 제거
            if not data_2k:
                self.compare_2k.pop(key, None)
            if not data_4k:
                self.compare_4k.pop(key, None)