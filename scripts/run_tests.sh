#!/usr/bin/env bash
# Automated tests for both versions. Run from anywhere: ./scripts/run_tests.sh  (or: make test)
# Servers are started on high ports (19001+) so they don't clash with one you started yourself.
set -uo pipefail
cd "$(dirname "$0")/.."

ROOT=$(pwd)
OUT="$ROOT/test_output"
V1S="$ROOT/version1/server";            V1C="$ROOT/version1/client"
V2S="$ROOT/version2_threadpool/server"; V2C="$ROOT/version2_threadpool/client"
PASS=0; FAIL=0
SERVER_PID=""

GREEN=$'\e[32m'; RED=$'\e[31m'; BOLD=$'\e[1m'; RESET=$'\e[0m'

for b in "$V1S" "$V1C" "$V2S" "$V2C"; do
    [[ -x $b ]] || { echo "Missing $b. Run 'make' first."; exit 1; }
done
[[ -f server_files/medium_10MB.bin ]] || ./scripts/make_test_files.sh >/dev/null

rm -rf "$OUT"; mkdir -p "$OUT/srv" "$OUT/dl"
cp server_files/file.txt server_files/empty.txt server_files/medium_10MB.bin "$OUT/srv/"

pass() { echo "  ${GREEN}PASS${RESET} $1"; PASS=$((PASS + 1)); }
fail() { echo "  ${RED}FAIL${RESET} $1"; FAIL=$((FAIL + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }

start_server() {  # log_name port command...
    local log=$1 port=$2; shift 2
    "$@" > "$OUT/$log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 50); do
        ss -ltn 2>/dev/null | grep -q ":$port " && return 0
        sleep 0.1
    done
    echo "Server failed to start, see $OUT/$log"; cat "$OUT/$log"; exit 1
}
stop_server() {
    [[ -n $SERVER_PID ]] && kill -INT "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
}
trap stop_server EXIT

# ---------------------------------------------------------------------------
echo "${BOLD}Version 1: sequential server${RESET}"
P=19001
start_server v1_server.log $P "$V1S" -p $P -d "$OUT/srv"

"$V1C" -p $P -o "$OUT/dl" file.txt > "$OUT/c.log" 2>&1
check "download file.txt, contents identical" "cmp -s $OUT/srv/file.txt $OUT/dl/received_file.txt"
check "client reports SHA-256 verified" "grep -q 'SHA-256 verified' $OUT/c.log"

"$V1C" -p $P -o "$OUT/dl" medium_10MB.bin > "$OUT/c.log" 2>&1
check "download 10 MB binary file, contents identical" "cmp -s $OUT/srv/medium_10MB.bin $OUT/dl/received_medium_10MB.bin"

"$V1C" -p $P -o "$OUT/dl" empty.txt > "$OUT/c.log" 2>&1
check "empty file is downloaded (not reported as missing)" "[[ -f $OUT/dl/received_empty.txt && ! -s $OUT/dl/received_empty.txt ]]"

"$V1C" -p $P -o "$OUT/dl" does_not_exist.txt > "$OUT/c.log" 2>&1; RC=$?
check "missing file -> 'file not found', exit code 1" "[[ $RC -eq 1 ]] && grep -q 'not found' $OUT/c.log"

"$V1C" -p $P -o "$OUT/dl" ../../etc/passwd > "$OUT/c.log" 2>&1
check "path traversal '../../etc/passwd' rejected" "grep -q 'bad request' $OUT/c.log"

PIDS=(); for i in 1 2 3 4 5; do "$V1C" -p $P -o "$OUT/dl" medium_10MB.bin > "$OUT/c$i.log" 2>&1 & PIDS+=($!); done; wait "${PIDS[@]}"
check "5 simultaneous clients all succeed" "[[ \$(cat $OUT/c[1-5].log | grep -c 'SHA-256 verified') -eq 5 ]]"

check "server log shows client IP, file requested and file size" \
      "grep -q 'client IP: 127.0.0.1' $OUT/v1_server.log && grep -q 'file requested: \"file.txt\"' $OUT/v1_server.log && grep -q 'file size:' $OUT/v1_server.log"
stop_server

# ---------------------------------------------------------------------------
echo "${BOLD}Version 2: thread-pool server${RESET}"
P=19002
start_server v2_server.log $P "$V2S" -p $P -d "$OUT/srv" -t 4 -m 50
rm -f "$OUT"/dl/*

"$V2C" -p $P -o "$OUT/dl" medium_10MB.bin > "$OUT/c.log" 2>&1
check "download 10 MB file, contents identical" "cmp -s $OUT/srv/medium_10MB.bin $OUT/dl/received_medium_10MB.bin"

"$V2C" -p $P -o "$OUT/dl" nope.bin > "$OUT/c.log" 2>&1
check "missing file -> 'file not found'" "grep -q 'not found' $OUT/c.log"

"$V2C" -p $P -u "$OUT/srv/medium_10MB.bin" > "$OUT/c.log" 2>&1
check "upload 10 MB file, stored copy identical" "cmp -s $OUT/srv/medium_10MB.bin $OUT/srv/uploads/medium_10MB.bin"

PIDS=(); for i in 1 2 3 4 5 6; do "$V2C" -p $P -u "$OUT/srv/file.txt" > "$OUT/u$i.log" 2>&1 & PIDS+=($!); done; wait "${PIDS[@]}"
check "6 concurrent uploads of the same name -> 6 separate files, none overwritten" \
      "[[ \$(ls $OUT/srv/uploads/ | grep -c '^file.*\.txt$') -eq 6 ]]"
ALL_SAME=1; for f in "$OUT"/srv/uploads/file*.txt; do cmp -s "$f" "$OUT/srv/file.txt" || ALL_SAME=0; done
check "all 6 uploaded copies are intact" "[[ $ALL_SAME -eq 1 ]]"
check "no leftover temp files in uploads/" "[[ -z \$(ls -A $OUT/srv/uploads | grep '^\.upload-') ]]"

"$V2C" -p $P -u ../../etc/hostname > "$OUT/c.log" 2>&1
check "upload name is reduced to its basename (no directories)" "[[ ! -e $OUT/etc ]]"

head -c $((60 * 1024 * 1024)) /dev/zero > "$OUT/srv/too_big.bin"
"$V2C" -p $P -o "$OUT/dl" too_big.bin > "$OUT/c.log" 2>&1
check "download over 50 MB limit rejected" "grep -q 'size limit' $OUT/c.log"
"$V2C" -p $P -u "$OUT/srv/too_big.bin" > "$OUT/c.log" 2>&1
check "upload over 50 MB limit rejected" "grep -q 'size limit' $OUT/c.log"

PIDS=(); for i in $(seq 1 10); do "$V2C" -p $P -o "$OUT/dl" medium_10MB.bin > "$OUT/p$i.log" 2>&1 & PIDS+=($!); done; wait "${PIDS[@]}"
check "10 simultaneous downloads all succeed" "[[ \$(cat $OUT/p*.log | grep -c 'SHA-256 verified') -eq 10 ]]"
check "server log shows pool metrics (queue length, active workers, avg service time)" \
      "grep -q 'queue length' $OUT/v2_server.log && grep -q 'active workers' $OUT/v2_server.log && grep -q 'average service time' $OUT/v2_server.log"
stop_server
check "graceful Ctrl+C prints summary" "grep -q 'Summary:' $OUT/v2_server.log"

# Max connections: 1 slot, slow requests -> second client refused.
P=19003
start_server v2_busy.log $P "$V2S" -p $P -d "$OUT/srv" -t 1 -c 1 -D 1500
"$V2C" -p $P -o "$OUT/dl" file.txt > "$OUT/b1.log" 2>&1 & B1=$!
sleep 0.3
"$V2C" -p $P -o "$OUT/dl" file.txt > "$OUT/b2.log" 2>&1
wait $B1
check "max connections: extra client gets 'server busy'" "grep -q 'server busy' $OUT/b2.log && grep -q 'SHA-256 verified' $OUT/b1.log"
stop_server

# Idle timeout: connect and send nothing.
P=19004
start_server v2_idle.log $P "$V2S" -p $P -d "$OUT/srv" -i 1
exec 3<>/dev/tcp/127.0.0.1/$P
sleep 2
exec 3>&-
check "idle client dropped after timeout" "grep -q 'idle timeout' $OUT/v2_idle.log"
stop_server

echo
if [[ $FAIL -eq 0 ]]; then
    echo "${GREEN}${BOLD}All $PASS tests passed.${RESET}  (logs in test_output/)"
else
    echo "${RED}${BOLD}$FAIL failed${RESET}, $PASS passed.  (logs in test_output/)"
    exit 1
fi
