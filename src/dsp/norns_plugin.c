/*
 * norns_plugin.c — Schwung DSP plugin for Norns FIFO bridge
 *                  with bidirectional MIDI FIFO bridge and screen FIFO
 *
 * Audio path:
 *   PipeWire sink → /tmp/pw-to-move-<slot> (FIFO) → ring buffer → render_block()
 *
 * MIDI path (Move → chroot):
 *   on_midi() → /tmp/midi-to-chroot-<slot> (FIFO, length-prefixed frames)
 *
 * MIDI path (chroot → Move):
 *   /tmp/midi-from-chroot-<slot> (FIFO) → pump_midi_out() → host->send_midi_internal()
 *
 * Screen path (norns matron → plugin):
 *   matron → /tmp/norns-screen-<slot> (FIFO, 1024-byte frames) → screen_buf
 *   get_param("screen_data") → hex-encoded framebuffer string
 *
 * The plugin runs as user 'ableton'. PipeWire chroot requires root, so
 * start/stop scripts are invoked via the setuid pw-helper-norns binary.
 * Move has no sudo — the setuid helper bridges ableton→root.
 *
 * Based on the proven airplay_plugin.c FIFO bridge pattern.
 */

#define _GNU_SOURCE
#include "plugin_api_v1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "../shm_audio.h"

/* ── Constants ────────────────────────────────────────── */

#define RING_SECONDS  1
#define RING_SAMPLES  (MOVE_AUDIO_SAMPLE_RATE * 2 * RING_SECONDS)
#define AUDIO_IDLE_MS 3000
#define FIFO_PIPE_SZ  (128 * 1024)  /* 128KB kernel FIFO buffer — fallback only, SHM preferred */

/* FIFOs are created at the resolved /tmp path (following symlinks).
 * On Move, /tmp → /var/tmp → /var/volatile/tmp.  The chroot bind-mounts
 * this same directory, so both host and chroot see the same files.
 * We MUST NOT use /data/UserData/pw-chroot/tmp/ because the bind mount
 * may not be set up yet when the plugin creates FIFOs, and the bind
 * mount would then hide the newly created FIFOs. */
#define FIFO_TMP_DIR  "/tmp"

/* ── Logging ──────────────────────────────────────────── */

static const host_api_v1_t *g_host = NULL;
static int g_log_fd = -1;

static void pw_log(const char *msg) {
    /* Always write to file — host->log may not be visible */
    if (g_log_fd < 0) {
        g_log_fd = open("/tmp/norns-dsp-debug.log",
                        O_WRONLY | O_CREAT | O_APPEND, 0666);
    }
    if (g_log_fd >= 0) {
        write(g_log_fd, "[norns] ", 8);
        write(g_log_fd, msg, strlen(msg));
        write(g_log_fd, "\n", 1);
    }
    if (g_host && g_host->log) {
        g_host->log("[norns] %s", msg);
    }
}

/* ── Timestamp ────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ── Instance State ───────────────────────────────────── */

typedef struct {
    char module_dir[512];
    char fifo_playback_path[256];
    char error_msg[256];
    int slot;

    int fifo_playback_fd;

    int16_t *ring;  /* heap-allocated ring buffer */
    size_t write_pos;
    uint64_t write_abs;
    uint64_t play_abs;
    uint8_t pending_bytes[4];
    uint8_t pending_len;

    float gain;
    bool pw_running;
    bool receiving_audio;
    uint64_t last_audio_ms;

    /* MIDI bridge FIFOs */
    char fifo_midi_in_path[256];   /* Move → chroot */
    char fifo_midi_out_path[256];  /* chroot → Move */
    int fifo_midi_in_fd;           /* plugin writes, bridge reads */
    int fifo_midi_out_fd;          /* bridge writes, plugin reads */

    /* Outbound MIDI accumulation buffer (handles partial FIFO reads) */
    uint8_t midi_out_buf[4096];
    uint16_t midi_out_buf_len;

    /* Screen FIFO */
    int fifo_screen_fd;                /* read end of screen FIFO */
    uint8_t screen_buf[4096];          /* latest framebuffer (4-bit packed, 128x64) */
    char screen_hex[2049];             /* 1-bit monochrome hex for get_param */
    int screen_valid;                  /* 1 if screen_hex has data */

    /* Grid emulator FIFO (matron writes LED state, plugin reads) */
    int fifo_grid_fd;
    uint8_t grid_leds[128];            /* latest 16x8 LED brightness buffer */
    int grid_valid;                    /* 1 if grid_leds has data */

    /* Screen dithering settings (controlled by knobs 6/7 via ui.js) */
    int dither_mode;      /* 0=off, 1=row-invert, 2=word-invert, 3=floyd-steinberg, 4=bayer */
    int dither_threshold; /* brightness cutoff for threshold modes (0-15, default 3) */

    /* Per-instance check counter (not static — avoids multi-instance bugs) */
    int check_counter;

    /* SHM audio ring (zero-copy path, replaces FIFO for audio) */
    shm_audio_t *shm_audio;
    char shm_audio_path[256];

    /* SHM audio input ring (Move mic/line → norns crone) */
    shm_audio_t *shm_audio_in;
    char shm_audio_in_path[256];
    bool audio_in_enabled;   /* off by default — scripts opt in */
} norns_instance_t;

static int g_instance_counter = 0;

/* ── Error Handling ───────────────────────────────────── */

static void set_error(norns_instance_t *inst, const char *msg) {
    if (!inst) return;
    snprintf(inst->error_msg, sizeof(inst->error_msg), "%s", msg);
    pw_log(msg);
}

/* ── Ring Buffer ──────────────────────────────────────── */

static size_t ring_available(const norns_instance_t *inst) {
    uint64_t avail;
    if (!inst) return 0;
    if (inst->write_abs <= inst->play_abs) return 0;
    avail = inst->write_abs - inst->play_abs;
    if (avail > (uint64_t)RING_SAMPLES) avail = (uint64_t)RING_SAMPLES;
    return (size_t)avail;
}

