#!/bin/bash
# Build the 3DS targets with devkitPro inside MSYS2. From Windows:
#   D:\msys64\usr\bin\bash.exe -l port/3ds/build.sh [Release|Debug] [target...]
# From Git Bash, the same path works: /d/msys64/usr/bin/bash.exe -l ...
# Output: port/3ds/build/*.3dsx and *.cia, plus a .text size line per ELF.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
type="${1:-Release}"
shift || true
targets=()
for t in "$@"; do targets+=(--target "$t"); done
# the game's ~260 parts: a dozen GCCs at once fit in 32 GB (PSP2I_JOBS to change)
jobs="${PSP2I_JOBS:-12}"
: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

# make_assets.py needs PIL, which MSYS2's own Python lacks: use the Windows
# Python (PSP2I_PYTHON, or the first python on the Windows PATH)
if [ -z "$PSP2I_PYTHON" ]; then
    # MSYS2's login shell drops the Windows PATH: try the usual installs
    # (cygpath -F 28: the user's Local AppData, even with a bare environment)
    for py in "$(cygpath -F 28)"/Programs/Python/Python3*/python.exe; do
        if [ -x "$py" ] && "$py" -c "import PIL" 2>/dev/null; then PSP2I_PYTHON="$py"; break; fi
    done
    [ -n "$PSP2I_PYTHON" ] || { echo "3ds: set PSP2I_PYTHON to a Python with PIL"; exit 1; }
fi

cmake -S "$here" -B "$here/build" -G Ninja -DCMAKE_BUILD_TYPE="$type" \
      -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
      -DPython3_EXECUTABLE="$PSP2I_PYTHON"
cmake --build "$here/build" -j "$jobs" "${targets[@]}"

# the code-size budget line (CLAUDE.md, 3DS rules)
for elf in "$here"/build/*.elf; do
    [ -f "$elf" ] || continue
    arm-none-eabi-size -A "$elf" | awk -v f="$(basename "$elf")" \
        '$1==".text"{printf "3ds: %s .text %.2f MB\n", f, $2/1048576}'
done
ls -la "$here"/build/*.3dsx "$here"/build/*.cia 2>/dev/null
