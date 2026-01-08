#include "signal_calculator.h"
#include <algorithm>
#include <ctime>
#include <sstream>

SignalCalculator::SignalCalculator(DataProvider* provider) 
    : data_provider_(provider) {
    logger = getLogger("DS_SignalCalc_log");
    logger->info("SignalCalculator 생성");
}

SignalCalculator::~SignalCalculator() {
    stop();
}

bool SignalCalculator::start(const SiteInfo& site_info, SignalChangeCallback callback) {
    if (!data_provider_ || !data_provider_->supportsSignalData()) {
        logger->error("신호 데이터를 지원하지 않는 DataProvider");
        return false;
    }
    
    if (site_info.target_signal <= 0) {
        logger->error("유효하지 않은 타겟 신호: {}", site_info.target_signal);
        return false;
    }
    
    std::lock_guard<std::mutex> lock(signal_mutex_);
    
    site_info_ = site_info;
    target_signal_ = site_info.target_signal;
    callback_ = callback;
    
    logger->info("신호역산 시작 - 교차로: {}, 타겟신호: {}", 
                 site_info_.spot_ints_id, target_signal_);
    
    // 초기 동기화
    int sleep_sec = syncWithServer();
    if (sleep_sec < 0) {
        logger->error("초기 서버 동기화 실패 - VoltDB 재연결 대기 중");
        return false;  // VoltDBSource가 백그라운드 재연결 처리 중
    }
    
    // 모니터링 스레드 시작
    running_ = true;
    signal_thread_ = std::thread(&SignalCalculator::signalMonitorThread, this);
    
    return true;
}

void SignalCalculator::stop() {
    logger->info("신호역산 중지");
    
    running_ = false;
    
    if (signal_thread_.joinable()) {
        signal_thread_.join();
    }
}

int SignalCalculator::syncWithServer() {
    logger->info("서버와 동기화 시작");
    
    int LC_CNT_before = LC_CNT_;
    
    // Phase Movement 정보 조회
    std::vector<int> phase_mvmt_info = 
        data_provider_->getMovementInfo(site_info_.spot_ints_id);
    
    // Phase Duration 정보 조회  
    std::vector<int> phase_duration_info = 
        data_provider_->getPhaseInfo(site_info_.spot_ints_id, LC_CNT_);
    
    // 조회 실패 처리 - VoltDBSource가 재연결 처리하므로 재시도하지 않음
    if (phase_mvmt_info.empty() || phase_duration_info.empty()) {
        if (!green_intervals_.empty()) {
            // 이전 데이터가 있으면 계속 사용
            logger->info("신호 DB 조회 실패, 이전 데이터로 계속 진행");
            if (LC_CNT_before == LC_CNT_) {
                LC_CNT_ = green_intervals_[0].first;
            }
            return calculateSleepTime(LC_CNT_);  // 이전 데이터로 계속
        }
        
        // 이전 데이터도 없으면 실패 (최초 실행 시)
        logger->warn("신호 데이터 없음 - Signal DB 연결 대기 중");
        return -1;  // VoltDBSource가 백그라운드에서 재연결 시도 중
    }
    
    // 신호 데이터 파싱
    if (!parseSignalData(phase_mvmt_info, phase_duration_info)) {
        logger->error("신호 데이터 파싱 실패");
        return -1;
    }
    
    // 현재 시점에서 다음 이벤트까지 시간 계산
    return calculateSleepTime(LC_CNT_);
}

