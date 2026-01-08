"""
Image Utility Module
====================
이미지 업로드 및 로컬 파일 관리 유틸리티

주요 기능:
1. 이미지 파일 업로드 (파일 경로 기반)
2. 이미지 바이트 업로드 (메모리 기반)
3. 업로드 후 로컬 파일 자동 삭제
4. 주기적인 오래된 이미지 정리

클래스:
- ImageRemove: 로컬 이미지 정리 관리
- ImagePost: 이미지 서버 업로드

정리 전략:
- 업로드 성공 시 즉시 삭제
- 10분 이상 오래된 파일 주기적 삭제
- 등록된 디렉토리만 정리 대상

지원 이미지 형식:
- jpg, jpeg, png, bmp, gif, webp, tif, tiff
"""

import os
import io
import time
import threading
import requests

from pathlib import Path
from typing import Set

from utils.logger import get_logger

logger = get_logger("image")

# 지원하는 이미지 확장자 목록
_IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tif", ".tiff"}


class ImageRemove:
    """
    로컬 이미지 파일 정리 관리 클래스
    
    Class Attributes:
        _dirs: 정리 대상 디렉토리 집합 (클래스 변수)
        _lock: 스레드 동기화 락
        THRESHOLD_SECONDS: 파일 삭제 기준 시간 (초)
        
    기능:
    1. register_path(): 정리 대상 디렉토리 등록
    2. cleanup(): 주기적 정리 실행 (10초마다 호출 권장)
    3. remove_image(): 특정 파일 즉시 삭제
    
    정리 전략:
    - 등록된 디렉토리의 모든 이미지 파일 검사
    - 10분(600초) 이상 수정되지 않은 파일 삭제
    - 재귀적으로 하위 디렉토리도 검사
    
    사용 예시:
        # 업로드 후 즉시 삭제
        ImageRemove.remove_image("/tmp/image.jpg")
        
        # 주기적 정리 (메인 루프에서)
        while True:
            ImageRemove.cleanup()
            time.sleep(10)
            
    주의사항:
    - 클래스 메서드로 전역 상태 공유
    - 스레드 안전 (락 사용)
    - 등록된 디렉토리만 정리 대상
    """
    
    # ========================================================================
    # 클래스 변수 (전역 상태)
    # ========================================================================
    _dirs: Set[Path] = set()  # 정리 대상 디렉토리 집합
    _lock = threading.Lock()  # 스레드 동기화 락

    # ========================================================================
    # 설정값
    # ========================================================================
    THRESHOLD_SECONDS = 10 * 60   # 10분 = 600초

    @classmethod
    def register_path(cls, file_path: str) -> None:
        """
        파일의 부모 디렉토리를 정리 대상으로 등록
        
        Args:
            file_path: 파일 경로 (절대 또는 상대)
            
        처리:
        1. 파일의 부모 디렉토리 추출
        2. 디렉토리가 존재하는지 확인
        3. _dirs 집합에 추가 (스레드 안전)
        
        사용처:
        - image_post_file(): 업로드 전 등록
        - remove_image(): 삭제 전 등록
        
        주의사항:
        - 디렉토리가 존재하지 않으면 무시
        - 중복 등록 가능 (set이므로 자동 제거)
        - 에러 발생 시 경고 로그만 남기고 계속
        """
        try:
            # 절대 경로로 변환 후 부모 디렉토리 추출
            p = Path(file_path).resolve().parent
            
            # 디렉토리가 존재하는지 확인
            if not p.exists():
                return
                
            # 스레드 안전하게 집합에 추가
            with cls._lock:
                cls._dirs.add(p)
        except Exception as e:
            logger.warning("ImageRemove register_path failed",
                           extra={"file_path": file_path, "error": str(e)})

    @classmethod
    def cleanup(cls) -> None:
        """
        등록된 모든 디렉토리에서 오래된 이미지 정리
        
        처리:
        1. _clean_once() 호출 (실제 정리 수행)
        2. 에러 발생 시 경고 로그
        
        호출 주기:
        - 메인 루프에서 10초마다 호출 권장
        - 너무 자주 호출하면 성능 저하
        
        주의사항:
        - 블로킹 방식 (정리 완료까지 대기)
        - 파일 많으면 시간 오래 걸림
        - 에러 발생해도 프로그램 계속 실행
        """
        try:
            cls._clean_once()
        except Exception as e:
            logger.warning("ImageRemove cleanup error", extra={"error": str(e)})

    @classmethod
    def _clean_once(cls) -> None:
        """
        실제 정리 수행 (내부 메서드)
        
        처리:
        1. _dirs 집합 복사 (락 최소화)
        2. 각 디렉토리의 모든 이미지 파일 검사
        3. 수정 시각이 10분 이전이면 삭제
        4. 통계 로깅 (검사/삭제 파일 수)
        
        삭제 조건:
        - 파일인지 확인 (디렉토리 제외)
        - 이미지 확장자인지 확인
        - st_mtime < (현재 - 10분)
        
        성능 최적화:
        - 락은 복사할 때만 사용
        - rglob("*")로 재귀 검색
        - FileNotFoundError 무시 (동시 삭제 대응)
        
        주의사항:
        - 삭제 실패 시 경고 로그
        - 파일 검사 중 에러는 무시하고 계속
        - 삭제는 remove_image() 사용 (일관성)
        """
        # 락 획득하여 디렉토리 목록 복사
        with cls._lock:
            dirs = list(cls._dirs)

        # 등록된 디렉토리 없으면 종료
        if not dirs:
            return

        # 삭제 기준 시각 계산
        now = time.time()
        cutoff = now - cls.THRESHOLD_SECONDS
        
        # 통계 변수
        checked = deleted = 0

        # 각 디렉토리 순회
        for d in dirs:
            # 재귀적으로 모든 파일 검사
            for p in d.rglob("*"):
                try:
                    # 파일인지 확인
                    if not p.is_file():
                        continue
                        
                    # 이미지 확장자인지 확인
                    if p.suffix.lower() not in _IMAGE_EXTS:
                        continue
                        
                    checked += 1
                    
                    # 수정 시각이 기준 이전이면 삭제
                    if p.stat().st_mtime < cutoff:
                        ok = cls.remove_image(str(p))
                        if ok:
                            deleted += 1
                        else:
                            logger.warning("Old image delete failed", extra={"image": str(p)})
                except FileNotFoundError:
                    # 동시 삭제된 경우 무시
                    continue
                except Exception as e:
                    logger.warning("Image check failed",
                                   extra={"path": str(p), "error": str(e)})

        # 정리 통계 로깅 (검사/삭제가 있는 경우만)
        if checked or deleted:
            logger.info(f"ImageRemove sweep dirs: {[_shorten_for_log(str(x)) for x in dirs]}, checked: {checked}, deleted: {deleted}")

    @classmethod
    def remove_image(cls, file_path: str) -> bool:
        """
        특정 이미지 파일 즉시 삭제
        
        Args:
            file_path: 삭제할 파일 경로
            
        Returns:
            bool: 삭제 성공 여부
            - True: 삭제 성공
            - False: 파일 없음 또는 삭제 실패
            
        처리:
        1. 파일 존재 확인
        2. 부모 디렉토리 등록 (정리 대상)
        3. 파일 삭제
        4. 성공/실패 로깅
        
        사용처:
        - image_post_file(): 업로드 성공 후 호출
        - _clean_once(): 오래된 파일 정리 시 호출
        
        주의사항:
        - 파일이 없어도 에러 아님 (False 반환)
        - 부모 디렉토리는 항상 등록됨
        - 삭제 실패 시 로그만 남김
        """
        try:
            # 파일 존재 확인
            if os.path.exists(file_path):
                # 부모 디렉토리 등록 (정리 대상)
                cls.register_path(file_path)
                
                # 파일 삭제
                os.remove(file_path)
                
                logger.info("Image Removal Success!", extra={"image": _shorten_for_log(file_path)})
                return True
            else:
                logger.error("Image Removal Failed",
                             extra={"image": _shorten_for_log(file_path), "error": "Image Doesn't Exist"})
                return False
        except Exception as e:
            logger.error("Image Removal Failed",
                         extra={"image": _shorten_for_log(file_path), "error": str(e)})
            return False


