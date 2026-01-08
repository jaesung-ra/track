/*
 * Copyright (c) 2018-2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

// GStreamer 관련
#include <gst/gst.h>            // GStreamer 핵심 라이브러리

// C 표준 라이브러리
#include <string.h>             // C 문자열 처리 함수 (strcpy, strcmp 등)
#include <math.h>               // 수학 함수 (sqrt, pow, abs 등)
#include <stdlib.h>             // 표준 유틸리티 함수 (malloc, exit 등)
#include <cstring>              // C++ 스타일 C 문자열 함수 (memcpy, memset 등)
#include <ctime>                // 시간 관련 함수 (time, localtime 등)

// 시스템/파일 관련
#include <sys/stat.h>           // 파일 상태 정보 (stat, mkdir 등)
#include <sys/types.h>          // 시스템 데이터 타입 정의
#include <fcntl.h>              // 파일 제어 옵션 (open, O_RDONLY 등)
#include <unistd.h>             // POSIX 운영체제 API (close, read, write 등)
#include <errno.h>              // 에러 번호 정의 (errno, EINVAL 등)

// C++ 표준 라이브러리 - 컨테이너
#include <iostream>             // 표준 입출력 스트림 (cout, cerr 등)
#include <map>                  // 맵 컨테이너 (키-값 쌍 저장)
#include <vector>               // 동적 배열 컨테이너
#include <deque>                // 양방향 큐 컨테이너 (객체 추적 이력)
#include <queue>                // FIFO 큐 컨테이너 (데이터 버퍼링)

// C++ 표준 라이브러리 - 유틸리티
#include <memory>               // 스마트 포인터 (shared_ptr, unique_ptr)
#include <mutex>                // 뮤텍스 동기화 (std::mutex, lock_guard)
#include <chrono>               // 시간 측정 및 조작 (시간 간격 계산)
#include <thread>               // 멀티스레딩 지원 (std::thread)
#include <algorithm>            // STL 알고리즘 (std::max, std::min, std::find 등)

// C++ 표준 라이브러리 - 입출력
#include <sstream>              // 문자열 스트림 (stringstream - 메타데이터 생성)
#include <fstream>              // 파일 스트림 (파일 읽기/쓰기)
#include <iomanip>              // 입출력 조작자 (setw, setprecision 등)

// 외부 라이브러리
#include <hiredis/hiredis.h>    // Redis C 클라이언트 라이브러리

// DeepStream 애플리케이션 헤더
#include "deepstream_app.h"

// 프로젝트 모듈 헤더
#include "analytics/statistics/stats_generator.h"         // 교통 통계 생성 및 집계 모듈
#include "common/common_types.h"                          // 공통 타입 정의
#include "common/object_data.h"                           // 객체 데이터 구조체 정의
#include "data/redis/channel_types.h"                     // Redis 채널 타입 정의
#include "data/redis/redis_client.h"                      // Redis 클라이언트 클래스
#include "data/sqlite/sqlite_handler.h"                   // SQLite 데이터베이스 핸들러
#include "detection/pedestrian/pedestrian_processor.h"    // 보행자 검출 처리기
#include "detection/special/special_site_adapter.h"       // Special Site 어댑터
#include "detection/vehicle/vehicle_processor_2k.h"       // 차량 검출 처리기 (2K)
#include "detection/vehicle/vehicle_processor_4k.h"       // 차량 검출 처리기 (4K)
#include "image/image_cropper.h"                          // 이미지 크롭 모듈
#include "image/image_storage.h"                          // 이미지 저장 모듈
#include "monitoring/car_presence.h"                      // 차량 Presence 모듈
#include "monitoring/pedestrian_presence.h"               // 보행자 Presence 모듈
#include "roi_module/roi_handler.h"                       // ROI 처리 모듈
#include "server/manager/system_manager.h"                // 시스템 전체 관리 및 조정
#include "utils/config_manager.h"                         // 설정 관리자

// NVIDIA 라이브러리
#include "nvbufsurface.h"                                 // NVIDIA 버퍼 서피스 API
#include "nvbufsurftransform.h"                           // NVIDIA 버퍼 변환 API

// OpenCV 헤더
#include "opencv2/opencv.hpp"                             // OpenCV 핵심 기능
#include "opencv2/imgproc/imgproc.hpp"                    // OpenCV 이미지 처리
#include "opencv2/highgui/highgui.hpp"                    // OpenCV GUI 및 이미지 I/O

// 로거 헤더
#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif

// Named pipe for tracker communication
#define DELETED_ID_PIPE "/tmp/deleted_tracker_pipe"

// Global variables
static std::shared_ptr<spdlog::logger> logger;
static std::map<int, obj_data> det_obj;
static std::mutex global_mutex;
static int previous_time = -1;

// ConfigManager 캐시 변수
static bool cached_vehicle_2k_enabled = false;
static bool cached_vehicle_4k_enabled = false;
static bool cached_pedestrian_meta_enabled = false;
static bool cached_statistics_enabled = false;
static bool cached_config_initialized = false;

// Module instances
static std::unique_ptr<ROIHandler> roi_handler;
static std::unique_ptr<SystemManager> system_manager;
static std::unique_ptr<VehicleProcessor2K> vehicle_processor_2k;
static std::unique_ptr<VehicleProcessor4K> vehicle_processor_4k;
static std::unique_ptr<PedestrianProcessor> pedestrian_processor;
static std::unique_ptr<ImageCropper> image_cropper;
static std::unique_ptr<ImageStorage> image_storage;

// Named pipe for deleted IDs
static int read_fd = -1;

GST_DEBUG_CATEGORY_EXTERN(NVDS_APP);

GQuark _dsmeta_quark;

#define CEIL(a, b) ((a + b - 1) / b)

// Forward declarations
static bool initializeModules(AppCtx *appCtx);
static void cleanupModules();
static void cacheProcessMetaConfigs();
static void discardDeletedId();

/**
 * @brief    Add the (nvmsgconv->nvmsgbroker) sink-bin to the
 *                 overall DS pipeline (if any configured) and link the same to
 *                 common_elements.tee (This tee connects
 *                 the common analytics path to Tiler/display-sink and
 *                 to configured broker sink if any)
 *                 NOTE: This API shall return TRUE if there are no
 *                 broker sinks to add to pipeline
 *
 * @param    appCtx [IN]
 * @return TRUE if succussful; FALSE otherwise
 */
static gboolean add_and_link_broker_sink(AppCtx *appCtx);

/**
 * @brief    Checks if there are any [sink] groups
 *                 configured for source_id=provided source_id
 *                 NOTE: source_id key and this API is valid only when we
 *                 disable [tiler] and thus use demuxer for individual
 *                 stream out
 * @param    config [IN] The DS Pipeline configuration struct
 * @param    source_id [IN] Source ID for which a specific [sink]
 *                 group is searched for
 */
static gboolean is_sink_available_for_source_id(NvDsConfig *config, guint source_id);

static void
process_meta(AppCtx *appCtx, NvDsBatchMeta *batch_meta, guint index, GstBuffer *buf);

/**
 * callback function to receive messages from components
 * in the pipeline.
 */
