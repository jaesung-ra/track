/*
 * Copyright (c) 2018-2019, NVIDIA CORPORATION. All rights reserved.
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

#include "deepstream_app.h"
#include "deepstream_config_file_parser.h"
#include "nvds_version.h"
#include <string.h>
#include <unistd.h>
#include <exception>
#include <termios.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <math.h>
#include "calibration.h"
#include "process_meta.h"
#ifndef __logger__
#define __logger__
#include "logger.hpp"
#endif
// #endif
using namespace std;
#define MAX_INSTANCES 128
#define APP_TITLE "Global Bridge AI Traffic Data Collecting System (GBAI-ITS) Ver.100-2020"

#define DEFAULT_X_WINDOW_WIDTH 1920
#define DEFAULT_X_WINDOW_HEIGHT 1080

AppCtx *appCtx[MAX_INSTANCES];
static guint cintr = FALSE;
static GMainLoop *main_loop = NULL;
static gchar **cfg_files = NULL;
static gchar **input_files = NULL;
static gboolean print_version = FALSE;
static gboolean show_bbox_text = FALSE;
static gboolean print_dependencies_version = FALSE;
static gboolean quit = FALSE;
static gint return_value = 0;
static guint num_instances;
static guint num_input_files;
static GMutex fps_lock;
static gdouble fps[MAX_SOURCE_BINS];
static gdouble fps_avg[MAX_SOURCE_BINS];
static guint num_fps_inst = 0;

static int setting = 0;
static char dist[10] = "";
static int num_enable = 0;
static int enter_presed = 0;
static int show_source_info = 1;

static Display *display = NULL;
static Window windows[MAX_INSTANCES] = {0};

static gint source_ids[MAX_INSTANCES];

static GThread *x_event_thread = NULL;
static GMutex disp_lock;

static std::shared_ptr<spdlog::logger> logger = NULL;

GST_DEBUG_CATEGORY(NVDS_APP);

GOptionEntry entries[] = {
    {"version", 'v', 0, G_OPTION_ARG_NONE, &print_version,
     "Print DeepStreamSDK version", NULL},
    {"tiledtext", 't', 0, G_OPTION_ARG_NONE, &show_bbox_text,
     "Display Bounding box labels in tiled mode", NULL},
    {"version-all", 0, 0, G_OPTION_ARG_NONE, &print_dependencies_version,
     "Print DeepStreamSDK and dependencies version", NULL},
    {"cfg-file", 'c', 0, G_OPTION_ARG_FILENAME_ARRAY, &cfg_files,
     "Set the config file", NULL},
    {"input-file", 'i', 0, G_OPTION_ARG_FILENAME_ARRAY, &input_files,
     "Set the input file", NULL},
    {NULL},
};

bool startsWith(const char *pre, const char *str)
{ // 첫번째 인자 값 문자열이 두번째 인자 값 문자열의 앞에 문자열과 동일할 경우
    size_t lenpre = strlen(pre),
           lenstr = strlen(str);
    return lenstr < lenpre ? false : memcmp(pre, str, lenpre) == 0;
}

char convert_num_to_char(int num)
{ // 숫자키 값을 char형으로 변환
    return (char)(num - XK_0 + '0');
}

void add_num_to_dist(int num)
{ // dist배열의 맨 끝에 인자 값의 숫자키를 char형으로 변화하여 추가. 인자 값이 -1인경우 '.'을 붙인다.
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    try{
    int i = 0;
    while (dist[i] != '\0')
        i++;

    if (num == -1)
    {
        dist[i] = '.';
    }
    else
    {
        dist[i] = convert_num_to_char(num);
    }

    dist[i + 1] = '\0';
}
    catch(exception& err){
        // logger->error("add_num_to_char Error! - {}", err.what());
    }
}

double get_distance()
{ // dist배열에 저장된 값을 float형으로 변환. 값이 저장되어있지 않은경우 0을 반환.
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    try{
    return dist[0] == '\0' ? 0 : atof(dist);
}
    catch(exception& err){
        // logger->error("get_distance Error! - {}", err.what());
    }
}

/**
 * Callback function to be called once all inferences (Primary + Secondary)
 * are done. This is opportunity to modify content of the metadata.
 * e.g. Here Person is being replaced with Man/Woman and corresponding counts
 * are being maintained. It should be modified according to network classes
 * or can be removed altogether if not required.
 */

