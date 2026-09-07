#!/bin/bash
# Type a BASIC line into the emulated CPC and read the result back out of RAM.
#
# The core's SUGARLIBRETRO_TYPE hook drives the autorun typist; RetroArch's
# READ_CORE_MEMORY network command reads the RAM the core exports as
# RETRO_MEMORY_SYSTEM_RAM. Together they report machine state as a number, so
# a change can be verified without anyone looking at the screen.
#
#   tools/cpc_probe.sh <core.so> <content> <basic> <addr> [len]
#
#   tools/cpc_probe.sh build/libSugarLibRetro.so disc.dsk \
#       'X=HIMEM:POKE &8000,X-256*INT(X/256):POKE &8001,INT(X/256)' 8000 2
#   -> 7B A6   (HIMEM = &A67B, i.e. AMSDOS is resident in ROM slot 7)
#
# Runs headless on its own Xvfb, and forces pause_nonactive=false: RetroArch
# defaults it to true, so an unfocused window freezes the emulator mid-typing
# and every read returns the pre-typing value. That confound cost real time --
# do not drop the config.
#
# BASIC is uppercased by the core, so lowercase input is fine. Everything the
# typist can reach is in kAutorunKeys (letters, digits, and the punctuation
# BASIC and AMSDOS need).
set -u

CORE=${1:?core .so}
CONTENT=${2:?content path}
BASIC=${3:?BASIC line}
ADDR=${4:?hex address}
LEN=${5:-1}

DISPLAY_NUM=${CPC_PROBE_DISPLAY:-:78}
PORT=${CPC_PROBE_PORT:-55355}
DELAY=${SUGARLIBRETRO_TYPE_DELAY:-250}
RATE=${SUGARLIBRETRO_TYPE_RATE:-6}
TIMEOUT=${CPC_PROBE_TIMEOUT:-120}

cfg=$(mktemp) || exit 1
cat > "$cfg" <<EOF
pause_nonactive = "false"
video_vsync = "false"
video_driver = "sdl2"
audio_driver = "null"
audio_enable = "false"
menu_driver = "null"
video_fullscreen = "false"
network_cmd_enable = "true"
network_cmd_port = "$PORT"
EOF

xvfb_pid=""
ra_pid=""
cleanup() {
   [ -n "$ra_pid" ] && kill "$ra_pid" 2>/dev/null
   [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null
   rm -f "$cfg"
}
trap cleanup EXIT

if ! DISPLAY="$DISPLAY_NUM" xdpyinfo >/dev/null 2>&1; then
   Xvfb "$DISPLAY_NUM" -screen 0 800x600x24 -nolisten tcp >/dev/null 2>&1 &
   xvfb_pid=$!
   for _ in $(seq 1 20); do
      DISPLAY="$DISPLAY_NUM" xdpyinfo >/dev/null 2>&1 && break
      sleep 0.5
   done
fi

DISPLAY="$DISPLAY_NUM" \
SUGARLIBRETRO_TYPE="${BASIC}\r" \
SUGARLIBRETRO_TYPE_DELAY="$DELAY" \
SUGARLIBRETRO_TYPE_RATE="$RATE" \
   retroarch --appendconfig "$cfg" -L "$CORE" "$CONTENT" >/dev/null 2>&1 &
ra_pid=$!

read_mem() { echo "READ_CORE_MEMORY $ADDR $LEN" | nc -u -w2 127.0.0.1 "$PORT"; }

# Zero is the "not written yet" sentinel: the typing has to finish before the
# POKE lands, and how long that takes depends on the line length and the rate.
zeros=$(printf '00 %.0s' $(seq 1 "$LEN")); zeros=${zeros% }
deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
   sleep 2
   out=$(read_mem)
   value=${out#READ_CORE_MEMORY $ADDR }
   if [ -n "$value" ] && [ "$value" != "$zeros" ] && [ "$out" != "$value" ]; then
      echo "$value"
      exit 0
   fi
done

echo "cpc_probe: timed out after ${TIMEOUT}s; last read: ${out:-<none>}" >&2
exit 1
