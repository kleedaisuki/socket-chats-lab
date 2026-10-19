# Interactive TCP tuning investigation (2026-10-19)

## Question and workload

Does Nagle/delayed-ACK interaction explain approximately 17 ms send-to-ACK latency in the Windows local 16-client fanout smoke workload? Isolated ping and one-client send latency are sub-millisecond, so the hypothesis requires a workload-dependent interaction rather than a generic receive timeout.

`python scripts/benchmark.py --server PATH --clients 16 --messages 100 --bytes 256 --output .cache/RESULT.json`

Environment: Windows 11 build 26200, Python 3.14.6, MinGW GCC 16 C++23 Debug (`-g`, no optimization), memory backend, four server workers, 16 Python threaded clients authenticated as the same tenant/user, all receiving tenant-wide broadcasts, one outstanding send per client, 1600 messages. Each message is 256 UTF-8 bytes, mostly repeated `x` with a Chinese/emoji prefix. This is deliberately compressible and has substantial fanout. It is not a capacity measurement or a realistic text corpus. The Python peer decodes compressed matches byte-by-byte, so client CPU/GIL effects remain part of measured end-to-end latency.

## Experiment and controls

The experimental patch enabled TCP_NODELAY on successful native outgoing connects and accepted server connections. It did not alter listening socket configuration or OS-wide settings. The existing server reactor-wakeup socket already uses TCP_NODELAY and was left unchanged. Application frames were already encoded in contiguous buffers, not sent one field at a time.

Three alternating runs compared a preserved earlier binary to the experimental current binary. Because a subsequent current-source revert did not return to the earlier binary's performance, the preserved binary is **not a valid isolated TCP tuning control**. Its results are retained to expose, rather than obscure, the confound:

| Binary/configuration | Run | Send ACK p50 ms | p95 ms | ACK/s |
| --- | ---: | ---: | ---: | ---: |
| Earlier preserved binary | 1 | 17.369 | 20.481 | 895.3 |
| Current TCP_NODELAY | 1 | 25.121 | 29.834 | 620.3 |
| Earlier preserved binary | 2 | 16.651 | 18.833 | 947.4 |
| Current TCP_NODELAY | 2 | 24.549 | 28.487 | 646.9 |
| Earlier preserved binary | 3 | 16.858 | 20.244 | 925.6 |
| Current TCP_NODELAY | 3 | 24.638 | 27.180 | 647.2 |
| Same current source, tuning reverted | 1 | 24.945 | 27.962 | 642.4 |
| Same current source, tuning reverted | 2 | 24.347 | 29.805 | 638.4 |
| Same current source, tuning reverted | 3 | 24.417 | 26.887 | 649.9 |

Raw JSON files are `.cache/benchmark-{nagle,nodelay,reverted}-{1,2,3}.json`; experimental binary was preserved as `.cache/benchmark-bin/chat-server-nodelay.exe`. These are generated local artifacts, not durable repository inputs. The table and exact methodology are the durable record.

## Decision and interpretation

**Do not retain broad TCP_NODELAY tuning.** With current sources it does not produce a useful, reproducible latency improvement. Differences among the current-source groups are small relative to the earlier/current binary gap, and are not evidence that TCP_NODELAY caused that gap. The added helper, declaration, automatic connect configuration and accepted-socket call were removed. Existing narrow reactor-wakeup tuning remains.

The original Nagle explanation is not supported by this experiment. That does not prove Nagle is irrelevant for every network/traffic pattern. Conversely, Microsoft's small-send examples are mechanisms, not evidence that this workload exhibits them. TCP_NODELAY can reduce intentional send coalescing, increasing packet/header and syscall processing pressure for chat fanout. Neither packet count nor actual on-wire bytes was captured here, so no measured bandwidth impact is claimed. Keep application-level compression (only when smaller) and contiguous-frame writes; do not disable kernel coalescing by habit.

## Useful next investigation

1. Server `enqueue` currently encodes a broadcast separately for every recipient. The native LZ encoder initializes a 65,536-entry uint32 table on each invocation, even for tiny frames. Encode a tenant event once, then reuse immutable encoded bytes across recipients; preserve independent per-socket offsets/backpressure. Measure this before more socket tuning.
2. Profile native server and Python peer CPU separately. A process-per-client harness or compiled native driver discriminates Python GIL/decompression overhead from server work.
3. Match source/build identity before comparing historical binaries; reproduce the earlier/current gap independently. Do not label it a network regression from these measurements.
4. If socket tuning is revisited, add packet tracing and a non-loopback latency-controlled test. Measure packet count, wire bytes, client CPU, native CPU and p50/p95/p99, not only ACK latency.

## Mechanism references

- [Microsoft: TCP/IP Characteristics](https://learn.microsoft.com/en-us/windows/win32/winsock/tcp-ip-characteristics-2): Nagle and delayed ACK can interact with small sends; timing examples are platform/context dependent.
- [Microsoft: TCP/IP-specific Issues](https://learn.microsoft.com/en-us/windows/win32/winsock/tcp-ip-specific-issues-2): send-send-receive patterns can expose delayed acknowledgment interactions.
- [POSIX netinet/tcp.h](https://www.man7.org/linux/man-pages/man0/netinet_tcp.h.0p.html): TCP_NODELAY avoids small-segment coalescing.