//TTAOGI - 오브젝트 id를 log로 남겨야할까..?
static void
all_bbox_generated(AppCtx *appCtx, GstBuffer *buf,
                   NvDsBatchMeta *batch_meta, guint index)
{ // 한 프레임 안에 포함되어있는 오브젝트 id를 리스트에 저장.
  // appctx의 primary_gie_config의 유니크 아이디와 동일하며 클래스 아이디가 0~128 범위인 경우에만 저장 
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    try{
        guint num_objects[128]; // 리스트 선언

        memset(num_objects, 0, sizeof(num_objects)); // 초기화
        // batch_mata: 검출결과, 트레킹결과 
    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL;
         l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)l_frame->data;
        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL;
             l_obj = l_obj->next)
        {
            NvDsObjectMeta *obj = (NvDsObjectMeta *)l_obj->data;
            if (obj->unique_component_id ==
                (gint)appCtx->config.primary_gie_config.unique_id)
            {
                if (obj->class_id >= 0 && obj->class_id < 128)
                {
                    num_objects[obj->class_id]++;
                }
            }
        }
    }
}
    catch(exception& err){
        // logger->error("all_bbox_generated Error! - {}", err.what());
    }
    return;
}

/**
 * Function to handle program interrupt signal.
 * It installs default handler after handling the interrupt.
 */
static void
_intr_handler(int signum)
{ // interupt 신호가 발생했을때 

    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    try{
    struct sigaction action;

    NVGSTDS_ERR_MSG_V("User Interrupted.. \n");

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
        // signum번호를 가지는 시그널이 발생했을 때 실행된 함수를 설치한다.
        // 함수외에도 SIG_DFL과 SIG_IGN을 지정할 수 있다. 전자는 시그널에 대한 기본행동을 후자는 시그널을 무시하기 위해서 사용한다.

    sigaction(SIGINT, &action, NULL);
        // SIGINT: 시그널 번호, 처리하려는 시그널의 정수형 값
        // &action: 새로 등록하려는 sigaction 구조체의 주소
        // NULL: sigaction 구조체에 대한 포인터를 하나 더 보내면 이 구조체를 현재 처리기의 내용으로 채워준다. 현재 처리기에 대해 신경쓸 필요 없으면  NULL로 설정.
    cintr = TRUE;
}
    catch(exception& err){
        // logger->error("_intr_handler Error! - signum : {}, err : {}", signum, err.what());
    }
    return;
}

/**
 * callback function to print the performance numbers of each stream.
 */
static void
perf_cb(gpointer context, NvDsAppPerfStruct *str)
{ // 성능 관련 콜백함수
    if(logger == NULL){
        logger = getLogger("DS_log");
    }

    try{
        static guint header_print_cnt = 0;
        guint i;
        AppCtx *appCtx = (AppCtx *)context;
        guint numf = (num_instances == 1) ? str->num_instances : num_instances;

        g_mutex_lock(&fps_lock);
        if (num_instances > 1)
        {
            fps[appCtx->index] = str->fps[0];
            fps_avg[appCtx->index] = str->fps_avg[0];
        }
        else
        {
            for (i = 0; i < numf; i++)
            {
                fps[i] = str->fps[i];
                fps_avg[i] = str->fps_avg[i];
            }
        }

        num_fps_inst++;
        if (num_fps_inst < num_instances)
        {
            g_mutex_unlock(&fps_lock);
            return;
        }

        num_fps_inst = 0;

        if (header_print_cnt % 20 == 0)
        {
            g_print("\n**PERF: ");
            for (i = 0; i < numf; i++)
            {
                g_print("FPS %d (Avg)\t", i);
            }
            g_print("\n");
            header_print_cnt = 0;
        }
        header_print_cnt++;
        // FILE *fp;
        // fp = fopen("/home/nvidia/Desktop/GB_log.txt", "a");

        g_print("**PERF: ");
        for (i = 0; i < numf; i++)
        {
            g_print("%.2f (%.2f)\t", fps[i], fps_avg[i]);
            logger->info("**PERF:  {:.4} ({:.4})", fps[i], fps_avg[i]);
            // fprintf(fp, "Current fps: %.2f\tAverage fps: %.2f\n", fps[i], fps_avg[i]);
            if (fps[i] < 2.5){
                logger->error("Camera FPS was 0. exiting...");
                // close(sock_cli);
                // fclose(fp);
                exit(102);
            }
        }
        // fclose(fp);

        g_print("\n");
        g_mutex_unlock(&fps_lock);
    }
    catch(exception& err){
        // logger->error("perf_cb error! - {}", err.what());
    }
    return;
}