static gboolean
bus_callback(GstBus *bus, GstMessage *message, gpointer data)
{
    AppCtx *appCtx = (AppCtx *)data;
    GST_CAT_DEBUG(NVDS_APP,
                  "Received message on bus: source %s, msg_type %s",
                  GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message));
    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_INFO:
    {
        GError *error = NULL;
        gchar *debuginfo = NULL;
        gst_message_parse_info(message, &error, &debuginfo);
        g_printerr("INFO from %s: %s\n",
                   GST_OBJECT_NAME(message->src), error->message);
        if (debuginfo)
        {
            g_printerr("Debug info: %s\n", debuginfo);
        }
        g_error_free(error);
        g_free(debuginfo);
        break;
    }
    case GST_MESSAGE_WARNING:
    {
        GError *error = NULL;
        gchar *debuginfo = NULL;
        gst_message_parse_warning(message, &error, &debuginfo);
        g_printerr("WARNING from %s: %s\n",
                   GST_OBJECT_NAME(message->src), error->message);
        if (debuginfo)
        {
            g_printerr("Debug info: %s\n", debuginfo);
        }
        g_error_free(error);
        g_free(debuginfo);
        break;
    }
    case GST_MESSAGE_ERROR:
    {
        GError *error = NULL;
        gchar *debuginfo = NULL;
        guint i = 0;
        gst_message_parse_error(message, &error, &debuginfo);
        g_printerr("ERROR from %s: %s\n",
                   GST_OBJECT_NAME(message->src), error->message);
        if (debuginfo)
        {
            g_printerr("Debug info: %s\n", debuginfo);
        }

        NvDsSrcParentBin *bin = &appCtx->pipeline.multi_src_bin;
        GstElement *msg_src_elem = (GstElement *)GST_MESSAGE_SRC(message);
        gboolean bin_found = FALSE;
        /* Find the source bin which generated the error. */
        while (msg_src_elem && !bin_found)
        {
            for (i = 0; i < bin->num_bins && !bin_found; i++)
            {
                if (bin->sub_bins[i].src_elem == msg_src_elem ||
                    bin->sub_bins[i].bin == msg_src_elem)
                {
                    bin_found = TRUE;
                    break;
                }
            }
            msg_src_elem = GST_ELEMENT_PARENT(msg_src_elem);
        }

        if ((i != bin->num_bins) &&
            (appCtx->config.multi_source_config[0].type == NV_DS_SOURCE_RTSP))
        {
            // Error from one of RTSP source.
            NvDsSrcBin *subBin = &bin->sub_bins[i];

            if (!subBin->reconfiguring ||
                g_strrstr(debuginfo, "500 (Internal Server Error)"))
            {
                subBin->reconfiguring = TRUE;
                g_timeout_add(0, reset_source_pipeline, subBin);
            }
            g_error_free(error);
            g_free(debuginfo);
            return TRUE;
        }

        if (appCtx->config.multi_source_config[0].type == NV_DS_SOURCE_CAMERA_V4L2)
        {
            if (g_strrstr(debuginfo, "reason not-negotiated (-4)"))
            {
                NVGSTDS_INFO_MSG_V("incorrect camera parameters provided, please provide supported resolution and frame rate\n");
            }

            if (g_strrstr(debuginfo, "Buffer pool activation failed"))
            {
                NVGSTDS_INFO_MSG_V("usb bandwidth might be saturated\n");
            }
        }

        g_error_free(error);
        g_free(debuginfo);
        appCtx->return_value = -1;
        appCtx->quit = TRUE;
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
    {
        GstState oldstate, newstate;
        gst_message_parse_state_changed(message, &oldstate, &newstate, NULL);
        if (GST_ELEMENT(GST_MESSAGE_SRC(message)) == appCtx->pipeline.pipeline)
        {
            switch (newstate)
            {
            case GST_STATE_PLAYING:
                NVGSTDS_INFO_MSG_V("Pipeline running\n");
                GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(appCtx->pipeline.pipeline), GST_DEBUG_GRAPH_SHOW_ALL,
                                                  "ds-app-playing");
                break;
            case GST_STATE_PAUSED:
                if (oldstate == GST_STATE_PLAYING)
                {
                    NVGSTDS_INFO_MSG_V("Pipeline paused\n");
                }
                break;
            case GST_STATE_READY:
                GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(appCtx->pipeline.pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "ds-app-ready");
                if (oldstate == GST_STATE_NULL)
                {
                    NVGSTDS_INFO_MSG_V("Pipeline ready\n");
                }
                else
                {
                    NVGSTDS_INFO_MSG_V("Pipeline stopped\n");
                }
                break;
            case GST_STATE_NULL:
                g_mutex_lock(&appCtx->app_lock);
                g_cond_broadcast(&appCtx->app_cond);
                g_mutex_unlock(&appCtx->app_lock);
                break;
            default:
                break;
            }
        }
        break;
    }
    case GST_MESSAGE_EOS:
    {
        /*
             * In normal scenario, this would use g_main_loop_quit() to exit the
             * loop and release the resources. Since this application might be
             * running multiple pipelines through configuration files, it should wait
             * till all pipelines are done.
             */
        NVGSTDS_INFO_MSG_V("Received EOS. Exiting ...\n");
        appCtx->quit = TRUE;
        return FALSE;
        break;
    }
    default:
        break;
    }
    return TRUE;
}

/**
 * Function which processes the inferred buffer and its metadata.
 * It also gives opportunity to attach application specific
 * metadata (e.g. clock, analytics output etc.).
 */
static void
process_buffer(GstBuffer *buf, AppCtx *appCtx, guint index)
{
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta)
    {
        NVGSTDS_WARN_MSG_V("Batch meta not found for buffer %p", buf);
        return;
    }
    process_meta(appCtx, batch_meta, index, buf);
    NvDsInstanceData *data = &appCtx->instance_data[index];

    data->frame_num++;

    return;
}

/**
 * Buffer probe function to get the results of primary infer.
 * Here it demonstrates the use by dumping bounding box coordinates in
 * kitti format.
 */
static GstPadProbeReturn
gie_primary_processing_done_buf_prob(GstPad *pad, GstPadProbeInfo *info, gpointer u_data)
{
    GstBuffer *buf = (GstBuffer *)info->data;
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta)
    {
        NVGSTDS_WARN_MSG_V("Batch meta not found for buffer %p", buf);
        return GST_PAD_PROBE_OK;
    }


    return GST_PAD_PROBE_OK;
}

/**
 * Probe function to get results after all inferences(Primary + Secondary)
 * are done. This will be just before OSD or sink (in case OSD is disabled).
 */
static GstPadProbeReturn
gie_processing_done_buf_prob(GstPad *pad, GstPadProbeInfo *info, gpointer u_data)
{

    GstBuffer *buf = (GstBuffer *)info->data;
    NvDsInstanceBin *bin = (NvDsInstanceBin *)u_data;
    guint index = bin->index;
    AppCtx *appCtx = bin->appCtx;

    if (gst_buffer_is_writable(buf))
        process_buffer(buf, appCtx, index);
    return GST_PAD_PROBE_OK;
}

/**
 * Buffer probe function after tracker.
 */
