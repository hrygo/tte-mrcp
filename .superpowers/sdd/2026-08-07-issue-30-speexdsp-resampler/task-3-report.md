# Task 3 report — Linux SpeexDSP package and verification

## Result and scope

Task 3 implementation is complete on `fix/issue-30-speexdsp-resampler`, with
the Linux x86_64/aarch64 artifact and GitHub Actions evidence intentionally
pending a PR run.

Changed only:

- `.github/workflows/build-linux.yml`
- this Task 3 report
- the Task 3 checkboxes/evidence in
  `docs/superpowers/plans/2026-08-07-issue-30-speexdsp-resampler.md`

The approved design spec was not changed because the workflow implementation
matches its SpeexDSP version, dynamic-linking, soname, ABI and test-gate
requirements. No files under `plugins/tts-websocket/src` or `tests` were
modified. The deleted `2026-08-07 09_47_13.zip` and `问题音频.wav` were not
restored or staged, and no `71934e1` files were reintroduced.

## RED gate

The required pre-edit assertions both returned exit code `1` with no output:

```text
rtk rg -n 'install_speexdsp|SpeexDSP-1.2.1|1b28a0f61bc31162979e1f26f3981fc3637095c8' .github/workflows/build-linux.yml
  exit 1

rtk rg -n 'test_tts_websocket_resampler' .github/workflows/build-linux.yml
  exit 1
```

This records that the workflow initially had neither the pinned SpeexDSP
installation nor the new resampler test gate.

## Workflow implementation

`build-linux.yml` now:

1. Builds the exact `SpeexDSP-1.2.1` tag, verifies commit
   `1b28a0f61bc31162979e1f26f3981fc3637095c8`, configures
   `--disable-static --enable-shared` with prefix `/opt/tte-mrcp`, and checks
   `pkg-config --modversion speexdsp = 1.2.1`.
2. Runs the existing x86_64 CentOS 7/GCC 4.8.5 and native aarch64/Rocky Linux
   matrix unchanged; the existing `glibc_max` values remain `2.17` and `2.28`.
3. Runs the TTS Autotools `make check` gate on both rows. The aarch64 CMake
   gate builds and runs all six TTS tests, including
   `test_tts_websocket_resampler` and `test_tts_websocket_stream_pipeline`.
4. Stages the `libspeexdsp.so` → `libspeexdsp.so.1` → versioned library chain,
   verifies the staged `tts_websocket.so` resolves `libspeexdsp.so.1` through
   the staged `LD_LIBRARY_PATH`, and keeps `audit_elf` machine/GLIBC checks
   over the staged ELF files.
5. Uses `zip -yr` so the soname symlinks remain symlinks in the ZIP, then
   asserts the unversioned link, soname link and versioned library are all
   archive entries.

## Verification evidence

All commands below were run with the required `rtk` prefix.

| Gate | Result |
|---|---|
| `rtk ruby -e 'require "yaml"; YAML.load_file(".github/workflows/build-linux.yml"); puts "workflow yaml ok"'` | exit `0`, `workflow yaml ok` |
| Extracted target container script piped to `rtk bash -n` | exit `0` |
| `rtk git diff --check` | exit `0` |
| `rtk env PKG_CONFIG_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib/pkgconfig pkg-config --modversion speexdsp` | exit `0`, version `1.2.1` |
| `rtk env PKG_CONFIG_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib/pkgconfig ./configure --help` | exit `0` |
| CMake configure with local APR/APR-util/Sofia plus SpeexDSP pkg-config paths | exit `0`; found Sofia `1.13.17` and SpeexDSP `1.2.1` |
| CMake build of six TTS test targets in `/tmp/tte-mrcp-issue30-final` | exit `0`; all six targets built |
| `rtk env LD_LIBRARY_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib ctest --test-dir /tmp/tte-mrcp-issue30-final --output-on-failure -R '^tts_websocket_'` | exit `0`; `100% tests passed out of 6` |
| Pin/target/ABI `rg` inspection of `.github/workflows/build-linux.yml` | exit `0`; required pins, tests, `glibc_max`, `require_gcc_485`, soname and audit assertions present |

The first local CMake attempt used only the SpeexDSP pkg-config path and
failed because the host environment did not expose `sofia-sip-ua`; that is a
host dependency-path issue, not a SpeexDSP or workflow failure. The clean
rerun used the existing Homebrew APR/APR-util/Sofia pkg-config paths plus the
fixed local SpeexDSP path and passed.

## Deferred evidence and boundaries

- GitHub Actions has not been run in this turn. Current-run RHEL 7 x86_64 and
  Kylin V10 aarch64 package contents, plugin loading, ELF audits, Autotools
  gates and CMake/CTest target-ISA evidence remain pending the PR run.
- No Linux cross-build or live MRCP/RTP/TTS service call was claimed from the
  macOS local evidence.
- The local Autotools build/test was not run because no local target-ABI
  `build/ci` tree exists; the workflow gate is present for both matrix rows.
- Codebase-memory MCP indexing was not run; this Task3 change is confined to
  workflow and evidence documents, and no source discovery claim relies on a
  graph result.
