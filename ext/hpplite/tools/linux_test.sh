#!/bin/sh
# Build and test HPPLite on Linux inside a container (Docker or Podman).
#
#   ext/hpplite/tools/linux_test.sh            # cmake + ctest (mock L1)
#   ext/hpplite/tools/linux_test.sh stress     # ... then the 100 MB stress test
#   ext/hpplite/tools/linux_test.sh bench      # ... then the microbenchmark
#   ext/hpplite/tools/linux_test.sh asan       # ctest under AddressSanitizer+UBSan
#   ext/hpplite/tools/linux_test.sh tsan       # ctest under ThreadSanitizer
#   ext/hpplite/tools/linux_test.sh shell      # interactive shell in the image
#
# The repository is mounted read-only at /src; the SQLite amalgamation is
# generated from it (tcl) and everything is built under a named volume so
# incremental runs are fast. Set HPPLITE_LINUX_IMAGE to use another base.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
image=${HPPLITE_LINUX_IMAGE:-hpplite-linux}
runtime=${HPPLITE_CONTAINER:-docker}
mode=${1:-test}

$runtime build -q -t "$image" "$here/linux" >/dev/null

if [ "$mode" = shell ]; then
    exec $runtime run --rm -it -v "$root:/src:ro" -v hpplite-linux-build:/work "$image" bash
fi

exec $runtime run --rm -v "$root:/src:ro" -v hpplite-linux-build:/work -e MODE="$mode" "$image" sh -ec '
    if [ ! -f /work/sqlite/sqlite3.c ] || [ /src/VERSION -nt /work/sqlite/sqlite3.c ]; then
        echo "== generating SQLite amalgamation"
        mkdir -p /work/sqlite && cd /work/sqlite
        /src/configure >/dev/null && make -s sqlite3.c sqlite3ext.h >/dev/null
    fi
    flags=""; dir=/work/build; type=Release
    case "$MODE" in
        asan) flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"; dir=/work/build-asan; type=RelWithDebInfo ;;
        tsan) flags="-fsanitize=thread -g"; dir=/work/build-tsan; type=RelWithDebInfo ;;
    esac
    echo "== cmake ($type, mock L1${flags:+, $flags})"
    mkdir -p $dir && cd $dir
    cmake -DHPPLITE_SQLITE_SRC=/work/sqlite -DCMAKE_BUILD_TYPE=$type \
          -DCMAKE_C_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags" \
          /src/ext/hpplite >/dev/null
    cmake --build . -j"$(nproc)" 2>&1 | grep -E "warning|error|Error" || true
    echo "== ctest"
    export ASAN_OPTIONS=detect_leaks=0 TSAN_OPTIONS=halt_on_error=0
    ctest --output-on-failure
    case "$MODE" in
        stress) echo "== stress"; ./stress_hpplite --mb 100 ;;
        bench)  echo "== bench";  ./bench_hpplite ;;
    esac
'