static GstPadProbeReturn
analytics_done_buf_prob(GstPad *pad, GstPadProbeInfo *info, gpointer u_data)
{
    GstBuffer *buf = (GstBuffer *)info->data;
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta)
    {
        NVGSTDS_WARN_MSG_V("Batch meta not found for buffer %p", buf);
        return GST_PAD_PROBE_OK;
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
latency_measurement_buf_prob(GstPad *pad, GstPadProbeInfo *info, gpointer u_data)
{
    AppCtx *appCtx = (AppCtx *)u_data;
    guint i = 0, num_sources_in_batch = 0;
    if (nvds_enable_latency_measurement)
    {
        GstBuffer *buf = (GstBuffer *)info->data;
        NvDsFrameLatencyInfo *latency_info = NULL;
        g_mutex_lock(&appCtx->latency_lock);
        latency_info = appCtx->latency_info;
        num_sources_in_batch = nvds_measure_buffer_latency(buf, latency_info);

        for (i = 0; i < num_sources_in_batch; i++)
        {
            g_print("Source id = %d Frame_num = %d Frame latency = %lf (ms) \n",
                    latency_info[i].source_id,
                    latency_info[i].frame_num,
                    latency_info[i].latency);
        }
        g_mutex_unlock(&appCtx->latency_lock);
    }

    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
demux_latency_measurement_buf_prob(GstPad *pad, GstPadProbeInfo *info, gpointer u_data)
{
    AppCtx *appCtx = (AppCtx *)u_data;
    guint i = 0, num_sources_in_batch = 0;
    if (nvds_enable_latency_measurement)
    {
        GstBuffer *buf = (GstBuffer *)info->data;
        NvDsFrameLatencyInfo *latency_info = NULL;
        g_mutex_lock(&appCtx->latency_lock);
        latency_info = appCtx->latency_info;
        num_sources_in_batch = nvds_measure_buffer_latency(buf, latency_info);

        for (i = 0; i < num_sources_in_batch; i++)
        {
            g_print("Source id = %d Frame_num = %d Frame latency = %lf (ms) \n",
                    latency_info[i].source_id,
                    latency_info[i].frame_num,
                    latency_info[i].latency);
        }
        g_mutex_unlock(&appCtx->latency_lock);
    }

    return GST_PAD_PROBE_OK;
}

static gboolean
add_and_link_broker_sink(AppCtx *appCtx)
{
    NvDsConfig *config = &appCtx->config;
    /** Only first instance_bin broker sink
     * employed as there's only one analytics path for N sources
     * NOTE: There shall be only one [sink] group
     * with type=6 (NV_DS_SINK_MSG_CONV_BROKER)
     * a) Multiple of them does not make sense as we have only
     * one analytics pipe generating the data for broker sink
     * b) If Multiple broker sinks are configured by the user
     * in config file, only the first in the order of
     * appearance will be considered
     * and others shall be ignored
     * c) Ideally it should be documented (or obvious) that:
     * multiple [sink] groups with type=6 (NV_DS_SINK_MSG_CONV_BROKER)
     * is invalid
     */
    NvDsInstanceBin *instance_bin = &appCtx->pipeline.instance_bins[0];
    NvDsPipeline *pipeline = &appCtx->pipeline;

    for (guint i = 0; i < config->num_sink_sub_bins; i++)
    {
        if (config->sink_bin_sub_bin_config[i].type == NV_DS_SINK_MSG_CONV_BROKER)
        {
            if (!pipeline->common_elements.tee)
            {
                NVGSTDS_ERR_MSG_V("%s failed; broker added without analytics; check config file\n", __func__);
                return FALSE;
            }
            /** add the broker sink bin to pipeline */
            if (!gst_bin_add(GST_BIN(pipeline->pipeline), instance_bin->sink_bin.sub_bins[i].bin))
            {
                return FALSE;
            }
            /** link the broker sink bin to the common_elements tee
             * (The tee after nvinfer -> tracker (optional) -> sgies (optional) block) */
            if (!link_element_to_tee_src_pad(pipeline->common_elements.tee, instance_bin->sink_bin.sub_bins[i].bin))
            {
                return FALSE;
            }
        }
    }
    return TRUE;
}

static gboolean
create_demux_pipeline(AppCtx *appCtx, guint index)
{
    gboolean ret = FALSE;
    NvDsConfig *config = &appCtx->config;
    NvDsInstanceBin *instance_bin = &appCtx->pipeline.demux_instance_bins[index];
    GstElement *last_elem;
    gchar elem_name[32];

    instance_bin->index = index;
    instance_bin->appCtx = appCtx;

    g_snprintf(elem_name, 32, "processing_demux_bin_%d", index);
    instance_bin->bin = gst_bin_new(elem_name);

    if (!create_demux_sink_bin(config->num_sink_sub_bins,
                               config->sink_bin_sub_bin_config, &instance_bin->demux_sink_bin,
                               config->sink_bin_sub_bin_config[index].source_id))
    {
        goto done;
    }

    gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->demux_sink_bin.bin);
    last_elem = instance_bin->demux_sink_bin.bin;

    if (config->osd_config.enable)
    {
        if (!create_osd_bin(&config->osd_config, &instance_bin->osd_bin))
        {
            goto done;
        }

        gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->osd_bin.bin);

        NVGSTDS_LINK_ELEMENT(instance_bin->osd_bin.bin, last_elem);

        last_elem = instance_bin->osd_bin.bin;
    }

    NVGSTDS_BIN_ADD_GHOST_PAD(instance_bin->bin, last_elem, "sink");
    if (config->osd_config.enable)
    {
        NVGSTDS_ELEM_ADD_PROBE(instance_bin->all_bbox_buffer_probe_id,
                               instance_bin->osd_bin.nvosd, "sink",
                               gie_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER, instance_bin);
    }
    else
    {
        NVGSTDS_ELEM_ADD_PROBE(instance_bin->all_bbox_buffer_probe_id,
                               instance_bin->demux_sink_bin.bin, "sink",
                               gie_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER, instance_bin);
    }

    ret = TRUE;
done:
    if (!ret)
    {
        NVGSTDS_ERR_MSG_V("%s failed", __func__);
    }
    return ret;
}

/**
 * Function to add components to pipeline which are dependent on number
 * of streams. These components work on single buffer. If tiling is being
 * used then single instance will be created otherwise < N > such instances
 * will be created for < N > streams
 */
static gboolean
create_processing_instance(AppCtx *appCtx, guint index)
{
    gboolean ret = FALSE;
    NvDsConfig *config = &appCtx->config;
    NvDsInstanceBin *instance_bin = &appCtx->pipeline.instance_bins[index];
    GstElement *last_elem;
    gchar elem_name[32];

    instance_bin->index = index;
    instance_bin->appCtx = appCtx;

    g_snprintf(elem_name, 32, "processing_bin_%d", index);
    instance_bin->bin = gst_bin_new(elem_name);

    if (!create_sink_bin(config->num_sink_sub_bins,
                         config->sink_bin_sub_bin_config, &instance_bin->sink_bin, index))
    {
        goto done;
    }

    gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->sink_bin.bin);
    last_elem = instance_bin->sink_bin.bin;

    if (config->osd_config.enable)
    {
        if (!create_osd_bin(&config->osd_config, &instance_bin->osd_bin))
        {
            goto done;
        }

        gst_bin_add(GST_BIN(instance_bin->bin), instance_bin->osd_bin.bin);

        NVGSTDS_LINK_ELEMENT(instance_bin->osd_bin.bin, last_elem);

        last_elem = instance_bin->osd_bin.bin;
    }

    NVGSTDS_BIN_ADD_GHOST_PAD(instance_bin->bin, last_elem, "sink");
    if (config->osd_config.enable)
    {
        NVGSTDS_ELEM_ADD_PROBE(instance_bin->all_bbox_buffer_probe_id,
                               instance_bin->osd_bin.nvosd, "sink",
                               gie_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER, instance_bin);
    }
    else
    {
        NVGSTDS_ELEM_ADD_PROBE(instance_bin->all_bbox_buffer_probe_id,
                               instance_bin->sink_bin.bin, "sink",
                               gie_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER, instance_bin);
    }

    ret = TRUE;
done:
    if (!ret)
    {
        NVGSTDS_ERR_MSG_V("%s failed", __func__);
    }
    return ret;
}

/**
 * Function to create common elements(Primary infer, tracker, secondary infer)
 * of the pipeline. These components operate on muxed data from all the
 * streams. So they are independent of number of streams in the pipeline.
 */
