"""
Logging System Module
=====================
계층적 로거 시스템 및 커스텀 필터 구현

주요 기능:
1. 계층적 로거 구조 (datahandler.receiver.redis 등)
2. 자동 로그 로테이션 (시간 기반)
3. 구조화된 로그 포맷 (컬럼 형식)
4. 예외 추적 정보 자동 추가
5. 스레드 안전 싱글톤 관리

로거 계층:
- datahandler (루트)
  - datahandler.receiver
    - datahandler.receiver.redis
  - datahandler.sender
    - datahandler.sender.grpc
    - datahandler.sender.volt
    - ...
  - datahandler.merger
  - datahandler.lp_detector

로그 포맷:
- 시간, 레벨, 스레드, 모듈:함수, 메시지
- 구조화된 extra 필드 (datatype, data, error 등)

주요 클래스:
- LoggingManager: 로거 생성 및 관리
- PadFilter: 필드 패딩 (정렬)
- ColumnsFilter: 구조화된 컬럼 출력
- ErrorFilter: 예외 정보 추출
"""

import os
import threading
import logging
import traceback

from logging import handlers

from utils.config_parser import ConfigParser


def _parse_bool(x):
    """
    다양한 타입을 boolean으로 변환
    """
    if isinstance(x, bool):
        return x
    if isinstance(x, str):
        return x.strip().lower() in ("1", "true", "yes", "y", "on")
    return bool(x)


def _parse_log_level(log_level, default=logging.INFO):
    """
    로그 레벨 문자열을 logging 상수로 변환
    
    Args:
        log_level: 로그 레벨 문자열 ("DEBUG", "INFO" 등)
        default: 기본값 (파싱 실패 시)
        
    Returns:
        int: logging 레벨 상수
        
    지원 레벨:
    - "DEBUG": logging.DEBUG (10)
    - "INFO": logging.INFO (20)
    - "WARNING": logging.WARNING (30)
    - "ERROR": logging.ERROR (40)
    - "CRITICAL": logging.CRITICAL (50)
    
    예시:
        >>> _parse_log_level("DEBUG")
        10
        >>> _parse_log_level("invalid")
        20  # default=logging.INFO
        
    주의:
    - 대소문자 무시 (대문자로 변환)
    - 잘못된 값은 default 반환
    """
    if not isinstance(log_level, str):
        return default
    log_level = log_level.upper()
    return getattr(logging, log_level, default)


