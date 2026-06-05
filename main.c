/*
 * 原始 ffmpeg 命令行播放器（已测试可用）
 * ffmpeg -re -i xxx.mp4 -vf scale=480:-2 -pix_fmt bgra -f fbdev /dev/fb0 -f alsa default
 */
#if 0
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>

int main()
{
    DIR *d = opendir("/mnt/sdcard");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *ext = strrchr(ent->d_name, '.');
            if (!ext) continue;
            if (strcasecmp(ext, ".mp4") == 0 ||
                strcasecmp(ext, ".mkv") == 0 ||
                strcasecmp(ext, ".avi") == 0) {
                char cmd[512];
                snprintf(cmd, sizeof(cmd),
                    "ffmpeg -re -i '/mnt/sdcard/%s' "
                    "-threads 2 -nostats -loglevel error "
                    "-vf 'scale=480:-2' "
                    "-pix_fmt bgra -f fbdev /dev/fb0 "
                    "-af aresample=48000 -f alsa default", ent->d_name);
                printf("cmd: %s\n", cmd);
                closedir(d);
                return system(cmd);
            }
        }
        closedir(d);
    }
    printf("no video found\n");
    return 1;
}
#endif

#include "main.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <strings.h>

#define CANVAS_W    480
#define CANVAS_H    480
#define MEDIA_DIR   "/mnt/sdcard"
#define AUDIO_OUT_RATE     48000
#define AUDIO_OUT_CHANNELS 2
#define AUDIO_OUT_BUF_SAMPLES 8192
#define AUDIO_QUEUE_SIZE   16

/* ── 播放列表项 ── */
struct pl_item {
    char      path[512];
    double    duration;      /* 秒，0 表示未探测 */
    lv_obj_t *item_obj;      /* 列表中的 UI 控件，给 timer 更新时长用 */
};

/* ── 播放器上下文，解码线程持续使用 ── */
struct video_ctx {
    AVFormatContext  *fmt_ctx;
    AVCodecContext   *dec_ctx;
    struct SwsContext *sws_ctx;
    AVFrame          *frame;
    AVPacket         *pkt;
    int               video_idx;
    int               off_x, off_y;
};

/* ── 音频上下文 ── */
struct audio_stream_ctx {
    int                audio_idx;
    AVCodecContext    *dec_ctx;
    struct SwrContext *swr_ctx;
    AVFrame           *frame;
    snd_pcm_t         *pcm_handle;
    int                out_channels;
    int                out_sample_rate;
    uint8_t           *out_buf;
    int                out_buf_samples;
    pthread_t           audio_tid;
    double             clock;
};

static struct video_ctx g_vctx;
static struct audio_stream_ctx  g_astx;

/* ── 音频包队列 ── */
static AVPacket        g_audio_queue[AUDIO_QUEUE_SIZE];
static int             g_aq_head;
static int             g_aq_tail;
static int             g_aq_count;
static pthread_mutex_t g_audio_mutex;
static pthread_cond_t  g_audio_cond;

/* ── 全局状态 ── */
static int               g_quit;
static lv_color_t        __attribute__((aligned(64))) g_canvas_buf[CANVAS_W * CANVAS_H];
static lv_obj_t         *g_canvas;

static pthread_t         g_vdec_tid;
static pthread_mutex_t   g_frame_lock;
static int               g_new_frame;
static int               g_disp_w, g_disp_h;
static double            g_frame_interval;
static uint8_t           __attribute__((aligned(64))) g_video_buf[2][CANVAS_W * CANVAS_H * 4];
static int               g_video_buf_idx;

/* ── 播放列表 ── */
static struct pl_item *g_playlist;
static int   g_playlist_count;
static int   g_playlist_cap;
static int   g_playlist_index;
static int   g_switch_file;
static int   g_skip_dir;
static int   g_pause;
static lv_obj_t *g_audio_label;
static lv_obj_t *g_file_label;
static lv_obj_t *g_pp_btn_label;
static lv_obj_t *g_list_btn;
static lv_obj_t *g_progress_slider;
static lv_obj_t *g_time_cur_label;
static lv_obj_t *g_time_total_label;
static double g_duration;

static lv_obj_t         *g_vol_slider;
static lv_obj_t         *g_vol_label;

/* ── 播放列表 UI ── */
static lv_obj_t     *g_playlist_page;
static lv_obj_t     *g_playlist_scroll;
static lv_timer_t   *g_probe_timer;
static int           g_probe_index;
static int           g_playlist_open;

static snd_ctl_t          *g_ctl;
static snd_ctl_elem_id_t  *g_ctl_id;
static long                g_ctl_vol_min, g_ctl_vol_max;

/* ── AV 同步 ── */
#define VIDEO_PKT_QUEUE_SIZE 8
static AVPacket g_video_pkt_queue[VIDEO_PKT_QUEUE_SIZE];
static int g_vpq_head, g_vpq_tail, g_vpq_count;
static double g_video_pts;

/* ── 信号处理 ── */
static void sig_handler(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void format_time(int sec, char *buf, int sz)
{
    int m = sec / 60;
    int s = sec % 60;
    snprintf(buf, sz, "%02d:%02d", m, s);
}

/* ── UI 回调 ── */
static void btn_quit_cb(lv_event_t *e)
{
    (void)e;
    g_quit = 1;
}

static void btn_pp_cb(lv_event_t *e)
{
    (void)e;
    g_pause = !g_pause;
    lv_label_set_text(g_pp_btn_label, g_pause ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

static void btn_prev_cb(lv_event_t *e)
{
    (void)e;
    if (g_playlist_open) {
        if (g_probe_timer) {
            lv_timer_del(g_probe_timer);
            g_probe_timer = NULL;
        }
        lv_obj_del(g_playlist_page);
        g_playlist_page = NULL;
        g_playlist_open = 0;
    }
    g_skip_dir = -1;
}

static void btn_next_cb(lv_event_t *e)
{
    (void)e;
    if (g_playlist_open) {
        if (g_probe_timer) {
            lv_timer_del(g_probe_timer);
            g_probe_timer = NULL;
        }
        lv_obj_del(g_playlist_page);
        g_playlist_page = NULL;
        g_playlist_open = 0;
    }
    g_skip_dir = +1;
}

/* ── 播放列表相关前置声明 ── */
static void playlist_page_create(void);

/* ── 播放列表浮层开关 ── */
static void list_btn_cb(lv_event_t *e)
{
    (void)e;
    if (g_playlist_open) {
        if (g_probe_timer) {
            lv_timer_del(g_probe_timer);
            g_probe_timer = NULL;
        }
        lv_obj_del(g_playlist_page);
        g_playlist_page = NULL;
        g_playlist_open = 0;
    } else {
        playlist_page_create();
        g_playlist_open = 1;
    }
}

/* ── 播放列表条目点击 ── */
static void playlist_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (g_probe_timer) {
        lv_timer_del(g_probe_timer);
        g_probe_timer = NULL;
    }
    lv_obj_del(g_playlist_page);
    g_playlist_page = NULL;
    g_playlist_open = 0;

    if (idx == g_playlist_index) return;

    g_skip_dir = idx - g_playlist_index;
    g_switch_file = 1;
}

#define CONFIG_PATH "/userdata/player.conf"

static void save_config(int step, int index)
{
    mkdir("/userdata", 0755);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "VOLUME=%d\n", step);
    fprintf(f, "INDEX=%d\n", index);
    fclose(f);
}

static int load_config(int *out_index)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return -1;
    int step = -1, index = -1;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "INDEX=%d", &index);
        sscanf(line, "VOLUME=%d", &step);
    }
    fclose(f);
    if (out_index) *out_index = index;
    if (step < 0 || step > 14) return -1;
    return step;
}

