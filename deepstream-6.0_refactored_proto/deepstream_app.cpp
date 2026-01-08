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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.    IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <locale>
#include <ios>
#include <limits>
#include <sstream>
#include <gst/gst.h>
#include <string.h>
#include <iostream>
#include <math.h>
#include <stdlib.h>
#include <vector>
#include <sys/types.h>
#include <dirent.h>
#include "deepstream_app.h"
#include <ctime>
#include <sys/stat.h>
#include <fstream>
#include <map> 
#include <deque>

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

// thread header
#include <thread>
#include <mutex>
#include <queue>

#include "nvbufsurface.h"
#include "nvbufsurftransform.h"

/* Open CV headers */
#include "opencv2/opencv.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"


#include "calibration.h"
#include "process_meta.h"
#include "roi_handler.h"
#include "redis_client.h"

#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif
// exception for try catch //
#include <exception>
#include <hiredis/hiredis.h>

// 정규식 체크 라이브러리
#include <regex>

#include <ctime>
#include <pthread.h>
static std::shared_ptr<spdlog::logger> logger = NULL;
static std::shared_ptr<spdlog::logger> image_logger = NULL;
inline std::string timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto timeT = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm = *std::localtime(&timeT);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S")
        << "." << std::setw(2) << std::setfill('0') << ms.count() / 10 << ": "; // .2f format
    return oss.str();
}
GST_DEBUG_CATEGORY_EXTERN(NVDS_APP);

GQuark _dsmeta_quark;

#define CEIL(a, b) ((a + b - 1) / b)

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
                dump_pipeline_properties(appCtx->pipeline.pipeline);
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
    // NvDsInstanceBin *bin = (NvDsInstanceBin *)u_data;
    // guint index = bin->index;
    // AppCtx *appCtx = bin->appCtx;
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

// 사내 테스트용 전역 플래그
bool wait_queue_enabled = false;
bool move_reverse_enabled = false;
bool illegal_wait_enabled = false;
bool realtime_enabled_2k = false;
bool realtime_enabled_4k = false;
bool pedestrian_enabled_2k = false;
bool pedestrian_jaywalk_enabled_2k = false;

static int read_fd = -1;
#define DELETED_ID_PIPE "/tmp/deleted_tracker_pipe"
static std::string BASE_PATH = "/opt/nvidia/deepstream/deepstream-6.0/sources/objectDetector_GB/";
std::mutex sqlite_lock;
// static std::unique_ptr<ServerInterface> server;
// static std::unique_ptr<ServerReceiver> server_receiver;
static std::unique_ptr<ROIHandler> roi_handler;
static std::unique_ptr<RedisClient> redis_client;
static std::map<int, obj_data> det_obj;
std::map<int, int> per_lane_max;
std::map<int, int> per_lane_max_five;
std::map<int, int> per_lane_min;
std::map<int, int> per_lane_min_five;
std::map<int, int> per_lane_total;
std::map<int, int> per_lane_total_five;
std::mutex per_lane_traffic_mutex;
std::mutex per_lane_five_mutex;
std::map<int, int> per_lane_count;
std::mutex per_lane_count_mutex;
static int total_lane;
std::atomic<int> residual_timestamp {0};
std::atomic<bool> current_phase {false};
std::atomic<int> current_cycle {0};
std::atomic<bool> waiting_image_save {false};

int getCurTime(){
    return (int)time(NULL);
}

std::vector<std::map<int, int>> getPerLaneDensity() {
    std::lock_guard<std::mutex> lock(per_lane_traffic_mutex);
    std::vector<std::map<int, int>> result;
    result.push_back(per_lane_max);
    result.push_back(per_lane_min);
    result.push_back(per_lane_total);
    per_lane_max.clear();
    per_lane_total.clear();
    for (int i=i; i<=total_lane; ++i) {
        per_lane_min[i] = INT_MAX;
    }
    return result;  
}
std::map<int, int> getPerLaneCount() {
    std::lock_guard<std::mutex> lock(per_lane_count_mutex);
    return per_lane_count;  
}
int resetPerLane() {
    std::lock_guard<std::mutex> lock(per_lane_traffic_mutex);
    per_lane_max.clear();
    per_lane_total.clear();
    for (int i=i; i<=total_lane; ++i) {
        per_lane_min[i] = INT_MAX;
    }
    return 0;
}

