# Sleep Stage Feature Specification

Canonical feature order defined in `include/ml/SleepStageTypes.h` function
`sleepStageFeatureNames()`. The Python extractor MUST produce columns in this
exact order.

## Base Features (23)

| # | Name              | Group       | Unit / Range          | Description                                      |
|---|-------------------|-------------|-----------------------|--------------------------------------------------|
| 0 | hr_mean           | Cardiac     | bpm                   | Mean heart rate in 30s epoch                     |
| 1 | hr_std            | Cardiac     | bpm                   | Std dev of heart rate                            |
| 2 | hr_min            | Cardiac     | bpm                   | Min heart rate                                   |
| 3 | hr_max            | Cardiac     | bpm                   | Max heart rate                                   |
| 4 | hrv_rmssd         | Cardiac     | ms                    | RMSSD of RR intervals (sqrt(mean(diff(RR)^2)))   |
| 5 | hrv_sdnn          | Cardiac     | ms                    | SDNN of RR intervals (std(RR))                   |
| 6 | spo2_mean         | SpO2        | %                     | Mean SpO2                                        |
| 7 | spo2_min          | SpO2        | %                     | Min SpO2                                         |
| 8 | spo2_std          | SpO2        | %                     | Std dev SpO2                                     |
| 9 | spo2_odi_proxy    | SpO2        | count                 | Desaturation events (>=3% dip from local mean)   |
| 10| spo2_slope        | SpO2        | %/s                   | Linear slope of SpO2 across epoch                |
| 11| rr_mean           | Respiratory | breaths/min           | Mean respiratory rate                            |
| 12| rr_std            | Respiratory | breaths/min           | Std dev respiratory rate                         |
| 13| tv_mean           | Respiratory | L (approx)            | Mean tidal volume                                |
| 14| tv_std            | Respiratory | L (approx)            | Std dev tidal volume                             |
| 15| mv_mean           | Respiratory | L/min (approx)        | Mean minute ventilation (tv * rr)                |
| 16| mv_std            | Respiratory | L/min (approx)        | Std dev minute ventilation                       |
| 17| ie_ratio_mean     | Respiratory | ratio                 | Mean I:E ratio                                   |
| 18| leak_mean         | Respiratory | L/min                 | Mean CPAP leak rate                              |
| 19| flow_p95          | Respiratory | L/min                 | 95th percentile flow                             |
| 20| motion_fraction   | Movement    | [0, 1]                | Fraction of epoch with motion                    |
| 21| vibration_count   | Movement    | count                 | Number of vibration events                       |
| 22| time_of_night     | Context     | [0, 1]                | epoch_index / total_epochs                       |

## Rolling Context Features

### Past rolling (both causal and bidirectional modes) -- 8 features

| # | Name                     | Window  |
|---|--------------------------|---------|
| 23| past_5_hr_mean           | 5 epoch |
| 24| past_5_spo2_mean         | 5 epoch |
| 25| past_5_rr_mean           | 5 epoch |
| 26| past_5_motion_fraction   | 5 epoch |
| 27| past_10_hr_mean          | 10 epoch|
| 28| past_10_spo2_mean        | 10 epoch|
| 29| past_10_rr_mean          | 10 epoch|
| 30| past_10_motion_fraction  | 10 epoch|

### Future rolling (bidirectional only) -- 4 features

| # | Name                      | Window  |
|---|---------------------------|---------|
| 31| future_5_hr_mean          | 5 epoch |
| 32| future_5_spo2_mean        | 5 epoch |
| 33| future_5_rr_mean          | 5 epoch |
| 34| future_5_motion_fraction  | 5 epoch |

## Feature Counts

- **Causal mode:** 31 features (23 base + 8 past rolling)
- **Bidirectional mode:** 35 features (23 base + 8 past rolling + 4 future rolling)

## Edge Case Handling

- Missing or invalid values: replace with 0.0
- SHHS data has no CPAP leak, flow_p95, motion_fraction, or vibration_count:
  set those columns to 0.0 (model learns from features that ARE present)

## HRV Computation

Given heart rate samples HR[i] in an epoch:

1. Derive RR intervals: `RR[i] = 60.0 / HR[i]` (seconds)
2. RMSSD: `sqrt(mean(diff(RR)^2))` -- converted to milliseconds (* 1000)
3. SDNN: `std(RR)` -- converted to milliseconds (* 1000)

If fewer than 2 valid HR samples, RMSSD = SDNN = 0.0.

## ODI Proxy

Count of samples where SpO2 drops >= 3% below the epoch mean SpO2.
If no valid SpO2 data, ODI proxy = 0.

## time_of_night

`epoch_index / total_epochs` where epoch_index is 0-based. Range [0, 1).
For the last epoch: `(total_epochs - 1) / total_epochs`.
