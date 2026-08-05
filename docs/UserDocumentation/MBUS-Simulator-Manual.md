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

**It has three modes, and they are a ladder.** Each one is harder for a reader
than the one below, so the first that fails tells you where the problem is:

| Mode | What it sends | What it tells you |
|---|---|---|
| **1** | A plain test pattern | Whether any signal is arriving at all |
| **2** | One small reading, always the same numbers | Whether the reader can decode anything |
| **3** | The real thing, values changing every time | Whether it can decode a real meter |

Mode 3 is the default and the one you will normally use. Its values change on
every reading, deliberately: a reader cannot pass by remembering a number, it
has to decode each message. Energy still follows power exactly, so if the reader
gets the arithmetic wrong, you find out.

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
I (285) sim: E450 simulator: mode 3 (e450), every 5 s, 2400 8E1
I (285) sim: modes: 1 pattern (is there a signal?), 2 simple (can it decode?), 3 e450 (…)
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

The simulator starts in mode 3 and is already sending. Watch the console:

```
sim> status
OK pi=4820 po=0 pf=0.93 ei=27 eo=0 … mode=3/e450 period=5 emissions=6
```

Every five seconds it logs what it sent:

```
I sim: e450 7: 381 bytes on GPIO17, 1747 ms on the wire, 3253 ms silent - pi=4820 po=0 ei=27 eo=0 i=6985
```

**Compare that line against what your reader reports.** They should agree — same
power, same energy. That is the whole test, and because mode 3's values change
every time, agreement means the reader really decoded the message.

If they do not agree, drop down the ladder:

```
sim> mode 2
OK 2 simple
```

Mode 2 sends the same two numbers every time — **777 W** and **12345 Wh**. If the
reader shows those, it can decode; the problem is with the full E450 telegram.
If it shows nothing, go to `mode 1` and chapter 5.2.

### 4.3 Commands

| Command | What it does |
|---|---|
| `mode` | Say which mode is running |
| `mode 1` | Test pattern — is a signal arriving? |
| `mode 2` | Small fixed reading — can the reader decode? |
| `mode 3` | Full E450, changing values — can it decode the real thing? |
| `status` | Show everything, on one line |
| `set time <YYYY-MM-DDTHH:MM:SS>` | Move the clock |
| `help` | List the commands |

Every command answers `OK` or `ERR …` on a single line. A mode change takes
effect at the next reading, so a message already going out is never cut short.

There is no command to set the power: each mode decides its own values. That is
what makes a test result mean something — you compare the reader against the
simulator's own log, not against a number you typed.

### 4.4 Reading `status`

```
OK pi=4820 po=0 pf=0.93 ei=27 eo=0 q1=3 q2=0 q3=0 q4=0 i=6985
   time=2026-01-01T00:00:28 mode=3/e450 period=5 emissions=6
```

| Field | Meaning |
|---|---|
| `pi` / `po` | Power being consumed / fed back, W |
| `pf` | Power factor; a trailing `L` as in `0.90L` means leading |
| `ei` / `eo` | Energy consumed / fed back, Wh — the running totals |
| `q1`…`q4` | Reactive energy, varh. All zero while `pf` is 1.00 |
| `i` | Current, mA — worked out from the power, not measured |
| `time` | The clock |
| `mode` | Which of the three modes is running |
| `period` | Seconds between readings |
| `emissions` | Readings sent since power-on |

Two things that look wrong and are not:

- **`ei` steps unevenly** — 1, 3, 5, 6, 8. It shows whole watt-hours while
  counting in fractions, so even a steady load looks uneven. Nothing is lost;
  see FSD FR-MDL-04.
- **In mode 2, `status` does not match what is sent.** Mode 2 sends its own fixed
  numbers and ignores the model, so read the console log line, not `status`.

**The clock starts from a fixed date, not today's.** That is on purpose: the same
run twice produces identical output, which is impossible with a real clock — and
it is also why mode 3's "random" values are the same random values every time
you reboot. `set time` moves the clock.

---

## 5. When something looks wrong

Work down in order. Each step rules something out, so the first failure tells
you where the problem is.

### 5.1 Is it sending anything?

Watch the console. A line appears every five seconds:

```
I (32082) sim: e450 6: 381 bytes on GPIO17, 1747 ms on the wire, 3253 ms silent - pi=…
```

- **It appears** → the simulator is fine; the problem is the wiring, the
  interface board, or the reader. Go to 5.2.
- **It does not** → re-check the banner (3.3). A board running different
  firmware looks exactly like a broken one.

### 5.2 Is the signal getting through correctly?

Mode 1 sends a plain test pattern — the byte `0x55`, which is just alternating
bits — instead of readings. Nothing complicated has to be right for it to mean
something, which is why it is the fastest way to find a wiring or settings
fault.

```
sim> mode 1
OK 1 pattern
```

Now look at what your reader received, then `mode 3` to put it back:

| It received | What is wrong |
|---|---|
| `55 55 55 …` | Nothing — the signal is correct. The fault is further along. |
| `D5 D5 D5 …` or `AA AA AA …` | The signal is inverted. Change `SIM_TX_INVERT` (chapter 7). |
| Bytes that vary, but always the same way | Wrong speed or parity setting at the reader. |
| Nothing at all | Wiring, or the interface board. The reader takes its power from this signal, so no signal also means no reader. |

`D5` looks like nonsense but is not: consistent nonsense means an inverted
signal, not interference. The reasoning is in FSD FR-LNK-02.

### 5.3 The reader gets data but cannot read it

Try `mode 2` first — if its two fixed numbers come through and mode 3's do not,
the reader cannot reassemble a telegram split across several frames, and that is
the fault. If neither works, check the **`ms silent`** figure on the console line
above. Readers decide a
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
