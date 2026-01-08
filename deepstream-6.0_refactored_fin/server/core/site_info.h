#ifndef SITE_INFO_H
#define SITE_INFO_H

#include <exception>
#include <regex>
#include <string>

/**
 * @brief 사이트 정보 구조체
 * 
 * CAM ID와 교차로 정보를 관리하며, 신호역산을 위한 타겟 신호 정보 포함
 */
struct SiteInfo {
    enum class Mode {
        VOLTDB,                         // VoltDB에서 CAM ID 획득
        MANUAL,                         // Config 파일에서 CAM ID 직접 입력
        UNKNOWN
    };

    // 임시 CAM ID - 실제 CAM ID와 절대 겹치지 않는 값 사용
    static constexpr const char* PENDING_CAM_ID = "__PENDING_CAM_ID__";

    std::string ip_address;
    std::string spot_camr_id;           // CAM ID (신호 DB 조회용, 메타데이터에는 사용 안함)
    std::string spot_ints_id;           // 교차로 ID (VoltDB 모드에서만 사용)
    
    // 신호 코드
    int target_signal = 0;              // 타겟 신호 (통계/대기행렬/Special Site용)
    
    Mode mode = Mode::UNKNOWN;
    bool is_valid = false;
    bool supports_signal_calc = false;  // 신호역산 지원 여부

    /**
     * @brief CAM ID가 임시 값인지 확인
     */
    bool isPendingCamId() const {
        return spot_camr_id == PENDING_CAM_ID;
    }

    /**
     * @brief CAM ID 파싱 (VoltDB 모드용)
     * 형식: "교차로ID_방향1_방향2" (예: 8082_07_04, 한들사거리-북)
     * 교차로ID는 4-5자리 숫자
     * 
     * target_signal 결정 규칙:
     * - b가 짝수이면 b 사용
     * - 그렇지 않고 a가 홀수이면 a 사용
     * 
     * @note 파싱 실패 시 is_valid = false로 설정
     * @note std::stoi 예외 처리 포함 (24/7 운영 안정성)
     */
    void parseVoltDBFormat() {
        if (spot_camr_id.empty()) {
            is_valid = false;
            supports_signal_calc = false;
            return;
        }

        // 임시 CAM ID인 경우 특별 처리
        if (isPendingCamId()) {
            spot_ints_id = "0000";
            target_signal = 0;
            is_valid = true;
            supports_signal_calc = false;
            return;
        }

        // 정규식: 4-5자리숫자_2자리숫자_2자리숫자
        std::regex pattern(R"(^(\d{4,5})_(\d{2})_(\d{2})$)");
        std::smatch match;
        
        if (std::regex_match(spot_camr_id, match, pattern)) {
            try {
                spot_ints_id = match[1];
                int a = std::stoi(match[2]);
                int b = std::stoi(match[3]);
                
                // target_signal 계산 (통계/대기행렬/Special Site용)
                // Special Site 모드에서는 타겟 신호 ON=직진, OFF=좌회전으로 사용
                // regex로 이미 2자리 숫자 검증했으므로 0 체크 불필요
                if (b % 2 == 0) {
                    target_signal = b;
                } else if (a % 2 == 1) {
                    target_signal = a;
                } else {
                    // 둘 다 조건에 맞지 않으면 0 (신호역산 비활성)
                    target_signal = 0;
                }
                
                is_valid = true;
                supports_signal_calc = (target_signal > 0);
                
            } catch (const std::exception&) {
                is_valid = false;
                supports_signal_calc = false;
                spot_ints_id.clear();
                target_signal = 0;
            }
        } else {
            // 정규식 매칭 실패
            is_valid = false;
            supports_signal_calc = false;
        }
    }
    
    /**
     * @brief 디버그용 정보 출력
     * @return 사이트 정보 문자열
     */
    std::string toString() const {
        std::string result = "SiteInfo{";
        result += "cam_id=" + spot_camr_id;
        result += ", ints_id=" + spot_ints_id;
        result += ", target=" + std::to_string(target_signal);
        result += ", signal_calc=" + std::string(supports_signal_calc ? "enabled" : "disabled");
        result += ", valid=" + std::string(is_valid ? "true" : "false");
        result += "}";
        return result;
    }
};

#endif // SITE_INFO_H