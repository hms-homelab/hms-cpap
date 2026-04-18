# CPAP ML Intelligence Quick Reference

**4 C++ Random Forest models, 42 features, trained on cpap_daily_summary**

All ML runs natively inside hms-cpap (pure C++17, zero external ML libraries).

## Models

| Model | Type | Output | CV Metric |
|-------|------|--------|-----------|
| AHI Forecaster | RF Regressor (200 trees) | Predicted next-night AHI | R2 + MAE |
| Mask Fit Predictor | RF Classifier (balanced) | P(high leak tonight) 0-100% | Accuracy + F1 |
| Compliance Predictor | RF Regressor (200 trees) | Predicted next-night hours | R2 + MAE |
| Anomaly Detector | RF Classifier (4-class, balanced) | NORMAL / AHI_ANOMALY / LEAK_ANOMALY / PRESSURE_ANOMALY | Accuracy + F1 |

**Hyperparams (all models):** 200 estimators, max_depth=8, min_samples_split=5, min_samples_leaf=3, seed=42

## Architecture

```
cpap_daily_summary (duration > 60 min)
  -> FeatureEngine (42 features: rolling 7d/14d, trends, z-scores)
  -> StandardScaler (per-feature z-score normalization)
  -> 4 RandomForest models (C++ CART implementation)
  -> JSON model files on disk
  -> MQTT publish + REST API + frontend dashboard
```

## Training

**Schedule:** configurable — daily (24h), weekly (168h, default), monthly (720h)
**Min data:** 30 days (configurable via `min_days`)
**Cross-validation:** 5-fold CV on every training run
**Persistence:** `{model_dir}/{name}.json` — contains model, scaler, features, metrics, timestamp

**Triggers:**
- Automatic on schedule (worker thread checks every 60s)
- Manual: `POST /api/ml/train`
- MQTT command: `cpap/{device_id}/command/train_models`

## Inference

**Triggers:**
- After training completes (automatic)
- MQTT: `cpap/{device_id}/session/completed` (auto after each session)
- Manual via frontend "Run Inference" button

**Pipeline:** loads last 30 days -> builds features from latest row -> scales -> runs all 4 models

**Output (Predictions struct):**
- `predicted_ahi` (double)
- `predicted_hours` (double)
- `leak_risk_pct` (0-100%)
- `anomaly_class` (NORMAL / AHI_ANOMALY / LEAK_ANOMALY / PRESSURE_ANOMALY)

## MQTT Topics

| Topic | Payload | Retained |
|-------|---------|----------|
| `cpap/{device_id}/ml/status` | `{"status":"training\|complete\|error","timestamp":"...","models":[...]}` | Yes |
| `cpap/{device_id}/ml/predictions` | `{"predicted_ahi":X,"predicted_hours":X,"leak_risk_pct":X,"anomaly_class":"..."}` | Yes |
| `cpap/{device_id}/command/train_models` | (any) | - |

## REST API

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/ml/train` | Trigger training |
| GET | `/api/ml/status` | Training status + last predictions |
| PUT | `/api/config` | Update ML config (`ml_training` block) |

## Feature Engineering (42 features)

**Rolling windows (7d and 14d):** mean, std, max for AHI, leak_95, mask_press_95, duration
**Trend slopes:** 7d linear regression for AHI, leak_95
**Z-scores:** AHI, leak_95, mask_press_95 (anomaly threshold: sigma > 2.0)
**Event breakdown:** OAI, CAI, HI raw + 7d means
**Respiratory:** resp_rate_50, tid_vol_50, min_vent_50
**Statistics:** leak_50, leak_max, mask_press_50, mask_press_max
**Temporal:** day_of_week, is_weekend, days_gap
**High leak threshold:** leak_95 > 24 L/min

## Configuration

**Environment variables:**
```bash
ML_ENABLED=true
ML_SCHEDULE=weekly          # daily | weekly | monthly
ML_MODEL_DIR=/path/to/models
ML_MIN_DAYS=30
ML_MAX_TRAINING_DAYS=0      # 0 = unlimited
```

Also configurable via `PUT /api/config` with `ml_training` JSON block.

## Frontend

Dashboard has an "ML Insights" section (visible when models are loaded):
- 4 cards: Predicted AHI, Predicted Hours, Leak Risk %, Anomaly Class
- Color-coded: leak risk red if >50%, anomaly orange if not NORMAL
- Model metadata: last trained, model count, samples used
- "Run Inference" button + collapsible per-model metrics detail

## Files

```
include/ml/RandomForest.h          # RF ensemble (header)
include/ml/DecisionTree.h          # CART tree (header)
include/ml/FeatureEngine.h         # Feature extraction (header)
include/ml/StandardScaler.h        # Z-score scaler (header-only)
include/ml/CrossValidator.h        # K-fold CV (header-only)
include/services/MLTrainingService.h  # Training + inference service
include/controllers/CpapController.h  # REST endpoints

src/ml/RandomForest.cpp            # RF implementation (200 trees)
src/ml/DecisionTree.cpp            # CART implementation (415 lines)
src/ml/FeatureEngine.cpp           # 42-feature pipeline
src/services/MLTrainingService.cpp # Training pipeline (761 lines)

frontend/src/app/pages/dashboard/  # ML Insights dashboard
```

## Database

- **DB:** cpap_monitoring on localhost:5432 (user: maestro)
- **Table:** cpap_daily_summary (source for training/inference)
- **Filter:** duration_minutes > 60

## Notes

- Pure C++17 — no sklearn, TensorFlow, or external ML libs
- Only dependency: nlohmann/json for model serialization
- Models saved as human-readable JSON (not binary joblib)
- Anomaly labels are rule-based: z-score > 2.0 sigma (AHI checked first, then leak, then pressure)
- Mask fit predictor uses balanced class weights (handles class imbalance)
