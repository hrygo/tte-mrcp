# Issue #30 SpeexDSP Resampler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the TTS plugin's three-sample 24 kHz→8 kHz averaging path with a tested, stateful SpeexDSP quality-10 resampler and ship its runtime library in both supported Linux artifacts.

**Architecture:** A new APR-independent adapter owns one `SpeexResamplerState` per SPEAK and exposes bounded process/finish operations over mono signed-16 PCM. The WebSocket worker keeps byte alignment, passes complete samples through the adapter, converts the 8 kHz output to PCMU, and writes the existing ring buffer; the RTP state machine remains unchanged. CI builds a pinned SpeexDSP release into `/opt/tte-mrcp`, then bundles and audits the shared library with the existing x86_64 and aarch64 packages.

**Tech Stack:** C89/C99-compatible C, SpeexDSP 1.2.1, APR/UniMRCP, G.711 PCMU, Autotools/Libtool, CMake/pkg-config, GitHub Actions Docker builds.

## Global Constraints

- All agent-issued shell commands must use the `rtk` prefix.
- Work in `/Users/huangzhonghui/tte-mrcp` on `fix/issue-30-speexdsp-resampler`; never commit or push `main`.
- Preserve the untracked `2026-08-07 09_47_13.zip` and `问题音频.wav`; never stage them.
- SpeexDSP is exactly release `SpeexDSP-1.2.1`, commit `1b28a0f61bc31162979e1f26f3981fc3637095c8`.
- The production resampler is mono PCM S16LE, 24000 Hz input, 8000 Hz output, `quality=10`, using `speex_resampler_process_int()`.
- Every SPEAK owns independent resampler state; normal `session.done` drains the filter, while STOP/error cleanup emits no tail audio and destroys state only after the worker stops.
- Total normal output is exactly `floor(total_input_samples / 3)`; arbitrary byte/message partitioning must not change output samples.
- Initialization or processing failures are visible stream errors; never fall back to the three-point averaging algorithm.
- Do not change WebSocket protocol messages, PCMU encoding, ring-buffer semantics, MPF/RTP packetization, or SPEAK-COMPLETE state transitions except where resampler EOF ordering requires it.
- Deployment gates are RHEL 7.9-compatible x86_64 (`glibc <= 2.17`, GCC/G++ 4.8.5) and Kylin V10-compatible aarch64 (`glibc <= 2.28`, native ARM runner).
- macOS and Windows are not release gates for this task and must be reported as unverified; do not claim cross-platform completion from Linux results.
- Follow TDD for production behavior: record the focused RED command/output before implementation and the GREEN command/output afterward in each task report.
- Each commit is single-purpose Conventional Commits and includes `Refs #30` in its body/footer.

---

### Task 1: Add the stateful SpeexDSP resampler adapter

**Files:**
- Create: `plugins/tts-websocket/src/tts_websocket_resampler.h`
- Create: `plugins/tts-websocket/src/tts_websocket_resampler.c`
- Create: `plugins/tts-websocket/tests/test_tts_websocket_resampler.c`
- Modify: `configure.ac`
- Modify: `plugins/tts-websocket/Makefile.am`
- Modify: `plugins/tts-websocket/CMakeLists.txt`
- Regenerate: `configure`
- Regenerate: `plugins/tts-websocket/Makefile.in`

**Interfaces:**
- Consumes: SpeexDSP `SpeexResamplerState`, `speex_resampler_init()`, `speex_resampler_process_int()`, `speex_resampler_get_input_latency()`, `speex_resampler_skip_zeros()`, and `speex_resampler_destroy()`.
- Produces: opaque `tts_websocket_resampler_t` and the exact create/process/finish/output-bound/destroy functions declared in the approved design.
- Error contract: `0` means success; non-zero adapter results preserve the underlying SpeexDSP error code where available. `finish` is idempotent only after a successful first finish and produces zero samples on later calls.

- [ ] **Step 1: Install a pinned local SpeexDSP build if pkg-config cannot already resolve 1.2.1**

Use a temporary source directory and the fixed prefix `/tmp/tte-mrcp-speexdsp-1.2.1`. Verify both the tag commit and pkg-config version:

```bash
rtk pkg-config --atleast-version=1.2.1 speexdsp
rtk git clone --branch SpeexDSP-1.2.1 https://github.com/xiph/speexdsp.git /tmp/tte-mrcp-speexdsp-src
rtk git -C /tmp/tte-mrcp-speexdsp-src rev-parse HEAD
rtk sh -c 'cd /tmp/tte-mrcp-speexdsp-src && ./autogen.sh'
rtk sh -c 'cd /tmp/tte-mrcp-speexdsp-src && ./configure --prefix=/tmp/tte-mrcp-speexdsp-1.2.1'
rtk make -C /tmp/tte-mrcp-speexdsp-src -j4
rtk make -C /tmp/tte-mrcp-speexdsp-src install
rtk env PKG_CONFIG_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib/pkgconfig pkg-config --modversion speexdsp
```

