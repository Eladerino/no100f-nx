#!/usr/bin/env bash
# Switch build of FrightWorks (no100f-nx). Run in WSL (or Linux) with podman or docker, after
# tools\build-no100f.ps1 has translated the game on Windows: this reuses that translation
# (wiicompiled\generated) and only compiles it, here for Horizon inside switch/scripts/Containerfile.
# Adapted from spm-nx switch/scripts/build-switch.sh (12ec5e7), minus its variants and patch passes.
#   bash switch/scripts/build-switch.sh [--jobs N] [--audit] [--deko] [--out DIR|none] [--game]
# --deko is the H3 build: FW_SWITCH_DEKO=ON (the deko3d interposer), in its own build-deko folder.
# --audit compiles only the runtime's sources for the Switch (no game, no Dawn, no Aurora): the quick
#   check after an upstream merge. It needs no translation.
# --out DIR stages the NRO and its companion files, laid out like the SD card, in DIR/switch/no100f-nx/.
#   DIR defaults to Switch Ports\FrightWorks\no100f-nx-sd in your Windows profile under WSL; --out none skips it.
# --game also stages your extracted disc (Disc/DATA) as DIR/switch/no100f-nx/game. It is about 1.4 GB

# --variant pgo is the instrumented PGO capture build (FW_PGO_INSTRUMENT + FW_PGO_ALL: the
# translated shards, the runtime and Aurora count every block they run; counts land in
# UserData/pgo on the card; never quote its frame rates; a capture build also records the
# training manifest into pgo/ at the repo root). --variant pgo-use merges the .profraw files
# in pgo/ with llvm-profdata, builds with the trained profile, then enforces the
# function-level fit check in pgo-fit-check.py before staging anything.
# --variant pgo-trim is the pgo capture build with the Aurora scope trimmed to the
# spm-nx seven (FW_PGO_TRIM_AURORA: aurora_gd/os/ms/card not instrumented), the
# wedge-diagnosis step-3 experiment; it writes pgo/manifest-trim.txt and leaves the
# --variant pgo manifest and build folder untouched.
#   and the card only needs it once. It is your own disc's data: it stays on your PC and your card.
# Sources are copied to ~/no100f-switch first (the container mounts that, not the Windows drive, which
# is much slower through WSL). Output: ~/no100f-switch/build/no100f-nx.nro
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
jobs=6   # translated shards need ~1 GB each (spm-nx's figure; a guess for FrightWorks)
audit=0
deko=0
out=auto
game=0
variant=default
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs=$2; shift ;;
        --audit) audit=1 ;;
        --deko) deko=1 ;;
        --out) out=$2; shift ;;
        --game) game=1 ;;
        --variant) variant=$2; shift ;;
        *) echo "unknown argument $1" >&2; exit 1 ;;
    esac
    shift
done

engine=$(command -v podman || command -v docker || true)
[[ -n $engine ]] || { echo "needs podman or docker" >&2; exit 1; }
image=no100f-nx-build
"$engine" build -t "$image" -f "$root/switch/scripts/Containerfile" "$root/switch/scripts"

# The clang major the build image carries (19 today). Raw PGO profiles are locked to it, so
# the capture build records it in the training manifest and pgo-use refuses another major.
pgo_clang_major() {
    "$engine" run --rm "$image" clang-19 --version 2>/dev/null | head -n 1 |
        sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p'
}

work=$HOME/no100f-switch
mkdir -p "$work/wiicompiled"
echo "copying sources to $work"
rsync -a --delete --exclude probe "$root/switch/" "$work/switch/"
rsync -a --delete "$root/wiicompiled/runtime/" "$work/wiicompiled/runtime/"
rsync -a --delete "$root/wiicompiled/aurora-main/" "$work/wiicompiled/aurora-main/"
rsync -a --delete "$root/wiicompiled/projects/" "$work/wiicompiled/projects/"
# runtime/cmake/build_info.cmake reads the FrightWorks version from the launcher's csproj.
mkdir -p "$work/launcher" && cp "$root/launcher/FrightWorks.Launcher.csproj" "$work/launcher/"
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
if [[ $deko == 1 ]]; then
    products=ON; target=WiiCompiled_nro; build=build-deko; extra=(-DFW_SWITCH_AUDIT=OFF -DFW_SWITCH_DEKO=ON)
fi

