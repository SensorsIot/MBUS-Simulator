# MBUS-Simulator — User Manual

A fake electricity meter, for testing anything that reads one.

Technical detail is deliberately not repeated here — where you need it, this
manual points at the [FSD](../Functionality/MBUS-Simulator-FSD.md).

---

## 1. What it does

It pretends to be the customer interface of a Landis+Gyr E450 smart meter, the
kind installed by EBL Liestal. Every five seconds it sends out a reading —
power, energy, reactive energy and current — exactly as the real meter would, so
you can build and test meter-reading software without having a meter.

**It has three modes, and they are a ladder.** Each one is harder for a receiver
than the one below, so the first that fails tells you where the problem is:

| Mode | What it sends | What it tells you |
|---|---|---|
| **1** | A plain test pattern | Whether any signal is arriving at all |
| **2** | One small reading, always the same numbers | Whether the receiver can decode anything |
| **3** | The real thing, values changing every time | Whether it can decode a real meter |
| **4** | A counted sequence, telegram-sized | **How many bytes** are being lost |

Mode 3 is the default and the one you will normally use. Its values change on
every reading, deliberately: a receiver cannot pass by remembering a number, it
has to decode each message. Energy still follows power exactly, so if the receiver
gets the arithmetic wrong, you find out.

You control it by typing commands on a serial console. There is no app, no web
page and no WiFi.

What it does **not** do: it is not a meter, measures nothing, and has no
metrological standing. See chapter 8 for the limits that matter in practice.

---

## 2. How it is connected

