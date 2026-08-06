# MBUS-Simulator — Functional Specification Document (FSD)

| | |
|---|---|
| **Status** | Draft — requirements specified, none verified |
| **Target** | ESP32 (classic, Xtensa), ESP-IDF v6.0.2 |
| **Complexity** | Low–Medium — one protocol, one output pin, no network |
| **Provenance legend** | `[user]` decided in the design interview · `[derived]` follows from another requirement · `[capture]` measured from a real E450 telegram · `[doc:EBL]` EBL Liestal parametrisation · `[doc:LG]` Landis+Gyr *Standard CIP-Liste*, 16 Sep 2024  · `[measured]` established on the bench, against the behaviour it describes |

> **Plane note.** WHAT (this document) and OPERATE
> ([the manual](../UserDocumentation/MBUS-Simulator-Manual.md)) exist; there is no
> Harness plane. The manual was added once the console surface became small
> enough to describe to someone who did not build the rig — three modes, in
> §13. `[user]`
>
> **Chapter numbers are stable, so they are not in reading order.** §13 is the
> newest chapter and sits in Part A where it belongs by layer, rather than being
> numbered to match its position.

---

## 1. System Overview

### 1.1 Purpose

An ESP32 that behaves, on the wire, like the customer information interface (CII)
of a Landis+Gyr E450 smart meter as parametrised by **EBL Liestal**, so that
software which consumes that interface can be developed and regression-tested
without a physical meter.

### 1.2 Problem statement

A meter's customer interface is a one-way broadcast that cannot be influenced.
Development against a real meter is therefore slow and unrepeatable: the values are
whatever the household is doing, the meter cannot be asked to produce a
particular reading, and the interesting cases — a burst interrupted by a power
cycle, a counter at a specific value — cannot be provoked at all. Two recorded
captures exist and are already used as host-tier fixtures, but a recording cannot
be driven, cannot be timed, and does not exercise a UART.

### 1.3 Users

One: whoever is building something that consumes an E450 customer interface,
operating the simulator from a serial console or from a test script.

### 1.4 Goals

1. Emit a **structurally valid** DLMS/COSEM push telegram that `dlms_parser`
   v2.1.0 decodes without error.
2. Emit **self-consistent** power and energy: the energy counters are the
   integral of the transmitted power.
3. Be **steerable at runtime** — pick one of three modes (§13) and observe what
   comes out.
4. Be **deterministic** — the same commands produce the same bytes.

### 1.5 Non-goals

Not a meter. It does not measure anything, has no tariff logic, no billing
accuracy, no metrological claim, and no relationship to any real installation.

### 1.6 System flow

```
console CLI ──► mode (§13) ──► push scheduler ──► telegram encoder ──► UART2
 (mode 1|2|3)   1 pattern         every 5 s          DataNotification      2400 8E1
                2 fixed values
                3 random load
                                                    → GBT → HDLC          TX not inverted
                                                                          GPIO17
                                                                             │
                                              12/24 V M-Bus driver ◄─────────┘
                                                      │     (non-inverting)
                                                      ▼
                                       whatever is listening,
                                       powered from the bus
```

---

## 2. System Architecture

### 2.1 Hardware / platform architecture

| Item | Value | Provenance |
|---|---|---|
| MCU | ESP32 classic (Xtensa dual-core) | `[user]` |
| Meter-link UART | **UART2**, TX **GPIO17** (U2TXD default routing) | `[user]` |
| Console UART | UART0 (GPIO1/GPIO3), 115200 8N1 | `[derived]` |
| Line parameters | 2400 baud, 8 data bits, **even** parity, 1 stop bit | `[doc:LG]` §3.1 |
| Driver stage | External, **non-inverting**, logic → 12/24 V M-Bus levels | `[user]`, `[measured]` |
| Downstream supply | **The M-Bus signal itself.** No separate rail, nothing switchable | `[user]` |

An M-Bus slave draws its power from the bus, so the 24 V mark state FR-LNK-04
requires the idle line to hold is not merely a signalling level — it is the
supply. A rig that leaves the line at 12 V, or floating from reset until
`app_main` runs (FR-LNK-05), starves whatever is connected as well as misframing
it. `[derived]`

### 2.2 Component layering

Strict one-way dependency, L0 → L1 → L2. The **L0/L1 line is ESP-IDF**: anything
that touches `driver/uart.h` or FreeRTOS is L0 and lives in `main/`; everything
above it is portable C++ that compiles on a Linux host.

