# Pipeline de Datos — PANdeMaiz Quake

Este módulo convierte registros sísmicos crudos en espectrogramas etiquetados para entrenar la CNN embebida en el firmware. La herramienta canónica es `seismic_dataset_builder_v3.ipynb`. El archivo v2 se conserva como referencia histórica y **no debe modificarse**.

---

## Fuentes de datos

| Clase | Fuente | Formato | Etiqueta |
|-------|--------|---------|---------|
| 0 — Ruido ambiental | Capturas propias del nodo ADXL345 (calle, parque, laboratorio) | `.bin` | Automática (toda ventana es ruido) |
| 1 — Sismo | Red Sismológica Nacional — SGC (2023–2026) | `.anc` | STA/LTA sobre señal SGC + ADXL345ificación |

```
DatosObtenidos/
├── PANAcelerografo/
│   └── <entorno>/          # .bin del sensor
└── Acelerografo_SGC/
    └── <evento>/           # .anc del SGC
```

> Estos directorios están en `.gitignore`. Solicitar los datos al equipo GICM.

---

## Formato `.anc` del SGC

Archivo de texto plano:
- **Líneas 1–20:** Cabecera de metadatos (estación, coordenadas, muestra, etc.)
- **Líneas 21+:** Datos numéricos en tres columnas: `EW [cm/s²]  VER [cm/s²]  NS [cm/s²]`

Mapeo de ejes a los canales del firmware (ADXL345):

| Columna `.anc` | Eje físico | Eje firmware |
|----------------|-----------|-------------|
| Columna 0 | EW (Este-Oeste) | ax |
| Columna 1 | VER (Vertical) | az |
| Columna 2 | NS (Norte-Sur) | ay |

---

## Funciones de lectura

### `parse_bin(path)`

```python
header, data = parse_bin("accel_20260504_161511.bin")
# header: dict  {version, sample_rate, lat, lon}
# data:   ndarray (N, 4) float32  →  [t_s, ax_g, ay_g, az_g]
#         escala: 1 LSB = 0.0039 g  (ADXL345 ±2g full-resolution)
```

Compatible con versiones v2 (cabecera 8 bytes) y v3 (cabecera 16 bytes con GPS).

### `parse_anc(path)`

```python
meta, data = parse_anc("SGC2023keosra_CAP2_10.anc")
# meta: dict  {estacion, lat, lon, profundidad, magnitud, ...}
# data: ndarray (N, 3) float32  →  [EW_g, VER_g, NS_g]
#       conversión: cm/s² ÷ 980.665
```

---

## Pipeline de espectrogramas

```
Señal filtrada (3 canales, N muestras @ 200 Hz)
    ↓
Ventana deslizante: 800 muestras (4 s), stride configurable
    ↓
STFT por canal  (scipy.signal.spectrogram)
    nperseg = 128,  noverlap = 64,  window = 'hann'
    → Sxx: shape (65, 11)  por canal
    65 = nperseg/2 + 1       (bins de frecuencia, 0–100 Hz)
    11 = time bins            (1 + (800 − 64) / (128 − 64))
    ↓
Escala logarítmica:  log₁₀(Sxx + 1e-12)   [g²/Hz → dB aproximado]
    ↓
Stack de 3 canales:  shape final (65, 11, 3)  float32
    orden:  [EW, VER, NS]
```

---

## Etiquetado STA/LTA

El etiquetado de ventanas en archivos SGC usa los **mismos parámetros que el firmware** para garantizar coherencia entre entrenamiento e inferencia en producción:

| Parámetro | Notebook | Firmware (`config.h`) |
|-----------|----------|-----------------------|
| STA (muestras) | 100 (0.5 s) | `STA_SAMPLES = 100` |
| LTA (muestras) | 2 000 (10 s) | `LTA_SAMPLES = 2000` |
| Umbral ON | 2.5 | `STA_LTA_ON = 2.5` |
| Umbral OFF | 1.5 | `STA_LTA_OFF = 1.5` |
| Overlap mínimo para etiqueta sismo | ≥ 50% de muestras en ventana | — |