static gboolean
create_common_elements(NvDsConfig *config, NvDsPipeline *pipeline,
                       GstElement **sink_elem, GstElement **src_elem,
                       bbox_generated_callback bbox_generated_post_analytics_cb)
{
    gboolean ret = FALSE;
    *sink_elem = *src_elem = NULL;

    if (config->primary_gie_config.enable)
    {
        if (config->num_secondary_gie_sub_bins > 0)
        {
            if (!create_secondary_gie_bin(config->num_secondary_gie_sub_bins,
                                          config->primary_gie_config.unique_id,
                                          config->secondary_gie_sub_bin_config,
                                          &pipeline->common_elements.secondary_gie_bin))
            {
                goto done;
            }
            gst_bin_add(GST_BIN(pipeline->pipeline),
                        pipeline->common_elements.secondary_gie_bin.bin);
            if (!*src_elem)
            {
                *src_elem = pipeline->common_elements.secondary_gie_bin.bin;
            }
            if (*sink_elem)
            {
                NVGSTDS_LINK_ELEMENT(pipeline->common_elements.secondary_gie_bin.bin,
                                     *sink_elem);
            }
            *sink_elem = pipeline->common_elements.secondary_gie_bin.bin;
        }
    }

    if (config->tracker_config.enable)
    {
        if (!create_tracking_bin(&config->tracker_config,
                                 &pipeline->common_elements.tracker_bin))
        {
            g_print("creating tracker bin failed\n");
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline),
                    pipeline->common_elements.tracker_bin.bin);
        if (!*src_elem)
        {
            *src_elem = pipeline->common_elements.tracker_bin.bin;
        }
        if (*sink_elem)
        {
            NVGSTDS_LINK_ELEMENT(pipeline->common_elements.tracker_bin.bin,
                                 *sink_elem);
        }
        *sink_elem = pipeline->common_elements.tracker_bin.bin;
    }

    if (config->primary_gie_config.enable)
    {
        if (!create_primary_gie_bin(&config->primary_gie_config,
                                    &pipeline->common_elements.primary_gie_bin))
        {
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline),
                    pipeline->common_elements.primary_gie_bin.bin);
        if (*sink_elem)
        {
            NVGSTDS_LINK_ELEMENT(pipeline->common_elements.primary_gie_bin.bin,
                                 *sink_elem);
        }
        *sink_elem = pipeline->common_elements.primary_gie_bin.bin;
        if (!*src_elem)
        {
            *src_elem = pipeline->common_elements.primary_gie_bin.bin;
        }
        NVGSTDS_ELEM_ADD_PROBE(pipeline->common_elements.primary_bbox_buffer_probe_id,
                               pipeline->common_elements.primary_gie_bin.bin, "src",
                               gie_primary_processing_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                               pipeline->common_elements.appCtx);
    }

    if (*src_elem)
    {
        NVGSTDS_ELEM_ADD_PROBE(pipeline->common_elements.primary_bbox_buffer_probe_id,
                               *src_elem, "src",
                               analytics_done_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                               &pipeline->common_elements);

        /* Add common message converter */
        if (config->msg_conv_config.enable)
        {
            NvDsSinkMsgConvBrokerConfig *convConfig = &config->msg_conv_config;
            pipeline->common_elements.msg_conv = gst_element_factory_make(NVDS_ELEM_MSG_CONV, "common_msg_conv");
            if (!pipeline->common_elements.msg_conv)
            {
                NVGSTDS_ERR_MSG_V("Failed to create element 'common_msg_conv'");
                goto done;
            }

            g_object_set(G_OBJECT(pipeline->common_elements.msg_conv),
                         "config", convConfig->config_file_path,
                         "msg2p-lib", (convConfig->conv_msg2p_lib ? convConfig->conv_msg2p_lib : "null"),
                         "payload-type", convConfig->conv_payload_type,
                         "comp-id", convConfig->conv_comp_id, NULL);

            gst_bin_add(GST_BIN(pipeline->pipeline),
                        pipeline->common_elements.msg_conv);

            NVGSTDS_LINK_ELEMENT(*src_elem, pipeline->common_elements.msg_conv);
            *src_elem = pipeline->common_elements.msg_conv;
        }
        pipeline->common_elements.tee = gst_element_factory_make(NVDS_ELEM_TEE, "common_analytics_tee");
        if (!pipeline->common_elements.tee)
        {
            NVGSTDS_ERR_MSG_V("Failed to create element 'common_analytics_tee'");
            goto done;
        }

        gst_bin_add(GST_BIN(pipeline->pipeline),
                    pipeline->common_elements.tee);

        NVGSTDS_LINK_ELEMENT(*src_elem, pipeline->common_elements.tee);
        *src_elem = pipeline->common_elements.tee;
    }

    ret = TRUE;
done:
    return ret;
}

static gboolean is_sink_available_for_source_id(NvDsConfig *config, guint source_id)
{
    for (guint j = 0; j < config->num_sink_sub_bins; j++)
    {
        if (config->sink_bin_sub_bin_config[j].enable &&
            config->sink_bin_sub_bin_config[j].source_id == source_id &&
            config->sink_bin_sub_bin_config[j].link_to_demux == FALSE)
        {
            return TRUE;
        }
    }
    return FALSE;
}

// 설정 캐싱 함수
static void cacheProcessMetaConfigs() {
    if (!cached_config_initialized) {
        auto& config = ConfigManager::getInstance();
        cached_vehicle_2k_enabled = config.isVehicle2KEnabled();
        cached_vehicle_4k_enabled = config.isVehicle4KEnabled();
        cached_pedestrian_meta_enabled = config.isPedestrianMetaEnabled();
        cached_statistics_enabled = config.isStatisticsEnabled();
        cached_config_initialized = true;
        logger->info("ConfigManager 설정 캐싱 완료");
    }
}

/**
 * Initialize modules
 */
static bool initializeModules(AppCtx *appCtx) {
    if (!logger) {
        logger = getLogger("DS_deepstream_app_log");
    }
    logger->info("=== Initializing ITS modules ===");
    
    try {
        // 1. Initialize ConfigManager
        std::string config_path = std::getenv("ITS_CONFIG_PATH") ? 
            std::getenv("ITS_CONFIG_PATH") : 
            "/opt/nvidia/deepstream/deepstream-6.0/sources/apps/sample_apps/deepstream-6.0-calibration/config/config.json";
            
        auto& config_manager = ConfigManager::getInstance();
        if (!config_manager.initialize(config_path)) {
            logger->error("Failed to initialize ConfigManager with path: {}", config_path);
            return false;
        }
        logger->info("ConfigManager initialized successfully from: {}", config_path);

        cacheProcessMetaConfigs();
        
        // 2. Create ROIHandler (DeepStream 의존성)
        roi_handler = std::make_unique<ROIHandler>(*appCtx);  
        logger->info("ROIHandler created successfully");

        // 3. Create image processing modules (SystemManager보다 먼저 생성)
        image_cropper = std::make_unique<ImageCropper>();
        logger->info("ImageCropper created successfully");

        image_storage = std::make_unique<ImageStorage>();
        logger->info("ImageStorage created successfully");
        
        // 4. Create and initialize SystemManager (Redis, SQLite, SiteInfo 통합 관리)
        system_manager = std::make_unique<SystemManager>();
        if (!system_manager->initialize(config_path, roi_handler.get(), 
                                       image_cropper.get(), image_storage.get())) {
            logger->error("Failed to initialize System Manager");
            return false;
        }
        logger->info("System Manager initialized successfully");
        
        // 5. Validate Redis and SQLite from SystemManager
        if (!system_manager->getRedisClient() || !system_manager->getRedisClient()->isConnected()) {
            logger->error("Redis client is not available or not connected");
            return false;
        }
        
        if (!system_manager->getSQLiteHandler() || !system_manager->getSQLiteHandler()->isHealthy()) {
            logger->error("SQLite handler is not available or not healthy");
            return false;
        }

        // 6. Create Vehicle Processor 2K if enabled
        if (config_manager.isVehicle2KEnabled()) {
            vehicle_processor_2k = std::make_unique<VehicleProcessor2K>(
                *roi_handler, 
                *(system_manager->getRedisClient()),
                *(system_manager->getSQLiteHandler()),
                *image_cropper, 
                *image_storage, 
                *(system_manager->getSiteInfoManager()),
                system_manager->getSpecialSiteAdapter()
            );
            logger->info("VehicleProcessor2K initialized successfully");
        }

        // 7. Create Vehicle Processor 4K if enabled
        if (config_manager.isVehicle4KEnabled()) {
            vehicle_processor_4k = std::make_unique<VehicleProcessor4K>(
                *roi_handler,
                *(system_manager->getRedisClient()),
                *image_cropper,
                *image_storage
            );
            logger->info("VehicleProcessor4K created successfully");
        }

        // 8. Create Pedestrian Processor if enabled
        if (config_manager.isPedestrianMetaEnabled()) {
            pedestrian_processor = std::make_unique<PedestrianProcessor>(
                *roi_handler,
                *(system_manager->getRedisClient())
            );
            
            // ROI 체크는 생성자 내부에서 처리되고 isEnabled()로 확인
            if (!pedestrian_processor->isEnabled()) {
                pedestrian_processor.reset();  // ROI 없으면 제거
                logger->info("PedestrianProcessor disabled (no crosswalk ROI)");
            } else {
                logger->info("PedestrianProcessor created successfully");
            }
        }

        // 9. Start SystemManager (통계 타이머 등 시작)
        if (system_manager) {
            system_manager->start();
            int total_lanes = roi_handler->lane_roi.size();
            logger->info("System Manager started - lanes: {}", total_lanes);
        }

        // 10. 모듈 상태 요약 로그
        logger->info("=== 활성 모듈 요약 ===");
        logger->info("  차량 2K: {}", vehicle_processor_2k ? "활성" : "비활성");
        logger->info("  차량 4K: {}", vehicle_processor_4k ? "활성" : "비활성");
        logger->info("  보행자: {}", pedestrian_processor ? "활성" : "비활성");
        logger->info("  통계: {}", system_manager->getStatsGenerator() ? "활성" : "비활성");
        logger->info("  대기행렬: {}", system_manager->getQueueAnalyzer() ? "활성" : "비활성");
        logger->info("  돌발상황: {}", system_manager->getIncidentDetector() ? "활성" : "비활성");
        logger->info("  차량 Presence: {}", system_manager->getCarPresence() ? "활성" : "비활성");
        logger->info("  보행자 Presence: {}", system_manager->getPedestrianPresence() ? "활성" : "비활성");
        if (system_manager->getSpecialSiteAdapter() && 
            system_manager->getSpecialSiteAdapter()->isActive()) {
            auto adapter = system_manager->getSpecialSiteAdapter();
            auto config = adapter->getConfig();
            logger->info("  Special Site: 활성 ({})", 
                        config.straight_left ? "직진/좌회전" : "우회전");
        }
        logger->info("=== All modules initialized successfully ===");
        return true;
        
    } catch (const std::exception& e) {
        logger->error("Module initialization error: {}", e.what());
        return false;
    }
}