static void ctl_lazy_init(void)
{
    if (g_ctl) return;

    if (snd_ctl_open(&g_ctl, "hw:0", 0) < 0) {
        printf("[ctl] open hw:0 failed\n");
        return;
    }

    snd_ctl_elem_id_malloc(&g_ctl_id);

    snd_ctl_elem_info_t *info;
    snd_ctl_elem_info_alloca(&info);

    const char *names[] = {"Master Playback Volume", "Master", NULL};
    int found = 0;
    for (int i = 0; names[i]; i++) {
        snd_ctl_elem_id_set_interface(g_ctl_id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(g_ctl_id, names[i]);
        snd_ctl_elem_info_set_id(info, g_ctl_id);
        if (snd_ctl_elem_info(g_ctl, info) >= 0) {
            g_ctl_vol_min = snd_ctl_elem_info_get_min(info);
            g_ctl_vol_max = snd_ctl_elem_info_get_max(info);
            printf("[ctl] found '%s', range %ld..%ld\n", names[i],
                   g_ctl_vol_min, g_ctl_vol_max);
            found = 1;
            break;
        }
    }

    if (!found) {
        printf("[ctl] Master control not found\n");
        snd_ctl_close(g_ctl);
        g_ctl = NULL;
    }
}

static void vol_apply(int step)
{
    ctl_lazy_init();

    if (g_ctl) {
        int pct = (step * 100 + 7) / 14;
        if (pct > 100) pct = 100;
        if (pct < 0)   pct = 0;
        long vol = g_ctl_vol_min + (g_ctl_vol_max - g_ctl_vol_min) * pct / 100;

        snd_ctl_elem_value_t *val;
        snd_ctl_elem_value_alloca(&val);
        snd_ctl_elem_value_set_id(val, g_ctl_id);
        snd_ctl_elem_value_set_integer(val, 0, (int)vol);
        snd_ctl_elem_value_set_integer(val, 1, (int)vol);
        snd_ctl_elem_write(g_ctl, val);
    }

    save_config(step, g_playlist_index);
}

static void vol_step_down_cb(lv_event_t *e)
{
    (void)e;
    int step = (int)lv_slider_get_value(g_vol_slider);
    if (step > 0) {
        step--;
        lv_slider_set_value(g_vol_slider, step, LV_ANIM_ON);
        vol_apply(step);
    }
}

static void vol_step_up_cb(lv_event_t *e)
{
    (void)e;
    int step = (int)lv_slider_get_value(g_vol_slider);
    if (step < 14) {
        step++;
        lv_slider_set_value(g_vol_slider, step, LV_ANIM_ON);
        vol_apply(step);
    }
}

static int g_vol_init_done;

static void volume_init_slider(void)
{
    int saved_index = -1;
    int saved = load_config(&saved_index);
    if (saved < 0) saved = 7;
    g_vol_init_done = 0;
    lv_slider_set_value(g_vol_slider, saved, LV_ANIM_OFF);
    g_vol_init_done = 1;
    if (saved_index >= 0 && saved_index < g_playlist_count)
        g_playlist_index = saved_index;
    printf("[volume] init slider to step=%d, index=%d\n",
           saved, saved_index);
}

static void pcm_warmup(void)
{
    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
        return;

    snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
                       AUDIO_OUT_CHANNELS, AUDIO_OUT_RATE, 1, 500000);

    vol_apply((int)lv_slider_get_value(g_vol_slider));

    uint8_t silence[AUDIO_OUT_CHANNELS * 2 * 768];
    memset(silence, 0, sizeof(silence));
    snd_pcm_writei(pcm, silence, 768);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);

    printf("[pcm-warmup] done\n");
}

static void vol_slider_cb(lv_event_t *e)
{
    if (!g_vol_init_done) return;
    int step = (int)lv_slider_get_value(g_vol_slider);
    vol_apply(step);
}

/* 探测文件时长（秒），0 表示失败 */
static double probe_duration(const char *path)
{
    AVFormatContext *fmt = NULL;
    double dur = 0;
    if (avformat_open_input(&fmt, path, NULL, NULL) >= 0) {
        if (avformat_find_stream_info(fmt, NULL) >= 0) {
            if (fmt->duration != AV_NOPTS_VALUE)
                dur = (double)fmt->duration / AV_TIME_BASE;
        }
        avformat_close_input(&fmt);
    }
    return dur;
}

/* 懒加载时长：定时器回调，每次探测一首 */
static void probe_next_cb(lv_timer_t *timer)
{
    (void)timer;
    while (g_probe_index < g_playlist_count &&
           g_playlist[g_probe_index].duration > 0.01) {
        g_probe_index++;
    }
    if (g_probe_index >= g_playlist_count) {
        lv_timer_del(g_probe_timer);
        g_probe_timer = NULL;
        return;
    }

    double dur = probe_duration(g_playlist[g_probe_index].path);
    g_playlist[g_probe_index].duration = dur;

    /* 通过 item_obj 直接找到时长 label，不受滚动条子对象干扰 */
    lv_obj_t *item = g_playlist[g_probe_index].item_obj;
    if (item) {
        lv_obj_t *dur_lbl = lv_obj_get_child(item, 2);
        if (dur_lbl) {
            char buf[16];
            format_time((int)dur, buf, sizeof(buf));
            lv_label_set_text(dur_lbl, buf);
        }
    }
    g_probe_index++;
}

