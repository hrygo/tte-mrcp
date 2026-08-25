# ASR English Debug Logging Design

## Scope

Set the default UniMRCP logger priority in `conf/logger.xml` to `DEBUG` and
replace Chinese ASR runtime log messages with English. Update the ASR log test
so its source assertions and failure diagnostics match the English messages.

## Design

`conf/logger.xml` remains the single runtime logging configuration source. Its
default `<priority>` value changes from `INFO` to `DEBUG`; source-specific
priorities are not changed.

The two active ASR WebSocket runtime messages retain their priority, fields,
and control flow. Only their human-readable text changes to ASCII English:

- The client media-frame receive message reports the frame size in bytes.
- The ASR WebSocket send message reports the successfully written size in
  bytes.

The static CMake test uses the same English literals and English failure
diagnostics. Chinese documentation and source comments are outside this change
because they are not runtime or test log output.

## Verification

- Parse `conf/logger.xml` as XML and assert its default priority is `DEBUG`.
- Run the ASR frame-debug-log CMake test.
- Search active ASR C/CMake sources for Chinese characters and expect no
  matches.
- Run `git diff --check`.
