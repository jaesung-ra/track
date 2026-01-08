"""
Redis Adaptor Module
====================
Redis Pub/Sub을 통한 데이터 송수신을 담당하는 어댑터

주요 기능:
1. Redis 채널 구독 및 메시지 수신 (Receiver 모드)
2. Redis 채널 발행 및 메시지 전송 (Sender 모드)
3. 연결 상태 모니터링 및 자동 재연결
4. 데이터 타입별 메시지 포맷팅

동작 모드:
- Receiver: label이 설정된 경우 (데이터 수신용)
- Sender: name이 설정된 경우 (데이터 전송용)

특수 처리:
- 존재 감지 데이터는 상태 값만 전송 (JSON이 아닌 정수)
"""

import redis
import json

from utils.logger import get_logger

from server_adaptor.server_adaptor import ServerAdaptor

from data.constants import FieldKey as fk
from data.constants import DataType as dt


class RedisAdaptor(ServerAdaptor):
    """
    Redis Pub/Sub 어댑터 클래스
    
    Attributes:
        stype: 서버 타입 ("redis")
        host: Redis 서버 호스트
        port: Redis 서버 포트
        channel: 구독/발행할 채널명
        send_to: 데이터 전송 대상 서버 목록 (Receiver 모드)
        label: 데이터 타입 라벨 (Receiver 모드)
        name: 어댑터 이름 (Sender 모드)
        logger: 로거 인스턴스
        client: Redis 클라이언트 객체
        redis_subscribe_client: Redis Pub/Sub 객체 (Receiver 모드)
        
    """
    
    def __init__(self, config):
        """
        RedisAdaptor 초기화
        
        Args:
            config: Redis 설정 딕셔너리
                - ip: Redis 서버 IP
                - port: Redis 서버 포트
                - channel: 채널명
                - label: 데이터 타입 라벨 (옵션, Receiver 모드)
                - name: 어댑터 이름 (옵션, Sender 모드)
                - send_to: 전송 대상 서버 (옵션, Receiver 모드)
                
        동작 모드 결정:
        - label이 있으면 Receiver 모드 (logger: redis_rcv)
        - name이 있으면 Sender 모드 (logger: redis_server)
        """
        self.stype   = "redis"
        self.host    = config["ip"]
        self.port    = config["port"]
        self.channel = config["channel"]
        self.send_to = config.get("send_to", None)
        
        # 동작 모드에 따라 로거 설정
        if config.get("label") is not None:
            # Receiver 모드: 데이터 수신용
            self.label = config["label"]
            self.logger = get_logger("redis_rcv")
        else:
            # Sender 모드: 데이터 전송용
            self.name = config["name"]
            self.logger = get_logger("redis_server")
            
        self.client = None
        
    def connect(self):
        """
        Redis 서버 연결 및 채널 구독
        
        처리:
        1. Redis 클라이언트 생성
        2. decode_responses=True로 자동 문자열 디코딩
        3. Pub/Sub 객체 생성 및 채널 구독
        4. 연결 성공 로깅
        
        주의사항:
        - decode_responses=True: 바이트를 자동으로 문자열로 변환
        - 연결 실패 시 에러 로그만 남기고 계속 진행
        """
        try:
            # Redis 클라이언트 생성
            self.client = redis.Redis(
                host=self.host, 
                port=self.port,
                decode_responses=True  # 바이트를 문자열로 자동 디코딩
            )
            
            # Pub/Sub 객체 생성 및 채널 구독
            self.redis_subscribe_client = self.client.pubsub()
            self.redis_subscribe_client.subscribe(self.channel)
            
            self.logger.info("Connection Success!", extra={
                "server": f"{self.stype}|{self.host}:{self.port}|{self.channel}"
            })
        except Exception as e:
            self.logger.error("Connection Failed!", extra={"error": e})
            return
        
    def disconnect(self):
        """
        Redis 연결 해제
        
        처리:
        - 클라이언트 연결 종료
        - 클라이언트 객체 None으로 설정
        """
        if self.client != None:
            self.client.close()
            self.client = None

    def insert(self, data, dtype):
        """
        Redis 채널에 메시지 발행 (Sender 모드)
        
        Args:
            data: 발행할 데이터 딕셔너리
            dtype: 데이터 타입 (사용 안 함, 호환성 유지용)
            
        Returns:
            bool: 발행 성공 여부
            - True: 발행 성공
            - False: 발행 실패
            
        처리 흐름:
        1. 연결 상태 확인 및 재연결
        2. 데이터 타입에 따라 메시지 포맷팅
        3. Redis 채널에 발행
        4. 성공/실패 로깅
        
        메시지 포맷:
        - 존재 감지 데이터: 상태 값만 전송 (예: "0" 또는 "1")
        - 기타 데이터: JSON 문자열로 직렬화
        
        주의사항:
        - 연결이 끊어진 경우 자동 재연결 시도
        - 발행 실패 시 재시도 없이 False 반환
        """
        # 연결 상태 확인 및 재연결
        self._ensure_connection()
        
        try:
            # 메시지 발행
            self._publish(data)
            
            self.logger.info("Redis Insert Success!", extra={
                "datatype": data[fk.DATA_TYPE], 
                "data": data[fk.UK_PLAIN], 
                "server": f"{self.stype}|{self.host}:{self.port}|{self.channel}"
            })
            inserted = True
        except Exception as e:
            self.logger.error("Redis Insert Failed!", extra={
                "datatype": data[fk.DATA_TYPE], 
                "data": data[fk.UK_PLAIN], 
                "error": e
            })
            inserted = False
            
        self.logger.debug(f"Redis Requested Full Data:\n{json.dumps(data)}")
        return inserted
    
    def get(self):
        """
        Redis 채널에서 메시지 수신 (Receiver 모드)
        
        Returns:
            str or None: 수신된 메시지 데이터
            - 메시지 수신 시: 메시지 내용 (문자열)
            - 에러 발생 시: None
            
        처리 흐름:
        1. Pub/Sub listen() 메서드로 메시지 대기 (블로킹)
        2. 메시지 타입이 "message"인 경우만 반환
        3. 에러 발생 시 재연결 시도
        
        메시지 타입:
        - "subscribe": 구독 확인 메시지 (무시)
        - "message": 실제 데이터 메시지 (반환)
        - "unsubscribe": 구독 해제 메시지 (무시)
        
        주의사항:
        - 블로킹 방식으로 동작 (메시지가 올 때까지 대기)
        - 에러 발생 시 재연결 후 None 반환
        - 연결 끊김 시 자동 재연결 시도
        """
        try:
            # 메시지 수신 대기 (블로킹)
            for message in self.redis_subscribe_client.listen():
                # 실제 데이터 메시지만 반환
                if message["type"] == "message":
                    return message["data"]
        except Exception as e:
            self.logger.error("Something Went Wrong!", extra={"error": e})
            # 연결 상태 확인 및 재연결
            self._ensure_connection()
            return
        
    def _publish(self, data):
        """
        데이터 타입에 따라 메시지 포맷팅 및 발행
        
        Args:
            data: 발행할 데이터 딕셔너리
            
        처리:
        - 존재 감지 데이터: 상태 값만 문자열로 전송
        - 기타 데이터: JSON 직렬화하여 전송
        
        존재 감지 데이터 타입:
        - PRESENCE_VH: 차량 존재 감지
        - PRESENCE_WAIT: 대기 차량 존재 감지
        - PRESENCE_CROSS: 횡단보도 존재 감지
        
        주의사항:
        - 존재 감지는 간단한 0/1 값만 필요하므로 JSON 불필요
        - publish()는 실패해도 예외를 던지지 않을 수 있음
        """
        if data[fk.DATA_TYPE] in {dt.PRESENCE_VH, dt.PRESENCE_WAIT, dt.PRESENCE_CROSS}:
            # 존재 감지: 상태 값만 전송 (0 또는 1)
            self.client.publish(self.channel, str(data[fk.PRESENCE_STATE]))
        else:
            # 기타 데이터: JSON 직렬화
            self.client.publish(self.channel, json.dumps(data))
    
    def _ensure_connection(self):
        """
        Redis 연결 상태 확인 및 재연결
        
        처리:
        1. _server_ok()로 연결 상태 확인
        2. 연결이 끊어진 경우 재연결 시도
        
        주의사항:
        - 재연결 시 구독 정보도 함께 복구됨
        - 재연결 중 메시지 누락 가능성 있음
        """
        if not self._server_ok(self.client) or self.client is None:
            self.logger.error("Connection Lost! Reconnecting..")
            self.connect()
            
    def _server_ok(self, client):
        """
        Redis 서버 연결 상태 확인
        
        Args:
            client: Redis 클라이언트 객체
            
        Returns:
            bool: 연결 정상 여부
            - True: 연결 정상
            - False: 연결 끊김
            
        확인 방법:
        - time(): 서버 시간 조회 (연결 확인용)
        - client_id(): 클라이언트 ID 조회 (연결 확인용)
        
        주의사항:
        - 두 명령어 모두 가벼운 명령으로 빠르게 확인 가능
        - 예외 발생 시 연결 끊김으로 판단
        """
        try:
            client.time()       # 서버 시간 조회
            client.client_id()  # 클라이언트 ID 조회
            return True
        except Exception:
            return False