static void ring_push(norns_instance_t *inst, const int16_t *samples, size_t n) {
    size_t i;
    uint64_t oldest;
    for (i = 0; i < n; i++) {
        inst->ring[inst->write_pos] = samples[i];
        inst->write_pos = (inst->write_pos + 1) % RING_SAMPLES;
        inst->write_abs++;
    }
    oldest = 0;
    if (inst->write_abs > (uint64_t)RING_SAMPLES)
        oldest = inst->write_abs - (uint64_t)RING_SAMPLES;
    if (inst->play_abs < oldest)
        inst->play_abs = oldest;
}

static size_t ring_pop(norns_instance_t *inst, int16_t *out, size_t n) {
    size_t got, i;
    uint64_t abs_pos;
    if (!inst || !out || n == 0) return 0;
    got = ring_available(inst);
    if (got > n) got = n;
    abs_pos = inst->play_abs;
    for (i = 0; i < got; i++) {
        out[i] = inst->ring[(size_t)(abs_pos % (uint64_t)RING_SAMPLES)];
        abs_pos++;
    }
    inst->play_abs = abs_pos;
    return got;
}

/* ── FIFO Management ──────────────────────────────────── */

static void ensure_chroot_tmp_bind(void) {
    /* The chroot's /tmp must be bind-mounted from the host's /tmp BEFORE
     * we create any FIFOs.  After a reboot the bind mount is gone, and
     * creating FIFOs at /tmp/ on the host places them on the root-fs mount.
     * When start-norns.sh later bind-mounts /tmp over $CHROOT/tmp, the FIFOs
     * become hidden from chroot processes → pipe data never flows.
     *
     * Fix: set up the bind mount here, before FIFO creation.  pw-helper
     * runs as root (setuid), but create_instance runs as ableton, so we
     * fork+exec pw-helper with a "mount" arg.  If it's already mounted,
     * this is a no-op. */
    struct stat st_host, st_chroot;
    static int done = 0;
    if (done) return;
    done = 1;

    /* Quick check: if both /tmp and $CHROOT/tmp show the same device+inode
     * for a sentinel file, the bind mount is already in place. */
    if (stat("/tmp/.", &st_host) == 0 &&
        stat("/data/UserData/pw-chroot/tmp/.", &st_chroot) == 0 &&
        st_host.st_dev == st_chroot.st_dev &&
        st_host.st_ino == st_chroot.st_ino) {
        pw_log("chroot /tmp bind mount already active");
        return;
    }

    pw_log("chroot /tmp bind mount missing — setting up");
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: mount --bind /tmp $CHROOT/tmp via pw-helper or directly */
        execl("/data/UserData/schwung/bin/pw-helper-norns",
              "pw-helper-norns", "mount", (char *)NULL);
        /* Fallback: try mount directly (will fail unless we're root) */
        execl("/bin/mount", "mount", "--bind", "/tmp",
              "/data/UserData/pw-chroot/tmp", (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            pw_log("bind mount setup OK");
        else
            pw_log("bind mount setup failed (will retry in start-norns.sh)");
    }
}

static int create_fifo(norns_instance_t *inst) {
    struct stat st;

    if (!inst) return -1;

    /* Ensure bind mount is in place before creating FIFOs */
    ensure_chroot_tmp_bind();

    snprintf(inst->fifo_playback_path, sizeof(inst->fifo_playback_path),
             FIFO_TMP_DIR "/pw-to-move-%d", inst->slot);

    pw_log("create_fifo: creating FIFO");

    /* Remove stale FIFO. If unlink fails (owned by another user), try
     * opening the existing FIFO — it may still be usable. */
    if (unlink(inst->fifo_playback_path) != 0 && errno != ENOENT) {
        /* Can't remove — check if it's already a FIFO we can use */
        if (stat(inst->fifo_playback_path, &st) == 0 && S_ISFIFO(st.st_mode)) {
            pw_log("create_fifo: reusing existing FIFO");
            inst->fifo_playback_fd = open(inst->fifo_playback_path, O_RDWR | O_NONBLOCK);
            if (inst->fifo_playback_fd >= 0) {
                (void)fcntl(inst->fifo_playback_fd, F_SETPIPE_SZ, FIFO_PIPE_SZ);
                pw_log("create_fifo: reuse OK");
                return 0;
            }
        }
        set_error(inst, "cannot remove stale FIFO");
        return -1;
    }

    if (mkfifo(inst->fifo_playback_path, 0666) != 0) {
        set_error(inst, "mkfifo failed");
        return -1;
    }

    inst->fifo_playback_fd = open(inst->fifo_playback_path, O_RDWR | O_NONBLOCK);
    if (inst->fifo_playback_fd < 0) {
        set_error(inst, "open FIFO failed");
        (void)unlink(inst->fifo_playback_path);
        return -1;
    }

    /* Increase kernel pipe buffer to reduce dropouts */
    (void)fcntl(inst->fifo_playback_fd, F_SETPIPE_SZ, FIFO_PIPE_SZ);

    pw_log("create_fifo: OK");

    /* Create SHM ring for zero-copy audio (jack-fifo-bridge will use this) */
    snprintf(inst->shm_audio_path, sizeof(inst->shm_audio_path),
             SHM_AUDIO_PATH_FMT, inst->slot);
    {
        int shm_fd = open(inst->shm_audio_path,
                          O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (shm_fd >= 0) {
            (void)fchmod(shm_fd, 0666);  /* override umask */
            if (ftruncate(shm_fd, SHM_AUDIO_FILE_SIZE) == 0) {
                void *p = mmap(NULL, SHM_AUDIO_FILE_SIZE,
                               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
                if (p != MAP_FAILED) {
                    inst->shm_audio = (shm_audio_t *)p;
                    shm_audio_init(inst->shm_audio, MOVE_AUDIO_SAMPLE_RATE);
                    pw_log("create_fifo: SHM ring created (zero-copy)");
                }
            }
            close(shm_fd);
        }
        if (!inst->shm_audio)
            pw_log("create_fifo: SHM setup failed, FIFO-only mode");
    }

    /* Create audio INPUT SHM ring (Move mic/line → norns crone).
     * Always created but only written to when audio_in_enabled is set. */
    snprintf(inst->shm_audio_in_path, sizeof(inst->shm_audio_in_path),
             SHM_AUDIO_IN_PATH_FMT, inst->slot);
    {
        int shm_fd = open(inst->shm_audio_in_path,
                          O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (shm_fd >= 0) {
            (void)fchmod(shm_fd, 0666);
            if (ftruncate(shm_fd, SHM_AUDIO_FILE_SIZE) == 0) {
                void *p = mmap(NULL, SHM_AUDIO_FILE_SIZE,
                               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
                if (p != MAP_FAILED) {
                    inst->shm_audio_in = (shm_audio_t *)p;
                    shm_audio_init(inst->shm_audio_in, MOVE_AUDIO_SAMPLE_RATE);
                    pw_log("create_fifo: audio input SHM created");
                }
            }
            close(shm_fd);
        }
    }

    return 0;
}

static void close_fifo(norns_instance_t *inst) {
    if (!inst) return;
    if (inst->shm_audio) {
        munmap(inst->shm_audio, SHM_AUDIO_FILE_SIZE);
        inst->shm_audio = NULL;
    }
    if (inst->shm_audio_path[0])
        (void)unlink(inst->shm_audio_path);
    if (inst->shm_audio_in) {
        munmap(inst->shm_audio_in, SHM_AUDIO_FILE_SIZE);
        inst->shm_audio_in = NULL;
    }
    if (inst->shm_audio_in_path[0])
        (void)unlink(inst->shm_audio_in_path);
    if (inst->fifo_playback_fd >= 0) {
        close(inst->fifo_playback_fd);
        inst->fifo_playback_fd = -1;
    }
    if (inst->fifo_playback_path[0] != '\0') {
        (void)unlink(inst->fifo_playback_path);
    }
}

/* ── MIDI FIFO Management ────────────────────────────── */

static int create_midi_fifos(norns_instance_t *inst) {
    const char *paths[2];
    int *fds[2];
    int i;

    if (!inst) return -1;

    snprintf(inst->fifo_midi_in_path, sizeof(inst->fifo_midi_in_path),
             FIFO_TMP_DIR "/midi-to-chroot-%d", inst->slot);
    snprintf(inst->fifo_midi_out_path, sizeof(inst->fifo_midi_out_path),
             FIFO_TMP_DIR "/midi-from-chroot-%d", inst->slot);

    paths[0] = inst->fifo_midi_in_path;
    paths[1] = inst->fifo_midi_out_path;
    fds[0] = &inst->fifo_midi_in_fd;
    fds[1] = &inst->fifo_midi_out_fd;

    for (i = 0; i < 2; i++) {
        struct stat st;

        if (unlink(paths[i]) != 0 && errno != ENOENT) {
            if (stat(paths[i], &st) == 0 && S_ISFIFO(st.st_mode)) {
                *fds[i] = open(paths[i], O_RDWR | O_NONBLOCK);
                if (*fds[i] >= 0) {
                    pw_log("create_midi_fifos: reusing existing FIFO");
                    continue;
                }
            }
            set_error(inst, "cannot remove stale MIDI FIFO");
            return -1;
        }

        if (mkfifo(paths[i], 0666) != 0) {
            set_error(inst, "mkfifo MIDI failed");
            return -1;
        }

        *fds[i] = open(paths[i], O_RDWR | O_NONBLOCK);
        if (*fds[i] < 0) {
            set_error(inst, "open MIDI FIFO failed");
            (void)unlink(paths[i]);
            return -1;
        }
    }

    pw_log("create_midi_fifos: OK");
    return 0;
}

static void close_midi_fifos(norns_instance_t *inst) {
    if (!inst) return;
    if (inst->fifo_midi_in_fd >= 0) {
        close(inst->fifo_midi_in_fd);
        inst->fifo_midi_in_fd = -1;
    }
    if (inst->fifo_midi_out_fd >= 0) {
        close(inst->fifo_midi_out_fd);
        inst->fifo_midi_out_fd = -1;
    }
    if (inst->fifo_midi_in_path[0] != '\0')
        (void)unlink(inst->fifo_midi_in_path);
    if (inst->fifo_midi_out_path[0] != '\0')
        (void)unlink(inst->fifo_midi_out_path);
}

/* ── Pipe Pump (FIFO → Ring Buffer) ──────────────────── */

static void pump_pipe(norns_instance_t *inst) {
    uint8_t buf[4096];
    uint8_t merged[4100];
    int16_t samples[2048];

    if (!inst || inst->fifo_playback_fd < 0) return;

    while (1) {
        if (ring_available(inst) + 2048 >= (size_t)RING_SAMPLES) break;

        ssize_t n = read(inst->fifo_playback_fd, buf, sizeof(buf));
        if (n > 0) {
            size_t merged_bytes = inst->pending_len;
            size_t total_bytes, aligned_bytes, remainder;

            if (inst->pending_len > 0)
                memcpy(merged, inst->pending_bytes, inst->pending_len);
            memcpy(merged + merged_bytes, buf, (size_t)n);
            total_bytes = merged_bytes + (size_t)n;

            /* jack-fifo-bridge writes S16LE interleaved stereo */
            aligned_bytes = total_bytes & ~((size_t)3U);
            remainder = total_bytes - aligned_bytes;
            if (remainder > 0)
                memcpy(inst->pending_bytes, merged + aligned_bytes, remainder);
            inst->pending_len = (uint8_t)remainder;

            {
                size_t sample_count = aligned_bytes / sizeof(int16_t);
                if (sample_count > 0) {
                    memcpy(samples, merged, sample_count * sizeof(int16_t));
                    ring_push(inst, samples, sample_count);
                }
            }

            inst->last_audio_ms = now_ms();
            inst->receiving_audio = true;

            if ((size_t)n < sizeof(buf)) break;
            continue;
        }

        break;
    }

    if (inst->receiving_audio && inst->last_audio_ms > 0) {
        uint64_t now = now_ms();
        if (now > inst->last_audio_ms && (now - inst->last_audio_ms) > AUDIO_IDLE_MS)
            inst->receiving_audio = false;
    }
}

/* ── MIDI Outbound Pump (chroot → Move) ──────────────── */

static void pump_midi_out(norns_instance_t *inst) {
    uint8_t tmp[512];
    if (!inst || inst->fifo_midi_out_fd < 0) return;

    while (1) {
        size_t space = sizeof(inst->midi_out_buf) - inst->midi_out_buf_len;
        if (space == 0) break;

        ssize_t n = read(inst->fifo_midi_out_fd, tmp,
                         space < sizeof(tmp) ? space : sizeof(tmp));
        if (n <= 0) break;

        memcpy(inst->midi_out_buf + inst->midi_out_buf_len, tmp, (size_t)n);
        inst->midi_out_buf_len += (uint16_t)n;
    }

    size_t pos = 0;
    while (pos + 2 <= inst->midi_out_buf_len) {
        uint16_t msg_len = (uint16_t)inst->midi_out_buf[pos]
                         | ((uint16_t)inst->midi_out_buf[pos + 1] << 8);
        if (msg_len == 0) { pos += 2; continue; }
        if (pos + 2 + msg_len > inst->midi_out_buf_len) break;

        if (g_host && g_host->send_midi_internal)
            g_host->send_midi_internal(inst->midi_out_buf + pos + 2, msg_len);
        pos += 2 + msg_len;
    }

    if (pos > 0 && pos < inst->midi_out_buf_len) {
        memmove(inst->midi_out_buf, inst->midi_out_buf + pos,
                inst->midi_out_buf_len - pos);
        inst->midi_out_buf_len -= (uint16_t)pos;
    } else if (pos >= inst->midi_out_buf_len) {
        inst->midi_out_buf_len = 0;
    }
}

/* ── Screen FIFO Pump (matron → plugin) ──────────────── */

static void pump_screen(norns_instance_t *inst) {
    if (inst->fifo_screen_fd < 0) return;
    uint8_t tmp[4096];
    int got_frame = 0;
    for (;;) {
        ssize_t n = read(inst->fifo_screen_fd, tmp, 4096);
        if (n == 4096) {
            memcpy(inst->screen_buf, tmp, 4096);
            got_frame = 1;
        } else {
            break;
        }
    }
    if (got_frame) {
        static const char hex[] = "0123456789abcdef";
        /* Convert 4-bit grayscale to 1-bit monochrome.
         *
         * Dither modes (controlled by knob 7 via ui.js):
         *   0 = Off — pure threshold, no tricks
         *   1 = Adaptive row inversion — invert entire bright rows
         *   2 = Adaptive word inversion — invert only bright pixel spans
         *   3 = Floyd-Steinberg error diffusion
         *   4 = Bayer 4x4 ordered dithering
         *   5 = Atkinson dithering — classic Mac, crisper than F-S
         *   6 = Highlight cursor — no dithering, '>' marker on bright rows
         *   7 = High contrast — only brightest pixels visible (level > 10)
         *
         * Threshold (controlled by knob 6): brightness cutoff (0-15) */
        uint8_t mono[1024];
        int x, y, src_idx, dst_idx;
        int thresh = inst->dither_threshold;

        /* Unpack 4-bit grayscale to 8-bit working buffer */
        uint8_t gray[128 * 64];
        for (y = 0; y < 64; y++) {
            for (x = 0; x < 128; x++) {
                src_idx = y * 64 + x / 2;
                gray[y * 128 + x] = (x & 1)
                    ? (inst->screen_buf[src_idx] & 0xF)
                    : ((inst->screen_buf[src_idx] >> 4) & 0xF);
            }
        }

        memset(mono, 0, 1024);

        switch (inst->dither_mode) {

        case 0: /* ── Off: pure threshold ────────────────────── */
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    if (gray[y * 128 + x] > thresh) {
                        dst_idx = y * 16 + x / 8;
                        mono[dst_idx] |= (1 << (7 - (x & 7)));
                    }
                }
            }
            break;

        case 1: { /* ── Adaptive row inversion ───────────────── */
            uint8_t row_max[64];
            int bright_rows = 0, dim_rows = 0;
            memset(row_max, 0, 64);
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    uint8_t v = gray[y * 128 + x];
                    if (v > row_max[y]) row_max[y] = v;
                }
                if (row_max[y] > 10) bright_rows++;
                else if (row_max[y] > 2) dim_rows++;
            }
            int do_invert = (bright_rows > 0 && dim_rows > 0);
            for (y = 0; y < 64; y++) {
                int inv = do_invert && (row_max[y] > 10);
                for (x = 0; x < 128; x++) {
                    int pixel = inv
                        ? (gray[y * 128 + x] <= 2)
                        : (gray[y * 128 + x] > thresh);
                    if (pixel) {
                        dst_idx = y * 16 + x / 8;
                        mono[dst_idx] |= (1 << (7 - (x & 7)));
                    }
                }
            }
            break;
        }

        case 2: { /* ── Adaptive word inversion ──────────────── */
            /* Like row inversion but only inverts contiguous bright
             * pixel spans with 1px padding, not entire rows. */
            uint8_t row_max[64];
            int bright_rows = 0, dim_rows = 0;
            memset(row_max, 0, 64);
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    uint8_t v = gray[y * 128 + x];
                    if (v > row_max[y]) row_max[y] = v;
                }
                if (row_max[y] > 10) bright_rows++;
                else if (row_max[y] > 2) dim_rows++;
            }
            int do_invert = (bright_rows > 0 && dim_rows > 0);
            for (y = 0; y < 64; y++) {
                if (do_invert && row_max[y] > 10) {
                    /* Find bright spans and invert only those regions */
                    /* First pass: mark bright pixel extents per row */
                    int span_start = -1, span_end = -1;
                    /* Find leftmost and rightmost bright pixel */
                    for (x = 0; x < 128; x++) {
                        if (gray[y * 128 + x] > thresh) {
                            if (span_start < 0) span_start = x;
                            span_end = x;
                        }
                    }
                    if (span_start >= 0) {
                        /* Add 1px padding on each side */
                        int pad_start = (span_start > 1) ? span_start - 1 : 0;
                        int pad_end = (span_end < 127) ? span_end + 1 : 127;
                        for (x = 0; x < 128; x++) {
                            int pixel;
                            if (x >= pad_start && x <= pad_end) {
                                /* Inside bright span: invert */
                                pixel = (gray[y * 128 + x] <= 2);
                            } else {
                                /* Outside: normal threshold */
                                pixel = (gray[y * 128 + x] > thresh);
                            }
                            if (pixel) {
                                dst_idx = y * 16 + x / 8;
                                mono[dst_idx] |= (1 << (7 - (x & 7)));
                            }
                        }
                    }
                } else {
                    /* Normal rows: plain threshold */
                    for (x = 0; x < 128; x++) {
                        if (gray[y * 128 + x] > thresh) {
                            dst_idx = y * 16 + x / 8;
                            mono[dst_idx] |= (1 << (7 - (x & 7)));
                        }
                    }
                }
            }
            break;
        }

        case 3: { /* ── Floyd-Steinberg error diffusion ──────── */
            /* Scale 4-bit (0-15) to 8-bit for error accumulation */
            int16_t err_buf[128 * 64];
            for (y = 0; y < 64; y++)
                for (x = 0; x < 128; x++)
                    err_buf[y * 128 + x] = (int16_t)(gray[y * 128 + x] * 17);
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    int16_t old_val = err_buf[y * 128 + x];
                    if (old_val < 0) old_val = 0;
                    if (old_val > 255) old_val = 255;
                    int new_val = (old_val > (thresh * 17)) ? 255 : 0;
                    int16_t quant_err = old_val - (int16_t)new_val;
                    if (new_val) {
                        dst_idx = y * 16 + x / 8;
                        mono[dst_idx] |= (1 << (7 - (x & 7)));
                    }
                    /* Distribute error to neighbors */
                    if (x + 1 < 128)
                        err_buf[y * 128 + x + 1] += (quant_err * 7) / 16;
                    if (y + 1 < 64) {
                        if (x > 0)
                            err_buf[(y+1) * 128 + x - 1] += (quant_err * 3) / 16;
                        err_buf[(y+1) * 128 + x] += (quant_err * 5) / 16;
                        if (x + 1 < 128)
                            err_buf[(y+1) * 128 + x + 1] += (quant_err * 1) / 16;
                    }
                }
            }
            break;
        }

        case 4: { /* ── Bayer 4x4 ordered dithering ─────────── */
            static const uint8_t bayer4[4][4] = {
                {  0,  8,  2, 10 },
                { 12,  4, 14,  6 },
                {  3, 11,  1,  9 },
                { 15,  7, 13,  5 }
            };
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    uint8_t v = gray[y * 128 + x];
                    /* Compare brightness against Bayer threshold matrix */
                    if (v > bayer4[y & 3][x & 3]) {
                        dst_idx = y * 16 + x / 8;
                        mono[dst_idx] |= (1 << (7 - (x & 7)));
                    }
                }
            }
            break;
        }

        case 5: { /* ── Atkinson dithering ──────────────────── */
            /* Classic Macintosh dither. Distributes 6/8 of quantization
             * error (discards 2/8), preserving bright areas and avoiding
             * smearing into dark regions.  Crisper than Floyd-Steinberg
             * on small displays. */
            int16_t err_buf[128 * 64];
            for (y = 0; y < 64; y++)
                for (x = 0; x < 128; x++)
                    err_buf[y * 128 + x] = (int16_t)(gray[y * 128 + x] * 17);
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    int16_t old_val = err_buf[y * 128 + x];
                    if (old_val < 0) old_val = 0;
                    if (old_val > 255) old_val = 255;
                    int new_val = (old_val > (thresh * 17)) ? 255 : 0;
                    int16_t frac = (old_val - (int16_t)new_val) / 8;
                    if (new_val) {
                        dst_idx = y * 16 + x / 8;
                        mono[dst_idx] |= (1 << (7 - (x & 7)));
                    }
                    /* Atkinson: 1/8 to each of 6 neighbors (6/8 total, 2/8 lost) */
                    if (x + 1 < 128)
                        err_buf[y * 128 + x + 1] += frac;
                    if (x + 2 < 128)
                        err_buf[y * 128 + x + 2] += frac;
                    if (y + 1 < 64) {
                        if (x > 0)
                            err_buf[(y+1) * 128 + x - 1] += frac;
                        err_buf[(y+1) * 128 + x] += frac;
                        if (x + 1 < 128)
                            err_buf[(y+1) * 128 + x + 1] += frac;
                    }
                    if (y + 2 < 64)
                        err_buf[(y+2) * 128 + x] += frac;
                }
            }
            break;
        }

        case 6: { /* ── Highlight cursor ─────────────────────── */
            /* No dithering. Plain threshold with a '>' cursor drawn
             * at the left edge of bright rows (instead of inversion).
             * Clean text, minimal noise, very readable for menus. */
            uint8_t row_max[64];
            int bright_rows = 0, dim_rows = 0;
            memset(row_max, 0, 64);
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    uint8_t v = gray[y * 128 + x];
                    if (v > row_max[y]) row_max[y] = v;
                }
                if (row_max[y] > 10) bright_rows++;
                else if (row_max[y] > 2) dim_rows++;
            }
            int do_cursor = (bright_rows > 0 && dim_rows > 0);
            /* Render all text with threshold */
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    if (gray[y * 128 + x] > thresh) {
                        dst_idx = y * 16 + x / 8;
                        mono[dst_idx] |= (1 << (7 - (x & 7)));
                    }
                }
            }
            if (do_cursor) {
                /* Draw '>' cursor: find vertical center of each bright band.
                 * A band is a group of consecutive rows with max > 10. */
                int in_band = 0, band_start = 0;
                for (y = 0; y <= 64; y++) {
                    int is_bright = (y < 64) && (row_max[y] > 10);
                    if (is_bright && !in_band) {
                        band_start = y;
                        in_band = 1;
                    } else if (!is_bright && in_band) {
                        /* Draw '>' at vertical center of this band, col 0-3 */
                        int mid = (band_start + y) / 2;
                        /* Simple 5-pixel '>' shape */
                        int cy;
                        for (cy = -2; cy <= 2; cy++) {
                            int py = mid + cy;
                            int px = (cy < 0 ? 2 + cy : 2 - cy); /* 0,1,2,1,0 */
                            if (py >= 0 && py < 64 && px >= 0 && px < 128) {
                                dst_idx = py * 16 + px / 8;
                                mono[dst_idx] |= (1 << (7 - (px & 7)));
                            }
                        }
                        in_band = 0;
                    }
                }
            }
            break;
        }

        case 7: { /* ── High contrast ────────────────────────── */
            /* Only the brightest pixels (level > 10) are visible.
             * Everything else is black. Useful for cluttered scripts
             * where you just want to see the selected/active element. */
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    if (gray[y * 128 + x] > 10) {
                        dst_idx = y * 16 + x / 8;
                        mono[dst_idx] |= (1 << (7 - (x & 7)));
                    }
                }
            }
            break;
        }

        default: /* fallback: threshold */
            for (y = 0; y < 64; y++) {
                for (x = 0; x < 128; x++) {
                    if (gray[y * 128 + x] > thresh) {
                        dst_idx = y * 16 + x / 8;
                        mono[dst_idx] |= (1 << (7 - (x & 7)));
                    }
                }
            }
            break;
        }

        for (x = 0; x < 1024; x++) {
            inst->screen_hex[x * 2]     = hex[(mono[x] >> 4) & 0xF];
            inst->screen_hex[x * 2 + 1] = hex[mono[x] & 0xF];
        }
        inst->screen_hex[2048] = '\0';
        inst->screen_valid = 1;
    }
}