/**
 * Loop function to check the status of interrupts.
 * It comes out of loop if application got interrupted.
 */
static gboolean
check_for_interrupt(gpointer data)
{
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    try{
        if (quit)
        {
            return FALSE;
        }

        if (cintr)
        {
            cintr = FALSE;

            quit = TRUE;
            g_main_loop_quit(main_loop);

            return FALSE;
        }
        }
    catch(exception& err){
        // logger->error("check_for_interrupt Error! = {}", err.what());
        return FALSE;
    }
    return TRUE;
}

/*
 * Function to install custom handler for program interrupt signal.
 */
static void
_intr_setup(void)
{
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    try{
        struct sigaction action; // 시그널 처리에 사용하는 sigaction구조체

    memset(&action, 0, sizeof(action));
        action.sa_handler = _intr_handler; //_intr_handler메소드를 지정

        sigaction(SIGINT, &action, NULL); // 핸들러 설정
}
    catch(exception& err){
        // logger->error("_intr_setup Error! - {}", err.what());
    }
    return;
}

static gboolean
kbhit(void)
{ // 키보드가 입력되었는지 확인하는 함수
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    try{
    struct timeval tv;
    fd_set rdfs;

    tv.tv_sec = 0;
    tv.tv_usec = 0;

    FD_ZERO(&rdfs);
    FD_SET(STDIN_FILENO, &rdfs);

    select(STDIN_FILENO + 1, &rdfs, NULL, NULL, &tv);
    return FD_ISSET(STDIN_FILENO, &rdfs);
}
    catch(exception& err){
        // logger->error("kbhit Error! - {}", err.what());
        return FALSE;
    }
}

/*
 * Function to enable / disable the canonical mode of terminal.
 * In non canonical mode input is available immediately (without the user
 * having to type a line-delimiter character).
 */
static void
changemode(int dir)
{ // 터미널의 모드 변경
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    try{
    static struct termios oldt, newt;

    if (dir == 1)
    {
        tcgetattr(STDIN_FILENO, &oldt); // 터미널 인터페이스 변수의 현재 값을 termios_p가 가리키는 구조체에 기록
        newt = oldt; // newt에 복사
        newt.c_lflag &= ~(ICANON); //newt 인스턴트 내 ICANON 플래그를 반전시킨다.
        tcsetattr(STDIN_FILENO, TCSANOW, &newt); //newt대로 터미널 인터페이스를 설정한다. (TSANOW == 바로 적용)
    }
    else
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt); //이전에 저장해둔 termios 구조체 대로 설정
}
    catch(exception& err){
        // logger->error("changemode Error! - dir : {}, err : {}", dir, err.what());
    }
}

static void
print_runtime_commands(void)
{ // 커맨드 설명을 출력
    g_print("\nRuntime commands:\n"
            "\th: Print this help\n"
            "\tq: Quit\n\n"
            "\tp: Pause\n"
            "\tr: Resume\n\n"
            "\ts: Setting\n"
            "\tl: Lane Setting\n"
            "\ti: Source info On/Off\n"
            "\tc: Cancel setting\n\n");

    if (appCtx[0]->config.tiled_display_config.enable)
    {
        g_print("NOTE: To expand a source in the 2D tiled display and view object details,"
                " left-click on the source.\n"
                "                        To go back to the tiled display, right-click anywhere on the window.\n\n");
    }
}

static guint rrow, rcol;
static gboolean rrowsel = FALSE, selecting = FALSE;

