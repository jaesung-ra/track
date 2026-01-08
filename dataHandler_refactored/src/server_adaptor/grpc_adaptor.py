"""
gRPC Adaptor Module
===================
gRPC를 통한 데이터 전송을 담당하는 어댑터

주요 기능:
1. Protocol Buffers 기반 데이터 전송
2. 카메라 정보 자동 조회 (Java 모드)
3. 데이터 타입별 gRPC 메서드 매핑
4. 요청 객체 자동 생성

지원 모드:
- Java 모드: Java 서버와 통신 (카메라 정보 자동 조회 포함)
- Sharp 모드: C# 서버와 통신 (VoltDB 연동)

데이터 타입별 gRPC 메서드:
- 차량: SaveSoitgrtmdtinfo_2K, SaveSoitgrtmdtinfo_4K, SaveSoitgrtmdtinfo
- 보행자: SaveSoitgcwdtinfo
- 통계: SaveSoitgaprdstats, SaveSoitgturntypestats 등
- 대기행렬: SaveSoitgaprdqueu, SaveSoitglanequeu
- 돌발상황: SaveSoitgunacevet_S, SaveSoitgunacevet_E

Protocol Buffers:
- 동적 import로 proto 파일 로드
- 서버 모드에 따라 다른 proto 패키지 사용
"""

import grpc
import threading
import importlib
import time
import os

from server_adaptor.server_adaptor import ServerAdaptor
from server_adaptor.volt_adaptor import VoltDBAdaptor

from data.sender import DataSender
from data.constants import DataType as dt
from data.constants import FieldKey as fk

from utils.logger import get_logger

from google.protobuf import empty_pb2
from google.protobuf import text_format


def _init_protos(mode):
    """
    서버 모드에 따라 Protocol Buffers 모듈 로드
    
    Args:
        mode: 서버 모드
            - "sharp": C# 서버용 proto
            - "java": Java 서버용 proto
            
    Returns:
        tuple: (pb, pb_grpc, eis, eis_grpc)
            - pb: EdgeData protobuf 정의
            - pb_grpc: EdgeData gRPC stub
            - eis: EdgeInfoService protobuf 정의 (Java만)
            - eis_grpc: EdgeInfoService gRPC stub (Java만)
            
    패키지 구조:
    - grpc_protos.sharp_protos_out.edge_data_pb2
    - grpc_protos.java_protos_out.edge_data_pb2
    - grpc_protos.java_protos_out.edge_info_service_pb2 (Java만)
    
    주의사항:
    - 동적 import 사용으로 런타임에 모듈 로드
    - sharp 모드는 EdgeInfoService 없음
    - proto 파일이 없으면 ImportError 발생
    """
    # 모드에 따라 패키지 선택
    if mode == "sharp":
        pkg = "sharp_protos_out"
    elif mode == "java":
        pkg = "java_protos_out"
        
    # EdgeData proto 로드
    pb      = importlib.import_module(f"grpc_protos.{pkg}.edge_data_pb2")
    pb_grpc = importlib.import_module(f"grpc_protos.{pkg}.edge_data_pb2_grpc")
    
    # Java 모드만 EdgeInfoService 로드
    if mode == "java":
        eis      = importlib.import_module(f"grpc_protos.{pkg}.edge_info_service_pb2")
        eis_grpc = importlib.import_module(f"grpc_protos.{pkg}.edge_info_service_pb2_grpc")
    else:
        eis = eis_grpc = None
        
    return pb, pb_grpc, eis, eis_grpc

