"""
Hash Utility Module
===================
데이터 해싱을 위한 유틸리티 함수

주요 기능:
- SHA-256 해시 생성
- MD5 해시 생성

사용처:
- SHA-256: 고유 키 생성 (차량 ID, 데이터 추적)
- MD5: 파일명 해싱 (이미지 파일명 익명화)

보안:
- MD5는 보안용으로 적합하지 않음 (파일명 해싱만 사용)
- SHA-256은 충돌 저항성이 높음
"""

import hashlib


def hash_sha256(data):
    """
    SHA-256 해시 생성
    
    Args:
        data: 해시할 문자열 데이터
        
    Returns:
        str: 64자리 16진수 해시 문자열
        
    용도:
        - 차량 고유 ID 생성
        - 데이터 추적 키 생성
        - 중복 검사
        
    예시:
        >>> hash_sha256("CAM001_12345")
        "a3b5c7d9e1f2a4b6c8d0e2f4a6b8c0d2e4f6a8b0c2d4e6f8a0b2c4d6e8f0a2b4"
        
    특성:
        - 같은 입력 → 항상 같은 출력
        - 출력 길이 고정 (64자)
        - 역방향 계산 불가능
        - 충돌 확률 극히 낮음
        
    주의사항:
        - 빈 문자열도 해시 가능
        - 대소문자 구분 (다른 해시 생성)
        - 인코딩은 UTF-8 고정
    """
    # SHA-256 해시 객체 생성
    hash_object = hashlib.sha256()
    
    # 문자열을 바이트로 인코딩하여 업데이트
    hash_object.update(data.encode())
    
    # 16진수 문자열로 변환
    hash_data = hash_object.hexdigest()

    return hash_data


def hash_md5(data):
    """
    MD5 해시 생성
    
    Args:
        data: 해시할 문자열 데이터
        
    Returns:
        str: 32자리 16진수 해시 문자열
        
    용도:
        - 이미지 파일명 익명화
        - 빠른 체크섬 생성
        
    예시:
        >>> hash_md5("/images/car_12345.jpg")
        "5d41402abc4b2a76b9719d911017c592"
        
    특성:
        - SHA-256보다 빠름
        - 출력 길이 짧음 (32자)
        - 보안용으로 부적합 (충돌 취약)
        
    주의사항:
        - 보안이 중요한 용도에는 사용 금지
        - 파일명 해싱 등 비보안 용도만 사용
        - 충돌 가능성 있음 (극히 드묾)
        - 인코딩은 UTF-8 고정
    """
    # MD5 해시 객체 생성
    hash_object = hashlib.md5()
    
    # 문자열을 바이트로 인코딩하여 업데이트
    hash_object.update(data.encode())
    
    # 16진수 문자열로 변환
    hash_data = hash_object.hexdigest()

    return hash_data