/*
 * image_cropper.h
 * 
 * 범용 이미지 크롭 모듈
 */

#ifndef IMAGE_CROPPER_H
#define IMAGE_CROPPER_H

#include <memory>
#include <opencv2/opencv.hpp>
#include "../common/object_data.h"
#include "nvbufsurface.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief 이미지 크롭 클래스
 * 
 * NvBufSurface에서 특정 영역을 크롭하여 OpenCV Mat으로 변환
 * 객체 검출, ROI 추출, 스냅샷 등 다양한 용도로 사용 가능
 */
class ImageCropper {
private:
    std::shared_ptr<spdlog::logger> logger;
    
    /**
     * @brief NvBufSurface에서 전체 프레임 추출
     * @param surface 서피스
     * @param batch_idx 배치 인덱스
     * @return OpenCV Mat 이미지
     */
    cv::Mat extractFullFrame(NvBufSurface* surface, int batch_idx);

public:
    /**
     * @brief 생성자
     */
    ImageCropper();
    
    /**
     * @brief 소멸자
     */
    ~ImageCropper();
    
    /**
     * @brief 객체 영역 크롭 (바운딩 박스 기반)
     * @param surface 서피스
     * @param batch_idx 배치 인덱스
     * @param bbox 바운딩 박스
     * @param padding 패딩 크기
     * @return 크롭된 이미지
     */
    cv::Mat cropObject(NvBufSurface* surface, int batch_idx, 
                      const box& bbox, int padding = 15);
    
    /**
     * @brief 특정 영역 크롭 (좌표 기반)
     * @param surface 서피스
     * @param batch_idx 배치 인덱스
     * @param x X 좌표
     * @param y Y 좌표
     * @param width 너비
     * @param height 높이
     * @param src_width 소스 너비
     * @param src_height 소스 높이
     * @return 크롭된 이미지
     */
    cv::Mat cropRegion(NvBufSurface* surface, int batch_idx,
                      int x, int y, int width, int height,
                      int src_width, int src_height);
    
    /**
     * @brief 전체 프레임 스냅샷 (크롭 없이)
     * @param surface 서피스
     * @param batch_idx 배치 인덱스
     * @return 전체 프레임 이미지
     */
    cv::Mat getFullFrame(NvBufSurface* surface, int batch_idx) {
        return extractFullFrame(surface, batch_idx);
    }
};

#endif // IMAGE_CROPPER_H