/**
 * Loop function to check keyboard inputs and status of each pipeline.
 */
static gboolean
event_thread_func(gpointer arg)
{
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }
    
    try{
        guint i;
        gboolean ret = TRUE;

        // Check if all instances have quit
        for (i = 0; i < num_instances; i++)
        {
            if (!appCtx[i]->quit)
                break;
        }

        if (i == num_instances)
        {
            quit = TRUE;
            g_main_loop_quit(main_loop);
            return FALSE;
        }
        // Check for keyboard input
        if (!kbhit())
        {
            //continue;
            return TRUE;
        }
        int c = fgetc(stdin);
        g_print("\n");

        gint source_id;
        GstElement *tiler = appCtx[0]->pipeline.tiled_display_bin.tiler;
        g_object_get(G_OBJECT(tiler), "show-source", &source_id, NULL);

        if (selecting)
        {
            if (rrowsel == FALSE)
            {
                if (c >= '0' && c <= '9')
                {
                    rrow = c - '0';
                        if (rrow < appCtx[0]->config.tiled_display_config.rows) // 타일의 행 개수가 입력받은 값보다 클때
                    {
                        g_print("--selecting source    row %d--\n", rrow);
                        rrowsel = TRUE;
                    }
                    else
                        {   //타일의 행 개수가 입력받은 값보다 작을 때 
                        g_print("--selected source    row %d out of bound, reenter\n", rrow);
                    }
                }
            }
            else
            {
                if (c >= '0' && c <= '9')
                {
                    unsigned int tile_num_columns = appCtx[0]->config.tiled_display_config.columns;
                    rcol = c - '0';
                    if (rcol < tile_num_columns) //타일의 열 개수가 입력받은 값보다 클 때
                    {
                        selecting = FALSE;
                        rrowsel = FALSE;
                        source_id = tile_num_columns * rrow + rcol;
                        g_print("--selecting source    col %d sou=%d--\n", rcol, source_id);
                        if (source_id >= (gint)appCtx[0]->config.num_source_sub_bins)
                        {
                            source_id = -1;
                        }
                        else
                        {
                            source_ids[0] = source_id;
                            // set_source_id(source_id);

                            appCtx[0]->show_bbox_text = TRUE;
                            g_object_set(G_OBJECT(tiler), "show-source", source_id, NULL);
                        }
                    }
                    else //타일의 열 개수가 입력받은 값보다 작을 때 
                    {
                        g_print("--selected source    col %d out of bound, reenter\n", rcol);
                    }
                }
            }
        }
        switch (c)
        {
        // case 's':
        //     record = 1;
        //     break;
        // case 'e':
        //     record = 0;
        //     break;
        case 'h': // 커맨드 도움말 출력
            print_runtime_commands();
            break;
        case 'p': // rtsp로 시작하지 않는 모든 인스턴스 파이프라인 정지
            for (i = 0; i < num_instances; i++){
                if (!startsWith("rtsp://", appCtx[i]->config.multi_source_config[0].uri))
                {
                    pause_pipeline(appCtx[i]);
                }
            }
            // logger->info("pause all pipeline.");
            break;
        case 'r': // 모든 파이프라인 재개
            for (i = 0; i < num_instances; i++){
                resume_pipeline(appCtx[i]);
            }
            // logger->info("resume all pipeline.");
            break;
        case 'q': // 종료
            // logger->info("q (quit) button pressed.");
            quit = TRUE;
            g_main_loop_quit(main_loop);
            ret = FALSE;
            break;
        case 'i':
            show_source_info = (show_source_info + 1) % 2;
            break;
        case 'z':
            if (source_id == -1 && selecting == FALSE)
            {
                //TTAOGI - 사용하게되면 나중에 로그 추가할것
                g_print("--selecting source --\n");
                selecting = TRUE;
            }
            else
            {
                if (!show_bbox_text)
                    appCtx[0]->show_bbox_text = FALSE;
                g_object_set(G_OBJECT(tiler), "show-source", -1, NULL);
                source_ids[0] = -1;
                selecting = FALSE;
                g_print("--tiled mode --\n");
            }
            break;
        // case 'y':
        //     g_print("What on Earh is going on?\n");

        //     break;
        default:
            break;
        }
        return ret;
    }
    catch(exception& err){
        // logger->error("event_thread_func Error! - {}", err.what());
        return FALSE;
    }
}

