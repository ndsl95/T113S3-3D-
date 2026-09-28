/* LVGL 9.3 fbdev 中文界面 for the salvaged T113-S3 3D printer board.
 *
 * The board rootfs has damaged shared libraries (the stock Qt UI and
 * several other binaries crash), so this demo is a fully static binary
 * that only talks to /dev/fb0 and /dev/input/event1 directly.
 *
 * Fonts lv_font_cn_22 / lv_font_cn_32 are generated from Source Han Sans SC
 * (OFL) with lv_font_conv and only contain the glyphs this UI needs; run
 * tools/make_cn_ui.ps1 after changing any text below.
 *
 * WiFi 页面通过 wpa_cli 操作 wpa_supplicant（板子上已有该命令），耗时动作
 * 放在独立线程里执行，主线程只轮询状态刷新界面，避免卡住 LVGL。
 */
#include "lvgl/lvgl.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

/* Implemented in evdev_touch.c */
lv_indev_t * touch_input_create(const char * device);

/* ---- 配色 ---- */
#define COL_BG        lv_color_hex(0x0E1116)
#define COL_CARD      lv_color_hex(0x1B2029)
#define COL_HEADER    lv_color_hex(0x1F6FEB)
#define COL_TEXT      lv_color_hex(0xE6EDF3)
#define COL_TEXT_DIM  lv_color_hex(0x9AA7B4)
#define COL_NOZZLE    lv_color_hex(0xF0883E)
#define COL_BED       lv_color_hex(0xE5534B)
#define COL_PROGRESS    lv_color_hex(0x2EA043)
#define COL_START       lv_color_hex(0x2EA043)
#define COL_PAUSE       lv_color_hex(0xD29922)
#define COL_STOP        lv_color_hex(0xDA3633)
#define COL_WIFI        lv_color_hex(0x8250DF)
#define COL_SCAN        lv_color_hex(0x1F6FEB)
#define COL_AP          lv_color_hex(0x262D38)

static volatile sig_atomic_t keep_running = 1;

/* ============================ 打印机页面 ============================ */

static lv_obj_t * lbl_status;
static lv_obj_t * lbl_nozzle;
static lv_obj_t * lbl_bed;
static lv_obj_t * lbl_progress;
static lv_obj_t * lbl_speed;
static lv_obj_t * lbl_time;
static lv_obj_t * lbl_fan_state;
static lv_obj_t * bar_nozzle;
static lv_obj_t * bar_bed;
static lv_obj_t * bar_progress;
static lv_obj_t * sw_fan;

/* ============================ WiFi 页面 ============================ */

#define WIFI_MAX_AP     24
#define WIFI_SSID_MAX   64
/* wpa_cli 会把 SSID 的每个字节写成 "\xNN"，最坏情况是 4 倍长度。
 * 凡是接这种转义串的缓冲区都得按这个尺寸来，否则中文 SSID 会被截断
 * （截断点还可能落在转义中间，显示成 "\xe" 这种半截东西）。 */
#define WIFI_SSID_ESC_MAX  (WIFI_SSID_MAX * 4 + 4)
#define WIFI_MSG_MAX    160

typedef struct {
    char ssid[WIFI_SSID_MAX];
    int  signal;
} ap_info_t;

static lv_obj_t * scr_main;
static lv_obj_t * scr_wifi;
static lv_obj_t * wifi_lbl_ssid;
static lv_obj_t * wifi_lbl_ip;
static lv_obj_t * wifi_lbl_ssh;
static lv_obj_t * wifi_lbl_hint;
static lv_obj_t * wifi_list;
static lv_obj_t * wifi_pwd_box;
static lv_obj_t * wifi_ta;
static lv_obj_t * wifi_kb;
static lv_obj_t * wifi_lbl_pwd_ssid;

static ap_info_t      g_ap[WIFI_MAX_AP];
static volatile int   g_ap_cnt;
static volatile int   g_wifi_busy;      /* 1 = 后台任务进行中 */
static volatile int   g_wifi_seq;       /* 每完成一次任务 +1 */
static volatile int   g_wifi_job_code;  /* 1=扫描 2=连接 3=刷新 */
static char           g_wifi_msg[WIFI_MSG_MAX];
static int            g_wifi_last_seq = -1;

static char g_cur_ssid[WIFI_SSID_MAX] = "未连接";
static char g_cur_ip[32] = "-";
static char g_pending_ssid[WIFI_SSID_MAX];
static char g_pending_pwd[WIFI_SSID_MAX];

/* ============================ 工具函数 ============================ */

static void stop_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

