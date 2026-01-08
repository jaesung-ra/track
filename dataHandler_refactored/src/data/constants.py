"""
Constants Module
=================
이 모듈은 데이터 처리 시스템에서 사용되는 모든 상수를 정의합니다.
- 데이터 타입 정의
- 필드 키 정의  
- 데이터베이스 테이블 명 정의
"""

# ============================================================================
# 데이터 타입 정의 (DataType)
# ============================================================================
# 용도: Redis 채널 및 데이터 라우팅 시 데이터 종류를 구분하기 위한 상수
# 사용처: receiver.py, router.py, sender.py 등에서 데이터 타입 분기 처리
# ============================================================================
class DataType(object):  
    # 차량 검지 데이터
    VEHICLE_2K               = "2k"              # 2K 해상도 차량 검지 데이터
    VEHICLE_4K               = "4k"              # 4K 해상도 차량 검지 데이터 (OCR 처리 완료)
    VEHICLE_RAW_4K           = "4k_R"            # 4K 해상도 차량 검지 원본 데이터 (OCR 미처리)
    MERGE                    = "merge"           # 2K와 4K 데이터 병합 결과
    
    # 보행자 검지 데이터
    PED_2K                   = "ped"             # 보행자 검지 데이터
    
    # 통계 데이터
    STATS                    = "stat"                 # 기본 통계 데이터
    STATS_APPROACH           = "approach_stats"       # 접근로별 통계
    STATS_TURN_TYPES         = "turn_types_stats"     # 회전 유형별 통계
    STATS_LANES              = "lanes_stats"          # 차로별 통계
    STATS_VEHICLE_TYPES      = "vehicle_types_stats"  # 차종별 통계
    
    # 대기행렬 데이터
    QUEUE                    = "queue"           # 기본 대기행렬 데이터
    QUEUE_APPROACH           = "approach_queue"  # 접근로별 대기행렬
    QUEUE_LANES              = "lanes_queue"     # 차로별 대기행렬
    
    # 돌발상황 데이터
    INCIDENT                 = "abn"             # 돌발상황 기본 데이터
    INCIDENT_START           = "abn_start"       # 돌발상황 시작 이벤트
    INCIDENT_END             = "abn_end"         # 돌발상황 종료 이벤트
    
    # SQLite 저장 데이터
    SQLITE_ST                = "st"              # 직진 차량 데이터
    SQLITE_LT                = "lt"              # 좌회전 차량 데이터
    SQLITE_RT                = "rt"              # 우회전 차량 데이터
    
    # 존재 감지 데이터
    PRESENCE_VH              = "pr_vh"           # 차량 존재 감지
    PRESENCE_WAIT            = "pr_wt"           # 대기 차량 존재 감지
    PRESENCE_CROSS           = "pr_cr"           # 횡단보도 존재 감지