Expected commit: `1b28a0f61bc31162979e1f26f3981fc3637095c8`; expected version: `1.2.1`. Skip clone/build commands when the first command succeeds with a compatible installed package.

- [ ] **Step 2: Write the failing adapter tests**

Create `test_tts_websocket_resampler.c` with real-signal tests that call the public adapter API:

```c
static int test_exact_length_and_chunk_invariance(void);
static int test_passband_300hz_1khz_3400hz(void);
static int test_stopband_6khz_10khz(void);
static int test_short_and_non_multiple_lengths(void);
static int test_arbitrary_byte_partitions_match_contiguous_input(void);
static int test_finish_preserves_nonzero_tail(void);
static int test_finish_is_idempotent(void);
static int test_state_isolation_between_sessions(void);
static int test_invalid_arguments_fail(void);
```

Generate sine samples with a deterministic phase accumulator. Compare contiguous input against fixed-seed sample chunks `{1, 7, 31, 160, 511}`. For byte-oriented coverage, use `tts_websocket_pcm_accumulate(..., alignment=2)` and feed every possible split over the first 64 byte positions, one-byte chunks, and a fixed-seed random partition. Add a final non-zero impulse inside the input-latency window. Assert exact output length `input_samples / 3`, sample-for-sample partition equality, preserved non-zero tail energy, passband amplitude loss no worse than 1 dB at 300/1000/3400 Hz, and aliased output RMS at least 60 dB below input RMS for 6/10 kHz. Print one named failure and return non-zero on the first failed assertion. Link `src/tts_websocket_pcm.c` into this test executable for the byte accumulator.

- [ ] **Step 3: Run the focused test and capture RED**

```bash
rtk env PKG_CONFIG_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib/pkgconfig cmake -S . -B /tmp/tte-mrcp-issue30-red -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_TTS_WEBSOCKET_TESTS=ON
rtk cmake --build /tmp/tte-mrcp-issue30-red --target test_tts_websocket_resampler -j4
```

Expected: configure or build fails because `test_tts_websocket_resampler` and the adapter source/interface do not exist yet. Record that exact expected failure in the task report.

- [ ] **Step 4: Implement the minimal adapter**

Declare the opaque type and exact signatures:

```c
typedef struct tts_websocket_resampler_t tts_websocket_resampler_t;

tts_websocket_resampler_t *tts_websocket_resampler_create(
    unsigned int input_rate, unsigned int output_rate,
    unsigned int channels, int quality, int *error_code);
int tts_websocket_resampler_process(
    tts_websocket_resampler_t *resampler,
    const short *input, size_t *input_samples,
    short *output, size_t *output_samples);
int tts_websocket_resampler_finish(
    tts_websocket_resampler_t *resampler,
    short *output, size_t *output_samples);
size_t tts_websocket_resampler_output_bound(
    const tts_websocket_resampler_t *resampler,
    size_t input_samples);
void tts_websocket_resampler_destroy(
    tts_websocket_resampler_t *resampler);
```

The private struct stores the Speex state, rates, cumulative input/output counts, output target, input latency, finished flag, and a finite zero-padding buffer. Call `speex_resampler_skip_zeros()` after initialization. `process` loops until every supplied input sample is consumed or the caller's output capacity is exhausted, updates both in/out lengths to actual counts, and rejects calls after finish. `finish` feeds only enough zeros to recover delayed real samples, caps emitted output at `floor(total_input * output_rate / input_rate) - total_output`, and then marks the object finished. Use `calloc/free`; the adapter remains APR-independent.

- [ ] **Step 5: Wire both build systems to SpeexDSP and the new test**

In `configure.ac`, after `UNI_PLUGIN_ENABLED(tts_websocket)`, require the package only when the plugin is enabled:

```m4
AS_IF([test "x${enable_tts_websocket_plugin}" = "xyes"], [
    PKG_CHECK_MODULES([SPEEXDSP], [speexdsp >= 1.2.1])
])
AC_SUBST([SPEEXDSP_CFLAGS])
AC_SUBST([SPEEXDSP_LIBS])
```

