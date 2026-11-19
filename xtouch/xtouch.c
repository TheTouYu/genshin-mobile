// xtouch —— 最小 XI2 触摸接收器：开窗口→选 XI2 触摸事件→把收到的事件打到 stdout
// 用法: xtouch [x y w h seconds]
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec/1e9; }

int main(int argc, char** argv)
{
    int overlay = (argc > 1 && strcmp(argv[1], "--overlay") == 0) ? 1 : 0;
    int x = overlay ? 0 : (argc > 1 ? atoi(argv[1]) : 100);
    int y = overlay ? 0 : (argc > 2 ? atoi(argv[2]) : 100);
    int w = overlay ? 2560 : (argc > 3 ? atoi(argv[3]) : 800);
    int h = overlay ? 1440 : (argc > 4 ? atoi(argv[4]) : 500);
    int secs = overlay ? (argc > 2 ? atoi(argv[2]) : 6) : (argc > 5 ? atoi(argv[5]) : 8);

    Display* d = XOpenDisplay(NULL);
    if (!d) { printf("cannot open display\n"); return 1; }
    int xi_opcode, ev, err;
    if (!XQueryExtension(d, "XInputExtension", &xi_opcode, &ev, &err)) { printf("no XInput\n"); return 2; }
    int major = 2, minor = 2;
    if (XIQueryVersion(d, &major, &minor) != Success) { printf("XIQueryVersion failed\n"); return 3; }
    printf("XI %d.%d\n", major, minor);

    Window root = DefaultRootWindow(d);
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.background_pixel = 0x202020;
    swa.override_redirect = overlay ? True : False;
    Window win = XCreateWindow(d, root, x, y, w, h, 0, CopyFromParent, InputOutput, CopyFromParent,
                               CWBackPixel, &swa);
    XStoreName(d, win, "xtouch-test");
    XMapRaised(d, win);
    XFlush(d);

    // 选 XI2 触摸事件（对所有设备）
    unsigned char mask[(XI_LASTEVENT + 7) / 8];
    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_TouchBegin);
    XISetMask(mask, XI_TouchUpdate);
    XISetMask(mask, XI_TouchEnd);
    XISetMask(mask, XI_TouchOwnership);
    XIEventMask em;
    em.deviceid = XIAllMasterDevices;
    em.mask_len = sizeof(mask);
    em.mask = mask;
    XISelectEvents(d, win, &em, 1);
    // 也选 root 上的，便于看全局
    XISelectEvents(d, root, &em, 1);
    XFlush(d);

    // 列出设备
    int ndev = 0;
    XIDeviceInfo* info = XIQueryDevice(d, XIAllDevices, &ndev);
    for (int i = 0; i < ndev; i++) {
        printf("dev id=%d use=%d name=%s ntouch=%d\n", info[i].deviceid, info[i].use, info[i].name,
               info[i].num_classes);
        for (int c = 0; c < info[i].num_classes; c++) {
            if (info[i].classes[c]->type == XITouchClass) {
                XITouchClassInfo* t = (XITouchClassInfo*)info[i].classes[c];
                printf("   touch class: mode=%d num_touches=%d\n", t->mode, t->num_touches);
            }
        }
    }
    XIFreeDeviceInfo(info);
    fflush(stdout);

    double t0 = now();
    int count = 0;
    while (now() - t0 < secs) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type == GenericEvent && e.xcookie.extension == xi_opcode) {
                if (XGetEventData(d, &e.xcookie)) {
                    XIDeviceEvent* de = (XIDeviceEvent*)e.xcookie.data;
                    printf("[%.3f] event_type=%d devid=%d touchid=%d pos=%.0f,%.0f flags=%x\n",
                           now() - t0, e.xcookie.evtype, de->deviceid, (int)de->detail,
                           de->event_x, de->event_y, de->flags);
                    fflush(stdout);
                    count++;
                    XFreeEventData(d, &e.xcookie);
                }
            }
        }
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    printf("total touch events=%d\n", count);
    XDestroyWindow(d, win);
    XCloseDisplay(d);
    return 0;
}