/**
 * Cleanup modules
 */
static void cleanupModules() {
    if (logger) {
        logger->info("=== Cleaning up modules ===");
        
        auto start = std::chrono::steady_clock::now();
        auto log_time = [&](const char* module) {
            auto end = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            logger->info("{} cleanup took {} ms", module, ms);
            start = end;
        };
        
        // 1. Vehicle Processor 먼저 정리 (Redis/SQLite 사용 중지)
        vehicle_processor_2k.reset();
        log_time("VehicleProcessor2K");
        
        vehicle_processor_4k.reset();
        log_time("VehicleProcessor4K");

        // 2. Pedestrian Processor 정리
        pedestrian_processor.reset();
        log_time("PedestrianProcessor");

        // 3. ROI Handler 정리
        roi_handler.reset();
        log_time("ROIHandler");

        // 4. SystemManager 정리 (Redis/SQLite/SiteInfo/ImageCaptureHandler 포함)
        // ImageCropper/Storage보다 먼저 정리해야 함
        if (system_manager) {
            system_manager->stop();
            system_manager.reset();
            log_time("SystemManager (includes Redis/SQLite/SiteInfo/ImageCaptureHandler/Presence cleanup)");
        }
        
        // 5. Image 관련 모듈 정리
        image_storage.reset();
        log_time("ImageStorage");
        
        image_cropper.reset();
        log_time("ImageCropper");
        
        logger->info("=== All modules cleaned up ===");
    }
    // 모든 로거 플러시 및 종료
    spdlog::shutdown();
}

/**
 * Process deleted tracker IDs from named pipe
 */
static void discardDeletedId() {
    char id_buffer[1024] = {0};
    ssize_t bytes_read = read(read_fd, id_buffer, sizeof(id_buffer) - 1);
    if (bytes_read > 0){
        std::vector<int> deleted_ids;
        id_buffer[bytes_read] = '\0';
        std::istringstream iss(id_buffer);
        int id;
        while ( iss >> id){
            deleted_ids.push_back(id);
        }
        for (int id : deleted_ids){
            det_obj.erase(id);
        }
    }
}

// Custom overlay function for object visualization
static void setBboxTextColor(AppCtx *appCtx, NvDsObjectMeta *obj, int object_id) {
    NvDsGieConfig *gie_config = &appCtx->config.primary_gie_config;
    gint class_index = obj->class_id;
    int id = obj->object_id;
    
    // mutex 없이 직접 참조
    obj_data &detected_object = det_obj[id];
    
    // 돌발상황 체크는 현재 구조에 맞게 수정
    bool has_incident = false;
    if (system_manager) {
        auto incident_detector = system_manager->getIncidentDetector();
        if (incident_detector && incident_detector->isEnabled()) {
            has_incident = incident_detector->hasIncident(id);
        }
    }
    
    // 돌발상황 object bbox color, width
    if (has_incident) {
        obj->rect_params.border_color = (NvOSD_ColorParams){200/255.0, 50/255.0, 200/255.0, 1};
        obj->rect_params.border_width = 12;
    }
    // Set object bbox color accordingly with the object's class
    else {
        if (g_hash_table_contains(gie_config->bbox_border_color_table, class_index + (gchar *)NULL)) {
            obj->rect_params.border_color = *((NvOSD_ColorParams *)
                g_hash_table_lookup(gie_config->bbox_border_color_table, class_index + (gchar *)NULL));
        } else {
            obj->rect_params.border_color = gie_config->bbox_border_color;
        }
        obj->rect_params.border_width = appCtx->config.osd_config.border_width;
    }
    obj->rect_params.has_bg_color = 0;

    // Set bbox text as configured in deepstream_app_yolov11.txt
    if (appCtx->show_bbox_text) {
        obj->text_params.x_offset = obj->rect_params.left;
        obj->text_params.y_offset = obj->rect_params.top - 30;
        obj->text_params.font_params.font_color = appCtx->config.osd_config.text_color;
        obj->text_params.font_params.font_size = appCtx->config.osd_config.text_size;
        obj->text_params.font_params.font_name = appCtx->config.osd_config.font;
        obj->text_params.set_bg_clr = 1; 
        obj->text_params.text_bg_clr = (NvOSD_ColorParams){0, 0, 0, 0};
    }
    
    // 차량인 경우 속도 표시
    if (isVehicleClass(class_index)) {
        obj->text_params.text_bg_clr = appCtx->config.osd_config.text_bg_color;
        char formatted_speed[7];
        sprintf(formatted_speed, "%.2f", detected_object.speed);
        std::string text = std::string(obj->obj_label) + " ID: " + std::to_string(id) + "\n" + formatted_speed + " Km/h";
        
        if (obj->text_params.display_text) {
            g_free(obj->text_params.display_text);
            obj->text_params.display_text = nullptr;
        }
        obj->text_params.display_text = g_strdup(text.c_str());
    }
}

