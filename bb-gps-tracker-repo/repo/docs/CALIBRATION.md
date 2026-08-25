# Battery calibration

Two independent systems, and confusing them wastes hours:

| System | What it does | How to fix it |
|---|---|---|
| **Voltage measurement** | Turns ADC counts into a pack voltage | Calibration — this document |
| **Percentage estimation** | Turns a pack voltage into a state of charge | The discharge curve — bottom of this document |

Calibrating the first will not fix an error in the second, and vice versa.

---

## The one-shot procedure

Change one value, upload once, done. You never enter it again.

1. Connect the battery and let it **rest** — no charging, modem idle. A pack under load or just off the charger reads high.
2. Measure the battery **terminals** with a trusted multimeter.
3. Put that reading in `BATTERY_CAL_REFERENCE_VOLTAGE` at the top of the sketch.
4. Upload once.
5. On first boot the firmware measures its own uncalibrated voltage, solves the correction against your reference, and writes it to NVS. Every later boot loads it.

```cpp
#define BATTERY_CAL_REFERENCE_VOLTAGE   3.720f   // <-- YOUR MULTIMETER READING
```

**Leave the value in place afterwards.** It is compared against the reference stored in NVS, so an unchanged value means "already calibrated, do nothing." The device will *not* re-solve itself on later boots against whatever charge state the pack happens to be in.

To disable auto-calibration entirely, set it to `0.0f` and use the `VCAL` commands instead.

The solve spends 8 bursts (~170 samples) rather than the single burst a normal tick uses — a calibration is a one-shot decision that then governs every later reading.

---

## The correction model

```
V_batt = V_uncalibrated × gain + offset
```

| Term | Absorbs |
|---|---|
| `gain` | Everything that **scales** with the reading: resistor tolerance, ADC reference error, source-impedance loss |
| `offset` | A **fixed** drop in the hardware path: protection FET, series diode |

`offset` defaults to `0`, so a gain-only calibration behaves exactly as it did before the two-point path existed. Accepted ranges: gain `(0.2, 5.0)`, offset `(-1.0, +1.0)` V. Anything outside falls back to `1.0` / `0.0`.

**One-point calibration solves gain only.** If your error is a fixed drop rather than a scaling error, one-point will be right at the calibration voltage and wrong everywhere else. That is what two-point is for.

---

## Two-point calibration

Use this when the error changes with voltage.

```
VCAL P1 <true_volts>     at a low charge state
  ... charge or discharge the pack ...
VCAL P2 <true_volts>     at a high charge state
```

The two points must be at least **`BATT_CAL_MIN_SPAN_V` (0.40 V)** apart. Closer than that and the solve amplifies measurement noise into a wildly wrong gain — the command refuses rather than accepting it.

`VCAL` with no argument shows a pending P1 if one is stored.

---

## Diagnostics

`VCAL` (no argument) prints the whole measurement chain:

```
[VCAL] ADC pin      : 1804.3 mV   <- measure GPIO34 to GND, compare
[VCAL] Divider      : R1 100k / R2 100k, nominal ratio x2.0000
[VCAL] Uncalibrated : 3.609 V     <- measure the battery terminals, compare
[VCAL] Correction   : gain 1.0547, offset +0.0000 V
[VCAL] Reported     : 3.806 V (45%)
[VCAL] Status       : VALID | source ref 3.720 V | firmware constant 3.720 V
```

### Reading it

The two `<-` lines are the whole diagnostic. Each isolates one half of the chain:

| Comparison | If it disagrees |
|---|---|
| **ADC pin** vs a DMM at GPIO `34` | The **ADC** is wrong — reference error, attenuation, or source-impedance contamination |
| **Uncalibrated** vs the battery terminals | The **divider** is wrong — resistor tolerance, a bad joint, or a wrong fitted value |

That distinction is the entire point of printing both. Do not skip to adjusting gain until you know which one is off — gain will paper over either, and the papered-over one will resurface as a nonlinear error at a different charge state.

| Command | Does |
|---|---|
| `VCAL` / `VCAL RAW` | The chain above |
| `VCAL DIAG` | Full diagnostics including rejected-sample count |
| `VCAL <v>` | One-point solve against `<v>` |
| `VCAL P1 <v>` / `VCAL P2 <v>` | Two-point solve |
| `VCAL RESET` | Erase calibration → gain 1.0, offset 0 |

