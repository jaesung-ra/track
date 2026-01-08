"""
Configuration Parser Module
============================
JSON 설정 파일을 로드하고 전역적으로 접근 가능하게 하는 싱글톤 클래스

주요 기능:
- config.json 파일 로드
- 싱글톤 패턴으로 전역 접근
- 스레드 안전 초기화

사용 예시:
    >>> config = ConfigParser.get()
    >>> host = config["redis"]["ip"]
    >>> port = config["redis"]["port"]

싱글톤 보장:
- 여러 모듈에서 동시 접근 가능
- 한 번만 파일 로드
- 스레드 안전 초기화

주의사항:
- config.json 파일이 없으면 RuntimeError 발생
- JSON 형식 오류 시 프로그램 종료
- 설정 변경 시 재시작 필요
"""

import json
from pathlib import Path
from typing import Any, Dict
import threading


class _Singleton(type):
    """
    싱글톤 메타클래스
    
    기능:
    - 클래스 인스턴스를 하나만 생성
    - 이후 호출 시 기존 인스턴스 반환
    
    구현:
    - __call__ 메서드 오버라이드
    - 더블 체크 락킹 패턴 사용
    
    Class Attributes:
        _instance: 싱글톤 인스턴스
        _lock: 스레드 동기화 락
        
    스레드 안전성:
    - 더블 체크 락킹으로 성능과 안전성 균형
    - 첫 번째 체크: 락 없이 빠른 확인
    - 두 번째 체크: 락 안에서 안전한 확인
    
    주의사항:
    - 모든 싱글톤 클래스에 사용 가능
    - 서브클래스마다 별도 인스턴스 생성
    """
    
    _instance = None
    _lock = threading.Lock()

    def __call__(cls, *args, **kwargs):
        """
        인스턴스 생성 또는 기존 인스턴스 반환
        
        Args:
            *args: 생성자 위치 인자
            **kwargs: 생성자 키워드 인자
            
        Returns:
            싱글톤 인스턴스
            
        처리 흐름:
        1. 인스턴스 존재 확인 (락 없이)
        2. 없으면 락 획득
        3. 다시 확인 (다른 스레드가 생성했을 수 있음)
        4. 여전히 없으면 생성
        5. 기존 또는 새 인스턴스 반환
        
        더블 체크 락킹:
        - 첫 체크: 대부분의 경우 빠른 반환
        - 락: 동시 생성 방지
        - 두번째 체크: 중복 생성 방지
        """
        # 첫 번째 체크 (락 없이)
        if cls._instance is None:
            # 락 획득
            with cls._lock:
                # 두 번째 체크 (락 안에서)
                if cls._instance is None:
                    # 인스턴스 생성
                    cls._instance = super().__call__(*args, **kwargs)
                    
        return cls._instance


class ConfigParser(metaclass=_Singleton):
    """
    설정 파일 파서 싱글톤 클래스
    
    Attributes:
        _initialized: 초기화 완료 여부
        _path: 설정 파일 경로
        _cfg: 로드된 설정 딕셔너리
        
    사용 방법:
        # 설정 가져오기
        config = ConfigParser.get()
        
        # 값 접근
        redis_ip = config["redis"]["ip"]
        log_level = config["log"]["defaults"]["log_level"]
        
    파일 구조:
        config.json:
        {
            "redis": {"ip": "...", "port": ...},
            "log": {...},
            "OCR": {...},
            ...
        }
        
    주의사항:
    - 첫 호출 시 파일 로드
    - 이후 호출은 캐시된 데이터 반환
    - 파일 변경 시 프로그램 재시작 필요
    - JSON 형식 오류 시 RuntimeError 발생
    """
    
    def __init__(self, config_path="./config.json"):
        """
        ConfigParser 초기화
        
        Args:
            config_path: 설정 파일 경로 (기본: "./config.json")
            
        처리:
        1. 이미 초기화되었으면 스킵
        2. 초기화 플래그 설정
        3. 파일 경로 저장
        4. 설정 파일 로드
        
        싱글톤 중복 초기화 방지:
        - _initialized 플래그로 확인
        - 첫 호출 후 다시 초기화 안 함
        
        주의사항:
        - config_path는 첫 호출에만 유효
        - 이후 호출 시 무시됨 (싱글톤)
        - 파일 없으면 RuntimeError 발생
        """
        # 이미 초기화된 경우 스킵
        if getattr(self, "_initialized", False):
            return
            
        # 초기화 플래그 설정
        self._initialized = True

        # 파일 경로 저장
        self._path = config_path
        
        # 설정 파일 로드
        self._cfg = self._load()

    def _load(self):
        """
        JSON 설정 파일 로드
        
        Returns:
            dict: 파싱된 설정 딕셔너리
            
        처리:
        1. 파일 열기
        2. JSON 파싱
        3. 딕셔너리 반환
        
        에러 처리:
        - 파일 없음: RuntimeError
        - JSON 오류: RuntimeError
        - 기타 예외: RuntimeError
        
        Raises:
            RuntimeError: 파일 로드 실패 시
            
        주의사항:
        - 파일 인코딩은 시스템 기본값 사용
        - 큰 파일도 한번에 로드 (메모리 주의)
        - 에러 메시지에 원인 포함
        """
        try:
            # 파일 열기 및 JSON 파싱
            with open(self._path) as f:
                return json.load(f)
        except Exception as e:
            # 에러 발생 시 RuntimeError로 래핑
            raise RuntimeError(f"Config load fail from {self._path}: {e}") from e
    
    @classmethod
    def get(cls):
        """
        설정 딕셔너리 가져오기
        
        Returns:
            dict: 로드된 설정 딕셔너리
            
        사용 예시:
            >>> config = ConfigParser.get()
            >>> redis_config = config["redis"]
            >>> log_path = config["log"]["path"]
            
        처리:
        1. 싱글톤 인스턴스 생성/가져오기
        2. _cfg 반환
        
        첫 호출:
        - __init__() 호출되어 파일 로드
        - _cfg에 설정 저장
        
        이후 호출:
        - 기존 인스턴스의 _cfg 반환
        - 파일 재로드 안 함
        
        주의사항:
        - 클래스 메서드로 인스턴스 없이 호출 가능
        - 설정 변경 시 프로그램 재시작 필요
        - 스레드 안전 (싱글톤 보장)
        """
        return cls()._cfg