bool isVehicle(char *s){
    if (strcmp("person", s) == 0)
        return false;
    return true;
}
void updateSpeed(obj_data &detected_object, ObjPoint p1){
    int current_time = getCurTime();
    if (detected_object.num_speed++ > 0){
        detected_object.speed = calculateSpeed(detected_object.prev_pos.x, detected_object.prev_pos.y, p1.x, p1.y, current_time - detected_object.prev_pos_time);
        if (std::fabs(p1.x - detected_object.prev_pos.x) > 20)
            detected_object.speed += 5.0;
        detected_object.avg_speed += (detected_object.speed - detected_object.avg_speed) / detected_object.num_speed;
    }
    detected_object.prev_pos = p1;
    detected_object.prev_pos_time = current_time;
}
void setBboxTextColor(AppCtx *appCtx, NvDsObjectMeta *obj){
    NvDsGieConfig *gie_config = NULL;
    gie_config = &appCtx->config.primary_gie_config;
    gint class_index = obj->class_id;
    int id = obj->object_id;
    obj_data &detected_object = det_obj[id];
    // 돌발상황 object bbox color, width
    if (detected_object.illegal_wait || detected_object.move_reverse || detected_object.jaywalk){
        obj->rect_params.border_color = (NvOSD_ColorParams){200/255.0, 50/255.0, 200/255.0, 1};
        obj->rect_params.border_width = 12;
    }
    // Set object bbox color accordingly with the object's class
    else{
        obj->rect_params.border_color = *((NvOSD_ColorParams *)
                                    g_hash_table_lookup(gie_config->bbox_border_color_table, class_index + (gchar *)NULL));
        obj->rect_params.border_width = appCtx->config.osd_config.border_width;
    }
    obj->rect_params.has_bg_color = 0;

    // Set bbox text as configured in deepstream_app_yolov11.txt
    if (appCtx->show_bbox_text){
        obj->text_params.x_offset = obj->rect_params.left;
        obj->text_params.y_offset = obj->rect_params.top - 30;
        obj->text_params.font_params.font_color = appCtx->config.osd_config.text_color;
        obj->text_params.font_params.font_size = appCtx->config.osd_config.text_size;
        obj->text_params.font_params.font_name = appCtx->config.osd_config.font;
        obj->text_params.set_bg_clr = 1; 
        obj->text_params.text_bg_clr = (NvOSD_ColorParams){0, 0, 0, 0};
    }
    // if (isVehicle(obj->obj_label) || true){
    //     obj->text_params.text_bg_clr = appCtx->config.osd_config.text_bg_color;
    //     char formatted_speed[7];
    //     sprintf(formatted_speed, "%.2f", detected_object.speed);
    //     std::string text = std::string(obj->obj_label) + " ID: "+std::to_string(id)+"\n"+formatted_speed+" Km/h";
    //     if (obj->text_params.display_text) {
    //         g_free(obj->text_params.display_text);
    //         obj->text_params.display_text = nullptr;
    //     }
    //     text = "";
    //     obj->text_params.display_text = g_strdup(text.c_str());
    // }
    if (isVehicle(obj->obj_label)){
        obj->text_params.text_bg_clr = appCtx->config.osd_config.text_bg_color;
        char formatted_speed[7];
        sprintf(formatted_speed, "%.2f", detected_object.speed);
        std::string text = std::string(obj->obj_label) + " ID: "+std::to_string(id)+"\n"+formatted_speed+" Km/h";
        if (obj->text_params.display_text) {
            g_free(obj->text_params.display_text);
            obj->text_params.display_text = nullptr;
        }
        obj->text_params.display_text = g_strdup(text.c_str());
    }
}
void discardDeletedId(){
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
int fetchStats(std::map<int ,std::vector<int>>& density, int time_type, int time_window) {
    std::lock_guard<std::mutex> db_lock(sqlite_lock);
    sqlite3* local_db = nullptr;
    int flags = SQLITE_OPEN_READONLY;
    int rc = sqlite3_open_v2("test.db", &local_db, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "DB open failed in fetchStats: " << sqlite3_errmsg(local_db) << std::endl;
        return -1;
    }
    std::time_t now = std::time(nullptr);
    int stats_end = now;
    int stats_start = now - time_window;
    sqlite3_stmt* stmt;
    std::string cam_id = redis_client->camID();

    // 1. Approach-level stats (soitgaprdstats)
    std::string query1 =
        "SELECT COUNT(*), AVG(stop_point_speed), AVG(interval_speed) "
        "FROM test_table WHERE timestamp BETWEEN " + std::to_string(stats_start) +
        " AND " + std::to_string(stats_end);
    int totl_trvl;
    if (sqlite3_prepare_v2(local_db, query1.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            totl_trvl = sqlite3_column_int(stmt, 0);
            double avg_stop = sqlite3_column_double(stmt, 1);
            double avg_interval = sqlite3_column_double(stmt, 2);
            int avg_density = 0;
            int min_density = INT_MAX;
            int max_density = 0;
            for (int i=1; i<=total_lane; ++i) {
                auto it = density.find(i);
                if (it != density.end()) {
                    const auto& v= it->second;
                    if (v.size() >=3 ) {
                        avg_density += v[0];
                        min_density = std::min(min_density, v[1]);
                        max_density = std::max(max_density, v[2]);
                    }
                }
            }
            std::cout << timestamp() << "[Approach Stats] Vehicles: " << totl_trvl
                      << ", StopAvg: " << avg_stop
                      << ", IntervalAvg: " << avg_interval
                      << ", Average Density: " << avg_density
                      << ", Minimum Density: " << min_density
                      << ", Maximum Density: " << max_density << "\n";
            // std::string approach_query = "Approach Stats Query";
            std::string approach_query = QueryMaker::approachStatsQuery(cam_id, time_type, stats_start, stats_end, 
                                                            totl_trvl, avg_stop, avg_interval, avg_density, min_density, 
                                                            max_density, (double)avg_density / total_lane);
            redis_client->redisSendQuery(approach_query);
        }
    }
    sqlite3_finalize(stmt);

    // 2. Turn Type Stats (soitgturntypestats) grouped by dir_out
    std::string query2 =
        "SELECT dir_out, "
        "SUM(label='MBUS'), SUM(label='LBUS'), SUM(label='PCAR'), "
        "SUM(label='MOTOR'), SUM(label='MTRUCK'), SUM(label='LTRUCK'), "
        "AVG(stop_point_speed), AVG(interval_speed) "
        "FROM test_table "
        "WHERE timestamp BETWEEN " + std::to_string(stats_start) +
        " AND " + std::to_string(stats_end) +
        " GROUP BY dir_out";

    if (sqlite3_prepare_v2(local_db, query2.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::cout << timestamp() << "[Turn Type Stats]\n";
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int dir = sqlite3_column_int(stmt, 0);
            int kncr1 = sqlite3_column_int(stmt, 1);
            int kncr2 = sqlite3_column_int(stmt, 2);
            int kncr3 = sqlite3_column_int(stmt, 3);
            int kncr4 = sqlite3_column_int(stmt, 4);
            int kncr5 = sqlite3_column_int(stmt, 5);
            int kncr6 = sqlite3_column_int(stmt, 6);
            double avg_stop = sqlite3_column_double(stmt, 7);
            double avg_interval = sqlite3_column_double(stmt, 8);

            std::cout << timestamp() << "dir_out: " << dir
                      << ", MBUS: " << kncr1
                      << ", LBUS: " << kncr2
                      << ", PCAR: " << kncr3
                      << ", MOTOR: " << kncr4
                      << ", MTRUCK: " << kncr5
                      << ", LTRUCK: " << kncr6
                      << ", StopAvg: " << avg_stop
                      << ", IntervalAvg: " << avg_interval << "\n";
            
            std::string turntype_query = QueryMaker::turntypeStatsQuery(cam_id, dir, time_type, stats_start, stats_end, 
                                                            kncr1, kncr2, kncr3, kncr4, kncr5, kncr6, 
                                                            avg_stop, avg_interval);

            // std::string turntype_query = "Turn Type Stats Query";
            redis_client->redisSendQuery(turntype_query);
        }
    }
    sqlite3_finalize(stmt);

    // 3. Class stats (soitgkncrstats) grouped by label
    std::string query3 =
        "SELECT label, COUNT(*), AVG(stop_point_speed), AVG(interval_speed) "
        "FROM test_table "
        "WHERE timestamp BETWEEN " + std::to_string(stats_start) +
        " AND " + std::to_string(stats_end) +
        " GROUP BY label";

    if (sqlite3_prepare_v2(local_db, query3.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::cout << timestamp() << "[Vehicle Class Stats]\n";
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            int count = sqlite3_column_int(stmt, 1);
            double avg_stop = sqlite3_column_double(stmt, 2);
            double avg_interval = sqlite3_column_double(stmt, 3);

            std::cout << timestamp() << "Label: " << label
                      << ", Count: " << count
                      << ", StopAvg: " << avg_stop
                      << ", IntervalAvg: " << avg_interval << "\n";

            std::string kncr_query = QueryMaker::kncrStatsQuery(cam_id, time_type, label, stats_start, stats_end, 
                                                    count, avg_stop, avg_interval);
            // std::string kncr_query = "Kncr Stats Query";
            redis_client->redisSendQuery(kncr_query);
        }
    }
    sqlite3_finalize(stmt);

    // 4. Lane stats (soitglanestats) grouped by lane
    std::string query4 =
        "SELECT lane, COUNT(*), AVG(stop_point_speed), AVG(interval_speed) "
        "FROM test_table "
        "WHERE timestamp BETWEEN " + std::to_string(stats_start) +
        " AND " + std::to_string(stats_end) +
        " GROUP BY lane";

    if (sqlite3_prepare_v2(local_db, query4.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::cout << timestamp() << "[Lane Stats]\n";
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int lane = sqlite3_column_int(stmt, 0);
            int count = sqlite3_column_int(stmt, 1);
            double avg_stop = sqlite3_column_double(stmt, 2);
            double avg_interval = sqlite3_column_double(stmt, 3);
            int avg_density = 0, min_density = 0, max_density = 0;
            auto it = density.find(lane);
            if (it != density.end()) {
                const auto& v = it->second;
                if (v.size() >= 3) { 
                    avg_density = v[0];
                    min_density = v[1];
                    max_density = v[2];
                }
            }
            double share = static_cast<double>(count) / totl_trvl;
            std::cout << timestamp() << "Lane: " << lane
                      << ", Count: " << count
                      << ", StopAvg: " << avg_stop
                      << ", IntervalAvg: " << avg_interval
                      << ", Average Density: " << avg_density
                      << ", Minimum Density: " << min_density
                      << ", Maximum Density: " << max_density
                      << ", Share: " << share << "\n";

            std::string lane_query = QueryMaker::laneStatsQuery(cam_id, time_type, lane, stats_start, stats_end, count, 
                                                    avg_stop, avg_interval, avg_density, min_density, max_density, share);
            // std::string lane_query = "Lane Stats Query";
            redis_client->redisSendQuery(lane_query);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(local_db);
    return 0;
}

void fiveMinNotifier(){
    while (true){
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_c);

        int curr_min = now_tm->tm_min;
        int curr_sec = now_tm->tm_sec;

        int expected_min = ((curr_min / 5) + 1) * 5;
        if (expected_min == 60)
            expected_min = 0;
        int min_till_next = 5 - (curr_min % 5);
        int sec_till_next = (min_till_next * 60) - curr_sec;
        std::this_thread::sleep_for(std::chrono::seconds(sec_till_next));
        do {
            now = std::chrono::system_clock::now();
            now_c = std::chrono::system_clock::to_time_t(now);
            now_tm = std::localtime(&now_c);
        } while (now_tm->tm_min != expected_min);
        std::map<int ,std::vector<int>> density;
        std::cout << timestamp() << BLU << "FIVE MINUTES! " << RESET << std::endl;
        {
            std::lock_guard<std::mutex> lock(per_lane_five_mutex);
            for (int i=1; i<=total_lane; ++i) {
                density[i].push_back(per_lane_total_five[i] / 4500);
                density[i].push_back(per_lane_min_five[i]);
                per_lane_min_five[i] = INT_MAX;
                density[i].push_back(per_lane_max_five[i]);
            }
            per_lane_total_five.clear();
            per_lane_max_five.clear();
        }
        std::cout << timestamp() << ": calling fetchStats() " << std::endl; 
        fetchStats(density, 3, 300);
        std::cout << timestamp() << ": fetchStats() returned" << std::endl;
    }
}

// 🔧 디렉토리 생성 유틸
bool createDirectoryIfNotExists(const std::string& path) {
    if (access(path.c_str(), F_OK) == -1) {
        if (mkdir(path.c_str(), 0755) == -1) {
            std::cerr << "Failed to create directory: " << path << std::endl;
            logger->error("Failed to create directory: {}", path);
            return false;
        }
    }
    return true;
}

// crop detected object and return cropped image
static cv::Mat
cropImage (NvBufSurface* surface, box bbox) {
    NvBufSurfTransform_Error err;
    NvBufSurfTransformParams transform_params;
    NvBufSurfTransformRect src_rect;
    NvBufSurfTransformRect dst_rect;

    NvBufSurface* new_surf = nullptr;

    gint src_left, src_top, src_width, src_height;
    src_left = (int)bbox.left - 15;
    src_top = (int)bbox.top - 15;
    src_width = (int)bbox.width + 15;
    src_height = (int)bbox.height + 15;

    NvBufSurfaceCreateParams create_params;
    create_params.gpuId = 0;
    create_params.width = src_width;
    create_params.height = src_height;
    create_params.size = 0;
    create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    create_params.layout = NVBUF_LAYOUT_PITCH;

#ifdef __aarch64__
    create_params.memType = NVBUF_MEM_DEFAULT;
#else
    create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
#endif

    if (NvBufSurfaceCreate(&new_surf, 1, &create_params) != 0) {
        std::cerr << "Failed to create new NvBufSurface" << std::endl;
        logger->error("Failed to create new NvBufSurface");
        return cv::Mat();
    }

    src_rect = {(guint)src_top, (guint)src_left, (guint)src_width, (guint)src_height};
    dst_rect = {0, 0, (guint)src_width, (guint)src_height};

    transform_params.src_rect = &src_rect;
    transform_params.dst_rect = &dst_rect;
    transform_params.transform_flag =
        NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_CROP_SRC |
        NVBUFSURF_TRANSFORM_CROP_DST;
    transform_params.transform_filter = NvBufSurfTransformInter_Default;

    NvBufSurfaceMemSet(new_surf, 0, 0, 0);
    err = NvBufSurfTransform(surface, new_surf, &transform_params);
    if (err != NvBufSurfTransformError_Success) {
        logger->error("Failed to transform nvbufsurface");
        NvBufSurfaceDestroy(new_surf);
        return cv::Mat();
    }
    if (NvBufSurfaceMap(new_surf, 0, 0, NVBUF_MAP_READ) != 0) {
        std::cerr << "Failed to map new surface" << std::endl;
        logger->error("Failed to map new surface");
        NvBufSurfaceDestroy(new_surf);
        return cv::Mat();
    }
    if (NvBufSurfaceSyncForCpu(new_surf, 0, 0) != 0) {
        std::cerr << "Failed to sync new surface for CPU" << std::endl;
        logger->error("Failed to sync new surface for CPU");
        NvBufSurfaceUnMap(new_surf, 0, 0);
        NvBufSurfaceDestroy(new_surf);
        return cv::Mat();
    }
    NvBufSurfaceParams* new_params = &new_surf->surfaceList[0];
    cv::Mat rgba_img(new_params->height, new_params->width, CV_8UC4, new_params->mappedAddr.addr[0], new_params->pitch);
    cv::Mat bgr_img;
    cv::cvtColor(rgba_img, bgr_img, cv::COLOR_RGBA2BGR);
    NvBufSurfaceUnMap(new_surf, 0, 0);
    NvBufSurfaceDestroy(new_surf);

    return bgr_img;
}

// take frame snapshot and return snapshot image
static cv::Mat
frameSnapshot (NvBufSurface* surface) {
    NvBufSurfaceParams* src_params = &surface->surfaceList[0];
    NvBufSurface* new_surf = nullptr;
    NvBufSurfaceCreateParams create_params;
    create_params.gpuId = 0;
    create_params.width = src_params->width;
    create_params.height = src_params->height;
    create_params.size = 0;
    create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    create_params.layout = NVBUF_LAYOUT_PITCH;
#ifdef __aarch64__
    create_params.memType = NVBUF_MEM_DEFAULT;
#else
    create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
#endif
    if (NvBufSurfaceCreate(&new_surf, 1, &create_params) != 0) {
        std::cerr << "Failed to create new NvBufSurface" << std::endl;
        logger->error("Failed to create new NvBufSurface");
        return cv::Mat();
    }
    new_surf->numFilled = surface->numFilled;
    if (NvBufSurfaceCopy(surface, new_surf) != 0) {
        std::cerr << "Failed to copy NvBufSurface" << std::endl;
        logger->error("Failed to copy NvBufSurface");
        NvBufSurfaceDestroy(new_surf);
        return cv::Mat();
    }
    if (NvBufSurfaceMap(new_surf, 0, 0, NVBUF_MAP_READ) != 0) {
        std::cerr << "Failed to map new surface" << std::endl;
        logger->error("Failed to map new surface");
        NvBufSurfaceDestroy(new_surf);
        return cv::Mat();
    }
    if (NvBufSurfaceSyncForCpu(new_surf, 0, 0) != 0) {
        std::cerr << "Failed to sync new surface for CPU" << std::endl;
        logger->error("Failed to sync new surface for CPU");
        NvBufSurfaceUnMap(new_surf, 0, 0);
        NvBufSurfaceDestroy(new_surf);
        return cv::Mat();
    }
    NvBufSurfaceParams* new_params = &new_surf->surfaceList[0];
    cv::Mat rgba_img(new_params->height, new_params->width, CV_8UC4, new_params->mappedAddr.addr[0], new_params->pitch);
    cv::Mat bgr_img;
    cv::cvtColor(rgba_img, bgr_img, cv::COLOR_RGBA2BGR);
    NvBufSurfaceUnMap(new_surf, 0, 0);
    NvBufSurfaceDestroy(new_surf);

    return bgr_img;
}
static int saveImage(cv::Mat& image, const std::string& file_path, const std::string& file_name ) {
    createDirectoryIfNotExists(file_path);
    
    std::vector<int> params_jpg;
    params_jpg.push_back(cv::IMWRITE_JPEG_QUALITY);
    params_jpg.push_back(95);
    std::string full_path = file_path + "/" + file_name;
    try {
        cv::imwrite(full_path, image, params_jpg);
        image_logger->info("Image Saved: [Image Name] : {}, [Image Path]: {}", file_name, full_path);
        return 0;
    } catch(exception& err) {
        image_logger->error("Image Save Error {} : {}", full_path, err.what());
        return -1;
    }
}
static void drawBbox(cv::Mat& image, int id) {
    obj_data& detected_object = det_obj[id];
    box& bb = detected_object.current_box;  
    cv::Point tl(static_cast<int>(bb.left),                  
                static_cast<int>(bb.top));                   
    cv::Point br(static_cast<int>(bb.left + bb.width),        
                static_cast<int>(bb.top  + bb.height));     
    const cv::Scalar ds_color_bgr(200, 50, 200);   
    const int thickness = 12;
    cv::rectangle(
        image,      
        tl, br,           
        ds_color_bgr,     
        thickness,       
        cv::LINE_AA       
    );
}
static void processPedJaywalk( int id, const ObjPoint& p1, int current_time, NvBufSurface* surface) {
    obj_data& detected_object = det_obj[id];

    if (roi_handler->isInNoPedZone(p1)) {
        if (!detected_object.jaywalk) {
            if (detected_object.jaywalk_start == 0)
                detected_object.jaywalk_start = current_time;
            else if (detected_object.jaywalk_start + 1 < current_time) {
                detected_object.jaywalk = true;
                cv::Mat frame_image = frameSnapshot(surface);
                drawBbox(frame_image, id);
                std::string image_name = std::to_string(id) + "_" + std::to_string(current_time) + "_" + std::to_string(3) + ".jpg";
                if (frame_image.empty())
                    logger->error("Failed to get frame snapshot : {}", image_name);
                else
                    saveImage(frame_image, BASE_PATH + "images", image_name);
                std::string jaywalk_query = QueryMaker::unexpectedIncidentQuery(id, current_time, 3);
                // std::string jaywalk_query = "Jaywalk Start Query";
                redis_client->redisSendQuery(jaywalk_query);
            }
        }
    }
    else if (detected_object.jaywalk) {
        detected_object.jaywalk = false;

        std::string jaywalk_query = QueryMaker::unexpectedIncidentUpdateQuery(redis_client->camID(), current_time, id, 3);
        // std::string jaywalk_query = "Jaywalk Update Query";
        redis_client->redisSendQuery(jaywalk_query);
    }
}
static void processPed(int id, const ObjPoint& p1, int current_time, NvBufSurface* surface) {
    obj_data& detected_object = det_obj[id];

    if (pedestrian_jaywalk_enabled_2k) {
        processPedJaywalk(id, p1, current_time, surface);
    }
    if (!detected_object.ped_pass) {
        if (roi_handler->isInCrossWalk(p1)) {
            if (detected_object.cross_out) {
                if (detected_object.prev_ped.size() == 15) {
                    bool ascending = true, descending = true;
                    for (size_t i=0; i<14; i++) {
                        if (detected_object.prev_ped[i].x > detected_object.prev_ped[i+1].x)
                            ascending = false;
                        if (detected_object.prev_ped[i].x < detected_object.prev_ped[i+1].x)
                            descending = false;
                    }
                    if (ascending) {
                        detected_object.ped_pass = true;
                        detected_object.ped_dir = 1;
            
                        std::string ped_query = QueryMaker::pedestrianQuery(redis_client->camID(), id, current_time, "R");
                        // std::string ped_query = "Pedestrian Direction Query";
                        redis_client->redisSendQuery(ped_query);
            
                        std::cout << timestamp() << BLU << "RIGHT dir pedestrian: " << id << ", " << detected_object.ped_dir << RESET << std::endl;
                    }
                    else if (descending) {
                        detected_object.ped_pass = true;
                        detected_object.ped_dir = -1;
            
                        std::string ped_query = QueryMaker::pedestrianQuery(redis_client->camID(), id, current_time, "L");
                        // std::string ped_query = "Pedestrian Direction Query";
                        redis_client->redisSendQuery(ped_query);
            
                        std::cout << timestamp() << RED << "LEFT dir pedestrian: " << id << ", " << detected_object.ped_dir << RESET << std::endl;
                    }
                    else {
                        detected_object.prev_ped.pop_front();
                        detected_object.prev_ped.push_back(p1);
                    }
                }
                else {
                    detected_object.prev_ped.push_back(p1);
                }
            }
        }
        else {
            detected_object.cross_out = true;
        }
    }
}
static void processWaitQueue() {
    std::lock_guard<std::mutex> traffic_lock(per_lane_traffic_mutex);
    std::lock_guard<std::mutex> five_lock(per_lane_five_mutex);
    for (int lane=1; lane<=total_lane; ++lane) {
        int count = per_lane_count[lane];
        per_lane_max[lane] = std::max(per_lane_max[lane], count);
        per_lane_max_five[lane] = std::max(per_lane_max_five[lane], count);
        per_lane_total[lane] += count;
        per_lane_total_five[lane] += count;
        per_lane_min[lane] = std::min(per_lane_min[lane], count);
        per_lane_min_five[lane] = std::min(per_lane_min_five[lane], count);
    }
}
static void processMoveReverse(int id, const ObjPoint& p1, int current_time, NvBufSurface* surface) {
    obj_data& detected_object = det_obj[id];

    if (!detected_object.move_reverse) {
        if (detected_object.prev_vehi.size() == 15) {
            bool ascending = true;
            for (size_t i=0; i<14; i++) {
                if (detected_object.prev_vehi[i].y <= detected_object.prev_vehi[i+1].y){
                    ascending = false;
                    break;
                }
            }
            if (ascending) {
                if (detected_object.speed > 15) {
                    detected_object.move_reverse = true;
                    cv::Mat frame_image = frameSnapshot(surface);
                    drawBbox(frame_image, id);
                    std::string image_name = std::to_string(id) + "_" + std::to_string(current_time) + "_" + std::to_string(4) + ".jpg";
                    if (frame_image.empty())
                        logger->error("Failed to get frame snapshot : {}", image_name);
                    else
                        saveImage(frame_image, BASE_PATH + "images", image_name);
                    std::string reverse_query = QueryMaker::unexpectedIncidentQuery(id, current_time, 4);
                    // std::string reverse_query = "Reverse Start Query";
                    redis_client->redisSendQuery(reverse_query);
                    std::cout << RED << "REVERSE CAR DETECTED: " << id << ", " << detected_object.lane << RESET << std::endl;
                }
            }
            else if(detected_object.move_reverse) {
                detected_object.move_reverse = false;
                std::string update_reverse_query = QueryMaker::unexpectedIncidentUpdateQuery(redis_client->camID(), current_time, id, 4);
                // std::string update_reverse_query = "Reverse Update Query";
                redis_client->redisSendQuery(update_reverse_query);
            }
            detected_object.prev_vehi.pop_front();
            detected_object.prev_vehi.push_back(p1);
            
        }
        else {
            detected_object.prev_vehi.push_back(p1);
        }
    }
}
static void updateMoveReverse(int id, int current_time) {
    obj_data& detected_object = det_obj[id];

    if (detected_object.move_reverse) {
        detected_object.move_reverse = false;
        std::string reverse_query = QueryMaker::unexpectedIncidentUpdateQuery(redis_client->camID(), current_time, id, 4);
        // std::string reverse_query = "Reverse Update Query";
        redis_client->redisSendQuery(reverse_query);
    }
}
static void processIllegalWait(int id, const ObjPoint& p1, int current_time, NvBufSurface* surface) {
    obj_data& detected_object = det_obj[id];
    if (detected_object.speed < 5.0 && roi_handler->isInInterROI(p1)) {
        if (detected_object.stop_sec == 0) {
            detected_object.stop_sec = current_time;
        }
        else if (detected_object.stop_sec + 3 < current_time && !detected_object.illegal_wait) {
            detected_object.illegal_wait = true;

            detected_object.illegal_wait_start_cycle = current_cycle.load();
            detected_object.illegal_wait_start_phase = current_phase.load();
            std::cout << timestamp() << YEL << "Illegal Waiting Car! : " << id << RESET << std::endl;
            cv::Mat frame_image = frameSnapshot(surface);
            drawBbox(frame_image, id);
            std::string image_name = std::to_string(id) + "_" + std::to_string(current_time) + "_" + std::to_string(1) + ".jpg";
            if (frame_image.empty())
                logger->error("Failed to get frame snapshot : {}", image_name);
            else 
                saveImage(frame_image, BASE_PATH + "images", image_name);
            std::string illegal_wait_query = QueryMaker::unexpectedIncidentQuery(id, current_time, 1);
            // std::string illegal_wait_query = "Illegal Wait Start Query";
            redis_client->redisSendQuery(illegal_wait_query);
        }
        else if (detected_object.illegal_wait) {
            if (!detected_object.tail_gate && detected_object.illegal_wait_start_phase != current_phase.load()) {
                detected_object.tail_gate = true;

                std::cout << timestamp() << MAG << "Tailgating Car! : " << id << RESET << std::endl;

                std::string illegal_wait_query = QueryMaker::unexpectedIncidentUpdateQuery(redis_client->camID(), current_time, id, 1);
                // std::string illegal_wait_query = "Illegal Update Query";
                redis_client->redisSendQuery(illegal_wait_query);       

                cv::Mat frame_image = frameSnapshot(surface);
                drawBbox(frame_image, id);
                std::string image_name = std::to_string(id) + "_" + std::to_string(current_time) + "_" + std::to_string(2) + ".jpg";
                if (frame_image.empty())
                    logger->error("Failed to get frame snapshot : {}", image_name);
                else
                    saveImage(frame_image, BASE_PATH + "images", image_name);
                std::string tailgate_query = QueryMaker::unexpectedIncidentQuery(id, current_time, 2);
                // std::string tailgate_query = "Tailgate Start Query";
                redis_client->redisSendQuery(tailgate_query);
            }
            else if (!detected_object.accident && detected_object.tail_gate && detected_object.illegal_wait_start_cycle + 1 < current_cycle.load()) {
                detected_object.accident = true;

                std::cout << timestamp() << RED << "Car Accident! : " << id << RESET << std::endl;

                std::string tailgate_query = QueryMaker::unexpectedIncidentUpdateQuery(redis_client->camID(), current_time, id, 2);
                // std::string tailgate_query = "Tailgate Update Query";
                redis_client->redisSendQuery(tailgate_query);

                cv::Mat frame_image = frameSnapshot(surface);
                drawBbox(frame_image, id);
                std::string image_name = std::to_string(id) + "_" + std::to_string(current_time) + "_" + std::to_string(5) + ".jpg";
                if (frame_image.empty())
                    logger->error("Failed to get frame snapshot : {}", image_name);
                else
                    saveImage(frame_image, BASE_PATH + "images", image_name);
                std::string accident_query = QueryMaker::unexpectedIncidentQuery(id, current_time, 5);
                // std::string accident_query = "Accident Start Query";
                redis_client->redisSendQuery(accident_query);
            }
        }
    }
    else {
        int event_type = 0;
        if (detected_object.accident) 
            event_type = 5;
        else if (detected_object.tail_gate) 
            event_type = 2;
        else if (detected_object.illegal_wait)
            event_type = 1;
        if (event_type != 0){
            std::string wait_query = QueryMaker::unexpectedIncidentUpdateQuery(redis_client->camID(), current_time, id, event_type);
            // std::string wait_query = "Wait Update Query";
            redis_client->redisSendQuery(wait_query);
        }
        detected_object.stop_sec = 0;
        detected_object.illegal_wait = false;
        detected_object.tail_gate = false;
        detected_object.accident = false;
    }
}
static void processRealtime2k(int id, const box& obj_box, int current_time, bool second_changed, NvBufSurface* surface) {
    ObjPoint p1 = {
        obj_box.left + obj_box.width * 0.5,
        obj_box.top + obj_box.height};
    
    
    obj_data& detected_object = det_obj[id];
    if (second_changed)
        updateSpeed(detected_object, p1);

    int lane = roi_handler->getLaneNum(p1);
    if (lane != 0){
        // object is IN lane ROI
        detected_object.lane = lane;
        if (wait_queue_enabled) {
            per_lane_count[lane]++;
        }
        
        if (move_reverse_enabled) {
            processMoveReverse(id, p1, current_time, surface);
        }
    }
    else {
        if (move_reverse_enabled) {
            updateMoveReverse(id, current_time);
        }
        
        if (illegal_wait_enabled) {
            processIllegalWait(id, p1, current_time, surface);
        }

        if (!detected_object.turn_pass){
            if (detected_object.stop_line_pass && detected_object.lane != 0) {
                int turn_type = roi_handler->isInTurnROI(p1);
                if (turn_type != -1){
                    detected_object.dir_out = turn_type;
                    detected_object.turn_pass = true;
                    detected_object.turn_time = current_time;
                    detected_object.turn_pass_speed = detected_object.speed;
                    std::cout << timestamp() << YEL << "turn object " << id << ", " << detected_object.label << ", " << detected_object.dir_out << ", " << detected_object.lane << ", " << detected_object.first_detected_time << ", " << detected_object.stop_pass_time << ", " << detected_object.turn_time << ", " << RESET << std::endl;
                    redis_client->redisSendData(id);
                }
            }
            else {
                ObjPoint before = detected_object.last_pos;
                ObjPoint current = p1;
                if (roi_handler->stopLinePassCheck(before, current)) {
                    detected_object.stop_line_pass = true;
                    detected_object.stop_pass_time = current_time;
                    detected_object.stop_pass_speed = detected_object.speed;
                    detected_object.image_name = std::to_string(id) + "_" + std::to_string(current_time) + ".jpg";
                    cv::Mat cropped_image = cropImage(surface, obj_box);
                    std::string image_name = std::to_string(id) + "_" + std::to_string(current_time) + ".jpg";
                    if (cropped_image.empty())
                        logger->error("Failed to crop image : {}", image_name);
                    else
                        saveImage(cropped_image, BASE_PATH + "car_images", image_name);
                }
                else if (detected_object.lane != 0) {
                    if (roi_handler->isInUTurnROI(p1)) {
                        detected_object.dir_out = 41;
                        detected_object.stop_line_pass = true;
                        detected_object.stop_pass_time = current_time;
                        detected_object.turn_pass = true;
                        detected_object.turn_time = current_time;
                        detected_object.turn_pass_speed = detected_object.speed;
                        detected_object.stop_pass_speed = detected_object.speed;
                        detected_object.image_name = std::to_string(id) + "_" + std::to_string(current_time) + ".jpg";
                        cv::Mat cropped_image = cropImage(surface, obj_box);
                        std::string image_name = std::to_string(id) + "_" + std::to_string(current_time) + ".jpg";
                        if (cropped_image.empty())
                            logger->error("Failed to crop image : {}", image_name);
                        else
                            saveImage(cropped_image, BASE_PATH + "car_images", image_name);
                        std::cout << timestamp() << BLU << "U turn object " << id << ", " << detected_object.label << ", " << detected_object.dir_out << ", " << detected_object.lane << ", " << detected_object.first_detected_time << ", " << detected_object.stop_pass_time << ", " << detected_object.turn_time << ", " << RESET << std::endl;
                        redis_client->redisSendData(id);
                    }
                    else{
                        int turn_type = roi_handler->isInTurnROI(p1);
                        if (turn_type != -1) {
                            detected_object.dir_out = turn_type;
                            detected_object.stop_line_pass = true;
                            detected_object.stop_pass_time = current_time;
                            detected_object.turn_pass = true;
                            detected_object.turn_time = current_time;
                            detected_object.turn_pass_speed = detected_object.speed;
                            detected_object.stop_pass_speed = detected_object.speed;
                            detected_object.image_name = std::to_string(id) + "_" + std::to_string(current_time) + ".jpg";
                            cv::Mat cropped_image = cropImage(surface, obj_box);
                            std::string image_name = std::to_string(id) + "_" + std::to_string(current_time) + ".jpg";
                            if (cropped_image.empty())
                                logger->error("Failed to crop image : {}", image_name);
                            else
                                saveImage(cropped_image, BASE_PATH + "car_images", image_name);
                            std::cout << timestamp() << RED << "Turn object " << id << ", " << detected_object.label << ", " << detected_object.dir_out << ", " << detected_object.lane << ", " <<  detected_object.first_detected_time << ", " << detected_object.stop_pass_time << ", " << detected_object.turn_time << ", " << RESET << std::endl;
                            redis_client->redisSendData(id);
                        }
                    }
                }

            }
        }
    }
    detected_object.last_pos = p1;
}
static void
processRealtime4k(int id, const box& obj_box, int current_time, bool second_changed, NvBufSurface* surface) {
    obj_data& detected_object = det_obj[id];
    ObjPoint before = detected_object.last_pos;
    ObjPoint p1 = {
        obj_box.left + obj_box.width * 0.5,
        obj_box.top + obj_box.height};
    
    if (second_changed)
        updateSpeed(detected_object, p1);
    
    if (!detected_object.stop_line_pass && roi_handler->stopLinePassCheck(before, p1)) 
        detected_object.stop_line_pass = true;

    if (detected_object.stop_line_pass) {
        if (!detected_object.turn_pass) {
            int lane = roi_handler->getLaneNum4k(before, p1);
            if (lane != 0) {
                detected_object.turn_pass = true;
                detected_object.lane = lane;
            }
        }
        else if (detected_object.image_count < 3) {
            detected_object.image_count++;
            cv::Mat cropped_image = cropImage(surface, obj_box);
            std::string image_name = std::to_string(id) + "_" + std::to_string(current_time) + "_" + 
                                    std::to_string(detected_object.lane) + "_" + detected_object.label + "_" + 
                                    std::to_string(detected_object.image_count) + ".jpg";
            if (cropped_image.empty())
                logger->error("Failed to crop image : {}", image_name);
            else
                saveImage(cropped_image, BASE_PATH + "images", image_name);
        }
    }
    detected_object.last_pos = p1;
    return;
}
// 매 프레임마다 실행됨
static void
process_meta(AppCtx *appCtx, NvDsBatchMeta *batch_meta, guint index, GstBuffer *buf)
{
    GstMapInfo in_map_info;
    memset(&in_map_info, 0, sizeof(in_map_info));

    if (!gst_buffer_map(buf, &in_map_info, GST_MAP_READ)) {
        logger->error("Failed to map gst buffer!");
        return;
    }

    NvBufSurface* surface = NULL;
    surface = (NvBufSurface *)in_map_info.data;

    int current_time = getCurTime();

    if (wait_queue_enabled && waiting_image_save.load() == true) {
        waiting_image_save.store(false);
        residual_timestamp.store(current_time);
        cv::Mat frame_image = frameSnapshot(surface);
        std::string image_name = std::to_string(current_time) + ".jpg";
        if (frame_image.empty())
            logger->error("Failed to get frame snapshot : {}", image_name);
        else
            saveImage(frame_image, BASE_PATH + "waiting_images", image_name);
    }

    std::lock_guard<std::mutex> lock(per_lane_count_mutex);
    per_lane_count.clear();
    static int last_second = -1;
    bool second_changed = false;
    if (current_time != last_second) {
        last_second = current_time;
        second_changed = true;
    }
    int smallest_id = INT_MAX;
    discardDeletedId();
    NvDsInstanceData *frame_data = &appCtx->instance_data[index];
    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta*) l_frame->data;
        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
            NvDsObjectMeta *obj = (NvDsObjectMeta *)l_obj->data;
            int id = obj->object_id;

            if (det_obj.count(id) == 0)
                det_obj[id].first_detected_time = current_time;
            
            det_obj[id].label = std::string(obj->obj_label);
            setBboxTextColor(appCtx, obj);

            box obj_box;
            obj_box.top = obj->rect_params.top;
            obj_box.height = obj->rect_params.height;
            obj_box.left = obj->rect_params.left;
            obj_box.width = obj->rect_params.width;
            obj_box.frame = frame_data->frame_num; 
            det_obj[id].current_box = obj_box;
            ObjPoint p1 = {
                obj_box.left + obj_box.width * 0.5,
                obj_box.top + obj_box.height};

            if (isVehicle(obj->obj_label)) {
                if (realtime_enabled_2k) {
                    processRealtime2k(id, obj_box, current_time, second_changed, surface);
                }
                if (realtime_enabled_4k) {
                    processRealtime4k(id, obj_box, current_time, second_changed, surface);
                }
            }
            else{
                if (pedestrian_enabled_2k) {
                    processPed(id, p1, current_time, surface);
                }
            }
        }
    }
    if (wait_queue_enabled) {
        processWaitQueue();
    }
    if (!roi_handler){
        std::cerr << RED << "ERROR: roi_handler is NULL" << RESET << std::endl;
    }
    else{
        roi_handler->overlayROI(batch_meta);
    }
    gst_buffer_unmap(buf, &in_map_info);
    return;
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
    /* 사내 테스트용 플래그 설정 */
    // ITS 4K
    // realtime_enabled_4k = true;

    // ITS 2K
    realtime_enabled_2k = true;
    pedestrian_enabled_2k = true;
    wait_queue_enabled = true;
    move_reverse_enabled = false;
    illegal_wait_enabled = true;
    pedestrian_jaywalk_enabled_2k = true;
    /* 사내 테스트용 플래그 설정 */

    if(logger == NULL){
        logger = getLogger("DS_log");
        image_logger = getLogger("DS_Image_log");
    }
    logger->info("Creating Pipeline...");
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

    _dsmeta_quark = g_quark_from_static_string(NVDS_META_STRING);
    appCtx->all_bbox_generated_cb = all_bbox_generated_cb;
    appCtx->bbox_generated_post_analytics_cb = bbox_generated_post_analytics_cb;
    appCtx->overlay_graphics_cb = overlay_graphics_cb;

    createDirectoryIfNotExists("/opt/nvidia/deepstream/deepstream-6.0/sources/objectDetector_GB/images");
    createDirectoryIfNotExists("/opt/nvidia/deepstream/deepstream-6.0/sources/objectDetector_GB/car_images");
    createDirectoryIfNotExists("/opt/nvidia/deepstream/deepstream-6.0/sources/objectDetector_GB/waiting_images");
    logger->info("Successfully Checked/Created Image Folders");
    // Creating named pipe for process_meta and tracker(libsort.so) to communicate
    if (mkfifo(DELETED_ID_PIPE, 0666) == -1) {
        if (errno != EEXIST) {
            perror("Error creating named pipe");
            return false;
        }
    }
    read_fd = open(DELETED_ID_PIPE, O_RDONLY | O_NONBLOCK);
    
    std::cout << "[SQLite] Version: " << GRN << sqlite3_libversion() << RESET << std::endl;
    logger->info("SQLite3 runtime version: {}", sqlite3_libversion());
    sqlite3* db_init = nullptr;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2("test.db", &db_init, flags, nullptr);
    if (rc) {
        std::cerr << RED << "[ERROR] database open failed: " << RESET << sqlite3_errmsg(db_init) << std::endl;
        logger->error("database open failed: {}", sqlite3_errmsg(db_init));
    }else{
        std::cout << "[MSG]" << GRN << " opened database succesfully in deepstream_app!" << RESET << std::endl;
        logger->info("opened database successfully in deepstream_app!");
        const char* create_table_sql = R"SQL(
            CREATE TABLE IF NOT EXISTS test_table(
                row_id INTEGER PRIMARY KEY AUTOINCREMENT,
                id INTEGER,
                turn_sensing_date INTEGER,
                stop_sensing_date INTEGER,
                first_detected_time INTEGER,
                label TEXT,
                lane INTEGER,
                dir_out INTEGER,
                turn_point_speed REAL,
                stop_point_speed REAL,
                interval_speed REAL,
                sensing_time INTEGER,
                image_name TEXT,
                timestamp INTEGER DEFAULT (strftime('%s', 'now'))
            );
            CREATE INDEX IF NOT EXISTS idx_timestamp ON test_table(timestamp);
            CREATE INDEX IF NOT EXISTS idx_dir_out ON test_table(dir_out);
            CREATE INDEX IF NOT EXISTS idx_lane ON test_table(lane);
            CREATE INDEX IF NOT EXISTS idx_label ON test_table(label);
        )SQL";

        char* err_msg = nullptr;
        if (sqlite3_exec(db_init, create_table_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
            std::cerr << RED << "[ERROR] Failed to create table/index: " << RESET << err_msg << std::endl;
            logger->error("Failed to create SQLite table/index: {}", err_msg);
            sqlite3_free(err_msg);
        } else {
            std::cout << "[MSG]" << GRN << " Table and indexes ensured OK." << RESET << std::endl;
            logger->info("SQLite table and indexes ensured OK.");
        }
    }
    sqlite3_close(db_init);

    try {
        roi_handler = std::make_unique<ROIHandler>(*appCtx);
    } catch (const std::exception& e) {
        std::exit(EXIT_FAILURE);
    }
    total_lane = roi_handler->lane_roi.size();
    redis_client = std::make_unique<RedisClient>(det_obj);
    // server = ServerManager::createServer("VoltDB");
    // server_receiver = std::make_unique<ServerReceiver>(server.get());
    if (realtime_enabled_2k) {
        std::thread fiveMinThread(fiveMinNotifier);
        fiveMinThread.detach();
    }
    // OSD(on screen display): 모니터가 화면상 직접 표시하는 기능
    if (config->osd_config.num_out_buffers < 8)
    {
        config->osd_config.num_out_buffers = 8;
    }

    pipeline->pipeline = gst_pipeline_new("pipeline"); // 파이프라인 생성
    if (!pipeline->pipeline)
    {
        NVGSTDS_ERR_MSG_V("Failed to create pipeline");
        goto done;
    }

    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline->pipeline));
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
    // gst_object_unref (fps_pad);

    NVGSTDS_ELEM_ADD_PROBE(latency_probe_id,
                           pipeline->instance_bins->sink_bin.sub_bins[0].sink, "sink",
                           latency_measurement_buf_prob, GST_PAD_PROBE_TYPE_BUFFER,
                           appCtx);
    latency_probe_id = latency_probe_id;

    if (config->num_message_consumers)
    {
        for (i = 0; i < config->num_message_consumers; i++)
        {
            /*
            appCtx->c2d_ctx[i] = start_cloud_to_device_messaging(
                &config->message_consumer_config[i], appCtx);
            if (appCtx->c2d_ctx[i] == NULL)
            {
                NVGSTDS_ERR_MSG_V("Failed to create message consumer");
                goto done;
            }
            */
        }
    }

    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(appCtx->pipeline.pipeline),
                                      GST_DEBUG_GRAPH_SHOW_ALL, "ds-app-null");

    g_mutex_init(&appCtx->app_lock);
    g_cond_init(&appCtx->app_cond);
    g_mutex_init(&appCtx->latency_lock);

    ret = TRUE;
