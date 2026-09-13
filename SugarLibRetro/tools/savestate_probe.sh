#!/bin/bash
# Prove a savestate round trip through RetroArch, not just through a unit test.
#
#   tools/savestate_probe.sh <core.so> [content]
#
# Types a POKE to put a known byte in RAM, saves a state, overwrites that byte
# through RetroArch's own WRITE_CORE_MEMORY, then loads the state back. A state
# that restores puts the typed value back; one that does not leaves the
# overwrite in place. The clobber is checked before the load, so a pass cannot
# come from the write silently failing.
#
# Same headless harness as cpc_probe.sh -- see that file for why
# pause_nonactive must be false.
set -u

CORE=${1:?core .so}
CONTENT=${2:-}

DISPLAY_NUM=${CPC_PROBE_DISPLAY:-:79}
PORT=${CPC_PROBE_PORT:-55356}
ADDR=8000
TIMEOUT=${CPC_PROBE_TIMEOUT:-120}

cfg=$(mktemp) || exit 1
state_dir=$(mktemp -d) || exit 1
cat > "$cfg" <<EOF
# Never write any of this back: RetroArch saves its configuration on
# exit, so an --appendconfig value ends up in the user's retroarch.cfg.
config_save_on_exit = "false"
pause_nonactive = "false"
video_vsync = "false"
video_driver = "sdl2"
audio_driver = "null"
audio_enable = "false"
menu_driver = "null"
video_fullscreen = "false"
network_cmd_enable = "true"
network_cmd_port = "$PORT"
savestate_directory = "$state_dir"
EOF

xvfb_pid=""; ra_pid=""
cleanup() {
   [ -n "$ra_pid" ] && kill "$ra_pid" 2>/dev/null
   [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null
   rm -rf "$cfg" "$state_dir"
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
SUGARLIBRETRO_TYPE="POKE &8000,111\r" \
SUGARLIBRETRO_TYPE_DELAY="${SUGARLIBRETRO_TYPE_DELAY:-250}" \
SUGARLIBRETRO_TYPE_RATE="${SUGARLIBRETRO_TYPE_RATE:-6}" \
   retroarch --appendconfig "$cfg" -L "$CORE" ${CONTENT:+"$CONTENT"} >/dev/null 2>&1 &
ra_pid=$!

cmd() { echo "$1" | nc -u -w2 127.0.0.1 "$PORT"; }
value_of() { local o; o=$(cmd "READ_CORE_MEMORY $ADDR 1"); echo "${o#READ_CORE_MEMORY $ADDR }"; }

deadline=$((SECONDS + TIMEOUT)); first=""
while [ "$SECONDS" -lt "$deadline" ]; do
   sleep 2
   v=$(value_of)
   case "$v" in 6F) first="$v"; break;; esac
done
[ -n "$first" ] || { echo "savestate_probe: the POKE never landed (last: ${v:-<none>})" >&2; exit 1; }
echo "typed value   : $first"

cmd "SAVE_STATE" >/dev/null; sleep 3

cmd "WRITE_CORE_MEMORY $ADDR DE" >/dev/null; sleep 1
clobbered=$(value_of)
echo "after clobber : $clobbered"
case "$clobbered" in
   DE) ;;
   *) echo "savestate_probe: the clobber did not take, so a pass would prove nothing" >&2; exit 1;;
esac

cmd "LOAD_STATE" >/dev/null; sleep 3
after=$(value_of)
echo "after load    : $after"

if [ "$after" = "$first" ]; then
   echo "OK: the state restored the byte"
   exit 0
fi
echo "FAIL: expected $first, got $after" >&2
exit 1
