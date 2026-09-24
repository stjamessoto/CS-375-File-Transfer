#!/usr/bin/env bash
# Creates sample files in server_files/ for testing and benchmarking.
#   empty.txt        0 bytes   (checks that an empty file is not mistaken for "not found")
#   medium_10MB.bin  10 MB of random data
#   large_100MB.bin  100 MB of random data
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p server_files

make_file() {  # name size_mb
    if [[ -f "server_files/$1" ]]; then
        echo "  exists   server_files/$1"
    else
        head -c "$(( $2 * 1024 * 1024 ))" /dev/urandom > "server_files/$1"
        echo "  created  server_files/$1 ($2 MB)"
    fi
}

echo "Creating test files..."
: > server_files/empty.txt && echo "  created  server_files/empty.txt (0 bytes)"
make_file medium_10MB.bin 10
make_file large_100MB.bin 100
echo "Done. file.txt is already included in the repository."
