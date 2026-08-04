# MBUS-Simulator — Functional Specification Document (FSD)

| | |
|---|---|
| **Status** | Draft — requirements specified, none verified |
| **Target** | ESP32 (classic, Xtensa), ESP-IDF v6.0.2 |
| **Peer under test** | [`gplug-mini`](https://github.com/SensorsIot/gplug-mini) — ESP32-C3 M-Bus/DLMS reader |
| **Complexity** | Low–Medium — one protocol, one output pin, no network |
| **Provenance legend** | `[user]` decided in the design interview · `[derived]` follows from another requirement · `[capture]` measured from a real E450 telegram · `[doc:EBL]` EBL Liestal parametrisation · `[doc:LG]` Landis+Gyr *Standard CIP-Liste*, 16 Sep 2024 |

> **Plane note.** This project deliberately carries the **WHAT plane only** — no
> Harness and no UserDocumentation. It is a bench instrument with no end user and
> no field deployment; the operating instructions are §8's twelve console
> commands. `[user]`

---

## 1. System Overview

### 1.1 Purpose

An ESP32 that behaves, on the wire, like the customer information interface (CII)
of a Landis+Gyr E450 smart meter as parametrised by **EBL Liestal**, so that
`gplug-mini` can be developed and regression-tested without a physical meter.

### 1.2 Problem statement

The gPlug is a passive listener on a meter it cannot influence. Development
against the real meter is therefore slow and unrepeatable: the values are
whatever the household is doing, the meter cannot be asked to produce a
particular reading, and the interesting cases — a burst interrupted by a power
cycle, a counter at a specific value — cannot be provoked at all. Two recorded
captures exist and are already used as host-tier fixtures, but a recording cannot
be driven, cannot be timed, and does not exercise a UART.

### 1.3 Users

One: the developer of `gplug-mini`, operating the simulator from a serial console
or from a `WorkbenchDriver` test script.

### 1.4 Goals

1. Emit a **structurally valid** DLMS/COSEM push telegram that `dlms_parser`
   v2.1.0 decodes without error.
2. Emit **self-consistent** power and energy: the energy counters are the
   integral of the transmitted power.
3. Be **steerable at runtime** — set a load, observe what the DUT reports.
4. Be **deterministic** — the same commands produce the same bytes.

### 1.5 Non-goals

Not a meter. It does not measure anything, has no tariff logic, no billing
accuracy, no metrological claim, and no relationship to any real installation.

### 1.6 System flow

```
console CLI ──► value model ──► push scheduler ──► telegram encoder ──► UART2
 (set pi …)     P, E, I, Q       every 5 s          DataNotification      2400 8E1
                                                    → GBT → HDLC          TX inverted
                                                                          GPIO18
                                                                             │
                                              12/24 V M-Bus driver ◄─────────┘
                                                      │        (inverting)
                                                      ▼
                                       gPlug GPIO7 ──► dlms_parser v2.1.0
                                       + 5 V supply, Data Request
```

---

## 2. System Architecture

### 2.1 Hardware / platform architecture

| Item | Value | Provenance |
|---|---|---|
| MCU | ESP32 classic (Xtensa dual-core) | `[user]` |
| Meter-link UART | **UART2**, TX **GPIO18** (via the GPIO matrix) | `[user]` |
| Console UART | UART0 (GPIO1/GPIO3), 115200 8N1 | `[derived]` |
| Line parameters | 2400 baud, 8 data bits, **even** parity, 1 stop bit | `[doc:LG]` §3.1 |
| Driver stage | External, **inverting**, logic → 12/24 V M-Bus levels | `[user]` |
| DUT supply | Rig provides 5 V + GND + Data Request on the RJ45, switchable | `[user]` |

The gPlug has no independent supply — it is powered from the meter's 5 V rail
(interface spec §2.1) — so a rig that drives only the data line leaves the DUT
dead. `[derived]`

### 2.2 Component layering

Strict one-way dependency, L0 → L1 → L2. The **L0/L1 line is ESP-IDF**: anything
that touches `driver/uart.h` or FreeRTOS is L0 and lives in `main/`; everything
above it is portable C++ that compiles on a Linux host.

| Layer | Components | Chapter |
|---|---|---|
| **L2 Application** | Meter value model · Push scheduler | §5, §6 |
| **L1 Interfaces** | Telegram encoder (A-XDR → DataNotification → GBT → HDLC) · Control console | §7, §8 |
| **L0 Foundation** | Meter link transport · Synthetic clock · DUT power | §9, §10, §11 |

```
components/e450_core/     L1+L2, zero ESP-IDF, builds for host and target
main/                     L0 shell: UART2, esp_console, timers, GPIO
test/host/                CMake + FetchContent dlms_parser v2.1.0 + ctest
```

This split is not tidiness — it is what makes §12's host tier possible. A frame
builder that includes `driver/uart.h` cannot be decoded on a laptop, and the
verification strategy collapses. `[derived]`

---

## 3. Implementation Phases

### 3.1 Phase 1 — Bytes right, off hardware

`e450_core` builds on Linux; the host test generates a burst from a known model
state, feeds it to `dlms_parser` v2.1.0, and asserts the decoded OBIS codes and
values equal the model. No ESP32 involved. **Exit:** `ctest` green in CI.

### 3.2 Phase 2 — Bytes on the wire

Firmware on the ESP32, UART2 configured and inverted, console live. **Exit:** a
logic analyser or a second ESP32 receives the same bytes the host test produced,
with no framing or parity errors.

### 3.3 Phase 3 — Against the DUT

Simulator drives a real gPlug through the interface, powering it. **Exit:**
`set pi 1500` is followed by the gPlug reporting 1.5 kW, and the energy the gPlug
reports rises at 1.5 kWh/h ±1 %.

---

## 4. Risks, Assumptions & Dependencies

### 4.1 Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Encoder and decoder are wrong in the same direction | Medium | Tests pass against a wire a real meter would never produce | The oracle is `dlms_parser`, which is independently validated against two real captures. Byte-exact reproduction of `e450_serial8.hex` remains available if a field discrepancy appears |
| TX pin floating between reset and UART init | High | Undefined bus level; DUT sees a break or garbage on every simulator reboot | FR-LNK-05 |
| A pin chosen for its default UART routing collides with module-specific use | Confirmed | GPIO16/17 are the SPI PSRAM on WROVER: both APIs succeed and the pin stays dead | FR-LNK-01 avoids them |
| Energy truncated to whole Wh per tick | High | At 777 W, 7 % of energy silently lost; at 300 W, 42 % | FR-MDL-04 |
| EBL enables message security later | Low | Simulator output no longer resembles the meter | §7 keeps the security stage as an identity function in the pipeline, not an absence |

### 4.2 Assumptions

- **A-01** The CII of the EBL Liestal E450 is **not encrypted**. `[user]` The
  Landis+Gyr document recommends the opposite — *"muss die Verschlüsselung der
  CIP-Daten zwingend eingeschaltet werden"* for METAS 8b conformance `[doc:LG]`
  p.5 — so EBL deviates from the vendor recommendation. If this proves wrong,
  every byte above the HDLC layer changes.
- **A-02** Balanced three-phase load: each phase carries one third of the total.
  `[user]`
- **A-03** The GBT block-split policy is free. A real meter's blocks are
  irregular — 112/122/44 bytes in one capture, 112/108/122/70 in the other
  `[capture]` — and the rule is not derivable, but `dlms_parser` reassembles
  before decoding, so any legal split decodes.

### 4.3 Explicitly out of scope

Message security (AES-GCM-128) · the 15 s, 1 min and 15 min push lists · M-Bus
sub-meter channels `0-n:24.x` · the optical port · receiving anything from the
DUT · WiFi, MQTT, OTA, NVS persistence · **fault injection** (corrupted CRC,
truncated bursts) — v1 emits only valid telegrams `[user]`.

### 4.4 Dependencies

| Dependency | Version | Why |
|---|---|---|
| ESP-IDF | v6.0.2 | Matches the toolchain already pinned in CI |
| `esphome-libs/dlms_parser` | **v2.1.0** | Host-tier oracle — the exact tag `gplug-mini` pins, so a host pass means a device pass |
| `e450_serial8.hex`, `e450_serial16.hex` | — | Source of every wire constant in Appendix B |

---

# Part A — Application Logic (L2)

## 5. Meter Value Model

### 5.1 Purpose & scope

Holds the quantities the meter would measure and keeps them mutually consistent.
Its correctness claim is that **energy is the integral of power** — which is what
makes the simulator falsifiable rather than merely plausible.

### 5.2 Requirements

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-MDL-01** | Must | The model shall hold active import power and active export power as independent non-negative integers in watts. | `[user]` |
| **FR-MDL-02** | Must | The model shall hold four active-energy and reactive-energy accumulators corresponding to OBIS `1-1:1.8.0`, `2.8.0`, `5.8.0`, `6.8.0`, `7.8.0`, `8.8.0`. | `[doc:EBL]` |
| **FR-MDL-03** | Must | Every accumulator shall be monotonically non-decreasing for the lifetime of a boot. | `[user]` |
| **FR-MDL-04** | Must | Accumulators shall be integrated in **milliwatt-seconds as unsigned 64-bit**, and converted to the transmitted whole-Wh value only at encoding time by integer division by 3 600 000. | `[derived]` |
| **FR-MDL-05** | Must | On each scheduler tick of Δt seconds the model shall add `P × Δt × 1000` mWs to the import accumulator and `P_export × Δt × 1000` to the export accumulator. | `[derived]` |
| **FR-MDL-06** | Must | All accumulators shall be zero at boot. | `[user]` |
| **FR-MDL-07** | Must | Phase currents `1-0:31.7.0`, `51.7.0`, `71.7.0` shall be derived as `I_mA = (P_import + P_export) × 1000 / (3 × 230)`, identical on all three phases. | `[user]` A-02 |
| **FR-MDL-08** | Should | Reactive power shall be derived as `Q = P × tan(arccos(pf))` from a settable power factor, default 1.00, and routed to exactly one of the four quadrant accumulators by the signs of P and Q. | `[user]` |
| **FR-MDL-09** | Must | With `pf = 1.00`, all four reactive accumulators shall hold at their current value. | `[derived]` |
| **FR-MDL-10** | Must | Setting export power shall never decrease the import accumulator. | `[user]` |

### 5.3 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-MDL-03/05 | `set pi 777`; run 720 ticks of 5 s | `1.8.0` = 777 Wh ±1 | Any tick where the value decreases | host |
| FR-MDL-04 | `set pi 300`; run 720 ticks of 5 s | 300 Wh ±1 | 0 Wh, or any value under 290 (truncation) | host |
| FR-MDL-07 | `set pi 1587` | Each current register reads 2300 mA ±1 | Currents differing between phases | host |
| FR-MDL-09 | `set pf 1.0`; run 100 ticks | All four reactive registers unchanged | Any reactive register incrementing | host |
| FR-MDL-10 | `set pi 1000`, 10 ticks, `set pi 0`, `set po 1000`, 10 ticks | `1.8.0` holds; `2.8.0` rises | `1.8.0` decreasing or resetting | host |

---

## 6. Push Scheduler

### 6.1 Purpose & scope

Decides *when* a telegram is emitted. EBL parametrises four push setups on four
schedules; **v1 implements Consumer Information 1 (the 5 s list) only** `[user]`.

### 6.2 Requirements

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-SCH-01** | Must | The scheduler shall emit one telegram whenever the synthetic clock's seconds field is divisible by 5. | `[doc:LG]` §3.1 |
| **FR-SCH-02** | Must | The telegram shall carry the 13 objects of Appendix A, in the listed order. | `[doc:EBL]` |
| **FR-SCH-03** | Must | The scheduler shall integrate the value model (FR-MDL-05) once per emission, using the elapsed time since the previous emission. | `[derived]` |
| **FR-SCH-04** | Must | Emission shall be suppressed while paused, and integration shall be suppressed with it. | `[derived]` |
| **FR-SCH-05** | Must | An emission forced by `burst` shall not disturb the periodic schedule. | `[user]` |
| **FR-SCH-06** | Must | If a telegram is still transmitting when the next tick falls due, the scheduler shall queue the new telegram and transmit it on completion, never interleaving two telegrams on the line. | `[derived]` |
| **FR-SCH-07** | Should | The architecture shall admit additional push setups with independent object lists and schedules without altering §7. | `[user]` |

> **On FR-SCH-06.** The 13-object list is ≈320 bytes of APDU, ≈356 bytes on the
> wire, **≈1.6 s at 218 byte/s** — comfortably inside 5 s. The requirement exists
> because the design must not *depend* on that: the moment a second push list is
> enabled, transmissions collide, and a real meter serialises them into what the
> DUT sees as one merged burst. `[derived]`

### 6.3 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-SCH-01 | `set time 12:00:00`; observe 60 s | 12 telegrams, at seconds 00,05,…,55 | Free-running 5 s period unaligned to the clock | target |
| FR-SCH-03 | `set pi 3600`; exactly 2 ticks | `1.8.0` advanced by 10 Wh | Advance of 5 or 20 Wh (single/double counting) | host |
| FR-SCH-05 | `burst` at second 02 | Extra telegram at 02; next scheduled at 05 | Schedule shifted to 07 | target |
| FR-SCH-06 | Force two emissions 100 ms apart | Second follows the first intact | Bytes of the two interleaved | target |

---

# Part B — Interfaces (L1)

## 7. Telegram Encoder

### 7.1 Purpose & peer

Turns an object list plus values into the exact byte sequence a Landis+Gyr E450
puts on its wired M-Bus interface. Its peer is `dlms_parser` v2.1.0.

### 7.2 Requirements

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-ENC-01** | Must | The encoder shall emit a DLMS `DataNotification` APDU (tag `0x0F`) comprising long-invoke-id-and-priority (4 bytes), a 12-byte date-time octet string, and a `structure` body. | `[capture]` |
| **FR-ENC-02** | Must | The body's first element shall be an `array` of `structure[4]` = {class-id `uint16`, OBIS `octet-string[6]`, attribute-index `int8`, data-index `uint16`}, one per object of Appendix A. | `[capture]` |
| **FR-ENC-03** | Must | The remaining body elements shall be the object values, positionally, in object-list order. | `[capture]` |
| **FR-ENC-04** | Must | Every numeric value shall be encoded as `uint32` (tag `0x06`), in watts, watt-hours, milliamperes or var-hours, with no scaler-unit attribute on the wire. | `[capture]` |
| **FR-ENC-05** | Must | The long-invoke-id shall increment by one per emitted telegram, starting at 0, with the high byte held at `0x00`. | `[capture]` |
| **FR-ENC-06** | Must | The APDU shall be segmented into General Block Transfer blocks in **streaming** mode: block control `0x40`, `0xC0` on the final block, block numbers from 1. | `[capture]` |
| **FR-ENC-07** | Must | Block payload size shall be a build-time constant, default 128 bytes. | `[derived]` A-03 |
| **FR-ENC-08** | Must | Each block shall be carried in one HDLC type-3 frame: `7E`, format `0xA0` + 11-bit length, destination `CE FF`, source `03`, control `13`, HCS, information, FCS, `7E`. | `[capture]` |
| **FR-ENC-09** | Must | The LLC header `E6 E7 00` shall precede the GBT header in the **first frame of a telegram only**. | `[capture]` |
| **FR-ENC-10** | Must | HCS shall cover the format byte through the control byte; FCS shall cover the format byte through the last information byte; both CRC-16/CCITT, polynomial `0x8408` reflected, init `0xFFFF`, final XOR `0xFFFF`, transmitted little-endian. | `[capture]` |
| **FR-ENC-11** | Must | The date-time field shall encode the synthetic clock with hundredths `0xFF`, deviation `0x8000` and clock status `0x00`. | `[capture]` |
| **FR-ENC-12** | Must | The encoder shall contain no ESP-IDF or FreeRTOS dependency and shall compile for a Linux host. | `[derived]` |
| **FR-ENC-13** | Should | Encoding shall pass through a security stage that is the identity function in v1 and is the sole place message security would be added. | `[user]` A-01 |

> **The frame length field counts from the format byte through the FCS**, i.e.
> the closing flag is at `frame_start + 1 + length`. Both captures confirm it and
> the HCS only validates on that reading. The interface spec's §4 description —
> single-byte addresses at offsets 3 and 4, payload window from offset 8 — does
> **not** match either capture, whose destination address is two bytes and whose
> information field therefore starts at offset 9. `[capture]`

### 7.3 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-ENC-01..04 | Encode a known model state; feed the bytes to `dlms_parser` v2.1.0 | Parse returns success; all 11 values equal the model | A parse error; a value off by a factor of 1000 | host |
| FR-ENC-05 | Encode 3 telegrams | Invoke ids 0, 1, 2 | Constant invoke id | host |
| FR-ENC-06/07 | Encode a 320-byte APDU with a 128-byte block size | 3 blocks, last flagged `0xC0` | A block flagged final that is not the last | host |
| FR-ENC-07 | Re-encode the same APDU with block size 64 | Decodes to identical values | Decode differs with block size | host |
| FR-ENC-10 | Encode one telegram; recompute both CRCs independently | Both match | A frame accepted by the DUT with a wrong FCS | host |
| FR-ENC-12 | `cmake -S test/host && ctest` on Linux, no ESP-IDF present | Builds and passes | Any ESP-IDF header required | host |

---

## 8. Control Console

### 8.1 Purpose & peer

The simulator's entire public API. Its peer is a human at a terminal or
`WorkbenchDriver.serial_write()` over the workbench's RFC2217 proxy — which is
why it is a serial line and not a network service: it needs no provisioning, no
broker and no IP, and it cannot interfere with the WiFi the DUT is being tested
on. `[user]`

### 8.2 Requirements

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-CLI-01** | Must | The device shall expose a line-based console on UART0 at 115200 8N1. | `[user]` |
| **FR-CLI-02** | Must | The console shall implement: `set pi <W>`, `set po <W>`, `set pf <0.00-1.00>`, `set time <ISO-8601>`, `status`, `burst`, `pause`, `resume`, `dut on`, `dut off`, `help`. | `[user]` |
| **FR-CLI-03** | Must | Every command shall reply on one line, beginning `OK` or `ERR `, before any further output. | `[derived]` |
| **FR-CLI-04** | Must | `status` shall print the complete model state — both powers, power factor, all accumulators in Wh, clock, paused flag, DUT power state, telegram count — as one `key=value` line. | `[derived]` |
| **FR-CLI-05** | Must | A malformed or out-of-range argument shall leave the model unchanged and reply `ERR`. | `[derived]` |
| **FR-CLI-06** | Must | Console output shall never be written to UART2. | `[derived]` |

### 8.3 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-CLI-03/04 | `set pi 1500` then `status` | `OK` then one line containing `pi=1500` | Multi-line or unparsable status | bench |
| FR-CLI-05 | `set pi -5`, then `status` | `ERR`; `pi` unchanged | Negative or wrapped power accepted | bench |
| FR-CLI-06 | Issue 100 commands while monitoring UART2 | Only telegram bytes on UART2 | Any console character on the meter link | target |

---

# Part C — Foundation (L0)

## 9. Meter Link Transport

### 9.1 Requirements

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-LNK-01** | Must | The device shall transmit on UART2, TX on a configurable GPIO defaulting to **18**, at 2400 baud, 8 data bits, even parity, 1 stop bit. | `[doc:LG]` §3.1 |
| **FR-LNK-02** | Must | The TX signal shall be inverted in the UART peripheral (`UART_SIGNAL_TXD_INV`). | `[user]` |
| **FR-LNK-03** | Must | The device shall never enable a receiver on UART2. | `[derived]` |
| **FR-LNK-04** | Must | Between telegrams the line shall rest in the mark state, presenting 24 V on the bus. | `[derived]` |
| **FR-LNK-05** | Must | The TX GPIO shall be driven to the mark-producing level before UART initialisation and within 10 ms of reset. | `[derived]` |

> **FR-LNK-05 is the one that bites.** With an inverting driver, UART idle drives
> the TX pin **low** to put 24 V (mark) on the bus. From reset until `app_main`
> configures it, that pin is a floating input and the bus level is undefined — so
> without this, every simulator reboot may present the DUT with a break condition
> or noise.

### 9.2 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-LNK-01/02 | Capture one telegram with a logic analyser at the driver input | Byte stream identical to the host test's output; 2400 8E1 decode clean | Any parity or framing error | target |
| FR-LNK-04 | Idle simulator; DVM on the bus | 24 V | 12 V (continuous space) | target |
| FR-LNK-05 | Scope the bus across a simulator reset, 20 resets | Bus at 24 V within 10 ms, no spurious edges | Any spurious start bit at boot | target |

---

## 10. Synthetic Clock

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-CLK-01** | Must | The device shall maintain a calendar clock advanced from the FreeRTOS tick, initialised at boot to a build-time epoch, default `2026-01-01T00:00:00`. | `[user]` |
| **FR-CLK-02** | Must | `set time` shall set the clock atomically between telegrams. | `[user]` |
| **FR-CLK-03** | Must | The device shall not obtain time from any external source. | `[user]` |
| **FR-CLK-04** | Must | Given identical commands from reset, two runs shall produce identical telegram bytes. | `[user]` |

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-CLK-04 | Reset; `set time 2026-08-04T17:00:00`; `set pi 1000`; capture 12 telegrams. Repeat. | Both captures byte-identical | Any differing byte | target |

## 11. DUT Power

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-PWR-01** | Must | The rig shall supply 5 V, ground and the Data Request line to the DUT connector. | `[user]` |
| **FR-PWR-02** | Must | `dut off` and `dut on` shall remove and restore that 5 V under firmware control. | `[user]` |
| **FR-PWR-03** | Must | Removing DUT power shall not alter the model, the clock or the schedule. | `[derived]` |

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-PWR-01 | Connect an unpowered gPlug; `dut on` | gPlug boots (LED sequence) | DUT requiring its own supply | bench |
| FR-PWR-03 | `dut off`, 30 s, `dut on` | Energy counters continued rising throughout | Counters frozen or reset | bench |

---

# Part D — Verification

## 12. Test Architecture

| Tier | Runs on | Needs | Covers |
|---|---|---|---|
| **host** | Linux, GitHub Actions, every push | Nothing | §5 model arithmetic, §7 encoder — decoded by `dlms_parser` v2.1.0 |
| **target** | The ESP32 alone | Logic analyser or second ESP32 | §6 timing, §9 line, §10 determinism |
| **bench** | Workbench + real gPlug | Both boards, workbench Pi | §8 console, §11 power, end-to-end |

**The host oracle is the decoder the DUT actually runs.** `test/host/CMakeLists.txt`
fetches `dlms_parser` at tag **v2.1.0** — the same tag `gplug-mini` pins — so a
host pass means the DUT can decode it. This proves *decodable*, not *faithful*: a
tolerant decoder would accept output a real E450 never emits. Byte-exact
reproduction of `e450_serial8.hex` is the stronger test held in reserve, and it
requires supplying the capture's own irregular block split (A-03).

**CI changes required with the first commit:**

1. `.github/workflows/build.yml` — `target: esp32c3` → **`esp32`**.
2. New `.github/workflows/host-tests.yml` — configure `test/host`, build, `ctest`.

---

## Appendix A — Consumer Information 1 object list (5 s)

Thirteen objects, in transmission order. Class IDs from `[capture]`; the register
selection is `[doc:EBL]`.

| # | OBIS | Attr | Class | Value on the wire | Label |
|---|---|---|---|---|---|
| 1 | `0-8:25.9.0` | 2 | 40 | the object-list array itself | Objektliste PS1 |
| 2 | `0-8:25.9.0` | 1 | 40 | `octet-string[6]` = `00 08 19 09 00 FF` | Logischer Name PS1 |
| 3 | `1-0:1.7.0` | 2 | 3 | `uint32`, W | Wirkleistungsbezug +P |
| 4 | `1-0:2.7.0` | 2 | 3 | `uint32`, W | Wirkleistungslieferung −P |
| 5 | `1-1:1.8.0` | 2 | 3 | `uint32`, Wh | Wirkenergiebezug +A |
| 6 | `1-1:2.8.0` | 2 | 3 | `uint32`, Wh | Wirkenergielieferung −A |
| 7 | `1-1:5.8.0` | 2 | 3 | `uint32`, varh | Blindenergie +Ri (QI) |
| 8 | `1-1:6.8.0` | 2 | 3 | `uint32`, varh | Blindenergie +Rc (QII) |
| 9 | `1-1:7.8.0` | 2 | 3 | `uint32`, varh | Blindenergie −Ri (QIII) |
| 10 | `1-1:8.8.0` | 2 | 3 | `uint32`, varh | Blindenergie −Rc (QIV) |
| 11 | `1-0:31.7.0` | 2 | 3 | `uint32`, mA | Strom L1 |
| 12 | `1-0:51.7.0` | 2 | 3 | `uint32`, mA | Strom L2 |
| 13 | `1-0:71.7.0` | 2 | 3 | `uint32`, mA | Strom L3 |

**The meter serial `0-0:96.1.0` is not here** — EBL puts it in the 15 s list,
which v1 does not emit. A DUT tested against this simulator therefore never
learns a meter identity, which differs from the field where it arrives every
15 s. Known and accepted `[user]`; closing it costs 4 objects and ≈0.6 s of
airtime.

## Appendix B — Wire constants

Every value below was measured from `e450_serial8.hex` and `e450_serial16.hex`,
which agree on all of them. `[capture]`

| Field | Value |
|---|---|
| HDLC opening/closing flag | `0x7E` |
| Format byte | `0xA0` \| high 3 bits of length |
| Length semantics | format byte through FCS; closing flag at `start + 1 + length` |
| Destination address | `CE FF` (**two bytes**) |
| Source address | `03` |
| Control | `13` |
| HCS coverage | offset 1 → control byte, little-endian |
| FCS coverage | offset 1 → last information byte, little-endian |
| CRC | CRC-16/CCITT, poly `0x8408` reflected, init `0xFFFF`, xorout `0xFFFF` |
| LLC | `E6 E7 00`, first frame of a telegram only |
| GBT block control | `0x40` streaming, `0xC0` on the last block |
| GBT block numbering | from 1; acknowledged-block field `0000` |
| DataNotification tag | `0x0F` |
| date-time | `YYYY MM DD DOW HH MM SS FF 80 00 00` (12 bytes) |
| Observed block sizes | 112/122/44 and 112/108/122/70 — irregular, not reproduced |

## Related

`[[gplug-mini-FSD]]` · `[[MBUS-E450-Interface-Spec]]` ·
`docs/Standard CIP-Liste_DE.pdf` (Landis+Gyr, 16 Sep 2024)