| Layer | Components | Chapter |
|---|---|---|
| **L2 Application** | Meter value model · Push scheduler · Operating modes · Runtime directives | §5, §6, §13, §14 |
| **L1 Interfaces** | Telegram encoder (A-XDR → DataNotification → GBT → HDLC) · Control console | §7, §8 |
| **L0 Foundation** | Meter link transport · Synthetic clock | §9, §10 |

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

Firmware on the ESP32, UART2 configured at the polarity FR-LNK-02 fixes, console
live. **Exit:** `mode 1` reads back as `0x55` at the far end (FR-MOD-02), and a
logic analyser or a second ESP32 receives the same bytes the host test produced,
with no framing or parity errors.

### 3.3 Phase 3 — On the bus

Simulator drives the interface board and a real bus. **Exit:** all three modes
behave as §13 requires, confirmed by a 2400 8E1 capture of the line.

---

## 4. Risks, Assumptions & Dependencies

### 4.1 Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Encoder and decoder are wrong in the same direction | Medium | Tests pass against a wire a real meter would never produce | The oracle is `dlms_parser`, which is independently validated against two real captures. Byte-exact reproduction of `e450_serial8.hex` remains available if a field discrepancy appears |
| TX pin floating between reset and UART init | High | Undefined bus level; a break condition or garbage on the bus at every simulator reboot | FR-LNK-05 |
| The TX pin is not free on another ESP32 module | Low | WROVER wires GPIO16/17 to the SPI PSRAM, where both APIs succeed and the pin stays dead. This board is a WROOM | FR-LNK-01 makes the pin Kconfig |
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
the bus · WiFi, MQTT, OTA, NVS persistence · **fault injection** (corrupted CRC,
truncated bursts) — v1 emits only valid telegrams `[user]`.

### 4.4 Dependencies

| Dependency | Version | Why |
|---|---|---|
| ESP-IDF | v6.0.2 | Matches the toolchain already pinned in CI |
| `esphome-libs/dlms_parser` | **v2.1.0** | Host-tier oracle — an independent DLMS decoder, so a host pass means the output is decodable by something we did not write |
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
| **FR-MDL-08** | Should | Reactive power shall be derived as `Q = P × tan(arccos(pf))` from the power factor the active mode selects, and routed to exactly one of the four quadrant accumulators by the signs of P and Q. | `[user]` |
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
| **FR-SCH-01** | Must | The scheduler shall emit one telegram whenever the synthetic clock's seconds field is divisible by the push period, which shall default to **5 s** and shall divide 60. | `[doc:LG]` §3.1 |
| **FR-SCH-02** | Must | The telegram shall carry the 13 objects of Appendix A, in the listed order. | `[doc:EBL]` |
| **FR-SCH-03** | Must | The scheduler shall integrate the value model (FR-MDL-05) once per emission, using the elapsed time since the previous emission. | `[derived]` |
| **FR-SCH-04** | ~~Must~~ | `deprecated` 2026-08-05 — emission shall be suppressed while paused. There is no paused state: the simulator always emits, and *what* it emits is the mode. | `[derived]` |
| **FR-SCH-05** | ~~Must~~ | `deprecated` 2026-08-05 — `burst` is removed. Every mode emits on the same period, so a forced emission has nothing left to disturb. | `[user]` |
| **FR-SCH-06** | Must | If a telegram is still transmitting when the next tick falls due, the scheduler shall queue the new telegram and transmit it on completion, never interleaving two telegrams on the line. | `[derived]` |
| **FR-SCH-07** | Should | The architecture shall admit additional push setups with independent object lists and schedules without altering §7. | `[user]` |
| **FR-SCH-08** | Must | Between the last byte of one telegram and the first byte of the next the line shall be silent for at least **3000 ms**, and never less than the 2000 ms that conventionally delimits one transmission from the next. | `[derived]` |

> **On FR-SCH-06.** The 13-object list is 381 bytes on the wire, **1747 ms at
> 218 byte/s** — inside 5 s. The requirement exists because the design must not
> *depend* on that: the moment a second push list is enabled, transmissions
> collide, and a real meter serialises them into what arrives as one merged
> burst. `[derived]`

