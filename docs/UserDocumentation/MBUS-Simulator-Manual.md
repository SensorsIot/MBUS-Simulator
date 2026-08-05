# MBUS-Simulator — Operator's Manual

How to install, run, and diagnose the E450 simulator. One document, chapters
below; if you are looking for *what it must do* see the
[FSD](../Functionality/MBUS-Simulator-FSD.md), and if you are looking for *how
it is built* see the source.

**Status:** covers everything the simulator does today. Chapter 6 is the one to
read when something is wrong.

---

## 1. What this is, and what you need

An ESP32 that pretends to be the customer-information interface of a Landis+Gyr
E450 as parametrised by EBL Liestal. It pushes a telegram every 5 seconds
carrying thirteen objects — power, energy, reactive energy by quadrant, and the
three phase currents — so a meter reader can be developed and regression-tested
without a meter.

It **generates**, it does not replay: the energy counters integrate the power you
set, so `set pi 2300` makes the reader see 2300 W and watch its import counter
climb by 3.2 Wh every five seconds.

To operate it you need:

| | |
|---|---|
| The simulator board | ESP32 classic (WROOM), on a workbench slot |
| The interface board | Non-inverting driver, logic level → 12/24 V M-Bus, fed from **GPIO17** |
| A reader | Powered from the rig's 5 V, not from its own supply |
| A workbench | Or a direct USB cable — chapter 3 covers both |

There is no WiFi, no MQTT and no web interface. The console on UART0 is the
entire user interface, by design: it needs no provisioning and it cannot
interfere with the network the reader is being tested on.

---

## 2. Wiring

| Signal | Where | Notes |
|---|---|---|
| M-Bus data | **GPIO17** → driver input | UART2 TX, 2400 baud, 8 data bits, even parity, 1 stop bit |
| Idle level | GPIO17 **high** | Driver is non-inverting; high = 24 V on the bus = the mark state |
| DUT 5 V enable | **GPIO16** | Switches the rail the reader runs on, so `dut off` can power-cycle it |
| Ground | Common | |

The reader has **no supply of its own** — a real meter feeds it — so a rig that
drives only the data line leaves it dead and looking like a wiring fault.

If your driver stage inverts, set `SIM_TX_INVERT=y` (chapter 7) and the idle
level flips to low. Chapter 6.2 tells you which you have, in ten seconds,
without a scope.

---

## 3. Installing the firmware

The firmware is built in GitHub Actions, not locally — there is no ESP-IDF
toolchain in this repo's devcontainer. Take the binaries from a green run.

```bash
. /run/secrets/env                      # gh needs its token in a non-interactive shell
gh run download -R SensorsIot/MBUS-Simulator -D /tmp/fw
ls /tmp/fw/firmware-esp32/
# flash_args  bootloader/  partition_table/  MBUS-Simulator.bin  ...
```

### 3.1 On a workbench

Find the bench and the slot first — never assume either:

```bash
export WORKBENCH_URL=http://192.168.0.27:8080      # Workbench2; yours may differ
curl -s $WORKBENCH_URL/api/devices | jq '.slots[] | {label, state, present, detected_chip}'
```

Then flash. The part names must equal the basenames `flash_args` refers to:

```bash
cd /tmp/fw/firmware-esp32
curl -s -X POST $WORKBENCH_URL/api/flash \
  -F slot=SLOT3 -F chip=esp32 -F baud=460800 \
  -F flash_args=@flash_args \
  -F bootloader.bin=@bootloader/bootloader.bin \
  -F partition-table.bin=@partition_table/partition-table.bin \
  -F MBUS-Simulator.bin=@MBUS-Simulator.bin | tail -c 200
```

The board is a CP2102 bridge, so it **must** be flashed this way rather than by
pointing esptool at the RFC2217 proxy: the auto-reset window is too short to
survive the network round-trips. `/api/flash` resets the board itself when it
finishes — do not follow it with `/api/serial/reset`.

### 3.2 Over a local USB cable

```bash
esptool --port /dev/ttyUSB0 --chip esp32 \
  --before default-reset --after hard-reset \
  write-flash @flash_args
```

### 3.3 Confirming what is running

The banner names the firmware, and it is the first thing to check when the
simulator "does nothing" — a board can easily be running something else
entirely:

```
I (203) app_init: Project name:     MBUS-Simulator
I (285) sim: E450 simulator: EBL Consumer Information 1, 13 objects, 5 s, 2400 8E1
```

