# Post_Analysis — Análisis post-captura de registros acelerométricos

Herramienta de análisis **offline** para explorar los archivos `.bin` grabados por los nodos ESP32 del proyecto PANdeMaiz Quake. No requiere conexión a Firebase ni al backend.

---

## Propósito

Una vez descargados los archivos `.bin` de la tarjeta SD (o del backend), este módulo permite:

1. **Inspeccionar** el contenido del archivo: número de muestras, duración, versión, coordenadas GPS (v3).
2. **Convertir** a formatos estándar:
   - **MiniSEED** (`.mseed`) — para análisis con ObsPy, SeisComp, etc.
   - **SGC-ANC** (`.anc`) — compatible con el `seismic_dataset_builder_v3.ipynb`.
3. **Visualizar** con 6 tipos de gráfica por archivo:
   - Serie de tiempo en **g** · eje relativo (desde 0 s)
   - Serie de tiempo en **g** · eje fecha/hora real
   - Serie de tiempo en **cm/s²** · eje relativo
   - Serie de tiempo en **cm/s²** · eje fecha/hora real
   - Espectrogramas STFT en las 3 componentes (EW, NS, VER)
   - Detección STA/LTA con zonas de evento resaltadas
4. **Resumir** estadísticas: duración, PGA por eje, número de triggers detectados.

---

## Estructura de carpetas

```
Post_Analysis/
├── post_analysis.ipynb        ← notebook principal (ejecutar en orden)
├── README.md                  ← este archivo
├── data/
│   ├── aceleraciones/         ← coloca aquí los .bin de grabación continua
│   └── eventos/               ← coloca aquí los .bin de eventos detectados
└── output/                    ← generado automáticamente al ejecutar el notebook
    ├── mseed/                 ← archivos .mseed convertidos
    ├── anc/                   ← archivos .anc convertidos
    └── plots/                 ← figuras PNG guardadas
```

> `data/` y `output/` están en `.gitignore` — los datos crudos y las figuras no se suben al repositorio.

---

## Uso rápido

```bash
# 1. Activar el entorno virtual del proyecto
source pan_env/bin/activate          # desde la raíz del repositorio

# 2. Copiar tus archivos .bin a las carpetas correspondientes
cp /ruta/a/mis/archivos/*.bin Post_Analysis/data/aceleraciones/
cp /ruta/a/eventos/*.bin      Post_Analysis/data/eventos/

# 3. Abrir Jupyter Lab
jupyter lab

# 4. Abrir post_analysis.ipynb y ejecutar todas las celdas en orden (Run All)
```

---

## Nombre de archivo recomendado

Para que el eje de **fecha/hora real** funcione, el nombre del archivo debe contener el patrón `YYYYMMDD_HHMMSS`:

```
PAN01_20260515_143000.bin       ✓
accel_20260515_143000.bin       ✓
evento_20260515_143512.bin      ✓
registro.bin                    ✗  (usa eje relativo como fallback)
```

El firmware del ESP32 genera archivos con este patrón por defecto.

---

## Descripción de cada sección del notebook

| Sección | Contenido |
|---|---|
| §1 | Título, descripción y tabla de mapeo de ejes |
| §2 | Importaciones (numpy, matplotlib, scipy, obspy) |
| §3 | **Configuración** — rutas, station ID, parámetros STA/LTA |
| §4 | `parse_bin()` — lectura del formato binario del ESP32 |
| §5 | `to_mseed()` — conversión a obspy.Stream |
| §6 | `to_anc()` — conversión al formato SGC con cabecera de 20 líneas |
| §7 | `discover_bins()` — listado y tabla de archivos |
| §8 | Conversión en lote (`.bin` → `.mseed` + `.anc`) |
| §9 | Funciones de eje de tiempo (relativo y fecha/hora real) |
| §10 | Funciones de graficación (`plot_timeseries`, `plot_spectrogram`, `plot_sta_lta`) |
| §11–16 | Gráficas de la carpeta **aceleraciones** (6 celdas, un tipo por celda) |
| §17–22 | Gráficas de la carpeta **eventos** (6 celdas, un tipo por celda) |
| §23 | Tabla resumen con PGA, duración, triggers y coordenadas |

---

## Formato `.bin` del ESP32

### Cabecera

| Campo | Tipo | Bytes | Descripción |
|---|---|---|---|
| MAGIC | uint32 LE | 4 | `0xDA7A1345` — identificador del formato |
| VERSION | uint16 LE | 2 | 2 = sin GPS · 3 = con lat/lon |
| SAMPLE_RATE | uint16 LE | 2 | Hz (normalmente 200) |
| lat | float32 LE | 4 | Solo versión 3 |
| lon | float32 LE | 4 | Solo versión 3 |

### Por muestra (10 bytes)

| Campo | Tipo | Bytes | Escala |
|---|---|---|---|
| timestamp_ms | uint32 LE | 4 | ms desde inicio de grabación |
| ax | int16 LE | 2 | 1 LSB = 3.9 mg (EW) |
| ay | int16 LE | 2 | 1 LSB = 3.9 mg (NS) |
| az | int16 LE | 2 | 1 LSB = 3.9 mg (VER) |

### Mapeo de ejes

| Canal firmware | Canal FDSN (MiniSEED) | Canal SGC (`.anc`) | Unidades |
|---|---|---|---|
| ax | HNE | EW (col 0) | g → cm/s² ×980.665 |
| ay | HNN | NS (col 2) | g → cm/s² ×980.665 |
| az | HNZ | VER (col 1) | g → cm/s² ×980.665 |

---

## Parámetros STA/LTA

Los siguientes valores son **idénticos** a los del firmware (`config.h`) y al dataset builder (`seismic_dataset_builder_v3.ipynb`):

| Parámetro | Valor | Equivalencia temporal |
|---|---|---|
| STA | 100 muestras | 0.5 s |
| LTA | 2000 muestras | 10.0 s |
| Umbral ON | 2.5 | — |
| Umbral OFF | 1.5 | — |

---

## Dependencias

Las mismas del entorno principal del proyecto (`pan_env`):

```
numpy
matplotlib
scipy
obspy
```

Instalar si no están:
```bash
uv pip install numpy matplotlib scipy obspy
```

---

## Outputs generados

| Tipo | Ubicación | Descripción |
|---|---|---|
| MiniSEED | `output/mseed/{stem}.mseed` | Stream de 3 trazas (HNE, HNN, HNZ) |
| ANC | `output/anc/{stem}.anc` | Texto SGC con cabecera de 20 líneas |
| PNG | `output/plots/{stem}_*.png` | Figuras de series de tiempo y espectrogramas |

Los archivos `.mseed` pueden abrirse con `obspy.read("output/mseed/archivo.mseed")`.
Los archivos `.anc` son compatibles con `parse_anc()` del `seismic_dataset_builder_v3.ipynb`.
