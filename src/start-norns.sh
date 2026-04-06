#!/bin/sh
# start-norns.sh — Start Norns stack inside the Debian chroot
# Called by pw-helper: start-norns.sh <fifo_playback_path> <slot>
set -e

FIFO_PLAYBACK="$1"
SLOT="${2:-1}"
CHROOT="/data/UserData/pw-chroot"
PID_DIR="/tmp/norns-pids-${SLOT}"
RUNTIME_DIR="/tmp/pw-runtime-${SLOT}"

if [ ! -d "$CHROOT/usr" ]; then
    echo "ERROR: Chroot not found at $CHROOT" >&2
    exit 1
fi

mkdir -p "$PID_DIR"
chmod 777 "$PID_DIR"

# Bind-mount system filesystems (skip if already mounted)
for fs in proc sys dev dev/pts tmp; do
    case "$fs" in
        proc)    mountpoint -q "$CHROOT/proc"    2>/dev/null || mount -t proc proc "$CHROOT/proc" ;;
        sys)     mountpoint -q "$CHROOT/sys"     2>/dev/null || mount -t sysfs sys "$CHROOT/sys" ;;
        dev)     mountpoint -q "$CHROOT/dev"     2>/dev/null || mount --bind /dev "$CHROOT/dev" ;;
        dev/pts) mountpoint -q "$CHROOT/dev/pts" 2>/dev/null || mount --bind /dev/pts "$CHROOT/dev/pts" ;;
        tmp)     mountpoint -q "$CHROOT/tmp"     2>/dev/null || mount --bind /tmp "$CHROOT/tmp" ;;
    esac
done

# Set up runtime dir
mkdir -p "$CHROOT/$RUNTIME_DIR"
chown 1000:1000 "$CHROOT/$RUNTIME_DIR"
chmod 700 "$CHROOT/$RUNTIME_DIR"

# Make FIFOs writable
chmod 666 "$FIFO_PLAYBACK" 2>/dev/null || true
for f in /tmp/midi-to-chroot-${SLOT} /tmp/midi-from-chroot-${SLOT} \
         /tmp/norns-screen-${SLOT} /tmp/norns-input-${SLOT}; do
    chmod 666 "$f" 2>/dev/null || true
done

# Helper: wait for a file/socket to exist
wait_for() {
    _wf_path="$1"
    _wf_max="$2"
    _wf_i=0
    while [ $_wf_i -lt $_wf_max ]; do
        [ -e "$_wf_path" ] && return 0
        sleep 1
        _wf_i=$((_wf_i+1))
    done
    echo "WARN: timeout waiting for $_wf_path" >&2
    return 1
}