If the banner ends with `inverted`, TX inversion is on (chapter 7).

---

## 4. Talking to it

The console is UART0 at **115200 8N1**, prompt `sim>`. Over a workbench, the
RFC2217 port is `4000 + slot number` — SLOT3 is `4003`:

```bash
python3 -c "
import serial
s = serial.serial_for_url('rfc2217://192.168.0.27:4003', baudrate=115200, timeout=1)
s.write(b'status\r\n')
import time; time.sleep(1)
print(s.read(1000).decode())"
```

Locally, any terminal will do: `idf.py -p /dev/ttyUSB0 monitor`, `picocom -b
115200 /dev/ttyUSB0`, whatever you have.

Every command answers on **one line**, beginning `OK` or `ERR `, before any
other output. That is a promise you can parse against.

### 4.1 Command reference

| Command | Does | Replies |
|---|---|---|
| `set pi <W>` | Import (consumed) active power, 0…1 000 000 W | `OK` |
| `set po <W>` | Export (produced) active power | `OK` |
| `set pf <0.00–1.00> [leading]` | Power factor; lagging unless `leading` is given | `OK` |
| `set time <YYYY-MM-DDTHH:MM:SS>` | Set the synthetic clock | `OK` |
| `status` | Whole model state, one `key=value` line | `OK pi=… ` |
| `burst` | Emit one telegram now, without shifting the schedule | `OK` |
| `pause` | Stop emitting **and** stop integrating | `OK` |
| `resume` | Start again | `OK` |
| `dut on` / `dut off` | Switch the reader's 5 V rail | `OK` |
| `tx [1–30]` | Hold the line and send only `0x55` — see 6.2 | `OK 1090 bytes of 0x55 on GPIO17` |
| `help` | List the above | |

`ERR` leaves the model untouched, always. `set pi -5` changes nothing.

### 4.2 Reading `status`

```
OK pi=2300 po=0 pf=1.00 ei=0 eo=0 q1=0 q2=0 q3=0 q4=0 i=3333
   time=2026-01-01T00:00:28 paused=0 dut=1 period=5 telegrams=6
```

| Field | Meaning |
|---|---|
| `pi` / `po` | Import / export active power, W |
| `pf` | Power factor; a trailing `L` means leading |
| `ei` / `eo` | Import / export active energy, Wh |
| `q1`…`q4` | Reactive energy per quadrant, varh |
| `i` | Line current, mA — derived as `P / (3 × 230 V)` |
| `time` | The synthetic clock |
| `paused` | 1 while paused |
| `dut` | 1 when the reader's 5 V rail is on |
| `period` | Seconds between telegrams |
| `telegrams` | Count since boot |

**The clock is synthetic and starts at a compiled-in epoch**, not at the real
date. That is deliberate: the same test run twice produces the same bytes, which
is impossible with a real clock. `set time` moves it.

---

## 5. What healthy looks like

Simulator side, one line per telegram:

```
I (32082) sim: telegram 6: 381 bytes on GPIO17, 1747 ms on the wire, 3253 ms silent
```

Three numbers to know:

- **381 bytes** — the thirteen-object list. Constant; the values are fixed-width,
  so it does not change with the load you set.
- **1747 ms on the wire** — at 2400 baud with 11 bits per character, the line
  carries 218 bytes per second and nothing faster.
- **3253 ms silent** — the gap before the next telegram. This one matters: a
  reader decides a transmission has ended after about 2 s of quiet, so if the gap
  ever fell below that it would splice two telegrams into one and decode neither.
  The firmware prints one `E`-level line if it drops under 3000 ms.

Reader side, one cycle per telegram:

```
gplug: active_power_plus  = 2300.000 W
gplug: cycle: 381 bytes, 12 objects, 301 consumed
```

**12 objects, not 13** — the first entry in the list is the push-setup's own
object list, an array of definitions rather than a reading, so twelve values come
out of thirteen objects. That is correct.

A round-trip you can do in a minute: `set pi 2300`, wait two cycles, and the
reader should say `2300.000 W` while its import counter climbs 3.2 Wh per cycle
(2300 W × 5 s = 3.19 Wh).

---

## 6. When something is wrong

Work down this chapter in order. Each step rules something out, so stopping at
the first failure tells you where the fault is.

### 6.1 Is it emitting at all?

Look for the `telegram N:` line on the simulator console. It appears every five
seconds.

- **Present** — the fault is after the pin: driver, wiring, reader, or line
  settings. Go to 6.2.
