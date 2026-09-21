# UE PIE Automation roadmap

UE PIE Automation's contract is a tight observe → hypothesize → reproduce → verify loop inside Play-In-Editor. The current release already records logs, input, state, drift, observations, performance and images; future work should improve conclusions without disturbing the native MCP surface.

## Current foundation

- `FPIEInputRecorder`, `FPIEInputReplayer`, `FPIEObserver`, `FPIEFrameSampler`, `FPIEInputInjector` and the file format remain the behavior core.
- `FGameplayHandlers` remains the JSON-facing service layer.
- `FUE_PIE_AutomationMCPRegistry` exposes those handlers as native `PIEStudio.*` tools through `IModelContextProtocolModule`.
- Tool descriptions and schemas are kept in `Resources/UE_PIE_AutomationTools.json`.
- Artifacts land under the host project's `Saved/MCPRecordings`, `Saved/MCPObservations`, `Saved/MCPSessions`, `Saved/MCPCaptures` and `Saved/MCPTraces` directories.

## Next useful increments

1. Add a real scenario runner that executes arrange, act and assert in one native tool while preserving the existing scaffold/validate format.
2. Add a small native smoke-test harness that creates a controllable Pawn and Enhanced Input action for repeatable PIE verification.
3. Improve replay conclusions by correlating first divergence, session errors and captured frames in one structured result.
4. Keep deterministic state replay separate from input re-simulation; fixed timestep and seeded randomness improve repeatability but do not promise full engine determinism.

## Non-goals

Do not add a second MCP server, a Node runtime, a general-purpose automation framework or a Gauntlet runner to the plugin. Use the UE native MCP server and the existing editor services; add a new layer only when a current tool cannot express the required workflow.
