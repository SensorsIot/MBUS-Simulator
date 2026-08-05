# MBUS-Simulator — User Manual

A fake electricity meter for testing meter readers.

Technical detail is deliberately not repeated here — where you need it, this
manual points at the [FSD](../Functionality/MBUS-Simulator-FSD.md).

---

## 1. What it does

It pretends to be the customer interface of a Landis+Gyr E450 smart meter, the
kind installed by EBL Liestal. Every five seconds it sends out a reading —
power, energy, reactive energy and current — exactly as the real meter would, so
you can develop and test a meter reader without having a meter.

**You tell it what the household is doing; it behaves accordingly.** Ask for a
2300 W load and the reader will show 2300 W, and its energy counter will climb
by 3.2 Wh every five seconds — because the simulator integrates the power you
set, rather than replaying a recording. If the reader gets the arithmetic wrong,
you find out.

You control it by typing commands on a serial console. There is no app, no web
page and no WiFi.

What it does **not** do: it is not a meter, measures nothing, and has no
metrological standing. See chapter 6 for the limits that matter in practice.

---

## 2. How it is connected

```
   ESP32                interface board              your reader
  ┌──────┐   GPIO17    ┌──────────────┐  12/24 V   ┌──────────┐
  │  sim │ ──────────► │  M-Bus driver│ ─────────► │  reader  │
  └──────┘             └──────────────┘            └──────────┘
      │
      │ USB — console, and how you flash it
      ▼
   your PC, or a workbench slot
```

Three connections and that is all:

| | |
|---|---|
| **GPIO17 → interface board** | The meter signal |
| **Ground** | Common between the boards |
| **USB** | Console and firmware, to your PC or a workbench slot |

**The reader is powered by the meter signal itself** — it has no supply of its
own, and there is no separate power wire to connect. That also means you cannot
switch it off from the console; unplug it by hand.

The board is an ESP32 classic (WROOM). Line settings and pin choices are
FSD §2.1 and §9.

---

## 3. Installing it

### 3.1 Get the firmware

It is built automatically on every change. Download the latest:

```bash
gh run download -R SensorsIot/MBUS-Simulator -D /tmp/fw
cd /tmp/fw/firmware-esp32
```

### 3.2 Put it on the board

**On a workbench** — find the address and the slot first, they are not fixed:

```bash
export WORKBENCH_URL=http://192.168.0.27:8080     # yours may differ
curl -s $WORKBENCH_URL/api/devices                # which slot is the board in?

curl -X POST $WORKBENCH_URL/api/flash \
  -F slot=SLOT3 -F chip=esp32 -F baud=460800 \
  -F flash_args=@flash_args \
  -F bootloader.bin=@bootloader/bootloader.bin \
  -F partition-table.bin=@partition_table/partition-table.bin \
  -F MBUS-Simulator.bin=@MBUS-Simulator.bin
```

**On a USB cable:**

```bash
esptool --port /dev/ttyUSB0 --chip esp32 write-flash @flash_args
```

### 3.3 Check it took

Open the console (4.1) and reset the board. It says what it is:

```
I (203) app_init: Project name:     MBUS-Simulator
I (285) sim: E450 simulator: EBL Consumer Information 1, 13 objects, 5 s, 2400 8E1
```

If that banner names something else, the board is running other firmware and
nothing else in this manual will make sense.

---

## 4. Using it

### 4.1 Open the console

115200 baud, prompt `sim>`. Locally:

```bash
picocom -b 115200 /dev/ttyUSB0          # or idf.py monitor, or any terminal
```

On a workbench, the port is `4000 +` the slot number — SLOT3 is `4003`:

```bash
picocom -b 115200 rfc2217://192.168.0.27:4003
```

### 4.2 A first session

Simulate a 2.3 kW load and watch the reader follow it:

```
sim> set pi 2300
OK
sim> status
OK pi=2300 po=0 pf=1.00 ei=0 eo=0 q1=0 q2=0 q3=0 q4=0 i=3333 …
```

Within five seconds the reader should report **2300 W**, and its import energy
counter should rise by about 3.2 Wh per reading. That is the basic round trip —
if it works, everything downstream of the simulator is fine.

### 4.3 Commands

| Command | What it does |
|---|---|
| `set pi <watts>` | Power being **consumed** — the normal case |
| `set po <watts>` | Power being **fed back**, e.g. from solar |
| `set pf <0.00–1.00> [leading]` | Power factor. Inductive unless you say `leading` |
| `set time <YYYY-MM-DDTHH:MM:SS>` | Move the clock |
| `status` | Show everything, on one line |
| `burst` | Send one reading immediately |
| `pause` / `resume` | Stop and restart sending |
| `tx [seconds]` | Send a test pattern instead of readings — see 5.2 |
| `help` | List the commands |

Every command answers `OK` or `ERR …` on a single line. `ERR` changes nothing.

