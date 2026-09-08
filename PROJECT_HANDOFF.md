# YM OFI Project Handoff

## Goal

Create a rule-based YM predictor from Databento `GLBX.MDP3` MBO data. It runs only during 09:30–11:00 New York time and displays rolling 2m, 5m, 10m, and 15m up/down OFI pressure.

No machine-learning model is used.

## Fixed decisions

- Instrument selector: `YM.v.0`
- Input symbology: `continuous`
- Dataset: `GLBX.MDP3`
- Schema: `mbo`
- Historical raw format: DBN
- Session timezone: `America/New_York`
- Publishing window: 09:30:00–11:00:00 ET
- Output frequency: once per second
- OFI core: Cont–Kukanov–Stoikov best-bid/best-ask event formula
- Rolling windows: 120, 300, 600, and 900 seconds
- Historical full-day request: 00:00 UTC through 11:15 ET
- Live initialization: MBO subscription with `snapshot=True`
- Output wording: pressure percentage, not calibrated probability

## Current status

Mark items with `[x]` only after they work locally.

- [ ] Project opened in VS Code
- [ ] `.venv` created and dependencies installed
- [ ] `.env` contains the Databento API key
- [ ] `scripts/01_check_api.py` succeeds
- [ ] Smoke cost estimated
- [ ] Smoke DBN downloaded
- [ ] Smoke MBO records inspected successfully
- [ ] Full-day cost estimated and accepted
- [ ] Full-day DBN downloaded
- [ ] Historical snapshot confirmed
- [ ] Historical order-book replay completes without errors
- [ ] Historical predictions CSV created
- [ ] Output manually reviewed
- [ ] Databento Live entitlement available
- [ ] Live snapshot and stream tested
- [ ] Live DBN archive created
- [ ] Live predictions CSV created
- [ ] Quantower bridge started

## Last command run

```text
PASTE THE COMMAND HERE
```

## Result or error

```text
PASTE THE OUTPUT OR FULL ERROR HERE
```

## Important files produced

```text
PASTE FILE PATHS HERE
```

## First unchecked task for the next chat

```text
WRITE ONE TASK HERE
```

## Notes for the next chat

The historical and live paths must continue using the shared classes in:

```text
src/ym_ofi/book.py
src/ym_ofi/ofi.py
src/ym_ofi/engine.py
```

Do not create a separate live OFI calculation. Fix or extend the shared engine instead.

Do not call `up_pressure_pct` a measured market probability unless an evaluation step has compared pressure bins with later YM outcomes.

## Suggested prompt for the next chat

> Continue the YM OFI project from this handoff. Read the fixed decisions and current-status checklist. Work on the first unchecked task only. Preserve the shared historical/live engine and explain any code changes by filename.