class LoggingManager:
    """
    로거 생성 및 관리 클래스
    
    Attributes:
        config: 로그 설정 딕셔너리
        base_path: 로그 파일 기본 경로
        defaults: 기본 설정값
        loggers_cfg: 개별 로거 설정
        _ordered: 계층 순서로 정렬된 로거 목록
        _cache: 로거 인스턴스 캐시
        _lock: 스레드 동기화 락
        
    Class Attributes:
        _NAME_TO_PATH: 플랫 이름 → 계층 경로 매핑
        schema: 구조화된 로그 필드 스키마
        
    계층 구조:
    - 부모 로거 먼저 생성 (propagate 설정 위해)
    - 자식 로거는 부모로 전파 (propagate=True)
    - 루트(datahandler)만 propagate=False
    
    스키마 형식:
    - (라벨, 속성명, 너비)
    - 예: ("dtype", "datatype", 30)
    - 너비 0: 가변 길이
    
    주의사항:
    - 로거는 한 번만 생성됨 (캐시 사용)
    - 설정 변경 시 프로그램 재시작 필요
    - 계층 순서 중요 (부모 → 자식)
    """
    
    # ========================================================================
    # 클래스 변수
    # ========================================================================
    
    _lock = threading.Lock()
    
    # 플랫 이름 → 계층 경로 매핑
    _NAME_TO_PATH = {
        "main"          : "datahandler",
        "receiver"      : "datahandler.receiver",
        "sender"        : "datahandler.sender",
        "grpc"          : "datahandler.sender.grpc",
        "volt"          : "datahandler.sender.volt",
        "redis_server"  : "datahandler.sender.redis",
        "image"         : "datahandler.sender.image",
        "sqlite"        : "datahandler.sender.sqlite",
        "redis_rcv"     : "datahandler.receiver.redis",
        "merger"        : "datahandler.merger",
        "lp_detector"   : "datahandler.lp_detector",
    }
    
    # 구조화된 로그 필드 스키마
    # (라벨, 속성명, 너비)
    schema = [
        ("dtype", "datatype", 30),             # 데이터 타입
        ("data", "data", 60),                  # 데이터 식별자
        ("uk", "uk", 60),                      # 고유 키
        ("image", "image", 60),                # 이미지 경로
        ("plate number", "plate_number", 10),  # 번호판 번호
        ("remote_dir", "remote_dir", 60),      # 원격 디렉토리
        ("srv", "server", 30),                 # 서버 정보
        ("res_code", "res_code", 5),           # 응답 코드
        ("error", "error", 0),                 # 에러 메시지 (가변)
        ("error_loc", "error_loc", 0),         # 에러 위치 (가변)
    ]
    
    def __init__(self):
        """
        LoggingManager 초기화
        
        처리:
        1. 설정 파일 로드
        2. 로그 디렉토리 생성
        3. 기본 설정 및 개별 로거 설정 추출
        4. 계층 순서로 정렬
        5. 모든 로거 생성
        
        계층 정렬:
        - "datahandler" (0개 점)
        - "datahandler.receiver" (1개 점)
        - "datahandler.receiver.redis" (2개 점)
        
        주의:
        - 부모 로거가 먼저 생성되어야 함
        - 디렉토리 없으면 자동 생성
        """
        # 설정 로드
        self.config = ConfigParser.get()["log"]
        self.base_path = self.config["path"]
        
        # 로그 디렉토리 생성
        if not os.path.exists(self.base_path):
            os.makedirs(self.base_path, exist_ok=True)
            
        self.defaults = self.config["defaults"]
        self.loggers_cfg = self.config["loggers"]
        
        # 계층 순서로 정렬 (점 개수 기준)
        self._ordered = sorted(
            self.loggers_cfg.items(),
            key=lambda kv: self._NAME_TO_PATH.get(kv[0], kv[0]).count(".")
        )
        
        self._cache = {}
        
        # 모든 로거 생성
        self._init_all()
        
    def _init_all(self):
        """
        모든 로거 생성
        
        처리:
        - 정렬된 순서로 각 로거 생성
        - _create_logger() 호출
        
        주의:
        - 계층 순서 중요 (부모 먼저)
        """
        for flat_name, cfg in self._ordered:
            self._create_logger(flat_name, cfg)
        
    def _create_logger(self, flat_name, cfg):
        """
        개별 로거 생성 및 설정
        
        Args:
            flat_name: 플랫 이름 (예: "receiver")
            cfg: 로거 설정 딕셔너리
            
        처리:
        1. 계층 경로 결정
        2. 설정값 추출 (기본값 사용)
        3. Formatter 생성
        4. Logger 생성 및 레벨 설정
        5. 필터 추가
        6. 핸들러 추가 (파일, 스트림)
        7. 캐시에 저장
        
        필터 순서:
        1. ErrorFilter: 예외 정보 추출
        2. ColumnsFilter: 구조화된 컬럼 생성
        3. PadFilter (3개): 필드 패딩
        
        핸들러:
        - TimedRotatingFileHandler: 시간 기반 로테이션
        - StreamHandler: 콘솔 출력 (옵션)
        
        주의:
        - 루트(datahandler)만 propagate=False
        - 기존 핸들러 제거 (logger.handlers = [])
        - stream 설정 주의 (None과 False 구분)
        """
        # 계층 경로 결정
        full_name = self._NAME_TO_PATH.get(flat_name, flat_name)
        
        # 설정값 추출 (기본값 사용)
        log_level   = _parse_log_level(
                      cfg.get("log_level"  , self.defaults["log_level"])
        )
        when        = cfg.get("when"       , self.defaults["when"])
        interval    = int(
                      cfg.get("interval"   , self.defaults["interval"])
        )
        backupCount = int(
                      cfg.get("backupCount", self.defaults["backupCount"])
        )
        stream      = _parse_bool(
                      cfg.get("stream"     , self.defaults["stream"])
        )
        log_format  = cfg.get("format"     , self.defaults["format"])
        filename    = cfg.get("filename"   , f"{flat_name}.log")
        filepath    = os.path.join(self.base_path, filename)
        
        # propagate 설정 (루트만 False)
        propagate = False if full_name == "datahandler" else True
        
        # Formatter 생성
        formatter = logging.Formatter(log_format)
        
        # Logger 생성
        logger = logging.getLogger(full_name)
        logger.setLevel(log_level)
        logger.propagate = propagate
        
        # 필터 추가
        logger.addFilter(ErrorFilter())
        logger.addFilter(ColumnsFilter(self.schema))
        logger.addFilter(PadFilter(dest="levelpad"   , width=9 , source="levelname"))
        logger.addFilter(PadFilter(dest="threadpad"  , width=12 , source="threadName"))
        logger.addFilter(PadFilter(dest="funcpad"    , width=26, source=lambda r: f"{r.module}:{r.funcName}"))
        logger.addFilter(PadFilter(dest="messagepad" , width=35 , source=lambda r: r.getMessage()))
        
        # 기존 핸들러 제거
        logger.handlers = []
        
        # 파일 핸들러 추가
        file_handler = handlers.TimedRotatingFileHandler(
            filename = filepath,
            when = when,              # 로테이션 단위 ("midnight", "H" 등)
            interval = interval,      # 로테이션 간격
            backupCount = backupCount,  # 백업 파일 개수
            encoding = "utf-8",
            delay = False,
            utc = False,
            atTime = None
        )
        file_handler.setLevel(log_level)
        file_handler.setFormatter(formatter)
        logger.addHandler(file_handler)
        
        # 스트림 핸들러 추가 (조건부)
        if full_name != "datahandler":
            # 자식 로거: stream 명시된 경우만
            stream = False if cfg.get("stream") is None else True
        if stream:
            stream_handler = logging.StreamHandler()
            stream_handler.setLevel(log_level)
            stream_handler.setFormatter(formatter)
            logger.addHandler(stream_handler)
        
        # 캐시에 저장
        self._cache[flat_name] = logger
        
    def _get_logger(self, flat_name):
        """
        캐시된 로거 가져오기
        
        Args:
            flat_name: 플랫 이름
            
        Returns:
            logging.Logger: 로거 인스턴스
            
        Raises:
            KeyError: 설정되지 않은 로거 요청 시
            
        주의:
        - 스레드 안전 (락 사용)
        - 없는 로거 요청 시 즉시 에러
        """
        with self._lock:
            if flat_name not in self._cache:
                raise KeyError(f"Logger '{flat_name}' Is Not Configured In Config.json")
            return self._cache[flat_name]


