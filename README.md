# YM OFI Predictor — Starter Project

This project builds a **rule-based**, non-ML YM order-flow-imbalance indicator from Databento MBO data.

The workflow is intentionally simple:

1. Prove that the Databento connection and raw MBO records work.
2. Download one complete historical test day and reconstruct the order book.
3. Replay that day through the Cont OFI calculation and write 2/5/10/15-minute pressure readings.
4. Replace the historical file source with Databento Live while keeping the same book and OFI engine.

The indicator publishes only from **09:30:00 through 10:59:59 New York time**. A historical day download continues to 11:15 ET so a later evaluation script can compare predictions with outcomes through the full 15-minute horizon.

## Important wording

`up_pressure_pct` and `down_pressure_pct` are deterministic OFI pressure shares. They are not yet statistically calibrated probabilities. After collecting historical and live results, a later project step can measure how often each pressure range was followed by an actual up or down move.

---

# STEP 1 — Create the Python environment and check Databento

## 1.1 Open the project in VS Code

Unzip the project, then open the `ym_ofi_starter` folder in Visual Studio Code.

Open **Terminal > New Terminal** and use PowerShell.

## 1.2 Create and activate a virtual environment

```powershell
py -3.11 -m venv .venv
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

Python 3.10 or newer is required by the current Databento Python client. Python 3.11 is a practical choice.

In VS Code, press `Ctrl+Shift+P`, select **Python: Select Interpreter**, and choose the interpreter inside `.venv`.

## 1.3 Add the API key

```powershell
Copy-Item .env.example .env
```

Open `.env` and replace the placeholder:

```text
DATABENTO_API_KEY=db-YOUR_REAL_KEY
```

Do not commit `.env` to Git.

## 1.4 Check authentication and schema access

```powershell
python scripts/01_check_api.py
```

Success means the script prints:

```text
Databento authentication succeeded.
MBO schema available: True
```

This metadata check does not download market data.

---

# STEP 2 — Run a cheap raw-MBO smoke test

Use a past normal trading date. The examples below use `2026-09-04`; replace it when needed.

The smoke file contains at most 5,000 MBO records from the first minute after 09:30 ET. It is only for checking fields and actions. It does **not** contain the midnight order-book snapshot, so do not use it for the predictor.

## 2.1 Estimate cost before downloading

```powershell
python scripts/02_estimate_cost.py --date 2026-09-04 --mode smoke
```

Read the estimated cost and record count.

## 2.2 Download the smoke sample

```powershell
python scripts/03_download_data.py --date 2026-09-04 --mode smoke
```

The file is saved here:

```text
data/raw/YM_2026-09-04_smoke.dbn
```

The request uses:

```text
dataset = GLBX.MDP3
schema = mbo
symbol = YM.v.0
stype_in = continuous
```

`YM.v.0` means the YM contract ranked first by the previous trading day's volume. Databento maps it to the actual unadjusted raw futures contract in the DBN metadata.

## 2.3 Inspect the MBO records

```powershell
python scripts/04_inspect_data.py --date 2026-09-04 --mode smoke
```

Check that you can see fields such as:

```text
action
side
price
size
order_id
F_LAST
instrument_id
```

Expected actions include `A`, `C`, `M`, `T`, `F`, `R`, or `N`. It is normal for a small sample not to contain every action.

**STEP 2 is complete when the raw records print correctly.**

---

# STEP 3 — Download one full test day and run the historical predictor

A correct MBO order book cannot normally be created from a random 09:30 start. The `day` request starts at **00:00 UTC**, includes Databento's CME MBO snapshot, and replays every following update into the 09:30 open.

## 3.1 Estimate the full-day test cost

```powershell
python scripts/02_estimate_cost.py --date 2026-09-04 --mode day
```

Do not run the download until the quoted cost is acceptable.

## 3.2 Download the full test file

```powershell
python scripts/03_download_data.py --date 2026-09-04 --mode day
```

The file is saved here:

```text
data/raw/YM_2026-09-04_day.dbn
```

The script refuses to overwrite an existing file because a duplicate historical streaming request can be billed again. Only use `--overwrite` intentionally.

## 3.3 Confirm that the snapshot exists

```powershell
python scripts/04_inspect_data.py --date 2026-09-04 --mode day --scan-limit 1000000
```

Look for:

```text
Snapshot records: a number greater than 0
```

The snapshot begins with a clear-book `R` action and then sends resting orders as `A` actions.

## 3.4 Replay the file and calculate OFI

```powershell
python scripts/05_run_historical.py --date 2026-09-04
```

The script:

1. Reconstructs the order-level book from order IDs.
2. Applies `A`, `C`, `M`, and `R` to the resting book.
3. Records `T` trades separately; `T` and `F` do not alter the book again.
4. Examines the BBO only after `F_LAST` marks the complete publisher event.
5. Calculates the Cont best-level OFI contribution.
6. Maintains rolling 2-, 5-, 10-, and 15-minute windows.
7. Writes one reading per second from 09:30 through 11:00 ET.

The result is saved here:

```text
data/output/YM_2026-09-04_predictions.csv
```

Important output columns:

```text
up_pressure_pct / down_pressure_pct  Core Cont-OFI pressure split
net_ofi                             Signed rolling OFI
average_best_depth                  Average BBO depth in the window
impact_score                        net_ofi divided by average depth
trade_buy_pct / trade_sell_pct      Aggressive trade confirmation
mbo_buy_support_pct                 Bid adds plus ask cancels
mbo_sell_support_pct                Ask adds plus bid cancels
top5_bid_depth_pct                  Current top-five bid depth share
direction                           UP, DOWN, or NEUTRAL
fully_warmed                        True only after the full window exists
```

The four windows become fully warm after approximately:

```text
2m  -> 09:32 ET
5m  -> 09:35 ET
10m -> 09:40 ET
15m -> 09:45 ET
```

**STEP 3 is complete when the CSV contains one-second readings and the reconstructed book runs without errors.**

---

# STEP 4 — Move the same engine to Databento Live

Do this only after the historical day works.

Databento Live is a separate service and may require the appropriate CME live-data entitlement and fees.

## 4.1 Start before the opening window

Start the program around 09:20–09:25 New York time:

```powershell
python scripts/06_run_live.py
```

It subscribes with:

```python
client.subscribe(
    dataset="GLBX.MDP3",
    schema="mbo",
    symbols="YM.v.0",
    stype_in="continuous",
    snapshot=True,
)
```

The live snapshot initializes the resting book. New MBO events then flow through the exact same `OrderBook`, `MBOEngine`, and `OFIPredictor` classes used by historical replay.

The program saves both:

```text
data/raw/YM_YYYY-MM-DD_live.dbn
data/output/YM_YYYY-MM-DD_live_predictions.csv
```

The raw live DBN file can later be replayed for debugging.

Press `Ctrl+C` to stop.

**STEP 4 is complete when the terminal displays 2m/5m/10m/15m pressure readings and both live files are being written.**

---

# File map

```text
scripts/01_check_api.py          Check key, dataset, and MBO schema
scripts/02_estimate_cost.py      Estimate records, bytes, and cost
scripts/03_download_data.py      Download smoke or full-day DBN data
scripts/04_inspect_data.py       Print and summarize raw MBO records
scripts/05_run_historical.py     Replay a full day and write OFI readings
scripts/06_run_live.py           Subscribe to live MBO and write readings

src/ym_ofi/settings.py           Dataset, symbol, session, and file settings
src/ym_ofi/book.py               Order-ID book and aggregated price levels
src/ym_ofi/ofi.py                Cont formula and rolling pressure windows
src/ym_ofi/engine.py             Shared historical/live MBO processor
src/ym_ofi/output.py             Terminal and CSV output

tests/test_cont_ofi.py           Basic formula tests
PROJECT_HANDOFF.md               State file for continuing in another chat
```

# Run the tests

```powershell
python -m pytest
```

# Continue in another ChatGPT conversation

1. Update `PROJECT_HANDOFF.md` after each completed step.
2. Upload the project ZIP or at least `PROJECT_HANDOFF.md` plus the relevant error/output file.
3. Say: **"Continue the YM OFI project from this handoff. Work on the first unchecked item only."**

That allows the next conversation to continue without redesigning the project.
