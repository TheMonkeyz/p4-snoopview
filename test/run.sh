#!/bin/sh
# Host tests (no hardware): the song lesson's steps. Needs cc (on this PC: WSL Ubuntu).
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/snoopview_lesson_test"
cc -std=c11 -Wall -Wextra -Werror -Imain test/lesson_test.c main/lesson.c -o "$out"
"$out"
