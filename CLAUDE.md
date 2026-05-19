# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

PANdeMaiz Quake is a portable distributed seismic early-warning network. It has three subsystems:

1. **Firmware** (`platform_acel/acel_ubuntu/`) — ESP32 (TTGO LILYGO T3 V1.6.1) + ADXL345 accelerometer, built with PlatformIO. Records filtered acceleration to SD card in a custom binary format and runs TFLite Micro inference on-device.
2. **Data pipeline** (`Data_Labeling/`) — Python/Jupyter notebooks that convert raw `.bin` (from the sensor) and `.anc` (from Colombia's SGC seismological service) into labeled spectrograms (`.npy`) used to train the CNN.
3. **ML** (`ML/`) — Keras notebook that trains a 1D-CNN classifier and exports it to TFLite INT8 + C header files ready to embed in the firmware.

## Environment setup

```bash
source pan_env/bin/activate          # activate the virtualenv (uv-created)
jupyter lab                          # launch notebooks
```

To create the env from scratch:

```bash
uv venv pan_env && source pan_env/bin/activate
uv pip install -r requirements.txt
python -m ipykernel install --user --name pandemaiz --display-name "PANdeMaiz"
# On Linux, if ObsPy fails: sudo apt-get install -y libxml2-dev libxslt-dev
```

## Firmware (PlatformIO)

All firmware work happens inside `platform_acel/acel_ubuntu/`.

```bash
# Build
pio run

# Flash (requires board connected via USB)
pio run --target upload

# Serial monitor (115200 baud, ESP32 exception decoder enabled)
pio run --target monitor
```

Credentials (WiFi, Firebase) go in `platform_acel/acel_ubuntu/.env` — copy from `.env.example`. The `load_env.py` pre-script injects them as `-D` build flags at compile time; never hardcode them in `config.h`.

`config.h` is the single file to edit for DSP parameters, pin assignments, STA/LTA thresholds, and TFLite arena size. Filter coefficients are pre-computed there for `fs=200 Hz`, `HP=0.1 Hz`, `LP=20 Hz` Butterworth biquads.

## Signal pipeline (firmware)

```
ADXL345 (I2C 400 kHz, ODR=200 Hz)
  → 2-s bias autocalibration at boot (AUTOCAL_SAMPLES=400)
  → HP biquad 0.1 Hz (DF2T, removes gravity/DC)
  → LP biquad 20 Hz  (DF2T, removes mechanical noise)
  → SD card  (/Aceleraciones, 1-min .bin files)
  → STA/LTA trigger (EMA, 30-s warmup)
       → [triggered] sliding window → FFT128 PSD → spectrogram (65,11,3)
                   → Z-score normalize → TFLite INT8 inference
                   → if SISMO: save to /Eventos + upload to Firebase RTDB
```

The ISR only sets a flag; all I2C and processing happens in `loop()` to avoid I2C-in-ISR issues.

## Binary `.bin` format

Header (16 bytes): `MAGIC(4=0xDA7A1345) + VERSION(2) + SAMPLE_RATE(2) + lat(4,float) + lon(4,float)`  
Per sample (10 bytes): `timestamp_ms(uint32_le) + ax(int16_le) + ay(int16_le) + az(int16_le)`  
Scale: 1 LSB = 3.9 mg (ADXL345 ±2g full-resolution mode).

Current version: `BIN_VERSION = 0x0003` (v3 adds GPS lat/lon to header).

## Dataset builder (`Data_Labeling/seismic_dataset_builder_v3.ipynb`)

This is the canonical notebook. Do not modify `v2` — it is kept as reference.

**Inputs:**
- `DatosObtenidos/PANAcelerografo/<env>/` — `.bin` files → labeled Class 0 (ambient noise)
- `DatosObtenidos/Acelerografo_SGC/<event>/` — `.anc` SGC files → labeled via STA/LTA (Class 0 or 1)

**Output shape:** `(65, 12, 3) float32`, `log10(PSD + 1e-12)` in g²/Hz, axes order EW/VER/NS.

**STA/LTA parameters** (must stay identical between notebook and firmware):

| Param | Value |
|-------|-------|
| STA | 100 samples = 0.5 s |
| LTA | 2000 samples = 10.0 s |
| ON threshold | 2.5 |
| OFF threshold | 1.5 |
| Window overlap for seismic label | ≥ 30% of samples |

**Augmentation** (v3): each `.anc` generates 4 signal variants (`orig`, `gauss`, `shift`, `scale`). The noise sigma values in `SIGMA_DICT` were measured empirically from the real ADXL345 at rest — do not invent new values.

The notebook is idempotent: it detects already-processed stems by reading existing `.npy` names and skips them.

## ML trainer (`ML/seismic_cnn_trainer.ipynb`)

Architecture: `Input(65,11,3) → Permute(2,1,3) → Reshape(11,195) → Conv1D(32,k=3)+BN → MaxPool1D(2) → Conv1D(64,k=3)+BN → MaxPool1D(2) → GAP → Dropout(0.3) → Dense(32) → Dense(1,sigmoid)`

Target: recall_sismo ≥ 0.85 on test set, INT8 model < 300 KB.

**After retraining**, copy both generated headers to firmware src:
```bash
cp ML/models/model.h          platform_acel/acel_ubuntu/src/model.h
cp ML/models/norm_constants.h platform_acel/acel_ubuntu/src/norm_constants.h
```

## TFLite export — known issues (TF 2.21 / Keras 3)

- Use `model.export(path)` (not `tf.saved_model.save()`) before TFLite conversion. `tf.saved_model.save()` leaves ReadVariable ops unfrozen, causing INT8 calibration to fail.
- The CNN uses `Conv1D` + `Permute` + `Reshape`, not `Conv2D`. `Conv2D` on the `(65,11,3)` input produces a `CONV_2D` op that TFLite Micro on ESP32 cannot run without full `AllOpsResolver`.
- The representative dataset must pass inputs in the same order as the model's `input` list. Swapping the spectrograms and PGA inputs causes silent quantization errors.

## Simulation tool

```bash
cd Data_Labeling
python anc_to_sim.py [archivo.anc] [salida.bin]
# Defaults to SGC2023keosra_CAP2_10.anc (M=6.6 Caribbean 2023-05-25, PGA=131 mg)
```

The output `.bin` includes 20 s of synthetic noise (LTA warmup) followed by up to 150 s of the real seismic event centered on the PGA peak. Copy to SD card root, uncomment `SIM_REPLAY_FILE` in `config.h`, then flash.

**Column mapping** (`.anc` → firmware axes): EW→ax, NS→ay, VER→az.

## Data not in git

The following are gitignored and must be obtained separately:
- `Data_Labeling/DatosObtenidos/` — raw `.anc` SGC files and `.bin` sensor captures
- `Data_Labeling/Dataset/0_Ruido/*.npy` and `1_Sismo/*.npy` — generated spectrograms
- `platform_acel/acel_ubuntu/.env` — device credentials
- `*.bin` binary recordings