> **On FR-SCH-08 — why the gap is a requirement and not a consequence.** The
> receiving end of an M-Bus link does not delimit transmissions by counting
> frames; it decodes once the line has been quiet, conventionally for 2000 ms. So the silence is load-bearing, and it is
> whatever the period has left over after the telegram — which makes it a
> property of the *telegram size*, a number nobody chose deliberately. At a 5 s
> period the budget is **436 bytes** for the 3000 ms target and **654 bytes**
> before consecutive telegrams start merging; the CI1 list uses 381. That is 55
> bytes of headroom, and FR-SCH-07's additional push setups would spend it at
> once.
>
> Two guards, because the failure is silent and lands at the far end. The host
> test computes the gap from the encoded telegram and fails the build if it
> drops below either threshold, which is possible because every value is a
> fixed-width encoding and the telegram is 381 bytes whatever the model holds.
> The firmware logs the gap on every telegram and raises one `E`-level line if
> it is short. Without those, a telegram that grew past the budget would present
> as a decode fault downstream and be debugged in the wrong place. `[derived]`

### 6.3 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-SCH-01 | `set time 12:00:00`; observe 60 s | 12 telegrams, at seconds 00,05,…,55 | Free-running 5 s period unaligned to the clock | target |
| FR-SCH-03 | `set pi 3600`; exactly 2 ticks | `1.8.0` advanced by 10 Wh | Advance of 5 or 20 Wh (single/double counting) | host |
| FR-SCH-05 | `burst` at second 02 | Extra telegram at 02; next scheduled at 05 | Schedule shifted to 07 | target |
| FR-SCH-06 | Force two emissions 100 ms apart | Second follows the first intact | Bytes of the two interleaved | target |
| FR-SCH-08 | Encode the CI1 telegram; compute the gap at a 5 s period | ≥ 3000 ms silent | Any gap under 2000 ms; a size that varies with model state | host |
| FR-SCH-08 | Capture ≥ 6 consecutive cycles off the bus | Each delimited separately, same byte count every time | Two cycles merged into one | bench |

---

## 13. Operating Modes

### 13.1 Purpose

Three things this simulator can put on the wire, chosen by one console command.

They are a **diagnostic ladder, not a feature list**: each mode adds exactly one
layer to the one below, so the first that fails names the fault. `[user]`

| Mode | On the wire | Answers |
|---|---|---|
| **1 pattern** | `0x55`, no protocol | Does a signal come off the bus, at the right line settings? |
| **2 simple** | One frame, three objects, fixed values | Is a single telegram decodable at all? |
| **3 e450** | The full 13-object list, values changing every emission | Can it decode *this meter's* telegrams? |
| **4 ramp** | `00 01 02 … FF` repeating, telegram-sized | **How many** bytes were lost, exactly? |

Between 2 and 3 lie the two things most likely to defeat a decoder: multi-block
General Block Transfer reassembly, and values it has not seen before. Running
them in order separates *the line is wrong* from *the decoder is wrong* from
*the decoder cannot handle the real thing*. `[derived]`

### 13.2 Requirements

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-MOD-01** | Must | The device shall be in exactly one of three modes, selected by `mode <1|2|3>`, defaulting to **3** at boot, and a change shall take effect at the next period boundary rather than interrupting an emission in progress. | `[user]` |
| **FR-MOD-02** | Must | Mode 1 shall emit a burst of `0x55` on every period boundary, of a length that leaves the FR-SCH-08 silence, rather than a continuous stream. | `[user]` |
| **FR-MOD-03** | Must | Mode 2 shall emit a telegram of one push-setup object and two registers with **fixed** values, occupying exactly one GBT block and one HDLC frame. | `[user]` |
| **FR-MOD-04** | Must | Mode 3 shall emit the Appendix A list with a load chosen per emission by a generator seeded identically at every boot, and shall choose that load **before** integrating the period, so the energy in a telegram equals the power in that same telegram times the period. | `[user]` |
| **FR-MOD-05** | Must | Every emission shall be logged on the console with its mode and the values it carried. | `[derived]` |
| **FR-MOD-06** | Must | Mode 4 shall emit `00 01 02 … FF` repeating, beginning at `0x00`, for exactly as many bytes as a mode 3 telegram in the current configuration occupies, followed by the same silence. | `[user]` |

> **Why a counted ramp is worth a mode of its own.** Loss is currently measured
> by subtracting a byte count taken on one device from a byte count taken on
> another, and trusting that both refer to the same burst. The ramp removes the
> subtraction and the trust: **the first byte received *is* the number of bytes
> lost.** Receive `0x19` and exactly 25 bytes went missing. No arithmetic, no
> second log, no correlation.
>
> It works only because the first byte sent is always `0x00`, which is why
> FR-MOD-06 states that rather than leaving it to the generator. Loss is read
> modulo 256; beyond that the count wraps, but 256 bytes is 1.2 s of line time
> at 2400 baud, so a wrap means something much larger than a lead-in problem.
> `[user]`

