# MBUS-Simulator

**ESP32** firmware that behaves, on the wire, like the customer information
interface of a Landis+Gyr **E450** smart meter as parametrised by **EBL Liestal**
— so that software which consumes that interface can be developed and
regression-tested without a physical meter.

It is a generator, not a tape player. Three modes form a diagnostic ladder — a
bare `0x55` pattern, one small telegram with fixed values, and the full 13-object
E450 list whose load changes on every emission, so a decoder has to read each
message rather than cache one. Energy integrates the transmitted power
throughout, so the arithmetic a decode produces can be checked, not just its
parsing.

```
console CLI ──► mode 1|2|3 ──► push scheduler ──► telegram encoder ──► UART2
 (mode 2)       1 pattern         every 5 s         DataNotification      2400 8E1
                2 fixed values                      → GBT → HDLC          TX not inverted
                3 random load                                             GPIO17
                                                                             │
                                              12/24 V M-Bus driver ◄─────────┘
                                                      │     (non-inverting)
                                                      ▼
                                       receiver, powered from the bus
```

## Hardware

| | |
|---|---|
| Board | ESP32 classic (Xtensa) — **not** an ESP32-C3 |
| Meter link | UART2, TX on **GPIO17**, 2400 8E1, **TX not inverted** |
| Driver stage | External, non-inverting, logic level → 12/24 V M-Bus |
| Downstream supply | The M-Bus signal itself — an M-Bus slave draws its power from the line, so the 24 V mark state is its supply |

Pins are Kconfig, not literals — GPIO16/17 are free on WROOM but belong to the
SPI PSRAM on WROVER, where they silently do nothing.

An M-Bus slave has no supply of its own: it draws power from the line, so the
24 V mark state the line rests in between telegrams is its supply as well as its
idle level. A rig that leaves the line at 12 V, or floating from reset until the
firmware configures the pin, starves whatever is connected as well as misframing
it.

## Layout

```
components/e450_core/   Portable C++: A-XDR, DataNotification, GBT, HDLC, value model.
                        No ESP-IDF, no FreeRTOS — this is what makes the host tier possible.
main/                   ESP32 shell: UART, console, scheduler, clock.  (Phase 2)
test/host/              Host tests: decode our own output with an independent decoder.
docs/Functionality/     The FSD — what it must do and how that is demonstrated.
docs/UserDocumentation/ The operator's manual — how to install, run and diagnose it.
```

## Testing

The host tier needs no hardware and no ESP-IDF, and runs on every push:

```bash
cmake -S test/host -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The oracle is **`dlms_parser` v2.1.0**, a DLMS decoder we did not write, so a
pass means an independent implementation can read what we emit.
`test/host/decode_dump` is a development tool that prints what the decoder makes
of any hex dump, generated or captured from a real meter.

## Building the firmware

Firmware is built by **GitHub Actions**, not in the devcontainer — the ESP-IDF
toolchain is deliberately not installed locally (see `.devcontainer/Dockerfile`).

- Target: **esp32**
- ESP-IDF: **v6.0.2**
- Workflows: `.github/workflows/build.yml`, `.github/workflows/host-tests.yml`

Push to `main` or open a PR to trigger a build; artifacts are attached to the run.
Tag `v*` to cut a release with the binaries attached.

To build locally anyway, use a machine that has ESP-IDF — do not add the
toolchain to this devcontainer.

## Documentation

**To operate the rig**, read
[`docs/UserDocumentation/MBUS-Simulator-Manual.md`](docs/UserDocumentation/MBUS-Simulator-Manual.md)
— installation, wiring, the console reference, what a healthy log looks like, and
a diagnostic chapter that walks down the three modes to localise a fault.

[`docs/Functionality/MBUS-Simulator-FSD.md`](docs/Functionality/MBUS-Simulator-FSD.md)
is the contract: 55 requirements, each with a verification contract naming the
outcome that would falsify it. `docs/Standard CIP-Liste_DE.pdf` is Landis+Gyr's
public Swiss customer-interface specification; note that EBL's parametrisation
differs from that standard list in both directions, and EBL's wins here.

## Devcontainer

SSH on host port **2547** (`ssh dev@dev-1.local -p 2547`). Secrets arrive as
`/run/secrets/env`; GitHub auth uses the `git-cred-from-secrets` helper. See
`~/.claude/devcontainer-common.md` for the shared configuration.