static int
get_source_id_from_coordinates(float x_rel, float y_rel)
{ // 타일의 위치를 입력받아 해당 소스의 id를 반환.
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    int source_id = -1;
    try{
        int tile_num_rows = appCtx[0]->config.tiled_display_config.rows; // 타일 행 번호
        int tile_num_columns = appCtx[0]->config.tiled_display_config.columns; // 타일 열 번호

        source_id = (int)(x_rel * tile_num_columns);
        source_id += ((int)(y_rel * tile_num_rows)) * tile_num_columns;

        /* Don't allow clicks on empty tiles. */
        if (source_id >= (gint)appCtx[0]->config.num_source_sub_bins)
            source_id = -1;
    }
    catch(exception& err){
        // logger->error("get_source_id_from_coordinates Error! - {}", err.what());
    }
    return source_id;
}

/**
 * Thread to monitor X window events.
 */
static gpointer
nvds_x_event_thread(gpointer data)
{
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    try{

        g_mutex_lock(&disp_lock);// 뮤텍스 잠금
        while (display)
        {
            XEvent e; // 이벤트 구조체
            guint index;
            while (XPending(display))
            {
                XNextEvent(display, &e);
                switch (e.type)
                {
                case ButtonPress:
                {
                    XWindowAttributes win_attr;
                    XButtonEvent ev = e.xbutton;
                    gint source_id;
                    GstElement *tiler;

                    XGetWindowAttributes(display, ev.window, &win_attr);

                    for (index = 0; index < MAX_INSTANCES; index++)
                        if (ev.window == windows[index])
                            break;
                    tiler = appCtx[index]->pipeline.tiled_display_bin.tiler;
                    g_object_get(G_OBJECT(tiler), "show-source", &source_id, NULL);

                    if (setting == 0)
                    {
                        if (ev.button == Button1 && source_id == -1)
                        {
                            source_id =
                                get_source_id_from_coordinates(ev.x * 1.0 / win_attr.width,
                                                            ev.y * 1.0 / win_attr.height);
                            if (source_id > -1)
                            {
                                g_object_set(G_OBJECT(tiler), "show-source", source_id, NULL);
                                // set_source_id(source_id);
                                source_ids[index] = source_id;
                                appCtx[index]->show_bbox_text = TRUE;
                            }
                        }
                        else if (ev.button == Button3)
                        {
                            g_object_set(G_OBJECT(tiler), "show-source", -1, NULL);
                            // set_source_id(-1);
                            source_ids[index] = -1;
                            if (!show_bbox_text)
                                appCtx[index]->show_bbox_text = FALSE;
                        }
                    }
                    
                }
                break;
                case KeyRelease:
                    break;
                case KeyPress:
                {
                    KeySym p, r, q, keyi, enter, dot;
                    guint i;

                    // General
                    p = XKeysymToKeycode(display, XK_P); // Pause
                    r = XKeysymToKeycode(display, XK_R); // Resume
                    q = XKeysymToKeycode(display, XK_Q); // Quit

                    // Deepstream Setting
                    keyi = XKeysymToKeycode(display, XK_I);
                    enter = XKeysymToKeycode(display, XK_Return);
                    dot = XKeysymToKeycode(display, XK_period);

                    // Pause Videostream
                    if (e.xkey.keycode == p)
                    {
                        for (i = 0; i < num_instances; i++)
                            if (!startsWith("rtsp://", appCtx[i]->config.multi_source_config[0].uri))
                            {
                                pause_pipeline(appCtx[i]);
                            }
                        break;
                    }
                    
                    // Resume Videostream
                    if (e.xkey.keycode == r)
                    {
                        for (i = 0; i < num_instances; i++)
                            resume_pipeline(appCtx[i]);
                        break;
                    }
                    
                    if (e.xkey.keycode == keyi)
                    {
                        show_source_info = (show_source_info + 1) % 2;
                        break;
                    }
                    if (e.xkey.keycode == dot)
                    {
                        if (num_enable == 1)
                        {
                            add_num_to_dist(-1);
                            printf("dist: %s\n", dist);
                        }
                        break;
                    }
                    if (e.xkey.keycode == enter)
                    {
                        if (num_enable == 1)
                        {
                            printf("Enter\n");
                            enter_presed = 1;
                            num_enable = 0;
                        }
                        break;
                    }
                    if (e.xkey.keycode == q)
                    {
                        quit = TRUE;
                        g_main_loop_quit(main_loop);
                    }
                    
                    
                }
                break;
            
                
                break;
                case ClientMessage:
                {
                    Atom wm_delete;
                    for (index = 0; index < MAX_INSTANCES; index++)
                        if (e.xclient.window == windows[index])
                            break;

                    wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", 1);
                    if (wm_delete != None && wm_delete == (Atom)e.xclient.data.l[0])
                    {
                        quit = TRUE;
                        g_main_loop_quit(main_loop);
                    }
                }
                break;
                }
            }
            g_mutex_unlock(&disp_lock);
            g_usleep(G_USEC_PER_SEC / 20);
            g_mutex_lock(&disp_lock);
        }
        
        g_mutex_unlock(&disp_lock);
    }
    catch(exception& err){
        // logger->error("nvds_x_event_thread Error! - {}", err.what());
    }
    return NULL;
}



