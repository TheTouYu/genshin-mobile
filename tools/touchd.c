// touchd —— 读 Linux evdev 多点触控设备（Sunshine 的 libvirtualhid Touchscreen / uinput），
// 每个 SYN_REPORT 把「当前全部接触点」用 UDP 发给游戏进程内的探针（默认 127.0.0.1:51721）。
//
// 协议 v2：'T'(0x54) | 0x4D 魔数 | u8 count | count × (u16 id, u16 nx, u16 ny 小端)
//          nx/ny 是 0..65535 归一化坐标；整包长度 3 + 6*count。
//          （v1 只有 'T'|count，少了魔数，探针按 buf+2 读会错位 —— 已在探针侧改成显式拒收。）
// 心跳：没有新事件时每 250ms 重发一次当前接触点集合。探针据此区分「手指按住不动」（有心跳）
//       与「touchd 死了」（1.5s 静默 → 把残留手指抬起），避免角色一直按住不放。
// 用法：touchd [设备路径=auto] [端口=51721] [打印=0/1]
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>

#define MAXSLOT 10
#define PROTO_MAGIC 0x4D
#define HEARTBEAT_MS 250
#define ID_MASK 0x3FF        // 探针侧同样只取低 10 位

struct slot { int id; int x, y; int active; };

static int xmin, xmax = 1, ymin, ymax = 1;

static void get_abs(int fd, int code, int* mn, int* mx)
{
    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(code), &ai) == 0) { *mn = ai.minimum; *mx = ai.maximum; }
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 按当前接触点集合组一包；返回整包字节数（含 3 字节头）
static int build_pkt(unsigned char* pkt, struct slot* s, int* out_cnt)
{
    int k = 0, cnt = 0;
    pkt[0] = 'T';
    pkt[1] = PROTO_MAGIC;
    for (int i = 0; i < MAXSLOT; i++) {
        if (!s[i].active || s[i].id < 0) continue;
        long nx = (long)(s[i].x - xmin) * 65535 / (xmax > xmin ? (xmax - xmin) : 1);
        long ny = (long)(s[i].y - ymin) * 65535 / (ymax > ymin ? (ymax - ymin) : 1);
        if (nx < 0) nx = 0;
        if (nx > 65535) nx = 65535;
        if (ny < 0) ny = 0;
        if (ny > 65535) ny = 65535;
        unsigned short id = (unsigned short)(s[i].id & ID_MASK);
        unsigned short x = (unsigned short)nx, y = (unsigned short)ny;
        memcpy(pkt + 3 + k, &id, 2);
        memcpy(pkt + 3 + k + 2, &x, 2);
        memcpy(pkt + 3 + k + 4, &y, 2);
        k += 6; cnt++;
    }
    pkt[2] = (unsigned char)cnt;
    if (out_cnt) *out_cnt = cnt;
    return 3 + k;
}

static char g_found[64];

/* ---- 焦点门闩（Linux 侧）----
   为什么在这里做：游戏进程里的 GetForegroundWindow() 在 XWayland 下不可靠——合成器已经把焦点
   给了别的窗口，wine 还认为自己在前台（2026-11-19 实测：只剩 Firefox 前台，游戏仍收到点击）。
   合成器才知道真相，所以由 touchd 每 200ms 问一次 Hyprland，游戏不在前台就不投递。
   环境变量 FOCUS_CLASS 指定活动窗口的 class（默认 yuanshen.exe）；设成空串即关闭门闩。 */
static volatile int g_focus_ok = 1;
static volatile int g_gate_on = 0;
static volatile int g_released = 0;
static char g_focus_class[64] = "yuanshen.exe";

/* 返回 1=匹配 0=不匹配 -1=没有 class 字段 */
static int class_matches(const char* json, const char* want)
{
    const char* p = strstr(json, "\"class\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    const char* q = strchr(p + 1, '"');
    if (!q) return -1;
    size_t len = (size_t)(q - p - 1);
    if (strlen(want) != len) return 0;
    return strncmp(p + 1, want, len) == 0;
}

static void* focus_thread(void* arg)
{
    (void)arg;
    char buf[4096];
    for (;;) {
        FILE* fp = popen("hyprctl -j activewindow 2>/dev/null", "r");
        int ok = 1;   /* 取不到就放行（fail-open：宁可多投，也不要把输入锁死） */
        if (fp) {
            size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
            buf[n] = 0;
            pclose(fp);
            if (n > 0) { int m = class_matches(buf, g_focus_class); if (m >= 0) ok = m; }
        }
        g_focus_ok = ok;
        usleep(200000);
    }
    return NULL;
}

// 在 /sys/class/input 里按名字找设备节点（名字含 needle 即命中）
static const char* find_device(const char* needle)
{
    DIR* d = opendir("/sys/class/input");
    if (!d) return NULL;
    struct dirent* e;
    static char path[64];
    g_found[0] = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char np[128];
        snprintf(np, sizeof(np), "/sys/class/input/%s/device/name", e->d_name);
        FILE* f = fopen(np, "r");
        if (!f) continue;
        char name[128] = {0};
        if (fgets(name, sizeof(name) - 1, f)) {
            if (strstr(name, needle)) {
                snprintf(g_found, sizeof(g_found), "%s", name);
                snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
                fclose(f);
                closedir(d);
                return path;
            }
        }
        fclose(f);
    }
    closedir(d);
    return NULL;
}

