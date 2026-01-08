#ifndef SIGNAL_CALCULATOR_H
#define SIGNAL_CALCULATOR_H

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../core/data_provider.h"
#include "../core/signal_types.h"
#include "../core/site_info.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

/**
 * @brief 신호역산 및 신호 변경 감지 클래스
 * 
 * VoltDB에서 신호 정보를 받아 현재 신호 상태를 계산하고
 * 신호 변경 이벤트를 감지하여 콜백 호출
 */
class SignalCalculator {
public:
    // 신호 변경 이벤트 콜백 타입
    using SignalChangeCallback = std::function<void(const SignalChangeEvent&)>;

private:
    // 의존성
    DataProvider* data_provider_;
    
    // 사이트 정보
    SiteInfo site_info_;
    int target_signal_;
    
    // 신호 정보
    std::vector<std::pair<int, int>> green_intervals_;  // (시작, 종료) 시간
    int cycle_duration_ = 0;
    int current_interval_idx_ = 0;
    int LC_CNT_ = 0;
    
    // 스레드 관련
    std::thread signal_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> signal_on_{false};
    mutable std::mutex signal_mutex_;
    
    // 콜백
    SignalChangeCallback callback_;
    
    // 동기화 관련
    static constexpr int SYNC_INTERVAL_CYCLES = 3;  // 3주기마다 재동기화
    
    // 로거
    std::shared_ptr<spdlog::logger> logger = nullptr;
    
    // 내부 메서드
    int syncWithServer();
    void signalMonitorThread();
    int calculateSleepTime(int LC_CNT);
    void processGreenSignal(int& prev_on_time, std::map<int, int>& residual_cars);
    void processRedSignal(std::map<int, int>& residual_cars);
    bool parseSignalData(const std::vector<int>& mvmt_info, 
                        const std::vector<int>& duration_info);
    void interruptibleSleep(int seconds);

public:
    SignalCalculator(DataProvider* provider);
    ~SignalCalculator();
    
    /**
     * @brief 신호역산 시작
     * @param site_info 사이트 정보 (교차로 ID, 타겟 신호 포함)
     * @param callback 신호 변경 시 호출될 콜백
     * @return 성공 시 true
     */
    bool start(const SiteInfo& site_info, SignalChangeCallback callback);
    
    /**
     * @brief 신호역산 중지
     */
    void stop();
    
    /**
     * @brief 현재 신호 상태 조회 (타겟 신호)
     * @return 현재 녹색 신호 여부
     */
    bool isGreenSignal() const;
    
    /**
     * @brief Special Site용 방향 결정
     * @return 타겟 신호 ON이면 11(직진), OFF면 21(좌회전)
     * 
     * Special Site 모드에서 사용:
     * - 타겟 신호가 녹색(ON) → 직진
     * - 타겟 신호가 적색(OFF) → 좌회전
     */
    int getDirectionForSpecialSite() const {
        return isGreenSignal() ? 11 : 21;
    }
    
    /**
     * @brief 다음 신호 변경까지 남은 시간
     * @return 남은 시간 (초)
     */
    int getTimeToNextChange() const;
    
    /**
     * @brief 현재 신호 주기 정보
     * @return 주기 시간 (초)
     */
    int getCycleDuration() const;
    
    /**
     * @brief 현재 LC_CNT 값
     */
    int getCurrentLCCNT() const;
    
    /**
     * @brief 수동으로 서버와 동기화
     * @return 성공 시 0, 실패 시 음수
     */
    int forceSync();
};

#endif // SIGNAL_CALCULATOR_H