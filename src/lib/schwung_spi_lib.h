// SPDX-License-Identifier: MIT
// Schwung SPI constants/types for standalone modules.
// Derived from charlesvestal/schwung-standalone-example.

#ifndef SCHWUNG_SPI_LIB_H
#define SCHWUNG_SPI_LIB_H

#include <stdint.h>

// SPI Protocol Constants
#define SCHWUNG_SPI_DEVICE         "/dev/ablspi0.0"
#define SCHWUNG_PAGE_SIZE          4096
#define SCHWUNG_FRAME_SIZE         768
#define SCHWUNG_SPI_FREQ           20000000
#define SCHWUNG_AUDIO_FRAMES       128
#define SCHWUNG_SAMPLE_RATE        44100

// Buffer layout -- output (host -> XMOS)
#define SCHWUNG_OFF_OUT_MIDI       0
#define SCHWUNG_OFF_OUT_DISP_STAT  80
#define SCHWUNG_OFF_OUT_DISP_DATA  84
#define SCHWUNG_OFF_OUT_AUDIO      256
#define SCHWUNG_OUT_DISP_CHUNK_LEN 172

// Buffer layout -- input (XMOS -> host)
#define SCHWUNG_OFF_IN_BASE        2048
#define SCHWUNG_OFF_IN_MIDI        (SCHWUNG_OFF_IN_BASE)
#define SCHWUNG_OFF_IN_AUDIO       (SCHWUNG_OFF_IN_BASE + 256)

// MIDI limits
#define SCHWUNG_MIDI_IN_MAX        31
#define SCHWUNG_MIDI_OUT_MAX       20

// Display
#define SCHWUNG_DISPLAY_SIZE       1024
#define SCHWUNG_DISPLAY_PHASES     7

// ioctl command numbers
enum schwung_ioctl_cmd {
    SCHWUNG_IOCTL_SET_MSG_SIZE   = 8,
    SCHWUNG_IOCTL_SET_SPEED      = 11,
    SCHWUNG_IOCTL_WAIT_SEND_SIZE = 10,
};

// MIDI types
typedef struct {
    uint8_t channel : 4;
    uint8_t type    : 4;
    uint8_t data1;
    uint8_t data2;
} SchwungMidiMsg;

typedef struct {
    uint8_t cin   : 4;
    uint8_t cable : 4;
    SchwungMidiMsg midi;
} SchwungUsbMidiMsg;

typedef struct __attribute__((packed)) {
    SchwungUsbMidiMsg message;
    uint32_t          timestamp;
} SchwungMidiEvent;

#endif // SCHWUNG_SPI_LIB_H
