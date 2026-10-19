#!/usr/bin/env bash
# tests/preload_smoke.sh -- run real programs with and without LD_PRELOAD of an
# allocator and compare what they produce.
#
# Usage: tests/preload_smoke.sh [--lib PATH] [--only=a,b] [--keep] [--verbose] [--list]
#
#   --lib PATH   allocator to preload (default: build/libsohamalloc.so)
#   --only=...   comma-separated case names (see --list)
#   --keep       keep the work directory (printed at the end)
#
# Every case runs twice in a fresh directory: once with the default
# allocator (the reference) and once with LD_PRELOAD=PATH.  A case FAILs if
# the preloaded run crashes or exits differently, if its stdout (or the
# files it produced, compared by checksum) differ from the reference, or if
# it writes anything to stderr that the reference run did not.  With
# SOHAMALLOC_VERBOSE unset sohamalloc must be silent.
#
# Cases: python (dict/list/json/sort-heavy script, tests/data/workload.py),
# gcc (hello.c and the generated 4500-line bigger.c, -O2, object files and
# program output compared), gxx (template-heavy tests/data/templates.cpp),
# git (temporary repo: init, add, commit x3, log, gc), perl (hash-heavy
# one-liner), sort (sort -S 10M of 200k generated lines), find
# (find /usr/include | wc -l), tar (czf + xzf of /usr/include, listing and
# extracted tree compared), bash (string/array-heavy loop).
# Exit status: 0 if every case passed (or was skipped because its program is
# not installed), 1 otherwise.
set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DATA="$REPO/tests/data"
LIB="$REPO/build/libsohamalloc.so"
ONLY=""
KEEP=0
VERBOSE=0
LIST=0
ALL_CASES="python gcc gxx git perl sort find tar bash"

while [ $# -gt 0 ]; do
    case "$1" in
        --lib) LIB="$2"; shift ;;
        --lib=*) LIB="${1#--lib=}" ;;
        --only=*) ONLY="${1#--only=}" ;;
        --keep) KEEP=1 ;;
        --verbose|-v) VERBOSE=1 ;;
        --list) LIST=1 ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "preload_smoke.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
    shift
done
if [ "$LIST" = 1 ]; then printf '%s\n' $ALL_CASES; exit 0; fi
[ -f "$LIB" ] || { echo "preload_smoke.sh: library not found: $LIB" >&2; exit 2; }
LIB=$(cd "$(dirname "$LIB")" && pwd)/$(basename "$LIB")

WORK=$(mktemp -d "${TMPDIR:-/tmp}/preload_smoke.XXXXXX")
cleanup() { if [ "$KEEP" = 1 ]; then echo "work dir kept: $WORK"; else rm -rf "$WORK"; fi; }
trap cleanup EXIT

# A stable environment: identical for both runs, so outputs can be diffed.
export LC_ALL=C TZ=UTC HOME="$WORK/home" PYTHONHASHSEED=0
export GIT_AUTHOR_NAME="Smoke Test" GIT_AUTHOR_EMAIL="smoke@example.invalid"
export GIT_COMMITTER_NAME="Smoke Test" GIT_COMMITTER_EMAIL="smoke@example.invalid"
export GIT_AUTHOR_DATE="2026-01-01T00:00:00Z" GIT_COMMITTER_DATE="2026-01-01T00:00:00Z"
export GIT_CONFIG_NOSYSTEM=1
unset SOHAMALLOC_VERBOSE SOHAMALLOC_STATS MALLOC_CHECK_ MALLOC_PERTURB_
mkdir -p "$HOME"

# ------------------------------------------------------------------ cases
# Each case_X runs in the current directory (fresh per run) and prints its
# observable result on stdout; produced files are checksummed by the case.

case_python() {
    command -v python3 >/dev/null || return 77
    python3 -I "$DATA/workload.py"
}