class ImagePost:
    """
    이미지 서버 업로드 클래스
    
    Static Methods:
        image_post_file(): 파일 경로 기반 업로드
        image_post_bytes(): 바이트 데이터 기반 업로드
        handle_response(): 서버 응답 처리
        
    업로드 프로토콜:
    - HTTP POST multipart/form-data
    - 필드: img (파일), img_path (원격 경로)
    - 타임아웃: (3초, 3초) - 연결/읽기
    - SSL 검증: 비활성화
    
    응답 형식:
    - 성공: {"rescd": "00"}
    - 실패: {"rescd": "에러코드"}
    
    주의사항:
    - 업로드 실패 시 재시도 없음
    - 파일 업로드 성공 시 로컬 파일 자동 삭제
    - 바이트 업로드는 로컬 파일 없음
    """
    
    @staticmethod
    def image_post_file(image_server_url, file_path, remote_dir, remote_image_name):
        """
        파일 경로 기반 이미지 업로드
        
        Args:
            image_server_url: 이미지 서버 URL (예: http://server:port/api/img)
            file_path: 업로드할 로컬 파일 경로
            remote_dir: 원격 저장 디렉토리
            remote_image_name: 원격 파일명
            
        Returns:
            bool: 업로드 성공 여부
            - True: 업로드 성공 (또는 파일 없음/네트워크 에러)
            - False: 서버 응답 에러
            
        처리 흐름:
        1. 부모 디렉토리 정리 대상 등록
        2. 파일 열기 및 multipart 요청 생성
        3. POST 요청 전송 (타임아웃 3초)
        4. 응답 처리
        5. 성공 시 로컬 파일 삭제
        
        에러 처리:
        - FileNotFoundError: True 반환 (재시도 불필요)
        - RequestException: True 반환 (재시도 불필요)
        - 서버 응답 에러: False 반환
        
        주의사항:
        - 네트워크 에러 시 True 반환 (재시도 방지)
        - SSL 검증 비활성화 (verify=False)
        - 업로드 성공 시 로컬 파일 즉시 삭제
        - 삭제 실패해도 업로드는 성공으로 간주
        """
        response = None

        # 부모 디렉토리를 정리 대상으로 등록
        ImageRemove.register_path(file_path)

        try:
            # 파일 열기 및 multipart 요청 생성
            with open(file_path, 'rb') as image:
                files = {'img': (remote_image_name, image, "image/jpeg")}
                
                # POST 요청 전송
                response = requests.post(
                    image_server_url,
                    files=files,
                    data={'img_path': remote_dir},
                    timeout=(3, 3),  # (연결 타임아웃, 읽기 타임아웃)
                    verify=False     # SSL 검증 비활성화
                )
        except (requests.RequestException, FileNotFoundError) as e:
            # 네트워크 에러 또는 파일 없음: 재시도 불필요
            logger.error(
                "Image Upload Failed! No Retries!",
                extra={"image": _shorten_for_log(file_path), "remote_dir": remote_dir, "error": str(e)}
            )
            return True

        # 서버 응답 처리
        success = ImagePost.handle_response(response, remote_dir)

        if success is True:
            # 업로드 성공: 로컬 파일 삭제
            logger.info("Image Upload Success!", extra={"image": _shorten_for_log(file_path), "remote_dir": remote_dir})
            ImageRemove.remove_image(file_path)
            return True
        else:
            # 업로드 실패: 에러 코드 로깅
            logger.error("Image Upload Failed!",
                         extra={"image": _shorten_for_log(file_path), "remote_dir": remote_dir, "res_code": success})
            return False

    @staticmethod
    def image_post_bytes(image_server_url, image_bytes, remote_dir, remote_image_name):
        """
        바이트 데이터 기반 이미지 업로드
        
        Args:
            image_server_url: 이미지 서버 URL
            image_bytes: 이미지 바이트 데이터
            remote_dir: 원격 저장 디렉토리
            remote_image_name: 원격 파일명
            
        Returns:
            bool: 업로드 성공 여부
            - True: 업로드 성공
            - False: 업로드 실패
            
        처리 흐름:
        1. image_bytes가 "NULL"이면 스킵 (성공 반환)
        2. BytesIO로 메모리 스트림 생성
        3. multipart 요청 생성 및 전송
        4. 응답 처리
        
        사용처:
        - 4K 이미지 업로드 (메모리에서 직접)
        - OCR 처리 후 번호판 이미지 업로드
        
        주의사항:
        - 로컬 파일 없음 (삭제 불필요)
        - "NULL"은 번호판 없음을 의미
        - 네트워크 에러 시 False 반환
        - SSL 검증 비활성화
        """
        # "NULL"은 데이터 없음 (번호판 없음 등)
        if image_bytes == "NULL":
            return True
            
        try:
            # 바이트 데이터를 메모리 스트림으로 변환
            image = io.BytesIO(image_bytes)
            files = {'img': (remote_image_name, image, 'image/jpeg')}
            
            # POST 요청 전송
            response = requests.post(
                image_server_url,
                files=files,
                data={'img_path': remote_dir},
                timeout=(3, 3),
                verify=False
            )
        except requests.RequestException:
            # 네트워크 에러: 재시도는 상위 계층에서 처리
            return False

        # 서버 응답 처리
        success = ImagePost.handle_response(response, remote_dir)
        if success:
            logger.info("Image Upload Success!", extra={"remote_dir": remote_dir})
        else:
            logger.error("Image Upload Failed!", extra={"remote_dir": remote_dir, "res_code": success})
        return success

    @staticmethod
    def handle_response(response, remote_dir):
        """
        서버 응답 처리 및 성공 여부 판단
        
        Args:
            response: requests.Response 객체
            remote_dir: 원격 디렉토리 (로깅용)
            
        Returns:
            bool or str: 처리 결과
            - True: 성공 (rescd == "00")
            - False: response가 None
            - str: 에러 코드 (rescd != "00")
            
        응답 형식:
        {
            "rescd": "00",  // 성공 코드
            ...
        }
        
        에러 처리:
        - response가 None: False 반환
        - JSON 파싱 실패: False 반환
        - rescd 키 없음: False 반환
        
        주의사항:
        - rescd == "00"만 성공으로 간주
        - 다른 코드는 에러 코드 문자열로 반환
        - 호출자가 True/False/에러코드 구분해야 함
        """
        # response가 None이면 실패
        if response is None:
            return False
            
        try:
            # JSON 파싱하여 rescd 추출
            res_code = response.json().get('rescd')
        except Exception:
            # JSON 파싱 실패
            return False
            
        # rescd == "00"이면 성공, 아니면 에러 코드 반환
        return True if res_code == '00' else res_code


def _shorten_for_log(path: str) -> str:
    """
    로그용 경로 축약
    
    Args:
        path: 전체 파일 경로
        
    Returns:
        str: 축약된 경로 (마지막 3개 디렉토리만)
        
    예시:
        >>> _shorten_for_log("/var/data/images/2024/01/01/image.jpg")
        ".../01/01/image.jpg"
        
    목적:
    - 로그 가독성 향상
    - 중요 정보만 표시 (년/월/일 등)
    
    주의:
    - 경로가 짧으면 전체 표시
    - "/" 기준 분할 (Windows "\\" 고려 안 함)
    """
    parts = path.split("/")
    tail = "/".join(parts[-3:])
    return f".../{tail}"