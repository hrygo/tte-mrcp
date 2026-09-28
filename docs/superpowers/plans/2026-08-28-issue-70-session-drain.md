# Issue #70 WebSocket session.done drain implementation plan

> **For implementation:** follow this plan task-by-task and keep the change limited to the TTS WebSocket plugin and its tests.

**Goal:** Prevent late binary audio already queued in the WebSocket/TCP receive path from being discarded when `session.done` arrives before the server WebSocket Close frame.

**Design:** Keep the existing single stream-thread receive ordering. Treat `session.done` as a transition into a drain state, not as stream termination. Continue decoding control and binary frames until the peer Close is decoded. If Close is absent, apply a 500ms monotonic grace deadline. Binary frames observed after `audio.done` or `session.done` remain eligible while the audio stream has started. Set the normal stream-complete state only after the drain loop exits and pending PCM/ring-buffer work has been flushed.

**Files:**

- `plugins/tts-websocket/src/tts_websocket_engine.c` — drain state, deadline-aware receive timeout, late binary acceptance, and normal Close handling.
- `plugins/tts-websocket/src/tts_websocket_drain.[ch]` — small pure state policy used by the stream thread and unit tests.
- `plugins/tts-websocket/tests/test_tts_websocket_drain.c` — boundary tests for Close, grace timeout, and late binary frames.
- `plugins/tts-websocket/CMakeLists.txt`, `plugins/tts-websocket/Makefile.am` — include the policy source and test target.

## TDD tasks

1. Add failing drain-policy tests for: `session.done` opens a 500ms window; a Close ends it immediately; a binary frame remains accepted after `audio.done`/`session.done`; and the deadline rejects further frames.
2. Run only the new test and record the expected compile/failure result before implementation.
3. Implement the policy and wire it into the stream-thread receive loop. Preserve the pre-`session.done` socket timeout, cap post-`session.done` blocking receives by the remaining grace period, and treat peer Close or grace expiry as normal completion.
4. Run the focused policy test, all TTS WebSocket tests, and repository diff/configuration checks. Refactor only after the tests are green.
5. Review the final diff against Issue #70, commit with `Closes #70`, push the Issue branch, and open a PR targeting `main`.
