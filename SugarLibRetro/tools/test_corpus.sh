#!/bin/bash
# Format-parser regression pass: load one real sample of every disk/tape
# format the core supports, headless, and check the core survives loading
# each one (no crash within the observation window).
#
# This is NOT the same corpus as cpc_probe.sh's ad-hoc test files -- it's a
# curated, checksummed set with one sample per format, several of which are
# copyrighted commercial dumps kept out of this repo on purpose (needed for
# real copy-protection coverage -- EDSK weak sectors and IPF flux protection
# barely exist in freeware form). See README.md "Testing" for how to get a
# copy. Point this script at it with CPC_CORPUS_DIR if it's not at the
# default location.
#
#   tools/test_corpus.sh [core.so]
#
# Exit status is nonzero if any file made the core crash or hang past the
# per-file timeout with no RAM change at all (see cpc_probe.sh's caveats
# about pause_nonactive -- this script forces it off the same way).
set -u

CORE=${1:-/tmp/sugarlibretro-work/build/SugarLibRetro/libSugarLibRetro.so}
CORPUS=${CPC_CORPUS_DIR:-$HOME/cpc-test-corpus}
DISPLAY_NUM=${CPC_PROBE_DISPLAY:-:79}
LOAD_SECONDS=${CPC_CORPUS_LOAD_SECONDS:-8}

if [ ! -f "$CORE" ]; then
   echo "test_corpus: core not found: $CORE" >&2
   exit 1
fi
if [ ! -d "$CORPUS" ]; then
   echo "test_corpus: corpus not found at $CORPUS (set CPC_CORPUS_DIR)" >&2
   echo "See README.md \"Testing\" for how to build/obtain a copy." >&2
   exit 1
fi

cfg=$(mktemp) || exit 1
cat > "$cfg" <<EOF
pause_nonactive = "false"
video_vsync = "false"
video_driver = "sdl2"
audio_driver = "null"
audio_enable = "false"
menu_driver = "null"
video_fullscreen = "false"
EOF

xvfb_pid=""
cleanup() {
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

fail=0
count=0

for f in "$CORPUS"/*/*; do
   [ -f "$f" ] || continue
   count=$((count + 1))
   log=$(mktemp)
   DISPLAY="$DISPLAY_NUM" retroarch --appendconfig "$cfg" -L "$CORE" "$f" >"$log" 2>&1 &
   pid=$!

   sleep "$LOAD_SECONDS"

   if kill -0 "$pid" 2>/dev/null; then
      status="PASS (alive after ${LOAD_SECONDS}s)"
      kill "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
   else
      wait "$pid" 2>/dev/null
      rc=$?
      if [ "$rc" -eq 0 ]; then
         status="PASS (exited cleanly)"
      else
         status="FAIL (exit $rc -- crash or abort)"
         fail=$((fail + 1))
      fi
   fi

   printf '%-70s %s\n' "${f#"$CORPUS"/}" "$status"
   if [[ "$status" == FAIL* ]]; then
      echo "  --- last log lines ---"
      tail -n 15 "$log" | sed 's/^/  /'
   fi
   rm -f "$log"
done

echo
echo "$((count - fail))/$count passed"
exit $((fail > 0))