After `VCAL RESET`, reboot and the firmware re-solves against `BATTERY_CAL_REFERENCE_VOLTAGE` if it is set. This is how you recalibrate at the same voltage as before.

Set `BATTERY_DEBUG` to `1` to print the full chain at boot. Off in production.

---

## Where calibration lives

Its own NVS namespace, `battcal` — CRC-guarded, plausibility-checked, versioned.

| Erased by | Calibration survives? |
|---|---|
| Config wipe | Yes |
| `CONFIG_SCHEMA_VERSION` bump | Yes |
| Boot provisioner | Yes |
| **Explicit factory reset** | **No** |

This is deliberate. Calibration is per-board *hardware* characterisation, not application configuration — it describes the resistors and the silicon actually fitted to this board. A config wipe must not take it, because nothing about the hardware changed. Only an operator explicitly saying "factory reset" erases it, and that is the only path that does.

---

## The measurement pipeline

Owned by `taskSensor` alone, at a fixed 250 ms cadence. **Nothing else touches the ADC.**

| Stage | Constant | Why |
|---|---|---|
| 2 dummy reads | `BATT_SETTLE_US` 200 | Charge the sample-and-hold after the channel switch |
| 21 samples | `BATT_BURST` 21 | Odd, so the median is a true sample |
| Median | — | Kills isolated spikes |
| MAD outlier rejection | — | Robust; a few bad reads cannot move it |
| Mean of survivors | — | |
| Divider + calibration | `BATT_R1`, `BATT_R2` | |
| Sanity gate | `[1.00, 5.00]` V | A broken conversion is not a battery state |
| Slew limit | `BATT_MAX_SLEW_V` 30 mV | One sample may not move the accepted value further |
| EMA | `BATT_EMA_ALPHA` 0.08 | τ ≈ 3 s at 4 Hz |
| Curve | `BATT_CURVE` | Voltage → percent |
| Percent hysteresis | `BATT_PCT_HYST` 0.7 | 1 LSB of noise must not flip a whole percent |

Rejected samples are counted and logged every 40th occurrence. A steadily climbing `rejectedSamples()` means a hardware fault, not a tuning problem — check the divider on GPIO `34` first.

---

## The discharge curve

```cpp
static const BattPoint BATT_CURVE[] = {
  {4.40f, 100}, {4.30f, 92}, {4.20f, 84}, {4.10f, 75}, {4.00f, 66},
  {3.90f, 56},  {3.85f, 50}, {3.80f, 44}, {3.75f, 37}, {3.70f, 30},
  {3.65f, 24},  {3.60f, 18}, {3.55f, 13}, {3.50f, 9},  {3.45f, 6},
  {3.40f, 4},   {3.30f, 2},  {3.20f, 0}
};
```

Targets a **4.40 V-charge** high-voltage LCO cell (Samsung EB-BG991ABY class, 3.86 V nominal) — **not** a 4.20 V cell. The previous table topped out at 4.20 V = 100%, which on this pack clips the entire top ~35% of usable charge: a full S21 cell sits at 4.40 V and would have read 100% from 4.20 V upward while still holding a third of its energy above that point.

### Provenance — read this before trusting the percentages

These points are an **estimate**, anchored on the published 3.86 V nominal and 4.40 V charge figures. They are **not measured from your pack.**

To replace them with real data:

1. Charge the pack fully and let it rest.
2. Discharge at a load representative of actual use — modem attaching and transmitting, not a constant resistor. The curve is load-dependent and a bench resistor will give you numbers that do not hold in the field.
3. Log resting terminal voltage against coulomb count.
4. Edit the table.

Nothing else in the firmware needs to change. Voltage measurement and percentage estimation are independent systems, which is the whole reason they are separable here.

### Note on resting voltage

Every point in the table is a *resting* voltage. Under load the terminal voltage sags by `I × R_internal`, and on a used cell `R_internal` can be 150 mΩ or more — a 2 A modem burst then costs 300 mV, which reads as roughly 25 percentage points on this curve.

The EMA (τ ≈ 3 s) smooths the burst, and the hysteresis stops the display flickering, but neither compensates for sag. If reported percentage drops noticeably during cellular attach and recovers afterwards, that is the sag, not a measurement fault.
