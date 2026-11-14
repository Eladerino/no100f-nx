#!/usr/bin/env bash
# Switch build of FrightWorks (no100f-nx). Run in WSL (or Linux) with podman or docker, after
# tools\build-no100f.ps1 has translated the game on Windows: this reuses that translation
# (wiicompiled\generated) and only compiles it, here for Horizon inside switch/scripts/Containerfile.
# Adapted from spm-nx switch/scripts/build-switch.sh (12ec5e7), minus its variants and patch passes.
#   bash switch/scripts/build-switch.sh [--jobs N] [--audit] [--out DIR|none] [--game]
# --audit compiles only the runtime's sources for the Switch (no game, no Dawn, no Aurora): the quick
#   check after an upstream merge. It needs no translation.
# --out DIR stages the NRO and its companion files, laid out like the SD card, in DIR/switch/no100f-nx/.
#   DIR defaults to Downloads\no100f-nx-sd in your Windows profile under WSL; --out none skips it.
# --game also stages your extracted disc (Disc/DATA) as DIR/switch/no100f-nx/game. It is about 1.4 GB
#   and the card only needs it once. It is your own disc's data: it stays on your PC and your card.
# Sources are copied to ~/no100f-switch first (the container mounts that, not the Windows drive, which
# is much slower through WSL). Output: ~/no100f-switch/build/no100f-nx.nro
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
jobs=6   # translated shards need ~1 GB each (spm-nx's figure; a guess for FrightWorks)
audit=0
out=auto
game=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs=$2; shift ;;
        --audit) audit=1 ;;
        --out) out=$2; shift ;;
        --game) game=1 ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done

engine=$(command -v podman || command -v docker || true)
[[ -n $engine ]] || { echo "needs podman or docker" >&2; exit 1; }
image=no100f-nx-build
"$engine" build -t "$image" -f "$root/switch/scripts/Containerfile" "$root/switch/scripts"

work=$HOME/no100f-switch
mkdir -p "$work/wiicompiled"
echo "copying sources to $work"
rsync -a --delete --exclude probe "$root/switch/" "$work/switch/"
rsync -a --delete "$root/wiicompiled/runtime/" "$work/wiicompiled/runtime/"
rsync -a --delete "$root/wiicompiled/aurora-main/" "$work/wiicompiled/aurora-main/"
rsync -a --delete "$root/wiicompiled/projects/" "$work/wiicompiled/projects/"
if [[ $audit == 0 ]]; then
    src=$root/wiicompiled
    [[ -f $src/generated/build_shards/shards.cmake && -f $src/generated/RuntimeConfig.h ]] ||
        { echo "no translated game in $src/generated: run tools\\build-no100f.ps1 on Windows first" >&2; exit 1; }
    rsync -a --delete --exclude functions "$src/generated/" "$work/wiicompiled/generated/"
    # The translator wrote absolute Windows paths into the shard list and the data blobs' .incbin
    # lines; inside the container the tree is at /work.
    if command -v wslpath >/dev/null; then
        win=$(wslpath -m "$src")
        grep -rlF "$win" "$work/wiicompiled/generated" --include='*.cmake' --include='*.S' --include='*.cpp' --include='*.h' |
            xargs -r sed -i "s|$win|/work/wiicompiled|g"
        # A path written with backslashes would slip past the rewrite above; say so instead of
        # letting the build fail later on a missing file.
        if grep -rqiE '[a-z]:\\\\' "$work/wiicompiled/generated/build_shards/shards.cmake"; then
            echo "WARNING: shards.cmake still holds a Windows path with backslashes; the build may not find those files" >&2
        fi
    fi
fi

# Separate build folders: CMake caches FW_SWITCH_AUDIT, so a full build in the audit's folder would
# still skip Dawn and Aurora.
products=ON; target=WiiCompiled_nro; build=build; extra=(-DFW_SWITCH_AUDIT=OFF)
if [[ $audit == 1 ]]; then
    products=OFF; target=recomp_switch_audit; build=build-audit; extra=(-DFW_SWITCH_AUDIT=ON)
fi

"$engine" run --rm -v "$work:/work" -w /work "$image" bash -c "
    set -e
    cmake -S wiicompiled/runtime -B $build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
        -DRECOMP_BUILD_PRODUCTS=$products -DRECOMP_TRANSLATED_COMPILE_JOBS=$jobs ${extra[*]}
    cmake --build $build --target $target --parallel $jobs -- -k 0
" 2>&1 | tee "$work/$build.log"

if [[ $audit == 1 ]]; then
    echo "DONE: runtime compiles for Horizon"
    exit 0
fi

nro=$work/$build/no100f-nx.nro
[[ -f $nro ]] || { echo "no NRO at $nro: see $work/$build.log" >&2; exit 1; }
echo "DONE: $nro"
if [[ $out == auto ]]; then
    out=
    if command -v wslpath >/dev/null && command -v cmd.exe >/dev/null; then
        profile=$(cd /mnt/c 2>/dev/null && cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r')
        [[ -n $profile ]] && out=$(wslpath -u "$profile")/Downloads/no100f-nx-sd
    fi
fi
if [[ -n $out && $out != none ]]; then
    dest=$out/switch/no100f-nx
    mkdir -p "$dest"
    cp "$nro" "$dest/"
    # The runtime's companion files (PublicProducts.cmake copies them beside the executable).
    for f in dsp_coef.bin cacert.pem initial_pipeline_cache.db; do
        [[ -f $work/$build/$f ]] && cp "$work/$build/$f" "$dest/"
    done
    [[ -d $work/$build/wii_bootstrap ]] && cp -r "$work/$build/wii_bootstrap" "$dest/"
    # The license pack travels with the binary.
    mkdir -p "$dest/licenses"
    cp "$root/LICENSE" "$root/wiicompiled/THIRD-PARTY-NOTICES.md" "$root/switch/THIRD_PARTY.md" "$dest/licenses/"
    cp "$root"/switch/licenses/*.txt "$dest/licenses/"
    if [[ $game == 1 ]]; then
        [[ -f $root/Disc/DATA/sys/boot.bin ]] || { echo "--game: no extracted disc at $root/Disc/DATA" >&2; exit 1; }
        echo "staging your extracted disc as $dest/game (one time; about 1.4 GB)"
        rsync -a --delete "$root/Disc/DATA/" "$dest/game/"
    fi
    echo "COPIED to $dest ($(date -r "$dest/no100f-nx.nro" '+%Y-%m-%d %H:%M'))"
    echo "copy it onto the SD card, merging (never replace the no100f-nx folder: its UserData holds saves and logs),"
    echo "with the disc in sdmc:/switch/no100f-nx/game (files/ and sys/), then launch with title takeover"
else
    echo "copy $nro to sdmc:/switch/no100f-nx/, then launch with title takeover"
fi
