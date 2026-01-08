#ifndef IMAGE_STORAGE_H
#define IMAGE_STORAGE_H

#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <string>

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief 범용 이미지 저장 클래스
 * 
 * OpenCV Mat 이미지를 파일 시스템에 저장
 * 특정 용도에 종속되지 않은 범용적인 인터페이스를 제공
 */
class ImageStorage {
private:
    std::shared_ptr<spdlog::logger> logger;
    std::mutex storage_mutex;
    
    // JPEG 압축 품질 (0-100)
    int jpeg_quality = 95;
    
    /**
     * @brief 디렉토리가 생성 확인 (static)
     * @param path 디렉토리 경로
     * @return 성공 시 true
     */
    static bool ensureDirectory(const std::string& path);
    
public:
    /**
     * @brief 생성자
     * @param quality JPEG 압축 품질 (기본값: 95)
     */
    explicit ImageStorage(int quality = 95);
    
    /**
     * @brief 소멸자
     */
    ~ImageStorage() = default;
    
    /**
     * @brief 이미지 저장 (범용)
     * @param image 저장할 이미지 (cv::Mat)
     * @param full_path 전체 파일 경로 (디렉토리 + 파일명)
     * @return 성공 시 true, 실패 시 false
     */
    bool save(const cv::Mat& image, const std::string& full_path);
    
    /**
     * @brief 이미지 저장 (디렉토리와 파일명 분리)
     * @param image 저장할 이미지 (cv::Mat)
     * @param directory 저장 디렉토리
     * @param filename 파일명 (확장자 포함)
     * @return 성공 시 전체 경로, 실패 시 빈 문자열
     */
    std::string saveImage(const cv::Mat& image, 
                         const std::string& directory,
                         const std::string& filename);
    
    /**
     * @brief JPEG 품질 설정
     * @param quality 압축 품질 (0-100)
     */
    void setJpegQuality(int quality) { 
        jpeg_quality = std::max(0, std::min(100, quality)); 
    }
    
    /**
     * @brief 현재 JPEG 품질 조회
     * @return 현재 설정된 JPEG 품질
     */
    int getJpegQuality() const { return jpeg_quality; }
    
    /**
     * @brief 디렉토리 생성 확인 (public static)
     * @param path 디렉토리 경로
     * @return 성공 시 true
     */
    static bool createDirectory(const std::string& path);
};

#endif // IMAGE_STORAGE_H