#!/usr/bin/env bash
# Performance experiment: sequential server (V1) vs. thread-pool server (V2).
#
# For each scenario it starts the server, launches N clients at the same time
# (like `for i in {1..10}; do ./client file.txt & done`) and measures:
#   - total wall-clock time until the last client finishes
#   - average / maximum client latency
#   - server CPU time and CPU usage (%) from /proc/<pid>/stat
#   - server peak memory (VmHWM) and thread count from /proc/<pid>/status
#
# Usage: ./scripts/benchmark.sh [-n clients] [-t threads] [-r repeats] [-f file] [-D delay_ms]
#   With no -f, runs the standard scenarios: file.txt, 10 MB, 100 MB, and file.txt with a 200 ms delay.
# The results table is printed and also saved to results/benchmark_results.md.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

CLIENTS=10; THREADS=4; REPEATS=3; ONLY_FILE=""; ONLY_DELAY=0
while getopts "n:t:r:f:D:h" opt; do
    case $opt in
        n) CLIENTS=$OPTARG ;; t) THREADS=$OPTARG ;; r) REPEATS=$OPTARG ;;
        f) ONLY_FILE=$OPTARG ;; D) ONLY_DELAY=$OPTARG ;;
        *) sed -n '2,15p' "$0"; exit 0 ;;
    esac
done

for b in version1/server version1/client version2_threadpool/server version2_threadpool/client; do
    [[ -x $b ]] || { echo "Missing $b. Run 'make' first."; exit 1; }
done
[[ -f server_files/large_100MB.bin ]] || ./scripts/make_test_files.sh

if [[ -n $ONLY_FILE ]]; then
    SCENARIOS=("$ONLY_FILE:$ONLY_DELAY")
else
    SCENARIOS=("file.txt:0" "medium_10MB.bin:0" "large_100MB.bin:0" "file.txt:200")
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/tcp-bench.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
TICKS=$(getconf CLK_TCK)

# Runs one scenario once. Prints: wall_s avg_ms max_ms cpu_s cpu_pct peak_mb threads ok
run_once() {  # version file delay
    local version=$1 file=$2 delay=$3 port server client
    if [[ $version == v1 ]]; then
        port=19101; server=(version1/server -p $port -d server_files -D "$delay"); client=version1/client
    else
        port=19102; server=(version2_threadpool/server -p $port -d server_files -t "$THREADS" -D "$delay")
        client=version2_threadpool/client
    fi

    "${server[@]}" > "$WORK/server.log" 2>&1 &
    local spid=$!
    for _ in $(seq 50); do ss -ltn | grep -q ":$port " && break; sleep 0.1; done

    local cpu0 cpu1 t0 t1 pids=()
    cpu0=$(awk '{print $14 + $15}' /proc/$spid/stat)
    t0=$(date +%s%N)
    for i in $(seq 1 "$CLIENTS"); do
        "$client" -p $port -o "$WORK/dl$i" "$file" > "$WORK/client$i.log" 2>&1 &
        pids+=($!)
    done
    wait "${pids[@]}"
    t1=$(date +%s%N)
    cpu1=$(awk '{print $14 + $15}' /proc/$spid/stat)
    local peak_kb threads
    peak_kb=$(awk '/VmHWM/ {print $2}' /proc/$spid/status)
    threads=$(awk '/Threads/ {print $2}' /proc/$spid/status)

    kill -INT $spid; wait $spid 2>/dev/null
    rm -rf "$WORK"/dl*

    local ok
    ok=$(cat "$WORK"/client*.log | grep -c 'SHA-256 verified')
    cat "$WORK"/client*.log | grep -o 'total [0-9.]* ms' | awk -v t0="$t0" -v t1="$t1" -v c0="$cpu0" -v c1="$cpu1" \
        -v hz="$TICKS" -v pk="$peak_kb" -v th="$threads" -v ok="$ok" '
        { sum += $2; if ($2 > max) max = $2; n++ }
        END {
            wall = (t1 - t0) / 1e9; cpu = (c1 - c0) / hz
            printf "%.3f %.1f %.1f %.2f %.0f %.1f %d %d\n", wall, (n ? sum / n : 0), max, cpu, cpu / wall * 100, pk / 1024, th, ok
        }'
}

TABLE="| Scenario | Server | Total time (s) | Avg client latency (ms) | Max client latency (ms) | Server CPU time (s) | Server CPU usage | Peak memory (MB) | Threads | Successful clients |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"

echo "Benchmark: $CLIENTS simultaneous clients, V2 with $THREADS threads, mean of $REPEATS runs"
echo "Machine: $(nproc) CPU cores, $(uname -sr)"
echo
for sc in "${SCENARIOS[@]}"; do
    file=${sc%%:*}; delay=${sc##*:}
    label="$file"; [[ $delay != 0 ]] && label="$file + ${delay} ms delay"
    for version in v1 v2; do
        name="V1 sequential"; [[ $version == v2 ]] && name="V2 pool ($THREADS threads)"
        echo -n "  running: $label, $name ... "
        rows=""
        for _ in $(seq 1 "$REPEATS"); do rows+="$(run_once $version "$file" "$delay")"$'\n'; done
        row=$(echo -n "$rows" | awk -v label="$label" -v name="$name" -v n="$CLIENTS" '
            { for (i = 1; i <= 8; i++) s[i] += $i; r++ }
            END {
                printf "| %s | %s | %.3f | %.1f | %.1f | %.2f | %.0f%% | %.1f | %d | %d/%d |",
                    label, name, s[1]/r, s[2]/r, s[3]/r, s[4]/r, s[5]/r, s[6]/r, s[7]/r, s[8]/r, n
            }')
        echo "done"
        TABLE+=$'\n'"$row"
    done
done

mkdir -p results
{
    echo "# Benchmark results"
    echo
    echo "- Date: $(date '+%Y-%m-%d %H:%M')"
    echo "- Machine: $(nproc) CPU cores, $(uname -sr)"
    echo "- $CLIENTS simultaneous clients per run, V2 pool size $THREADS, each value is the mean of $REPEATS runs"
    echo "- Command: \`./scripts/benchmark.sh -n $CLIENTS -t $THREADS -r $REPEATS\`"
    echo
    echo "$TABLE"
} > results/benchmark_results.md

echo
echo "$TABLE"
echo
echo "Saved to results/benchmark_results.md"