/**
 * callback function to add application specific metadata.
 * Here it demonstrates how to display the URI of source in addition to
 * the text generated after inference.
 */
static gboolean
overlay_graphics(AppCtx *appCtx, GstBuffer *buf,
                 NvDsBatchMeta *batch_meta, guint index)
{
    return true;
}

#include <gst/gst.h>
#include <stdio.h>

static void
fprint_element_properties (FILE *out, GstElement *elem, gint level)
{
    gchar *name   = gst_element_get_name (elem);
    gchar *indent = g_strdup_printf ("%*s", level * 2, "");

    /* list all properties on this element’s class */
    guint n_props = 0;
    GParamSpec **props =
        g_object_class_list_properties (G_OBJECT_GET_CLASS (elem), &n_props);

    fprintf (out, "%s%s (%s)\n",
             indent, name,
             gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (gst_element_get_factory (elem))));

    for (guint i = 0; i < n_props; i++) {
        GParamSpec *pspec = props[i];

        /* skip write-only properties */
        if (!(pspec->flags & G_PARAM_READABLE))
            continue;

        GValue value = G_VALUE_INIT;
        g_value_init (&value, pspec->value_type);
        g_object_get_property (G_OBJECT (elem), pspec->name, &value);

        gchar *val_str = g_strdup_value_contents (&value);
        fprintf (out, "%s  |-- %s = %s\n", indent, pspec->name, val_str);

        g_free (val_str);
        g_value_unset (&value);
    }
    fprintf (out, "%s  -------------------------\n", indent);

    g_free (props);
    g_free (name);
    g_free (indent);
}

static void
fwalk_bin (FILE *out, GstBin *bin, gint level)
{
    GstIterator *it = gst_bin_iterate_elements (bin);
    GValue item = G_VALUE_INIT;

    while (gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
        GstElement *elem = GST_ELEMENT (g_value_get_object (&item));

        fprint_element_properties (out, elem, level);

        if (GST_IS_BIN (elem))
            fwalk_bin (out, GST_BIN (elem), level + 1);

        g_value_unset (&item);
    }
    gst_iterator_free (it);
}

/* ---------- public entry point ------------------------------------------ */

void
dump_pipeline_properties (GstElement *pipeline)
{
    /* overwrite (w) or create the file */
    FILE *out = fopen ("pipeline_config.txt", "w");
    if (!out) {
        g_printerr ("Could not open pipeline_config.txt for writing\n");
        return;
    }

    fprintf (out, "================= PIPELINE PROPERTY DUMP =================\n");
    fwalk_bin (out, GST_BIN (pipeline), 0);
    fprintf (out, "==========================================================\n");

    fclose (out);
}


