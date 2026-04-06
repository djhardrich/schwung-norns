/*
 * standalone_norns.c — Standalone Schwung module for Monome Norns
 *
 * Runs with exclusive hardware access (Move firmware killed by Schwung).
 * Opens SPI directly, starts norns chroot with RT JACK, bridges:
 *   - SPI audio <-> JACK (crone)
 *   - SPI MIDI  <-> norns input/output FIFOs
 *   - norns screen FIFO -> SPI display
 *
 * Exit: Back button (CC 51) or SIGTERM.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <stdbool.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#include "lib/schwung_spi_lib.h"
#include "shm_audio.h"

/* ── Display SHM (for remote viewer, matches schwung host API) ── */

#define NORNS_DISPLAY_SHM_NAME "/schwung-norns-display-live"
#define NORNS_DISPLAY_SHM_BYTES 4096
#define NORNS_DISPLAY_MAGIC "NR4SHM1"
#define NORNS_DISPLAY_FORMAT "gray4"

typedef struct __attribute__((packed)) {
    char magic[8];
    char format[16];
    uint64_t last_update_ms;
    uint32_t version;
    uint32_t header_size;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_frame;
    uint32_t frame_counter;
    uint8_t active;
    uint8_t reserved[7];
    uint8_t frame[NORNS_DISPLAY_SHM_BYTES];
} norns_display_shm_t;

/* ── Constants ── */

#define CHROOT       "/data/UserData/pw-chroot"
#define MODULE_DIR   "/data/UserData/schwung/modules/tools/norns"
#define RNBO_JACK    "/opt/rnbo-jack"
#define SLOT         1

#define MIDI_CC      0xB0
#define MIDI_NOTE_ON 0x90
#define CC_BACK      51

/* ── Globals ── */

static volatile sig_atomic_t g_running = 1;
static int g_spi_fd = -1;
static uint8_t *g_spi_buf = NULL;

/* JACK client */
static jack_client_t *g_jack_client = NULL;
static jack_port_t *g_jack_out_L = NULL;   /* from crone:output_1 */
static jack_port_t *g_jack_out_R = NULL;   /* from crone:output_2 */
static jack_port_t *g_jack_in_L = NULL;    /* to crone:input_1 */
static jack_port_t *g_jack_in_R = NULL;    /* to crone:input_2 */
static jack_port_t *g_jack_midi_in = NULL;
static jack_port_t *g_jack_midi_out = NULL;

/* Lock-free audio rings (JACK callback <-> SPI main loop) */
static shm_audio_t *g_ring_out = NULL;     /* crone -> SPI */
static shm_audio_t *g_ring_in = NULL;      /* SPI -> crone */

/* Lock-free MIDI rings */
static uint8_t g_midi_in_ring[4096];       /* JACK -> main loop */
static volatile uint32_t g_midi_in_wr = 0;
static volatile uint32_t g_midi_in_rd = 0;
static uint8_t g_midi_out_ring[4096];      /* main loop -> JACK */
static volatile uint32_t g_midi_out_wr = 0;
static volatile uint32_t g_midi_out_rd = 0;

/* Norns FIFOs */
static int g_fifo_midi_to_chroot = -1;     /* /tmp/midi-to-chroot-SLOT */
static int g_fifo_midi_from_chroot = -1;   /* /tmp/midi-from-chroot-SLOT */
static int g_fifo_screen = -1;             /* /tmp/norns-screen-SLOT */
static int g_fifo_input = -1;              /* /tmp/norns-input-SLOT */

/* Screen state */
static uint8_t g_screen_4bit[4096];        /* 4-bit packed from matron */
static uint8_t g_screen_1bit[1024];        /* 1-bit for SPI display */
static int g_screen_valid = 0;
static int g_disp_phase = 0;               /* 0-6 for 7-phase push */

/* Display SHM (remote viewer) */
static int g_display_shm_fd = -1;
static norns_display_shm_t *g_display_shm = NULL;

/* JACK connection state */
static bool g_jack_ports_connected = false;
static uint64_t g_jack_retry_ms = 0;

/* ── Helpers ── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void sighandler(int sig) {
    (void)sig;
    g_running = 0;
}

static void log_msg(const char *msg) {
    fprintf(stderr, "[norns-standalone] %s\n", msg);
}

/* ── Display SHM (remote viewer) ── */