> **Why mode 1 bursts rather than streams.** A transmission is delimited by the
> silence that follows it, so a stream with no end is one nothing downstream ever
> reports on — the operator learns nothing, which defeats the mode. One second of
> pattern on the 5 s schedule is a delimited burst, which is the entire point.
> `[derived]`

> **Why mode 2's values are fixed and mode 3's are not.** They are being used for
> opposite purposes. Mode 2 is a *known answer*: 777 W and 12 345 Wh can be
> checked against without consulting the simulator at all. Mode 3 is a
> *challenge*: values that change every emission are what stop a decoder passing
> by caching a constant or guessing, so it must genuinely decode each message —
> which is why FR-MOD-05 requires the simulator to record what it sent. `[user]`

> **Random, but replayable.** FR-CLK-04 requires two runs from reset to produce
> identical bytes, and mode 3 varies its values. Both hold because the generator
> is seeded from a constant: unpredictable downstream, reproducible for us.
> `[derived]`

### 13.3 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-MOD-01 | `mode 2` mid-emission | Current emission completes intact; next is mode 2 | A truncated frame at the switch | bench |
| FR-MOD-02 | `mode 1`, capture the line | Every byte on the line is `0x55`, once per period | A continuous stream with no gap | bench |
| FR-MOD-03 | Encode mode 2; count `7E A0` openings; decode | Exactly 1 frame; 777 W and 12 345 Wh | More than one frame; any decode failure | host |
| FR-MOD-04 | 50 draws from the seeded generator | >25 distinct loads; import and export never both non-zero | A constant; both directions at once | host |
| FR-MOD-04 | Two runs from reset, same seed | Identical load sequence | Any divergence | host |
| FR-MOD-04 | 12 emissions; compare energy to Σ(P × period) | Equal | Energy that does not match the powers reported | host |
| FR-MOD-06 | Generate the ramp; drop the first *k* bytes for k = 1, 25, 255 | The new first byte equals *k* every time | A first byte that is not the loss count | host |
| FR-MOD-06 | `mode 4` on the bench | Byte count and wire time equal a mode 3 telegram's | A burst of a different length | bench |
| FR-MOD-05 | Any emission | Console line names mode and values | A mode-3 emission whose values are unrecorded | bench |

---

## 14. Runtime Directives

### 14.1 Purpose

Modes (§13) say what a correct meter sounds like. This chapter says how to make
it misbehave on purpose — a corrupted checksum, a missing block, a burst that
starts mid-frame, silence — so that error handling downstream can be provoked
rather than waited for. `[user]`

Five constraints shape the whole set, and each one exists because its opposite
was tried somewhere and hurt:

1. **Everything is runtime, over the console. Nothing is Kconfig.** A fault that
   needs a rebuild makes the firmware image part of the test fixture, and a bench
   run then cannot say which simulator produced its evidence.
2. **The simulator reports what it emitted, machine-readably.** A test that
   corrupts frame 2 of 3 must be able to assert *that frame was the corrupted
   one*. Without the report, "something downstream discarded something" is the
   only available observation.
3. **One fault at a time, and it clears itself.** A sticky corruption silently
   poisons every later case in the run.
4. **Faults ride on fixed values.** An arithmetic assertion cannot be made
   against a load that changes every emission, so a fault never composes with
   mode 3's generator.
5. **Silence is a capability, not the absence of one.** "The meter stopped
   talking while the rig kept running" is a case, and it needs to be commanded.