// Main processing function
static void process_meta(AppCtx *appCtx, NvDsBatchMeta *batch_meta, guint index, GstBuffer *buf) {
    try {
        // Get surface data
        GstMapInfo in_map_info;
        memset(&in_map_info, 0, sizeof(in_map_info));
        
        if (!gst_buffer_map(buf, &in_map_info, GST_MAP_READ)) {
            logger->error("Failed to map gst buffer!");
            return;
        }

        NvBufSurface *surface = (NvBufSurface *)in_map_info.data;
        
        // Update time
        int current_time = getCurTime();
        bool second_changed = (current_time != previous_time);
        if (second_changed) {
            previous_time = current_time;
        }

        // Process deleted tracker IDs
        discardDeletedId();

        // ConfigManager 캐싱 확인
        if (!cached_config_initialized) {
            cacheProcessMetaConfigs();
        }

        // 이미지 캡처 처리 (통합 - 매 프레임마다)
        // IncidentDetector의 요청을 ImageCaptureHandler가 처리
        if (system_manager) {
            auto capture_handler = system_manager->getImageCaptureHandler();
            if (capture_handler) {
                capture_handler->processFrame(surface, current_time);
            }
        }

        // 차로별 차량 수 계산을 위한 맵
        std::map<int, int> lane_vehicle_counts;

        // Process each frame in the batch
        for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
            NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) l_frame->data;
            if (!frame_meta) continue;

            // Process each object in the frame
            for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
                NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) l_obj->data;
                if (!obj_meta) continue;

                int id = obj_meta->object_id;
                int class_id = obj_meta->class_id;
                
                // Update or create object data
                {
                    std::lock_guard<std::mutex> lock(global_mutex);
                    
                    // 새 객체인지 판단
                    if (det_obj.find(id) == det_obj.end()) {
                        det_obj[id].object_id = id;
                        det_obj[id].first_detected_time = current_time;
                    }
                    
                    // 기본 정보 업데이트 (process_meta가 담당)
                    det_obj[id].class_id = class_id;
                    det_obj[id].label = std::string(obj_meta->obj_label);
                    
                    // Convert NvDsObjectMeta bbox to our box structure
                    box obj_box;
                    obj_box.top = obj_meta->rect_params.top;
                    obj_box.height = obj_meta->rect_params.height;
                    obj_box.left = obj_meta->rect_params.left;
                    obj_box.width = obj_meta->rect_params.width;
                    
                    // 현재 위치 계산
                    ObjPoint current_pos = getBottomCenter(obj_box);
                    
                    // 차량인 경우 처리
                    if (isVehicleClass(class_id)) {
                        // 차로 판별 및 카운트
                        int lane = roi_handler->getLaneNum(current_pos);
                        if (lane > 0) {
                            lane_vehicle_counts[lane]++;
                        }
                        
                        // Process vehicle in 2K mode if enabled
                        if (vehicle_processor_2k && cached_vehicle_2k_enabled) {
                            obj_data processed = vehicle_processor_2k->processVehicle(
                                det_obj[id], obj_box, current_pos, current_time, second_changed, surface);
                            
                            // 반환된 데이터 병합
                            det_obj[id] = processed;
                            
                            // 데이터 전송 완료 체크
                            if (processed.turn_pass && !processed.data_sent_2k) {
                                det_obj[id].data_sent_2k = true;
                                logger->trace("2K 차량 ID {} 데이터 전송 완료 표시", id);
                            }
                        }

                        // Process vehicle in 4K mode if enabled
                        if (vehicle_processor_4k && cached_vehicle_4k_enabled) {
                            obj_data processed = vehicle_processor_4k->processVehicle(
                                det_obj[id], obj_box, current_pos, current_time, second_changed, surface);
                            
                            // 반환된 데이터 병합
                            det_obj[id] = processed;
                            
                            // 4K 데이터 전송 완료 체크
                            if (processed.stop_line_pass && !processed.data_sent_4k) {
                                det_obj[id].data_sent_4k = true;
                                logger->trace("4K 차량 ID {} 데이터 전송 완료 표시", id);
                            }
                        }
                        
                        // last_pos 업데이트 (다음 프레임을 위해)
                        det_obj[id].last_pos = current_pos;

                        // Process vehicle for incident detection (last_pos 업데이트 후)
                        if (system_manager) {
                            auto incident_detector = system_manager->getIncidentDetector();
                            if (incident_detector && incident_detector->isEnabled()) {
                                incident_detector->processVehicle(id, det_obj[id], obj_box, surface, current_time);
                            }
                        }
                    }
                    // 보행자인 경우 처리
                    else if (isPedestrianClass(class_id)) {
                        // Process pedestrian if enabled
                        if (pedestrian_processor && cached_pedestrian_meta_enabled) {
                            obj_data processed = pedestrian_processor->processPedestrian(
                                det_obj[id], obj_box, current_pos, current_time, second_changed);
                            
                            // 반환된 데이터 병합
                            det_obj[id] = processed;
                            
                            // 보행자 처리 완료 체크
                            if (processed.ped_pass) {
                                logger->trace("보행자 ID {} 방향 결정 완료: {}", id, 
                                            processed.ped_dir == 1 ? "오른쪽" : "왼쪽");
                            }
                        }
                        
                        // last_pos 업데이트 (다음 프레임을 위해)
                        det_obj[id].last_pos = current_pos;

                        // Process pedestrian for incident detection (last_pos 업데이트 후)
                        if (system_manager) {
                            auto incident_detector = system_manager->getIncidentDetector();
                            if (incident_detector && incident_detector->isEnabled()) {
                                incident_detector->processPedestrian(id, det_obj[id], obj_box, surface, current_time);
                            }
                        }
                    }
                }
                
                // Apply custom overlay (모든 객체 처리가 완료된 후, mutex lock 밖에서 호출)
                setBboxTextColor(appCtx, obj_meta, id);
            }
        }
        
        // 통계 모듈에 프레임 데이터 업데이트 (매 프레임)
        if (cached_statistics_enabled && system_manager) {
            auto stats_gen = system_manager->getStatsGenerator();
            if (stats_gen) {
                stats_gen->updateFrameData(lane_vehicle_counts);
            }
        }

        // Presence 모듈 업데이트를 위한 위치 정보 수집 (매 프레임)
        if (system_manager) {
            std::map<int, ObjPoint> vehicle_positions;
            std::map<int, ObjPoint> pedestrian_positions;
            
            // det_obj에서 현재 프레임의 차량/보행자 위치 수집
            {
                std::lock_guard<std::mutex> lock(global_mutex);
                for (const auto& [id, obj] : det_obj) {
                    // 현재 프레임에서 처리되지 않은 객체 스킵
                    if (obj.last_pos.x <= 0 || obj.last_pos.y <= 0) {
                        continue;  // 첫 프레임이거나 아직 처리 안 된 객체
                    }
                    
                    if (isVehicleClass(obj.class_id)) {
                        vehicle_positions[id] = obj.last_pos;
                    } else if (isPedestrianClass(obj.class_id)) {
                        pedestrian_positions[id] = obj.last_pos;
                    }
                }
            }        
            
            // Presence 모듈 업데이트 (신호와 무관하게 매 프레임 호출)
            system_manager->updatePresenceModules(vehicle_positions, pedestrian_positions, current_time);
        }

        // 매 초마다 SystemManager 업데이트 (신호 변경 체크 및 대기행렬 업데이트)
        if (second_changed && system_manager) {
            system_manager->updatePerSecondData(lane_vehicle_counts, current_time);
        }
        
        // ROI overlay
        if (roi_handler) {
            roi_handler->overlayROI(batch_meta);
        }
        
        gst_buffer_unmap(buf, &in_map_info);
        
    } catch (const std::exception& e) {
        logger->error("Error in process_meta: {}", e.what());
    }
}

/**
 * Main function to create the pipeline.
 */