done:
    if (!ret)
    {
        NVGSTDS_ERR_MSG_V("%s failed", __func__);
    }
    return ret;
}

/**
 * Function to destroy pipeline and release the resources, probes etc.
 */
void destroy_pipeline(AppCtx *appCtx)
{
    gint64 end_time;
    NvDsConfig *config = &appCtx->config;
    guint i;
    GstBus *bus = NULL;

    end_time = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

    if (!appCtx)
        return;

    if (appCtx->pipeline.demuxer)
    {
        gst_pad_send_event(gst_element_get_static_pad(appCtx->pipeline.demuxer,
                                                      "sink"),
                           gst_event_new_eos());
    }
    else if (appCtx->pipeline.instance_bins[0].sink_bin.bin)
    {
        gst_pad_send_event(gst_element_get_static_pad(appCtx->pipeline.instance_bins[0].sink_bin.bin, "sink"),
                           gst_event_new_eos());
    }

    g_usleep(100000);

    g_mutex_lock(&appCtx->app_lock);
    if (appCtx->pipeline.pipeline)
    {
        destroy_smart_record_bin(&appCtx->pipeline.multi_src_bin);
        bus = gst_pipeline_get_bus(GST_PIPELINE(appCtx->pipeline.pipeline));

        while (TRUE)
        {
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

    for (i = 0; i < appCtx->config.num_source_sub_bins; i++)
    {
        NvDsInstanceBin *bin = &appCtx->pipeline.instance_bins[i];
        if (config->osd_config.enable)
        {
            NVGSTDS_ELEM_REMOVE_PROBE(bin->all_bbox_buffer_probe_id,
                                      bin->osd_bin.nvosd, "sink");
        }
        else
        {
            NVGSTDS_ELEM_REMOVE_PROBE(bin->all_bbox_buffer_probe_id,
                                      bin->sink_bin.bin, "sink");
        }

        if (config->primary_gie_config.enable)
        {
            NVGSTDS_ELEM_REMOVE_PROBE(bin->primary_bbox_buffer_probe_id,
                                      bin->primary_gie_bin.bin, "src");
        }
    }
    if (appCtx->latency_info == NULL)
    {
        free(appCtx->latency_info);
        appCtx->latency_info = NULL;
    }

    destroy_sink_bin();
    g_mutex_clear(&appCtx->latency_lock);

    if (appCtx->pipeline.pipeline)
    {
        bus = gst_pipeline_get_bus(GST_PIPELINE(appCtx->pipeline.pipeline));
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
        gst_object_unref(appCtx->pipeline.pipeline);
    }

    if (config->num_message_consumers)
    {
        for (i = 0; i < config->num_message_consumers; i++)
        {
            if (appCtx->c2d_ctx[i])
                stop_cloud_to_device_messaging(appCtx->c2d_ctx[i]);
        }
    }

    return;
}

gboolean
pause_pipeline(AppCtx *appCtx)
{
    GstState cur;
    GstState pending;
    GstStateChangeReturn ret;
    GstClockTime timeout = 5 * GST_SECOND / 1000;

    ret =
        gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending,
                              timeout);

    if (ret == GST_STATE_CHANGE_ASYNC)
    {
        return FALSE;
    }

    if (cur == GST_STATE_PAUSED)
    {
        return TRUE;
    }
    else if (cur == GST_STATE_PLAYING)
    {
        gst_element_set_state(appCtx->pipeline.pipeline, GST_STATE_PAUSED);
        gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending,
                              GST_CLOCK_TIME_NONE);
        pause_perf_measurement(&appCtx->perf_struct);
        return TRUE;
    }
    else
    {
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

    ret =
        gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending,
                              timeout);

    if (ret == GST_STATE_CHANGE_ASYNC)
    {
        return FALSE;
    }

    if (cur == GST_STATE_PLAYING)
    {
        return TRUE;
    }
    else if (cur == GST_STATE_PAUSED)
    {
        gst_element_set_state(appCtx->pipeline.pipeline, GST_STATE_PLAYING);
        gst_element_get_state(appCtx->pipeline.pipeline, &cur, &pending,
                              GST_CLOCK_TIME_NONE);
        resume_perf_measurement(&appCtx->perf_struct);
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}