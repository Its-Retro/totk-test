#!/bin/bash
# Combined speed rebuild: regenerate C (translator changed) then full native build.
# Developer script (friends use build.ps1). Run from anywhere; PY defaults to python on PATH.
PY=${PY:-python}
cd "$(dirname "$0")" || exit 1
export TOTK_DRAW_PATH=1 TOTK_MEMORY_PROFILE=expanded-lab TOTK_COMPILE_WORKERS=${WORKERS:-16}
echo "$(date '+%F %T') regenerate start"
[ "$SKIP_REGEN" = 1 ] || "$PY" -B -c "import json,time;json.dump({'pid':0,'output':'${OUT:-totk-startup-speed-1.dll}','phase':'regenerating','compiled':0,'total':13265,'reused':0,'bytes_done':0,'bytes_total':1,'started':time.time(),'updated':time.time()},open('build-progress.json','w'))"
[ "$SKIP_REGEN" = 1 ] || "$PY" -B incremental/translate_startup_incremental.py --ranges-dir . --output translated_startup.c \
  --edges-output external-edges.json --sidecar incremental-page-cache.sqlite --write-instruction-cache > /dev/null || { echo "REGEN FAILED"; exit 1; }
echo "$(date '+%F %T') regenerate done; compiling"
"$PY" incremental/compile_cached_fast.py --optimization O2 --precompiled-header --output ${OUT:-totk-startup-speed-1.dll} || { echo "BUILD FAILED"; exit 1; }
echo "$(date '+%F %T') BUILD DONE"
