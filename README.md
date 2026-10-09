# UE PIE Automation

UE PIE Automation is a native Unreal Engine 5.8 Editor plugin for recording, replaying, observing and diagnosing Play-In-Editor sessions. It exposes the existing PIE actions through Unreal's built-in Model Context Protocol server.

## Install

Copy this repository directory directly into a project's `Plugins` directory. The plugin root is this directory: `UE_PIE_Automation.uplugin`, `Source/` and `Resources/` are at its top level.

Enable `ModelContextProtocol` and `UE_PIE_Automation` in the project. Enable the native MCP server's auto-start setting once in **Editor Preferences > Plugins > Model Context Protocol**, then restart the editor. The first use of a source checkout compiles the C++ module with the normal Unreal project build; no Node.js, npm package manager or bridge plugin is required.

The plugin was verified through a directory junction at:

```text
E:/Workspace/_UE/Blank_5_8/Plugins/UE_PIE_Automation
```

## Native MCP tools

The plugin registers 54 tools named `PIEStudio.<action>`. With the native server's tool-search mode, discover them with `list_toolsets` and `describe_toolset`; direct calls use `tools/call` with the full name:

```json
{
  "name": "PIEStudio.record_status",
  "arguments": {}
}
```

The tools cover input injection, recording, replay, drift analysis, snapshots, observation profiles and runs, Actor manipulation, assertions, scenarios, session logs, captures and performance traces. The JSON schemas live in `Resources/UE_PIE_AutomationTools.json` and are served directly by the native MCP module.

## Typical workflow

1. Call `PIEStudio.record_arm` before starting PIE, or while PIE is already running.
2. Poll `PIEStudio.record_status` until it reports `recording`.
3. Call `PIEStudio.record_stop` to write `Saved/MCPRecordings/<id>/`.
4. Use `PIEStudio.replay_run` for unattended replay and poll `PIEStudio.replay_status` until `pie_active` is false.
5. Use `PIEStudio.frame_diff` to compare the latest PNG frames with `ref_frames`; defaults are 8 channel tolerance and 0.5% changed pixels.
6. Read `drift.json`, session errors, CSV data or captured frames from the returned artifact paths.

### Verify an existing recording after code changes

Use the MCP tool descriptions as the agent instructions; reading this README is not required by the client. The minimal **Replay & Capture** workflow is:

1. `PIEStudio.replay_run({"recording_id":"recording-20261009-161624-1a2a"})`.
2. Poll `PIEStudio.replay_status({})` until playback has started and finished: `state` is `idle` or `completed`, `pie_active` is false and `capture_frames_pending` is 0. The initial `armed` response can have `pie_active=false` and an old `last_result`; it is not completion.
3. `PIEStudio.frame_diff({"recording_id":"recording-20261009-161624-1a2a"})` returns batch image statistics and per-frame differences.

No repeated input-action paths, sampling rate, seed or settle delay are needed. These come from the recording. PNG capture uses the plugin defaults; optional overrides are only needed for a baseline made with different capture settings. MCP does not read unsaved panel FPS/resolution/time-scale controls. Inspect capture errors, dropped frames and missing frames before treating the comparison as a regression result. `drift.json` and `replay_analyze` describe gameplay state drift, not pixel differences.

Replay replaces the latest `frames` and preserves `ref_frames`. Never call `reference_save` during verification; it replaces the baseline and requires the user's explicit request.

The plugin adds a dockable PIE Automation panel under Tools. The UI and MCP call the same native C++ services.

## Data layout

```text
Saved/
  MCPRecordings/<id>/       manifest.json, sequence.json, recording.csv, drift.json
  MCPObservations/<run>/    observation.csv, tracked.jsonl, manifest.json
  MCPSessions/<session>/    session_log.jsonl, session_errors.json
  MCPCaptures/<run>/        viewport frames and contact sheets
  MCPTraces/                Unreal Insights .utrace files
```

## Build and test

Use the UE 5.8 Editor target for the host project, then run the `PIEStudio` Automation tests. The repository contains 11 tests covering serialization, session logs, contact sheets, drift, state replay, observations, assertions, performance summaries and scenario parsing.

```powershell
& 'C:/Program Files/Epic Games/UE_5.8/Engine/Build/BatchFiles/Build.bat' `
  Blank_5_8Editor Win64 Development `
  '-project=E:/Workspace/_UE/Blank_5_8/Blank_5_8.uproject' -WaitMutex
```

The native MCP endpoint defaults to `http://127.0.0.1:8000/mcp`; use `-ModelContextProtocolPort=<port>` when another editor owns port 8000.