# ============================================================================
# 필드 키 정의 (FieldKey)
# ============================================================================
# 용도: 데이터 딕셔너리의 키 값을 일관되게 관리
# ============================================================================
class FieldKey(object):  
    # ========================================================================
    # 공통 필드
    # ========================================================================
    SPOT_CAMR_ID             = "spot_camr_id"           # 카메라 ID (필수)
    CAR_TYPE                 = "kncr_cd"                # 차종 코드 (MOTOR, PCAR 등)
    LANE_NO                  = "lane_no"                # 차로 번호
    TURN_TYPE_CD             = "turn_type_cd"           # 회전 유형 코드 (11: 직진, 21: 좌회전, 31: 우회전, 41: 유턴)
    TURN_TIME                = "turn_dttn_unix_tm"      # 회전 검지 시각 (Unix timestamp)
    TURN_SPEED               = "turn_dttn_sped"         # 회전 구간 속도
    STOP_PASS_TIME           = "stln_pasg_unix_tm"      # 정지선 통과 시각 (Unix timestamp)
    STOP_PASS_SPEED          = "stln_dttn_sped"         # 정지선 통과 속도
    INTERVAL_SPEED           = "vhcl_sect_sped"         # 구간 평균 속도
    FIRST_DET_TIME           = "frst_obsrvn_unix_tm"    # 최초 검지 시각 (Unix timestamp)
    OBSERVE_TIME             = "vhcl_obsrvn_hr"         # 관측 시각 (초 단위)
    IMAGE_PATH_NAME          = "img_path_nm"            # 이미지 파일 경로
    IMAGE_FILE_NAME          = "img_file_nm"            # 이미지 파일명
    CAR_IMAGE_FILE_NAME      = "vhcl_img_file_nm"       # 차량 이미지 파일명
    PLATE_IMAGE_FILE_NAME    = "nopl_img_file_nm"       # 번호판 이미지 파일명
    PLATE_NUM                = "vhno_nm"                # 차량 번호 (OCR 결과)
    PLATE_YN                 = "vhno_dttn_yn"           # 번호판 검지 여부 (Y/N)
    CAR_ID_2K                = "vhcl_dttn_2k_id"        # 2K 차량 고유 ID
    CAR_ID_4K                = "vhcl_dttn_4k_id"        # 4K 차량 고유 ID
    CAR_ID                   = "vhcl_dttn_id"           # 병합 후 차량 고유 ID
    
    # ========================================================================
    # 통계 / 대기행렬 관련 필드
    # ========================================================================
    HR_TYPE_CD               = "hr_type_cd"             # 시간 유형 코드
    STAT_START_TIME          = "stats_bgng_unix_tm"     # 통계 시작 시각
    STAT_END_TIME            = "stats_end_unix_tm"      # 통계 종료 시각
    REMAIN_QUEUE             = "rmnn_queu_lngt"         # 잔여 대기행렬 길이
    MAX_QUEUE                = "max_queu_lngt"          # 최대 대기행렬 길이
    OCCUPY                   = "ocpn_rt"                # 점유율 (0~100)
    TOTAL_TRAVEL             = "totl_trvl"              # 총 통행량
    AVG_STOP_PASS_SPEED      = "avg_stln_dttn_sped"     # 평균 정지선 통과 속도
    AVG_INTERVAL_SPEED       = "avg_sect_sped"          # 평균 구간 속도
    AVG_DENSITY              = "avg_trfc_dnst"          # 평균 교통 밀도
    MIN_DENSITY              = "min_trfc_dnst"          # 최소 교통 밀도
    MAX_DENSITY              = "max_trfc_dnst"          # 최대 교통 밀도
    CRT_UNIX_TM              = "crt_unix_tm"            # 생성 시각
    AVG_LANE_OCCUPY          = "avg_lane_ocpn_rt"       # 평균 차로 점유율
    
    # 차종별 통행량
    MBUS_TRAVEL              = "kncr1_trvl"             # 중형버스 통행량
    LBUS_TRAVEL              = "kncr2_trvl"             # 대형버스 통행량
    PCAR_TRAVEL              = "kncr3_trvl"             # 승용차 통행량
    MOTOR_TRAVEL             = "kncr4_trvl"             # 오토바이 통행량
    MTRUCK_TRAVEL            = "kncr5_trvl"             # 중형트럭 통행량
    LTRUCK_TRAVEL            = "kncr6_trvl"             # 대형트럭 통행량
  
    # ========================================================================
    # 돌발상황 관련 필드
    # ========================================================================
    TRACE_ID                 = "trce_id"                # 추적 ID
    ABNORMAL_START_TIME      = "ocrn_unix_tm"           # 돌발상황 발생 시각
    ABNORMAL_PROC_TIME       = "prcs_unix_tm"           # 돌발상황 처리 시각
    ABNORMAL_END_TIME        = "end_unix_tm"            # 돌발상황 종료 시각
    ABNORMAL_TYPE            = "evet_type_cd"           # 돌발상황 유형 코드
  
    # ========================================================================
    # 보행자 관련 필드
    # ========================================================================
    PED_DET_TIME             = "dttn_unix_tm"           # 보행자 검지 시각
    PED_DIR                  = "drct_se_cd"             # 보행자 이동 방향 코드

    # ========================================================================
    # 존재 감지 관련 필드
    # ========================================================================
    PRESENCE_STATE           = "presence_state"         # 존재 감지 상태 (0/1)

    # ========================================================================
    # 내부 처리용 커스텀 필드
    # 주의: 이 필드들은 외부 시스템으로 전송되지 않고 내부 처리에만 사용됨
    # ========================================================================
    DATA_TYPE                = "data_type"              # 데이터 타입 (DataType 클래스 참조)
    UK                       = "unique_key"             # 고유 키 (해시값)
    UK_PLAIN                 = "unique_key_plain"       # 고유 키 (원본 텍스트)
    IMAGE_FILE               = "image_file"             # 이미지 파일 전체 경로
    IMAGE_BYTES_4K           = "image_data_4k"          # 4K 차량 이미지 바이너리 데이터
    IMAGE_BYTES_PLATE_4K     = "plate_image_data_4k"    # 4K 번호판 이미지 바이너리 데이터
    OBJ_ID                   = "object_id"              # 객체 원본 ID (해시 전)

# ============================================================================
# 데이터베이스 테이블명 정의 (Tables)
# ============================================================================
# 용도: VoltDB 데이터베이스 테이블명 관리
# 주의: 테이블명 변경 시 데이터베이스 스키마와 일치해야 함
# ============================================================================
class Tables(object):
    # 차량 검지 데이터 테이블
    TABLE_2K                 = "soitgrtmdtinfo_2K"      # 2K 해상도 차량 데이터
    TABLE_4K                 = "soitgrtmdtinfo_4K"      # 4K 해상도 차량 데이터
    TABLE_MERGE              = "soitgrtmdtinfo"         # 병합된 차량 데이터
    
    # 보행자 데이터 테이블
    TABLE_PED                = "soitgcwdtinfo"          # 보행자 검지 데이터
    
    # 통계 데이터 테이블
    TABLE_STATS_APPROACH     = "soitgaprdstats"         # 접근로별 통계
    TABLE_STATS_TURNTYPE     = "soitgturntypestats"     # 회전 유형별 통계
    TABLE_STATS_LANE         = "soitglanestats"         # 차로별 통계
    TABLE_STATS_VEHICLE_TYPE = "soitgkncrstats"         # 차종별 통계
    
    # 대기행렬 데이터 테이블
    TABLE_QUEUE_APPROACH     = "soitgaprdqueu"          # 접근로별 대기행렬
    TABLE_QUEUE_LANE         = "soitglanequeu"          # 차로별 대기행렬
    
    # 돌발상황 데이터 테이블
    TABLE_ABNORMAL           = "soitgunacevet"          # 돌발상황 이벤트