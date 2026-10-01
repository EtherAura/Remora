/* uinput-kbd — create a phantom HW keyboard via /dev/uinput so Android registers a real
 * physical keyboard, hides the on-screen IME and offers Gboard's physical-keyboard toolbar
 * (the VM's persist-kbd trick, restored for the container — bd remora-gf7).
 *
 * Static-compiled on the host, bind-mounted into the container, run there detached. It is a
 * DECOY: it never delivers a keystroke. Real keys still arrive through the mirror's SDK key
 * injection; this device exists only so Android's Configuration reports a hardware qwerty
 * keyboard instead of `nokeys`.
 *
 * Why it also has to mknod its own device node: the container's /dev is docker's own tmpfs,
 * populated once at container creation, and the container has its own network namespace so the
 * kernel's uevents for a device created later never reach it. The node therefore has to be made
 * by hand or Android sees nothing. Android's EventHub *does* pick it up — it watches /dev/input
 * with inotify — but it opens the node the instant it appears, so the mode has to be right at
 * creation: a chmod afterwards loses the race and EventHub logs
 * "could not open /dev/input/eventNN, Permission denied" and never retries.
 *
 * The identity comes from REMORA_KBD_* (see kbdIdentityEnv in src/core/Builders.cpp). Gboard
 * gates its toolbar on a keyboard that looks real, so impersonating the operator's own keyboard
 * is the point; with nothing set it still creates a generic one, which is enough to hide the IME.
 */
#include <linux/uinput.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>

static char node_path[64];  /* the node we made, so the signal handler can take it away again */

static void cleanup(int sig) {
    if (node_path[0]) unlink(node_path);
    _exit(sig ? 128 + sig : 0);
}

static unsigned env_hex(const char *key, unsigned fallback) {
    const char *v = getenv(key);
    if (!v || !*v) return fallback;
    return (unsigned)strtoul(v, NULL, 0);  /* values are written as 0x1234 */
}

/* Find the eventNN belonging to the input device we just created, and make its node. The sysfs
 * lookup is the reliable direction: UI_GET_SYSNAME names the input device ("input32"), and the
 * event child underneath it names the evdev node. */
static int make_node(const char *sysname) {
    char dir[128];
    snprintf(dir, sizeof dir, "/sys/class/input/%s", sysname);
    DIR *d = opendir(dir);
    if (!d) { dprintf(2, "[uinput-kbd] opendir %s: %s\n", dir, strerror(errno)); return -1; }

    char event[32] = {0};
    for (struct dirent *e; (e = readdir(d));)
        if (strncmp(e->d_name, "event", 5) == 0) {
            snprintf(event, sizeof event, "%s", e->d_name);
            break;
        }
    closedir(d);
    if (!event[0]) { dprintf(2, "[uinput-kbd] no event* under %s\n", dir); return -1; }

    char devfile[160];
    snprintf(devfile, sizeof devfile, "/sys/class/input/%s/dev", event);
    FILE *f = fopen(devfile, "r");
    if (!f) { dprintf(2, "[uinput-kbd] open %s: %s\n", devfile, strerror(errno)); return -1; }
    unsigned maj = 0, min = 0;
    const int got = fscanf(f, "%u:%u", &maj, &min);
    fclose(f);
    if (got != 2) { dprintf(2, "[uinput-kbd] unparsable %s\n", devfile); return -1; }

    snprintf(node_path, sizeof node_path, "/dev/input/%s", event);
    /* A node with this name in the container is a leftover from the snapshot docker took at
     * container start: the kernel cannot have handed us a number a live device still holds. */
    unlink(node_path);
    umask(0);  /* the mode has to be exact at creation — see the race described at the top */
    if (mknod(node_path, S_IFCHR | 0666, makedev(maj, min)) < 0) {
        dprintf(2, "[uinput-kbd] mknod %s: %s\n", node_path, strerror(errno));
        node_path[0] = '\0';
        return -1;
    }
    printf("[uinput-kbd] %s -> %s (%u:%u)\n", sysname, node_path, maj, min);
    fflush(stdout);
    return 0;
}

int main(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { dprintf(2, "[uinput-kbd] open /dev/uinput: %s\n", strerror(errno)); return 1; }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_REP);  /* a real keyboard repeats; without it Android calls it partial */
    /* a full alpha key range so Android classifies it ALPHAKEY -> KeyboardType 2, which is what
     * flips Configuration from `nokeys` to `qwerty` and takes the on-screen keyboard away */
    for (int k = KEY_ESC; k <= KEY_KPDOT; k++) ioctl(fd, UI_SET_KEYBIT, k);
    for (int k = KEY_F1; k <= KEY_F12; k++) ioctl(fd, UI_SET_KEYBIT, k);

    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = (unsigned short)env_hex("REMORA_KBD_BUS", BUS_USB);
    us.id.vendor = (unsigned short)env_hex("REMORA_KBD_VENDOR", 0x18d1);   /* Google */
    us.id.product = (unsigned short)env_hex("REMORA_KBD_PRODUCT", 0x4e05);
    us.id.version = (unsigned short)env_hex("REMORA_KBD_VERSION", 1);
    const char *name = getenv("REMORA_KBD_NAME");
    snprintf(us.name, sizeof us.name, "%s",
             (name && *name) ? name : "Remora virtual keyboard");

    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) {
        dprintf(2, "[uinput-kbd] UI_DEV_SETUP: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        dprintf(2, "[uinput-kbd] UI_DEV_CREATE: %s\n", strerror(errno));
        return 1;
    }

    char sysname[64] = {0};
    if (ioctl(fd, UI_GET_SYSNAME(sizeof sysname), sysname) < 0) {
        dprintf(2, "[uinput-kbd] UI_GET_SYSNAME: %s\n", strerror(errno));
        return 1;
    }

    /* Take the node away again on the way out: the uinput device dies with this process, and a
     * node left pointing at nothing would leave Android holding a keyboard that is not there. */
    signal(SIGTERM, cleanup);
    signal(SIGINT, cleanup);
    signal(SIGHUP, cleanup);
    if (make_node(sysname) < 0) return 1;

    dprintf(2, "[uinput-kbd] created '%s' (%04x:%04x); holding open\n", us.name, us.id.vendor,
            us.id.product);
    for (;;) pause();  /* keep the device alive for the container's lifetime */
}