```
   ESP32                interface board             your device
  ┌──────┐   GPIO17    ┌──────────────┐  12/24 V   ┌──────────┐
  │  sim │ ──────────► │  M-Bus driver│ ─────────► │ receiver │
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

**The receiver is powered by the meter signal itself** — it has no supply of its
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

**Compare that line against what your receiver reports.** They should agree — same
power, same energy. That is the whole test, and because mode 3's values change
every time, agreement means the receiver really decoded the message.

If they do not agree, drop down the ladder:

```
sim> mode 2
OK 2 simple
```

Mode 2 sends the same two numbers every time — **777 W** and **12345 Wh**. If the
receiver shows those, it can decode; the problem is with the full E450 telegram.
If it shows nothing, go to `mode 1` and chapter 5.2.

### 4.3 Commands

| Command | What it does |
|---|---|
| `mode` | Say which mode is running |
| `mode 1` | Test pattern — is a signal arriving? |
| `mode 2` | Small fixed reading — can the receiver decode? |
| `mode 3` | Full E450, changing values — can it decode the real thing? |
| `mode 4` | Counted ramp — how many bytes are being lost? |
| `invert [on\|off]` | Invert the output signal; no argument reports it |
| `status` | Show everything, on one line |
| `set time <YYYY-MM-DDTHH:MM:SS>` | Move the clock |
| `help` | List the commands |

Every command answers `OK` or `ERR …` on a single line. A mode change takes
effect at the next reading, so a message already going out is never cut short.

There is no command to set the power: each mode decides its own values. That is
what makes a test result mean something — you compare the receiver against the
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
  interface board, or the receiver. Go to 5.2.
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

Now look at what your receiver received, then `mode 3` to put it back:

| It received | What is wrong |
|---|---|
| `55 55 55 …` | Nothing — the signal is correct. The fault is further along. |
| `D5 D5 D5 …` or `AA AA AA …` | The signal is inverted. Type `invert on` and look again. |
| Bytes that vary, but always the same way | Wrong speed or parity setting at the receiving end. |
| Nothing at all | Wiring, or the interface board. The receiver takes its power from this signal, so no signal also means no receiver. |

`D5` looks like nonsense but is not: consistent nonsense means an inverted
signal, not interference. The reasoning is in FSD FR-LNK-02.

Fixing it takes one command — no rebuild:

```
sim> invert on
OK invert=1 (idle level flipped; one edge on the line)
```

Re-run `mode 1` and the bytes should now read `55`. The setting lasts until the
board is reset; if it turns out to be the right one for your wiring
permanently, set `SIM_TX_INVERT` (chapter 7) so it comes up that way.

### 5.3 Data arrives but will not decode

Try `mode 2` first — if its two fixed numbers come through and mode 3's do not,
the receiver cannot reassemble a telegram split across several frames, and that is
the fault. If neither works, check the **`ms silent`** figure on the console line
above. Receivers decide a
transmission has finished when the line goes quiet for about two seconds, so
that number needs to stay comfortably above 2000. If you have shortened the
period it may not be. The simulator prints an error line of its own when this
happens — see FSD FR-SCH-08.

A receiver complaining about oversized data, or reporting far more bytes than the
simulator sent, means readings were run together — same cause.

### 5.4 Nothing at the other end responds

It is powered by the meter signal, so "no power" and "no signal" are the same
fault. Confirm the interface board is producing 24 V on the line while the
simulator is idle. That needs a meter on the wire; the console cannot tell you.

---

## 6. Making it misbehave on purpose

Chapters 4 and 5 cover a meter that is working. This one is for testing what
happens when it is not: a corrupted checksum, a missing piece, silence. All of
it is typed at the console — nothing here needs a rebuild.

### 6.1 Faults

**A fault applies to the next reading only, then clears itself.** That is
deliberate: a corruption left switched on quietly spoils every test that comes
after it. Arm one, watch one reading, move on.

```
sim> fault fcs 2
OK fcs:2 armed for the next telegram
```

| Command | What it sends |
|---|---|
| `fault fcs <n>` | Reading whose frame *n* has a wrong checksum, everything else intact |
| `fault truncate <n>` | Frame *n* cut off in the middle, with no ending |
| `fault drop-block <k>` | Reading with piece *k* missing entirely |
| `fault start-mid-frame` | Starts transmitting halfway through the first frame |
| `fault duplicate-register <obis>` | One reading containing the same value twice, with different numbers |
| `fault noise <n>` | *n* bytes of rubbish instead of a reading |
| `fault none` | Disarm |
| `fault` | Say what is armed |

OBIS codes are typed the way a decoder prints them: `fault duplicate-register
1.1.1.8.0.255`.

**Faulted readings always carry the same fixed numbers**, never mode 3's
changing ones — otherwise you could not say what the correct answer was supposed
to be.

### 6.2 Counting lost bytes, and lead-in

If readings arrive but are damaged, the question is usually *how much of the
start went missing*. **Mode 4 answers it directly.**

It sends `00 01 02 03 …` counting up, for exactly as long as a real reading
takes. So whatever your receiver logs as its **first byte is the number of
bytes that were lost.** First byte `0x19`? Twenty-five bytes went missing.
First byte `0x00`? Nothing was lost.

```
sim> mode 4
OK 4 ramp
```

No subtracting one device's count from another's and hoping they refer to the
same burst. (It counts up to 255 and starts over, so losses beyond 255 bytes
are ambiguous — but that is 1.2 seconds of missing line, which is a different
problem.)

Once you know how much is lost, `preamble` tests whether a lead-in fixes it:

```
sim> preamble 32
OK preamble=32 bytes of 0x7E
```

It sends *n* bytes of padding before the real reading, in the same burst, so
anything lost off the front is padding rather than data.

**Which padding byte matters, a lot.** The default `0x7E` is the "correct"
choice on paper — the M-Bus framing standard calls repeated `0x7E` harmless
padding. Measured against a real decoder, it is the worst possible choice: **a
single leading `0x7E` stopped a working reading from decoding at all.** Padding
with something else cost only the first frame. So try both:

```
sim> preamble 32 0x00      # padding that is not a frame marker
```

### 6.3 Timing

```
sim> gap 2000
OK gap=2000 (free-running)
sim> silence 30
OK silent for 30 s
```

`gap <ms>` sets the quiet time between readings exactly, which is how you test a
receiver's cut-off — try 1999, 2000 and 2001. It replaces the normal five-second
schedule until you set `gap 0`.

`silence <seconds>` stops transmission altogether for a while. Everything else
keeps running: the clock advances, the console answers, and it starts again on
its own.

### 6.4 Changing what the reading contains

| Command | Effect |
|---|---|
| `identity ldn` / `devid` / `none` | Which identity object the reading carries, if any |
| `serial 8` / `serial 16` | How long the serial number is |
| `energy <wh>` / `energy auto` | Set the total-energy figure directly |
| `scaler <-9..9>` / `scaler 0` | Send values with a multiplier instead of plain |

Unlike faults, **these stay set** until you change them — they are not
corruptions, just things a real meter might do differently.

Two notes worth having:

- `energy` is how you check large numbers. Above about 16.7 million, software
  that stores readings as a 32-bit decimal starts rounding them, which looks
  like consumption that never happened. `energy 16777217` provokes it.
- `scaler` changes the *shape* of the reading, not just a number. The E450 does
  not send scalers at all, so there would be nothing to test otherwise; with one
  set, the simulator sends the self-describing form other meters use. It is a
  test of the receiver, not an imitation of this meter.

### 6.5 What gets reported

Every reading is logged as `key=value`, so a script can check it rather than
guess:

```
I sim: emit n=42 mode=e450 fault=fcs:2 bytes=381 frames=3 pre=0/0x7E wire_ms=1747
       tx_ms=1748 lead_us=n/a silent_ms=3253 gpio=17 inv=0 pi=777 ei=777 fixed=1