/* ── 创建播放列表浮层 ── */
static void playlist_page_create(void)
{
    /* 1. 浮层容器 */
    g_playlist_page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(g_playlist_page, 468, 600);
    lv_obj_align(g_playlist_page, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(g_playlist_page, lv_color_hex(0x1E1E2E), 0);
    lv_obj_set_style_bg_opa(g_playlist_page, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_playlist_page, 14, 0);
    lv_obj_set_style_pad_all(g_playlist_page, 0, 0);
    lv_obj_set_style_border_width(g_playlist_page, 0, 0);

    /* 2. 标题栏 */
    lv_obj_t *title = lv_label_create(g_playlist_page);
    lv_label_set_text(title, "Playlist");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);

    /* 关闭按钮 */
    lv_obj_t *close_btn = lv_btn_create(g_playlist_page);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, list_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 3. 可滚动曲目列表 */
    g_playlist_scroll = lv_obj_create(g_playlist_page);
    lv_obj_set_size(g_playlist_scroll, 452, 528);
    lv_obj_align(g_playlist_scroll, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_flex_flow(g_playlist_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_playlist_scroll, LV_DIR_VER);
    lv_obj_set_style_pad_all(g_playlist_scroll, 8, 0);
    lv_obj_set_style_pad_row(g_playlist_scroll, 2, 0);
    lv_obj_set_style_border_width(g_playlist_scroll, 0, 0);
    lv_obj_set_style_bg_opa(g_playlist_scroll, LV_OPA_TRANSP, 0);

    /* 4. 预设样式 */
    static lv_style_t style_item, style_item_pr, style_item_cur;
    static int style_inited;
    if (!style_inited) {
        lv_style_init(&style_item);
        lv_style_set_bg_opa(&style_item, LV_OPA_TRANSP);
        lv_style_set_radius(&style_item, 8);

        lv_style_init(&style_item_pr);
        lv_style_set_bg_opa(&style_item_pr, LV_OPA_20);
        lv_style_set_bg_color(&style_item_pr, lv_color_hex(0x4C4C6C));

        lv_style_init(&style_item_cur);
        lv_style_set_bg_opa(&style_item_cur, LV_OPA_30);
        lv_style_set_bg_color(&style_item_cur, lv_color_hex(0x3273DC));
        style_inited = 1;
    }

    /* 5. 遍历创建每条曲目 */
    static lv_style_t style_item_grid;
    static int grid_style_inited;
    if (!grid_style_inited) {
        lv_style_init(&style_item_grid);
        lv_style_set_layout(&style_item_grid, LV_LAYOUT_GRID);
        lv_style_set_pad_left(&style_item_grid, 12);
        lv_style_set_pad_right(&style_item_grid, 12);
        static lv_coord_t row_h[2] = {28, LV_GRID_TEMPLATE_LAST};
        lv_style_set_grid_row_dsc_array(&style_item_grid, row_h);
        static lv_coord_t col_w[4] = {
            LV_GRID_CONTENT, LV_GRID_FR(1),
            LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST
        };
        lv_style_set_grid_column_dsc_array(&style_item_grid, col_w);
        grid_style_inited = 1;
    }

    for (int i = 0; i < g_playlist_count; i++) {
        const char *full = g_playlist[i].path;
        const char *name = strrchr(full, '/') ?
                           strrchr(full, '/') + 1 : full;

        lv_obj_t *item = lv_obj_create(g_playlist_scroll);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, lv_pct(100), 58);
        lv_obj_add_style(item, &style_item_grid, 0);
        lv_obj_add_style(item, &style_item, 0);
        lv_obj_add_style(item, &style_item_pr, LV_STATE_PRESSED);
        if (i == g_playlist_index)
            lv_obj_add_style(item, &style_item_cur, 0);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        g_playlist[i].item_obj = item;  /* 保存引用，供 timer 更新时长 */

        lv_obj_add_event_cb(item, playlist_item_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* 序号 */
        lv_obj_t *idx_lbl = lv_label_create(item);
        if (i == g_playlist_index) {
            lv_label_set_text_fmt(idx_lbl, "%s %02d",
                                  LV_SYMBOL_PLAY, i + 1);
            lv_obj_set_style_text_color(idx_lbl,
                lv_color_hex(0x48C774), 0);
        } else {
            lv_label_set_text_fmt(idx_lbl, "  %02d", i + 1);
            lv_obj_set_style_text_color(idx_lbl,
                lv_color_hex(0x888888), 0);
        }
        lv_obj_set_style_text_font(idx_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_grid_cell(idx_lbl, LV_GRID_ALIGN_START,
            0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

        /* 文件名 */
        lv_obj_t *nm_lbl = lv_label_create(item);
        lv_label_set_text(nm_lbl, name);
        lv_obj_set_style_text_color(nm_lbl, lv_color_hex(0xE0E0E0), 0);
        lv_obj_set_style_text_font(nm_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_grid_cell(nm_lbl, LV_GRID_ALIGN_START,
            1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

        /* 时长 */
        lv_obj_t *dur_lbl = lv_label_create(item);
        double dur = g_playlist[i].duration;
        char tbuf[16];
        if (dur > 0.01) {
            int m = (int)dur / 60, s = (int)dur % 60;
            snprintf(tbuf, sizeof(tbuf), "%d:%02d", m, s);
        } else {
            snprintf(tbuf, sizeof(tbuf), "--:--");
        }
        lv_label_set_text(dur_lbl, tbuf);
        lv_obj_set_style_text_color(dur_lbl, lv_color_hex(0x9090A0), 0);
        lv_obj_set_style_text_font(dur_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_grid_cell(dur_lbl, LV_GRID_ALIGN_END,
            2, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    }

    /* 6. 滚动到当前曲目 */
    lv_obj_t *cur = g_playlist[g_playlist_index].item_obj;
    if (cur) lv_obj_scroll_to_view(cur, LV_ANIM_OFF);

    /* 7. 启动懒加载定时器，逐个探测时长 */
    g_probe_index = 0;
    g_probe_timer = lv_timer_create(probe_next_cb, 200, NULL);
}

/* ══════════════════════════════════════════════════════════════
 *  video_player_init : 打开文件，初始化解码器，不进入播放循环
 * ══════════════════════════════════════════════════════════════ */
static int video_player_init(const char *filename,
                       char *codec_name, int codec_name_sz,
                       int *fps_num, int *fps_den)
{
    AVCodec *codec = NULL;
    int ret = -1;

    memset(&g_vctx, 0, sizeof(g_vctx));

    /* ── 1. 打开文件 ── */
    if (avformat_open_input(&g_vctx.fmt_ctx, filename, NULL, NULL) < 0) {
        printf("[video] avformat_open_input failed\n");
        return -1;
    }

    if (avformat_find_stream_info(g_vctx.fmt_ctx, NULL) < 0) {
        printf("[video] avformat_find_stream_info failed\n");
        goto fail;
    }

    if (g_vctx.fmt_ctx->duration != AV_NOPTS_VALUE)
        g_duration = (double)g_vctx.fmt_ctx->duration / AV_TIME_BASE;
    else
        g_duration = 0.0;

    g_vctx.video_idx = av_find_best_stream(g_vctx.fmt_ctx,
                                            AVMEDIA_TYPE_VIDEO,
                                            -1, -1, &codec, 0);
    if (g_vctx.video_idx < 0) {
        printf("[video] no video stream\n");
        goto fail;
    }

    AVStream         *vs  = g_vctx.fmt_ctx->streams[g_vctx.video_idx];
    AVCodecParameters *par = vs->codecpar;

    snprintf(codec_name, codec_name_sz, "%s",
             codec->name ? codec->name : "unknown");
    *fps_num = vs->avg_frame_rate.num;
    *fps_den = vs->avg_frame_rate.den;

    printf("[video] file    : %s\n", filename);
    printf("[video] codec   : %s\n", codec_name);
    printf("[video] size    : %d x %d\n", par->width, par->height);

    /* ── 2. 计算缩放后尺寸，保持宽高比 ── */
    {
        double scale = (double)CANVAS_W / par->width;
        double scale_h = (double)CANVAS_H / par->height;
        if (scale_h < scale) scale = scale_h;
        g_disp_w = (int)(par->width  * scale);
        g_disp_h = (int)(par->height * scale);
        printf("[video] display : %d x %d (fit %dx%d)\n",
               g_disp_w, g_disp_h, CANVAS_W, CANVAS_H);
    }

    /* ── 3. 居中偏移 ── */
    g_vctx.off_y = (CANVAS_H - g_disp_h) / 2;
    g_vctx.off_x = (CANVAS_W - g_disp_w) / 2;

    /* ── 4. 帧间隔 ── */
    g_frame_interval = (*fps_den) ? (double)(*fps_den) / (*fps_num)
                                  : (1.0 / 30.0);
    printf("[video] fps     : %d/%d (%.2f), interval %.1f ms\n",
           *fps_num, *fps_den,
           *fps_den ? (double)*fps_num / *fps_den : 0.0,
           g_frame_interval * 1000.0);

    /* ── 5. 打开解码器 ── */
    g_vctx.dec_ctx = avcodec_alloc_context3(codec);
    if (!g_vctx.dec_ctx) {
        printf("[video] avcodec_alloc_context3 failed\n");
        goto fail;
    }
    avcodec_parameters_to_context(g_vctx.dec_ctx, par);
    if (avcodec_open2(g_vctx.dec_ctx, codec, NULL) < 0) {
        printf("[video] avcodec_open2 failed\n");
        goto fail;
    }

    /* ── 6. 颜色转换器 ── */
    g_vctx.sws_ctx = sws_getContext(
        par->width, par->height, g_vctx.dec_ctx->pix_fmt,
        g_disp_w, g_disp_h, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!g_vctx.sws_ctx) {
        printf("[video] sws_getContext failed\n");
        goto fail;
    }

    /* ── 7. 帧 / 包缓冲 ── */
    g_vctx.frame = av_frame_alloc();
    g_vctx.pkt   = av_packet_alloc();
    if (!g_vctx.frame || !g_vctx.pkt) {
        printf("[video] alloc frame/pkt failed\n");
        goto fail;
    }

    /* ── 8. 互斥锁 ── */
    pthread_mutex_init(&g_frame_lock, NULL);
    g_new_frame = 0;
    g_video_pts = 0.0;
    g_video_buf_idx = 0;

    printf("[video] init OK\n");
    return 0;

fail:
    if (g_vctx.pkt)      av_packet_free(&g_vctx.pkt);
    if (g_vctx.frame)    av_frame_free(&g_vctx.frame);
    if (g_vctx.sws_ctx)  sws_freeContext(g_vctx.sws_ctx);
    if (g_vctx.dec_ctx) {
        avcodec_close(g_vctx.dec_ctx);
        avcodec_free_context(&g_vctx.dec_ctx);
    }
    if (g_vctx.fmt_ctx)  avformat_close_input(&g_vctx.fmt_ctx);
    memset(&g_vctx, 0, sizeof(g_vctx));
    return ret;
}

/* ══════════════════════════════════════════════════════════════
 *  audio_out_thread : 从队列取包 → 解码 → swr → ALSA，独立线程
 * ══════════════════════════════════════════════════════════════ */
static void *audio_out_thread(void *arg)
{
    (void)arg;

    while (1) {
        AVPacket pkt = {0};

        pthread_mutex_lock(&g_audio_mutex);
        while (g_aq_count == 0 && !g_quit) {
            pthread_cond_wait(&g_audio_cond, &g_audio_mutex);
        }
        if (g_aq_count == 0) {
            pthread_mutex_unlock(&g_audio_mutex);
            break;
        }
        if (g_pause) {
            pthread_mutex_unlock(&g_audio_mutex);
            usleep(10000);
            continue;
        }
        av_packet_move_ref(&pkt, &g_audio_queue[g_aq_head]);
        g_aq_head = (g_aq_head + 1) % AUDIO_QUEUE_SIZE;
        g_aq_count--;
        pthread_cond_signal(&g_audio_cond);
        pthread_mutex_unlock(&g_audio_mutex);

        int r = avcodec_send_packet(g_astx.dec_ctx, &pkt);
        if (r >= 0 || r == AVERROR(EAGAIN)) {
            while (avcodec_receive_frame(g_astx.dec_ctx,
                                         g_astx.frame) == 0) {
                uint8_t *out = g_astx.out_buf;
                int out_nb = swr_convert(g_astx.swr_ctx,
                    &out, g_astx.out_buf_samples,
                    (const uint8_t **)g_astx.frame->data,
                    g_astx.frame->nb_samples);
                if (out_nb > 0) {
                    int wr = snd_pcm_writei(g_astx.pcm_handle,
                                            g_astx.out_buf, out_nb);
                    if (wr < 0) {
                        if (wr == -EPIPE)
                            snd_pcm_prepare(g_astx.pcm_handle);
                        break;
                    }
                }

                if (g_astx.frame->pts != AV_NOPTS_VALUE) {
                    double raw = g_astx.frame->pts *
                        av_q2d(g_vctx.fmt_ctx
                               ->streams[g_astx.audio_idx]->time_base);
                    snd_pcm_sframes_t delay;
                    if (snd_pcm_delay(g_astx.pcm_handle, &delay) == 0 && delay > 0)
                        g_astx.clock = raw - (double)delay / AUDIO_OUT_RATE;
                    else
                        g_astx.clock = raw;
                }
            }
        }

        av_packet_unref(&pkt);
    }

    printf("[audio-out] exit\n");
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  audio_stream_init : 找音频流 → 解码器 → swr → ALSA
 * ══════════════════════════════════════════════════════════════ */
static int audio_stream_init(void)
{
    AVCodec *codec = NULL;

    memset(&g_astx, 0, sizeof(g_astx));

    g_astx.audio_idx = av_find_best_stream(g_vctx.fmt_ctx,
                                            AVMEDIA_TYPE_AUDIO,
                                            -1, -1, &codec, 0);
    if (g_astx.audio_idx < 0) {
        printf("[audio-stream] no audio stream, skip\n");
        return 0;
    }

    AVStream         *as  = g_vctx.fmt_ctx->streams[g_astx.audio_idx];
    AVCodecParameters *par = as->codecpar;

    printf("[audio-stream] codec   : %s\n",
           codec->name ? codec->name : "unknown");
    printf("[audio-stream] sample  : %d Hz, %d ch, fmt %d\n",
           par->sample_rate, par->channels, par->format);

    g_astx.dec_ctx = avcodec_alloc_context3(codec);
    if (!g_astx.dec_ctx) {
        printf("[audio-stream] avcodec_alloc_context3 failed\n");
        return -1;
    }
    avcodec_parameters_to_context(g_astx.dec_ctx, par);
    if (avcodec_open2(g_astx.dec_ctx, codec, NULL) < 0) {
        printf("[audio-stream] avcodec_open2 failed\n");
        return -1;
    }

    /* ── swr 重采样 ── */
    {
        int64_t in_ch_layout = g_astx.dec_ctx->channel_layout;
        if (!in_ch_layout)
            in_ch_layout = av_get_default_channel_layout(
                g_astx.dec_ctx->channels);

        g_astx.swr_ctx = swr_alloc_set_opts(NULL,
            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, AUDIO_OUT_RATE,
            in_ch_layout, g_astx.dec_ctx->sample_fmt,
            g_astx.dec_ctx->sample_rate,
            0, NULL);
        if (!g_astx.swr_ctx) {
            printf("[audio-stream] swr_alloc_set_opts failed\n");
            return -1;
        }
        if (swr_init(g_astx.swr_ctx) < 0) {
            printf("[audio-stream] swr_init failed\n");
            return -1;
        }
    }

    /* ── 输出缓冲 ── */
    g_astx.out_buf_samples = AUDIO_OUT_BUF_SAMPLES;
    av_samples_alloc(&g_astx.out_buf, NULL, AUDIO_OUT_CHANNELS,
                     g_astx.out_buf_samples, AV_SAMPLE_FMT_S16, 0);

    /* ── ALSA PCM ── */
    {
        int ret = snd_pcm_open(&g_astx.pcm_handle, "default",
                               SND_PCM_STREAM_PLAYBACK, 0);
        if (ret < 0) {
            printf("[audio-stream] snd_pcm_open failed: %s\n", snd_strerror(ret));
            return -1;
        }

        ret = snd_pcm_set_params(g_astx.pcm_handle,
            SND_PCM_FORMAT_S16,
            SND_PCM_ACCESS_RW_INTERLEAVED,
            AUDIO_OUT_CHANNELS,
            AUDIO_OUT_RATE,
            1,
            500000);
        if (ret < 0) {
            printf("[audio-stream] snd_pcm_set_params failed: %s\n",
                   snd_strerror(ret));
            return -1;
        }
    }

    vol_apply((int)lv_slider_get_value(g_vol_slider));
    snd_pcm_prepare(g_astx.pcm_handle);

    g_astx.out_channels   = AUDIO_OUT_CHANNELS;
    g_astx.out_sample_rate = AUDIO_OUT_RATE;

    g_astx.frame = av_frame_alloc();

    /* ── 创建音频线程 ── */
    pthread_mutex_init(&g_audio_mutex, NULL);
    pthread_cond_init(&g_audio_cond, NULL);
    pthread_create(&g_astx.audio_tid, NULL, audio_out_thread, NULL);

    printf("[audio-stream] init OK\n");
    return 0;
}

static void vpq_flush(void)
{
    while (g_vpq_count > 0) {
        av_packet_unref(&g_video_pkt_queue[g_vpq_head]);
        g_vpq_head = (g_vpq_head + 1) % VIDEO_PKT_QUEUE_SIZE;
        g_vpq_count--;
    }
}

/* ══════════════════════════════════════════════════════════════
 *  video_decode_thread : 解码线程，循环读帧 → 写 canvas
 * ══════════════════════════════════════════════════════════════ */
static void *video_decode_thread(void *arg)
{
    (void)arg;
    int frame_count = 0;
    int audio_ok = (g_astx.audio_idx >= 0 && g_astx.dec_ctx != NULL);

    vpq_flush();

    while (!g_quit && !g_switch_file) {

        if (g_pause) { usleep(20000); continue; }

        {
            int aq;
            pthread_mutex_lock(&g_audio_mutex);
            aq = g_aq_count;
            pthread_mutex_unlock(&g_audio_mutex);

            if (aq >= AUDIO_QUEUE_SIZE - 3
                && (!g_vpq_count || g_new_frame)) {
                usleep((unsigned)(g_frame_interval * 500000));
                continue;
            }
        }

        /* ── 阶段1: 优先进音频包，补满队列 ── */
        {
            int audio_full;
            do {
                int r = av_read_frame(g_vctx.fmt_ctx, g_vctx.pkt);
                if (r < 0) {
                    if (r == AVERROR_EOF) {
                        printf("[vdec] EOF\n");
                        g_switch_file = 1;
                    } else {
                        printf("[vdec] read error %d\n", r);
                    }
                    goto exit_loop;
                }

                if (audio_ok && g_vctx.pkt->stream_index
                                == g_astx.audio_idx) {
                    pthread_mutex_lock(&g_audio_mutex);
                    if (g_aq_count < AUDIO_QUEUE_SIZE) {
                        av_packet_move_ref(
                            &g_audio_queue[g_aq_tail], g_vctx.pkt);
                        g_aq_tail = (g_aq_tail + 1)
                                    % AUDIO_QUEUE_SIZE;
                        g_aq_count++;
                        pthread_cond_signal(&g_audio_cond);
                    }
                    pthread_mutex_unlock(&g_audio_mutex);
                } else if (g_vctx.pkt->stream_index
                           == g_vctx.video_idx) {
                    if (g_vpq_count < VIDEO_PKT_QUEUE_SIZE) {
                        av_packet_move_ref(
                            &g_video_pkt_queue[g_vpq_tail],
                            g_vctx.pkt);
                        g_vpq_tail = (g_vpq_tail + 1)
                                     % VIDEO_PKT_QUEUE_SIZE;
                        g_vpq_count++;
                    }
                }

                av_packet_unref(g_vctx.pkt);

                if (g_aq_count >= AUDIO_QUEUE_SIZE - 3)
                    audio_full = 1;
                else
                    audio_full = 0;
            } while (!audio_full
                     && g_vpq_count < VIDEO_PKT_QUEUE_SIZE);
        }

        if (g_switch_file || g_quit) break;

        /* ── 阶段2: 解码一个视频帧 ── */
        if (g_vpq_count > 0) {
            int can_decode;
            pthread_mutex_lock(&g_frame_lock);
            can_decode = !g_new_frame;
            pthread_mutex_unlock(&g_frame_lock);

            if (!can_decode) { usleep(2000); continue; }

            int rr = avcodec_send_packet(g_vctx.dec_ctx,
                                         &g_video_pkt_queue[g_vpq_head]);
            av_packet_unref(&g_video_pkt_queue[g_vpq_head]);
            g_vpq_head = (g_vpq_head + 1) % VIDEO_PKT_QUEUE_SIZE;
            g_vpq_count--;

            if (rr >= 0 || rr == AVERROR(EAGAIN)) {
                while (avcodec_receive_frame(g_vctx.dec_ctx,
                                             g_vctx.frame) == 0) {
                    int ready;
                    do {
                        pthread_mutex_lock(&g_frame_lock);
                        ready = !g_new_frame;
                        pthread_mutex_unlock(&g_frame_lock);
                        if (!ready) usleep(500);
                    } while (!ready);

                    int wbuf = 1 - g_video_buf_idx;
                    uint8_t *dst[4]={g_video_buf[wbuf],NULL,NULL,NULL};
                    int dst_s[4]={g_disp_w*4,0,0,0};

                    sws_scale(g_vctx.sws_ctx,
                        (const uint8_t *const*)g_vctx.frame->data,
                        g_vctx.frame->linesize,
                        0, g_vctx.frame->height, dst, dst_s);

                    double pts = g_vctx.frame->pts;
                    if (pts == AV_NOPTS_VALUE)
                        pts = g_vctx.frame->best_effort_timestamp;
                    if (pts != AV_NOPTS_VALUE) {
                        AVStream *vs =
                            g_vctx.fmt_ctx->streams[g_vctx.video_idx];
                        g_video_pts = pts * av_q2d(vs->time_base);
                    }

                    pthread_mutex_lock(&g_frame_lock);
                    g_video_buf_idx = wbuf;
                    g_new_frame = 1;
                    pthread_mutex_unlock(&g_frame_lock);

                    frame_count++;
                }
            }
        } else {
            usleep(2000);
        }
    }

exit_loop:
    vpq_flush();
    printf("[vdec] thread exit, total %d frames\n", frame_count);
    return NULL;
}

/* ══════════════════════════════════════════════════════════════
 *  audio_stream_deinit : 停止 ALSA，释放音频资源
 * ══════════════════════════════════════════════════════════════ */
static void audio_stream_deinit(void)
{
    int i;

    /* 中断阻塞的 snd_pcm_writei，使音频线程快退 */
    if (g_astx.pcm_handle)
        snd_pcm_drop(g_astx.pcm_handle);

    /* 唤醒 cond_wait 中的音频线程 */
    pthread_mutex_lock(&g_audio_mutex);
    pthread_cond_signal(&g_audio_cond);
    pthread_mutex_unlock(&g_audio_mutex);

    if (g_astx.audio_tid) {
        pthread_join(g_astx.audio_tid, NULL);
        g_astx.audio_tid = 0;
    }

    /* 排空队列残余 */
    for (i = 0; i < g_aq_count; i++) {
        int idx = (g_aq_head + i) % AUDIO_QUEUE_SIZE;
        av_packet_unref(&g_audio_queue[idx]);
    }
    g_aq_head = g_aq_tail = g_aq_count = 0;

    pthread_mutex_destroy(&g_audio_mutex);
    pthread_cond_destroy(&g_audio_cond);

    if (g_astx.pcm_handle) {
        snd_pcm_close(g_astx.pcm_handle);
        g_astx.pcm_handle = NULL;
    }

    av_freep(&g_astx.out_buf);

    if (g_astx.frame) av_frame_free(&g_astx.frame);

    if (g_astx.swr_ctx) swr_free(&g_astx.swr_ctx);

    if (g_astx.dec_ctx) {
        avcodec_close(g_astx.dec_ctx);
        avcodec_free_context(&g_astx.dec_ctx);
    }

    memset(&g_astx, 0, sizeof(g_astx));
    printf("[audio-stream] deinit done\n");
}

/* ══════════════════════════════════════════════════════════════
 *  video_player_deinit : 停止线程，清理资源
 * ══════════════════════════════════════════════════════════════ */
static void video_player_deinit(void)
{
    g_quit = 1;
    g_pause = 0;
    pthread_cond_signal(&g_audio_cond);

    /* 唤醒可能在 cond_wait 的音频线程 */
    pthread_mutex_lock(&g_audio_mutex);
    pthread_cond_signal(&g_audio_cond);
    pthread_mutex_unlock(&g_audio_mutex);

    if (g_vdec_tid) {
        pthread_join(g_vdec_tid, NULL);
        g_vdec_tid = 0;
    }

    audio_stream_deinit();

    pthread_mutex_destroy(&g_frame_lock);

    if (g_vctx.pkt)      av_packet_free(&g_vctx.pkt);
    if (g_vctx.frame)    av_frame_free(&g_vctx.frame);
    if (g_vctx.sws_ctx)  sws_freeContext(g_vctx.sws_ctx);
    if (g_vctx.dec_ctx) {
        avcodec_close(g_vctx.dec_ctx);
        avcodec_free_context(&g_vctx.dec_ctx);
    }
    if (g_vctx.fmt_ctx)  avformat_close_input(&g_vctx.fmt_ctx);

    vpq_flush();

    printf("[video] deinit done\n");
}

/* ══════════════════════════════════════════════════════════════
 *  find_media_file : 扫描 MEDIA_DIR 找到第一个视频文件
 * ══════════════════════════════════════════════════════════════ */
#define PLAYLIST_INIT_CAP 128

static int has_ext(const char *name)
{
    const char *exts[] = { ".mp3", ".mp4", ".mkv", ".avi", ".wav", ".flac", NULL };
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(ext, exts[i]) == 0) return 1;
    return 0;
}

static int build_playlist(void)
{
    DIR *d = opendir(MEDIA_DIR);
    if (!d) {
        printf("[scan] opendir %s failed\n", MEDIA_DIR);
        return -1;
    }

    g_playlist_cap = PLAYLIST_INIT_CAP;
    g_playlist = malloc(g_playlist_cap * sizeof(struct pl_item));
    if (!g_playlist) { closedir(d); return -1; }
    g_playlist_count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_ext(ent->d_name)) continue;
        if (g_playlist_count >= g_playlist_cap) {
            g_playlist_cap *= 2;
            struct pl_item *tmp = realloc(g_playlist,
                                 g_playlist_cap * sizeof(struct pl_item));
            if (!tmp) { printf("[scan] realloc failed\n"); break; }
            g_playlist = tmp;
        }
        snprintf(g_playlist[g_playlist_count].path, 512,
                 MEDIA_DIR "/%s", ent->d_name);
        g_playlist[g_playlist_count].duration = 0;
        printf("[scan] #%d: %s\n", g_playlist_count,
               g_playlist[g_playlist_count].path);
        g_playlist_count++;
    }
    closedir(d);

    printf("[scan] total %d files\n", g_playlist_count);
    return g_playlist_count > 0 ? 0 : -1;
}

static void free_playlist(void)
{
    free(g_playlist);
    g_playlist = NULL;
    g_playlist_count = g_playlist_cap = 0;
}

/* ══════════════════════════════════════════════════════════════
 *  纯音频播放（单线程，不依赖 g_vctx / g_astx）
 * ══════════════════════════════════════════════════════════════ */
struct audio_player_ctx {
    AVFormatContext  *fmt_ctx;
    AVCodecContext   *dec_ctx;
    struct SwrContext *swr_ctx;
    AVFrame          *frame;
    snd_pcm_t        *pcm_handle;
    uint8_t          *out_buf;
    int               out_buf_samples;
    int               audio_idx;
    double            clock;
};

static void *audio_player_thread(void *arg)
{
    struct audio_player_ctx *ctx = (struct audio_player_ctx *)arg;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return NULL;

    while (!g_quit) {
        if (g_pause) { usleep(20000); continue; }
        int r = av_read_frame(ctx->fmt_ctx, pkt);
        if (r < 0) {
            if (r == AVERROR_EOF) {
                printf("[audio-player] EOF\n");
                g_switch_file = 1;
                break;
            }
            printf("[audio-player] av_read_frame error: %d\n", r);
            break;
        }

        if (pkt->stream_index == ctx->audio_idx) {
            r = avcodec_send_packet(ctx->dec_ctx, pkt);
            if (r >= 0 || r == AVERROR(EAGAIN)) {
                while (avcodec_receive_frame(ctx->dec_ctx,
                                             ctx->frame) == 0) {
                    uint8_t *out = ctx->out_buf;
                    int out_nb = swr_convert(ctx->swr_ctx,
                        &out, ctx->out_buf_samples,
                        (const uint8_t **)ctx->frame->data,
                        ctx->frame->nb_samples);
                    if (out_nb > 0) {
                        int wr = snd_pcm_writei(ctx->pcm_handle,
                                                ctx->out_buf, out_nb);
                        if (wr < 0) {
                            if (wr == -EPIPE)
                                snd_pcm_prepare(ctx->pcm_handle);
                            break;
                        }
                    }

                    if (ctx->frame->pts != AV_NOPTS_VALUE) {
                        double raw = ctx->frame->pts *
                            av_q2d(ctx->fmt_ctx
                                   ->streams[ctx->audio_idx]->time_base);
                        snd_pcm_sframes_t delay;
                        if (snd_pcm_delay(ctx->pcm_handle, &delay) == 0 && delay > 0)
                            ctx->clock = raw - (double)delay / AUDIO_OUT_RATE;
                        else
                            ctx->clock = raw;
                    }
                }
            }
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    printf("[audio-player] thread exit\n");
    return NULL;
}

static void audio_player_deinit(struct audio_player_ctx *ctx)
{
    if (!ctx) return;

    if (ctx->pcm_handle) {
        snd_pcm_drop(ctx->pcm_handle);
        snd_pcm_close(ctx->pcm_handle);
        ctx->pcm_handle = NULL;
    }

    av_freep(&ctx->out_buf);

    if (ctx->frame)       av_frame_free(&ctx->frame);
    if (ctx->swr_ctx)     swr_free(&ctx->swr_ctx);
    if (ctx->dec_ctx) {
        avcodec_close(ctx->dec_ctx);
        avcodec_free_context(&ctx->dec_ctx);
    }
    if (ctx->fmt_ctx) avformat_close_input(&ctx->fmt_ctx);

    memset(ctx, 0, sizeof(*ctx));
    printf("[audio-player] deinit done\n");
}

static int audio_player_init(const char *filename,
                              struct audio_player_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    AVCodec *codec = NULL;

    printf("[audio-player] file: %s\n", filename);

    if (avformat_open_input(&ctx->fmt_ctx, filename, NULL, NULL) < 0) {
        printf("[audio-player] avformat_open_input failed\n");
        return -1;
    }
    if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0) {
        printf("[audio-player] avformat_find_stream_info failed\n");
        return -1;
    }

    if (ctx->fmt_ctx->duration != AV_NOPTS_VALUE)
        g_duration = (double)ctx->fmt_ctx->duration / AV_TIME_BASE;
    else
        g_duration = 0.0;
    ctx->clock = 0.0;

    ctx->audio_idx = av_find_best_stream(ctx->fmt_ctx,
                                          AVMEDIA_TYPE_AUDIO,
                                          -1, -1, &codec, 0);
    if (ctx->audio_idx < 0) {
        printf("[audio-player] no audio stream\n");
        return -1;
    }

    AVStream *as = ctx->fmt_ctx->streams[ctx->audio_idx];
    AVCodecParameters *par = as->codecpar;
    printf("[audio-player] codec: %s, %d Hz, %d ch\n",
           codec->name, par->sample_rate, par->channels);

    ctx->dec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->dec_ctx) return -1;
    avcodec_parameters_to_context(ctx->dec_ctx, par);
    if (avcodec_open2(ctx->dec_ctx, codec, NULL) < 0) return -1;

    {
        int64_t in_layout = ctx->dec_ctx->channel_layout;
        if (!in_layout)
            in_layout = av_get_default_channel_layout(
                ctx->dec_ctx->channels);

        ctx->swr_ctx = swr_alloc_set_opts(NULL,
            AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, AUDIO_OUT_RATE,
            in_layout, ctx->dec_ctx->sample_fmt,
            ctx->dec_ctx->sample_rate, 0, NULL);
        if (!ctx->swr_ctx || swr_init(ctx->swr_ctx) < 0) return -1;
    }

    ctx->out_buf_samples = AUDIO_OUT_BUF_SAMPLES;
    av_samples_alloc(&ctx->out_buf, NULL, AUDIO_OUT_CHANNELS,
                     ctx->out_buf_samples, AV_SAMPLE_FMT_S16, 0);

    if (snd_pcm_open(&ctx->pcm_handle, "default",
                     SND_PCM_STREAM_PLAYBACK, 0) < 0) return -1;
    if (snd_pcm_set_params(ctx->pcm_handle,
        SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED,
        AUDIO_OUT_CHANNELS, AUDIO_OUT_RATE, 1, 500000) < 0) return -1;

    vol_apply((int)lv_slider_get_value(g_vol_slider));
    snd_pcm_prepare(ctx->pcm_handle);

    ctx->frame = av_frame_alloc();
    printf("[audio-player] init OK\n");
    return 0;
}

static int has_video_ext(const char *filename)
{
    const char *exts[] = { ".mp4", ".mkv", ".avi", NULL };
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(ext, exts[i]) == 0) return 1;
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    char buf[128];
    char codec_name[32];
    int  fps_num, fps_den;

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    av_log_set_level(AV_LOG_ERROR);

    if (build_playlist() < 0) {
        printf("[main] no media found\n");
        return 1;
    }

    lv_port_init(480, 854, 0);

    /* ── canvas ── */
    g_canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(g_canvas, g_canvas_buf, CANVAS_W, CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(g_canvas, lv_color_black(), LV_OPA_COVER);
    lv_obj_align(g_canvas, LV_ALIGN_TOP_MID, 0, 0);

    /* ── Audio Only 标签 ── */
    g_audio_label = lv_label_create(g_canvas);
    lv_label_set_text(g_audio_label, "Audio Only");
    lv_obj_set_style_text_color(g_audio_label,
        lv_color_hex(0x0099FF), 0);
    lv_obj_set_style_text_font(g_audio_label,
        &lv_font_montserrat_48, 0);
    lv_obj_center(g_audio_label);
    lv_obj_add_flag(g_audio_label, LV_OBJ_FLAG_HIDDEN);

    /* ═══ 底部控制区域 (Grid 布局) ═══
     *  行0: 进度条+时间  |  行1: 文件名  |  行2: 播放控制
     *  行3: 音量控制    |  行4: 弹性间距  |  行5: 播放列表按钮(左下角) */
    {
        lv_obj_t *bottom = lv_obj_create(lv_scr_act());
        lv_obj_set_size(bottom, 480, 374);
        lv_obj_align(bottom, LV_ALIGN_TOP_MID, 0, 480);
        lv_obj_set_style_pad_all(bottom, 10, 0);
        lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bottom, 0, 0);
        lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE |
                                   LV_OBJ_FLAG_CLICKABLE);

        static const lv_coord_t cols[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
        static const lv_coord_t rows[] = {
            30,               /* row 0: 进度条行 */
            38,               /* row 1: 文件名行 */
            62,               /* row 2: 控制按钮行  */
            54,               /* row 3: 音量控制行  */
            LV_GRID_FR(1),    /* row 4: 弹性间距     */
            64,               /* row 5: 播放列表按钮 */
            LV_GRID_TEMPLATE_LAST
        };
        lv_obj_set_grid_dsc_array(bottom, cols, rows);

        /* ── row 0: 进度条 ── */
        {
            lv_obj_t *row = lv_obj_create(bottom);
            lv_obj_remove_style_all(row);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(row, lv_pct(100), lv_pct(100));
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);

            g_time_cur_label = lv_label_create(row);
            lv_label_set_text(g_time_cur_label, "00:00");
            lv_obj_set_style_text_font(g_time_cur_label,
                &lv_font_montserrat_14, 0);
            lv_obj_set_width(g_time_cur_label, 50);

            g_progress_slider = lv_slider_create(row);
            lv_obj_set_flex_grow(g_progress_slider, 1);
            lv_obj_set_height(g_progress_slider, 10);
            lv_slider_set_range(g_progress_slider, 0, 1000);
            lv_obj_set_style_pad_hor(g_progress_slider, 8, 0);

            g_time_total_label = lv_label_create(row);
            lv_label_set_text(g_time_total_label, "00:00");
            lv_obj_set_style_text_font(g_time_total_label,
                &lv_font_montserrat_14, 0);
            lv_obj_set_width(g_time_total_label, 50);

            lv_obj_set_grid_cell(row, LV_GRID_ALIGN_STRETCH,
                                 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);
        }

        /* ── row 1: 文件名 ── */
        {
            lv_obj_t *row = lv_obj_create(bottom);
            lv_obj_remove_style_all(row);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(row, lv_pct(100), lv_pct(100));
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);

            g_file_label = lv_label_create(row);
            lv_label_set_text(g_file_label, "---");
            lv_obj_set_style_text_font(g_file_label,
                &lv_font_montserrat_24, 0);
            lv_label_set_long_mode(g_file_label,
                LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_width(g_file_label, 440);

            lv_obj_set_grid_cell(row, LV_GRID_ALIGN_STRETCH,
                                 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);
        }

        /* ── row 2: 控制按钮 ── */
        {
            lv_obj_t *row = lv_obj_create(bottom);
            lv_obj_remove_style_all(row);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(row, lv_pct(100), lv_pct(100));
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 14, 0);

            lv_obj_t *btn, *lbl;
            int btn_w = 80, btn_h = 54;

            btn = lv_btn_create(row);
            lv_obj_set_size(btn, btn_w, btn_h);
            lbl = lv_label_create(btn);
            lv_label_set_text(lbl, LV_SYMBOL_PREV);
            lv_obj_center(lbl);
            lv_obj_add_event_cb(btn, btn_prev_cb, LV_EVENT_CLICKED, NULL);

            btn = lv_btn_create(row);
            lv_obj_set_size(btn, btn_w, btn_h);
            g_pp_btn_label = lv_label_create(btn);
            lv_label_set_text(g_pp_btn_label, LV_SYMBOL_PAUSE);
            lv_obj_center(g_pp_btn_label);
            lv_obj_add_event_cb(btn, btn_pp_cb, LV_EVENT_CLICKED, NULL);

            btn = lv_btn_create(row);
            lv_obj_set_size(btn, btn_w, btn_h);
            lbl = lv_label_create(btn);
            lv_label_set_text(lbl, LV_SYMBOL_NEXT);
            lv_obj_center(lbl);
            lv_obj_add_event_cb(btn, btn_next_cb, LV_EVENT_CLICKED, NULL);

            lv_obj_set_grid_cell(row, LV_GRID_ALIGN_STRETCH,
                                 0, 1, LV_GRID_ALIGN_CENTER, 2, 1);
        }

        /* ── row 3: 音量控制 ── */
        {
            lv_obj_t *row = lv_obj_create(bottom);
            lv_obj_remove_style_all(row);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(row, lv_pct(100), lv_pct(100));
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);

            g_vol_label = lv_label_create(row);
            lv_label_set_text(g_vol_label, LV_SYMBOL_VOLUME_MID);
            lv_obj_set_style_text_font(g_vol_label,
                &lv_font_montserrat_24, 0);
            lv_obj_set_width(g_vol_label, 36);

            lv_obj_t *btn, *lbl;

            btn = lv_btn_create(row);
            lv_obj_set_size(btn, 44, 44);
            lbl = lv_label_create(btn);
            lv_label_set_text(lbl, LV_SYMBOL_MINUS);
            lv_obj_center(lbl);
            lv_obj_add_event_cb(btn, vol_step_down_cb,
                LV_EVENT_CLICKED, NULL);

            g_vol_slider = lv_slider_create(row);
            lv_obj_set_flex_grow(g_vol_slider, 1);
            lv_obj_set_height(g_vol_slider, 10);
            lv_slider_set_range(g_vol_slider, 0, 14);
            lv_obj_add_event_cb(g_vol_slider, vol_slider_cb,
                LV_EVENT_VALUE_CHANGED, NULL);
            lv_obj_set_style_pad_hor(g_vol_slider, 8, 0);

            btn = lv_btn_create(row);
            lv_obj_set_size(btn, 44, 44);
            lbl = lv_label_create(btn);
            lv_label_set_text(lbl, LV_SYMBOL_PLUS);
            lv_obj_center(lbl);
            lv_obj_add_event_cb(btn, vol_step_up_cb,
                LV_EVENT_CLICKED, NULL);

            lv_obj_set_grid_cell(row, LV_GRID_ALIGN_STRETCH,
                                 0, 1, LV_GRID_ALIGN_CENTER, 3, 1);
        }

        /* ── row 5: 播放列表按钮 (左下角) ── */
        {
            lv_obj_t *row = lv_obj_create(bottom);
            lv_obj_remove_style_all(row);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(row, lv_pct(100), lv_pct(100));
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(row, 4, 0);

            g_list_btn = lv_btn_create(row);
            lv_obj_set_size(g_list_btn, 80, 54);
            lv_obj_t *lbl2 = lv_label_create(g_list_btn);
            lv_label_set_text(lbl2, LV_SYMBOL_LIST);
            lv_obj_center(lbl2);
            lv_obj_add_event_cb(g_list_btn, list_btn_cb,
                LV_EVENT_CLICKED, NULL);

            lv_obj_set_grid_cell(row, LV_GRID_ALIGN_STRETCH,
                                 0, 1, LV_GRID_ALIGN_CENTER, 5, 1);
        }
    }

#if 0
    /* ── 退出按钮（保留逻辑）── */
    {
        lv_obj_t *btn, *lbl;
        btn = lv_btn_create(lv_scr_act());
        lv_obj_set_size(btn, 66, 44);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 0, -15);
        lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "X");
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, btn_quit_cb, LV_EVENT_CLICKED, NULL);
    }