Add `src/tts_websocket_resampler.c` to `tts_websocket_la_SOURCES`; add `$(SPEEXDSP_CFLAGS)` to plugin CPPFLAGS and `$(SPEEXDSP_LIBS)` to plugin LIBADD. Register the Autotools test explicitly:

```make
check_PROGRAMS = test_tts_websocket_resampler
TESTS = $(check_PROGRAMS)
test_tts_websocket_resampler_SOURCES = \
    tests/test_tts_websocket_resampler.c \
    src/tts_websocket_resampler.c \
    src/tts_websocket_pcm.c
test_tts_websocket_resampler_CPPFLAGS = $(AM_CPPFLAGS) $(SPEEXDSP_CFLAGS)
test_tts_websocket_resampler_LDADD = $(SPEEXDSP_LIBS) -lm
```

In CMake, run `pkg_check_modules(SPEEXDSP REQUIRED speexdsp>=1.2.1)` when the TTS plugin is configured, add include/library directories, link the plugin and `test_tts_websocket_resampler`, and register CTest name `tts_websocket_resampler`.

- [ ] **Step 6: Regenerate Autotools files and verify GREEN**

```bash
rtk ./bootstrap
rtk env PKG_CONFIG_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib/pkgconfig cmake -S . -B /tmp/tte-mrcp-issue30-task1 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_TTS_WEBSOCKET_TESTS=ON
rtk cmake --build /tmp/tte-mrcp-issue30-task1 --target test_tts_websocket_resampler -j4
rtk env LD_LIBRARY_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib ctest --test-dir /tmp/tte-mrcp-issue30-task1 --output-on-failure -R '^tts_websocket_resampler$'
```

Expected: the new test passes with pristine output. Run the existing TTS plugin unit targets once and record all results.

- [ ] **Step 7: Commit Task 1**

Stage only the adapter, its test, and build-system files, verify the staged diff, then commit:

```bash
rtk git commit -m "feat(tts): add SpeexDSP stream resampler" -m "Refs #30"
```

### Task 2: Replace the engine's three-point averaging path

**Files:**
- Modify: `plugins/tts-websocket/src/tts_websocket_engine.c`
- Modify: `plugins/tts-websocket/tests/test_tts_websocket_resampler.c`

**Interfaces:**
- Consumes: Task 1's `tts_websocket_resampler_*` interface and existing `tts_websocket_pcm_accumulate()`.
- Produces: WebSocket worker flow `PCM bytes → 16-bit alignment → SpeexDSP → PCMU → ring`; no callable legacy `resample_pcm_to_8k()` remains.
- Lifecycle: the worker is the sole process/finish user; cleanup joins the worker before any remaining adapter destruction.

- [ ] **Step 1: Establish the engine-integration RED gate**

Task 1 already created the behavior tests before this production integration. Confirm they are green, then run a source-level gate that requires the old engine path to be absent:

```bash
rtk env LD_LIBRARY_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib ctest --test-dir /tmp/tte-mrcp-issue30-task1 --output-on-failure -R '^tts_websocket_resampler$'
rtk sh -c '! rg -n "resample_pcm_to_8k|每3个采样点|alignment.*6" plugins/tts-websocket/src/tts_websocket_engine.c'
```

Expected: the behavior test passes, while the source-level gate fails because the old three-point engine integration is still active. This is the RED evidence for wiring the already-tested component into production.

- [ ] **Step 2: Integrate the adapter into the WebSocket worker**

Make these bounded changes in `tts_websocket_engine.c`:

```c
#include "tts_websocket_resampler.h"
```

- Create one 24000→8000, mono, quality-10 adapter before binary audio processing starts; fail the stream visibly if creation fails.
- Preserve only incomplete 16-bit samples across WebSocket messages by calling `tts_websocket_pcm_accumulate(..., alignment=2)`; rename comments and bounds from 6-byte decimation groups to 2-byte PCM samples.
- Allocate output from `tts_websocket_resampler_output_bound()`, call process until input is fully consumed, then pass only produced 8 kHz samples to `convert_16bit_to_ulaw()` and the existing ring writer.
- On normal `session.done`, process any complete final sample, discard at most one orphan byte with an explicit count, call finish, encode/write its produced tail, and only then set `stream_complete`.
- On STOP, socket error, timeout or cleanup, skip finish and destroy the adapter after the worker exits.
- Remove `resample_pcm_to_8k()` and its forward declaration. Remove `resample_ulaw_to_8k()` if no active caller remains; confirm with a repository search before deletion.
- Add session summary counters for resampler input samples, output samples, process calls, finish samples and error code. Do not add per-frame INFO logs or PCM dumps.