/* ── Grid LED Pump (matron device_monome → plugin) ───── */

static void pump_grid(norns_instance_t *inst) {
    if (!inst || inst->fifo_grid_fd < 0) return;
    uint8_t tmp[128];
    int got_frame = 0;
    for (;;) {
        ssize_t n = read(inst->fifo_grid_fd, tmp, 128);
        if (n == 128) {
            memcpy(inst->grid_leds, tmp, 128);
            got_frame = 1;
        } else {
            break;
        }
    }
    if (got_frame) inst->grid_valid = 1;
}

/* ── PipeWire Chroot Daemon ───────────────────────────── */

static void start_pw_chroot(norns_instance_t *inst) {
    char slot_str[8];
    if (!inst) return;

    /* Plugin runs as 'ableton' but chroot/mount need root.
     * Use setuid pw-helper binary installed at /usr/local/bin.
     * Fork + exec to avoid blocking the audio thread. */
    snprintf(slot_str, sizeof(slot_str), "%d", inst->slot);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        /* Close all FIFOs so child/PipeWire don't inherit them */
        if (inst->fifo_playback_fd >= 0)
            close(inst->fifo_playback_fd);
        if (inst->fifo_midi_in_fd >= 0)
            close(inst->fifo_midi_in_fd);
        if (inst->fifo_midi_out_fd >= 0)
            close(inst->fifo_midi_out_fd);
        if (inst->fifo_screen_fd >= 0)
            close(inst->fifo_screen_fd);
        if (inst->fifo_grid_fd >= 0)
            close(inst->fifo_grid_fd);
        /* Close log fd */
        if (g_log_fd >= 0)
            close(g_log_fd);
        int fd = open("/tmp/pw-start.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execl("/data/UserData/schwung/bin/pw-helper-norns", "pw-helper-norns", "start",
              inst->fifo_playback_path, slot_str, (char *)NULL);
        _exit(127);
    }

    /* Don't wait for child — it runs in background */
    inst->pw_running = true;
    pw_log("PipeWire chroot launch requested (via pw-helper-norns)");
}