#endif

    /* ═══ 播放列表循环 ═══ */
    g_playlist_index = 0;

    volume_init_slider();
    pcm_warmup();

    while (!g_quit) {
        char *file = g_playlist[g_playlist_index].path;
        int is_video = has_video_ext(file);

        struct audio_player_ctx aptx;
        pthread_t aptid = 0;

        snprintf(buf, sizeof(buf), "%s",
                 strrchr(file, '/') ? strrchr(file, '/') + 1 : file);
        lv_label_set_text(g_file_label, buf);

        printf("[main] play #%d/%d: %s (%s)\n",
               g_playlist_index + 1, g_playlist_count, buf,
               is_video ? "video" : "audio");

        if (is_video) {
            lv_obj_add_flag(g_audio_label, LV_OBJ_FLAG_HIDDEN);
            lv_canvas_fill_bg(g_canvas, lv_color_black(), LV_OPA_COVER);

            memset(codec_name, 0, sizeof(codec_name));
            int ret = video_player_init(file, codec_name,
                                        sizeof(codec_name),
                                        &fps_num, &fps_den);
            if (ret < 0) {
                printf("[main] video init fail, skip\n");
                g_playlist_index = (g_playlist_index + 1)
                                   % g_playlist_count;
                continue;
            }
            audio_stream_init();
            pthread_create(&g_vdec_tid, NULL,
                           video_decode_thread, NULL);
        } else {
            lv_obj_clear_flag(g_audio_label, LV_OBJ_FLAG_HIDDEN);
            lv_canvas_fill_bg(g_canvas, lv_color_black(), LV_OPA_COVER);
            if (audio_player_init(file, &aptx) < 0) {
                printf("[main] audio init fail, skip\n");
                g_playlist_index = (g_playlist_index + 1)
                                   % g_playlist_count;
                continue;
            }
            pthread_create(&aptid, NULL, audio_player_thread, &aptx);
        }

        /* ── 播放循环 ── */
        g_switch_file = 0;
        while (!g_quit && !g_switch_file && g_skip_dir == 0) {
            lv_tick_inc(5);

            /* 播放列表浮层打开时：更新进度但跳过视频渲染 */
            if (g_playlist_open) {
                double cur = is_video ? g_astx.clock : aptx.clock;
                if (g_duration > 0.01) {
                    int v = (int)(cur / g_duration * 1000.0);
                    if (v < 0) v = 0; if (v > 1000) v = 1000;
                    lv_slider_set_value(g_progress_slider, v,
                                        LV_ANIM_OFF);
                    char tbuf[16];
                    format_time((int)cur, tbuf, sizeof(tbuf));
                    lv_label_set_text(g_time_cur_label, tbuf);
                    format_time((int)g_duration, tbuf, sizeof(tbuf));
                    lv_label_set_text(g_time_total_label, tbuf);
                }
                lv_task_handler();
                usleep(10000);
                continue;
            }

            if (g_pause) {
                lv_task_handler();
                usleep(20000);
                continue;
            }

            int rendered = 0;

            if (is_video) {
                pthread_mutex_lock(&g_frame_lock);
                if (g_new_frame) {
                    double aclock = g_astx.clock;
                    double diff = g_video_pts - aclock;

                    if (diff > 0.05) {
                    } else if (diff < -0.2) {
                        g_new_frame = 0;
                    } else {
                        int y;
                        lv_canvas_fill_bg(g_canvas,
                            lv_color_black(), LV_OPA_COVER);
                        for (y = 0; y < g_disp_h; y++)
                            memcpy((uint8_t *)g_canvas_buf
                                   + (g_vctx.off_y + y)
                                   * CANVAS_W * 4
                                   + g_vctx.off_x * 4,
                                   g_video_buf[g_video_buf_idx]
                                   + y * g_disp_w * 4,
                                   g_disp_w * 4);
                        lv_obj_invalidate(g_canvas);
                        g_new_frame = 0;
                        rendered = 1;
                    }
                }
                pthread_mutex_unlock(&g_frame_lock);
            }

            {
                double cur = is_video ? g_astx.clock : aptx.clock;
                if (g_duration > 0.01) {
                    int v = (int)(cur / g_duration * 1000.0);
                    if (v < 0) v = 0;
                    if (v > 1000) v = 1000;
                    lv_slider_set_value(g_progress_slider, v, LV_ANIM_OFF);

                    char tbuf[16];
                    format_time((int)cur, tbuf, sizeof(tbuf));
                    lv_label_set_text(g_time_cur_label, tbuf);
                    format_time((int)g_duration, tbuf, sizeof(tbuf));
                    lv_label_set_text(g_time_total_label, tbuf);
                }
            }

            lv_task_handler();
            if (rendered)
                usleep(16000);
            else
                usleep(5000);
        }

        lv_task_handler();
        usleep(10000);
        lv_task_handler();

        /* ── 清理当前 ── */
        if (is_video) {
            int user_quit = g_quit;
            video_player_deinit();
            g_quit = user_quit;
        } else {
            int user_quit = g_quit;
            snd_pcm_drop(aptx.pcm_handle);
            g_quit = 1;
            if (aptid) pthread_join(aptid, NULL);
            g_quit = user_quit;
            audio_player_deinit(&aptx);
        }

        g_pause = 0;

        {
            lv_indev_t *idv = NULL;
            while ((idv = lv_indev_get_next(idv)) != NULL)
                lv_indev_reset(idv, NULL);
        }
        for (int i = 0; i < 5; i++) {
            lv_task_handler();
            usleep(5000);
        }

        if (g_quit) break;

        if (g_skip_dir != 0) {
            g_playlist_index += g_skip_dir;
            if (g_playlist_index < 0)
                g_playlist_index += g_playlist_count;
            else if (g_playlist_index >= g_playlist_count)
                g_playlist_index = 0;
            g_skip_dir = 0;
        } else {
            g_playlist_index = (g_playlist_index + 1) % g_playlist_count;
        }
        g_switch_file = 0;
        save_config((int)lv_slider_get_value(g_vol_slider),
                    g_playlist_index);
    }

    if (g_ctl) snd_ctl_close(g_ctl);
    if (g_ctl_id) snd_ctl_elem_id_free(g_ctl_id);
    free_playlist();
    printf("[main] exit\n");
    return 0;
}
