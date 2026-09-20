# SDH OptiScaler Control

Test-only companion ASI for SDH FrameGen. It is loaded by OptiScaler's existing ASI plugin loader and never replaces or patches the original OptiScaler dxgi.dll on disk.

Phase 1 implements a file-based runtime transport in the isolated SDH OptiScaler directory:
- sdh-control.cmd.json
- sdh-control.state.json

The first build also fingerprints the exact dxgi.dll from the current SDH OptiScaler payload so the memory adapter can be bound only to a known binary hash.