static void stop_pw_chroot(norns_instance_t *inst) {
    char slot_str[8];
    if (!inst) return;

    snprintf(slot_str, sizeof(slot_str), "%d", inst->slot);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        /* Close inherited fds */
        if (inst->fifo_playback_fd >= 0) close(inst->fifo_playback_fd);
        if (inst->fifo_midi_in_fd >= 0) close(inst->fifo_midi_in_fd);
        if (inst->fifo_midi_out_fd >= 0) close(inst->fifo_midi_out_fd);
        if (inst->fifo_screen_fd >= 0) close(inst->fifo_screen_fd);
        if (g_log_fd >= 0) close(g_log_fd);
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execl("/data/UserData/schwung/bin/pw-helper-norns", "pw-helper-norns", "stop",
              slot_str, (char *)NULL);
        _exit(127);
    }
    /* Don't wait — stop runs in background */

    inst->pw_running = false;
    pw_log("Norns chroot stop requested (via pw-helper-norns)");
}

static void check_pw_alive(norns_instance_t *inst) {
    char pid_path[64];
    char pid_buf[16];
    int fd, pid;

    if (!inst) return;
    if (++inst->check_counter < 17200) return;  /* ~every 50 seconds */
    inst->check_counter = 0;

    if (!inst || !inst->pw_running) return;

    /* Non-blocking check: read PID file and test with kill(pid, 0) */
    snprintf(pid_path, sizeof(pid_path), "/tmp/norns-pids-%d/pipewire.pid", inst->slot);
    fd = open(pid_path, O_RDONLY);
    if (fd < 0) {
        inst->pw_running = false;
        pw_log("PipeWire PID file not found");
        return;
    }
    memset(pid_buf, 0, sizeof(pid_buf));
    (void)read(fd, pid_buf, sizeof(pid_buf) - 1);
    close(fd);
    pid = atoi(pid_buf);
    if (pid <= 0 || kill(pid, 0) != 0) {
        inst->pw_running = false;
        pw_log("PipeWire process not found");
    }
}