- [ ] **Step 3: Verify GREEN and regression tests**

```bash
rtk cmake --build /tmp/tte-mrcp-issue30-task1 --target test_tts_websocket_resampler test_tts_websocket_pcm test_tts_websocket_lifecycle test_tts_websocket_ws test_tts_websocket_http_parse -j4
rtk env LD_LIBRARY_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib ctest --test-dir /tmp/tte-mrcp-issue30-task1 --output-on-failure -R '^tts_websocket_'
rtk rg -n 'resample_pcm_to_8k|resample_ulaw_to_8k|每3个采样点|alignment.*6' plugins/tts-websocket/src plugins/tts-websocket/tests
```

Expected: all TTS tests pass; the search returns no active legacy implementation or stale production comment. Historical design documents are outside this search scope.

- [ ] **Step 4: Commit Task 2**

```bash
rtk git commit -m "fix(tts): use SpeexDSP for streamed PCM" -m "Refs #30"
```

### Task 3: Package and verify SpeexDSP on both Linux architectures

**Files:**
- Modify: `.github/workflows/build-linux.yml`
- Modify: `docs/superpowers/specs/2026-08-07-issue-30-speexdsp-resampler-design.md` only if implementation facts differ from the approved design
- Modify: `docs/superpowers/plans/2026-08-07-issue-30-speexdsp-resampler.md` to mark completed checkboxes and record executed evidence

**Interfaces:**
- Consumes: Task 1's pkg-config requirement and Task 2's dynamically linked `tts_websocket.so`.
- Produces: both Linux ZIP artifacts containing a compatible SpeexDSP soname chain and a CI gate that executes the new resampler tests on target ISA/ABI.

- [x] **Step 1: Establish the failing CI assertions**

Run these before editing the workflow and record their expected non-zero results:

```bash
rtk rg -n 'install_speexdsp|SpeexDSP-1.2.1|1b28a0f61bc31162979e1f26f3981fc3637095c8' .github/workflows/build-linux.yml
rtk rg -n 'test_tts_websocket_resampler' .github/workflows/build-linux.yml
```

Expected: neither pinned dependency installation nor the target test exists yet.

- [x] **Step 2: Add pinned SpeexDSP installation**

Add an `install_speexdsp()` function next to `install_sofia_sip()`:

```bash
install_speexdsp() {
  local version=1.2.1
  local commit=1b28a0f61bc31162979e1f26f3981fc3637095c8
  local prefix=/opt/tte-mrcp

  git clone --branch "SpeexDSP-$version" https://github.com/xiph/speexdsp.git /tmp/speexdsp
  test "$(cd /tmp/speexdsp && git rev-parse HEAD)" = "$commit"
  cd /tmp/speexdsp
  ./autogen.sh
  ./configure --prefix="$prefix" --disable-static --enable-shared
  make -j"$(getconf _NPROCESSORS_ONLN)"
  make install
  export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  test "$(pkg-config --modversion speexdsp)" = "$version"
}
```

Invoke it after `install_sofia_sip` and before configuring TTE-MRCP. Preserve GCC 4.8.5 on x86_64 and native aarch64 execution.

- [x] **Step 3: Extend unit, packaging and runtime gates**

- Add `test_tts_websocket_resampler` to the CMake build target list and CTest regex coverage.
- Add an Autotools `make check` invocation for the TTS plugin so x86_64 also executes its tests.
- After bundling, assert `tts_websocket.so` resolves SpeexDSP from the staged package library under the runtime `LD_LIBRARY_PATH`.
- Assert the ZIP contains `libspeexdsp.so` and its soname symlink.
- Keep `audit_elf` responsible for both machine type and GLIBC ceiling on the bundled library.

- [x] **Step 4: Run local static and configure gates**

```bash
rtk git diff --check
rtk env PKG_CONFIG_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib/pkgconfig ./configure --help
rtk env PKG_CONFIG_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib/pkgconfig cmake -S . -B /tmp/tte-mrcp-issue30-final -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_TTS_WEBSOCKET_TESTS=ON
rtk cmake --build /tmp/tte-mrcp-issue30-final --target test_tts_websocket_resampler test_tts_websocket_pcm test_tts_websocket_lifecycle test_tts_websocket_ws test_tts_websocket_http_parse -j4
rtk env LD_LIBRARY_PATH=/tmp/tte-mrcp-speexdsp-1.2.1/lib ctest --test-dir /tmp/tte-mrcp-issue30-final --output-on-failure -R '^tts_websocket_'
```

Expected: no whitespace errors, both configuration systems resolve SpeexDSP 1.2.1, and every TTS plugin test passes with pristine output.