class PadFilter(logging.Filter):
    """
    로그 필드 패딩 필터
    
    기능:
    - 지정된 너비에 맞춰 공백 패딩 생성
    - 로그 출력 정렬용
    
    Attributes:
        dest: 패딩을 저장할 속성명
        width: 목표 너비
        source: 원본 값 (속성명 또는 함수)
        
    사용 예시:
        # levelname을 9자로 패딩
        PadFilter(dest="levelpad", width=9, source="levelname")
        # "INFO" → need=5 → "     " (5칸 공백)
        
        # 함수로 값 생성
        PadFilter(dest="funcpad", width=26, 
                  source=lambda r: f"{r.module}:{r.funcName}")
        
    동작:
    - 원본 길이 < 너비: 부족한 만큼 공백 생성
    - 원본 길이 >= 너비: 공백 없음
    
    주의:
    - 너비 초과 시 잘라내지 않음
    - dest 속성은 포맷 문자열에서 사용
    """
    
    def __init__(self, dest, width, source):
        """
        PadFilter 초기화
        
        Args:
            dest: 패딩을 저장할 속성명 (예: "levelpad")
            width: 목표 너비 (문자 수)
            source: 원본 값
                - str: 속성명 (예: "levelname")
                - callable: 함수 (예: lambda r: r.getMessage())
        """
        super().__init__()
        self.dest = dest
        self.width = width
        self.source = source

    def filter(self, record):
        """
        패딩 계산 및 설정
        
        Args:
            record: LogRecord 인스턴스
            
        Returns:
            bool: 항상 True (필터링 안 함)
            
        처리:
        1. source에서 텍스트 추출
        2. 부족한 길이 계산
        3. dest 속성에 공백 문자열 설정
        
        주의:
        - 에러 발생 시 빈 문자열 사용
        - 음수 길이는 0으로 처리
        """
        # source에서 텍스트 추출
        if isinstance(self.source, str):
            # 속성명인 경우
            text = str(getattr(record, self.source, ""))
        else:
            # 함수인 경우
            try:
                text = str(self.source(record))
            except Exception:
                text = ""
                
        # 부족한 길이 계산
        need = self.width - len(text)
        
        # 패딩 설정 (음수면 0)
        setattr(record, self.dest, " " * max(0, need))
        
        return True