/* ── Plugin API v2 Implementation ─────────────────────── */

static void *v2_create_instance(const char *module_dir, const char *json_defaults) {
    norns_instance_t *inst;

    pw_log("create_instance: enter");

    inst = calloc(1, sizeof(*inst));
    if (!inst) { pw_log("create_instance: calloc failed"); return NULL; }

    inst->slot = ++g_instance_counter;
    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s",
             module_dir ? module_dir : ".");
    inst->gain = 1.0f;
    inst->fifo_playback_fd = -1;
    inst->shm_audio = NULL;
    inst->shm_audio_in = NULL;
    inst->audio_in_enabled = false;
    inst->fifo_midi_in_fd = -1;
    inst->fifo_midi_out_fd = -1;
    inst->midi_out_buf_len = 0;
    inst->fifo_screen_fd = -1;
    inst->screen_valid = 0;
    inst->fifo_grid_fd = -1;
    inst->grid_valid = 0;
    memset(inst->grid_leds, 0, 128);
    inst->dither_mode = 0;      /* default: off (pure threshold) */
    inst->dither_threshold = 3; /* default: brightness > 3 = white */
    (void)json_defaults;

    /* Heap-allocate ring buffer */
    inst->ring = calloc(RING_SAMPLES, sizeof(int16_t));
    if (!inst->ring) {
        pw_log("create_instance: ring alloc failed");
        free(inst);
        return NULL;
    }

    if (create_fifo(inst) != 0) {
        pw_log("create_instance: FIFO failed");
        free(inst->ring);
        free(inst);
        return NULL;
    }

    if (create_midi_fifos(inst) != 0) {
        pw_log("create_instance: MIDI FIFO failed (continuing without MIDI)");
        /* Non-fatal — audio still works */
    }

    /* Screen FIFO: matron writes, plugin reads */
    char screen_path[64];
    snprintf(screen_path, sizeof(screen_path), FIFO_TMP_DIR "/norns-screen-%d", inst->slot);
    if (mkfifo(screen_path, 0666) != 0 && errno != EEXIST) {
        inst->fifo_screen_fd = -1;
    } else {
        chmod(screen_path, 0666);
        inst->fifo_screen_fd = open(screen_path, O_RDWR | O_NONBLOCK);
    }

    /* Input FIFO: norns-input-bridge writes, matron reads */
    char input_path[64];
    snprintf(input_path, sizeof(input_path), FIFO_TMP_DIR "/norns-input-%d", inst->slot);
    if (mkfifo(input_path, 0666) != 0 && errno != EEXIST) {
        /* non-fatal */
    }
    chmod(input_path, 0666);

    /* Grid LED FIFO: matron (device_monome) writes, plugin reads */
    char grid_path[64];
    snprintf(grid_path, sizeof(grid_path), FIFO_TMP_DIR "/norns-grid-%d", inst->slot);
    if (mkfifo(grid_path, 0666) != 0 && errno != EEXIST) {
        inst->fifo_grid_fd = -1;
    } else {
        chmod(grid_path, 0666);
        inst->fifo_grid_fd = open(grid_path, O_RDWR | O_NONBLOCK);
    }

    /* Start PipeWire in background — don't block or fail on error */
    start_pw_chroot(inst);

    pw_log("create_instance: OK");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    norns_instance_t *inst = (norns_instance_t *)instance;
    if (!inst) return;
    pw_log("destroy_instance");

    stop_pw_chroot(inst);
    close_midi_fifos(inst);
    close_fifo(inst);

    if (inst->fifo_screen_fd >= 0) close(inst->fifo_screen_fd);
    if (inst->fifo_grid_fd >= 0) close(inst->fifo_grid_fd);

    free(inst->ring);
    free(inst);
    /* Don't decrement g_instance_counter — prevents slot collisions
     * if instances are created/destroyed out of order */
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    norns_instance_t *inst = (norns_instance_t *)instance;
    if (!inst || !msg || len <= 0 || len > 65535) return;

    /* Debug log all incoming MIDI — check /tmp/norns-dsp-debug.log */
    {
        char dbg[128];
        if (len >= 3)
            snprintf(dbg, sizeof(dbg), "on_midi: src=%d [%02x %02x %02x]",
                     source, msg[0], msg[1], msg[2]);
        else if (len >= 2)
            snprintf(dbg, sizeof(dbg), "on_midi: src=%d [%02x %02x]",
                     source, msg[0], msg[1]);
        else
            snprintf(dbg, sizeof(dbg), "on_midi: src=%d [%02x]",
                     source, msg[0]);
        pw_log(dbg);
    }

    if (inst->fifo_midi_in_fd < 0) return;

    /* Cap at practical size for stack allocation */
    if (len > 4096) return;

    /* 2-byte LE length prefix + raw MIDI bytes */
    uint8_t frame[4098];
    uint16_t ulen = (uint16_t)len;
    frame[0] = (uint8_t)(ulen & 0xFF);
    frame[1] = (uint8_t)((ulen >> 8) & 0xFF);
    memcpy(frame + 2, msg, len);

    /* Non-blocking write — drop if FIFO full (acceptable for MIDI) */
    (void)write(inst->fifo_midi_in_fd, frame, 2 + len);
    (void)source;
}

