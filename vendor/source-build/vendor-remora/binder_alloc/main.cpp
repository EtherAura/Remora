// binder_alloc — create binderfs device nodes (bd remora-28ix.4 R2b).
//
// A container gets a private binderfs instance, and it starts EMPTY: the binder, hwbinder and
// vndbinder nodes every Android process expects do not exist until something asks binderfs to
// create them. That ask is a single BINDER_CTL_ADD ioctl on binder-control, which is all this
// program is.
//
// It runs from init.rc (`exec -- /vendor/bin/remora_binder_alloc /dev/binderfs/binder-control
// binder hwbinder vndbinder`) before anything that could use binder, so the install path and name
// are a contract with that line — which is why the binary is installed under its Remora-specific
// module name rather than as binder_alloc (see Android.bp).
//
// Failures are fatal on purpose. Without these nodes servicemanager cannot start and the boot
// dies later, further from the cause; refusing here names it.

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <linux/android/binderfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

// binderfs's own limit on a device name, from the kernel uapi.
constexpr size_t kMaxNameLen = BINDERFS_MAX_NAME;

// The uapi header ships the struct and the ioctl; declaring them locally would be a second
// definition of a kernel contract, which is exactly how such things drift.
int addDevice(int ctlFd, const char *name) {
    struct binderfs_device device;
    memset(&device, 0, sizeof(device));
    strncpy(device.name, name, kMaxNameLen);

    if (ioctl(ctlFd, BINDER_CTL_ADD, &device) < 0) {
        // EEXIST is success for our purposes: the node is there, which is all init needs. It
        // happens when this runs twice (a re-exec, or a container restarted without a fresh
        // binderfs mount), and treating it as failure would turn a healthy boot into a dead one.
        if (errno == EEXIST) {
            printf("binder_alloc: %s already exists\n", name);
            return 0;
        }
        fprintf(stderr, "binder_alloc: cannot create %s: %s\n", name, strerror(errno));
        return -1;
    }
    printf("binder_alloc: created %s (%u:%u)\n", name, device.major, device.minor);
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s BINDER-CONTROL-PATH DEVICE [DEVICE ...]\n", basename(argv[0]));
        fprintf(stderr, "example: %s /dev/binderfs/binder-control binder hwbinder vndbinder\n",
                basename(argv[0]));
        return EXIT_FAILURE;
    }

    const int ctlFd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (ctlFd < 0) {
        fprintf(stderr, "binder_alloc: cannot open %s: %s\n", argv[1], strerror(errno));
        return EXIT_FAILURE;
    }

    int rc = EXIT_SUCCESS;
    for (int i = 2; i < argc; ++i) {
        if (strlen(argv[i]) > kMaxNameLen) {
            fprintf(stderr, "binder_alloc: name too long: %s\n", argv[i]);
            rc = EXIT_FAILURE;
            continue;
        }
        // Every device is attempted even after one fails, so the log names ALL the missing nodes
        // rather than only the first — the difference between one diagnosis and three.
        if (addDevice(ctlFd, argv[i]) < 0) rc = EXIT_FAILURE;
    }

    close(ctlFd);
    return rc;
}
