# FM-1 development
Work with Elias in Swedish. This is a Dinghy-based firmware for the M-VAVE FM-1.
The hardware has not arrived: distinguish host tests, emulator tests, and real-device tests.

## Build and test
- Fast host test: ./tests/run_host.sh
- Host output: build/host/dinghy.wav and build/host/*.png
- Firmware build: ./build.sh
- Firmware output: build/dinghy.fwsc
- Convenience wrapper: ./fm1 test | build | play | codex | claude | github
- The setup script's pinned SDK: ~/fw-AC79_AIoT_SDK
- The default toolchain: ~/.jieli/toolchain
- Read docs/API.md and docs/RULES.md before changing DSP or platform code.
- Run the host test after app changes. Run the firmware build before calling a change ready.
- Report commands actually run and what remains untested. Never infer hardware performance
  from host speed; emulator operation does not prove hardware timing.

## Code and hardware
- Start in firmware/src/dinghy.c; keep the inherited hardware platform unless a change requires it.
- Follow Dinghy's integer DSP conventions and existing compiler/linker flags.
- Keep flash writes, screen drawing, allocation and waits out of app_audio().
- Keep platform_poll(), the updater, watchdog handling and USB rescue working.
- Never flash attached hardware as part of a test/build command.
- Keep vendor SDKs, toolchains, logs, generated audio/screens and emulator flash out of Git.
- Preserve the existing licence and credits.
- One writing agent at a time in this checkout; use separate worktrees for concurrent edits.
- Read docs/vision.md and docs/status.md at the start of a task; update status after meaningful work.
