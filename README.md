# CS-375 Lab 4: TCP File Transfer in C++

A TCP file-transfer client and server built with BSD sockets, in two versions:

| | **Version 1** (`version1/`) | **Version 2** (`version2_threadpool/`) |
|---|---|---|
| Model | Sequential: one client at a time | Thread pool: 4 workers serve clients in parallel |
| Features | Download, SHA-256 check, logging, timing, error checking | Everything in V1, plus upload, 64-bit sizes, pool metrics and resource limits |

---

## Quick Start

Requires Linux or WSL with `g++` (C++17) and `make`. No other libraries: SHA-256 is built in.

```bash
make              # build both versions
make testfiles    # create test files in server_files/ (10 MB, 100 MB, empty)
make test         # run the automated test suite (22 checks)
```

Use **two terminals**, both in the project root folder.

**Terminal A: start a server**

```bash
./version1/server -d server_files               # Version 1 (sequential)
# or
./version2_threadpool/server -d server_files    # Version 2 (thread pool)
```

**Terminal B: run clients**

```bash
./version1/client file.txt                        # download (use the client that matches the server)
./version2_threadpool/client file.txt             # download from the V2 server
./version2_threadpool/client -u some/local/file   # upload to the V2 server
```

Downloads are saved as `downloads/received_<name>`. Uploads are stored in `server_files/uploads/`.
Stop a server with **Ctrl+C**; it prints a summary.

> Both servers use port 9000 by default, so run only one at a time (or pass `-p` to choose another port).

---

## Project Structure

```
CS-375-File-Transfer/
├── Makefile                    # make | make test | make bench | make testfiles | make clean
├── common/                     # shared by both versions
│   ├── net_utils.h             # send_all()/recv_all(), byte-order helpers, logging, timing
│   ├── protocol.h              # PORT, BUFFER_SIZE, status codes, filename validation
│   └── sha256.h                # self-contained SHA-256
├── version1/                   # Part 1: single-threaded server
│   ├── server.cpp
│   └── client.cpp
├── version2_threadpool/        # Part 2: thread-pool server + Challenges 1–5
│   ├── server.cpp
│   ├── client.cpp
│   ├── threadpool.h
│   └── threadpool.cpp
├── server_files/               # files the servers serve (file.txt is included)
├── scripts/
│   ├── make_test_files.sh      # generates test files
│   ├── run_tests.sh            # automated tests
│   └── benchmark.sh            # performance experiment (V1 vs V2)
└── results/
    └── benchmark_results.md    # latest benchmark output
```

---

## Command Reference

### Version 1 server: `./version1/server [options]`

| Option | Meaning | Default |
|---|---|---|
| `-p port` | port to listen on | 9000 |
| `-d dir` | folder to serve files from | `.` |
| `-D ms` | simulated extra work per request (makes blocking easy to see) | 0 |

### Version 2 server: `./version2_threadpool/server [options]`

| Option | Meaning | Default |
|---|---|---|
| `-p port` | port to listen on | 9000 |
| `-d dir` | folder to serve files from; uploads go to `dir/uploads/` | `.` |
| `-t threads` | worker threads in the pool | 4 |
| `-m MB` | maximum file size (downloads and uploads) | 1024 |
| `-c N` | maximum concurrent connections (queued + active) | 64 |
| `-i seconds` | idle timeout: drop clients that send nothing for this long | 30 |
| `-D ms` | simulated extra work per request | 0 |

### Clients

| Command | What it does |
|---|---|
| `./version1/client [-H host] [-p port] [-o dir] [-v] <file>` | download from the V1 server |
| `./version2_threadpool/client [-H host] [-p port] [-o dir] [-v] <file>` | download from the V2 server |
| `./version2_threadpool/client [-H host] [-p port] [-v] -u <local_file>` | upload to the V2 server |

`-H` server address (default `127.0.0.1`), `-o` download folder (default `downloads`), `-v` also prints both SHA-256 values.

Each client prints one summary line:

```
[client 4242] OK download file.txt -> downloads/received_file.txt | 483 B | wait 0.2 ms | transfer 0.1 ms | total 0.9 ms | 4.6 MB/s | SHA-256 verified
```

- **wait**: time from sending the request until the server starts replying (time spent queued behind other clients)
- **transfer**: time spent moving the data and checksum

---

## Protocol

All integers are in network byte order. Every server reply starts with a 1-byte status
(`0` OK, `1` not found, `2` bad request, `3` too large, `4` server busy, `5` checksum mismatch, `6` I/O error).

```
Version 1
  C→S  [uint32 name_len][name]
  S→C  [uint8 status] then, if OK: [uint32 size][data][SHA-256 (32 bytes)]

Version 2
  S→C  [uint8 admission: OK | SERVER_BUSY]
  C→S  [uint8 'D' | 'U'][uint32 name_len][name]
  'D'  S→C [uint8 status] then, if OK: [uint64 size][data][SHA-256]
  'U'  C→S [uint64 size]; S→C [uint8 status]; C→S [data][SHA-256]; S→C [uint8 status][uint32 len][stored name]
```

---

## Testing and Benchmarking

```bash
make test     # 22 automated checks: downloads, uploads, checksums, errors, path traversal,
              # concurrent clients, size and connection limits, idle timeout
make bench    # V1 vs V2 with 10 simultaneous clients: time, CPU, memory (takes ~1–2 minutes)
./scripts/benchmark.sh -h                             # options: clients, threads, file, delay
./scripts/benchmark.sh -f large_100MB.bin -t 8 -r 1   # example: one custom scenario
```

Benchmark results are written to [`results/benchmark_results.md`](results/benchmark_results.md).
Test logs go to `test_output/`.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `bind: Address already in use` | Another server is running on that port. Stop it (Ctrl+C) or use `-p 9001` on both server and client. |
| `Connection refused. Is the server running?` | Start the server first, and check that server and client use the same port. |
| `file not found on server` | Check the name exists in the folder given to `-d` (e.g. `ls server_files`). Run `make testfiles` for the `.bin` files. |
| `bad request (invalid or unsafe filename)` | Only plain file names are allowed: no `/`, `..` or leading `.`. |
| V1 client talking to V2 server (or vice versa) hangs or errors | The protocols differ. Use the client from the same folder as the server. |
| `make: g++: not found` | `sudo apt install build-essential` |

`make clean` removes binaries, downloads, uploads and test output.
