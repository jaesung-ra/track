"""
File Utility Module
===================
파일 경로 생성 및 시간 변환 유틸리티

주요 기능:
- Unix timestamp를 년/월/일/시/분 경로로 변환
- 이미지 타입별 경로 구조 생성

경로 형식:
- 차량/돌발 이미지 (10, 30): YYYY/MM/DD/HH/MM
- 기타 이미지 (20): YYYY/MM/DD

시간대:
- KST (UTC+9) 기준으로 변환
- Unix timestamp에 32400초 (9시간) 추가
"""

import os
from datetime import datetime


def generate_time_path(unix_time, image_type):
    """
    Unix timestamp를 이미지 저장 경로로 변환
    
    Args:
        unix_time: Unix timestamp (초 단위)
        image_type: 이미지 타입
            - 10: 차량 이미지 (분 단위 경로)
            - 20: 대기행렬/번호판 이미지 (일 단위 경로)
            - 30: 돌발상황 이미지 (분 단위 경로)
            
    Returns:
        str: 상대 경로 문자열
            - image_type 10/30: "YYYY/MM/DD/HH/MM"
            - image_type 20: "YYYY/MM/DD"
            
    예시:
        >>> generate_time_path(1609459200, 10)  # 2021-01-01 00:00:00 UTC
        "2021/01/01/09/00"  # KST 09:00
        
        >>> generate_time_path(1609459200, 20)
        "2021/01/01"
        
    시간대 변환:
        - Unix timestamp는 UTC 기준
        - 32400초 (9시간) 추가로 KST 변환
        - datetime.utcfromtimestamp() 사용으로 KST 시각 생성
        
    주의사항:
        - 32400초는 하드코딩됨 (시간대 설정 없음)
        - 서머타임 미고려 (한국은 서머타임 없음)
        - image_type이 10/30이 아니면 모두 일 단위 경로
    """
    # KST 변환 (UTC+9)
    unix_time += 32400 
    
    # timestamp를 YYYYMMDDHHMMSS 형식 문자열로 변환
    timestamp = datetime.utcfromtimestamp(unix_time).strftime('%Y%m%d%H%M%S')
    
    # 년/월/일/시/분 추출
    year = timestamp[0:4]
    month = timestamp[4:6]
    day = timestamp[6:8]
    hour = timestamp[8:10]
    minute = timestamp[10:12]

    # 이미지 타입에 따라 경로 생성
    if image_type == 10 or image_type == 30:
        # 차량/돌발 이미지: 분 단위 경로
        remote_path = os.path.join(year, month, day, hour, minute)
    else:
        # 대기행렬/번호판 이미지: 일 단위 경로
        remote_path = os.path.join(year, month, day)

    return remote_path