bool SignalCalculator::parseSignalData(const std::vector<int>& mvmt_info, 
                                      const std::vector<int>& duration_info) {
    if (mvmt_info.size() != 16 || duration_info.size() != 16) {
        logger->error("잘못된 신호 데이터 크기 - movement: {}, duration: {}", 
                     mvmt_info.size(), duration_info.size());
        return false;
    }
    
    // A/B 링 분리
    std::vector<int> a_mvmt(mvmt_info.begin(), mvmt_info.begin() + 8);
    std::vector<int> b_mvmt(mvmt_info.begin() + 8, mvmt_info.end());
    std::vector<int> a_duration(duration_info.begin(), duration_info.begin() + 8);
    std::vector<int> b_duration(duration_info.begin() + 8, duration_info.end());
    
    // 타겟 신호가 어느 링에 있는지 확인
    bool isA = std::find(a_mvmt.begin(), a_mvmt.end(), target_signal_) != a_mvmt.end();
    bool isB = std::find(b_mvmt.begin(), b_mvmt.end(), target_signal_) != b_mvmt.end();
    
    if (isA && isB) {
        logger->error("타겟 신호가 양쪽 링에 모두 존재 - 잘못된 설정");
        return false;
    }
    
    if (!isA && !isB) {
        logger->error("타겟 신호 {}를 찾을 수 없음", target_signal_);
        return false;
    }
    
    const std::vector<int>& selected_mvmt = isA ? a_mvmt : b_mvmt;
    const std::vector<int>& selected_duration = isA ? a_duration : b_duration;
    
    // 로깅
    std::ostringstream mvmt_str, dur_str;
    mvmt_str << "Movement: [";
    for (size_t i = 0; i < selected_mvmt.size(); ++i) {
        mvmt_str << selected_mvmt[i];
        if (i < selected_mvmt.size() - 1) mvmt_str << ", ";
    }
    mvmt_str << "]";
    
    dur_str << "Duration: [";
    for (size_t i = 0; i < selected_duration.size(); ++i) {
        dur_str << selected_duration[i];
        if (i < selected_duration.size() - 1) dur_str << ", ";
    }
    dur_str << "]";
    
    logger->info("LC_CNT: {}", LC_CNT_);
    logger->info("{}", mvmt_str.str());
    logger->info("{}", dur_str.str());
    
    // 녹색 신호 구간 계산
    green_intervals_.clear();
    int cur_time = 0;
    
    for (int i = 0; i < 8; i++) {
        if (selected_mvmt[i] == target_signal_) {
            int start = cur_time;
            int duration = selected_duration[i];
            int end = start + duration;
            
            // 연속된 구간 병합
            if (!green_intervals_.empty() && green_intervals_.back().second == start) {
                green_intervals_.back().second = end;
            } else {
                green_intervals_.emplace_back(start, end);
            }
        }
        cur_time += selected_duration[i];
    }
    
    cycle_duration_ = cur_time;
    
    logger->info("신호 주기: {}초, 녹색 구간 수: {}", 
                 cycle_duration_, green_intervals_.size());
    
    return true;
}

int SignalCalculator::calculateSleepTime(int LC_CNT) {
    int sleep_sec = 0;
    current_interval_idx_ = 0;
    
    // 현재 녹색 신호 구간 찾기
    for (size_t i = 0; i < green_intervals_.size(); ++i) {
        int on = green_intervals_[i].first;
        int off = green_intervals_[i].second;
        
        if (LC_CNT >= on && LC_CNT < off) {
            // 현재 녹색 신호 중
            sleep_sec = off - LC_CNT;
            current_interval_idx_ = i;
            signal_on_ = true;
            return sleep_sec;
        }
        
        if (LC_CNT < on) {
            // 다음 녹색 신호 대기
            current_interval_idx_ = i;
            sleep_sec = on - LC_CNT;
            signal_on_ = false;
            return sleep_sec;
        }
    }
    
    // 다음 주기의 첫 녹색 신호 대기
    sleep_sec = cycle_duration_ - LC_CNT + green_intervals_[0].first;
    current_interval_idx_ = 0;
    signal_on_ = false;
    
    return sleep_sec;
}

void SignalCalculator::signalMonitorThread() {
    logger->info("신호 모니터링 스레드 시작");
    
    int prev_on_time = std::time(nullptr);
    std::map<int, int> residual_cars;
    int cycle_count = 0;
    
    // 초기 동기화 결과에 따른 대기
    int sleep_sec = calculateSleepTime(LC_CNT_);
    interruptibleSleep(sleep_sec);
    
    // 초기 상태 처리
    if (LC_CNT_ >= green_intervals_[current_interval_idx_].first && 
        LC_CNT_ < green_intervals_[current_interval_idx_].second) {
        processRedSignal(residual_cars);
        
        if (green_intervals_.size() == 1) {
            interruptibleSleep(cycle_duration_ - green_intervals_[0].second + green_intervals_[0].first);
        } else {
            interruptibleSleep(green_intervals_[1].first - green_intervals_[0].second);
            current_interval_idx_++;
        }
    }
    
    // 메인 루프
    while (running_.load()) {
        // 3주기마다 재동기화
        if (cycle_count == SYNC_INTERVAL_CYCLES) {
            sleep_sec = syncWithServer();
            
            if (sleep_sec != -1) {
                if (LC_CNT_ >= green_intervals_[current_interval_idx_].first && 
                    LC_CNT_ < green_intervals_[current_interval_idx_].second) {
                    processGreenSignal(prev_on_time, residual_cars);
                } else if (LC_CNT_ >= green_intervals_[current_interval_idx_].second) {
                    processGreenSignal(prev_on_time, residual_cars);
                    processRedSignal(residual_cars);
                }
                
                interruptibleSleep(sleep_sec);
                
                if (LC_CNT_ >= green_intervals_[current_interval_idx_].first && 
                    LC_CNT_ < green_intervals_[current_interval_idx_].second) {
                    processRedSignal(residual_cars);
                    
                    if (green_intervals_.size() == 1) {
                        interruptibleSleep(cycle_duration_ - green_intervals_[0].second + green_intervals_[0].first);
                    } else {
                        interruptibleSleep(green_intervals_[1].first - green_intervals_[0].second);
                        current_interval_idx_++;
                    }
                }
            }
            cycle_count = 0;
        }
        
        int on_time = green_intervals_[current_interval_idx_].first;
        int off_time = green_intervals_[current_interval_idx_].second;
        
        // 녹색 신호 시작
        processGreenSignal(prev_on_time, residual_cars);
        
        // 녹색 신호 지속 시간 대기
        int wait_time = off_time - on_time + prev_on_time - std::time(nullptr);
        if (wait_time > 0) {
            interruptibleSleep(wait_time);
        }
        
        // 적색 신호 시작
        processRedSignal(residual_cars);
        
        // 다음 녹색 신호까지 대기
        int next_idx = (current_interval_idx_ + 1) % green_intervals_.size();
        if (green_intervals_[next_idx].first > off_time) {
            interruptibleSleep(green_intervals_[next_idx].first - off_time);
        } else {
            interruptibleSleep(cycle_duration_ - off_time + green_intervals_[next_idx].first);
        }
        
        current_interval_idx_ = next_idx;
        
        // 주기 완료 체크
        if (current_interval_idx_ == 0) {
            cycle_count++;
        }
    }
    
    logger->info("신호 모니터링 스레드 종료");
}

