# Entrenamiento ML — PANdeMaiz Quake

Este módulo entrena la CNN de doble entrada (espectrograma + PGA) que detecta sismos, y la exporta en formato TFLite INT8 listo para ejecutarse en el ESP32 con TFLite Micro.

---

## Arquitectura del modelo

El clasificador usa **dos entradas en paralelo** que se combinan antes de la capa de decisión:

```
┌─ Input 0: PGA (3,)  ──────────────────────────────────────┐
│   Peak Ground Acceleration por eje (ax, ay, az) en g      │
│   Dense(8, ReLU)  →  pga_feat (8,)                        │
│                                                            │
├─ Input 1: Espectrograma (65, 11, 3)  ─────────────────────┤
│   log₁₀(PSD + 1e-12) por canal EW/VER/NS                  │
│   Conv2D(32, 3×3, same) + BatchNorm + ReLU                 │
│   MaxPool2D(2×2)                                           │
│   Conv2D(64, 3×3, same) + BatchNorm + ReLU                 │
│   GlobalAveragePooling2D  →  spec_feat (64,)               │
│                                                            │
├─ Concatenate([pga_feat, spec_feat])  →  (72,)              │
│   Dropout(0.3)                                             │
│   Dense(32, ReLU)                                          │
│   Dense(1, sigmoid)  →  score [0, 1]                       │
└────────────────────────────────────────────────────────────┘
```

| Métrica | Valor |
|---------|-------|
| Parámetros totales | ~27 500 |
| Tamaño FP32 | ~110 KB |
| **Tamaño INT8** | **~38 KB** |

La entrada PGA compensa que el espectrograma log₁₀(PSD) comprime la información de amplitud: el modelo puede distinguir simultáneamente la *distribución espectral* (¿el patrón frecuencial parece sismo?) y la *amplitud absoluta* (¿qué tan fuerte?).

---

## Hiperparámetros de entrenamiento

| Parámetro | Valor |
|-----------|-------|
| Optimizador | Adam, lr=1e-3 |
| Función de pérdida | Binary Crossentropy |
| Batch size | 32 |
| Máximo de épocas | 100 |
| Split | 70 % train / 15 % val / 15 % test (estratificado) |
| Class weight sismo | 1.1132 (compensa ligero desbalance) |

### Callbacks

| Callback | Configuración |
|----------|--------------|
| `EarlyStopping` | monitor=`val_roc_auc`, patience=20, restore best weights |
| `ReduceLROnPlateau` | factor=0.3, patience=10, min_lr=1e-6 |
| `ModelCheckpoint` | guarda `models/best_model.keras` por `val_roc_auc` |

---

## Métricas objetivo

| Métrica | Objetivo |
|---------|----------|
| Recall sismo (test) | ≥ 0.85 |
| ROC-AUC (test) | ≥ 0.83 |
| Tamaño INT8 | < 300 KB |
| Falsos negativos | minimizar (la política es conservadora) |

---

## Normalización

### Espectrograma (Z-score global)

```python
x_norm = (log10(PSD + 1e-12) − NORM_MEAN) / NORM_STD
NORM_MEAN = -7.98922157   # calculado sobre conjunto de entrenamiento
NORM_STD  =  1.37405312
```

### PGA (por eje)

```python
pga_norm[i] = (pga[i] − PGA_MEAN[i]) / PGA_STD[i]
PGA_MEAN = [0.01375112, 0.01821116, 0.04943195]  # g, ejes ax/ay/az
PGA_STD  = [0.06148684, 0.05301067, 0.15822884]
```

Estos valores se generan automáticamente al ejecutar el notebook y se guardan en `models/norm_params.npz` y `models/norm_constants.h`.

---

## Exportación a TFLite INT8

### Por qué `model.export()` y no `tf.saved_model.save()`

`tf.saved_model.save()` en TF 2.21 / Keras 3 deja las variables `ReadVariable` sin congelar, lo que impide que el cuantizador INT8 de TFLite determine el rango de los tensores durante la calibración. El método correcto es:

```python
model.export(export_path)   # congela pesos como constantes
```

### Proceso de conversión

```python
# 1. Convertidor FP32 (baseline)
converter = tf.lite.TFLiteConverter.from_saved_model(export_path)
tflite_fp32 = converter.convert()                # → 102 KB

# 2. Convertidor INT8 con dataset representativo
def repr_gen():
    for i in range(300):
        yield [X_spec_train_n[i:i+1], X_pga_train_n[i:i+1]]

converter = tf.lite.TFLiteConverter.from_saved_model(export_path)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = repr_gen
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type  = tf.float32   # inputs permanecen float32
converter.inference_output_type = tf.float32   # output permanece float32
tflite_int8 = converter.convert()              # → 38 KB
```

> El orden de los inputs en `repr_gen` debe coincidir exactamente con el orden en `model.inputs`.  
> Invertirlos produce errores silenciosos de cuantización.

### Por qué Conv2D y no Conv1D

La arquitectura usa `Conv2D` sobre el tensor `(65, 11, 3)`. Usar `Conv1D` requiere un `Reshape` que altera la estructura espacial del espectrograma. Alternativamente, `Conv2D` produce la operación `CONV_2D` en TFLite, que está incluida en `MicroMutableOpResolver` del firmware sin necesitar `AllOpsResolver`.

---

## Archivos generados

```
ML/models/
├── best_model.keras        # Modelo Keras completo (FP32)
├── seismic_fp32.tflite     # TFLite sin cuantizar (~102 KB)
├── seismic_int8.tflite     # TFLite INT8 (~38 KB) — para ESP32
├── model.h                 # Array C del modelo INT8
├── norm_constants.h        # Constantes de normalización para C++
├── norm_params.npz         # Backup NumPy de las constantes
├── training_curves.png     # Loss / AUC por época
└── confusion_matrix.png    # Matriz de confusión sobre test set
```

### Copiar al firmware después de reentrenar

```bash
cp ML/models/model.h          platform_acel/acel_ubuntu/src/model.h
cp ML/models/norm_constants.h platform_acel/acel_ubuntu/src/norm_constants.h
```

Luego recompilar y flashear: `pio run --target upload`.

---

## Configuración del entorno Python

```bash
# Desde la raíz del repositorio
source pan_env/bin/activate              # activar entorno existente
# — o crear desde cero —
uv venv pan_env && source pan_env/bin/activate
uv pip install -r requirements.txt

python -m ipykernel install --user --name pandemaiz --display-name "PANdeMaiz"
jupyter lab
```

Abrir `ML/seismic_cnn_trainer.ipynb` y ejecutar Kernel → Restart & Run All.  
El notebook asume que `Data_Labeling/Dataset/` contiene los `.npy` generados por `seismic_dataset_builder_v3.ipynb`.