# PGO variants (ported from spm-nx's spec E flow): pgo is the instrumented capture build
# (FW_PGO_INSTRUMENT + FW_PGO_ALL, the whole clang scope from day one; never quote its frame
# rates), pgo-use consumes the trained profile in $root/pgo and enforces the fit check before
# staging anything. Each gets its own build folder: CMake caches the PGO options.
case $variant in
    default) ;;
    pgo) build=$build-pgo; extra+=(-DFW_PGO_INSTRUMENT=ON -DFW_PGO_ALL=ON) ;;
    pgo-trim) build=$build-pgo-trim; extra+=(-DFW_PGO_INSTRUMENT=ON -DFW_PGO_ALL=ON -DFW_PGO_TRIM_AURORA=ON) ;;
    pgo-use) build=$build-pgo-use; extra+=(-DFW_PGO_USE=ON -DFW_PGO_PROFILE=/work/pgo/no100f.profdata) ;;
    *) echo "unknown variant $variant (default|pgo|pgo-trim|pgo-use)" >&2; exit 1 ;;
esac

if [[ $variant == pgo-use ]]; then
    # Provenance first: the manifest the capture build recorded and at least one raw profile
    # must sit in $root/pgo; the hard stops mirror FrightWorksSwitch.cmake's own checks, so a
    # bad profile fails here in seconds rather than inside the container build.
    pgo_dir=$root/pgo
    [[ -f $pgo_dir/manifest.txt ]] || { echo "pgo-use: no $pgo_dir/manifest.txt: run a --variant pgo capture build first (it records the manifest), then a training session" >&2; exit 1; }
    pgo_raws=()
    for f in "$pgo_dir"/*.profraw; do [[ -f $f ]] && pgo_raws+=("$f"); done
    (( ${#pgo_raws[@]} > 0 )) || { echo "pgo-use: no .profraw files in $pgo_dir: copy the last fw-*.profraw of each training session from the SD card's UserData/pgo/ into $pgo_dir first" >&2; exit 1; }
    pgo_mf() { sed -n "s/^$1=//p" "$pgo_dir/manifest.txt" | head -n 1; }
    pgo_region=$(pgo_mf region); pgo_commit=$(pgo_mf commit); pgo_clang=$(pgo_mf clang_major)
    pgo_dirty=$(pgo_mf tree_dirty); pgo_shards=$(pgo_mf shards_fingerprint)
    image_clang=$(pgo_clang_major)
    [[ -n $image_clang ]] || { echo "pgo-use: could not read the build image's clang version" >&2; exit 1; }
    [[ $pgo_region == GIHE78-USA ]] || { echo "pgo-use: the profile manifest says region '$pgo_region'; the Switch build is the USA game (GIHE78) only and generated code differs per region. Train a USA profile." >&2; exit 1; }
    [[ $pgo_clang == "$image_clang" ]] || { echo "pgo-use: the profile was recorded with clang major '$pgo_clang' but the build image has clang $image_clang: raw profile formats are version-locked. Re-train." >&2; exit 1; }
    head_commit=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)
    if [[ $pgo_commit != "$head_commit" ]]; then
        echo "============================================================" >&2
        echo "PGO WARNING: the profile was trained on tree $pgo_commit" >&2
        echo "but this tree is $head_commit. That is allowed (profiles are" >&2
        echo "iterative); the fit check after the build is the arbiter, and" >&2
        echo "too much drift fails the build instead of shipping quietly." >&2
        echo "============================================================" >&2
    fi
    [[ $pgo_dirty == 1 ]] && echo "PGO WARNING: the capture build ran on a dirty tree; its recorded commit does not fully describe the trained code." >&2
    if [[ -n $pgo_shards && -f $root/wiicompiled/generated/build_shards/shards.cmake ]]; then
        now_shards=$(sha256sum "$root/wiicompiled/generated/build_shards/shards.cmake" | cut -d' ' -f1)
        [[ $now_shards == "$pgo_shards" ]] || echo "PGO WARNING: the translated game's shard list changed since training (the generated code moved); the fit check will judge how much of the profile still lands." >&2
    fi
    mkdir -p "$work/pgo"
    rsync -a --delete "$pgo_dir/" "$work/pgo/"
    echo "pgo-use: merging ${#pgo_raws[@]} raw profile(s) with llvm-profdata"
    "$engine" run --rm -v "$work:/work" -w /work "$image" bash -c "
        set -e
        profdata=\$(command -v llvm-profdata-19 || command -v llvm-profdata)
        \$profdata merge -o /work/pgo/no100f.profdata /work/pgo/*.profraw
        \$profdata show /work/pgo/no100f.profdata > /work/pgo/profdata-show.txt
    "
fi

"$engine" run --rm -v "$work:/work" -w /work "$image" bash -c "
    set -e
    cmake -S wiicompiled/runtime -B $build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
        -DRECOMP_BUILD_PRODUCTS=$products -DRECOMP_TRANSLATED_COMPILE_JOBS=$jobs ${extra[*]}
    cmake --build $build --target $target --parallel $jobs -- -k 0
" 2>&1 | tee "$work/$build.log"

if [[ $variant == pgo-use ]]; then
    if ! python3 "$root/switch/scripts/pgo-fit-check.py" \
            --log "$work/$build.log" --show "$work/pgo/profdata-show.txt" \
            --max-stale-pct "${FW_PGO_FIT_MAX_STALE_PCT:-25}"; then
        echo "============================================================" >&2
        echo "PGO FIT CHECK FAILED: the NRO above is NOT staged. It compiled," >&2
        echo "but the profile does not fit this tree well enough to carry" >&2
        echo "the PGO name. Re-train (--variant pgo, a training session," >&2
        echo "--variant pgo-use), or raise FW_PGO_FIT_MAX_STALE_PCT knowingly." >&2
        echo "============================================================" >&2
        exit 1
    fi
fi

if [[ $variant == pgo && $audit == 0 ]]; then
    # Record the training manifest beside where the raw profiles will land. It is the
    # profile's provenance (region, commit, compiler, scope, fingerprint of the generated
    # code): pgo-use checks it before trusting any profile, and CMake checks it again beside
    # the merged profile.
    mkdir -p "$root/pgo"
    pgo_clang_written=$(pgo_clang_major)
    [[ -n $pgo_clang_written ]] || { echo "pgo: could not read the build image's clang version for the manifest" >&2; exit 1; }
    pgo_dirty_written=0
    [[ -n $(git -C "$root" status --porcelain 2>/dev/null) ]] && pgo_dirty_written=1
    {
        echo "# no100f-nx PGO profile manifest. Written by build-switch.sh --variant pgo;"
        echo "# checked by FrightWorksSwitch.cmake (FW_PGO_USE) and the pgo-use variant."
        echo "region=GIHE78-USA"
        echo "commit=$(git -C "$root" rev-parse HEAD)"
        echo "tree_dirty=$pgo_dirty_written"
        echo "clang_major=$pgo_clang_written"
        echo "scope=game+runtime+aurora"
        if [[ -f $root/wiicompiled/generated/build_shards/shards.cmake ]]; then
            echo "shards_fingerprint=$(sha256sum "$root/wiicompiled/generated/build_shards/shards.cmake" | cut -d' ' -f1)"
        fi
        echo "trained_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$root/pgo/manifest.txt"
    echo "pgo: training manifest written to $root/pgo/manifest.txt"
    echo "pgo: after the training session, copy the LAST fw-*.profraw of each session from the SD card's UserData/pgo/ into $root/pgo/, then build --variant pgo-use"
fi

if [[ $variant == pgo-trim && $audit == 0 ]]; then
    # The trim experiment gets its own provenance, beside (never over) the --variant
    # pgo manifest: same fields, its own file, so a trim training session's raw
    # profiles can be told from an untrimmed session's. Adopting the trim as the
    # training configuration means pointing pgo-use at this manifest deliberately,
    # as a separate later step.
    mkdir -p "$root/pgo"
    pgo_clang_written=$(pgo_clang_major)
    [[ -n $pgo_clang_written ]] || { echo "pgo-trim: could not read the build image's clang version for the manifest" >&2; exit 1; }
    pgo_dirty_written=0
    [[ -n $(git -C "$root" status --porcelain 2>/dev/null) ]] && pgo_dirty_written=1
    {
        echo "# no100f-nx PGO profile manifest (trimmed Aurora scope). Written by build-switch.sh --variant pgo-trim;"
        echo "# the --variant pgo manifest.txt is the untrimmed capture build's and is not touched."
        echo "region=GIHE78-USA"
        echo "commit=$(git -C "$root" rev-parse HEAD)"
        echo "tree_dirty=$pgo_dirty_written"
        echo "clang_major=$pgo_clang_written"
        echo "scope=game+runtime+aurora-trim-spm7"
        if [[ -f $root/wiicompiled/generated/build_shards/shards.cmake ]]; then
            echo "shards_fingerprint=$(sha256sum "$root/wiicompiled/generated/build_shards/shards.cmake" | cut -d' ' -f1)"
        fi
        echo "trained_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$root/pgo/manifest-trim.txt"
    echo "pgo-trim: training manifest written to $root/pgo/manifest-trim.txt"
fi

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
        [[ -n $profile ]] && out=$(wslpath -u "$profile")/Switch Ports/FrightWorks/no100f-nx-sd
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