### 14.2 Requirements

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-FLT-01** | Must | At most one fault shall be armed at a time; an armed fault shall apply to exactly one telegram and then clear itself; and a faulted telegram shall carry the fixed load of `fixed_model()` regardless of the active mode. | `[user]` |
| **FR-FLT-02** | Must | `fault fcs <n>` shall emit a telegram in which frame *n*'s FCS is inverted and every other byte of every frame, including frame *n*'s, is unchanged. | `[user]` |
| **FR-FLT-03** | Must | `fault truncate <n>` shall cut frame *n* inside its information field, emitting no FCS and no closing flag for it. | `[user]` |
| **FR-FLT-04** | Must | `fault drop-block <k>` shall omit GBT block *k* entirely, so that the block numbers on the wire show the gap and the APDU cannot be completed. | `[user]` |
| **FR-FLT-05** | Must | `fault start-mid-frame` shall begin transmission partway through frame 1, leaving every later frame byte-identical to an unfaulted emission. | `[user]` |
| **FR-FLT-06** | Must | `fault duplicate-register <obis>` shall emit one telegram carrying that OBIS twice, with a **different** value the second time. | `[user]` |
| **FR-FLT-07** | Must | `fault noise <n>` shall emit exactly *n* bytes containing no `0x7E` and no valid framing, then fall silent. | `[user]` |
| **FR-FLT-08** | Must | `gap <ms>` shall set the silence between the last byte of one telegram and the first of the next, to a resolution of 1 ms; `gap 0` shall restore the wall-clock schedule of FR-SCH-01. | `[user]` |
| **FR-FLT-09** | Must | `silence <seconds>` shall stop transmission for that long while the clock, the console and the schedule continue, and shall resume on the schedule rather than immediately. | `[user]` |
| **FR-FLT-10** | Must | `identity <ldn\|devid\|none>` shall select which identity object the telegram carries — `0-0:42.0.0`, `0-0:96.1.0`, or neither. | `[user]` |
| **FR-FLT-11** | Must | `serial <8\|16>` shall set the identity serial to that many characters, with no rebuild. | `[user]` |
| **FR-FLT-12** | Must | `energy <wh>` shall set the cumulative import register to exactly that value, carried as a 4-byte unsigned integer, for any value up to 2³²−1. | `[user]` |
| **FR-FLT-13** | Must | `scaler <n>` shall emit registers in the self-describing form carrying scaler *n*, such that the transmitted value is `raw × 10ⁿ` applied exactly once; `scaler 0` shall restore bare values. | `[user]` |
| **FR-FLT-14** | Must | Every emission shall be reported on the console as `key=value` pairs including the emission number, mode, fault applied, byte count and frame count. | `[user]` |
| **FR-FLT-15** | Must | Every directive in this chapter shall be settable at runtime over the console, and none shall be a build-time option. | `[user]` |
| **FR-FLT-16** | Must | `preamble <n> [byte]` shall prepend *n* bytes of filler ahead of the first frame **inside the same burst**, defaulting to `0x7E`, leaving the telegram behind it byte-identical. | `[user]` |
| **FR-FLT-17** | Must | The emission report shall carry the **measured** transmission duration `tx_ms` alongside the computed `wire_ms`, and the delay from transmission starting to the line leaving idle as `lead_us` — reporting `n/a` when no line-sense input is wired rather than substituting a quantity it can measure. | `[user]` |

> **On FR-FLT-16 — the faithful filler is not the safe one.** HDLC calls
> repeated flags inter-frame fill, so `0x7E` is what a real meter's lead-in
> would look like, and it is the default for that reason. It is also, measured
> against `dlms_parser` v2.1.0, **fatal**: one single leading `0x7E` takes a
> telegram from 12 decoded captures to zero, and thirty-two do the same. A
> non-flag filler costs only the first frame — 6 captures of 12 — and the
> receiver resynchronises for the rest.
>
> That is why the filler byte is a parameter and not a constant. A directive
> built to answer "is a lead-in a viable mitigation?" must be able to express
> the alternatives, and the answer for at least one decoder is "not with flags".
> `[measured]`

> **On FR-FLT-17 — what cannot be measured, and why it says so.** The quantity
> asked for is the delay between *the line* leaving idle and the first data bit.
> The firmware cannot see the line: it drives GPIO17 into a driver stage and has
> no path back from the bus. It knows only when it handed bytes to the UART,
> which is a different quantity, and reporting that as though it were the answer
> would put a number where an unknown is.
>
> So `lead_us` reports `n/a` unless `SIM_LINE_SENSE_GPIO` names an input wired
> back from the **driver stage's output**, at which point an edge-triggered ISR
> timestamps the first transition and the figure is real to about a microsecond.
> The pin is Kconfig rather than a console directive because it is a fact about
> the wiring, like the TX pin — not a test parameter.
>
> **In practice this is measured with an oscilloscope and the pin stays unwired**
> `[user]`, which is the better instrument anyway: a scope shows the rise time
> and the settling, where a GPIO shows one threshold crossing. Mode 4 makes the
> trigger easy — `0x00` at 8E1 is ten consecutive zero bits, a 4.17 ms low after
> 3.25 s of idle, and it is the first byte of every ramp burst. The number to
> compare settling against is one bit time, **417 µs**. The firmware path exists
> for unattended runs, and until one needs it, it stays dormant.
>
> `tx_ms` needs no extra hardware and is worth having on its own: `wire_ms` is
> arithmetic on the byte count and would read 1747 ms even if the UART were
> misconfigured. The two disagreeing is a finding. `[derived]`

