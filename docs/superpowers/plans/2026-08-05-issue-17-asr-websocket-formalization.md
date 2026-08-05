# ASR WebSocket Formalization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 FunASR WebSocket 识别插件从 `demo-recog`/`demorecog` 迁移为与 TTS 对称的生产级 `asr-websocket`/`asr_websocket`，并同步构建、配置、工具、测试和文档。

**Architecture:** 仅迁移正式插件的产品/工程命名和装配边界；`funasr_*` 保留为协议、音频、控制和 transport 实现层名称。服务端 XML 使用唯一的 `ASR-WebSocket-1` engine 与 `speechrecog` 映射，诊断 fixture 将旧配置去重并生成 canonical 配置，同时在文档中记录旧名称到新名称的迁移表。

**Tech Stack:** C/C++、APR/UniMRCP、Autotools/Libtool、CMake、Visual Studio `.sln/.vcxproj/.vcproj`、Python `unittest`、Bash、XML。

## Global Constraints

- 正式 ASR 插件 canonical 目录为 `plugins/asr-websocket/`，目标/动态库/注册名为 `asr_websocket`，engine id 为 `ASR-WebSocket-1`。
- `funasr_*`、`funasr-host`、`funasr-port`、`funasr-path` 属于 FunASR 协议实现和部署参数，保持不变。
- 不修改 FunASR 协议语义，不修改 MRCP、SIP、RTSP、RTP 或 MPF 公共库。
- 活动构建、配置、诊断和正式插件文档不得把 ASR 插件描述为 demo；示例客户端中的 `demo_*` 保持原有示例语义。
- 生成的 `configure`、`Makefile.in` 等 Autotools 文件只能由 `./bootstrap` 生成，不能手工编辑。
- macOS、Windows、Linux 验证结果分别记录；未执行的平台标记为“未验证”。

---

### Task 1: Establish canonical naming and migration regression test

**Files:**
- Modify: `tools/diagnostics/funasr_ws_fixture.py:300-335,515-540`
- Create: `plugins/asr-websocket/README.md`

**Interfaces:**
- Consumes: existing XML fixture generator and its `unittest` cases.
- Produces: canonical engine config generation using `ASR-WebSocket-1`/`asr_websocket`, with legacy `Demo-Recog-1`/`demorecog` entries removed and documented.

- [x] **Step 1: Write the failing migration test**

Update the fixture test input to contain two legacy engine entries and assert that `emit_config()` produces exactly one canonical engine:

```python
def test_emit_config_migrates_legacy_engine_to_canonical_name(self) -> None:
    xml = """<unimrcpserver><components><plugin-factory>
    <engine id="Demo-Recog-1" name="demorecog" enable="true"/>
    <engine id="Demo-Recog-1" name="demorecog" enable="true"/>
    </plugin-factory></components></unimrcpserver>"""
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "source.xml"
        target = Path(directory) / "target.xml"
        source.write_text(xml, encoding="utf-8")
        emit_config(source, target, "127.0.0.1", 8022, "/ws/audio")
        engines = ET.parse(target).getroot().findall(
            ".//engine[@id='ASR-WebSocket-1']"
        )
        self.assertEqual(len(engines), 1)
        self.assertEqual(engines[0].get("name"), "asr_websocket")
        self.assertEqual(
            ET.parse(target).getroot().findall(".//engine[@name='demorecog']"), []
        )
```

- [x] **Step 2: Run the focused test and verify the expected red failure**

Run: `python3 -m unittest tools/diagnostics/funasr_ws_fixture.py -v`

Expected: the new migration test fails because the fixture still writes `Demo-Recog-1`/`demorecog`.

- [x] **Step 3: Implement canonical fixture migration**

Make `emit_config()` match both legacy and canonical ASR engine entries, remove all matches, and insert exactly one engine with:

```python
{"id": "ASR-WebSocket-1", "name": "asr_websocket", "enable": "true"}
```

Keep the `funasr-*` parameter names and reject source==target or overwrite exactly as before.

- [x] **Step 4: Run the focused test and verify green**

Run: `python3 -m unittest tools/diagnostics/funasr_ws_fixture.py -v`

Expected: all fixture tests pass, including the canonical migration assertion.

- [x] **Step 5: Add the plugin README**

Document the production role, runtime chain, canonical names, endpoint parameters, compatibility mapping (`Demo-Recog-1`/`demorecog` → `ASR-WebSocket-1`/`asr_websocket`), default logging fields, test commands, and a three-row macOS/Windows/Linux verification matrix with unexecuted platforms marked “未验证”.

### Task 2: Rename the formal plugin and synchronize all build systems