gboolean
create_pipeline(AppCtx *appCtx,
                bbox_generated_callback bbox_generated_post_analytics_cb,
                bbox_generated_callback all_bbox_generated_cb, perf_callback perf_cb,
                overlay_graphics_callback overlay_graphics_cb)
{
    gboolean ret = FALSE;
    NvDsPipeline *pipeline = &appCtx->pipeline;
    NvDsConfig *config = &appCtx->config;
    GstBus *bus;
    GstElement *last_elem;
    GstElement *tmp_elem1;
    GstElement *tmp_elem2;
    guint i;
	GstPad *fps_pad;
    gulong latency_probe_id;

    static std::string base_path;
    static std::string vehicle_2k_path;
    static std::string vehicle_4k_path;
    static std::string wait_queue_path;
    static std::string incident_path;

    if (!logger) {
        logger = getLogger("DS_deepstream_app_log");
        logger->info("=== DeepStream ITS App Starting ===");
        logger->info("Creating Pipeline...");
    }

    _dsmeta_quark = g_quark_from_static_string(NVDS_META_STRING);
    appCtx->all_bbox_generated_cb = all_bbox_generated_cb;
    appCtx->bbox_generated_post_analytics_cb = bbox_generated_post_analytics_cb;
    appCtx->overlay_graphics_cb = overlay_graphics_cb;

    // Initialize our modules before pipeline creation
    if (!initializeModules(appCtx)) {
        logger->error("Failed to initialize ITS modules");
        goto done;
    }
    
    base_path = CONFIG.getBasePath();
    vehicle_2k_path = CONFIG.getFullImagePath("vehicle_2k");
    vehicle_4k_path = CONFIG.getFullImagePath("vehicle_4k");
    wait_queue_path = CONFIG.getFullImagePath("wait_queue");
    incident_path = CONFIG.getFullImagePath("incident_event");

    // 폴더가 없으면 자동 생성
    ImageStorage::createDirectory(vehicle_2k_path);
    ImageStorage::createDirectory(vehicle_4k_path);
    ImageStorage::createDirectory(wait_queue_path);
    ImageStorage::createDirectory(incident_path);

    logger->info("Image directories checked/created:");
    logger->info("  - Vehicle 2K: {}", vehicle_2k_path);
    logger->info("  - Vehicle 4K: {}", vehicle_4k_path);
    logger->info("  - Wait Queue: {}", wait_queue_path);
    logger->info("  - Incident: {}", incident_path);
				
    // Setup named pipe for deleted tracker IDs
    if (mkfifo(DELETED_ID_PIPE, 0666) == -1) {
        if (errno != EEXIST) {
            logger->error("Error creating named pipe: {}", strerror(errno));
            goto done;
        }
    }
    if (read_fd < 0) {
        read_fd = open(DELETED_ID_PIPE, O_RDONLY | O_NONBLOCK);
    }

    if (config->osd_config.num_out_buffers < 8)
    {
        config->osd_config.num_out_buffers = 8;
    }

    pipeline->pipeline = gst_pipeline_new("pipeline");
    if (!pipeline->pipeline)
    {
        NVGSTDS_ERR_MSG_V("Failed to create pipeline");
		logger->error("Failed to create GStreamer pipeline");
        goto done;
    }

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline->pipeline));       // 파이프라인 생성
    pipeline->bus_id = gst_bus_add_watch(bus, bus_callback, appCtx);
    gst_object_unref(bus);

    if (config->file_loop)
    {
        /* Let each source bin know it needs to loop. */
        guint i;
        for (i = 0; i < config->num_source_sub_bins; i++)
            config->multi_source_config[i].loop = TRUE;
    }

    for (guint i = 0; i < config->num_sink_sub_bins; i++)
    {
        NvDsSinkSubBinConfig *sink_config = &config->sink_bin_sub_bin_config[i];
        switch (sink_config->type)
        {
        case NV_DS_SINK_FAKE:
        case NV_DS_SINK_RENDER_EGL:
        case NV_DS_SINK_RENDER_OVERLAY:
            /* Set the "qos" property of sink, if not explicitly specified in the
                     config. */
            if (!sink_config->render_config.qos_value_specified)
            {
                /* QoS events should be generated by sink always in case of live sources
                         or with synchronous playback for non-live sources. */
                if (config->streammux_config.live_source || sink_config->render_config.sync)
                {
                    sink_config->render_config.qos = TRUE;
                }
                else
                {
                    sink_config->render_config.qos = FALSE;
                }
            }
        default:
            break;
        }
    }

    /*
     * Add muxer and < N > source components to the pipeline based
     * on the settings in configuration file.
     */
    if (!create_multi_source_bin(config->num_source_sub_bins,
                                 config->multi_source_config, &pipeline->multi_src_bin))
        goto done;
    gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->multi_src_bin.bin);

    if (config->streammux_config.is_parsed)
        set_streammux_properties(&config->streammux_config,
                                 pipeline->multi_src_bin.streammux);

    if (appCtx->latency_info == NULL)
    {
        appCtx->latency_info = (NvDsFrameLatencyInfo *)
            calloc(1, config->streammux_config.batch_size *
                          sizeof(NvDsFrameLatencyInfo));
    }

    /** a tee after the tiler which shall be connected to sink(s) */
    pipeline->tiler_tee = gst_element_factory_make(NVDS_ELEM_TEE, "tiler_tee");
    if (!pipeline->tiler_tee)
    {
        NVGSTDS_ERR_MSG_V("Failed to create element 'tiler_tee'");
        goto done;
    }
    gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->tiler_tee);

    /** Tiler + Demux in Parallel Use-Case */
    if (config->tiled_display_config.enable == NV_DS_TILED_DISPLAY_ENABLE_WITH_PARALLEL_DEMUX)
    {
        pipeline->demuxer =
            gst_element_factory_make(NVDS_ELEM_STREAM_DEMUX, "demuxer");
        if (!pipeline->demuxer)
        {
            NVGSTDS_ERR_MSG_V("Failed to create element 'demuxer'");
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->demuxer);

        /** NOTE:
         * demux output is supported for only one source
         * If multiple [sink] groups are configured with
         * link_to_demux=1, only the first [sink]
         * shall be constructed for all occurences of
         * [sink] groups with link_to_demux=1
         */
        {
            gchar pad_name[16];
            GstPad *demux_src_pad;

            i = 0;
            if (!create_demux_pipeline(appCtx, i))
            {
                goto done;
            }

            for (i = 0; i < config->num_sink_sub_bins; i++)
            {
                if (config->sink_bin_sub_bin_config[i].link_to_demux == TRUE)
                {
                    g_snprintf(pad_name, 16, "src_%02d", config->sink_bin_sub_bin_config[i].source_id);
                    break;
                }
            }

            if (i >= config->num_sink_sub_bins)
            {
                g_print("\n\nError : sink for demux (use link-to-demux-only property) is not provided in the config file\n\n");
                goto done;
            }

            i = 0;

            gst_bin_add(GST_BIN(pipeline->pipeline),
                        pipeline->demux_instance_bins[i].bin);

            demux_src_pad = gst_element_get_request_pad(pipeline->demuxer, pad_name);
            NVGSTDS_LINK_ELEMENT_FULL(pipeline->demuxer, pad_name,
                                      pipeline->demux_instance_bins[i].bin, "sink");
            gst_object_unref(demux_src_pad);

            NVGSTDS_ELEM_ADD_PROBE(latency_probe_id,
                                   appCtx->pipeline.demux_instance_bins[i].demux_sink_bin.bin,
                                   "sink",
                                   demux_latency_measurement_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                                   appCtx);
            latency_probe_id = latency_probe_id;
        }

        last_elem = pipeline->demuxer;
        link_element_to_tee_src_pad(pipeline->tiler_tee, last_elem);
        last_elem = pipeline->tiler_tee;
    }

    if (config->tiled_display_config.enable)
    {

        /* Tiler will generate a single composited buffer for all sources. So need
         * to create only one processing instance. */
        if (!create_processing_instance(appCtx, 0))
        {
            goto done;
        }
        // create and add tiling component to pipeline.
        if (config->tiled_display_config.columns *
                config->tiled_display_config.rows <
            config->num_source_sub_bins)
        {
            if (config->tiled_display_config.columns == 0)
            {
                config->tiled_display_config.columns =
                    (guint)(sqrt(config->num_source_sub_bins) + 0.5);
            }
            config->tiled_display_config.rows =
                (guint)ceil(1.0 * config->num_source_sub_bins /
                            config->tiled_display_config.columns);
            NVGSTDS_WARN_MSG_V("Num of Tiles less than number of sources, readjusting to "
                               "%u rows, %u columns",
                               config->tiled_display_config.rows,
                               config->tiled_display_config.columns);
        }

        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->instance_bins[0].bin);
        last_elem = pipeline->instance_bins[0].bin;

        if (!create_tiled_display_bin(&config->tiled_display_config,
                                      &pipeline->tiled_display_bin))
        {
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->tiled_display_bin.bin);
        NVGSTDS_LINK_ELEMENT(pipeline->tiled_display_bin.bin, last_elem);
        last_elem = pipeline->tiled_display_bin.bin;

        link_element_to_tee_src_pad(pipeline->tiler_tee, pipeline->tiled_display_bin.bin);
        last_elem = pipeline->tiler_tee;

        NVGSTDS_ELEM_ADD_PROBE(latency_probe_id,
                               pipeline->instance_bins->sink_bin.sub_bins[0].sink, "sink",
                               latency_measurement_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                               appCtx);
        latency_probe_id = latency_probe_id;
    }
    else
    {
        /*
         * Create demuxer only if tiled display is disabled.
         */
        pipeline->demuxer =
            gst_element_factory_make(NVDS_ELEM_STREAM_DEMUX, "demuxer");
        if (!pipeline->demuxer)
        {
            NVGSTDS_ERR_MSG_V("Failed to create element 'demuxer'");
            goto done;
        }
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->demuxer);

        for (i = 0; i < config->num_source_sub_bins; i++)
        {
            gchar pad_name[16];
            GstPad *demux_src_pad;

            /* Check if any sink has been configured to render/encode output for
             * source index `i`. The processing instance for that source will be
             * created only if atleast one sink has been configured as such.
             */
            if (!is_sink_available_for_source_id(config, i))
                continue;

            if (!create_processing_instance(appCtx, i))
            {
                goto done;
            }
            gst_bin_add(GST_BIN(pipeline->pipeline),
                        pipeline->instance_bins[i].bin);

            g_snprintf(pad_name, 16, "src_%02d", i);
            demux_src_pad = gst_element_get_request_pad(pipeline->demuxer, pad_name);
            NVGSTDS_LINK_ELEMENT_FULL(pipeline->demuxer, pad_name,
                                      pipeline->instance_bins[i].bin, "sink");
            gst_object_unref(demux_src_pad);

            NVGSTDS_ELEM_ADD_PROBE(latency_probe_id,
                                   // pipeline->instance_bins[i].sink_bin.sub_bins[0].sink, "sink",
                                   pipeline->instance_bins[i].sink_bin.sub_bins[0].sink, "sink",
                                   latency_measurement_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                                   appCtx);
            latency_probe_id = latency_probe_id;
        }
        last_elem = pipeline->demuxer;
    }

    if (config->tiled_display_config.enable == NV_DS_TILED_DISPLAY_ENABLE)
    {
        fps_pad = gst_element_get_static_pad(pipeline->tiled_display_bin.bin, "sink");
    }
    else
    {
        fps_pad = gst_element_get_static_pad(pipeline->demuxer, "sink");
    }

    pipeline->common_elements.appCtx = appCtx;
    // Decide where in the pipeline the element should be added and add only if
    // enabled
    if (config->dsexample_config.enable)
    {
        // Create dsexample element bin and set properties
        if (!create_dsexample_bin(&config->dsexample_config,
                                  &pipeline->dsexample_bin))
        {
            goto done;
        }
        // Add dsexample bin to instance bin
        gst_bin_add(GST_BIN(pipeline->pipeline), pipeline->dsexample_bin.bin);

        // Link this bin to the last element in the bin
        NVGSTDS_LINK_ELEMENT(pipeline->dsexample_bin.bin, last_elem);

        // Set this bin as the last element
        last_elem = pipeline->dsexample_bin.bin;
    }
    // create and add common components to pipeline.
    if (!create_common_elements(config, pipeline, &tmp_elem1, &tmp_elem2,
                                bbox_generated_post_analytics_cb))
    {
        goto done;
    }

    if (!add_and_link_broker_sink(appCtx))
    {
        goto done;
    }

    if (tmp_elem2)
    {
        NVGSTDS_LINK_ELEMENT(tmp_elem2, last_elem);
        last_elem = tmp_elem1;
    }

    NVGSTDS_LINK_ELEMENT(pipeline->multi_src_bin.bin, last_elem);

    // enable performance measurement and add call back function to receive
    // performance data.
    if (config->enable_perf_measurement)
    {
        appCtx->perf_struct.context = appCtx;
        enable_perf_measurement(&appCtx->perf_struct, fps_pad,
                                pipeline->multi_src_bin.num_bins,
                                config->perf_measurement_interval_sec,
                                config->multi_source_config[0].dewarper_config.num_surfaces_per_frame,
                                perf_cb);
    }

    NVGSTDS_ELEM_ADD_PROBE(latency_probe_id,
                           pipeline->instance_bins->sink_bin.sub_bins[0].sink, "sink",
                           latency_measurement_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                           appCtx);
    latency_probe_id = latency_probe_id;

    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(appCtx->pipeline.pipeline),
                                      GST_DEBUG_GRAPH_SHOW_ALL, "ds-app-null");

    g_mutex_init(&appCtx->app_lock);
    g_cond_init(&appCtx->app_cond);
    g_mutex_init(&appCtx->latency_lock);

    logger->info("Pipeline created successfully");
    ret = TRUE;