Implementación: `obspy.signal.trigger.classic_sta_lta()` sobre la energía compuesta √(ax² + ay² + az²).

---

## ADXL345ificación sintética

Los registros del SGC provienen de sensómetros de banda ancha profesionales (Trillium, RefTek) con piso de ruido ~10⁻¹⁰ g²/Hz — órdenes de magnitud más limpio que el ADXL345. Para que el modelo generalice correctamente al hardware real, cada ventana SGC pasa por la función `adxl345ify()`:

1. **Clipping a ±2 g** — simula la saturación del ADXL345 en modo full-resolution.
2. **Cuantización a 3.9 mg/LSB** — `round(x / 0.0039) × 0.0039`
3. **Ruido blanco gaussiano** — σ medido empíricamente en laboratorio (sensor en reposo):

| Eje | σ (g) |
|-----|-------|
| ax (EW) | 0.000496 |
| ay (NS) | 0.000683 |
| az (VER) | 0.001320 |

> Estos valores no deben modificarse. Representan el piso de ruido real del ADXL345 en las condiciones de montaje del nodo.

---

## Augmentación de datos (v3)

Cada archivo `.anc` genera **4 variantes** por ventana para aumentar la diversidad del dataset:

| Variante | Transformación |
|----------|---------------|
| `orig` | Señal original sin modificar |
| `gauss` | Ruido gaussiano añadido (σ del ADXL345 por eje) |
| `shift` | Desplazamiento circular ±0.2 s (±40 muestras), semilla determinista |
| `scale` | Amplitud × U(0.7, 1.3), semilla determinista |

Todas las variantes reciben también la ADXL345ificación descrita arriba.  
La semilla se calcula como `hash(stem) % 2³¹` — resultados reproducibles por archivo.

---

## Salida del notebook

Los espectrogramas se guardan en:

```
Dataset/
├── 0_Ruido/
│   ├── noise_<entorno>_<stem>_w<idx>.npy        # (65, 11, 3) float32
│   └── noise_<entorno>_<stem>_w<idx>_pga.npy   # (3,) float32 [ax, ay, az en g]
└── 1_Sismo/
    ├── sgc_<stem>_<aug>_1S_w<idx>.npy
    └── sgc_<stem>_<aug>_1S_w<idx>_pga.npy
```

El notebook es **idempotente**: detecta los stems ya procesados comparando con los nombres de archivos `.npy` existentes y los omite.

---

## Herramienta de simulación — `anc_to_sim.py`

Convierte un archivo `.anc` del SGC en un `.bin` compatible con el firmware para validar el sistema de extremo a extremo sin necesitar un sismo real.

```bash
cd Data_Labeling
python anc_to_sim.py [archivo.anc] [salida.bin]
# Valores por defecto:
#   entrada: SGC2023keosra_CAP2_10.anc  (M=6.6 Caribe, 2023-05-25, PGA=131 mg)
#   salida:  sim_sismo.bin
```

**Estructura del `.bin` generado:**
- 20 s de ruido gaussiano sintético (σ = 0.55 × 3.9 mg ≈ 2.1 mg) → precalienta el filtro LTA del firmware
- Hasta 150 s del evento real, centrado en el pico de PGA

**Uso en firmware:**
1. Copiar `sim_sismo.bin` a la raíz de la SD card.
2. En `config.h`, descomentar `#define SIM_REPLAY_FILE "/sim_sismo.bin"`.
3. Compilar y flashear: `pio run --target upload`.

---

## Configuración del entorno Python

```bash
# Crear entorno (requiere uv)
uv venv pan_env
source pan_env/bin/activate
uv pip install -r ../requirements.txt

# En Linux, si ObsPy falla en la instalación:
sudo apt-get install -y libxml2-dev libxslt-dev

# Registrar kernel en Jupyter
python -m ipykernel install --user --name pandemaiz --display-name "PANdeMaiz"

# Lanzar JupyterLab
jupyter lab
```

Abrir `seismic_dataset_builder_v3.ipynb` y ejecutar todas las celdas (Kernel → Restart & Run All). Al finalizar, el directorio `Dataset/` contendrá los archivos `.npy` listos para entrenamiento.