class ColumnsFilter(logging.Filter):
    """
    구조화된 컬럼 출력 필터
    
    기능:
    - extra 필드를 "라벨: [값]" 형식으로 변환
    - 여러 필드를 공백으로 구분하여 조합
    
    Attributes:
        schema: 필드 스키마 리스트
            - (라벨, 속성명, 너비)
        sep: 구분자 (기본: " ")
        
    예시:
        logger.info("Test", extra={"datatype": "2k", "data": "12345"})
        # → "dtype: [2k]  data: [12345]"
        
    스키마 형식:
        ("dtype", "datatype", 30)
        - "dtype": 출력 라벨
        - "datatype": LogRecord 속성명
        - 30: 최소 너비 (0이면 가변)
        
    주의:
    - 값이 None이거나 빈 문자열이면 생략
    - 너비는 전체 텍스트 기준 (라벨 포함)
    """
    
    def __init__(self, schema, sep=" "):
        """
        ColumnsFilter 초기화
        
        Args:
            schema: 필드 스키마 리스트
            sep: 구분자 (기본: 공백)
        """
        super().__init__()
        self.schema = schema
        self.sep = sep

    def filter(self, record):
        """
        구조화된 컬럼 문자열 생성
        
        Args:
            record: LogRecord 인스턴스
            
        Returns:
            bool: 항상 True
            
        처리:
        1. 스키마의 각 필드 순회
        2. LogRecord에서 값 추출
        3. "라벨: [값]" 형식 생성
        4. 너비에 맞춰 패딩
        5. 모두 조합하여 columns 속성 설정
        
        출력 형식:
        - 값 있음: "dtype: [2k]  data: [12345]"
        - 값 없음: "" (빈 문자열)
        
        주의:
        - 모든 필드가 빈 값이면 columns=""
        - 첫 필드 앞에도 구분자 추가됨
        """
        parts = []
        
        # 스키마의 각 필드 처리
        for label, attr, width in self.schema:
            # LogRecord에서 값 추출
            val = getattr(record, attr, "")
            
            # 값이 없으면 스킵
            if val is None or val == "":
                continue
                
            # "라벨: [값]" 형식 생성
            text = f"{label}: [{val}]"
            
            # 너비에 맞춰 패딩 (너비 > 0인 경우만)
            if width and len(text) < width:
                text += " " * (width - len(text))
                
            parts.append(text)
            
        # 모든 필드 조합 (구분자 포함)
        record.columns = (self.sep + self.sep.join(parts)) if parts else ""
        
        return True