```

`wire_ms` is calculated from the byte count; `tx_ms` is what actually took
place. They should agree — if they do not, something is wrong with the link
itself.

`lead_us` would be the delay between the line waking up and the first data bit.
It reports **`n/a`, and that is deliberate**: the simulator drives the interface
board but has no wire back from it, so it cannot see the line at all. It knows
only when it handed bytes to the UART, which is a different thing. Printing that
instead would look like a measurement and be a guess.

**Measure it with an oscilloscope instead** — see 6.6. (There is a firmware path
for unattended runs: wire the interface board's *output*, divided down, to a
spare input pin and set `SIM_LINE_SENSE_GPIO`. It is off by default and not
normally needed.)

### 6.6 Measuring the line with a scope

If the ramp (6.2) says bytes are being lost, the next question is whether the
interface board is slow to pull the line. A scope answers it, and mode 4 makes
the trigger easy.

**Why:** at these line settings the byte `0x00` is ten zero bits in a row — a
**4.17 ms** low, the longest the line ever goes — and mode 4 starts every burst
with it, after 3.25 seconds of silence. Nothing else looks like that, so it is a
clean trigger with no traffic around it.

```
sim> mode 4
OK 4 ramp
```

Probe the M-Bus line, trigger on the falling edge, and look at how long the line
takes to settle at 12 V.

**The number to compare against is 417 µs** — one bit. If settling eats a real
fraction of that, the first characters are being read mid-transition, and the
ramp will tell you exactly how many of them were lost.

⚠ **Probe the bus, not a logic pin.** The M-Bus line swings between 12 V and
24 V. Never connect it to a microcontroller input without dividing it down.

`fault=` and `frames=` are the two that matter for fault testing: they let you
assert *which* frame was damaged out of *how many*, instead of only noticing
that something went wrong somewhere.

`status` shows everything currently set:

```
OK pi=777 … mode=3/e450 period=5 gap=0 silence=0 serial=8 identity=none
   invert=0 preamble=0/0x7E scaler=0 energy=auto fault=none emissions=42
```

---

## 7. Settings

Changed before building, under **MBUS Simulator** in `idf.py menuconfig`. Only
wiring-level things live here — everything you would change during a test is a
console command (chapter 6).

| Setting | Default | Change it when |
|---|---|---|
| `SIM_MBUS_TX_GPIO` | 17 | The interface board is on another pin |
| `SIM_TX_INVERT` | off | Your wiring *permanently* needs the other polarity. For a one-off test use `invert` instead — no rebuild |
| `SIM_PUSH_PERIOD_SECONDS` | 5 | You need longer gaps between readings |
| `SIM_EPOCH` | 2026-01-01 | You want a different starting date |
| `SIM_LINE_SENSE_GPIO` | −1 (off) | You want `lead_us` measured automatically instead of with a scope — see 6.5 |

Five seconds is what a real E450 does, so leave it alone unless you have a
reason.

---

## 8. What it will not do

Worth knowing before you trust a test result:

- **No meter serial number.** The real meter sends it in a different, slower
  list that this simulator does not produce. A receiver waiting to learn the
  meter's identity will wait forever here.
- **Nothing is encrypted.** Real installations often are.
- **One kind of reading only** — the five-second list. No 15-second, minute or
  quarter-hour lists.
- **Passing here is necessary, not sufficient.** It proves a receiver *can* read
  this output. A forgiving receiver would also accept things a real meter never
  sends.

Full scope and the reasoning behind each: FSD §1.5 and Appendix A.
