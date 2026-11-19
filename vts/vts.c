// vts —— 虚拟多点触控屏（uinput, MT protocol B）。从 stdin 读命令驱动。
// 命令: d <slot> <id> <x> <y> | m <slot> <x> <y> | u <slot> | s | q
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

static int fd;
static int width = 2560, height = 1440;

static void emit(int type, int code, int val)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = val;
    if (write(fd, &ev, sizeof(ev)) < 0) perror("write");
}
static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void abs_setup(int code, int min, int max)
{
    struct uinput_abs_setup a;
    memset(&a, 0, sizeof(a));
    a.code = code;
    a.absinfo.minimum = min;
    a.absinfo.maximum = max;
    if (ioctl(fd, UI_ABS_SETUP, &a) < 0) fprintf(stderr, "abs setup %d failed\n", code);
}

int main(int argc, char** argv)
{
    if (argc > 2) { width = atoi(argv[1]); height = atoi(argv[2]); }
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    abs_setup(ABS_MT_SLOT, 0, 9);
    abs_setup(ABS_MT_TRACKING_ID, 0, 65535);
    abs_setup(ABS_MT_POSITION_X, 0, width - 1);
    abs_setup(ABS_MT_POSITION_Y, 0, height - 1);
    abs_setup(ABS_X, 0, width - 1);
    abs_setup(ABS_Y, 0, height - 1);

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    snprintf(us.name, sizeof(us.name), "vts-multitouch");
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x1a2b;
    us.id.product = 0x3c4d;
    us.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); return 2; }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return 3; }
    fprintf(stderr, "vts created %dx%d, pid %d\n", width, height, (int)getpid());
    fflush(stderr);

    char line[256];
    int active = 0;
    while (fgets(line, sizeof(line), stdin)) {
        char c = line[0];
        int slot, id, x, y;
        if (c == 'd' && sscanf(line + 1, "%d %d %d %d", &slot, &id, &x, &y) == 4) {
            emit(EV_ABS, ABS_MT_SLOT, slot);
            emit(EV_ABS, ABS_MT_TRACKING_ID, id);
            emit(EV_ABS, ABS_MT_POSITION_X, x);
            emit(EV_ABS, ABS_MT_POSITION_Y, y);
            if (!active) { emit(EV_KEY, BTN_TOUCH, 1); active = 1; }
            syn();
            fprintf(stderr, "down slot=%d id=%d %d,%d\n", slot, id, x, y); fflush(stderr);
        } else if (c == 'm' && sscanf(line + 1, "%d %d %d", &slot, &x, &y) == 3) {
            emit(EV_ABS, ABS_MT_SLOT, slot);
            emit(EV_ABS, ABS_MT_POSITION_X, x);
            emit(EV_ABS, ABS_MT_POSITION_Y, y);
            syn();
        } else if (c == 'u' && sscanf(line + 1, "%d", &slot) == 1) {
            emit(EV_ABS, ABS_MT_SLOT, slot);
            emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
            syn();
            fprintf(stderr, "up slot=%d\n", slot); fflush(stderr);
        } else if (c == 'z') {
            emit(EV_KEY, BTN_TOUCH, 0); active = 0; syn();
        } else if (c == 'q') {
            break;
        }
    }
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