static void open_display_shm(void) {
    int fd = shm_open(NORNS_DISPLAY_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return;
    if (ftruncate(fd, sizeof(norns_display_shm_t)) != 0) { close(fd); return; }
    void *p = mmap(NULL, sizeof(norns_display_shm_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return; }
    g_display_shm_fd = fd;
    g_display_shm = (norns_display_shm_t *)p;
    memset(g_display_shm, 0, sizeof(*g_display_shm));
    memcpy(g_display_shm->magic, NORNS_DISPLAY_MAGIC, sizeof(g_display_shm->magic));
    snprintf(g_display_shm->format, sizeof(g_display_shm->format), "%s", NORNS_DISPLAY_FORMAT);
    g_display_shm->version = 1;
    g_display_shm->header_size = (uint32_t)(sizeof(norns_display_shm_t) - NORNS_DISPLAY_SHM_BYTES);
    g_display_shm->width = 128;
    g_display_shm->height = 64;
    g_display_shm->bytes_per_frame = NORNS_DISPLAY_SHM_BYTES;
    g_display_shm->active = 0;
    log_msg("display SHM opened");
}

static void close_display_shm(void) {
    if (g_display_shm) {
        g_display_shm->active = 0;
        g_display_shm->last_update_ms = now_ms();
        munmap(g_display_shm, sizeof(norns_display_shm_t));
        g_display_shm = NULL;
    }
    if (g_display_shm_fd >= 0) {
        close(g_display_shm_fd);
        shm_unlink(NORNS_DISPLAY_SHM_NAME);
        g_display_shm_fd = -1;
    }
}

/* ── Audio Ring Buffers ── */

static shm_audio_t *alloc_ring(void) {
    void *p = calloc(1, SHM_AUDIO_FILE_SIZE);
    if (!p) return NULL;
    shm_audio_t *r = (shm_audio_t *)p;
    shm_audio_init(r, SCHWUNG_SAMPLE_RATE);
    return r;
}

/* ── JACK Callback (RT thread) ── */

static int jack_process_cb(jack_nframes_t nframes, void *arg) {
    (void)arg;

    /* Audio: crone output -> ring_out (for SPI) */
    if (g_jack_out_L && g_jack_out_R && g_ring_out) {
        jack_default_audio_sample_t *out_L =
            (jack_default_audio_sample_t *)jack_port_get_buffer(g_jack_out_L, nframes);
        jack_default_audio_sample_t *out_R =
            (jack_default_audio_sample_t *)jack_port_get_buffer(g_jack_out_R, nframes);
        if (out_L && out_R) {
            int16_t buf[256];  /* 128 stereo frames max */
            uint32_t n = (nframes > 128) ? 128 : nframes;
            for (uint32_t i = 0; i < n; i++) {
                float l = out_L[i], r = out_R[i];
                if (l > 1.0f) l = 1.0f; if (l < -1.0f) l = -1.0f;
                if (r > 1.0f) r = 1.0f; if (r < -1.0f) r = -1.0f;
                buf[i * 2]     = (int16_t)(l * 32767.0f);
                buf[i * 2 + 1] = (int16_t)(r * 32767.0f);
            }
            shm_write(g_ring_out, buf, n);
        }
    }

    /* Audio: ring_in (from SPI) -> crone input */
    if (g_jack_in_L && g_jack_in_R && g_ring_in) {
        jack_default_audio_sample_t *in_L =
            (jack_default_audio_sample_t *)jack_port_get_buffer(g_jack_in_L, nframes);
        jack_default_audio_sample_t *in_R =
            (jack_default_audio_sample_t *)jack_port_get_buffer(g_jack_in_R, nframes);
        if (in_L && in_R) {
            int16_t buf[256];
            uint32_t n = (nframes > 128) ? 128 : nframes;
            uint32_t got = shm_read(g_ring_in, buf, n);
            for (uint32_t i = 0; i < got; i++) {
                in_L[i] = (float)buf[i * 2]     / 32768.0f;
                in_R[i] = (float)buf[i * 2 + 1] / 32768.0f;
            }
            /* Zero-fill remainder */
            for (uint32_t i = got; i < nframes; i++) {
                in_L[i] = 0.0f;
                in_R[i] = 0.0f;
            }
        }
    }

    /* MIDI in: JACK -> ring (drained by main loop -> norns FIFO) */
    if (g_jack_midi_in) {
        void *midi_buf = jack_port_get_buffer(g_jack_midi_in, nframes);
        uint32_t count = jack_midi_get_event_count(midi_buf);
        for (uint32_t i = 0; i < count; i++) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, midi_buf, i) != 0) continue;
            if (ev.size == 0 || ev.size > 3) continue;
            uint32_t wr = __atomic_load_n(&g_midi_in_wr, __ATOMIC_RELAXED);
            uint32_t rd = __atomic_load_n(&g_midi_in_rd, __ATOMIC_ACQUIRE);
            uint32_t space = sizeof(g_midi_in_ring) - ((wr - rd) & (sizeof(g_midi_in_ring) - 1));
            if (space < ev.size + 2) continue;
            uint32_t mask = sizeof(g_midi_in_ring) - 1;
            g_midi_in_ring[wr & mask] = (uint8_t)(ev.size & 0xFF);
            g_midi_in_ring[(wr + 1) & mask] = (uint8_t)((ev.size >> 8) & 0xFF);
            for (uint32_t j = 0; j < ev.size; j++)
                g_midi_in_ring[(wr + 2 + j) & mask] = ev.buffer[j];
            __atomic_store_n(&g_midi_in_wr, wr + 2 + ev.size, __ATOMIC_RELEASE);
        }
    }

    /* MIDI out: ring (filled by main loop from norns FIFO) -> JACK */
    if (g_jack_midi_out) {
        void *midi_buf = jack_port_get_buffer(g_jack_midi_out, nframes);
        jack_midi_clear_buffer(midi_buf);
        uint32_t wr = __atomic_load_n(&g_midi_out_wr, __ATOMIC_ACQUIRE);
        uint32_t rd = __atomic_load_n(&g_midi_out_rd, __ATOMIC_RELAXED);
        uint32_t mask = sizeof(g_midi_out_ring) - 1;
        while (rd != wr) {
            uint16_t len = g_midi_out_ring[rd & mask]
                         | ((uint16_t)g_midi_out_ring[(rd + 1) & mask] << 8);
            if (len == 0 || len > 3) break;
            if ((wr - rd) < len + 2u) break;
            uint8_t msg[3];
            for (uint16_t j = 0; j < len; j++)
                msg[j] = g_midi_out_ring[(rd + 2 + j) & mask];
            jack_midi_event_write(midi_buf, 0, msg, len);
            rd += 2 + len;
        }
        __atomic_store_n(&g_midi_out_rd, rd, __ATOMIC_RELEASE);
    }

    return 0;
}

