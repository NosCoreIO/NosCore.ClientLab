#!/usr/bin/env bash
# Linux counterpart of decompile.ps1: Ghidra alone produces functions.txt. IDR (Windows-only) is skipped; the lifter
# recovers what it would have seeded (unit init routines from the init table, methods from relocated pointers).
#
# usage: tools/decompile.sh <NosCore.exe>     env: GHIDRA_HOME (contains support/analyzeHeadless), NOSRECOMP_WORK
set -euo pipefail
exe=$1
work=${NOSRECOMP_WORK:-$HOME/re/work}
ghidra=${GHIDRA_HOME:?set GHIDRA_HOME to the Ghidra install}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$work/input" "$work/ghidra"
cp "$exe" "$work/input/NosCore.exe"
"$ghidra/support/analyzeHeadless" "$work/ghidra" NosTaleClient \
    -import "$work/input/NosCore.exe" -overwrite -processor x86:LE:32:default -cspec borlanddelphi \
    -scriptPath "$here/ghidra" -postScript ExportFunctions.java "$work/functions.txt"
echo "$(wc -l < "$work/functions.txt") functions in $work/functions.txt"