> **On FR-FLT-13 — the scaler is a different shape, and that is deliberate.** The
> E450 carries bare values with no scaler anywhere (FR-ENC-04), so a decoder given
> a real telegram has nothing to scale by and the directive would be unobservable.
> Setting a scaler therefore switches the registers to the self-describing form
> `structure[3] { OBIS, value, structure[2]{ scaler, unit } }` — valid DLMS, and
> what plenty of other meters emit, but **not** what this meter emits. It is a
> decoder exercise, not a fidelity claim, and it appears only when asked for.
> `[derived]`

> **On FR-FLT-08 — `gap` suspends FR-SCH-01.** Wall-clock alignment and an exact
> inter-telegram gap are incompatible: one puts telegrams on multiples of five
> seconds, the other measures from the last byte out. A boundary case at 1999 vs
> 2000 ms can only be placed by the second, so `gap` deliberately switches the
> scheduler to free-running until set back to 0. `[derived]`

> **On FR-FLT-12 — the one the host tier cannot close.** 16 777 217 is 2³²'s
> little brother: 2²⁴ + 1, the first integer a 32-bit float cannot represent. The
> host test proves the encoder carries it exactly, by finding `06 01 00 00 01` in
> the APDU. It cannot prove what a decoder does with it, because the oracle's own
> accessor returns a `float` and quantises it back to 16 777 216 — which the test
> prints rather than hides. Closing this one needs a bench run against something
> that does not use a float carrier. `[derived]`

### 14.3 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-FLT-01 | Arm any fault; observe two telegrams | Fault on the first, `fault=none` on the second | A fault surviving into a later telegram | host, bench |
| FR-FLT-01 | Encode a faulted telegram in mode 3 | Load equals `fixed_model()`, not the generator's | Random values in a fault case | host |
| FR-FLT-02 | `fault fcs 2`; recompute every frame's CRC independently | Frame 2 fails, all others pass; total length unchanged | Any other frame damaged; a length change | host |
| FR-FLT-03 | `fault truncate 1` | Frame 1's declared length exceeds the bytes present; no closing flag | A short but internally consistent frame | host |
| FR-FLT-04 | `fault drop-block 2` | Block numbers on the wire are 1, 3; surviving frames pass CRC | Block 2 present; a damaged survivor | host |
| FR-FLT-05 | `fault start-mid-frame` | Stream does not begin on `0x7E`; frame 2 byte-identical to unfaulted | Any change to a later frame | host |
| FR-FLT-06 | `fault duplicate-register 1.1.1.8.0.255` | The OBIS appears twice, values differ | Two identical values — the case would prove nothing | host |
| FR-FLT-07 | `fault noise 64` | Exactly 64 bytes, no `0x7E`, decoder matches nothing | Any byte that could be read as a flag | host |
| FR-FLT-08 | `gap 2000`, then `gap 1999`, capture the line | Measured silence tracks the setting to ±1 ms | Wall-clock alignment still in force | bench |
| FR-FLT-09 | `silence 30`; poll `status` throughout | Nothing emitted; clock advances; console answers | A reset; a telegram; a frozen clock | bench |
| FR-FLT-10 | Each of the three settings | The named object present, the other two absent | Both identity objects at once | host |
| FR-FLT-11 | `serial 8` then `serial 16` | Octet string of 8 then 16 characters | A rebuild required | host |
| FR-FLT-12 | `energy 16777217`; inspect the APDU | `06 01 00 00 01` present | A float-rounded 16 777 216 on the wire | host |
| FR-FLT-13 | `scaler -2`, `0`, `2` with a 12345 register | Decoded 123.45, 12345, 1 234 500 | Scaling applied twice or not at all | host |
| FR-FLT-14 | Any emission | One `key=value` line with `n=`, `mode=`, `fault=`, `bytes=`, `frames=` | A fault applied but not reported | bench |
| FR-FLT-15 | `idf.py menuconfig` | No option for any directive in §14.2 | Any directive reachable only by rebuilding | host |
| FR-FLT-16 | `preamble 32`, both fillers; compare bytes | Burst grows by exactly 32; telegram behind it byte-identical | Any change to the telegram | host |
| FR-FLT-16 | Decode with 0, 1 and 32 flag bytes and 32 non-flag bytes | 12, 0, 0, 6 captures — the recorded characterisation | A silent change in this behaviour going unnoticed | host |
| FR-FLT-17 | Any emission with no sense pin wired | `lead_us=n/a` | A number presented as a line measurement | bench |
| FR-FLT-17 | Scope on the bus, mode 4, trigger on the first falling edge | Settling time recorded against one bit time (417 µs at 2400 baud) | Treating `n/a` as though the delay were zero | operator |
| FR-FLT-17 | Any emission | `tx_ms` within 5 % of `wire_ms` | A silent disagreement between the two | bench |

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
| FR-ENC-10 | Encode one telegram; recompute both CRCs independently | Both match | Any frame emitted with a wrong HCS or FCS | host |
| FR-ENC-12 | `cmake -S test/host && ctest` on Linux, no ESP-IDF present | Builds and passes | Any ESP-IDF header required | host |