/* ── JACK Client Setup ── */

static bool jack_connect_ports(void) {
    if (!g_jack_client) return false;
    int ok = 0;
    ok += (jack_connect(g_jack_client, "crone:output_1",
                        jack_port_name(g_jack_out_L)) == 0) ? 1 : 0;
    ok += (jack_connect(g_jack_client, "crone:output_2",
                        jack_port_name(g_jack_out_R)) == 0) ? 1 : 0;
    ok += (jack_connect(g_jack_client, jack_port_name(g_jack_in_L),
                        "crone:input_1") == 0) ? 1 : 0;
    ok += (jack_connect(g_jack_client, jack_port_name(g_jack_in_R),
                        "crone:input_2") == 0) ? 1 : 0;
    jack_connect(g_jack_client, "system:midi_capture_1",
                 jack_port_name(g_jack_midi_in));
    jack_connect(g_jack_client, jack_port_name(g_jack_midi_out),
                 "system:midi_playback_1");
    if (ok >= 4) {
        g_jack_ports_connected = true;
        log_msg("JACK ports connected to crone");
    }
    return g_jack_ports_connected;
}

static int jack_client_setup(void) {
    jack_status_t status;
    g_jack_client = jack_client_open("norns-standalone", JackNoStartServer, &status);
    if (!g_jack_client) return -1;

    g_jack_out_L = jack_port_register(g_jack_client, "audio_from_crone_L",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    g_jack_out_R = jack_port_register(g_jack_client, "audio_from_crone_R",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    g_jack_in_L = jack_port_register(g_jack_client, "audio_to_crone_L",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_jack_in_R = jack_port_register(g_jack_client, "audio_to_crone_R",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_jack_midi_in = jack_port_register(g_jack_client, "midi_in",
        JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    g_jack_midi_out = jack_port_register(g_jack_client, "midi_out",
        JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    jack_set_process_callback(g_jack_client, jack_process_cb, NULL);

    if (jack_activate(g_jack_client) != 0) {
        jack_client_close(g_jack_client);
        g_jack_client = NULL;
        return -1;
    }

    log_msg("JACK client activated");
    jack_connect_ports();
    return 0;
}

/* ── SPI Setup ── */

static int spi_open(void) {
    /* SPI device may still be held briefly after launch-standalone.sh kills Move.
     * Retry for up to 5 seconds. */
    for (int attempt = 0; attempt < 50; attempt++) {
        g_spi_fd = open(SCHWUNG_SPI_DEVICE, O_RDWR);
        if (g_spi_fd >= 0) break;
        if (errno != EBUSY) {
            perror("open SPI device");
            return -1;
        }
        usleep(100000); /* 100ms */
    }
    if (g_spi_fd < 0) {
        log_msg("FATAL: SPI device still busy after 5s");
        return -1;
    }
    g_spi_buf = mmap(NULL, SCHWUNG_PAGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, g_spi_fd, 0);
    if (g_spi_buf == MAP_FAILED) {
        perror("mmap SPI buffer");
        close(g_spi_fd);
        g_spi_fd = -1;
        return -1;
    }
    ioctl(g_spi_fd, SCHWUNG_IOCTL_SET_MSG_SIZE, SCHWUNG_FRAME_SIZE);
    ioctl(g_spi_fd, SCHWUNG_IOCTL_SET_SPEED, SCHWUNG_SPI_FREQ);
    log_msg("SPI device opened");
    return 0;
}

static void spi_close(void) {
    if (g_spi_buf && g_spi_buf != MAP_FAILED) {
        /* Clear audio and LEDs before exit */
        memset(g_spi_buf, 0, SCHWUNG_OFF_IN_BASE);
        ioctl(g_spi_fd, SCHWUNG_IOCTL_WAIT_SEND_SIZE, SCHWUNG_FRAME_SIZE);
    }
    if (g_spi_buf && g_spi_buf != MAP_FAILED)
        munmap(g_spi_buf, SCHWUNG_PAGE_SIZE);
    if (g_spi_fd >= 0)
        close(g_spi_fd);
    g_spi_buf = NULL;
    g_spi_fd = -1;
}

/* ── Chroot Lifecycle ── */

static void start_chroot(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open("/tmp/pw-start.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execl("/bin/sh", "sh", MODULE_DIR "/start-norns.sh",
              "/tmp/pw-to-move-1", "1", (char *)NULL);
        _exit(127);
    }
    log_msg("norns chroot launch requested");
}

static void stop_chroot(void) {
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execl("/bin/sh", "sh", MODULE_DIR "/stop-norns.sh", "1", (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    log_msg("norns chroot stopped");
}

/* ── FIFO Setup ── */

static void open_fifos(void) {
    char path[64];

    snprintf(path, sizeof(path), "/tmp/midi-to-chroot-%d", SLOT);
    mkfifo(path, 0666); chmod(path, 0666);
    g_fifo_midi_to_chroot = open(path, O_RDWR | O_NONBLOCK);

    snprintf(path, sizeof(path), "/tmp/midi-from-chroot-%d", SLOT);
    mkfifo(path, 0666); chmod(path, 0666);
    g_fifo_midi_from_chroot = open(path, O_RDWR | O_NONBLOCK);

    snprintf(path, sizeof(path), "/tmp/norns-screen-%d", SLOT);
    mkfifo(path, 0666); chmod(path, 0666);
    g_fifo_screen = open(path, O_RDWR | O_NONBLOCK);

    snprintf(path, sizeof(path), "/tmp/norns-input-%d", SLOT);
    mkfifo(path, 0666); chmod(path, 0666);
    g_fifo_input = open(path, O_RDWR | O_NONBLOCK);

    /* Grid LED FIFO */
    snprintf(path, sizeof(path), "/tmp/norns-grid-%d", SLOT);
    mkfifo(path, 0666); chmod(path, 0666);

    log_msg("FIFOs created");
}

static void close_fifos(void) {
    if (g_fifo_midi_to_chroot >= 0) close(g_fifo_midi_to_chroot);
    if (g_fifo_midi_from_chroot >= 0) close(g_fifo_midi_from_chroot);
    if (g_fifo_screen >= 0) close(g_fifo_screen);
    if (g_fifo_input >= 0) close(g_fifo_input);
}

/* ── SPI MIDI Processing ── */

static void process_spi_midi_in(void) {
    /* Read MIDI events from SPI input region, forward to norns input FIFO */
    SchwungMidiEvent *events = (SchwungMidiEvent *)(g_spi_buf + SCHWUNG_OFF_IN_MIDI);

    for (int i = 0; i < SCHWUNG_MIDI_IN_MAX; i++) {
        SchwungMidiMsg msg = events[i].message.midi;
        if (msg.type == 0 && msg.data1 == 0 && msg.data2 == 0) continue;

        uint8_t status_byte = (msg.type << 4) | msg.channel;

        /* Check for Back button (CC 51 value 127) -> exit */
        if (status_byte == MIDI_CC && msg.data1 == CC_BACK && msg.data2 == 127) {
            g_running = 0;
            return;
        }

        /* Forward to norns input FIFO as length-prefixed MIDI */
        if (g_fifo_midi_to_chroot >= 0) {
            uint8_t frame[5];
            int len = 3;
            /* Program change and channel pressure are 2 bytes */
            if ((status_byte & 0xF0) == 0xC0 || (status_byte & 0xF0) == 0xD0)
                len = 2;
            uint16_t ulen = (uint16_t)len;
            frame[0] = (uint8_t)(ulen & 0xFF);
            frame[1] = (uint8_t)((ulen >> 8) & 0xFF);
            frame[2] = status_byte;
            frame[3] = msg.data1;
            frame[4] = msg.data2;
            write(g_fifo_midi_to_chroot, frame, 2 + len);
        }
    }
}

static void process_midi_out_to_spi(void) {
    /* Read from norns MIDI output FIFO, write to SPI output MIDI region */
    if (g_fifo_midi_from_chroot < 0) return;

    SchwungUsbMidiMsg *out_slots = (SchwungUsbMidiMsg *)(g_spi_buf + SCHWUNG_OFF_OUT_MIDI);
    int slot_idx = 0;

    uint8_t hdr[2];
    while (slot_idx < SCHWUNG_MIDI_OUT_MAX) {
        ssize_t n = read(g_fifo_midi_from_chroot, hdr, 2);
        if (n != 2) break;
        uint16_t len = hdr[0] | ((uint16_t)hdr[1] << 8);
        if (len == 0 || len > 3) break;
        uint8_t msg[3] = {0, 0, 0};
        n = read(g_fifo_midi_from_chroot, msg, len);
        if (n != (ssize_t)len) break;

        SchwungMidiMsg midi_msg;
        midi_msg.type = (msg[0] >> 4) & 0x0F;
        midi_msg.channel = msg[0] & 0x0F;
        midi_msg.data1 = (len > 1) ? msg[1] : 0;
        midi_msg.data2 = (len > 2) ? msg[2] : 0;

        /* CIN = message type for USB MIDI */
        out_slots[slot_idx].cin = midi_msg.type;
        out_slots[slot_idx].cable = 0;
        out_slots[slot_idx].midi = midi_msg;
        slot_idx++;
    }
}

/* Drain JACK MIDI ring -> norns input FIFO */
static void drain_midi_in_ring(void) {
    if (g_fifo_midi_to_chroot < 0) return;
    uint32_t wr = __atomic_load_n(&g_midi_in_wr, __ATOMIC_ACQUIRE);
    uint32_t rd = __atomic_load_n(&g_midi_in_rd, __ATOMIC_RELAXED);
    uint32_t mask = sizeof(g_midi_in_ring) - 1;

    while (rd != wr) {
        uint16_t len = g_midi_in_ring[rd & mask]
                     | ((uint16_t)g_midi_in_ring[(rd + 1) & mask] << 8);
        if (len == 0 || len > 3) break;
        if ((wr - rd) < len + 2u) break;
        uint8_t frame[5];
        frame[0] = (uint8_t)(len & 0xFF);
        frame[1] = (uint8_t)((len >> 8) & 0xFF);
        for (uint16_t j = 0; j < len; j++)
            frame[2 + j] = g_midi_in_ring[(rd + 2 + j) & mask];
        write(g_fifo_midi_to_chroot, frame, 2 + len);
        rd += 2 + len;
    }
    __atomic_store_n(&g_midi_in_rd, rd, __ATOMIC_RELEASE);
}

/* Pump norns MIDI output FIFO -> JACK ring */
static void pump_midi_out_to_ring(void) {
    if (g_fifo_midi_from_chroot < 0) return;
    uint8_t hdr[2];
    for (int i = 0; i < 32; i++) {
        ssize_t n = read(g_fifo_midi_from_chroot, hdr, 2);
        if (n != 2) break;
        uint16_t len = hdr[0] | ((uint16_t)hdr[1] << 8);
        if (len == 0 || len > 3) break;
        uint8_t msg[3] = {0};
        n = read(g_fifo_midi_from_chroot, msg, len);
        if (n != (ssize_t)len) break;

        uint32_t wr = __atomic_load_n(&g_midi_out_wr, __ATOMIC_RELAXED);
        uint32_t rd = __atomic_load_n(&g_midi_out_rd, __ATOMIC_ACQUIRE);
        uint32_t mask = sizeof(g_midi_out_ring) - 1;
        uint32_t space = sizeof(g_midi_out_ring) - ((wr - rd) & mask);
        if (space < len + 2u) break;
        g_midi_out_ring[wr & mask] = (uint8_t)(len & 0xFF);
        g_midi_out_ring[(wr + 1) & mask] = (uint8_t)((len >> 8) & 0xFF);
        for (uint16_t j = 0; j < len; j++)
            g_midi_out_ring[(wr + 2 + j) & mask] = msg[j];
        __atomic_store_n(&g_midi_out_wr, wr + 2 + len, __ATOMIC_RELEASE);
    }
}

/* ── Screen -> SPI Display ── */

static void pump_screen(void) {
    if (g_fifo_screen < 0) return;

    /* Drain all available frames, keep latest */
    uint8_t tmp[4096];
    int got_frame = 0;
    for (;;) {
        ssize_t n = read(g_fifo_screen, tmp, 4096);
        if (n == 4096) {
            memcpy(g_screen_4bit, tmp, 4096);
            got_frame = 1;
        } else {
            break;
        }
    }

    if (!got_frame) return;
    g_screen_valid = 1;

    /* Update display SHM for remote viewer */
    if (g_display_shm) {
        memcpy(g_display_shm->frame, g_screen_4bit, NORNS_DISPLAY_SHM_BYTES);
        __sync_synchronize();
        g_display_shm->frame_counter++;
        __sync_synchronize();
        g_display_shm->last_update_ms = now_ms();
        g_display_shm->active = 1;
    }

    /* Convert 4-bit grayscale -> 1-bit monochrome for SPI display */
    memset(g_screen_1bit, 0, 1024);
    for (int i = 0; i < 4096; i++) {
        uint8_t hi = (g_screen_4bit[i] >> 4) & 0x0F;
        uint8_t lo = g_screen_4bit[i] & 0x0F;
        int px = i * 2;
        /* Pack into 1-bit: MSB = leftmost pixel in byte */
        int byte_idx = px / 8;
        int bit_idx = 7 - (px % 8);
        if (hi > 3) g_screen_1bit[byte_idx] |= (1 << bit_idx);
        if (lo > 3) g_screen_1bit[byte_idx] |= (1 << (bit_idx - 1));
    }
}

static void push_display_slice(void) {
    if (!g_screen_valid) return;

    /* Write display phase and data to SPI output buffer */
    *(uint32_t *)(g_spi_buf + SCHWUNG_OFF_OUT_DISP_STAT) = g_disp_phase;

    if (g_disp_phase > 0) {
        /* Phases 1-6: push pixel data chunks */
        int chunk_idx = g_disp_phase - 1;
        int offset = chunk_idx * SCHWUNG_OUT_DISP_CHUNK_LEN;
        int remaining = SCHWUNG_DISPLAY_SIZE - offset;
        int chunk_len = (remaining < SCHWUNG_OUT_DISP_CHUNK_LEN)
                      ? remaining : SCHWUNG_OUT_DISP_CHUNK_LEN;
        if (chunk_len > 0)
            memcpy(g_spi_buf + SCHWUNG_OFF_OUT_DISP_DATA,
                   g_screen_1bit + offset, chunk_len);
    }

    g_disp_phase = (g_disp_phase + 1) % SCHWUNG_DISPLAY_PHASES;
}

/* ── Audio: SPI <-> JACK ring ── */

static void process_audio(void) {
    int16_t *spi_out = (int16_t *)(g_spi_buf + SCHWUNG_OFF_OUT_AUDIO);
    int16_t *spi_in = (int16_t *)(g_spi_buf + SCHWUNG_OFF_IN_AUDIO);

    /* Read from ring_out (crone output via JACK) -> SPI audio out */
    if (g_ring_out) {
        int16_t buf[256];  /* 128 stereo frames */
        uint32_t got = shm_read(g_ring_out, buf, SCHWUNG_AUDIO_FRAMES);
        if (got > 0) {
            memcpy(spi_out, buf, got * 2 * sizeof(int16_t));
            /* Zero-fill remainder */
            if (got < SCHWUNG_AUDIO_FRAMES)
                memset(spi_out + got * 2, 0,
                       (SCHWUNG_AUDIO_FRAMES - got) * 2 * sizeof(int16_t));
        } else {
            memset(spi_out, 0, SCHWUNG_AUDIO_FRAMES * 2 * sizeof(int16_t));
        }
    }

    /* SPI audio in -> ring_in (for JACK callback to feed to crone) */
    if (g_ring_in) {
        shm_write(g_ring_in, spi_in, SCHWUNG_AUDIO_FRAMES);
    }
}

/* ── Main ── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    log_msg("starting");

    /* Open SPI */
    if (spi_open() != 0) {
        log_msg("FATAL: cannot open SPI");
        return 1;
    }

    /* Allocate audio rings */
    g_ring_out = alloc_ring();
    g_ring_in = alloc_ring();
    if (!g_ring_out || !g_ring_in) {
        log_msg("FATAL: cannot allocate audio rings");
        spi_close();
        return 1;
    }

    /* Open FIFOs and display SHM */
    open_fifos();
    open_display_shm();

    /* Start norns chroot (JACK + crone + matron + sclang + maiden) */
    start_chroot();

    /* Wait for JACK server, then connect as client */
    log_msg("waiting for JACK server...");
    for (int i = 0; i < 60 && g_running; i++) {
        if (jack_client_setup() == 0) break;
        sleep(1);
    }
    if (!g_jack_client) {
        log_msg("WARN: JACK client not connected, continuing without audio bridge");
    }

    log_msg("entering main loop");

    /* ── SPI main loop ── */
    while (g_running) {
        /* Clear output region */
        memset(g_spi_buf, 0, SCHWUNG_OFF_IN_BASE);

        /* Retry JACK port connections if needed */
        if (g_jack_client && !g_jack_ports_connected) {
            uint64_t now = now_ms();
            if (now - g_jack_retry_ms > 1000) {
                g_jack_retry_ms = now;
                jack_connect_ports();
            }
        }

        /* Process audio */
        process_audio();

        /* Process MIDI: SPI -> norns */
        process_spi_midi_in();

        /* Drain JACK MIDI ring -> norns FIFO */
        drain_midi_in_ring();

        /* Pump norns MIDI out FIFO -> JACK ring */
        pump_midi_out_to_ring();

        /* Process MIDI: norns -> SPI */
        process_midi_out_to_spi();

        /* Process display */
        pump_screen();
        push_display_slice();

        /* Blocking SPI transfer */
        int ret;
        do {
            ret = ioctl(g_spi_fd, SCHWUNG_IOCTL_WAIT_SEND_SIZE, SCHWUNG_FRAME_SIZE);
        } while (ret < 0 && errno == EINTR && g_running);
    }

    log_msg("shutting down");

    /* Clean up */
    if (g_jack_client) {
        jack_deactivate(g_jack_client);
        jack_client_close(g_jack_client);
    }

    /* Clear SPI (silence + blank display) before handing back */
    if (g_spi_buf) {
        memset(g_spi_buf, 0, SCHWUNG_OFF_IN_BASE);
        ioctl(g_spi_fd, SCHWUNG_IOCTL_WAIT_SEND_SIZE, SCHWUNG_FRAME_SIZE);
    }

    stop_chroot();
    close_display_shm();
    close_fifos();
    spi_close();
    free(g_ring_out);
    free(g_ring_in);

    log_msg("exit");
    return 0;
}