### 4.4 Reading `status`

```
OK pi=2300 po=0 pf=1.00 ei=0 eo=0 q1=0 q2=0 q3=0 q4=0 i=3333
   time=2026-01-01T00:00:28 paused=0 period=5 telegrams=6
```

**Three fields are what you asked for; the rest are what follows from it.**

| You set | |
|---|---|
| `pi` / `po` | Consumed / fed-back power, W |
| `pf` | Power factor; a trailing `L` as in `pf=0.90L` means leading |

| It works out | |
|---|---|
| `ei` / `eo` | Energy consumed / fed back, Wh — the running totals |
| `q1`…`q4` | Reactive energy, varh. All four stay at 0 while `pf` is 1.00 |
| `i` | Current, mA — worked out from the power, not measured |
| `telegrams` | Readings sent since power-on |

| Housekeeping | |
|---|---|
| `time` | The clock |
| `paused` | 1 if you have paused it |
| `period` | Seconds between readings |

Two things that look wrong and are not:

- **`ei` can sit at 0 while power flows.** Energy accrues from the power set *at
  the time*, so it counts from when you set the load, not before.
- **`ei` steps unevenly** — 1, 3, 5, 6, 8, 10. It shows whole watt-hours while
  counting in fractions, so a steady load gives uneven-looking steps. Nothing is
  lost; see FSD FR-MDL-04.

**The clock starts from a fixed date, not today's.** That is on purpose: the same
test run twice produces identical output, which is impossible with a real clock.
`set time` moves it.

---

## 5. When something looks wrong

Work down in order. Each step rules something out, so the first failure tells
you where the problem is.

### 5.1 Is it sending anything?

Watch the console. A line appears every five seconds:

```
I (32082) sim: telegram 6: 381 bytes on GPIO17, 1747 ms on the wire, 3253 ms silent
```

- **It appears** → the simulator is fine; the problem is the wiring, the
  interface board, or the reader. Go to 5.2.
- **It does not** → check `status` for `paused=1`, then re-check the banner
  (3.3). A board running different firmware looks exactly like a broken one.

### 5.2 Is the signal getting through correctly?

`tx` sends a plain test pattern — the byte `0x55`, which is just alternating
bits — instead of readings. Nothing complicated has to be right for it to mean
something, which is why it is the fastest way to find a wiring or settings
fault.

```
sim> pause
sim> tx 5
OK 1090 bytes of 0x55 on GPIO17
sim> resume
```

Now look at what your reader received:

| It received | What is wrong |
|---|---|
| `55 55 55 …` | Nothing — the signal is correct. The fault is further along. |
| `D5 D5 D5 …` or `AA AA AA …` | The signal is inverted. Change `SIM_TX_INVERT` (chapter 7). |
| Bytes that vary, but always the same way | Wrong speed or parity setting at the reader. |
| Nothing at all | Wiring, or the interface board. The reader takes its power from this signal, so no signal also means no reader. |

`D5` looks like nonsense but is not: consistent nonsense means an inverted
signal, not interference. The reasoning is in FSD FR-LNK-02.

### 5.3 The reader gets data but cannot read it

Check the **`ms silent`** figure on the console line above. Readers decide a
transmission has finished when the line goes quiet for about two seconds, so
that number needs to stay comfortably above 2000. If you have shortened the
period it may not be. The simulator prints an error line of its own when this
happens — see FSD FR-SCH-08.

A reader complaining about oversized data, or reporting far more bytes than the
simulator sent, means readings were run together — same cause.

### 5.4 The reader seems dead

It is powered by the meter signal, so "no power" and "no signal" are the same
fault. Confirm the interface board is producing 24 V on the line while the
simulator is idle. That needs a meter on the wire; the console cannot tell you.

---

## 6. Settings

Changed before building, under **MBUS Simulator** in `idf.py menuconfig`.

| Setting | Default | Change it when |
|---|---|---|
| `SIM_MBUS_TX_GPIO` | 17 | The interface board is on another pin |
| `SIM_TX_INVERT` | off | 5.2 says the signal is inverted |
| `SIM_PUSH_PERIOD_SECONDS` | 5 | You need longer gaps between readings |
| `SIM_EPOCH` | 2026-01-01 | You want a different starting date |

Five seconds is what a real E450 does, so leave it alone unless you have a
reason.

---

## 7. What it will not do

Worth knowing before you trust a test result:

- **No meter serial number.** The real meter sends it in a different, slower
  list that this simulator does not produce. A reader waiting to learn the
  meter's identity will wait forever here.
- **Nothing is encrypted.** Real installations often are.
- **One kind of reading only** — the five-second list. No 15-second, minute or
  quarter-hour lists.
- **Passing here is necessary, not sufficient.** It proves a reader *can* read
  this output. A forgiving reader would also accept things a real meter never
  sends.

Full scope and the reasoning behind each: FSD §1.5 and Appendix A.