class gRPCAdaptor(ServerAdaptor):
    """
    gRPC 데이터 전송 어댑터 클래스
    
    Attributes:
        logger: 로거 인스턴스
        config: gRPC 설정 딕셔너리
        stype: 서버 타입 ("grpc")
        host: gRPC 서버 호스트
        port: gRPC 서버 포트
        mode: 서버 모드 ("sharp" 또는 "java")
        name: 어댑터 이름
        pb: Protocol Buffers 정의
        pb_grpc: gRPC stub 클래스
        eis: EdgeInfoService proto (Java만)
        eis_grpc: EdgeInfoService stub (Java만)
        grpc_request_maker: 요청 객체 생성기
        grpc_function_map: 데이터 타입 → gRPC 메서드 매핑
        channel: gRPC 채널
        stub: EdgeDataService stub
        edgeinfo_stub: EdgeInfoService stub (Java만)
        
    모드별 차이:
    - Java: unique_key 포함, 카메라 정보 자동 조회
    - Sharp: crt_unix_tm 포함, VoltDB 연동
    
    주의사항:
    - 삽입 실패 시 최대 3회 재시도
    - Java 모드는 response.status_code 확인 필요
    - Sharp 모드는 예외 없으면 성공으로 간주
    """
    
    def __init__(self, config):
        """
        gRPCAdaptor 초기화
        
        Args:
            config: gRPC 설정 딕셔너리
                - ip: gRPC 서버 IP
                - port: gRPC 서버 포트
                - mode: 서버 모드 ("sharp" 또는 "java")
                - name: 어댑터 이름
                
        초기화 과정:
        1. Protocol Buffers 모듈 로드
        2. 요청 생성기 초기화 (모드별 옵션)
        3. 데이터 타입별 gRPC 메서드 매핑
        
        주의:
        - stub은 connect()에서 초기화됨
        - grpc_function_map은 stub 참조하므로 connect() 후 사용 가능
        """
        self.logger = get_logger("grpc")
        self.config = config
        
        self.stype = "grpc"
        self.host  = self.config["ip"]
        self.port  = self.config["port"]
        self.mode  = self.config["mode"]
        self.name  = self.config["name"]
        
        # Protocol Buffers 모듈 로드
        self.pb, self.pb_grpc, self.eis, self.eis_grpc = _init_protos(self.mode)
        
        # 모드별 요청 생성기 초기화
        if self.mode == "java":
            # Java 모드: unique_key 포함
            self.grpc_request_maker = gRPCRequestMaker(self.pb, include_uk=True)
        elif self.mode == "sharp":
            # Sharp 모드: crt_unix_tm 포함
            self.grpc_request_maker = gRPCRequestMaker(self.pb, include_crt_unix_tm=True)
    
        # 데이터 타입별 gRPC 메서드 매핑
        
    def connect(self):
        """
        gRPC 서버 연결 및 백그라운드 초기화
        
        처리:
        1. insecure 채널 생성 (암호화 없음)
        2. EdgeDataService stub 생성
        3. EdgeInfoService stub 생성 (Java만)
        4. 백그라운드 초기화 스레드 시작
        
        백그라운드 작업:
        - Java 모드: 카메라 정보 조회 및 설정
        - Sharp 모드: VoltDB 연결
        
        주의사항:
        - 비블로킹 방식 (즉시 리턴)
        - 실제 초기화는 백그라운드에서 진행
        - 채널은 암호화되지 않음 (insecure)
        """
        # insecure 채널 생성
        self.channel = grpc.insecure_channel(f"{self.host}:{self.port}")
        
        # EdgeDataService stub 생성
        self.stub = self.pb_grpc.EdgeDataServiceStub(self.channel)
        
        # Java 모드만 EdgeInfoService stub 생성
        if self.mode == "java":
            self.edgeinfo_stub = self.eis_grpc.EdgeInfoServiceStub(self.channel)
        
        self.grpc_function_map = {
            dt.VEHICLE_2K           : self.stub.SaveSoitgrtmdtinfo_2K,
            dt.VEHICLE_4K           : self.stub.SaveSoitgrtmdtinfo_4K,
            dt.VEHICLE_RAW_4K       : self.stub.SaveSoitgrtmdtinfo_4K,
            dt.MERGE                : self.stub.SaveSoitgrtmdtinfo,
            dt.PED_2K               : self.stub.SaveSoitgcwdtinfo,
            dt.STATS_APPROACH       : self.stub.SaveSoitgaprdstats,
            dt.STATS_TURN_TYPES     : self.stub.SaveSoitgturntypestats,
            dt.STATS_LANES          : self.stub.SaveSoitglanestats,
            dt.STATS_VEHICLE_TYPES  : self.stub.SaveSoitgkncrstats,
            dt.QUEUE_APPROACH       : self.stub.SaveSoitgaprdqueu,
            dt.QUEUE_LANES          : self.stub.SaveSoitglanequeu,
            dt.INCIDENT_START       : self.stub.SaveSoitgunacevet_S,
            dt.INCIDENT_END         : self.stub.SaveSoitgunacevet_E
        }

        # 백그라운드 초기화 스레드 시작
        self._t = threading.Thread(target=self._retry_loop, name="RPC CON-RET", daemon=True)
        self._t.start()
        
    def disconnect(self):
        """
        gRPC 연결 해제
        
        주의:
        - 현재 구현은 아무것도 안 함
        - 채널은 프로세스 종료 시 자동 정리
        """
        return
        
    def insert(self, data, dtype):
        """
        gRPC를 통한 데이터 전송
        
        Args:
            data: 전송할 데이터 딕셔너리
            dtype: 데이터 타입
            
        Returns:
            bool: 전송 성공 여부
            - True: 전송 성공
            - False: 3회 재시도 실패
            
        처리 흐름:
        1. 요청 객체 생성 (gRPCRequestMaker)
        2. 데이터 타입에 맞는 gRPC 메서드 호출
        3. 응답 확인 (모드별 상이)
        4. 실패 시 최대 3회 재시도
        
        모드별 성공 조건:
        - Sharp: 예외 없으면 성공
        - Java: response.status_code == 200
        
        주의사항:
        - 요청 생성 실패 시 즉시 False 반환
        - 각 재시도 사이 대기 없음
        - 마지막 요청 로그는 디버그 레벨
        """
        # 최대 3회 재시도
        for i in range(0, 3):
            # 1. 요청 객체 생성
            try:
                request = self.grpc_request_maker.make_request(data, dtype)
            except Exception as e:
                self.logger.error("gRPC Request Make Failed!", extra={"error": e})
                return False
            
            # 2. gRPC 메서드 호출
            try:
                response = self.grpc_function_map[dtype](request)
                
                # Sharp 모드: 예외 없으면 성공
                if self.mode == "sharp":
                    self.logger.info("gRPC Insert Success!", extra={
                        "datatype": dtype, 
                        "data": data[fk.UK_PLAIN], 
                        "uk": data[fk.UK]
                    })
                    return True
            except Exception as e:
                self.logger.error(f"gRPC Insert Failed {i+1} Times!", extra={
                    "datatype": dtype, 
                    "data": data[fk.UK_PLAIN], 
                    "uk": data[fk.UK], 
                    "server": f"{self.stype}|{self.host}:{self.port}", 
                    "error": e
                })
            
            # Java 모드: status_code 확인
            if self.mode == "java" and response.status_code == 200:
                self.logger.info("gRPC Insert Success!", extra={
                    "datatype": dtype, 
                    "data": data[fk.UK_PLAIN], 
                    "uk": data[fk.UK]
                })
                break
            else:
                self.logger.error(f"gRPC Insert Failed {i+1} Times!", extra={
                    "datatype": dtype, 
                    "data": data[fk.UK_PLAIN], 
                    "uk": data[fk.UK], 
                    "server": f"{self.stype}|{self.host}:{self.port}", 
                    "error": response.message
                })
                
        # 마지막 요청 로그 (디버그)
        self.logger.debug("gRPC Requested Full Data:\n%s", text_format.MessageToString(request, as_utf8=True))
        
        # Java 모드 최종 결과 확인
        if self.mode == "java" and response.status_code == 200:
            return True
        return False
    
    def _retry_loop(self):
        """
        백그라운드 초기화 루프
        
        모드별 동작:
        - Java: 카메라 정보 조회 및 설정
        - Sharp: VoltDB 연결
        
        Java 모드 처리:
        1. EdgeInfoService.getEdgeInfo() 호출
        2. spot_camera_id, lane_offset 추출
        3. DataSender에 설정
        4. 실패 시 10초마다 재시도
        
        Sharp 모드 처리:
        1. VoltDB 어댑터 생성 (고정 IP/Port)
        2. VoltDB 연결
        
        주의사항:
        - Java 모드만 재시도 루프
        - Sharp 모드는 한 번만 실행
        - VoltDB IP는 하드코딩됨 (192.168.1.3:7777)
        """
        if self.mode == "java":
            # Java 모드: 카메라 정보 조회 루프
            while DataSender.camera_id is None:
                try:
                    # EdgeInfoService.getEdgeInfo() 호출
                    resp = self.edgeinfo_stub.getEdgeInfo(empty_pb2.Empty(), timeout=2.0)
                    
                    # 카메라 정보 추출
                    cid = resp.spot_camera_id
                    lane_offset = resp.lane_offset
                    
                    # DataSender에 설정
                    DataSender.set_camera_id(cid)
                    DataSender.set_lane_offset(lane_offset)
                    
                    self.logger.info(f"gRPC CAM_ID Retrieved! CAM_ID: [{cid}]", extra={
                        "server": f"{self.stype}|{self.host}:{self.port}"
                    })
                except Exception as e:
                    self.logger.error("gRPC CAM_ID Fetch Failed!", extra={
                        "server": f"{self.stype}|{self.host}:{self.port}", 
                        "error": e
                    })
                    self.logger.error("Retrying Every 10 Seconds to Fetch CAM_ID")
                    time.sleep(10)
            return
            
        elif self.mode == "sharp":
            # Sharp 모드: VoltDB 연결
            # 주의: IP와 포트가 하드코딩됨
            volt = VoltDBAdaptor({"ip": "192.168.1.3", "port": 7777, "name": "L4"})
            volt.connect()