static uint64_t g_setparam_count = 0;

static void v2_set_param(void *instance, const char *key, const char *val) {
    norns_instance_t *inst = (norns_instance_t *)instance;
    if (!inst || !key || !val) return;

    /* Log first 20 set_param calls for debugging */
    if (++g_setparam_count <= 20) {
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "set_param[%llu]: key='%s' val='%.40s'",
                 (unsigned long long)g_setparam_count, key, val);
        pw_log(dbg);
    }

    if (strcmp(key, "gain") == 0) {
        inst->gain = strtof(val, NULL);
        if (inst->gain < 0.0f) inst->gain = 0.0f;
        if (inst->gain > 2.0f) inst->gain = 2.0f;
    } else if (strcmp(key, "restart") == 0) {
        stop_pw_chroot(inst);
        start_pw_chroot(inst);
    } else if (strcmp(key, "midi_in") == 0) {
        /* Parse hex MIDI from ui.js (e.g. "b0 47 3f") and write to FIFO */
        if (inst->fifo_midi_in_fd < 0) return;
        unsigned int b[3] = {0, 0, 0};
        int n = sscanf(val, "%x %x %x", &b[0], &b[1], &b[2]);
        if (n >= 1 && n <= 3) {
            uint8_t msg[3];
            int i;
            for (i = 0; i < n; i++) msg[i] = (uint8_t)b[i];
            /* 2-byte LE length prefix + raw MIDI bytes */
            uint8_t frame[5];
            uint16_t ulen = (uint16_t)n;
            frame[0] = (uint8_t)(ulen & 0xFF);
            frame[1] = (uint8_t)((ulen >> 8) & 0xFF);
            memcpy(frame + 2, msg, n);
            (void)write(inst->fifo_midi_in_fd, frame, 2 + n);
        }
    } else if (strcmp(key, "grid_key") == 0) {
        /* Parse "x y state" from ui.js, write as grid key marker to MIDI FIFO.
         * Uses 0xF9 (MIDI undefined) as a grid key marker byte.
         * norns-input-bridge detects this and emits type 3 frames. */
        if (inst->fifo_midi_in_fd < 0) return;
        unsigned int gx = 0, gy = 0, gs = 0;
        if (sscanf(val, "%u %u %u", &gx, &gy, &gs) == 3) {
            uint8_t msg[4] = { 0xF9, (uint8_t)gx, (uint8_t)gy, (uint8_t)gs };
            uint8_t frame[6];
            uint16_t ulen = 4;
            frame[0] = (uint8_t)(ulen & 0xFF);
            frame[1] = (uint8_t)((ulen >> 8) & 0xFF);
            memcpy(frame + 2, msg, 4);
            (void)write(inst->fifo_midi_in_fd, frame, 6);
        }
    } else if (strcmp(key, "audio_in") == 0) {
        inst->audio_in_enabled = (atoi(val) != 0);
        pw_log(inst->audio_in_enabled
               ? "audio input ENABLED" : "audio input disabled");
    } else if (strcmp(key, "dither_mode") == 0) {
        int m = atoi(val);
        if (m >= 0 && m <= 7) inst->dither_mode = m;
    } else if (strcmp(key, "dither_threshold") == 0) {
        int t = atoi(val);
        if (t >= 0 && t <= 15) inst->dither_threshold = t;
    }
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    norns_instance_t *inst = (norns_instance_t *)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "screen_data") == 0) {
        pump_screen(inst);
        if (inst->screen_valid && buf_len >= 2049) {
            memcpy(buf, inst->screen_hex, 2049);
            return 2048;
        }
        buf[0] = '\0';
        return 0;
    } else if (strcmp(key, "grid_leds") == 0) {
        pump_grid(inst);
        if (inst->grid_valid && buf_len >= 257) {
            static const char hx[] = "0123456789abcdef";
            int i;
            for (i = 0; i < 128; i++) {
                buf[i * 2]     = hx[(inst->grid_leds[i] >> 4) & 0xF];
                buf[i * 2 + 1] = hx[inst->grid_leds[i] & 0xF];
            }
            buf[256] = '\0';
            return 256;
        }
        buf[0] = '\0';
        return 0;
    } else if (strcmp(key, "gain") == 0) {
        snprintf(buf, buf_len, "%.2f", inst->gain);
    } else if (strcmp(key, "status") == 0) {
        if (inst->pw_running && inst->receiving_audio)
            snprintf(buf, buf_len, "receiving");
        else if (inst->pw_running)
            snprintf(buf, buf_len, "running");
        else
            snprintf(buf, buf_len, "stopped");
    } else if (strcmp(key, "fifo") == 0) {
        snprintf(buf, buf_len, "%s", inst->fifo_playback_path);
    } else if (strcmp(key, "dither_mode") == 0) {
        snprintf(buf, buf_len, "%d", inst->dither_mode);
    } else if (strcmp(key, "dither_threshold") == 0) {
        snprintf(buf, buf_len, "%d", inst->dither_threshold);
    } else {
        return -1;
    }
    return 0;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    norns_instance_t *inst = (norns_instance_t *)instance;
    if (!inst || !buf || buf_len <= 0) return -1;
    if (inst->error_msg[0] == '\0') return -1;
    snprintf(buf, buf_len, "%s", inst->error_msg);
    inst->error_msg[0] = '\0';
    return 0;
}

