// keywatch: print key events from every input device, to find a remote button's code.
// Passive: devices are only read, never grabbed, so the TV keeps receiving every key.
//   keywatch [seconds]       (default 60) -> "<t> <device> type=<t> code=<c> value=<v>"
// Build: zig cc -target arm-linux-gnueabi.2.35 -mcpu=cortex_a9 -O2 -o keywatch keywatch.c
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    struct pollfd pfd[32];
    char name[32][64];
    int n = 0;
    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof(name[n])), name[n]) < 0) snprintf(name[n], sizeof(name[n]), "?");
        printf("watching %s \"%s\"\n", path, name[n]);
        pfd[n].fd = fd;
        pfd[n].events = POLLIN;
        n++;
    }
    fflush(stdout);
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        if (poll(pfd, n, 500) <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            struct input_event ev;
            while (read(pfd[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type != EV_KEY && ev.type != EV_MSC) continue; // skip pointer motion/sync
                printf("%ld.%03ld \"%s\" type=%d code=%d (0x%x) value=%d\n", (long)ev.time.tv_sec,
                    (long)ev.time.tv_usec / 1000, name[i], ev.type, ev.code, ev.code, ev.value);
                fflush(stdout);
            }
        }
    }
    return 0;
}