---

## 8. Control Console

### 8.1 Purpose & peer

The simulator's entire public API. Its peer is a human at a terminal or
`WorkbenchDriver.serial_write()` over the workbench's RFC2217 proxy — which is
why it is a serial line and not a network service: it needs no provisioning, no
broker and no IP, and it cannot interfere with any network in use around it. `[user]`

### 8.2 Requirements

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-CLI-01** | Must | The device shall expose a line-based console on UART0 at 115200 8N1. | `[user]` |
| **FR-CLI-02** | Must | The console shall implement: `mode [1|2|3]`, `status`, `set time <ISO-8601>`, `help`, and the directives of §14 — `fault`, `gap`, `silence`, `serial`, `identity`, `energy`, `scaler`. | `[user]` |
| **FR-CLI-03** | Must | Every command shall reply on one line, beginning `OK` or `ERR `, before any further output. | `[derived]` |
| **FR-CLI-04** | Must | `status` shall print the complete state — both powers, power factor, all accumulators in Wh, clock, current mode, push period, emission count — as one `key=value` line. | `[derived]` |
| **FR-CLI-05** | Must | A malformed or out-of-range argument shall leave the model unchanged and reply `ERR`. | `[derived]` |
| **FR-CLI-06** | Must | Console output shall never be written to UART2. | `[derived]` |
| **FR-CLI-07** | ~~Must~~ | `superseded` 2026-08-05 by **FR-MOD-02** — `tx [seconds]` shall emit `0x55` on the meter link. The pattern is now a mode rather than a one-shot command, so it repeats on the schedule and needs no `pause` around it. | `[user]` |

> **Why a requirement for a test pattern.** `0x55` alternates every bit, so it
> separates the line settings from everything layered on top: no framing, no
> checksums and no content have to be right for it to mean something. What the
> comes off the line names the fault — `0x55` clean, `0xD5` one inversion out
> (FR-LNK-02), varying bytes a wrong rate. It is the first thing to reach for
> when nothing decodes, and it is how FR-LNK-02 was settled.

### 8.3 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-CLI-03/04 | `set pi 1500` then `status` | `OK` then one line containing `pi=1500` | Multi-line or unparsable status | bench |
| FR-CLI-05 | `set pi -5`, then `status` | `ERR`; `pi` unchanged | Negative or wrapped power accepted | bench |
| FR-CLI-06 | Issue 100 commands while monitoring UART2 | Only telegram bytes on UART2 | Any console character on the meter link | target |
| FR-CLI-07 | `tx 10` while capturing the meter link | `OK 2180 bytes…`; every byte captured is `0x55` | Any telegram byte interleaved into the pattern | bench |

---

# Part C — Foundation (L0)

## 9. Meter Link Transport

### 9.1 Requirements

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-LNK-01** | Must | The device shall transmit on UART2, TX on a configurable GPIO defaulting to **17**, at 2400 baud, 8 data bits, even parity, 1 stop bit. | `[doc:LG]` §3.1 |
| **FR-LNK-02** | Must | The TX signal shall **not** be inverted at boot: the signal this rig puts on the bus is not inverted. | `[user]`, `[measured]` |
| **FR-LNK-06** | Must | `invert [on\|off]` shall change the line polarity for the remainder of the session, take effect between telegrams and never mid-telegram, and be reported on every emission and by `status`. | `[user]` |
| **FR-LNK-03** | Must | The device shall never enable a receiver on UART2. | `[derived]` |
| **FR-LNK-04** | Must | Between telegrams the line shall rest in the mark state, presenting 24 V on the bus. | `[derived]` |
| **FR-LNK-05** | Must | The TX GPIO shall be driven to the mark-producing level before UART initialisation and within 10 ms of reset. | `[derived]` |

> **FR-LNK-05 is the one that bites.** UART idle must already be on the pin
> before the peripheral takes it over — **high** with FR-LNK-02 as written, low if
> inversion is ever turned back on. From reset until `app_main` configures it,
> that pin is a floating input and the bus level is undefined, so without this
> every simulator reboot may present the bus with a break condition or noise.

