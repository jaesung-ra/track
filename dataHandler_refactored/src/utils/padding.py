"""
Image Padding Utility Module
=============================
이미지를 정사각형으로 패딩하는 유틸리티

주요 기능:
- 직사각형 이미지를 정사각형으로 변환
- 양쪽 또는 상하에 검은색 패딩 추가
- 원본 비율 유지하며 정사각형 만들기

사용처:
- YOLO OCR 입력 전처리 (256x256 필요)
- 번호판 이미지 정규화

처리 방식:
- 세로가 길면: 좌우에 패딩
- 가로가 길면: 상하에 패딩
- 최종 크기: max(h, w) x max(h, w)
"""

import cv2
import numpy as np


def padding(img):
    """
    이미지를 정사각형으로 패딩
    
    Args:
        img: 입력 이미지 (numpy array, BGR 형식)
            - shape: (height, width, 3)
            
    Returns:
        numpy array: 정사각형 이미지
            - shape: (max_side, max_side, 3)
            - 패딩 영역: 검은색 (0, 0, 0)
            
    처리 과정:
    1. 이미지 크기 확인 (h, w)
    2. h > w: 세로가 길면 좌우 패딩
       - 새 이미지 크기: (h, h, 3)
       - 원본 위치: [:, padding:padding+w]
    3. h <= w: 가로가 길면 상하 패딩
       - 새 이미지 크기: (w, w, 3)
       - 원본 위치: [padding:padding+h, :]
    4. 리사이즈 (보간: INTER_LINEAR)
    
    예시:
        # 100x200 → 200x200
        >>> img = np.zeros((100, 200, 3), dtype=np.uint8)
        >>> padded = padding(img)
        >>> padded.shape
        (200, 200, 3)
        
        # 300x150 → 300x300
        >>> img = np.zeros((300, 150, 3), dtype=np.uint8)
        >>> padded = padding(img)
        >>> padded.shape
        (300, 300, 3)
        
    패딩 계산:
        - h > w: padding = (h - w) // 2
        - h <= w: padding = (w - h) // 2
        - 홀수 차이인 경우 1픽셀 오차 발생 가능
        
    주의사항:
    - 원본 이미지는 변경되지 않음
    - 패딩 색상은 검은색 고정 (변경 불가)
    - 리사이즈 보간: INTER_LINEAR (품질/속도 균형)
    - 정사각형 입력도 리사이즈됨 (동일 크기로)
    - 컬러 이미지만 지원 (채널 3)
    """
    # 이미지 크기 추출
    h, w = img.shape[:2]
    
    if h > w:
        # 세로가 길면 좌우 패딩
        padding = (h - w) // 2  # 좌우 패딩 크기
        
        # 검은색 정사각형 이미지 생성 (h x h)
        new_img = np.zeros((h, h, 3), np.uint8)
        
        # 원본 이미지를 중앙에 배치
        new_img[:, padding: padding + w] = img
        
        # 리사이즈 (크기 유지, 보간 적용)
        new_img = cv2.resize(new_img,
                             (h, h),
                             interpolation=cv2.INTER_LINEAR)

    else:
        # 가로가 길거나 같으면 상하 패딩
        padding = (w - h) // 2  # 상하 패딩 크기
        
        # 검은색 정사각형 이미지 생성 (w x w)
        new_img = np.zeros((w, w, 3), np.uint8)
        
        # 원본 이미지를 중앙에 배치
        new_img[padding: padding + h, :] = img
        
        # 리사이즈 (크기 유지, 보간 적용)
        new_img = cv2.resize(new_img,
                             (w, w),
                             interpolation=cv2.INTER_LINEAR)
                             
    return new_img