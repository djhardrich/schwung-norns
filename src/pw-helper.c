/*
 * pw-helper-norns — setuid root helper for norns chroot management
 *
 * Installed as /data/UserData/schwung/bin/pw-helper (owned root, setuid bit set).
 * Callable by the ableton user from the DSP plugin.
 *
 * Usage:
 *   pw-helper start <fifo_path> <slot>
 *   pw-helper stop <slot>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MODULE_DIR "/data/UserData/schwung/modules/tools/norns"

int main(int argc, char *argv[]) {
    if (setuid(0) != 0) {
        fprintf(stderr, "pw-helper: setuid(0) failed (not setuid root?)\n");
        return 1;
    }
    if (setgid(0) != 0) {
        fprintf(stderr, "pw-helper: setgid(0) failed\n");
        return 1;
    }

    if (argc < 2) {
        fprintf(stderr, "Usage: pw-helper start <fifo_path> <slot>\n"
                        "       pw-helper stop <slot>\n");
        return 1;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: pw-helper start <fifo_path> <slot>\n");
            return 1;
        }
        int slot = atoi(argv[3]);
        if (slot < 1 || slot > 8) {
            fprintf(stderr, "pw-helper: invalid slot %d\n", slot);
            return 1;
        }
        if (strncmp(argv[2], "/tmp/pw-to-move-", 16) != 0) {
            fprintf(stderr, "pw-helper: invalid fifo path\n");
            return 1;
        }
        execl("/bin/sh", "sh", MODULE_DIR "/start-norns.sh",
              argv[2], argv[3], (char *)NULL);
        perror("pw-helper: execl failed");
        return 1;

    } else if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: pw-helper stop <slot>\n");
            return 1;
        }
        int slot = atoi(argv[2]);
        if (slot < 1 || slot > 8) {
            fprintf(stderr, "pw-helper: invalid slot %d\n", slot);
            return 1;
        }
        execl("/bin/sh", "sh", MODULE_DIR "/stop-norns.sh",
              argv[2], (char *)NULL);
        perror("pw-helper: execl failed");
        return 1;

    } else if (strcmp(argv[1], "restart") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: pw-helper restart <slot>\n");
            return 1;
        }
        int slot = atoi(argv[2]);
        if (slot < 1 || slot > 8) {
            fprintf(stderr, "pw-helper: invalid slot %d\n", slot);
            return 1;
        }
        execl("/bin/sh", "sh", MODULE_DIR "/restart-norns.sh",
              argv[2], (char *)NULL);
        perror("pw-helper: execl failed");
        return 1;

    } else if (strcmp(argv[1], "mount") == 0) {
        /* Ensure /tmp is bind-mounted into the chroot.
         * Called early by the DSP plugin before creating FIFOs. */
        const char *chroot_tmp = "/data/UserData/pw-chroot/tmp";
        /* Check if already a mountpoint */
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "mountpoint -q %s", chroot_tmp);
        if (system(cmd) == 0) {
            return 0;  /* Already mounted */
        }
        /* Set up the bind mount */
        snprintf(cmd, sizeof(cmd), "mount --bind /tmp %s", chroot_tmp);
        return system(cmd) != 0;

    } else {
        fprintf(stderr, "pw-helper: unknown command '%s'\n", argv[1]);
        return 1;
    }
}
