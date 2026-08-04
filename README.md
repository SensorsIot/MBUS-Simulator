# MBUS-Simulator

ESP32-C3 firmware that simulates an M-Bus / DLMS smart meter, for exercising
meter-reading clients without physical hardware.

## Building

Firmware is built by **GitHub Actions**, not in the devcontainer — the ESP-IDF
toolchain is deliberately not installed locally (see `.devcontainer/Dockerfile`).

- Target: **esp32c3**
- ESP-IDF: **v6.0.2**
- Workflow: `.github/workflows/build.yml`

Push to `main` or open a PR to trigger a build; artifacts are attached to the run.
Tag `v*` to cut a release with the binaries attached.

To build locally anyway, use a machine that has ESP-IDF — do not add the
toolchain to this devcontainer.

## Devcontainer

SSH on host port **2547** (`ssh dev@dev-1.local -p 2547`). Secrets arrive as
`/run/secrets/env`; GitHub auth uses the `git-cred-from-secrets` helper. See
`~/.claude/devcontainer-common.md` for the shared configuration.