done:
    if (!ret)
    {
        NVGSTDS_ERR_MSG_V("%s failed", __func__);
        logger->error("create_pipeline failed");
        logger->info("Cleaning up modules due to pipeline creation failure");
        cleanupModules();
    }
    return ret;
}

/**
 * Function to destroy pipeline and release the resources, probes etc.
 */
void destroy_pipeline(AppCtx *appCtx) 
{
    if (!appCtx) return;
    
    logger->info("Destroying pipeline...");
    
    gint64 end_time;
    NvDsConfig *config = &appCtx->config;
    guint i;
    GstBus *bus = NULL;

    end_time = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

    if (appCtx->pipeline.demuxer) {
        gst_pad_send_event(gst_element_get_static_pad(appCtx->pipeline.demuxer, "sink"),
                          gst_event_new_eos());
    } 
	else if (appCtx->pipeline.instance_bins[0].sink_bin.bin) {
        gst_pad_send_event(gst_element_get_static_pad(appCtx->pipeline.instance_bins[0].sink_bin.bin, "sink"),
                          gst_event_new_eos());
    }

    g_usleep(100000);

    g_mutex_lock(&appCtx->app_lock);
    if (appCtx->pipeline.pipeline) {
        destroy_smart_record_bin(&appCtx->pipeline.multi_src_bin);
        bus = gst_pipeline_get_bus(GST_PIPELINE(appCtx->pipeline.pipeline));

        while (TRUE) {
            GstMessage *message = gst_bus_pop(bus);
            if (message == NULL)
                break;
            else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
                bus_callback(bus, message, appCtx);
            else
                gst_message_unref(message);
        }
        gst_element_set_state(appCtx->pipeline.pipeline, GST_STATE_NULL);
    }
    g_cond_wait_until(&appCtx->app_cond, &appCtx->app_lock, end_time);
    g_mutex_unlock(&appCtx->app_lock);

    // Remove probes
    for (i = 0; i < appCtx->config.num_source_sub_bins; i++) {
        NvDsInstanceBin *bin = &appCtx->pipeline.instance_bins[i];
        if (config->osd_config.enable) {
            NVGSTDS_ELEM_REMOVE_PROBE(bin->all_bbox_buffer_probe_id,
                                     bin->osd_bin.nvosd, "sink");
        } else {
            NVGSTDS_ELEM_REMOVE_PROBE(bin->all_bbox_buffer_probe_id,
                                     bin->sink_bin.bin, "sink");
        }

        if (config->primary_gie_config.enable) {
            NVGSTDS_ELEM_REMOVE_PROBE(bin->primary_bbox_buffer_probe_id,
                                     bin->primary_gie_bin.bin, "src");
        }
    }

    if (appCtx->latency_info == NULL) {
        free(appCtx->latency_info);
        appCtx->latency_info = NULL;
    }

    destroy_sink_bin();
    g_mutex_clear(&appCtx->latency_lock);

    if (appCtx->pipeline.pipeline) {
        bus = gst_pipeline_get_bus(GST_PIPELINE(appCtx->pipeline.pipeline));
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
        gst_object_unref(appCtx->pipeline.pipeline);
    }

    if (config->num_message_consumers) {
        for (i = 0; i < config->num_message_consumers; i++) {
            if (appCtx->c2d_ctx[i])
                stop_cloud_to_device_messaging(appCtx->c2d_ctx[i]);
        }
    }
    
    // 파이프라인 정리가 끝난 후 Named pipe 정리
    if (read_fd >= 0) {
        close(read_fd);
        read_fd = -1;
        unlink(DELETED_ID_PIPE);
        logger->info("Named pipe closed and removed");
    }
	
	// 마지막에 모듈 정리
    cleanupModules();

    logger->info("Pipeline destroyed");
    
    return;
}

gboolean
pause_pipeline(AppCtx *appCtx) 
{
    GstState cur;
    GstState pending;
    GstStateChangeReturn ret;
    GstClockTime timeout = 5 * GST_SECOND / 1000;

    ret = gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending, timeout);

    if (ret == GST_STATE_CHANGE_ASYNC) {
        return FALSE;
    }

    if (cur == GST_STATE_PAUSED) {
        return TRUE;
    } 
	else if (cur == GST_STATE_PLAYING) {
        gst_element_set_state(appCtx->pipeline.pipeline, GST_STATE_PAUSED);
        gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending, GST_CLOCK_TIME_NONE);
        pause_perf_measurement(&appCtx->perf_struct);
        return TRUE;
    }
	else {
		return FALSE;
	}
}

gboolean
resume_pipeline(AppCtx *appCtx) 
{
    GstState cur;
    GstState pending;
    GstStateChangeReturn ret;
    GstClockTime timeout = 5 * GST_SECOND / 1000;

    ret = gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending, timeout);

    if (ret == GST_STATE_CHANGE_ASYNC) {
        return FALSE;
    }

    if (cur == GST_STATE_PLAYING) {
        return TRUE;
    } 
	else if (cur == GST_STATE_PAUSED) {
        gst_element_set_state(appCtx->pipeline.pipeline, GST_STATE_PLAYING);
        gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending, GST_CLOCK_TIME_NONE);
        resume_perf_measurement(&appCtx->perf_struct);
        return TRUE;
    }
	else {
		return FALSE;
	}
}