**Files:**
- Rename: `plugins/demo-recog/` → `plugins/asr-websocket/`
- Rename: `plugins/asr-websocket/src/demo_recog_engine.c` → `plugins/asr-websocket/src/asr_websocket_engine.c`
- Rename: `plugins/asr-websocket/demorecog.vcxproj` → `plugins/asr-websocket/asr_websocket.vcxproj`
- Rename: `plugins/asr-websocket/demorecog.vcxproj.filters` → `plugins/asr-websocket/asr_websocket.vcxproj.filters`
- Rename: `plugins/asr-websocket/demorecog.vcproj` → `plugins/asr-websocket/asr_websocket.vcproj`
- Rename: `plugins/asr-websocket/tests/test_funasr_control.vcxproj` → `plugins/asr-websocket/tests/test_funasr_control.vcxproj` (path retained; project identity remains test-focused)
- Rename: `plugins/asr-websocket/tests/test_funasr_ws_transport.vcxproj` → `plugins/asr-websocket/tests/test_funasr_ws_transport.vcxproj` (path retained; project identity remains test-focused)
- Modify: `CMakeLists.txt`, `configure.ac`, `plugins/Makefile.am`, `data/Makefile.am`
- Modify: `.github/workflows/build-linux.yml`
- Modify: `plugins/asr-websocket/CMakeLists.txt`, `plugins/asr-websocket/Makefile.am`, `plugins/asr-websocket/asr_websocket.vcxproj*`, `plugins/asr-websocket/asr_websocket.vcproj`
- Modify: `unimrcp.sln`, `unimrcp-2010.sln`
- Regenerate: `configure`, `Makefile.in` files affected by `./bootstrap`

**Interfaces:**
- Consumes: Task 1 canonical plugin contract.
- Produces: `asr_websocket` library/module and `ENABLE_ASR_WEBSOCKET_PLUGIN`/`ASR_WEBSOCKET_PLUGIN` build switches across Autotools, CMake, and Visual Studio.

- [x] **Step 1: Rename tracked plugin paths and engine source**

Run explicit `git mv` operations for the plugin directory and engine source/project filenames, preserving all `funasr_*` source and test names.

- [x] **Step 2: Update CMake and Autotools source-of-truth files**

Use `ASR_WEBSOCKET_SOURCES`, `project(asr_websocket)`, `BUILD_ASR_WEBSOCKET_TESTS`, `asr_websocket.la`, `ASR_WEBSOCKET_PLUGIN`, and the `plugins/asr-websocket` path. Register tests as `asr_websocket_resample`, `asr_websocket_json_unescape`, `asr_websocket_funasr_ws_transport`, and `asr_websocket_funasr_control`.

- [x] **Step 3: Update Visual Studio project and solution identities**

Set project/root namespace to `asr_websocket`, update the engine source path to `src\asr_websocket_engine.c`, and update both solution generations to reference `plugins\asr-websocket\asr_websocket.vcproj` or `.vcxproj`. Preserve existing GUIDs and test project GUIDs so solution dependencies remain stable.

- [x] **Step 4: Regenerate Autotools inputs**

Run: `./bootstrap`

Expected: `configure` and affected `Makefile.in` files contain only the new ASR WebSocket conditional/path names; no hand-edited generated content is introduced.

- [x] **Step 5: Verify build-system references**

Run: `rtk rg -n -i 'demo-recog|demorecog|DEMORECOG|DEMO_RECOG' CMakeLists.txt configure.ac plugins data unimrcp.sln unimrcp-2010.sln`

Expected: no formal plugin build reference remains; only explicitly documented legacy compatibility references in the migration fixture/docs remain.

### Task 3: Migrate runtime configuration, diagnostics, and stress observability

**Files:**
- Modify: `conf/unimrcpserver.xml`
- Modify: `tools/diagnostics/funasr_ws_fixture.py`
- Modify: `tools/diagnostics/diagnose.sh`
- Modify: `tools/stress/stress_test_improved.sh`
- Modify: `tools/README.md`

**Interfaces:**
- Consumes: canonical library/module names from Task 2 and migration API from Task 1.
- Produces: one canonical `speechrecog` mapping and operational tools that inspect `asr_websocket.so` and call the ASR transport metrics by product name.

- [x] **Step 1: Replace the duplicate runtime engine configuration**

In `conf/unimrcpserver.xml`, remove both legacy duplicate `Demo-Recog-1` entries and add one enabled engine:

```xml
<engine id="ASR-WebSocket-1" name="asr_websocket" enable="true">
  <param name="funasr-host" value="127.0.0.1"/>
  <param name="funasr-port" value="8022"/>
  <param name="funasr-path" value="/ws/asr"/>
</engine>
```

Map `speechrecog` exactly once to `ASR-WebSocket-1`; retain the endpoint parameter literals and make the XML comments identify them as FunASR deployment parameters.

- [x] **Step 2: Update diagnostic plugin checks and messages**

Check for `$PLUGIN_DIR/asr_websocket.so` and report `ASR WebSocket` in user-facing output. Do not print the default endpoint as a hard-coded production fact; parse `funasr-host/port/path` from the active XML, check the parsed port, and report missing configuration explicitly.