/* Required because lv_conf.h points LV_SYSMON_GET_IDLE at this function. */
unsigned int t113_linux_get_idle_percent(void)
{
    static unsigned long long prev_total;
    static unsigned long long prev_idle;
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    FILE * stat = fopen("/proc/stat", "r");
    if(!stat) return 100;
    int count = fscanf(stat, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    fclose(stat);
    if(count < 4) return 100;
    if(count < 5) iowait = 0;
    if(count < 6) irq = 0;
    if(count < 7) softirq = 0;
    if(count < 8) steal = 0;

    unsigned long long idle_all = idle + iowait;
    unsigned long long total = user + nice + system + idle_all + irq + softirq + steal;
    if(prev_total == 0 || total <= prev_total) {
        prev_total = total;
        prev_idle = idle_all;
        return 100;
    }
    unsigned long long total_delta = total - prev_total;
    unsigned long long idle_delta = idle_all - prev_idle;
    prev_total = total;
    prev_idle = idle_all;
    return total_delta ? (unsigned int)(idle_delta * 100 / total_delta) : 100;
}

static const char * getenv_default(const char * name, const char * dflt)
{
    const char * v = getenv(name);
    return v ? v : dflt;
}

/* 跑一条 shell 命令并把输出收进 out（不需要输出时传 NULL / 0） */
static void run_cmd(const char * cmd, char * out, size_t outsz)
{
    FILE * p = popen(cmd, "r");
    size_t used = 0;

    if(out && outsz) out[0] = '\0';
    if(p == NULL) return;

    if(out && outsz > 1) {
        used = fread(out, 1, outsz - 1, p);
        out[used] = '\0';
    }
    else {
        char buf[256];
        while(fread(buf, 1, sizeof(buf), p) > 0) { }
    }
    pclose(p);
}

/* 单引号包裹，供 /bin/sh 使用 */
static void shell_quote(const char * in, char * out, size_t outsz)
{
    size_t o = 0;
    for(size_t i = 0; in[i] && o + 5 < outsz; i++) {
        if(in[i] == '\'') {
            memcpy(out + o, "'\\''", 4);
            o += 4;
        }
        else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* 从 wpa_cli status 输出里取 key=value */
static int status_field(const char * out, const char * key, char * dst, size_t dstsz)
{
    size_t klen = strlen(key);
    const char * p = out;

    while((p = strstr(p, key)) != NULL) {
        if((p == out || p[-1] == '\n') && p[klen] == '=') {
            const char * v = p + klen + 1;
            const char * e = strchr(v, '\n');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if(n > dstsz - 1) n = dstsz - 1;
            memcpy(dst, v, n);
            dst[n] = '\0';
            return 1;
        }
        p += klen;
    }
    return 0;
}

static void trim_eol(char * s)
{
    size_t n = strlen(s);
    while(n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = '\0';
}

/* wpa_cli 会把 SSID 里的非 ASCII 字节按 \xNN 转义输出
 * （中文热点名会变成 "\xe4\xb8\xad\xe9\x93\x81..."），
 * 这里还原成真正的 UTF-8 字节序列，否则界面上显示的就是这串转义文本。
 * 所有从 wpa_cli 输出里取 SSID 的地方都要过一遍这个函数，
 * 否则连接时"目标 SSID 是否真的连上"的比较会因为一边带转义而永远不相等。 */
static int is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void unescape_ssid(const char * in, char * out, size_t outsz)
{
    size_t o = 0;

    for(size_t i = 0; in[i] && o + 1 < outsz; ) {
        if(in[i] == '\\' && in[i + 1] == 'x' && is_hex_digit(in[i + 2]) && is_hex_digit(in[i + 3])) {
            unsigned v = 0;
            for(int k = 2; k <= 3; k++) {
                char c = in[i + k];
                v <<= 4;
                if(c <= '9')      v |= (unsigned)(c - '0');
                else if(c <= 'F') v |= (unsigned)(c - 'A' + 10);
                else              v |= (unsigned)(c - 'a' + 10);
            }
            out[o++] = (char)v;
            i += 4;
        }
        else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

/* ============================ WiFi 后台任务 ============================ */

static pthread_t g_wifi_thr;

static void wifi_read_status(void)
{
    char out[1024];
    char state[32] = "";
    char ssid[WIFI_SSID_ESC_MAX] = "";
    char ssid_u[WIFI_SSID_MAX] = "";

    run_cmd("wpa_cli -i wlan0 status 2>/dev/null", out, sizeof(out));
    status_field(out, "wpa_state", state, sizeof(state));
    status_field(out, "ssid", ssid, sizeof(ssid));
    unescape_ssid(ssid, ssid_u, sizeof(ssid_u));

    run_cmd("ifconfig wlan0 2>/dev/null", out, sizeof(out));
    char * c = strstr(out, "inet addr:");
    if(c) {
        c += 10;
        size_t n = 0;
        while(c[n] && c[n] != ' ' && n < sizeof(g_cur_ip) - 1) {
            g_cur_ip[n] = c[n];
            n++;
        }
        g_cur_ip[n] = '\0';
    }
    else {
        snprintf(g_cur_ip, sizeof(g_cur_ip), "-");
    }

    if(strcmp(state, "COMPLETED") == 0 && ssid_u[0]) {
        snprintf(g_cur_ssid, sizeof(g_cur_ssid), "%s", ssid_u);
    }
    else {
        snprintf(g_cur_ssid, sizeof(g_cur_ssid), "未连接");
        snprintf(g_cur_ip, sizeof(g_cur_ip), "-");
    }
}

static int wifi_parse_scan(const char * out)
{
    char buf[8192];
    char * save1 = NULL;
    int cnt = 0;

    snprintf(buf, sizeof(buf), "%s", out);

    char * ln = strtok_r(buf, "\n", &save1);
    if(ln && strstr(ln, "bssid")) ln = strtok_r(NULL, "\n", &save1);

    while(ln && cnt < WIFI_MAX_AP) {
        char * f[5] = {0};
        int nf = 0;
        char * save2 = NULL;
        char * tk = strtok_r(ln, "\t", &save2);

        while(tk && nf < 5) {
            f[nf++] = tk;
            tk = strtok_r(NULL, "\t", &save2);
        }

        if(nf >= 5 && f[4][0]) {
            int sig;
            int dup = 0;
            char ssid_u[WIFI_SSID_MAX];

            trim_eol(f[4]);
            unescape_ssid(f[4], ssid_u, sizeof(ssid_u));
            sig = atoi(f[2]);
            if(ssid_u[0] == '\0') { ln = strtok_r(NULL, "\n", &save1); continue; }

            for(int i = 0; i < cnt; i++) {
                if(strcmp(g_ap[i].ssid, ssid_u) == 0) {
                    dup = 1;
                    if(sig > g_ap[i].signal) g_ap[i].signal = sig;
                    break;
                }
            }
            if(!dup) {
                snprintf(g_ap[cnt].ssid, sizeof(g_ap[cnt].ssid), "%s", ssid_u);
                g_ap[cnt].signal = sig;
                cnt++;
            }
        }
        ln = strtok_r(NULL, "\n", &save1);
    }

    /* 信号强的排前面 */
    for(int i = 1; i < cnt; i++) {
        ap_info_t key = g_ap[i];
        int j = i - 1;
        while(j >= 0 && g_ap[j].signal < key.signal) {
            g_ap[j + 1] = g_ap[j];
            j--;
        }
        g_ap[j + 1] = key;
    }
    return cnt;
}

static void wifi_job_status(void)
{
    wifi_read_status();
    g_wifi_msg[0] = '\0';
    g_wifi_busy = 0;
    g_wifi_seq++;
}

static void wifi_job_scan(void)
{
    char out[8192];
    int cnt = 0;

    run_cmd("wpa_cli -i wlan0 scan >/dev/null 2>&1", NULL, 0);

    for(int i = 0; i < 15; i++) {
        usleep(600 * 1000);
        out[0] = '\0';
        run_cmd("wpa_cli -i wlan0 scan_results 2>/dev/null", out, sizeof(out));
        cnt = wifi_parse_scan(out);
        if(cnt > 0) break;
    }

    g_ap_cnt = cnt;
    if(cnt > 0) snprintf(g_wifi_msg, sizeof(g_wifi_msg), "发现 %d 个网络", cnt);
    else        snprintf(g_wifi_msg, sizeof(g_wifi_msg), "没有发现网络");
    g_wifi_busy = 0;
    g_wifi_seq++;
}

/* 在已保存的配置里找出指定 SSID 的 network id 并选上 */
static int wifi_select_by_ssid(const char * ssid)
{
    char out[1024];
    char buf[1024];
    char cmd[128];
    char * save = NULL;
    int found = -1;

    run_cmd("wpa_cli -i wlan0 list_networks 2>/dev/null", out, sizeof(out));
    snprintf(buf, sizeof(buf), "%s", out);

    char * ln = strtok_r(buf, "\n", &save);
    if(ln && strstr(ln, "network id")) ln = strtok_r(NULL, "\n", &save);
    while(ln) {
        char * f[4] = {0};
        int nf = 0;
        char * save2 = NULL;
        char * tk = strtok_r(ln, "\t", &save2);
        while(tk && nf < 4) {
            f[nf++] = tk;
            tk = strtok_r(NULL, "\t", &save2);
        }
        if(nf >= 2) {
            char list_ssid[WIFI_SSID_MAX];
            unescape_ssid(f[1], list_ssid, sizeof(list_ssid));
            if(strcmp(list_ssid, ssid) == 0) {
                found = atoi(f[0]);
                break;
            }
        }
        ln = strtok_r(NULL, "\n", &save);
    }
    if(found < 0) return -1;

    snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 enable_network %d >/dev/null 2>&1", found);
    run_cmd(cmd, NULL, 0);
    snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 select_network %d >/dev/null 2>&1", found);
    run_cmd(cmd, NULL, 0);
    return found;
}

static void wifi_run_dhcp(void)
{
    run_cmd("killall udhcpc 2>/dev/null", NULL, 0);
    run_cmd("udhcpc -i wlan0 -n -q >/dev/null 2>&1", NULL, 0);
}

/* 网络通了就把 SSH 拉起来，省得还得接串口调试 */
static void wifi_ensure_ssh(void)
{
    run_cmd("mkdir -p /var/run/sshd", NULL, 0);
    run_cmd("[ -f /var/lock/sshd ] || /etc/init.d/S50sshd start >/dev/null 2>&1", NULL, 0);
}

static void wifi_job_connect(void)
{
    char cmd[1024];
    char out[768];
    char ssid_e[200];
    char psk_e[200];
    char prev_ssid[WIFI_SSID_MAX];
    char got_ssid[WIFI_SSID_ESC_MAX];
    int  id = -1;
    int  ok = 0;

    /* 先记下当前连着的网络：新网络只要连不上，还得回得去 */
    wifi_read_status();
    snprintf(prev_ssid, sizeof(prev_ssid), "%s", g_cur_ssid);

    shell_quote(g_pending_ssid, ssid_e, sizeof(ssid_e));
    shell_quote(g_pending_pwd, psk_e, sizeof(psk_e));

    out[0] = '\0';
    run_cmd("wpa_cli -i wlan0 add_network 2>/dev/null", out, sizeof(out));
    trim_eol(out);
    if(out[0] >= '0' && out[0] <= '9') id = atoi(out);
    if(id < 0) {
        snprintf(g_wifi_msg, sizeof(g_wifi_msg), "新建网络配置失败");
        goto done;
    }

    snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 set_network %d ssid '\"%s\"' 2>&1", id, ssid_e);
    run_cmd(cmd, out, sizeof(out));
    if(strncmp(out, "OK", 2) != 0) {
        snprintf(g_wifi_msg, sizeof(g_wifi_msg), "SSID 设置失败");
        goto cleanup;
    }

    if(g_pending_pwd[0]) {
        snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 set_network %d psk '\"%s\"' 2>&1", id, psk_e);
    }
    else {
        snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 set_network %d key_mgmt NONE 2>&1", id);
    }
    run_cmd(cmd, out, sizeof(out));
    if(strncmp(out, "OK", 2) != 0) {
        snprintf(g_wifi_msg, sizeof(g_wifi_msg), "密码设置失败");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 enable_network %d >/dev/null 2>&1", id);
    run_cmd(cmd, NULL, 0);
    snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 select_network %d >/dev/null 2>&1", id);
    run_cmd(cmd, NULL, 0);

    /* 必须等到"目标 SSID"真的连上，光看 COMPLETED 可能还挂在旧网络上 */
    for(int i = 0; i < 24; i++) {
        usleep(500 * 1000);
        out[0] = '\0';
        got_ssid[0] = '\0';
        run_cmd("wpa_cli -i wlan0 status 2>/dev/null", out, sizeof(out));
        status_field(out, "ssid", got_ssid, sizeof(got_ssid));
        /* 原地反转义是安全的：输出下标永远不超前于输入下标 */
        unescape_ssid(got_ssid, got_ssid, sizeof(got_ssid));
        if(strstr(out, "wpa_state=COMPLETED") && strcmp(got_ssid, g_pending_ssid) == 0) {
            ok = 1;
            break;
        }
    }

    if(ok) {
        run_cmd("wpa_cli -i wlan0 save_config >/dev/null 2>&1", NULL, 0);
        wifi_run_dhcp();
        wifi_ensure_ssh();
        snprintf(g_wifi_msg, sizeof(g_wifi_msg), "已连接到 %s", g_pending_ssid);
        goto done;
    }

    snprintf(g_wifi_msg, sizeof(g_wifi_msg), "连接失败，请检查密码");

cleanup:
    /* 失败就删掉这次试的配置，并恢复原来连着的网络，别把板子搞掉线 */
    snprintf(cmd, sizeof(cmd), "wpa_cli -i wlan0 remove_network %d >/dev/null 2>&1", id);
    run_cmd(cmd, NULL, 0);
    run_cmd("wpa_cli -i wlan0 save_config >/dev/null 2>&1", NULL, 0);

    if(prev_ssid[0] && strcmp(prev_ssid, "未连接") != 0) {
        if(wifi_select_by_ssid(prev_ssid) >= 0) {
            for(int i = 0; i < 20; i++) {
                usleep(500 * 1000);
                out[0] = '\0';
                run_cmd("wpa_cli -i wlan0 status 2>/dev/null", out, sizeof(out));
                if(strstr(out, "wpa_state=COMPLETED")) break;
            }
            wifi_run_dhcp();
        }
    }

done:
    wifi_read_status();
    g_wifi_busy = 0;
    g_wifi_seq++;
}

static void wifi_job_disconnect(void)
{
    run_cmd("killall udhcpc 2>/dev/null", NULL, 0);
    run_cmd("wpa_cli -i wlan0 disconnect >/dev/null 2>&1", NULL, 0);
    run_cmd("ifconfig wlan0 0.0.0.0 >/dev/null 2>&1", NULL, 0);
    wifi_read_status();
    snprintf(g_wifi_msg, sizeof(g_wifi_msg), "已断开");
    g_wifi_busy = 0;
    g_wifi_seq++;
}

static void * wifi_trampoline(void * arg)
{
    void (*fn)(void) = (void (*)(void))arg;
    fn();
    return NULL;
}

static void wifi_start_job(void (*fn)(void), int code)
{
    if(g_wifi_busy) return;
    g_wifi_busy = 1;
    g_wifi_job_code = code;
    g_wifi_msg[0] = '\0';

    if(pthread_create(&g_wifi_thr, NULL, wifi_trampoline, (void *)fn) != 0) {
        g_wifi_busy = 0;
        snprintf(g_wifi_msg, sizeof(g_wifi_msg), "任务启动失败");
        g_wifi_seq++;
    }
    else {
        pthread_detach(g_wifi_thr);
    }
}

/* ============================ 界面样式助手 ============================ */

static lv_obj_t * make_card(lv_obj_t * parent)
{
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t * make_row(lv_obj_t * parent)
{
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t * make_big_label(lv_obj_t * parent, const char * text, lv_color_t color)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_cn_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    return l;
}

static lv_obj_t * make_title_label(lv_obj_t * parent, const char * text)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, COL_TEXT_DIM, LV_PART_MAIN);
    return l;
}

static lv_obj_t * make_bar(lv_obj_t * parent, int32_t max, lv_color_t color)
{
    lv_obj_t * bar = lv_bar_create(parent);
    lv_obj_set_size(bar, lv_pct(100), 22);
    lv_bar_set_range(bar, 0, max);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A313C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 11, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 11, LV_PART_INDICATOR);
    return bar;
}

static void make_button(lv_obj_t * parent, const char * text, lv_color_t color,
                        int32_t height, lv_event_cb_t cb, void * user_data)
{
    lv_obj_t * btn = lv_button_create(parent);
    lv_obj_set_height(btn, height);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    if(cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t * l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_cn_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(l);
}

/* ============================ 打印机页面 ============================ */

static void btn_cb(lv_event_t * e)
{
    const char * action = (const char *)lv_event_get_user_data(e);
    if(strcmp(action, "开始") == 0)      lv_label_set_text(lbl_status, "打印中");
    else if(strcmp(action, "暂停") == 0) lv_label_set_text(lbl_status, "已暂停");
    else if(strcmp(action, "停止") == 0) lv_label_set_text(lbl_status, "待机");
}

static void fan_cb(lv_event_t * e)
{
    lv_obj_t * sw = (lv_obj_t *)lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    lv_label_set_text(lbl_fan_state, on ? "开" : "关");
}

static void open_wifi_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    lv_screen_load(scr_wifi);
    if(!g_wifi_busy) wifi_start_job(wifi_job_status, 3);
}

static void printer_timer_cb(lv_timer_t * t)
{
    static int32_t progress = 0;
    static int32_t step = 1;
    LV_UNUSED(t);

    progress += step;
    if(progress >= 100) { progress = 100; step = -1; }
    if(progress <= 0)   { progress = 0;   step = 1;  }

    int32_t nozzle = 190 + (progress % 25);
    int32_t bed    = 55 + (progress % 8);

    lv_bar_set_value(bar_nozzle, nozzle, LV_ANIM_OFF);
    lv_label_set_text_fmt(lbl_nozzle, "%d ℃", (int)nozzle);
    lv_bar_set_value(bar_bed, bed, LV_ANIM_OFF);
    lv_label_set_text_fmt(lbl_bed, "%d ℃", (int)bed);

    lv_bar_set_value(bar_progress, progress, LV_ANIM_OFF);
    lv_label_set_text_fmt(lbl_progress, "%d %%", (int)progress);

    lv_label_set_text_fmt(lbl_speed, "%d mm/s", (int)(60 + (progress % 40)));

    int32_t left = 3600 - progress * 30;
    lv_label_set_text_fmt(lbl_time, "%02d:%02d:%02d",
                          (int)(left / 3600), (int)((left / 60) % 60), (int)(left % 60));
}

static void printer_build(void)
{
    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_main, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr_main, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(scr_main, &lv_font_cn_22, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr_main, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr_main, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr_main, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 标题栏 ---- */
    lv_obj_t * header = lv_obj_create(scr_main);
    lv_obj_set_size(header, lv_pct(100), 88);
    lv_obj_set_style_bg_color(header, COL_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 26, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(header, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(header);
    lv_label_set_text(title, "3D 打印机");
    lv_obj_set_style_text_font(title, &lv_font_cn_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    lv_obj_t * right = lv_obj_create(header);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(right, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(right, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(right, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * wifi_btn = lv_button_create(right);
    lv_obj_set_size(wifi_btn, 168, 56);
    lv_obj_set_style_bg_color(wifi_btn, COL_WIFI, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(wifi_btn, 0, LV_PART_MAIN);
    /* 主题给的左右内边距会吃掉按钮宽度，32px 的 "WiFi" 会被裁成 "iFi" */
    lv_obj_set_style_pad_hor(wifi_btn, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_btn, open_wifi_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * wl = lv_label_create(wifi_btn);
    lv_label_set_text(wl, "WiFi");
    lv_obj_set_style_text_font(wl, &lv_font_cn_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(wl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(wl);

    lbl_status = lv_label_create(right);
    lv_label_set_text(lbl_status, "待机");
    lv_obj_set_style_text_color(lbl_status, COL_TEXT, LV_PART_MAIN);

    /* ---- 主体 ---- */
    lv_obj_t * body = lv_obj_create(scr_main);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * c1 = make_card(body);
    lv_obj_t * r1 = make_row(c1);
    make_title_label(r1, "喷头温度");
    lbl_nozzle = make_big_label(r1, "0 ℃", COL_NOZZLE);
    bar_nozzle = make_bar(c1, 300, COL_NOZZLE);

    lv_obj_t * c2 = make_card(body);
    lv_obj_t * r2 = make_row(c2);
    make_title_label(r2, "热床温度");
    lbl_bed = make_big_label(r2, "0 ℃", COL_BED);
    bar_bed = make_bar(c2, 120, COL_BED);

    lv_obj_t * c3 = make_card(body);
    lv_obj_t * r3 = make_row(c3);
    make_title_label(r3, "打印进度");
    lbl_progress = make_big_label(r3, "0 %", COL_PROGRESS);
    bar_progress = make_bar(c3, 100, COL_PROGRESS);

    lv_obj_t * c4 = make_card(body);
    lv_obj_t * r4 = make_row(c4);
    make_title_label(r4, "打印速度");
    lbl_speed = make_big_label(r4, "0 mm/s", COL_TEXT);
    lv_obj_t * r5 = make_row(c4);
    make_title_label(r5, "剩余时间");
    lbl_time = make_big_label(r5, "00:00:00", COL_TEXT);

    lv_obj_t * c5 = make_card(body);
    lv_obj_t * r6 = make_row(c5);
    lv_obj_t * fan_lbl = lv_label_create(r6);
    lv_label_set_text(fan_lbl, "风扇");
    lv_obj_set_style_text_color(fan_lbl, COL_TEXT, LV_PART_MAIN);
    sw_fan = lv_switch_create(r6);
    lv_obj_add_event_cb(sw_fan, fan_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lbl_fan_state = lv_label_create(r6);
    lv_label_set_text(lbl_fan_state, "关");
    lv_obj_set_style_text_color(lbl_fan_state, COL_TEXT, LV_PART_MAIN);

    make_title_label(c5, "亮度");
    lv_obj_t * slider = lv_slider_create(c5);
    lv_obj_set_width(slider, lv_pct(100));
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 70, LV_ANIM_OFF);

    /* ---- 底部按钮 ---- */
    lv_obj_t * btn_row = lv_obj_create(scr_main);
    lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn_row, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn_row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(btn_row, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    make_button(btn_row, "开始", COL_START, 88, btn_cb, (void *)"开始");
    make_button(btn_row, "暂停", COL_PAUSE, 88, btn_cb, (void *)"暂停");
    make_button(btn_row, "停止", COL_STOP,  88, btn_cb, (void *)"停止");

    lv_obj_t * foot = lv_label_create(scr_main);
    lv_label_set_text(foot, "当前文件    test.gcode");
    lv_obj_set_style_text_color(foot, COL_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(foot, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(foot, 16, LV_PART_MAIN);

    lv_timer_create(printer_timer_cb, 400, NULL);
}

/* ============================ WiFi 页面 ============================ */

static void back_main_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    lv_obj_add_flag(wifi_pwd_box, LV_OBJ_FLAG_HIDDEN);
    lv_screen_load(scr_main);
}

static void scan_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    wifi_start_job(wifi_job_scan, 1);
}

static void wifi_status_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    wifi_start_job(wifi_job_status, 3);
}

static void wifi_disconnect_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    wifi_start_job(wifi_job_disconnect, 3);
}

static void ap_clicked_cb(lv_event_t * e)
{
    int idx = (int)(size_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= g_ap_cnt) return;
    snprintf(g_pending_ssid, sizeof(g_pending_ssid), "%s", g_ap[idx].ssid);
    lv_textarea_set_text(wifi_ta, "");
    lv_label_set_text_fmt(wifi_lbl_pwd_ssid, "网络：%s", g_pending_ssid);
    lv_obj_remove_flag(wifi_pwd_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_pwd_box);
}

static void pwd_cancel_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    lv_obj_add_flag(wifi_pwd_box, LV_OBJ_FLAG_HIDDEN);
}

static void pwd_connect_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    const char * pwd = lv_textarea_get_text(wifi_ta);
    snprintf(g_pending_pwd, sizeof(g_pending_pwd), "%s", pwd ? pwd : "");
    lv_obj_add_flag(wifi_pwd_box, LV_OBJ_FLAG_HIDDEN);
    wifi_start_job(wifi_job_connect, 2);
}

static void wifi_refresh_list(void)
{
    lv_obj_clean(wifi_list);

    if(g_ap_cnt == 0) {
        lv_obj_t * l = lv_label_create(wifi_list);
        lv_label_set_text(l, "点扫描搜索网络");
        lv_obj_set_style_text_color(l, COL_TEXT_DIM, LV_PART_MAIN);
        return;
    }

    for(int i = 0; i < g_ap_cnt; i++) {
        lv_obj_t * b = lv_button_create(wifi_list);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_set_height(b, 82);
        lv_obj_set_flex_grow(b, 0);
        lv_obj_set_style_bg_color(b, COL_AP, LV_PART_MAIN);
        lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(b, ap_clicked_cb, LV_EVENT_CLICKED, (void *)(size_t)i);

        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text_fmt(l, "%s    %d dBm", g_ap[i].ssid, g_ap[i].signal);
        /* 热点名什么字都可能出现，用覆盖全 CJK 的字库 */
        lv_obj_set_style_text_font(l, &lv_font_ssid, LV_PART_MAIN);
        lv_obj_center(l);
    }
}

static void wifi_timer_cb(lv_timer_t * t)
{
    static int tick = 0;
    LV_UNUSED(t);

    if(lv_screen_active() != scr_wifi) {
        tick = 0;
        return;
    }

    if(g_wifi_seq != g_wifi_last_seq) {
        g_wifi_last_seq = g_wifi_seq;
        lv_label_set_text(wifi_lbl_ssid, g_cur_ssid);
        lv_label_set_text(wifi_lbl_ip, g_cur_ip);
        if(strcmp(g_cur_ip, "-") == 0) lv_label_set_text(wifi_lbl_ssh, "-");
        else lv_label_set_text_fmt(wifi_lbl_ssh, "root@%s", g_cur_ip);
        wifi_refresh_list();
    }

    if(g_wifi_busy) {
        const char * m = "正在刷新...";
        if(g_wifi_job_code == 1)      m = "正在扫描...";
        else if(g_wifi_job_code == 2) m = "正在连接...";
        lv_label_set_text(wifi_lbl_hint, m);
    }
    else {
        lv_label_set_text(wifi_lbl_hint, g_wifi_msg);
    }

    if(++tick >= 12) {
        tick = 0;
        wifi_start_job(wifi_job_status, 3);
    }
}

/* 键盘按钮高度：交给 flex 拉伸会把按键拉得过高，这里给固定高度 */
#define WIFI_KB_HEIGHT 480

/* 键面文字用的是 buttonmatrix 的 LV_PART_ITEMS 字体：不显式设置的话，这一路
 * 查不到继承值，会退回 LV_FONT_DEFAULT（montserrat_14）。14px 在 720x1280
 * 的小屏上就是"糊"的观感，这里统一换成 32px。montserrat 同时带 LVGL 的
 * 功能键符号（退格/回车/关闭），中文字库没有这些。
 */
#define KB_FONT (&lv_font_montserrat_32)

/* 自定义符号页。
 *
 * LVGL 内置的 SPECIAL 页缺少 WiFi 密码里很常见的 '!'、'^'、'{' 等符号，
 * 这里重排一页：第 2 行第 2 个键就是 '!'，一眼能看到。
 * 保留 LVGL 约定的 "abc" 键（点击会切回小写字母页）。
 * 行内按键数：11 + 12 + 12 + 5 = 40。
 */
static const char * const kb_map_sym[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "-", "\n",
    "_", "=", "+", "[", "]", "{", "}", ";", ":", "'", "\"", "/", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

/* 每个按键的宽度/标志，条目数必须和 kb_map_sym 的按键数严格一致（40 个） */
static const lv_buttonmatrix_ctrl_t kb_ctrl_sym[] = {
    /* 第 1 行 */
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_CHECKED | 2,
    /* 第 2 行 */
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    /* 第 3 行 */
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1, LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    /* 第 4 行 */
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_BUTTONMATRIX_CTRL_POPOVER | 2,
    LV_BUTTONMATRIX_CTRL_POPOVER | 1,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2
};

static void wifi_pwd_box_build(void)
{
    wifi_pwd_box = lv_obj_create(scr_wifi);
    lv_obj_set_size(wifi_pwd_box, lv_pct(100), lv_pct(100));
    /* 覆盖层要脱离屏幕的 flex 布局，否则会被当成普通子项参与排布 */
    lv_obj_add_flag(wifi_pwd_box, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(wifi_pwd_box, 0, 0);
    lv_obj_set_style_bg_color(wifi_pwd_box, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_pwd_box, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_pwd_box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_pwd_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wifi_pwd_box, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(wifi_pwd_box, 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(wifi_pwd_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifi_pwd_box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(wifi_pwd_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * t = lv_label_create(wifi_pwd_box);
    lv_label_set_text(t, "输入密码");
    lv_obj_set_style_text_font(t, &lv_font_cn_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, COL_TEXT, LV_PART_MAIN);

    wifi_lbl_pwd_ssid = lv_label_create(wifi_pwd_box);
    lv_label_set_text(wifi_lbl_pwd_ssid, "");
    lv_obj_set_style_text_font(wifi_lbl_pwd_ssid, &lv_font_ssid, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_lbl_pwd_ssid, COL_TEXT_DIM, LV_PART_MAIN);

    wifi_ta = lv_textarea_create(wifi_pwd_box);
    lv_obj_set_width(wifi_ta, lv_pct(100));
    lv_obj_set_height(wifi_ta, 84);
    lv_textarea_set_one_line(wifi_ta, true);
    lv_textarea_set_password_mode(wifi_ta, true);
    lv_textarea_set_placeholder_text(wifi_ta, "密码");
    lv_obj_set_style_text_font(wifi_ta, &lv_font_cn_32, LV_PART_MAIN);

    lv_obj_t * row = lv_obj_create(wifi_pwd_box);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    make_button(row, "连接", COL_START, 84, pwd_connect_cb, NULL);
    make_button(row, "取消", COL_STOP,  84, pwd_cancel_cb, NULL);

    lv_obj_t * kb_hint = lv_label_create(wifi_pwd_box);
    lv_label_set_text(kb_hint, "点 1# 键输入符号");
    lv_obj_set_style_text_color(kb_hint, COL_TEXT_DIM, LV_PART_MAIN);

    wifi_kb = lv_keyboard_create(wifi_pwd_box);
    lv_obj_set_size(wifi_kb, lv_pct(100), WIFI_KB_HEIGHT);
    lv_obj_set_style_text_font(wifi_kb, KB_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_font(wifi_kb, KB_FONT, LV_PART_ITEMS);
    lv_keyboard_set_map(wifi_kb, LV_KEYBOARD_MODE_SPECIAL, kb_map_sym, kb_ctrl_sym);
    lv_keyboard_set_textarea(wifi_kb, wifi_ta);
}

static void wifi_build(void)
{
    scr_wifi = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wifi, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr_wifi, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr_wifi, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(scr_wifi, &lv_font_cn_22, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr_wifi, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scr_wifi, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(scr_wifi, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(scr_wifi, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 标题栏 ---- */
    lv_obj_t * header = lv_obj_create(scr_wifi);
    lv_obj_set_size(header, lv_pct(100), 88);
    lv_obj_set_style_bg_color(header, COL_WIFI, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 26, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(header, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(header);
    lv_label_set_text(title, "WiFi 连接");
    lv_obj_set_style_text_font(title, &lv_font_cn_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    lv_obj_t * back = lv_button_create(header);
    lv_obj_set_size(back, 130, 56);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x4B3A8F), LV_PART_MAIN);
    lv_obj_set_style_radius(back, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, back_main_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * bl = lv_label_create(back);
    lv_label_set_text(bl, "返回");
    lv_obj_set_style_text_font(bl, &lv_font_cn_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(bl);

    /* ---- 状态卡片 ---- */
    lv_obj_t * body = lv_obj_create(scr_wifi);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * card = make_card(body);
    lv_obj_t * r1 = make_row(card);
    make_title_label(r1, "当前网络");
    wifi_lbl_ssid = make_big_label(r1, "未连接", COL_TEXT);
    lv_obj_set_style_text_font(wifi_lbl_ssid, &lv_font_ssid, LV_PART_MAIN);
    lv_obj_t * r2 = make_row(card);
    make_title_label(r2, "IP 地址");
    wifi_lbl_ip = make_big_label(r2, "-", COL_TEXT);
    lv_obj_t * r3 = make_row(card);
    make_title_label(r3, "SSH 地址");
    wifi_lbl_ssh = make_big_label(r3, "-", COL_TEXT);

    /* ---- 操作按钮 ---- */
    lv_obj_t * btn_row = lv_obj_create(body);
    lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(btn_row, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    make_button(btn_row, "扫描", COL_SCAN, 82, scan_cb, NULL);
    make_button(btn_row, "刷新", COL_PROGRESS, 82, wifi_status_cb, NULL);
    make_button(btn_row, "断开", COL_STOP, 82, wifi_disconnect_cb, NULL);

    /* ---- 提示 ---- */
    wifi_lbl_hint = lv_label_create(body);
    lv_label_set_text(wifi_lbl_hint, "");
    lv_obj_set_style_text_color(wifi_lbl_hint, COL_TEXT_DIM, LV_PART_MAIN);

    /* ---- 网络列表 ---- */
    wifi_list = lv_obj_create(body);
    lv_obj_set_width(wifi_list, lv_pct(100));
    lv_obj_set_flex_grow(wifi_list, 1);
    lv_obj_set_style_bg_color(wifi_list, COL_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_list, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wifi_list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(wifi_list, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(wifi_list, LV_SCROLLBAR_MODE_AUTO);

    wifi_pwd_box_build();
    wifi_refresh_list();

    lv_timer_create(wifi_timer_cb, 500, NULL);
}

/* ============================ main ============================ */

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("lvgl-printer: start\n");

    lv_init();

    const char * fbdev = getenv_default("LV_LINUX_FBDEV_DEVICE", "/dev/fb0");
    lv_display_t * disp = lv_linux_fbdev_create();
    if(disp == NULL) {
        printf("lvgl-printer: lv_linux_fbdev_create() failed\n");
        return 1;
    }
    lv_linux_fbdev_set_file(disp, fbdev);
    printf("lvgl-printer: fbdev=%s native=%dx%d\n", fbdev,
           (int)lv_display_get_horizontal_resolution(disp),
           (int)lv_display_get_vertical_resolution(disp));

    int rot = atoi(getenv_default("LV_ROTATION", "0"));
    lv_display_rotation_t r = LV_DISPLAY_ROTATION_0;
    if(rot == 90) r = LV_DISPLAY_ROTATION_90;
    else if(rot == 180) r = LV_DISPLAY_ROTATION_180;
    else if(rot == 270) r = LV_DISPLAY_ROTATION_270;
    lv_display_set_rotation(disp, r);
    printf("lvgl-printer: rotation=%d logical=%dx%d\n", rot,
           (int)lv_display_get_horizontal_resolution(disp),
           (int)lv_display_get_vertical_resolution(disp));

    const char * tdev = getenv_default("LV_LINUX_EVDEV_POINTER_DEVICE", "/dev/input/event1");
    if(tdev[0] != '\0') {
        lv_indev_t * indev = touch_input_create(tdev);
        if(indev) {
            lv_indev_set_display(indev, disp);
            printf("lvgl-printer: touchscreen %s registered\n", tdev);
        }
        else {
            printf("lvgl-printer: touchscreen %s not available, running display only\n", tdev);
        }
    }

    printer_build();
    wifi_build();
    lv_screen_load(scr_main);
    wifi_read_status();
    printf("lvgl-printer: ui created\n");

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    while(keep_running) {
        uint32_t sleep_ms = lv_timer_handler();
        if(sleep_ms < 5) sleep_ms = 5;
        else if(sleep_ms > 16) sleep_ms = 16;
        usleep(sleep_ms * 1000u);
    }

    printf("lvgl-printer: exit\n");
    return 0;
}