void SignalCalculator::processGreenSignal(int& prev_on_time, 
                                         std::map<int, int>& residual_cars) {
    logger->info("신호 변경: 녹색 (GREEN) - 타겟신호: {}", target_signal_);
    
    signal_on_ = true;
    
    // 콜백을 통해 외부에 신호 변경 알림
    if (callback_) {
        SignalChangeEvent event;
        event.type = SignalChangeEvent::Type::GREEN_ON;
        event.timestamp = std::time(nullptr);
        event.phase = 1;  // 녹색
        event.residual_cars = residual_cars;
        event.duration_seconds = green_intervals_[current_interval_idx_].second - 
                               green_intervals_[current_interval_idx_].first;
        
        callback_(event);
    }
    
    prev_on_time = std::time(nullptr);
}

void SignalCalculator::processRedSignal(std::map<int, int>& residual_cars) {
    logger->info("신호 변경: 적색 (RED) - 타겟신호: {}", target_signal_);
    
    signal_on_ = false;
    
    // 콜백을 통해 외부에 신호 변경 알림
    if (callback_) {
        SignalChangeEvent event;
        event.type = SignalChangeEvent::Type::GREEN_OFF;
        event.timestamp = std::time(nullptr);
        event.phase = 0;  // 적색
        event.residual_cars = residual_cars;
        
        // 다음 녹색까지의 시간 계산
        int next_idx = (current_interval_idx_ + 1) % green_intervals_.size();
        if (green_intervals_[next_idx].first > green_intervals_[current_interval_idx_].second) {
            event.duration_seconds = green_intervals_[next_idx].first - 
                                   green_intervals_[current_interval_idx_].second;
        } else {
            event.duration_seconds = cycle_duration_ - green_intervals_[current_interval_idx_].second + 
                                   green_intervals_[next_idx].first;
        }
        
        callback_(event);
    }
}

bool SignalCalculator::isGreenSignal() const {
    return signal_on_.load();
}

int SignalCalculator::getTimeToNextChange() const {
    std::lock_guard<std::mutex> lock(signal_mutex_);
    
    if (green_intervals_.empty()) return -1;
    
    int current_time = LC_CNT_;
    
    if (signal_on_) {
        // 현재 녹색 - 적색까지 시간
        return green_intervals_[current_interval_idx_].second - current_time;
    } else {
        // 현재 적색 - 다음 녹색까지 시간
        int next_green = green_intervals_[current_interval_idx_].first;
        if (next_green > current_time) {
            return next_green - current_time;
        } else {
            return cycle_duration_ - current_time + next_green;
        }
    }
}

int SignalCalculator::getCycleDuration() const {
    std::lock_guard<std::mutex> lock(signal_mutex_);
    return cycle_duration_;
}

int SignalCalculator::getCurrentLCCNT() const {
    std::lock_guard<std::mutex> lock(signal_mutex_);
    return LC_CNT_;
}

int SignalCalculator::forceSync() {
    std::lock_guard<std::mutex> lock(signal_mutex_);
    return syncWithServer();
}

void SignalCalculator::interruptibleSleep(int seconds) {
    for (int i = 0; i < seconds && running_.load(); i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}