#!/bin/bash
# Run the core over a corpus of real media and report what actually happened.
#
#   tools/corpus_run.sh [-j N] [-o OUTDIR] [-n COUNT] <core.so> <corpus dir>...
#
# For every image found: load it, let the core's autorun type its RUN"/CAT,
# then check four things that a broken core cannot fake.
#
#   booted      non-zero bytes in the exported RAM. A machine that never
#               executed reads 0; a live CPC at the BASIC prompt is ~3300.
#               "The process is still alive after N seconds" proves nothing
#               and is what the older corpus script settled for.
#   typed       the core reports having typed its RUN"/CAT. Whether the program
#               then did something is what the screenshots are for: a program
#               that loads and waits at a menu stops changing RAM, so a
#               before/after comparison calls it a failure -- the first version
#               of this script did exactly that, and the screenshot showed the
#               game sitting happily on its title screen.
#   state       save a state, overwrite a byte of RAM through the frontend,
#               load the state back, and check the byte returns. The clobber is
#               verified before the load, so a pass cannot come from the write
#               having silently failed, and the emulator is paused throughout:
#               a running program writes all over the address space, and an
#               earlier version of this script reported ninety-seven failures
#               that were nothing but the game overwriting the probe byte.
#   shot        two screenshots in OUTDIR, one just after boot and one after the
#               autorun, so the difference between them shows what loading the
#               image actually did.
#
# Every image gets its own RetroArch on its own X display and its own network
# port, so runs do not interfere and -j actually parallelises.
set -u

JOBS=4
OUTDIR=""
LIMIT=0
while getopts "j:o:n:" opt; do
   case "$opt" in
      j) JOBS=$OPTARG ;;
      o) OUTDIR=$OPTARG ;;
      n) LIMIT=$OPTARG ;;
      *) echo "usage: $0 [-j N] [-o OUTDIR] [-n COUNT] <core.so> <corpus dir>..." >&2; exit 2 ;;
   esac
done
shift $((OPTIND - 1))