class gRPCRequestMaker():
    """
    gRPC 요청 객체 생성 클래스
    
    Attributes:
        pb: Protocol Buffers 정의 모듈
        include_crt_unix_tm: crt_unix_tm 필드 포함 여부 (Sharp 모드)
        include_uk: unique_key 필드 포함 여부 (Java 모드)
        request_function_map: 데이터 타입 → 빌드 함수 매핑
        
    기능:
    - 데이터 타입에 맞는 protobuf 메시지 생성
    - 모드별 선택적 필드 추가
    - 데이터 타입 변환 (문자열, 정수, 실수)
    
    사용 예시:
        maker = gRPCRequestMaker(pb, include_uk=True)
        request = maker.make_request(data, dt.VEHICLE_2K)
    """
    
    def __init__(self, pb, include_crt_unix_tm=None, include_uk=None):
        """
        gRPCRequestMaker 초기화
        
        Args:
            pb: Protocol Buffers 정의 모듈
            include_crt_unix_tm: crt_unix_tm 포함 여부 (Sharp 모드)
            include_uk: unique_key 포함 여부 (Java 모드)
            
        모드별 옵션:
        - Sharp: include_crt_unix_tm=True
        - Java: include_uk=True
        """
        self.pb = pb
        self.include_crt_unix_tm = include_crt_unix_tm
        self.include_uk = include_uk
            
        # 데이터 타입별 빌드 함수 매핑
        self.request_function_map = {
            dt.VEHICLE_2K           : self._build_soitgrtmdtinfo_2k,
            dt.VEHICLE_4K           : self._build_soitgrtmdtinfo_4k,
            dt.VEHICLE_RAW_4K       : self._build_soitgrtmdtinfo_raw_4k,
            dt.MERGE                : self._build_soitgrtmdtinfo,
            dt.PED_2K               : self._build_soitgcwdtinfo,
            dt.STATS_APPROACH       : self._build_soitgaprdstats,
            dt.STATS_TURN_TYPES     : self._build_soitgturntypestats,
            dt.STATS_LANES          : self._build_soitglanestats,
            dt.STATS_VEHICLE_TYPES  : self._build_soitgkncrstats,
            dt.QUEUE_APPROACH       : self._build_soitgaprdqueu,
            dt.QUEUE_LANES          : self._build_soitglanequeu,
            dt.INCIDENT_START       : self._build_soitgunacevet_S,
            dt.INCIDENT_END         : self._build_soitgunacevet_E,
        }
    
    def make_request(self, data, dtype):
        """
        데이터 타입에 맞는 protobuf 요청 객체 생성
        
        Args:
            data: 변환할 데이터 딕셔너리
            dtype: 데이터 타입
            
        Returns:
            protobuf 메시지 객체
            
        처리:
        - 데이터 타입에 맞는 빌드 함수 호출
        - 필드별 데이터 타입 변환
        - 모드별 선택적 필드 추가
        
        Raises:
            KeyError: 지원하지 않는 데이터 타입
        """
        return self.request_function_map[dtype](data)
    
    def _maybe_crt(self):
        """
        crt_unix_tm 필드 조건부 추가
        
        Returns:
            dict: {"crt_unix_tm": timestamp} 또는 {}
            
        용도:
        - Sharp 모드에서 생성 시각 추가
        - ** 언패킹으로 protobuf 생성자에 전달
        """
        return {"crt_unix_tm": int(time.time())} if self.include_crt_unix_tm else {}
    
    def _maybe_uk(self, d):
        """
        unique_key 필드 조건부 추가
        
        Args:
            d: 데이터 딕셔너리
            
        Returns:
            dict: {"unique_key": uk} 또는 {}
            
        용도:
        - Java 모드에서 고유 키 추가
        - ** 언패킹으로 protobuf 생성자에 전달
        """
        return {"unique_key": str(d[fk.UK])} if self.include_uk else {}
    
    # ========================================================================
    # 차량 검지 데이터 빌드 함수
    # ========================================================================
    
    def _build_soitgrtmdtinfo_2k(self, d):
        """
        2K 차량 검지 데이터 → protobuf 메시지 변환
        """
        return self.pb.Soitgrtmdtinfo_2K_Request(
            spot_camr_id        = str  (d[fk.SPOT_CAMR_ID]),
            kncr_cd             = str  (d[fk.CAR_TYPE]),
            lane_no             = int  (d[fk.LANE_NO]),
            turn_type_cd        = str  (d[fk.TURN_TYPE_CD]),
            turn_dttn_unix_tm   = int  (d[fk.TURN_TIME]),
            turn_dttn_sped      = float(d[fk.TURN_SPEED]),
            stln_pasg_unix_tm   = int  (d[fk.STOP_PASS_TIME]),
            stln_dttn_sped      = float(d[fk.STOP_PASS_SPEED]),
            vhcl_sect_sped      = float(d[fk.INTERVAL_SPEED]),
            frst_obsrvn_unix_tm = int  (d[fk.FIRST_DET_TIME]),
            vhcl_obsrvn_hr      = int  (d[fk.OBSERVE_TIME]),
            img_path_nm         = str  (d[fk.IMAGE_PATH_NAME]),
            vhcl_img_file_nm    = str  (d[fk.CAR_IMAGE_FILE_NAME]),
            vhcl_dttn_2k_id     = str  (d[fk.CAR_ID_2K]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    def _build_soitgrtmdtinfo_4k(self, d):
        """
        4K 차량 검지 데이터 → protobuf 메시지 변환
        """
        return self.pb.Soitgrtmdtinfo_4K_Request(
            spot_camr_id      = str(d[fk.SPOT_CAMR_ID]),
            kncr_cd           = str(d[fk.CAR_TYPE]),
            lane_no           = int(d[fk.LANE_NO]),
            stln_pasg_unix_tm = int(d[fk.STOP_PASS_TIME]),
            vhno_nm           = str(d[fk.PLATE_NUM]),
            vhno_dttn_yn      = str(d[fk.PLATE_YN]),
            img_path_nm       = str(d[fk.IMAGE_PATH_NAME]),
            vhcl_img_file_nm  = str(d[fk.CAR_IMAGE_FILE_NAME]),
            nopl_img_file_nm  = str(d[fk.PLATE_IMAGE_FILE_NAME]),
            vhcl_dttn_4k_id   = str(d[fk.CAR_ID_4K]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    def _build_soitgrtmdtinfo_raw_4k(self, d):
        """
        4K 원본 데이터 → protobuf 메시지 변환
        """
        return self._build_soitgrtmdtinfo_4k(d)

    def _build_soitgrtmdtinfo(self, d):
        """
        2K·4K 병합 데이터 → protobuf 메시지 변환
        """
        return self.pb.SoitgrtmdtinfoRequest(
            spot_camr_id        = str  (d[fk.SPOT_CAMR_ID]),
            kncr_cd             = str  (d[fk.CAR_TYPE]),
            lane_no             = str  (d[fk.LANE_NO]),
            turn_type_cd        = str  (d[fk.TURN_TYPE_CD]),
            turn_dttn_unix_tm   = int  (d[fk.TURN_TIME]),
            turn_dttn_sped      = float(d[fk.TURN_SPEED]),
            stln_pasg_unix_tm   = int  (d[fk.STOP_PASS_TIME]),
            stln_dttn_sped      = float(d[fk.STOP_PASS_SPEED]),
            vhcl_sect_sped      = float(d[fk.INTERVAL_SPEED]),
            frst_obsrvn_unix_tm = int  (d[fk.FIRST_DET_TIME]),
            vhcl_obsrvn_hr      = int  (d[fk.OBSERVE_TIME]),
            vhno_nm             = str  (d[fk.PLATE_NUM]),
            vhno_dttn_yn        = str  (d[fk.PLATE_YN]),
            img_path_nm         = str  (d[fk.IMAGE_PATH_NAME]),
            vhcl_img_file_nm    = str  (d[fk.CAR_IMAGE_FILE_NAME]),
            nopl_img_file_nm    = str  (d[fk.PLATE_IMAGE_FILE_NAME]),
            vhcl_dttn_id        = str  (d[fk.CAR_ID]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    # ========================================================================
    # 보행자 검지 데이터 빌드 함수
    # ========================================================================

    def _build_soitgcwdtinfo(self, d):
        """
        보행자 검지 데이터 → protobuf 메시지 변환
        """
        return self.pb.SoitgcwdtinfoRequest(
            spot_camr_id = str(d[fk.SPOT_CAMR_ID]),
            trce_id      = int(d[fk.TRACE_ID]),
            dttn_unix_tm = int(d[fk.PED_DET_TIME]),
            drct_se_cd   = str(d[fk.PED_DIR]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    # ========================================================================
    # 통계 데이터 빌드 함수
    # ========================================================================

    def _build_soitgaprdstats(self, d):
        """
        접근로 통계 데이터 → protobuf 메시지 변환
        """
        return self.pb.SoitgaprdstatsRequest(
            spot_camr_id        = str  (d[fk.SPOT_CAMR_ID]),
            hr_type_cd          = int  (d[fk.HR_TYPE_CD]),
            stats_bgng_unix_tm  = int  (d[fk.STAT_START_TIME]),
            stats_end_unix_tm   = int  (d[fk.STAT_END_TIME]),
            totl_trvl           = int  (d[fk.TOTAL_TRAVEL]),
            avg_stln_dttn_sped  = float(d[fk.AVG_STOP_PASS_SPEED]),
            avg_sect_sped       = float(d[fk.AVG_INTERVAL_SPEED]),
            avg_trfc_dnst       = int  (d[fk.AVG_DENSITY]),
            min_trfc_dnst       = int  (d[fk.MIN_DENSITY]),
            max_trfc_dnst       = int  (d[fk.MAX_DENSITY]),
            avg_lane_ocpn_rt    = float(d[fk.AVG_LANE_OCCUPY]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    def _build_soitgturntypestats(self, d):
        """
        회전 유형별 통계 데이터 → protobuf 메시지 변환
        """
        return self.pb.SoitgturntypestatsRequest(
            spot_camr_id        = str  (d[fk.SPOT_CAMR_ID]),
            hr_type_cd          = int  (d[fk.HR_TYPE_CD]),
            turn_type_cd        = int  (d[fk.TURN_TYPE_CD]),
            stats_bgng_unix_tm  = int  (d[fk.STAT_START_TIME]),
            stats_end_unix_tm   = int  (d[fk.STAT_END_TIME]),
            kncr1_trvl          = int  (d[fk.MBUS_TRAVEL]),
            kncr2_trvl          = int  (d[fk.LBUS_TRAVEL]),
            kncr3_trvl          = int  (d[fk.PCAR_TRAVEL]),
            kncr4_trvl          = int  (d[fk.MOTOR_TRAVEL]),
            kncr5_trvl          = int  (d[fk.MTRUCK_TRAVEL]),
            kncr6_trvl          = int  (d[fk.LTRUCK_TRAVEL]),
            avg_stln_dttn_sped  = float(d[fk.AVG_STOP_PASS_SPEED]),
            avg_sect_sped       = float(d[fk.AVG_INTERVAL_SPEED]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    def _build_soitglanestats(self, d):
        """
        차로별 통계 데이터 → protobuf 메시지 변환
        """
        return self.pb.SoitglanestatsRequest(
            spot_camr_id        = str  (d[fk.SPOT_CAMR_ID]),
            hr_type_cd          = int  (d[fk.HR_TYPE_CD]),
            lane_no             = int  (d[fk.LANE_NO]),
            stats_bgng_unix_tm  = int  (d[fk.STAT_START_TIME]),
            stats_end_unix_tm   = int  (d[fk.STAT_END_TIME]),
            totl_trvl           = int  (d[fk.TOTAL_TRAVEL]),
            avg_stln_dttn_sped  = float(d[fk.AVG_STOP_PASS_SPEED]),
            avg_sect_sped       = float(d[fk.AVG_INTERVAL_SPEED]),
            avg_trfc_dnst       = int  (d[fk.AVG_DENSITY]),
            min_trfc_dnst       = int  (d[fk.MIN_DENSITY]),
            max_trfc_dnst       = int  (d[fk.MAX_DENSITY]),
            ocpn_rt             = float(d[fk.OCCUPY]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    def _build_soitgkncrstats(self, d):
        """
        차종별 통계 데이터 → protobuf 메시지 변환
        """
        return self.pb.SoitgkncrstatsRequest(
            spot_camr_id        = str  (d[fk.SPOT_CAMR_ID]),
            hr_type_cd          = int  (d[fk.HR_TYPE_CD]),
            kncr_cd             = str  (d[fk.CAR_TYPE]),
            stats_bgng_unix_tm  = int  (d[fk.STAT_START_TIME]),
            stats_end_unix_tm   = int  (d[fk.STAT_END_TIME]),
            totl_trvl           = int  (d[fk.TOTAL_TRAVEL]),
            avg_stln_dttn_sped  = float(d[fk.AVG_STOP_PASS_SPEED]),
            avg_sect_sped       = float(d[fk.AVG_INTERVAL_SPEED]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    # ========================================================================
    # 대기행렬 데이터 빌드 함수
    # ========================================================================

    def _build_soitgaprdqueu(self, d):
        """
        접근로 대기행렬 데이터 → protobuf 메시지 변환
        """
        return self.pb.SoitgaprdqueuRequest(
            spot_camr_id        = str  (d[fk.SPOT_CAMR_ID]),
            stats_bgng_unix_tm  = int  (d[fk.STAT_START_TIME]),
            stats_end_unix_tm   = int  (d[fk.STAT_END_TIME]),
            rmnn_queu_lngt      = float(d[fk.REMAIN_QUEUE]),
            max_queu_lngt       = float(d[fk.MAX_QUEUE]),
            img_path_nm         = str  (d[fk.IMAGE_PATH_NAME]),
            img_file_nm         = str  (d[fk.IMAGE_FILE_NAME]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    def _build_soitglanequeu(self, d):
        """
        차로별 대기행렬 데이터 → protobuf 메시지 변환
        """
        return self.pb.SoitglanequeuRequest(
            spot_camr_id        = str  (d[fk.SPOT_CAMR_ID]),
            lane_no             = int  (d[fk.LANE_NO]),
            stats_bgng_unix_tm  = int  (d[fk.STAT_START_TIME]),
            stats_end_unix_tm   = int  (d[fk.STAT_END_TIME]),
            rmnn_queu_lngt      = float(d[fk.REMAIN_QUEUE]),
            max_queu_lngt       = float(d[fk.MAX_QUEUE]),
            img_path_nm         = str  (d[fk.IMAGE_PATH_NAME]),
            img_file_nm         = str  (d[fk.IMAGE_FILE_NAME]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )

    # ========================================================================
    # 돌발상황 데이터 빌드 함수
    # ========================================================================

    def _build_soitgunacevet_S(self, d):
        """
        돌발상황 시작 이벤트 → protobuf 메시지 변환
        """
        return self.pb.Soitgunacevet_S_Request(
            spot_camr_id  = str(d[fk.SPOT_CAMR_ID]),
            trce_id       = int(d[fk.TRACE_ID]),
            ocrn_unix_tm  = int(d[fk.ABNORMAL_START_TIME]),
            evet_type_cd  = str(d[fk.ABNORMAL_TYPE]),
            img_path_nm   = str(d[fk.IMAGE_PATH_NAME]),
            img_file_nm   = str(d[fk.IMAGE_FILE_NAME]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )
        
    def _build_soitgunacevet_E(self, d):
        """
        돌발상황 종료 이벤트 → protobuf 메시지 변환
        """
        return self.pb.Soitgunacevet_E_Request(
            spot_camr_id  = str(d[fk.SPOT_CAMR_ID]),
            trce_id       = int(d[fk.TRACE_ID]),
            ocrn_unix_tm  = int(d[fk.ABNORMAL_START_TIME]),
            end_unix_tm   = int(d[fk.ABNORMAL_END_TIME]),
            **self._maybe_uk(d),
            **self._maybe_crt(),
        )