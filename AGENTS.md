# Project working instructions

- Deliver complete user journeys; do not stop at design, compilation, or unit tests. Report meaningful milestones and consequential blockers rather than routine chatter.
- Keep generated files under the repository's `.cache/` and `.temp/` directories. Never commit credentials, daemon state, database connection files, build outputs, or machine-local paths.
- Read the relevant document in `docs/` before substantial design/investigation. Extend existing shared knowledge in English, including assumptions, actual measurements and reproducible commands. Do not turn a lack of public sources into a rejection of supplied context.
- Preserve external protocol and CLI contracts. Add compatible fields/options or version the protocol deliberately; do not silently change existing frame meanings.
- C++23 and operating-system sockets; no third-party networking frameworks. Mature TUI and other non-network components are permitted and preferred when useful, integrated from pinned source archives through CMake FetchContent. libpq is the system database client, not a networking framework replacement. Preserve existing on-wire compression compatibility.
- Keep socket ownership in the reactor, SQL/domain work in the global bounded worker pool, per-session FIFO and per-room commit/publication ordering intact. Never publish success before PostgreSQL commit.
- Tenant identity comes from the authenticated session. Never trust a tenant/user supplied in an ordinary command or weaken token/IPC state protection for a test.
- Use native platform facilities, concise cohesive functions and explicit invariants. Comments explain constraints and trade-offs, not paraphrases. Follow `.clang-format`; write source comments in English.
- Scope tests around real server-daemon-TUI behavior, including terminal restoration, disconnects, tenancy, metadata and persistence. Use seeded property tests for byte-stream, UTF-8, cache and cursor invariants. Do not weaken tests to hide a concrete user-visible failure.
- Operational/development tooling is Python standard library. CMake is target-based with subdirectories. Keep Windows, Linux epoll, POSIX poll and macOS compatibility in mind.
- Default listeners remain loopback. No home-grown cryptography. This version is plaintext; use SSH tunneling for remote use and do not claim public-Internet readiness.

## Fast checks

```console
python scripts/dev.py build --portable
python scripts/dev.py test --portable
```

For PostgreSQL build without `--portable`; for database persistence/recovery tests,
follow `README.md` using a fresh dedicated test database. Never run the destructive
connection-termination recovery test against unrelated application sessions.

For POSIX sanitizer and poll validation:

```console
cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize
cmake -S . -B .cache/build/poll -DCHAT_WITH_POSTGRES=OFF -DCHAT_FORCE_POLL=ON
cmake --build .cache/build/poll --parallel
ctest --test-dir .cache/build/poll --output-on-failure
```