class ErrorFilter(logging.Filter):
    """
    예외 정보 추출 필터
    
    기능:
    - extra의 error가 예외 객체면 정보 추출
    - 에러 메시지와 발생 위치 분리
    
    Attributes:
        없음
        
    처리:
    - error: BaseException → str(error)
    - error_loc: "파일명|함수명|라인번호"
    
    예시:
        try:
            raise ValueError("test")
        except Exception as e:
            logger.error("Error", extra={"error": e})
        # → error: "test"
        #    error_loc: "test.py|main|42"
        
    주의:
    - error가 문자열이면 그대로 유지
    - __cause__ 체인 추적 (근본 원인)
    - 위치 추출 실패 시 "<unknown>"
    """
    
    def filter(self, record):
        """
        예외 정보 추출 및 설정
        
        Args:
            record: LogRecord 인스턴스
            
        Returns:
            bool: 항상 True
            
        처리:
        1. error 속성 확인
        2. BaseException이면 정보 추출
        3. error를 문자열로 변환
        4. error_loc 설정
        5. 문자열이면 그대로 유지
        
        에러 위치 형식:
        - "파일명|함수명|라인번호"
        - 예: "receiver.py|main_loop|42"
        
        주의:
        - 예외 체인 추적 (__cause__)
        - 추출 실패 시 "<unknown>|<unknown>|-1"
        """
        try:
            val = getattr(record, "error", None)
            
            # error가 예외 객체인 경우
            if isinstance(val, BaseException):
                try:
                    # 예외 발생 위치 추출
                    f, fn, ln = _exc_loc(val) 
                except Exception:
                    f, fn, ln = ("", "", "")
                    
                # error를 문자열로 변환
                record.error = str(val)
                
                # error_loc 설정 ("파일|함수|라인")
                record.error_loc = f"{f}|{fn}|{ln}"
            else:
                # error가 문자열이거나 None인 경우
                if getattr(record, "error", None) is not None:
                    record.error = str(record.error)
                record.error_loc = getattr(record, "error_loc", "")
        except Exception:
            # 에러 발생 시 무시
            pass
            
        return True


# ============================================================================
# 전역 싱글톤 관리
# ============================================================================

_MANAGER = None
_MANAGER_INIT_LOCK = threading.Lock()


def _exc_loc(e):
    """
    예외 발생 위치 추출
    
    Args:
        e: BaseException 인스턴스
        
    Returns:
        tuple: (파일명, 함수명, 라인번호)
        
    처리:
    1. __cause__ 체인 추적 (근본 원인)
    2. traceback에서 마지막 프레임 찾기
    3. 파일명, 함수명, 라인번호 추출
    
    예외 체인:
    - e.__cause__: 명시적 연결
    - 체인의 끝까지 추적
    
    traceback 추적:
    - tb.tb_next: 다음 프레임
    - 마지막 프레임: 실제 에러 발생 위치
    
    주의:
    - traceback 없으면 "<unknown>"
    - 파일명만 반환 (전체 경로 아님)
    
    예시:
        >>> try:
        ...     raise ValueError("test")
        ... except Exception as e:
        ...     _exc_loc(e)
        ("test.py", "main", 42)
    """
    # __cause__ 체인 추적
    cause = e
    while getattr(cause, "__cause__", None) is not None:
        cause = cause.__cause__
        
    # traceback 추출
    tb = getattr(cause, "__traceback__", None) or getattr(e, "__traceback__", None)
    
    # traceback 없으면 unknown
    if tb is None:
        return ("<unknown>", "<unknown>", -1)
        
    # 마지막 프레임까지 이동
    while tb.tb_next is not None:
        tb = tb.tb_next
        
    # 프레임 정보 추출
    frame = tb.tb_frame
    
    return (
        os.path.basename(frame.f_code.co_filename),  # 파일명 (경로 제외)
        frame.f_code.co_name,                        # 함수명
        tb.tb_lineno                                 # 라인번호
    )


def get_logger(name):
    """
    로거 인스턴스 가져오기
    
    Args:
        name: 플랫 이름 (예: "receiver", "sender" 등)
        
    Returns:
        logging.Logger: 로거 인스턴스
        
    처리:
    1. 싱글톤 매니저 생성 (첫 호출 시)
    2. 매니저에서 로거 가져오기
    
    싱글톤:
    - 더블 체크 락킹 패턴
    - 스레드 안전 초기화
    
    사용 예시:
        logger = get_logger("receiver")
        logger.info("Message", extra={"datatype": "2k"})
        
    주의:
    - 첫 호출 시 설정 파일 로드
    - 설정되지 않은 로거 요청 시 KeyError
    - 스레드 안전
    
    Raises:
        KeyError: 설정되지 않은 로거 요청 시
    """
    global _MANAGER
    
    # 싱글톤 매니저 생성 (더블 체크 락킹)
    if _MANAGER is None:
        with _MANAGER_INIT_LOCK:
            if _MANAGER is None:
                _MANAGER = LoggingManager()
                
    # 로거 가져오기
    return _MANAGER._get_logger(name)