static uint64_t g_render_count = 0;

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    norns_instance_t *inst = (norns_instance_t *)instance;
    size_t needed, got, i;

    if (!out_interleaved_lr || frames <= 0) return;

    needed = (size_t)frames * 2;

    /* Log render_block rate and pump_pipe diagnostics periodically */
    if (inst && (++g_render_count % 44100) == 0) {
        /* Try a diagnostic read from the FIFO */
        uint8_t test_buf[16];
        ssize_t test_n = -99;
        int test_errno = 0;
        if (inst->fifo_playback_fd >= 0) {
            test_n = read(inst->fifo_playback_fd, test_buf, sizeof(test_buf));
            if (test_n < 0) test_errno = errno;
            if (test_n > 0) {
                /* Push back what we read so it's not lost */
                size_t aligned = (size_t)test_n & ~3U;
                if (aligned > 0) {
                    int16_t samp[8];
                    memcpy(samp, test_buf, aligned);
                    ring_push(inst, samp, aligned / 2);
                }
            }
        }
        char dbg[192];
        snprintf(dbg, sizeof(dbg),
                 "render_block: count=%llu ring=%zu fd=%d read=%zd errno=%d path=%s",
                 (unsigned long long)g_render_count, ring_available(inst),
                 inst->fifo_playback_fd, test_n, test_errno,
                 inst->fifo_playback_path);
        pw_log(dbg);
    }
    memset(out_interleaved_lr, 0, needed * sizeof(int16_t));

    if (!inst) return;

    check_pw_alive(inst);
    pump_midi_out(inst);

    /* Audio input: copy Move's input to SHM for the bridge to pick up.
     * Only when enabled — avoids feedback and saves CPU for scripts
     * that don't use audio input. */
    if (inst->audio_in_enabled && inst->shm_audio_in && g_host && g_host->audio_in) {
        shm_write(inst->shm_audio_in, g_host->audio_in, (uint32_t)frames);
    }

    /* SHM zero-copy path: read directly from shared memory ring.
     * No syscalls, no intermediate ring buffer, no FIFO overhead. */
    if (inst->shm_audio) {
        got = shm_read(inst->shm_audio, out_interleaved_lr, (uint32_t)frames);
        got *= 2;  /* shm_read returns frames, we need sample count */
        if (got > 0) {
            inst->last_audio_ms = now_ms();
            inst->receiving_audio = true;
        } else if (inst->receiving_audio && inst->last_audio_ms > 0) {
            uint64_t now = now_ms();
            if (now > inst->last_audio_ms && (now - inst->last_audio_ms) > AUDIO_IDLE_MS)
                inst->receiving_audio = false;
        }
    } else {
        /* FIFO fallback */
        pump_pipe(inst);
        got = ring_pop(inst, out_interleaved_lr, needed);
    }

    if (inst->gain != 1.0f && got > 0) {
        for (i = 0; i < got; i++) {
            float s = out_interleaved_lr[i] * inst->gain;
            if (s > 32767.0f) s = 32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            out_interleaved_lr[i] = (int16_t)s;
        }
    }

    /* Keepalive */
    out_interleaved_lr[needed - 1] |= 5;
}

/* ── Plugin Registration ──────────────────────────────── */

static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi          = v2_on_midi,
    .set_param        = v2_set_param,
    .get_param        = v2_get_param,
    .get_error        = v2_get_error,
    .render_block     = v2_render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    pw_log("move_plugin_init_v2 called");
    return &g_plugin_api_v2;
}
