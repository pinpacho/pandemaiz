# PANdeMaiz Quake

Red acelerográfica portátil y distribuida para la detección y alerta temprana de sismos.
Desarrollada como trabajo de grado en la Universidad de Antioquia — Grupo GICM.

---

## Arquitectura del sistema

```
[Nodo ESP32 + ADXL345]
        |
        | (Wi-Fi / Firebase)
        v
[Firebase Realtime DB]
        |
        v
[API FastAPI + uvicorn]
        |
   ┌────┴────┐
   |         |
[Dashboard] [Alertas]
 Mapa calor  Gmail/Discord
```

### Componentes

| Módulo | Tecnología | Estado |
|--------|-----------|--------|
| Firmware nodo | ESP32 / C++ / PlatformIO | En desarrollo |
| Dataset ML | Python / ObsPy / SciPy | En desarrollo |
| Modelo edge | TensorFlow Lite / 1D-CNN | Pendiente |
| Backend API | FastAPI / uvicorn | Pendiente |
| Base de datos | Firebase Realtime DB | Pendiente |

---

## Hardware del nodo

- **MCU:** TTGO LILYGO T3 V1.6.1 (ESP32)
- **Acelerómetro:** ADXL345 (I2C, ODR=200 Hz, ±2g full-res)
- **GPS:** Neo 6M
- **Almacenamiento:** MicroSD
- **Display:** OLED SSD1306 128×64

### Pipeline de señal (200 Hz)

```
ADXL345 → burst 6 bytes → ax/ay/az (int16)
  → calibración bias (2s en reposo)
  → Filtro Biquad HP 0.1 Hz (Butterworth DF2T)
  → Filtro Biquad LP 20 Hz  (Butterworth DF2T)
  → SD card (.bin propio)
```

### Formato binario `.bin`

| Campo | Tamaño | Descripción |
|-------|--------|-------------|
| MAGIC | 4 bytes | `0xDA7A1345` |
| Version | 2 bytes | `0x0002` |
| Sample rate | 2 bytes | `200` Hz |
| Timestamp ms | 4 bytes | por muestra |
| ax / ay / az | 2+2+2 bytes | int16, LSB=3.9 mg |

---

## Pipeline de datos ML

### Fuentes de datos

- **Ruido ambiental** → archivos `.bin` del ADXL345 (capturas en campo: calle, parque)
- **Eventos sísmicos** → archivos `.anc` del SGC (Servicio Geológico Colombiano), 2023–2026

### Detección automática (STA/LTA)

| Parámetro | Valor |
|-----------|-------|
| Ventana corta (STA) | 0.5 s |
| Ventana larga (LTA) | 10.0 s |
| Umbral ON | 2.5 |
| Umbral OFF | 1.5 |
| Overlap mínimo | 30% de ventana |

### Espectrograma de salida

```
Ventana temporal: 4 s (configurable)
Shape: (65, 11, 3) float32
  65  = bins de frecuencia (0–100 Hz)
  11  = bins de tiempo  [= 1 + (WIN_SAMPLES - NPERSEG) // (NPERSEG - NOVERLAP)]
  3   = canales (EW, VER, NS)
Escala: log10(PSD + 1e-12)
```

### Augmentación (v3)

- Ruido gaussiano (σ del sensor real)
- Desplazamiento temporal circular (±0.2 s)
- Escalado de amplitud (×0.7 – ×1.3)

---

## Estructura del repositorio

```
PANdeMaiz/
├── platform_acel/
│   └── acel_ubuntu/          # Firmware ESP32 (PlatformIO)
│       ├── src/
│       │   ├── main.cpp
│       │   └── config.h
│       └── platformio.ini
├── Data_Labeling/
│   ├── DatosObtenidos/
│   │   ├── Acelerografo_SGC/ # .anc del SGC (2023–2026)
│   │   └── PANAcelerografo/  # .bin del sensor (calle, parque…)
│   ├── Dataset/
│   │   ├── 0_Ruido/          # .npy espectrogramas ruido
│   │   └── 1_Sismo/          # .npy espectrogramas sismo
│   ├── seismic_dataset_builder_v2.ipynb  # Referencia (no modificar)
│   └── seismic_dataset_builder_v3.ipynb  # Versión actual
├── requirements.txt
├── .gitignore
└── README.md
```

---

## Configuración del entorno

```bash
# Instalar uv (si no está disponible)
curl -LsSf https://astral.sh/uv/install.sh | sh

# Crear entorno y instalar dependencias
uv venv pan_env
source pan_env/bin/activate
uv pip install -r requirements.txt

# Registrar kernel en Jupyter
python -m ipykernel install --user --name pandemaiz --display-name "PANdeMaiz"

# Lanzar JupyterLab
jupyter lab
```

> **Nota para ObsPy en Linux:** si la instalación falla, ejecutar primero:
> `sudo apt-get install -y libxml2-dev libxslt-dev`

---

## Hoja de ruta

- [x] Firmware básico ESP32 (ADXL345 + SD + OLED + Web server)
- [x] Pipeline de etiquetado v2 (STA/LTA sobre .anc SGC)
- [ ] Dataset balanceado v3 (ruido ambiental + augmentación ampliada)
- [ ] Modelo 1D-CNN / CNN optimizado para TFLite (<300 KB)
- [ ] Integración GPS Neo 6M en firmware
- [ ] Backend FastAPI + Firebase (dashboard + alertas)

---

## Créditos

Universidad de Antioquia — Grupo de Investigación GICM  
Datos sísmicos: Servicio Geológico Colombiano (SGC)
