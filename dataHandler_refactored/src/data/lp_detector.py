"""
License Plate Detector Module
==============================
번호판 검지 및 OCR 처리를 담당하는 모듈

주요 기능:
1. YOLO 기반 번호판 영역 검출
2. OCR을 통한 번호판 문자 인식
3. 여러 이미지 중 최적의 OCR 결과 선택
4. 이미지 인코딩 및 전송 준비

처리 흐름:
Raw 4K 이미지 → 번호판 검출 → 번호판 자르기 → OCR → 최적 결과 선택 → 이미지 인코딩
"""

import cv2
import threading
import os
import glob
import re
import numpy as np

from data import yolo
from data.sender import DataSender
from data.constants import FieldKey as fk

from utils.config_parser import ConfigParser
from utils.logger import get_logger
from utils.padding import padding
from utils.images import ImageRemove

from sklearn.linear_model import LinearRegression


class LpDetector:
    """
    번호판 검지 및 OCR 처리 클래스
    
    Attributes:
        logger: 로거 인스턴스
        config: OCR 설정 정보
        to_server_q: 서버 전송 큐
        to_ocr_q: OCR 처리 대기 큐
        plate_detector: 번호판 검출 YOLO 모델
        OCR: OCR YOLO 모델
        flag: 스레드 종료 플래그
        
    주의사항:
        - YOLO 모델은 CUDA를 사용하므로 GPU 메모리 관리 필요
        - 오토바이(MOTOR)는 번호판 검출을 건너뜀
        - 처리 완료된 이미지는 자동 삭제됨
    """
    
    def __init__(self, to_server_q, to_ocr_q):
        """
        LpDetector 초기화
        
        Args:
            to_server_q: 서버 전송 큐 (처리 완료 데이터 전달)
            to_ocr_q: OCR 처리 대기 큐 (처리할 데이터 수신)
        """
        self.logger = get_logger("lp_detector")
        self.config =  ConfigParser.get()["OCR"]
        self.to_server_q = to_server_q
        self.to_ocr_q = to_ocr_q
        
        # YOLO 모델 로드 (번호판 검출 + OCR)
        self.plate_detector = yolo.load_model(self.config["plate_detector_model"])
        self.OCR = yolo.load_model(self.config["OCR_model"])
        
        self.flag = threading.Event()
        
    def stop(self):
        """스레드 종료 플래그 설정"""
        self.flag.set()
        
    def main_loop(self):
        """
        메인 처리 루프
        
        처리 단계:
        1. 모델 워밍업 (초기 한번)
        2. OCR 큐에서 데이터 수신
        3. 이미지 파일 검색
        4. 각 이미지에 대해 번호판 검출 및 OCR 수행
        5. 가장 높은 신뢰도의 결과 선택
        6. 이미지 인코딩 및 메타데이터 설정
        7. 서버 전송 큐로 전달
        
        예외 처리:
        - 이미지 없음: N_PLATE로 설정 후 전송
        - 이미지 로드 실패: 해당 이미지 스킵
        - OCR 실패: 이전까지의 최선 결과 사용
        """
        self._model_warm_up()
        
        while not self.flag.is_set():
            try:
                data = self.to_ocr_q.get()
                
                # 데이터에 해당하는 이미지 파일들 검색
                image_paths = self._grep_images(data)
                print(image_paths)
                
                # 이미지가 없는 경우 처리
                if not image_paths:
                    data[fk.PLATE_NUM] = "N_PLATE"
                    data[fk.PLATE_YN] = "N"
                    data[fk.IMAGE_PATH_NAME] = "N_IMAGE"
                    data[fk.CAR_IMAGE_FILE_NAME] = "N_IMAGE" 
                    data[fk.PLATE_IMAGE_FILE_NAME] = "N_IMAGE"
                    self.logger.error("No Images Exist!", extra={"data":data[fk.UK_PLAIN]})
                    self.to_server_q.put(data)
                    continue
                
                # 최적의 OCR 결과를 찾기 위한 변수 초기화
                best_ocr = 0.0          # 최고 OCR 신뢰도
                best_plate_num = ""     # 최고 신뢰도의 번호판 번호
                best_plate = None       # 최고 신뢰도의 번호판 이미지
                best_image = None       # 최고 신뢰도의 원본 차량 이미지
                
                # 모든 이미지에 대해 OCR 수행 및 최적 결과 선택
                for path in image_paths:
                    # 오토바이는 번호판 검출 건너뜀
                    if data[fk.CAR_TYPE] == "MOTOR":
                        if best_image is None:
                            best_image = cv2.imread(path)
                            best_plate_num = "N_PLATE"
                        ImageRemove.remove_image(path)
                        continue
                    
                    # 이미지 로드 및 자동 삭제
                    try:
                        img = cv2.imread(path)
                        ImageRemove.remove_image(path)
                    except Exception as e:
                        self.logger.error("Image Load Failed!", extra={"data":data[fk.UK_PLAIN], "error":e})
                        continue
                    
                    # 번호판 검출 및 자르기
                    plate_image, plate_conf = self._detect_plate_crop(img)
                    
                    # OCR 수행
                    plate_num, ocr_conf = self._ocr_plate(plate_image)
                    
                    # 더 높은 신뢰도의 결과인 경우 업데이트
                    if ocr_conf > best_ocr:
                        best_plate = plate_image
                        best_image = img
                        best_plate_num = plate_num
                        best_ocr = ocr_conf
                    else:
                        # 메모리 절약을 위해 사용하지 않는 이미지 삭제
                        del plate_image
                        del img
                
                # 최종 결과가 있는 경우 메타데이터 설정 및 이미지 인코딩
                if best_image is not None:
                    data[fk.PLATE_YN] = "N" if best_plate_num == "N_PLATE" else "Y"
                    data[fk.PLATE_NUM] = best_plate_num
                    data[fk.CAR_IMAGE_FILE_NAME] = data[fk.CAR_ID_4K] + "_" + data[fk.CAR_TYPE] + "_" + data[fk.LANE_NO] + "_" + data[fk.STOP_PASS_TIME] + ".jpg"
                    data[fk.PLATE_IMAGE_FILE_NAME] = data[fk.CAR_ID_4K] + ".jpg"
                    
                    try:
                        # 차량 이미지 JPG 인코딩
                        ok, buffer = cv2.imencode(".jpg", best_image)
                        if not ok:
                            self.logger.error(f"Crop Image Encode Failed")
                            continue
                        data[fk.IMAGE_BYTES_4K] = bytes(buffer)
                        
                        # 번호판 이미지가 있는 경우 JPG 인코딩
                        if best_plate is not None:
                            ok, buffer = cv2.imencode(".jpg", best_plate)
                            if not ok:
                                self.logger.error(f"Plate Image Encode Failed")
                                continue
                            data[fk.IMAGE_BYTES_PLATE_4K] = bytes(buffer)
                    except Exception as e:
                        self.logger.error("Image Encoding Failed!", extra={"data":data[fk.UK_PLAIN], "error":e})
                
                self.logger.info("Processed Data in LpDetector!", extra={"data":data[fk.UK_PLAIN], "plate_number":data[fk.PLATE_NUM]})
                self.logger.debug("Processed Full Data:\n%s", {k:v for k,v in data.items() if k not in {fk.IMAGE_BYTES_4K, fk.IMAGE_BYTES_PLATE_4K}})
                self.to_server_q.put(data)
            except Exception as e:
                self.logger.critical("Something Went Wrong in LpDetector!", extra={"error":e})
    
    def _model_warm_up(self):
        """
        YOLO 모델 워밍업
        
        목적:
        - 첫 추론 시 발생하는 초기화 지연 제거
        - CUDA 메모리 사전 할당
        
        동작:
        - 더미 이미지로 2회 추론 수행
        - 번호판 검출 모델과 OCR 모델 모두 워밍업
        """
        self.logger.debug("Model Warm Up Start")
        for _ in range(2):
            # 번호판 검출 모델 워밍업
            plate_dummy = np.zeros((416, 416, 3), dtype=np.uint8)
            self._detect_plate_crop(plate_dummy)
            
            # OCR 모델 워밍업
            ocr_dummy = np.zeros((256, 256, 3), dtype=np.uint8)
            self._ocr_plate(ocr_dummy)
        self.logger.debug("Model Warm Up Finished")
            
    def _grep_images(self, data):
        """
        데이터에 해당하는 이미지 파일 검색
        
        Args:
            data: 차량 검지 데이터 (CAR_ID_4K 포함)
            
        Returns:
            list: 매칭되는 이미지 파일 경로 리스트
            
        검색 패턴:
        - {base_dir}/{car_id_4k}_*.*
        - 예: /images/12345_lane1_1234567890.jpg
        
        주의:
        - car_id_4k와 timestamp가 파일명에 포함되어 있어야 함
        """
        base_dir = data[fk.IMAGE_PATH_NAME].rstrip("/\\")
        
        car_id_4k = str(data[fk.CAR_ID_4K])
        ts        = str(data[fk.STOP_PASS_TIME])
        
        # glob 패턴으로 이미지 파일 검색
        pattern = os.path.join(base_dir, f"{car_id_4k}_*.*")
        paths = []      
        paths = glob.glob(pattern)

        return paths
    
    def _ocr_plate(self, plate_image):
        """
        번호판 이미지에서 문자 인식 (OCR)
        
        Args:
            plate_image: 번호판 영역이 잘린 이미지 (numpy array)
            
        Returns:
            tuple: (번호판 문자열, 신뢰도 합계)
            - 번호판 문자열: 인식된 번호판 텍스트
            - 신뢰도: 전체 문자의 신뢰도 합 (높을수록 좋음)
            
        처리 과정:
        1. YOLO로 각 문자 검출
        2. 선형 회귀로 문자 배열 방향 분석
        3. 2줄 번호판 처리 (분산값이 큰 경우)
        4. 문자 위치 기준 정렬
        5. 문자열 조합
        
        특수 케이스:
        - None 입력: "N_PLATE" 반환
        - 문자 없음: "N_OCR" 반환
        - 2줄 번호판: 상단줄 + 하단줄 순서로 조합
        
        주의:
        - variance >= 10: 2줄 번호판으로 판단
        - 문자 간 y 좌표 차이 < 9: 같은 줄로 보정
        """
        if plate_image is None:
            return "N_PLATE", 0.1
            
        # YOLO로 문자 검출
        output = yolo.detect_objects(
            img=plate_image,
            net=self.OCR[0],
            outputLayers=self.OCR[-1],
            dim=(256,256)
        )
        
        # 바운딩 박스, 신뢰도, 클래스 ID 추출
        b, c, class_ids = yolo.get_box_dimensions(output[-1], plate_image.shape[:2])
        
        # NMS (Non-Maximum Suppression)로 중복 제거
        indexes = cv2.dnn.NMSBoxes(b, c, 0.5, 0.4)
        zipped = list(zip(b, c, class_ids))
        zipped = [zipped[i[0]] for i in indexes]
        
        plate_num = None
        conf = 0
        
        if len(zipped) == 0:
            return "N_OCR", 0.1
            
        # 문자 중심 좌표 추출
        x_center = [[i[0][0]] for i in zipped]
        y_center = [[j[0][1]] for j in zipped]

        # 선형 회귀로 문자 배열 방향 분석
        lr_model = LinearRegression().fit(x_center, y_center)
        y_predict = lr_model.predict(x_center)
        
        # 회귀선으로부터의 분산 계산 (2줄 번호판 판별용)
        sum_val = 0
        for i in range(len(y_predict)):
            sum_val += (y_predict[i] - y_center[i])**2
        variance = sum_val / len(zipped)
        
        # 신뢰도 합산 및 같은 줄 문자 y 좌표 보정
        for i in range(len(zipped)):
            conf += zipped[i][1]
            for j in range(len(zipped)):
                if i != j:
                    # y 좌표 차이가 9 미만이면 같은 줄로 간주
                    if abs(zipped[i][0][1] - zipped[j][0][1]) < 9:
                        zipped[j][0][1] = zipped[i][0][1]
                        
        # 회귀선 계수
        a=lr_model.coef_
        b=lr_model.intercept_
        
        # 분산이 크면 2줄 번호판으로 처리
        if variance >= 10:
            zip1 = []  # 상단 줄
            zip2 = []  # 하단 줄
            
            # 회귀선 기준으로 상/하단 분리
            for i in range(len(y_predict)):
                if zipped[i][0][1] < y_predict[i]:
                    zip1.append(zipped[i])
                else:
                    zip2.append(zipped[i])

            # 상단 줄 중심점 계산
            u_x = 0
            u_y = 0            
            for val in zip1:
                u_x += val[0][0]
                u_y += val[0][1]
            u_x = u_x / len(zip1)
            u_y = u_y / len(zip1)

            # 하단 줄 중심점 계산
            d_x = 0
            d_y = 0            
            for val in zip2:
                d_x += val[0][0]
                d_y += val[0][1]
            d_x = d_x / len(zip2)
            d_y = d_y / len(zip2)        

            # 전체 중심점 계산
            center_x = (u_x + d_x) / 2
            center_y = (u_y + d_y) / 2

            # 중심점 기준 회귀선 재조정
            b = center_y - (a * center_x)
            upper=[]
            down=[]

            # 재조정된 회귀선 기준 재분류
            for i in range(len(zipped)):
                if zipped[i][0][1] < (a * zipped[i][0][0] + b):
                    upper.append(zipped[i])
                else:
                    down.append(zipped[i])
                
            # 각 줄별로 x 좌표 기준 정렬
            upper = sorted(upper, key=lambda upper:(upper[0][0]))
            down = sorted(down, key=lambda down:(down[0][0]))
            
            # 상단 + 하단 순서로 조합
            zipped = upper + down
        else:
            # 단일 줄 번호판: x 좌표 기준 정렬
            zipped = sorted(zipped, key=lambda zipped:(zipped[0][0]))
            
        # 문자 클래스 ID를 실제 문자로 변환하여 조합
        # 클래스 ID > 9: 문자 (A-Z 등)
        # 클래스 ID <= 9: 숫자 (0-9)
        plate_num = "".join(
            [self.OCR[1][x[-1]] if int(x[-1]) > 9 else str(x[-1]) for x in zipped]
        )    
        return plate_num, conf
            
    def _detect_plate_crop(self, image):
        """
        차량 이미지에서 번호판 영역 검출 및 자르기
        
        Args:
            image: 전체 차량 이미지 (numpy array)
            
        Returns:
            tuple: (번호판 이미지, 신뢰도)
            - 번호판 이미지: 잘린 번호판 영역 (패딩 적용됨)
            - 신뢰도: 검출 신뢰도 리스트
            
        처리 과정:
        1. YOLO로 번호판 영역 검출
        2. NMS로 중복 제거
        3. 첫 번째 검출 결과 사용
        4. 바운딩 박스 좌표로 이미지 자르기
        5. 패딩 적용 (정사각형 만들기)
        
        주의:
        - 번호판이 없으면 (None, None) 반환
        - x 좌표가 음수면 0으로 보정
        - 원본 이미지 변경하지 않도록 copy() 사용
        """
        # YOLO로 번호판 영역 검출
        output = yolo.detect_objects(
            img=image,
            net=self.plate_detector[0],
            outputLayers=self.plate_detector[-1],
            dim=(416,416)
        )
        
        # 바운딩 박스와 신뢰도 추출
        boxes, confs, _ = yolo.get_box_dimensions(output[-1], image.shape[:2])
        
        # NMS로 중복 제거
        indexes = cv2.dnn.NMSBoxes(boxes, confs, 0.5, 0.4)
        
        # 번호판이 검출되지 않은 경우
        if len(boxes) == 0:
            return None, None
        
        # 첫 번째 검출 결과 사용
        x, y, w, h = boxes[0]
        
        # 음수 좌표 보정
        if x < 0:
            x = 0
            
        # 번호판 영역 자르기 (원본 보호를 위해 copy)
        plate_image = image[y:y+h, x:x+w].copy()
        
        # 정사각형으로 패딩 적용 (OCR 입력 형식 맞추기)
        plate_image = padding(plate_image)
        
        return plate_image, confs