> **Why polarity is runtime as well as build-time.** FR-LNK-02 was settled by
> flipping `SIM_TX_INVERT` and reflashing — which meant the firmware image was
> part of the experiment measuring it, the thing §14's first constraint exists to
> prevent. FR-LNK-06 makes the same test one console command, so a bench run can
> try both polarities against one binary and say which one it used. The Kconfig
> option survives as the boot default, because a rig permanently wired to an
> inverting driver should come up correct without anyone typing anything.
>
> Flipping the polarity flips the idle level, so the line makes **one
> transition** at the moment of the change. That is unavoidable and harmless
> between telegrams — but it is why FR-LNK-06 requires the change to be taken
> under the transmit lock: applied mid-telegram it would invert the tail of a
> frame and nothing else, a corruption no fault directive describes. `[derived]`

> **How FR-LNK-02 was settled.** It was originally written the other way round,
> from a description of the driver stage as inverting, and never measured. The
> `tx` command (FR-CLI-07) then sent a continuous `0x55` and a 2400 8E1 capture showed a
> uniform `0xD5` across all 520 bytes received. That byte is not a symptom of a
> wrong rate, which produces varying bytes: inverting a `0x55` 8E1 frame moves
> the receiver's start-bit edge one bit late, so it samples `1 0 1 0 1 0 1 1` —
> `0xD5` — and flags a parity error, every frame, identically. One inversion too
> many, and the user confirmed the simulator's output is not inverted.

### 9.2 Verification contracts

| ID | Precondition · stimulus | Expected | **Prohibited** | Tier |
|---|---|---|---|---|
| FR-LNK-01/02 | `tx 10` (FR-CLI-07), captured at 2400 8E1 | Every byte captured is `0x55` | `0xD5` (one inversion out); varying bytes (wrong rate); no bytes | bench |
| FR-LNK-06 | `mode 1`; `invert on`; capture; `invert off`; capture | The two captures are bitwise complements of each other | A polarity change that needs a reflash | bench |
| FR-LNK-06 | `invert on` issued mid-telegram | The telegram in flight completes at the old polarity | A frame whose tail is inverted | bench |
| FR-LNK-06 | `invert on`, then `status` and the next emission line | Both report `invert=1` | A polarity applied but unreported | bench |
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

## 11. Downstream Power — **withdrawn**

| ID | Pri | Requirement | Provenance |
|---|---|---|---|
| **FR-PWR-01** | ~~Must~~ | `deprecated` 2026-08-05 — the rig shall supply 5 V, ground and the Data Request line on the connector. | `[user]` |
| **FR-PWR-02** | ~~Must~~ | `deprecated` 2026-08-05 — a console command shall remove and restore that 5 V under firmware control. | `[user]` |
| **FR-PWR-03** | ~~Must~~ | `deprecated` 2026-08-05 — removing that power shall not alter the model, the clock or the schedule. | `[derived]` |

> **Why these are withdrawn, and why the IDs stay.** The premise was a separate
> switched 5 V rail on GPIO16. There is no such rail: **the pin is not connected,
> and an M-Bus slave is powered from the signal itself** — the 24 V mark state
> that FR-LNK-04 requires the idle line to hold is what feeds it. `[user]`
>
> So the simulator has no way to remove that power without ceasing to be a
> meter, and the commands were driving an unconnected pin and replying `OK`.
> They, the two Kconfig options and the corresponding `status` field are
> removed. The IDs are kept and struck through rather than
> deleted, so that earlier results referring to them stay readable.
>
> Power-cycling whatever is on the bus is still useful and is now **out of scope**:
> it would need a hardware enable on the driver stage, which does not exist. An
> operator does it by hand.

---

# Part D — Verification

## 12. Test Architecture

| Tier | Runs on | Needs | Covers |
|---|---|---|---|
| **host** | Linux, GitHub Actions, every push | Nothing | §5 model arithmetic, §7 encoder — decoded by `dlms_parser` v2.1.0 |
| **target** | The ESP32 alone | Logic analyser or second ESP32 | §6 timing, §9 line, §10 determinism |
| **bench** | Workbench + a capture of the line | Board, interface, workbench Pi | §8 console, §9 line settings, §6 cycle timing, end-to-end |

**The host oracle is a decoder we did not write.** `test/host/CMakeLists.txt`
fetches `dlms_parser` at tag **v2.1.0**, so a host pass means the output is
readable by an independent implementation. This proves *decodable*, not *faithful*: a
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
which v1 does not emit. Anything tested against this simulator therefore never
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

[The operator's manual](../UserDocumentation/MBUS-Simulator-Manual.md) ·
`docs/Standard CIP-Liste_DE.pdf` (Landis+Gyr, 16 Sep 2024)
