"""
Server Adaptor Base Class
==========================
모든 서버 어댑터의 기본 인터페이스를 정의하는 추상 클래스

목적:
- 다양한 서버 타입(Redis, VoltDB, SQLite, gRPC)에 대한 통일된 인터페이스 제공
- 서버 연결, 데이터 삽입, 조회 등의 공통 메서드 정의

구현체:
- RedisAdaptor: Redis Pub/Sub 통신
- VoltDBAdaptor: VoltDB 데이터베이스 연결
- SqliteAdaptor: SQLite 로컬 저장소
- GrpcAdaptor: gRPC 통신

사용 패턴:
모든 어댑터는 이 클래스를 상속받아 추상 메서드를 구현해야 함
"""

from abc import ABC, abstractmethod


class ServerAdaptor:
    """
    서버 어댑터 추상 기본 클래스
    
    추상 메서드:
    - __init__(): 어댑터 초기화
    - connect(): 서버 연결
    - disconnect(): 서버 연결 해제
    - insert(): 데이터 삽입
    - get(): 데이터 조회
    
    주의사항:
    - 모든 추상 메서드는 반드시 구현해야 함
    - 구현하지 않으면 인스턴스 생성 시 TypeError 발생
    """
    
    @abstractmethod
    def __init__(self):
        """
        어댑터 초기화
        
        구현 필수 항목:
        - 서버 연결 정보 설정 (host, port 등)
        - 로거 초기화
        - 클라이언트 객체 초기화
        
        Raises:
            NotImplementedError: 구현되지 않은 경우
        """
        raise NotImplementedError

    @abstractmethod
    def connect(self):
        """
        서버 연결 수립
        
        구현 필수 항목:
        - 서버 연결 시도
        - 연결 실패 시 에러 처리
        - 연결 성공 로깅
        
        Raises:
            NotImplementedError: 구현되지 않은 경우
        """
        raise NotImplementedError

    @abstractmethod
    def disconnect(self):
        """
        서버 연결 해제
        
        구현 필수 항목:
        - 리소스 정리
        - 연결 종료
        - 클라이언트 객체 해제
        
        Raises:
            NotImplementedError: 구현되지 않은 경우
        """
        raise NotImplementedError

    @abstractmethod
    def insert(self):
        """
        데이터 삽입
        
        구현 필수 항목:
        - 데이터 전송/저장
        - 성공/실패 여부 반환
        - 에러 처리 및 로깅
        
        Returns:
            bool: 삽입 성공 여부
            
        Raises:
            NotImplementedError: 구현되지 않은 경우
        """
        raise NotImplementedError
    
    @abstractmethod
    def get(self):
        """
        데이터 조회
        
        구현 필수 항목:
        - 데이터 수신/조회
        - 데이터 반환 또는 None
        - 에러 처리 및 로깅
        
        Returns:
            데이터 또는 None
            
        Raises:
            NotImplementedError: 구현되지 않은 경우
        """
        raise NotImplementedError