int main(int argc, char** argv)
{
    const char* dev = argc > 1 ? argv[1] : "auto";

    /* 焦点门闩初始化（见 focus_thread 处的说明） */
    {
        const char* fc = getenv("FOCUS_CLASS");
        if (fc) snprintf(g_focus_class, sizeof(g_focus_class), "%s", fc);
        if (g_focus_class[0]) {
            if (!getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
                const char* rd = getenv("XDG_RUNTIME_DIR");
                char p[256];
                snprintf(p, sizeof(p), "%s/hypr", rd ? rd : "/run/user/1000");
                DIR* d = opendir(p);
                if (d) {
                    struct dirent* e;
                    while ((e = readdir(d)) != NULL) {
                        if (e->d_name[0] == '.') continue;
                        setenv("HYPRLAND_INSTANCE_SIGNATURE", e->d_name, 1);
                        break;
                    }
                    closedir(d);
                }
            }
            pthread_t th;
            if (pthread_create(&th, NULL, focus_thread, NULL) == 0) {
                pthread_detach(th);
                g_gate_on = 1;
                fprintf(stderr, "focus-gate: 仅当活动窗口 class == '%s' 时投递\n", g_focus_class);
            } else {
                fprintf(stderr, "focus-gate: 线程创建失败，门闩关闭\n");
            }
        } else {
            fprintf(stderr, "focus-gate: 已关闭（FOCUS_CLASS 为空）\n");
        }
    }
    int port = argc > 2 ? atoi(argv[2]) : 51721;
    int verbose = argc > 3 ? atoi(argv[3]) : 0;

    // auto：优先 Sunshine 的虚拟触摸屏，其次我们自己的 vts
    for (int wait = 0; ; wait++) {
        const char* found = NULL;
        if (strcmp(dev, "auto") == 0) {
            found = find_device("libvirtualhid Touchscreen");
            if (!found) found = find_device("vts-multitouch");
        } else found = dev;
        if (found) { dev = found; break; }
        if (wait % 30 == 0) fprintf(stderr, "touchd: 等待触摸设备（libvirtualhid Touchscreen / vts-multitouch）...\n");
        sleep(1);
    }
    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", dev, strerror(errno)); return 1; }
    char name[256] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
    get_abs(fd, ABS_MT_POSITION_X, &xmin, &xmax);
    get_abs(fd, ABS_MT_POSITION_Y, &ymin, &ymax);
    fprintf(stderr, "touchd: dev=%s name=%s x=[%d,%d] y=[%d,%d] proto=v2 -> udp 127.0.0.1:%d\n",
            dev, name, xmin, xmax, ymin, ymax, port);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct slot s[MAXSLOT];
    memset(s, 0, sizeof(s));
    for (int i = 0; i < MAXSLOT; i++) s[i].id = -1;
    int cur = 0;

    unsigned char pkt[3 + MAXSLOT * 6];
    struct input_event ev;
    long sent = 0, last_send = now_ms();
    for (;;) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        int do_send = 0, is_hb = 0;
        if (n == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_ABS) {
                switch (ev.code) {
                case ABS_MT_SLOT: cur = ev.value; if (cur < 0 || cur >= MAXSLOT) cur = 0; break;
                case ABS_MT_TRACKING_ID:
                    s[cur].id = ev.value;
                    s[cur].active = (ev.value >= 0);
                    if (ev.value < 0) { s[cur].x = s[cur].y = 0; }
                    break;
                case ABS_MT_POSITION_X: s[cur].x = ev.value; break;
                case ABS_MT_POSITION_Y: s[cur].y = ev.value; break;
                }
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                do_send = 1;
            }
        } else if (n < 0 && errno != EAGAIN) {
            fprintf(stderr, "read: %s\n", strerror(errno)); return 1;
        } else {
            usleep(1000);
        }

        long t = now_ms();
        if (!do_send && (t - last_send) >= HEARTBEAT_MS) { do_send = 1; is_hb = 1; }
        if (!do_send) continue;

        /* 焦点门闩：游戏不在前台时整包丢弃；失焦瞬间发一个空包，
           让探针立刻抬起全部接触点（否则按住不放的手指会一直卡着）。 */
        if (g_gate_on && !g_focus_ok) {
            if (!g_released) {
                unsigned char z[3] = {'T', (unsigned char)PROTO_MAGIC, 0};
                sendto(sock, z, 3, 0, (struct sockaddr*)&addr, sizeof(addr));
                g_released = 1;
                if (verbose) fprintf(stderr, "focus-gate: 失焦 -> 空包抬起全部接触点\n");
            }
            last_send = t;
            continue;
        }
        if (g_released) {
            g_released = 0;
            if (verbose) fprintf(stderr, "focus-gate: 回到前台，恢复投递\n");
        }

        int cnt = 0;
        int len = build_pkt(pkt, s, &cnt);
        sendto(sock, pkt, (size_t)len, 0, (struct sockaddr*)&addr, sizeof(addr));
        last_send = t;
        if (verbose && (sent < 300 || (sent % 20) == 0)) {
            char pts[400]; int off = 0;
            off += snprintf(pts + off, sizeof(pts) - off, "%s", "");
            for (int j = 0; j < MAXSLOT && off < (int)sizeof(pts) - 24; j++) {
                if (!s[j].active || s[j].id < 0) continue;
                off += snprintf(pts + off, sizeof(pts) - off, " [%d %d,%d]", s[j].id, s[j].x, s[j].y);
            }
            fprintf(stderr, "t=%ld n=%d%s%s\n", t % 1000000, cnt, is_hb ? " hb" : "", pts);
        }
        sent++;
    }
    return 0;
}
