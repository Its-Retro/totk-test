#!/bin/sh
# build_launcher.sh [startup dir with the play-build headers] : builds startup/totk.exe
# The launcher includes boot_runtime.h only for the BootContext layout (it checks boot_context_size()).
S=${1:-$(cd "$(dirname "$0")" && pwd)}
ZIG=${ZIG:-$S/../tools/zig-x86_64-windows-0.16.0/zig.exe}
$ZIG cc -target x86_64-windows-gnu -std=c11 -O2 -Wall -Wextra -Werror -municode -I "$S" \
  "$S/totk_launcher.c" -lbcrypt -luser32 -o "$S/totk.exe"