- [x] **Step 5: Validate the target workflow and update evidence**

Run a YAML parse, inspect the exact staged diff, and verify every pin/target/artifact assertion:

```bash
rtk ruby -e 'require "yaml"; YAML.load_file(".github/workflows/build-linux.yml"); puts "workflow yaml ok"'
rtk rg -n 'SpeexDSP-1.2.1|1b28a0f61bc31162979e1f26f3981fc3637095c8|test_tts_websocket_resampler|glibc_max|require_gcc_485' .github/workflows/build-linux.yml
rtk git diff --check
```

Mark only actually executed plan checkboxes. Record Linux artifact builds as pending until GitHub Actions supplies current-run evidence; never substitute macOS configuration results for Linux deployment verification.

- [x] **Step 6: Commit Task 3**

```bash
rtk git commit -m "ci(tts): package SpeexDSP for Linux targets" -m "Refs #30"
```

#### Task 3 execution evidence — 2026-08-07

- Step 1 RED: both pre-edit `rtk rg` assertions exited `1` with no output;
  the workflow had no SpeexDSP pin/install or resampler target gate.
- Step 2: `.github/workflows/build-linux.yml` now clones
  `SpeexDSP-1.2.1`, verifies commit
  `1b28a0f61bc31162979e1f26f3981fc3637095c8`, builds shared-only at
  `/opt/tte-mrcp`, and checks pkg-config version `1.2.1`.
- Step 3: both matrix rows run the TTS Autotools `make check`; the aarch64
  CMake gate builds all six TTS tests, runs an explicit six-name TTS CTest
  regex, stages/verifies the SpeexDSP soname chain, checks staged plugin
  resolution, and audits ELF machine/GLIBC ceilings. ZIP uses `zip -yr`.
- Step 4/5: YAML parse, embedded container Bash syntax, `git diff --check`,
  configure help, CMake configure, six-target TTS build, and TTS CTest all
  passed locally. CMake required the existing local APR/APR-util/Sofia
  pkg-config paths in addition to the fixed SpeexDSP path.
- Linux x86_64/aarch64 artifact and target-ISA evidence remains pending until
  GitHub Actions runs on the PR; it is not substituted with macOS evidence.

## Whole-branch verification and delivery

**Files:**
- Modify only files required to address final review findings.
- Do not add the problem ZIP, downloaded audio, generated build trees, logs or temporary PCM files.

**Interfaces:**
- Consumes: all Task 1–3 commits and their reports.
- Produces: reviewed Issue #30 branch, pushed remote branch, PR targeting `main`, and Issue comment linking evidence.

- [ ] **Run source and dependency hygiene checks**

```bash
rtk git status --short
rtk git diff --check origin/main...HEAD
rtk rg -n 'resample_pcm_to_8k|resample_ulaw_to_8k|three.point|3点|移动平均' plugins/tts-websocket/src plugins/tts-websocket/tests
rtk rg -n 'SpeexDSP-1.2.1|1b28a0f61bc31162979e1f26f3981fc3637095c8|speexdsp >= 1.2.1|quality.?10' configure.ac plugins/tts-websocket .github/workflows/build-linux.yml
```

Expected: only the two user attachments are untracked; no legacy resampler remains; dependency/version/quality pins are present.

- [ ] **Run the final local test gate**

Run the focused CTest suite from a fresh `/tmp` build directory, then run any repository-provided shell/XML/configuration checks affected by the diff. Save exact commands and summaries in the SDD report; do not claim unrun Linux artifact gates.

- [ ] **Reindex codebase-memory and verify the new call path**

Run the available codebase-memory repository index, then verify canonical nodes for the resampler create/process/finish functions and the WebSocket worker call path. If the MCP tools are unavailable, record that limitation explicitly rather than substituting text search as graph evidence.

- [ ] **Complete the final branch review and one bounded fix wave**

Review the complete `origin/main...HEAD` diff for specification compliance, lifecycle safety, input/output accounting, test validity, dependency packaging and unrelated files. Resolve all Critical/Important findings through one reviewed fix wave; ledger any non-blocking minor finding with a ruling.

- [ ] **Push, create PR and update Issue #30**

Push only `fix/issue-30-speexdsp-resampler`. Create a PR targeting `main` with root cause, implementation scope, RED/GREEN tests, local limitations, and `Closes #30`. After CI completes, verify both `rhel7-x86_64` and `kylinv10-aarch64` jobs, artifact contents, plugin loading and resampler tests. Add an Issue comment linking the PR and evidence; do not merge until protection rules and review pass.