- **Absent** — the fault is before the pin. Check `status` for `paused=1`, then
  check the boot banner (3.3) actually names `MBUS-Simulator`. A board running
  some other project looks exactly like a broken simulator.

### 6.2 Are the line settings right? — the `tx` test

This is the most useful ten seconds in the manual. `0x55` is `01010101`, every
bit alternating, so it tests the line and nothing else: no framing, no checksums
and no content have to be right for the answer to mean something.

```
sim> pause
sim> tx 5
OK 1090 bytes of 0x55 on GPIO17
sim> resume
```

`tx` holds the line for its whole duration — the scheduler is suppressed, so
nothing else can get spliced into the pattern. `pause` is belt-and-braces so the
reader sees the burst alone rather than merged with a telegram.

Now read what the reader received:

| It receives | Meaning |
|---|---|
| `55 55 55 …` | Line settings correct. The fault is in the content or further up. |
| `D5 D5 D5 …` | **One inversion out.** Flip `SIM_TX_INVERT` (chapter 7). |
| `AA AA AA …` | Inverted, with the receiver re-locking differently. Same fix. |
| Bytes that vary, but consistently | Wrong baud rate or parity. |
| Nothing at all | Wiring, driver, or the reader is unpowered — check `dut on`. |

`0xD5` is worth recognising on sight, because it looks like garbage and is not.
Invert a `0x55` frame and the receiver's start-bit edge lands one bit late, so it
samples `1 0 1 0 1 0 1 1` — `0xD5` — with a parity error, identically every
frame. Perfectly consistent nonsense means polarity, not noise.

### 6.3 The reader receives bytes but decodes nothing

If 6.2 is clean and telegrams still do not decode, check the gap. On the
simulator console the `ms silent` figure must be comfortably above 2000; if you
have raised the object count or lowered the period it may not be. Raise
`SIM_PUSH_PERIOD_SECONDS` (chapter 7).

Two symptoms that name this fault directly: the reader reporting a cycle much
larger than 381 bytes, or complaining that a cycle exceeded its buffer. Both mean
transmissions were merged.

### 6.4 The reader is dead, not silent

`dut off`, wait a second, `dut on`. If `status` shows `dut=1` and the reader
still does nothing, it is not being powered by the rig — confirm the 5 V and
ground reach it, and that `SIM_DUT_POWER_GPIO` matches how it is wired.

### 6.5 Reading the console over the network

The workbench can capture a slot read-only, which is the right way to watch the
**reader**: opening its serial port directly toggles DTR, and on a native-USB
part that resets it.

```bash
curl -s -X POST $WORKBENCH_URL/api/serial/monitor \
  -H 'Content-Type: application/json' \
  -d '{"slot":"SLOT1","timeout":20}' | jq -r '.output[]'
```

---

## 7. Configuration

Build-time, via `idf.py menuconfig` under **MBUS Simulator**, or by editing
`sdkconfig.defaults` and rebuilding.

| Option | Default | Change it when |
|---|---|---|
| `SIM_MBUS_TX_GPIO` | 17 | The interface board is on a different pin |
| `SIM_TX_INVERT` | off | 6.2 says you are one inversion out |
| `SIM_PUSH_PERIOD_SECONDS` | 5 | You need a wider silent gap. Must divide 60 |
| `SIM_DUT_POWER_GPIO` | 16 | The 5 V rail is switched from another pin, or `-1` if permanent |
| `SIM_DUT_POWER_ACTIVE_HIGH` | on | The enable is active low |
| `SIM_EPOCH` | `2026-01-01T00:00:00` | You want a different starting date |

5 s is the faithful period — it is what a real E450 does — so raise it only for a
rig that needs the margin.

---

## 8. Limits worth knowing before you trust a result

- **No meter serial.** EBL puts `0-0:96.1.0` in the 15 s list, which this version
  does not emit, so a reader tested here never learns a meter identity. In the
  field it arrives every 15 s. A reader that defers work until it has one will
  defer forever against this rig.
- **Not encrypted.** Real E450 customer interfaces are commonly authenticated and
  encrypted; this one is plaintext.
- **One list only** — Consumer Information 1, every 5 s. No 15 s, 1 min or
  15 min lists.
- **Decodable is not faithful.** The host tests prove the reader's decoder
  accepts these bytes. A tolerant decoder would also accept telegrams a real
  E450 never emits, so passing here is necessary, not sufficient.
- **The clock is synthetic** and starts from the compiled-in epoch on every boot.
