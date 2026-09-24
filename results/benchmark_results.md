# Benchmark results

- Date: 2026-09-24 17:09
- Machine: 16 CPU cores, Linux 6.18.33.2-microsoft-standard-WSL2
- 10 simultaneous clients per run, V2 pool size 4, each value is the mean of 3 runs
- Command: `./scripts/benchmark.sh -n 10 -t 4 -r 3`

| Scenario | Server | Total time (s) | Avg client latency (ms) | Max client latency (ms) | Server CPU time (s) | Server CPU usage | Peak memory (MB) | Threads | Successful clients |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| file.txt | V1 sequential | 0.016 | 2.5 | 4.6 | 0.00 | 0% | 4.0 | 1 | 10/10 |
| file.txt | V2 pool (4 threads) | 0.008 | 1.4 | 2.1 | 0.00 | 0% | 4.1 | 5 | 10/10 |
| medium_10MB.bin | V1 sequential | 1.164 | 473.1 | 1158.9 | 0.47 | 40% | 4.0 | 1 | 10/10 |
| medium_10MB.bin | V2 pool (4 threads) | 0.163 | 103.4 | 157.7 | 0.53 | 324% | 4.2 | 5 | 10/10 |
| large_100MB.bin | V1 sequential | 5.068 | 2729.2 | 5061.6 | 4.58 | 90% | 4.0 | 1 | 10/10 |
| large_100MB.bin | V2 pool (4 threads) | 1.887 | 1171.9 | 1881.3 | 5.64 | 299% | 4.2 | 5 | 10/10 |
| file.txt + 200 ms delay | V1 sequential | 2.012 | 1103.2 | 2004.7 | 0.00 | 0% | 4.0 | 1 | 10/10 |
| file.txt + 200 ms delay | V2 pool (4 threads) | 0.607 | 362.1 | 601.7 | 0.00 | 0% | 4.2 | 5 | 10/10 |
