// Check the actual Ilitek evdev -> bridge -> desktop path without a finger.
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
static int emit(int fd, unsigned type, unsigned code, int value) {
    struct input_event event = {.type=type, .code=code, .value=value};
    return write(fd, &event, sizeof(event)) == sizeof(event) ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    unsigned x = strtoul(argv[1], NULL, 10), y = strtoul(argv[2], NULL, 10);
    if (x > 10000 || y > 10000) return 2;
    int fd = -1;
    for (int i=0; i<32; i++) {
        char path[64], name[128] = {0};
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int candidate = open(path, O_RDWR | O_CLOEXEC);
        if (candidate < 0) continue;
        if (ioctl(candidate, EVIOCGNAME(sizeof(name)), name) >= 0 && !strcmp(name,"ILITEK_TDDI")) {
            fd = candidate; break;
        }
        close(candidate);
    }
    if (fd < 0) return 1;
    struct input_absinfo ax, ay;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) < 0 ||
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) < 0) return 1;
    x = ax.minimum + (unsigned long long)x * (ax.maximum-ax.minimum)/10000;
    y = ay.minimum + (unsigned long long)y * (ay.maximum-ay.minimum)/10000;
    if (emit(fd,EV_ABS,ABS_MT_SLOT,0) || emit(fd,EV_ABS,ABS_MT_TRACKING_ID,1234) ||
        emit(fd,EV_ABS,ABS_MT_POSITION_X,x) || emit(fd,EV_ABS,ABS_MT_POSITION_Y,y) ||
        emit(fd,EV_KEY,BTN_TOUCH,1) || emit(fd,EV_SYN,SYN_REPORT,0)) return 1;
    usleep(100000);
    if (emit(fd,EV_ABS,ABS_MT_TRACKING_ID,-1) || emit(fd,EV_KEY,BTN_TOUCH,0) ||
        emit(fd,EV_SYN,SYN_REPORT,0)) return 1;
    close(fd);
    puts("Synthetic Ilitek contact sent");
    return 0;
}
