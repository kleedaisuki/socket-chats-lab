# SCL1 compression and framing

The 20-byte header uses network byte order, magic `SCL1`, version 1, operation byte, 16-bit flags, 32-bit request ID, 32-bit wire length and 32-bit raw length. Only flag bit 0 is defined. Both lengths are at most 1 MiB. Raw length is at least two bytes. Uncompressed lengths must match. Unknown operation bytes and reserved flags are rejected.

Raw data is a BE16 field count (at most 128), followed by repeated BE32 byte lengths and strictly valid UTF-8 strings. Embedded NUL is codec-valid; application validation rejects it where necessary. Trailing data is invalid. An incremental decoder buffers only one bounded frame, and processes arbitrary fragmented or coalesced streams.

## Native LZ token format

With flag bit 0 set, the wire payload is a sequence of these tokens:

| First byte | Meaning | Following bytes |
| --- | --- | --- |
| 0..127 | Literal length = token + 1 | That many literal bytes |
| 128..255 | Match length = (token & 127) + 3 | BE16 backward distance |

Backward distance must be 1..65535 and cannot exceed bytes already decoded. Match copying proceeds byte by byte, so overlap is allowed. A repeated byte can therefore be encoded using distance 1. The decoded size must equal the declared raw length exactly; overflow, truncated tokens, impossible distances and trailing decoded bytes are rejected. The format is deterministic to decode, but different encoders may choose different matches.

The encoder uses a fixed 65536-entry hash table holding the last position of each three-byte hash, greedily matching up to 130 bytes. This bounds work and memory without an attacker-controlled hash chain. It emits compression only when smaller than raw, avoiding overhead for short or incompressible messages. Hello and welcome are never compressed, and compressed hello/welcome frames are rejected. Application code must keep secrets out of all other operations; this is not encryption or a claim of resistance to cross-message compression side channels.

`receive_frame` asks the decoder for the exact number of bytes needed for its next header/payload boundary. It never overreads a subsequent frame, avoiding a hidden per-socket frame queue while keeping syscalls proportional to chunks rather than bytes.

## Terminal policy

UTF-8 decoding rejects overlong encodings, surrogates and code points above U+10FFFF. Display sanitization replaces malformed bytes with U+FFFD; C0/C1 controls, DEL, line/paragraph separators and bidi embedding/override/isolate controls become spaces. Thus untrusted text cannot introduce ANSI escapes or forge extra terminal lines. Combining marks and ZWJ have zero best-effort width; CJK and common emoji are treated as two cells. This is intentionally not a grapheme shaping engine.

## Verification record

Core tests exercise 600 deterministic randomized UTF-8 field vectors, both compressed and uncompressed, with arbitrary incremental fragmentation; coalesced frames; exact 1 MiB raw payload boundaries; 1000 mutated streams; malformed lengths, flags, UTF-8 and LZ tokens; terminal control neutralization; loopback TCP readiness; timeout; and sequential receive of coalesced frames.

2026-10-19: passed native Windows MinGW GCC 16 (WSAPoll), Ubuntu WSL GCC 13 (epoll), Ubuntu WSL GCC 13 (`CHAT_FORCE_POLL`) and GCC 13 AddressSanitizer + UndefinedBehaviorSanitizer. Commands compile `src/core/{protocol,utf8,net}.cpp` and `tests/core_tests.cpp` using C++23, `-Iinclude`, Windows `-lws2_32` or POSIX `-pthread`; sanitizer build adds `-fsanitize=address,undefined -fno-omit-frame-pointer`. All generated binaries remain under `.cache/`. These are core subsystem checks; repository `tests/journey.py` supplies application end-to-end coverage separately.
