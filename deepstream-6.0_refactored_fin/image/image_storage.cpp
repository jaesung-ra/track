#include "image_storage.h"
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

ImageStorage::ImageStorage(int quality) : jpeg_quality(quality) {
    logger = getLogger("DS_ImageStorage_log");
    logger->info("ImageStorage 초기화 (JPEG 품질: {})", jpeg_quality);
}

bool ImageStorage::ensureDirectory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == -1) {
        // 0775 권한으로 변경 (그룹 쓰기 권한 추가)
        if (mkdir(path.c_str(), 0775) == -1) {
            // static 함수에서는 logger 사용 불가
            return false;
        }
    }
    return true;
}

// Public static 메서드
bool ImageStorage::createDirectory(const std::string& path) {
    return ensureDirectory(path);
}

bool ImageStorage::save(const cv::Mat& image, const std::string& full_path) {
    if (image.empty()) {
        logger->error("빈 이미지는 저장할 수 없음");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(storage_mutex);
    
    try {
        // JPEG 파라미터 설정
        std::vector<int> params;
        params.push_back(cv::IMWRITE_JPEG_QUALITY);
        params.push_back(jpeg_quality);
        
        // 이미지 저장
        if (cv::imwrite(full_path, image, params)) {
            logger->info("이미지 저장 완료: {}", full_path);
            return true;
        } else {
            logger->error("이미지 저장 실패: {}", full_path);
            return false;
        }
        
    } catch (const std::exception& e) {
        logger->error("이미지 저장 중 예외 발생: {} - {}", full_path, e.what());
        return false;
    }
}

std::string ImageStorage::saveImage(const cv::Mat& image, 
                                  const std::string& directory,
                                  const std::string& filename) {
    if (image.empty()) {
        logger->error("빈 이미지는 저장할 수 없음");
        return "";
    }
    
    std::lock_guard<std::mutex> lock(storage_mutex);
    
    try {
        // 디렉토리 생성 (775 권한)
        if (!ensureDirectory(directory)) {
            logger->error("디렉토리 생성 실패: {}", directory);
            return "";
        }
        
        // 전체 경로 생성
        std::string full_path = directory + "/" + filename;
        
        // JPEG 파라미터 설정
        std::vector<int> params;
        params.push_back(cv::IMWRITE_JPEG_QUALITY);
        params.push_back(jpeg_quality);
        
        // 이미지 저장
        if (cv::imwrite(full_path, image, params)) {
            logger->info("이미지 저장 완료: [파일명] {}, [경로] {}", 
                             filename, full_path);
            return full_path;
        } else {
            logger->error("이미지 저장 실패: {}", full_path);
            return "";
        }
        
    } catch (const std::exception& e) {
        logger->error("이미지 저장 중 예외 발생: {} - {}", filename, e.what());
        return "";
    }
}