# Launch everything in a single backgrounded subshell
(
    set +e  # Individual failures handled gracefully — don't exit subshell
    # Start dbus system bus (skip if already running)
    chrt -o 0 chroot "$CHROOT" sh -c "
        if ! pgrep -x dbus-daemon >/dev/null 2>&1; then
            mkdir -p /run/dbus
            dbus-daemon --system --fork 2>/dev/null || true
        fi
    "

    # Start jackd (from RNBO Takeover)
    JACKD="/data/UserData/rnbo/bin/jackd"
    if [ -x "$JACKD" ] && ! pgrep -x jackd >/dev/null 2>&1; then
        chrt -o 0 "$JACKD" -d move -r 44100 -p 128 &
        echo $! > "$PID_DIR/jackd.pid"
        sleep 2  # wait for JACK server to initialize
        echo "jackd started"
    elif pgrep -x jackd >/dev/null 2>&1; then
        echo "jackd already running"
    else
        echo "ERROR: jackd not found at $JACKD — install RNBO Takeover for Move" >&2
    fi

    # Ensure /dev/shm is accessible from chroot for JACK SHM transport
    if ! mountpoint -q "$CHROOT/dev/shm" 2>/dev/null; then
        mount --bind /dev/shm "$CHROOT/dev/shm" || true
    fi

    # Remove 32-bit SC plugins that crash scsynth on 64-bit Move.
    # Scripts like AmenBreak install PortedPlugins (compiled for Pi CM3).
    chroot "$CHROOT" sh -c '
        for f in $(find /home/we/.local/share/SuperCollider/Extensions -name "*.so" 2>/dev/null); do
            case "$(file -b "$f")" in *32-bit*) rm -f "$f" ;; esac
        done
    ' 2>/dev/null

    # Start crone (JACK audio routing — must start before sclang)
    if [ -x "$CHROOT/home/we/norns/build/crone/crone" ]; then
        chrt -o 0 chroot "$CHROOT" su - move -c "
            export XDG_RUNTIME_DIR=$RUNTIME_DIR
            cd /home/we/norns
            nohup ./build/crone/crone >/dev/null 2>&1 &
            echo \$! > /tmp/norns-pids-${SLOT}/crone.pid
        "
        sleep 2
    fi

    # Start matron BEFORE sclang — matron must be listening on port 8888
    # when sclang initializes, because Crone.sc sends engine registration
    # OSC messages (/engine/register) to matron during startup. If matron
    # isn't running yet, those UDP packets are lost and matron ends up with
    # an empty engine list → "error: missing ENGINE_NAME" on every script.
    if [ -x "$CHROOT/home/we/norns/build/matron/matron" ]; then
        chrt -o 0 chroot "$CHROOT" su - move -c "
            export XDG_RUNTIME_DIR=$RUNTIME_DIR
            export NORNS_SCREEN_FIFO=/tmp/norns-screen-${SLOT}
            export NORNS_INPUT_FIFO=/tmp/norns-input-${SLOT}
            cd /home/we/norns
            nohup ./build/ws-wrapper/ws-wrapper ws://*:5555 ./build/matron/matron >/dev/null 2>&1 &
            echo \$! > /tmp/norns-pids-${SLOT}/matron.pid
        "
        sleep 1
    fi

    # Start sclang via ws-wrapper (boots scsynth, loads Crone.sc, exposes SC REPL on port 5556)
    # sclang manages scsynth's lifecycle and the norns engine system
    if chroot "$CHROOT" which sclang >/dev/null 2>&1; then
        chrt -o 0 chroot "$CHROOT" su - move -c "
            export XDG_RUNTIME_DIR=$RUNTIME_DIR
            export QT_QPA_PLATFORM=offscreen
            cd /home/we/norns
            nohup ./build/ws-wrapper/ws-wrapper ws://*:5556 sclang -l /home/we/norns/sclang_conf.yaml >/dev/null 2>&1 &
            echo \$! > /tmp/norns-pids-${SLOT}/sclang.pid
        "
        # Wait for scsynth to connect to JACK (visible as SuperCollider ports).
        # norns uses shared memory (not UDP), so we check JACK ports instead.
        _sc_wait=0
        while [ $_sc_wait -lt 45 ]; do
            if chroot "$CHROOT" su - move -c "
                export XDG_RUNTIME_DIR=$RUNTIME_DIR
                jack_lsp 2>/dev/null | grep -q SuperCollider" 2>/dev/null; then
                echo "SuperCollider JACK ports found (waited ${_sc_wait}s)"
                break
            fi
            sleep 1
            _sc_wait=$((_sc_wait+1))
        done
        [ $_sc_wait -ge 45 ] && echo "WARN: SuperCollider not on JACK after 45s"
        sleep 8  # Let crone finish CroneDefs + AudioContext init
    elif chroot "$CHROOT" which scsynth >/dev/null 2>&1; then
        echo "WARN: sclang not found, starting scsynth directly (no engines)"
        chrt -o 0 chroot "$CHROOT" su - move -c "
            export XDG_RUNTIME_DIR=$RUNTIME_DIR
            nohup scsynth -u 57110 -a 128 -i 2 -o 2 -r 44100 -z 128 -Z 128 >/dev/null 2>&1 &
            echo \$! > /tmp/norns-pids-${SLOT}/scsynth.pid
        "
        sleep 3
    else
        echo "WARN: no SuperCollider found, softcut-only mode"
    fi

    # Send /crone/ready OSC to matron as a fallback.
    # Normally sclang (Crone.sc) sends this after engine registration,
    # but if UDP is unreliable in the chroot, we send it manually.
    (
        _cr_wait=0
        while [ $_cr_wait -lt 60 ]; do
            if chroot "$CHROOT" su - move -c "
                export XDG_RUNTIME_DIR=$RUNTIME_DIR
                jack_lsp 2>/dev/null | grep -q SuperCollider" 2>/dev/null; then
                sleep 8  # Let CroneDefs + AudioContext finish
                # Send /crone/ready OSC message to matron (port 8888)
                chroot "$CHROOT" python3 -c "
import socket, struct
# OSC message: /crone/ready with no args
path = b'/crone/ready' + b'\x00' * 4  # pad to 16 bytes
types = b',\x00\x00\x00'
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(path + types, ('127.0.0.1', 8888))
s.close()
" 2>/dev/null
                echo "Sent /crone/ready to matron"
                break
            fi
            sleep 1
            _cr_wait=$((_cr_wait+1))
        done
    ) &

    # Start norns-input-bridge (JACK MIDI client for encoder/key translation)
    INPUT_FIFO="/tmp/norns-input-${SLOT}"
    MIDI_IN_FIFO="/tmp/midi-to-chroot-${SLOT}"
    if [ -e "$INPUT_FIFO" ]; then
        chrt -o 0 chroot "$CHROOT" su - move -c "
            nohup /usr/local/bin/norns-input-bridge $INPUT_FIFO $MIDI_IN_FIFO >/dev/null 2>&1 &
            echo \$! > /tmp/norns-pids-${SLOT}/norns-input-bridge.pid
        "
    fi

    # Start midi-bridge (if available)
    MIDI_OUT_FIFO="/tmp/midi-from-chroot-${SLOT}"
    if [ -e "$MIDI_IN_FIFO" ] && [ -e "$MIDI_OUT_FIFO" ] && [ -x "$CHROOT/usr/local/bin/midi-bridge" ]; then
        chrt -o 0 chroot "$CHROOT" su - move -c "
            export XDG_RUNTIME_DIR=$RUNTIME_DIR
            nohup /usr/local/bin/midi-bridge $MIDI_IN_FIFO $MIDI_OUT_FIFO >/dev/null 2>&1 &
            echo \$! > /tmp/norns-pids-${SLOT}/midi-bridge.pid
        "
    fi

    # Start Maiden
    if [ -x "$CHROOT/home/we/maiden/maiden" ]; then
        chrt -o 0 chroot "$CHROOT" su - move -c "
            cd /home/we/maiden
            nohup ./maiden server --port 5000 --data /home/we/dust --app /home/we/maiden/app/build --doc /home/we/norns/doc >/dev/null 2>&1 &
            echo \$! > /tmp/norns-pids-${SLOT}/maiden.pid
        "
    fi

    # Renice audio-critical processes to highest non-RT priority.
    # (SCHED_RR/FIFO gets processes killed by Move's watchdog.)
    for proc in jackd crone matron sclang scsynth; do
        for pid in $(chroot "$CHROOT" pgrep -x "$proc" 2>/dev/null); do
            renice -n -20 -p "$pid" >/dev/null 2>&1
        done
    done

    echo "Norns started in chroot (slot $SLOT)"
) &

echo "Norns launch backgrounded (slot $SLOT)"