- [x] **Step 3: Update stress-tool labels**

Change only product labels and help text from `demorecog` to `asr_websocket`; preserve `funasr` fixture schema names and existing metrics fields (`session_id`, generation, byte/frame counters, queue overflow, timeout, abnormal close, completion reason).

Update the Linux package and CMake gates to use the canonical `asr-websocket` path, `BUILD_ASR_WEBSOCKET_TESTS`, and `asr_websocket_*` CTest prefix.

- [x] **Step 4: Run fixture and XML migration checks**

Run: `python3 -m unittest tools/diagnostics/funasr_ws_fixture.py -v`

Run: `python3 -c 'import xml.etree.ElementTree as ET; r=ET.parse("conf/unimrcpserver.xml").getroot(); e=r.findall(".//engine[@id=\"ASR-WebSocket-1\"]"); m=r.findall(".//resource[@id=\"speechrecog\"][@engine=\"ASR-WebSocket-1\"]"); assert len(e)==1 and len(m)==1 and e[0].get("name")=="asr_websocket"; print("canonical XML ok")'`

Expected: all Python tests pass and the XML assertion prints `canonical XML ok`.

### Task 4: Update project-facing documentation and governance references

**Files:**
- Modify: `README.md`
- Modify: `docs/mainpage.docs`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: canonical names, compatibility mapping, and verification boundaries from Tasks 1–3.
- Produces: source-of-truth project guidance that consistently describes the ASR plugin as production-grade.

- [x] **Step 1: Update README plugin/deployment sections**

Add a concise “正式 ASR WebSocket 插件” section beside the existing TTS section, listing canonical directory/library/engine id/registration name, FunASR parameters, diagnostic entrypoint, and the legacy migration table. State that real production gray release remains a manual runtime gate.

- [x] **Step 2: Update Doxygen and AGENTS architecture text**

Replace the formal `demo-recog` product description with `asr-websocket`; preserve `demo_*` wording only for sample client applications. Update the plugin directory responsibility and canonical naming statements in `AGENTS.md`.

- [x] **Step 3: Verify active-reference hygiene**

Run: `rtk rg -n -i 'demo-recog|demorecog|Demo-Recog|DEMORECOG|DEMO_RECOG' --glob '!docs/dox/**' --glob '!docs/ea/**' --glob '!docs/reports/**' --glob '!docs/superpowers/**' --glob '!**/Makefile.in' --glob '!configure' .`

Expected: remaining matches are limited to the explicit compatibility mapping/test fixture and allowed sample-client `demo_*` identifiers; no active formal plugin build/config/docs path uses the old product name.

### Task 5: Run verification, review the diff, and prepare the PR

**Files:**
- Verify: all changed files from Tasks 1–4
- Modify: none unless verification exposes a scoped defect

**Interfaces:**
- Consumes: completed implementation and documentation.
- Produces: evidence-backed commit and PR for Issue #17 targeting `main`.

- [x] **Step 1: Run focused and syntax checks**

Run:

```sh
python3 -m unittest tools/diagnostics/funasr_ws_fixture.py -v
find tools -type f -name '*.sh' -print0 | xargs -0 bash -n
python3 -m py_compile tools/diagnostics/funasr_ws_fixture.py
git diff --check
```

- [x] **Step 2: Run macOS configuration gates**

Run:

```sh
./tools/dev/setup_macos.sh
./bootstrap
./configure --help
cmake -S . -B /tmp/tte-mrcp-issue-17-cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

Run the plugin tests when dependencies permit: `cmake --build /tmp/tte-mrcp-issue-17-cmake --target test_funasr_ws_transport test_funasr_control test_resample test_json_unescape` and `ctest --test-dir /tmp/tte-mrcp-issue-17-cmake -R '^asr_websocket_' --output-on-failure`.

- [x] **Step 3: Record platform boundaries**

Record macOS results from the actual commands; mark Windows Visual Studio build/DLL load and Linux production `.so`/runtime smoke as “未验证” if no corresponding environment is available. Do not substitute macOS results for those platforms.

- [x] **Step 4: Inspect scope and commit**

Run `git status --short`, `git diff --stat`, and `git diff --name-status`; stage only Issue #17 files and commit with `refactor(asr-websocket): formalize production recognizer plugin` including `Closes #17` in the commit body.

- [x] **Step 5: Request review before publish**

Review the final diff against `origin/main`, resolve all Critical/Important findings, and rerun the relevant checks.

- [x] **Step 6: Push and create the PR**

Run `git push -u origin refactor/issue-17-formalize-asr-plugin`, then create a PR to `main` with title `refactor(asr-websocket): formalize production recognizer plugin` and a body containing summary, migration mapping, validation results, platform boundaries, and `Closes #17`.