case_gcc() {
    command -v gcc >/dev/null || return 77
    gcc -O2 -o hello "$DATA/hello.c" && ./hello || return 1
    gcc -O2 -Wall -c -o bigger.o "$DATA/bigger.c" || return 1
    gcc -o bigger bigger.o && ./bigger || return 1
    # Same compiler, same input: the object code must be byte-identical.
    sha256sum bigger.o | cut -d' ' -f1
    gcc -O0 -g -E "$DATA/bigger.c" | sha256sum | cut -d' ' -f1
}

case_gxx() {
    command -v g++ >/dev/null || return 77
    g++ -std=c++17 -O2 -c -o templates.o "$DATA/templates.cpp" || return 1
    g++ -o templates templates.o && ./templates || return 1
    sha256sum templates.o | cut -d' ' -f1
}

case_git() {
    command -v git >/dev/null || return 77
    git init -q -b main repo && cd repo || return 1
    git config user.name "$GIT_AUTHOR_NAME" && git config user.email "$GIT_AUTHOR_EMAIL"
    git config gc.auto 0
    cp "$DATA"/*.c "$DATA"/*.cpp "$DATA"/*.py . && git add . && git commit -q -m "first" || return 1
    for i in 1 2; do
        seq 1 $((i * 20000)) | awk '{ print $1 * 7919 % 100003 }' > "nums$i.txt"
        sed -i "s/hello, world/hello, world $i/" hello.c
        git add . && git commit -q -m "change $i" || return 1
    done
    git log --stat --format='%H %an %s'
    git diff HEAD~2 --stat
    git gc -q --aggressive 2>&1 && git fsck --strict 2>&1 | grep -v '^Checking' || true
    git rev-parse HEAD^{tree}
}

case_perl() {
    command -v perl >/dev/null || return 77
    perl -e 'my %h; for my $i (1..300000) { $h{"k".($i*7919 % 100003)} .= chr(97 + $i % 26); }
             my @k = sort { length($h{$b}) <=> length($h{$a}) || $a cmp $b } keys %h;
             my %inv; push @{$inv{length $h{$_}}}, $_ for @k;
             delete @h{@k[0..999]};
             my $s = join(",", map { "$_=" . scalar(@{$inv{$_}}) } sort { $a <=> $b } keys %inv);
             print scalar(keys %h), " ", $k[0], " ", length($s), " ", substr($s, 0, 120), "\n";'
}

case_sort() {
    command -v sort >/dev/null || return 77
    awk 'BEGIN { x = 12345; for (i = 0; i < 200000; i++) { x = (x * 1103515245 + 12345) % 2147483648;
         printf "%08x %d line-%d %s\n", x, x % 977, i, substr("abcdefghijklmnopqrstuvwxyz", x % 26 + 1) } }' > in.txt
    sort -S 10M -k2,2n -k1,1 in.txt > out1.txt || return 1
    sort -S 10M -u -r in.txt > out2.txt || return 1
    sha256sum out1.txt out2.txt | cut -d' ' -f1
    wc -l < out2.txt
}

case_find() {
    command -v find >/dev/null || return 77
    find /usr/include | wc -l
    find /usr/include -name '*.h' -size +20k | sort | sha256sum | cut -d' ' -f1
}

case_tar() {
    command -v tar >/dev/null || return 77
    tar czf inc.tgz -C /usr include || return 1
    tar tzvf inc.tgz | awk '{ print $1, $3, $6 }' | sort | sha256sum | cut -d' ' -f1
    mkdir x && tar xzf inc.tgz -C x || return 1
    if diff -r -q --no-dereference x/include /usr/include > /dev/null; then echo "extracted tree identical"; else echo "extracted tree differs"; fi
}

case_bash() {
    bash -c '
        declare -A h; s=""; a=()
        for ((i = 0; i < 40000; i++)); do
            k="key$(( i * 7919 % 4001 ))"; h[$k]+="${i: -1}"; a+=("$k:$i")
            (( i % 100 == 0 )) && s+="${#h[$k]},"
        done
        unset "a[5]" "a[77]"; b=("${a[@]:100:5000}"); c="${s//1/one}"
        echo "${#h[@]} ${#a[@]} ${#b[@]} ${#s} ${#c} ${h[key17]:0:20}"
        printf "%s\n" "${a[@]}" | sort | uniq | wc -l
    '
}

# ------------------------------------------------------------------ driver
run_case() {    # name mode(ref|lib) -> $WORK/name.mode.{out,err,rc}
    # Both runs use the same directory path (compilers record the cwd).
    local name="$1" mode="$2" dir="$WORK/run"
    rm -rf "$dir"; mkdir -p "$dir"
    (
        cd "$dir" || exit 99
        if [ "$mode" = lib ]; then export LD_PRELOAD="$LIB"; fi
        "case_$name"
    ) > "$WORK/$name.$mode.out" 2> "$WORK/$name.$mode.err" < /dev/null
    echo $? > "$WORK/$name.$mode.rc"
}

npass=0; nfail=0; nskip=0
echo "== preload smoke test, LD_PRELOAD=$LIB"
printf '%-8s %-6s %7s %7s  %s\n' CASE RESULT REF LIB DETAIL
printf '%-8s %-6s %7s %7s  %s\n' -------- ------ ------- ------- ------------------------------
for name in $ALL_CASES; do
    if [ -n "$ONLY" ] && [[ ",$ONLY," != *",$name,"* ]]; then continue; fi
    t0=$(date +%s.%N); run_case "$name" ref; t1=$(date +%s.%N); run_case "$name" lib; t2=$(date +%s.%N)
    tr=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1fs", b - a }')
    tl=$(awk -v a="$t1" -v b="$t2" 'BEGIN { printf "%.1fs", b - a }')
    rr=$(cat "$WORK/$name.ref.rc"); rl=$(cat "$WORK/$name.lib.rc")
    status=PASS; detail=""
    if [ "$rr" = 77 ]; then
        status=SKIP; detail="program not installed"
    elif [ "$rr" != 0 ]; then
        status=FAIL; detail="reference run (no preload) failed with status $rr"
    elif [ "$rl" != "$rr" ]; then
        status=FAIL
        if [ "$rl" -gt 128 ]; then detail="crashed with signal $((rl - 128))"; else detail="exit status $rl (reference $rr)"; fi
    elif ! cmp -s "$WORK/$name.ref.out" "$WORK/$name.lib.out"; then
        status=FAIL; detail="stdout differs from the reference"
    elif ! cmp -s "$WORK/$name.ref.err" "$WORK/$name.lib.err"; then
        # Anything on stderr that the reference did not print is unexpected.
        if [ -s "$WORK/$name.lib.err" ]; then status=FAIL; detail="unexpected stderr: $(head -c 120 "$WORK/$name.lib.err" | tr '\n' ' ')"; fi
    fi
    case $status in PASS) npass=$((npass + 1)) ;; SKIP) nskip=$((nskip + 1)) ;; *) nfail=$((nfail + 1)) ;; esac
    printf '%-8s %-6s %7s %7s  %s\n' "$name" "$status" "$tr" "$tl" "$detail"
    if [ "$status" = FAIL ] || [ "$VERBOSE" = 1 ]; then
        if [ "$status" = FAIL ] && ! cmp -s "$WORK/$name.ref.out" "$WORK/$name.lib.out"; then
            diff "$WORK/$name.ref.out" "$WORK/$name.lib.out" | head -20 | sed 's/^/    | /'
        fi
        tail -n 10 "$WORK/$name.lib.err" | sed 's/^/    ! /'
        [ "$VERBOSE" = 1 ] && head -n 20 "$WORK/$name.lib.out" | sed 's/^/    > /'
    fi
done
echo
echo "== $npass passed, $nfail failed, $nskip skipped"
[ $nfail -eq 0 ]