int main(int argc, char *argv[])
{
    // if(logger == NULL){
    //     logger = getLogger("DS_log");
    // }

    GOptionContext *ctx = NULL;
    GOptionGroup *group = NULL;
    GError *error = NULL;
    guint i;

    ctx = g_option_context_new("Nvidia DeepStream Demo");
    group = g_option_group_new("abc", NULL, NULL, NULL, NULL);
    g_option_group_add_entries(group, entries);

    g_option_context_set_main_group(ctx, group);
    g_option_context_add_group(ctx, gst_init_get_option_group());

    GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, NULL);

    if (!g_option_context_parse(ctx, &argc, &argv, &error))
    {
        NVGSTDS_ERR_MSG_V("%s", error->message);
        return -1;
    }

    if (print_version)
    {
        g_print("deepstream-app version %d.%d.%d\n",
                NVDS_APP_VERSION_MAJOR, NVDS_APP_VERSION_MINOR, NVDS_APP_VERSION_MICRO);
        nvds_version_print();
        return 0;
    }

    if (print_dependencies_version)
    {
        g_print("deepstream-app version %d.%d.%d\n",
                NVDS_APP_VERSION_MAJOR, NVDS_APP_VERSION_MINOR, NVDS_APP_VERSION_MICRO);
        nvds_version_print();
        nvds_dependencies_version_print();
        return 0;
    }

    if (cfg_files)
    {
        num_instances = g_strv_length(cfg_files);
    }
    if (input_files)
    {
        num_input_files = g_strv_length(input_files);
    }

    memset(source_ids, -1, sizeof(source_ids));

    if (!cfg_files || num_instances == 0)
    {
        NVGSTDS_ERR_MSG_V("Specify config file with -c option");
        return_value = -1;
        goto done;
    }

    for (i = 0; i < num_instances; i++)
    {
        appCtx[i] = (AppCtx *)g_malloc0(sizeof(AppCtx));
        appCtx[i]->person_class_id = -1;
        appCtx[i]->car_class_id = -1;
        appCtx[i]->index = i;
        if (show_bbox_text)
        {
            appCtx[i]->show_bbox_text = TRUE;
        }

        if (input_files && input_files[i])
        {
            appCtx[i]->config.multi_source_config[0].uri =
                g_strdup_printf("file://%s", input_files[i]);
            g_free(input_files[i]);
        }

        if (!parse_config_file(&appCtx[i]->config, cfg_files[i]))
        {
            NVGSTDS_ERR_MSG_V("Failed to parse config file '%s'", cfg_files[i]);
            appCtx[i]->return_value = -1;
            // close(sock_cli);
            exit(104);
        }
    }

    for (i = 0; i < num_instances; i++)
    {
        if (!create_pipeline(appCtx[i], NULL,
                             all_bbox_generated, perf_cb, overlay_graphics))
        {
            NVGSTDS_ERR_MSG_V("Failed to create pipeline");
            return_value = -1;
            goto done;
        }
        // dump_pipeline_properties(appCtx[i]->pipeline.pipeline);
    }

    main_loop = g_main_loop_new(NULL, FALSE);

    _intr_setup();
    g_timeout_add(400, check_for_interrupt, NULL);

    g_mutex_init(&disp_lock);
    display = XOpenDisplay(NULL);
    // set_source_id(0);

    for (i = 0; i < num_instances; i++)
    {
        guint j;

        if (gst_element_set_state(appCtx[i]->pipeline.pipeline,
                                  GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
        {
            NVGSTDS_ERR_MSG_V("Failed to set pipeline to PAUSED");
            return_value = -1;
            goto done;
        }

        if (!appCtx[i]->config.tiled_display_config.enable)
            continue;

        for (j = 0; j < appCtx[i]->config.num_sink_sub_bins; j++)
        {
            XTextProperty xproperty;
            gchar *title;
            guint width, height;

            if (!GST_IS_VIDEO_OVERLAY(appCtx[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink))
            {
                continue;
            }

            if (!display)
            {
                NVGSTDS_ERR_MSG_V("Could not open X Display");
                return_value = -1;
                goto done;
            }

            if (appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.width)
                width =
                    appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.width;
            else
                width = appCtx[i]->config.tiled_display_config.width;

            if (appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.height)
                height =
                    appCtx[i]->config.sink_bin_sub_bin_config[j].render_config.height;
            else
                height = appCtx[i]->config.tiled_display_config.height;

            width = (width) ? width : DEFAULT_X_WINDOW_WIDTH;
            height = (height) ? height : DEFAULT_X_WINDOW_HEIGHT;
            height = round(width / ((float)appCtx[0]->config.tiled_display_config.width / appCtx[0]->config.tiled_display_config.height));

            windows[i] =
                XCreateSimpleWindow(display, RootWindow(display, DefaultScreen(display)), 0, 0, width, height, 2, 0x00000000,
                                    0x00000000);
            if (num_instances > 1)
                title = g_strdup_printf(APP_TITLE "-%d", i);
            else
                title = g_strdup(APP_TITLE);
            if (XStringListToTextProperty((char **)&title, 1, &xproperty) != 0)
            {
                XSetWMName(display, windows[i], &xproperty);
                XFree(xproperty.value);
            }

            XSetWindowAttributes attr = {0};
            if ((appCtx[i]->config.tiled_display_config.enable &&
                 appCtx[i]->config.tiled_display_config.rows *
                         appCtx[i]->config.tiled_display_config.columns ==
                     1) ||
                (appCtx[i]->config.tiled_display_config.enable == 0 &&
                 appCtx[i]->config.num_source_sub_bins == 1))
            {
                GstElement *tiler = appCtx[i]->pipeline.tiled_display_bin.tiler;
                g_object_set(G_OBJECT(tiler), "show-source", 0, NULL);
                // set_source_id(0);
                source_ids[i] = 0;
                appCtx[i]->show_bbox_text = TRUE;
            }
            attr.event_mask = ButtonPress | KeyRelease;
            XChangeWindowAttributes(display, windows[i], CWEventMask, &attr);

            Atom wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
            if (wmDeleteMessage != None)
            {
                XSetWMProtocols(display, windows[i], &wmDeleteMessage, 1);
            }
            XMapRaised(display, windows[i]);
            XSync(display, 1); // discard the events for now
            gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(appCtx[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink),
                                                (gulong)windows[i]);
            gst_video_overlay_expose(GST_VIDEO_OVERLAY(appCtx[i]->pipeline.instance_bins[0].sink_bin.sub_bins[j].sink));
            if (!x_event_thread)
                x_event_thread = g_thread_new("nvds-window-event-thread",
                                              nvds_x_event_thread, NULL);
        }
    }

    /* Dont try to set playing state if error is observed */
    if (return_value != -1)
    {
        for (i = 0; i < num_instances; i++)
        {
            if (gst_element_set_state(appCtx[i]->pipeline.pipeline,
                                      GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            {

                g_print("\ncan't set pipeline to playing state.\n");
                return_value = -1;
                goto done;
            }
        }
    }

    print_runtime_commands();

    changemode(1);

    g_timeout_add(40, event_thread_func, NULL);
    g_main_loop_run(main_loop);

    changemode(0);

done:

    g_print("Quitting\n");
    for (i = 0; i < num_instances; i++)
    {
        if (appCtx[i]->return_value == -1)
            return_value = -1;
        destroy_pipeline(appCtx[i]);

        g_mutex_lock(&disp_lock);
        if (windows[i])
            XDestroyWindow(display, windows[i]);
        windows[i] = 0;
        g_mutex_unlock(&disp_lock);

        g_free(appCtx[i]);
    }

    g_mutex_lock(&disp_lock);
    if (display)
        XCloseDisplay(display);
    display = NULL;
    g_mutex_unlock(&disp_lock);
    g_mutex_clear(&disp_lock);

    if (main_loop)
    {
        g_main_loop_unref(main_loop);
    }

    if (ctx)
    {
        g_option_context_free(ctx);
    }

    if (return_value == 0)
    {
        g_print("App run successful\n");
    }
    else
    {
        g_print("App run failed\n");
        printf("Close Sock!\n");
        exit(101);
    }

    gst_deinit();
    // printf("Close Sock!\n");
    // close(sock_cli);
    // if(logger != NULL){
    //     //closeLogger(logger);
    //     logger->error("Deepstream terminated!");
    //     spdlog::drop_all();
    // }
    return return_value;
}