CORE=${1:?core .so}
shift
[ $# -ge 1 ] || { echo "$0: give at least one corpus directory" >&2; exit 2; }

[ -f "$CORE" ] || { echo "$0: core not found: $CORE" >&2; exit 1; }
OUTDIR=${OUTDIR:-$(mktemp -d -t cpc-corpus-XXXXXX)}
mkdir -p "$OUTDIR/shots" "$OUTDIR/logs"

BOOT_SECONDS=${CPC_CORPUS_BOOT_SECONDS:-10}
TYPE_SECONDS=${CPC_CORPUS_TYPE_SECONDS:-8}
PROBE=8000     # scratch byte, above the firmware's own variables

# One image, one worker. $1 file, $2 slot number.
run_one() {
   local file=$1 slot=$2
   local name display port cfg statedir log shot
   name=$(basename "$file")
   display=":$((90 + slot))"
   port=$((56000 + slot))
   cfg=$(mktemp); statedir=$(mktemp -d)
   log="$OUTDIR/logs/$name.log"
   shot="$OUTDIR/shots"

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
network_cmd_port = "$port"
savestate_directory = "$statedir"
fastforward_ratio = "0"
# RetroArch's own media player claims .wav before the core does, and the
# CPC's tapes include WAV: without this the core never sees the content
# and the run reports it as "never typed".
builtin_mediaplayer_enable = "false"
screenshot_directory = "$shot"
EOF

   if ! DISPLAY="$display" xdpyinfo >/dev/null 2>&1; then
      Xvfb "$display" -screen 0 800x600x24 -nolisten tcp >/dev/null 2>&1 &
      local xvfb=$!
      for _ in $(seq 1 20); do
         DISPLAY="$display" xdpyinfo >/dev/null 2>&1 && break
         sleep 0.5
      done
   fi

   DISPLAY="$display" retroarch --appendconfig "$cfg" -L "$CORE" "$file" >"$log" 2>&1 &
   local ra=$!

   cmd() { echo "$1" | nc -u -w2 127.0.0.1 "$port" 2>/dev/null; }
   ram_used() {
      # 256 bytes from the BASIC work area: all-zero means nothing ran.
      local o; o=$(cmd "READ_CORE_MEMORY 0100 64")
      echo "${o#READ_CORE_MEMORY 0100 }" | tr -d ' ' | tr -s '0' '0' \
         | grep -qvE '^0*$' && echo live || echo dead
   }
   probe_byte() { local o; o=$(cmd "READ_CORE_MEMORY $PROBE 1"); echo "${o#READ_CORE_MEMORY $PROBE }"; }

   # Pause, and ask the frontend whether it worked rather than inferring it.
   # The commands go over UDP, which drops packets, and under -j the toggle
   # can take longer to land than any fixed sleep. Reading RAM twice and
   # calling it frozen does not work either: a loaded game leaves the
   # firmware work area alone, so two equal reads prove nothing and the
   # program goes on overwriting the probe byte -- forty six "savestate
   # failures" in the run before this. GET_STATUS reports the real state.
   paused() { case "$(cmd 'GET_STATUS')" in *PAUSED*) return 0 ;; *) return 1 ;; esac; }
   pause_until_paused() {
      local i
      for i in 1 2 3 4 5 6; do
         paused && return 0
         cmd "PAUSE_TOGGLE" >/dev/null
         sleep 1
      done
      paused
   }
   unpause() {
      local i
      for i in 1 2 3; do
         paused || return 0
         cmd "PAUSE_TOGGLE" >/dev/null
         sleep 1
      done
   }

   local booted=FAIL typed=FAIL state=FAIL shotres=FAIL

   # 1. Did the machine execute at all?
   local deadline=$((SECONDS + BOOT_SECONDS))
   while [ "$SECONDS" -lt "$deadline" ]; do
      sleep 1
      [ "$(ram_used)" = live ] && { booted=ok; break; }
   done

   if [ "$booted" = ok ]; then
      # 2. A picture of the machine before the autorun has had its effect...
      local shots_before
      shots_before=$(ls "$shot" 2>/dev/null | wc -l)
      cmd "SCREENSHOT" >/dev/null; sleep 2
      # Run the load at full speed. A tape game is three to five minutes of
      # real cassette; at 1x every tape screenshot is a "Loading" banner.
      # fastforward_ratio = 0 above makes this as fast as the host allows.
      cmd "FAST_FORWARD" >/dev/null
      sleep "$TYPE_SECONDS"
      cmd "FAST_FORWARD" >/dev/null; sleep 2
      # ...and one after, so the pair shows what loading the image did.
      cmd "SCREENSHOT" >/dev/null; sleep 2
      # Count what appeared rather than matching a filename: corpus names are
      # full of spaces, brackets and parentheses, and grepping them as a
      # pattern reported half the run as missing screenshots that were there.
      [ "$(ls "$shot" 2>/dev/null | wc -l)" -ge "$((shots_before + 2))" ] && shotres=ok

      # Did the core type anything? Machine-checkable. A cartridge boots on
      # its own and has nothing to type, so "did not type" there is the right
      # answer rather than a failure -- reported as n/a so it does not read as
      # two hundred false negatives once the whole corpus runs.
      case "${name,,}" in
         *.cpr) typed="n/a" ;;
         *)     grep -q "Autorun: typing" "$log" && typed=ok ;;
      esac

      # 3. Savestate round trip, with the machine verifiably paused so nothing
      #    but this script touches the probe byte. Every step waits for its
      #    own result rather than sleeping a fixed time: under -j a 136 KB
      #    state takes longer to write than any sleep guessed here, and the
      #    run before this reported thirteen "failures" that all passed when
      #    the same image was probed on its own.
      wait_probe() {  # $1 expected byte, waits up to ~10s
         local i
         for i in $(seq 1 20); do
            [ "$(probe_byte)" = "$1" ] && return 0
            sleep 0.5
         done
         return 1
      }
      if ! pause_until_paused; then
         state="nopause"
      else
         cmd "WRITE_CORE_MEMORY $PROBE A5" >/dev/null
         if wait_probe A5; then
            cmd "SAVE_STATE" >/dev/null
            # The state file appearing is the proof the save completed.
            for _ in $(seq 1 30); do
               [ -n "$(find "$statedir" -name '*.state*' -size +1k 2>/dev/null)" ] && break
               sleep 0.5
            done
            cmd "WRITE_CORE_MEMORY $PROBE 5A" >/dev/null
            if wait_probe 5A; then
               cmd "LOAD_STATE" >/dev/null
               wait_probe A5 && state=ok
            fi
         fi
         # LOAD_STATE resumes the emulator by itself, so this is usually a
         # no-op; it still runs for the paths that never got that far.
         unpause
      fi

   fi

   local alive=crashed
   kill -0 "$ra" 2>/dev/null && { alive=ok; kill "$ra" 2>/dev/null; }
   wait "$ra" 2>/dev/null
   rm -rf "$cfg" "$statedir"

   printf '%-52s boot=%-4s typed=%-4s state=%-7s shot=%-4s proc=%s\n' \
      "${name:0:52}" "$booted" "$typed" "$state" "$shotres" "$alive"
}
export -f run_one
export CORE OUTDIR BOOT_SECONDS TYPE_SECONDS PROBE

mapfile -t files < <(for d in "$@"; do
   find "$d" -type f \( -iname '*.dsk' -o -iname '*.cdt' -o -iname '*.ipf' \
      -o -iname '*.hfe' -o -iname '*.scp' -o -iname '*.raw' -o -iname '*.cpr' \
      -o -iname '*.wav' -o -iname '*.ctr' \) 2>/dev/null
done | sort)
[ "$LIMIT" -gt 0 ] && files=("${files[@]:0:$LIMIT}")

echo "core   : $CORE"
echo "images : ${#files[@]}"
echo "output : $OUTDIR"
echo

results="$OUTDIR/results.txt"
: > "$results"
# Throttle with an explicit counter and wait -n rather than polling jobs,
# so the concurrency is exactly -j and not whatever the job table reports.
slot=0
running=0
for f in "${files[@]}"; do
   run_one "$f" "$((slot % JOBS))" >> "$results" &
   slot=$((slot + 1))
   running=$((running + 1))
   if [ "$running" -ge "$JOBS" ]; then
      wait -n
      running=$((running - 1))
   fi
done
wait

sort "$results"
echo
for k in boot typed state shot; do
   ok=$(grep -c "$k=ok" "$results")
   na=$(grep -c "$k=n/a" "$results")
   total=$(( ${#files[@]} - na ))
   if [ "$na" -gt 0 ]; then
      printf '%-7s %d/%d  (%d n/a)\n' "$k" "$ok" "$total" "$na"
   else
      printf '%-7s %d/%d\n' "$k" "$ok" "$total"
   fi
done
crashed=$(grep -c "proc=crashed" "$results")
echo "crashed $crashed/${#files[@]}"
echo
echo "screenshots in $OUTDIR/shots, logs in $OUTDIR/logs"
[ "$crashed" -eq 0 ] && [ "$(grep -c 'boot=ok' "$results")" -eq "${#files[@]}" ]
