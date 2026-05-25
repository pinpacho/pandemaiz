# PANdeMaiz Quake — Red Acelerográfica Portátil de Alerta Temprana con IoT y Machine Learning

![Python](https://img.shields.io/badge/Python-3.11+-3776AB?logo=python&logoColor=white)
![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-F5822A?logo=platformio&logoColor=white)
![FastAPI](https://img.shields.io/badge/FastAPI-0.111+-009688?logo=fastapi&logoColor=white)
![TFLite](https://img.shields.io/badge/TFLite-INT8-FF6F00?logo=tensorflow&logoColor=white)
![License](https://img.shields.io/badge/License-MIT-green)

PANdeMaiz Quake es una red distribuida de nodos acelerométricos de bajo costo para la detección y alerta temprana de sismos. Cada nodo integra un microcontrolador ESP32 con un acelerómetro ADXL345 que registra aceleración del suelo a 200 Hz, aplica un filtro paso-banda Butterworth (0.1–20 Hz) y ejecuta un clasificador de redes neuronales convolucionales directamente en el dispositivo (Edge AI). Cuando múltiples nodos detectan un evento de forma simultánea, un motor de consenso distribuido en la nube dispara alertas multicanal (Email, Discord, Telegram) y actualiza un dashboard cartográfico en tiempo real.

---

## Arquitectura del sistema

```
┌─────────────────────────────────────────────────────┐
│             CAPA DE ADQUISICIÓN (Edge)               │
│  [ESP32 + ADXL345]  →  Filtrado → SD card (.bin)    │
│       ↓ STA/LTA trigger                              │
│  [TFLite INT8 CNN]  →  score > 0.05 → Firebase      │
└──────────────────────────┬──────────────────────────┘
                           │  Wi-Fi / Firebase RTDB
┌──────────────────────────▼──────────────────────────┐
│               CAPA DE PROCESAMIENTO                  │
│  [Firebase RTDB /alertas]  ←  múltiples nodos       │
│       ↓ SSE listener (FastAPI backend)               │
│  [Motor de Consenso]  →  ≥2 estaciones en 30 s      │
│       ↓ alerta global                                │
│  [Notificador]  →  Email + Discord + Telegram        │
└──────────────────────────┬──────────────────────────┘
                           │  API REST
┌──────────────────────────▼──────────────────────────┐
│                CAPA DE VISUALIZACIÓN                 │
│  [Dashboard Leaflet.js]  →  pines verde/rojo         │
│  [Descarga histórica]    →  ZIP bin/anc/mseed        │
└─────────────────────────────────────────────────────┘
```

---

## Subsistemas

| Subsistema | Carpeta | README |
|------------|---------|--------|
| Firmware ESP32 | `platform_acel/acel_ubuntu/` | [README](platform_acel/acel_ubuntu/README.md) |
| Pipeline de datos | `Data_Labeling/` | [README](Data_Labeling/README.md) |
| Entrenamiento ML | `ML/` | [README](ML/README.md) |
| Backend API + Consenso | `backend/` | [README](backend/README.md) |
| Dashboard + Conversión | `backend/static/` | [README](backend/static/README.md) |

---

## Requisitos mínimos de despliegue

| Componente | Requisito |
|------------|-----------|
| Python | 3.11+ |
| PlatformIO | CLI o extensión VS Code |
| Docker + Compose | v24+ |
| Firebase | Proyecto con RTDB habilitado |
| Hardware nodo | ESP32, ADXL345, MicroSD |
| SO host (backend) | Linux/macOS/Windows con Docker |

---

## Quickstart global

### 1 — Firmware

```bash
cd platform_acel/acel_ubuntu
cp .env.example .env          # completar WiFi + Firebase
pio run --target upload       # compilar y flashear
pio run --target monitor      # monitor serie 115200 baud
```

### 2 — Pipeline de datos + entrenamiento

```bash
source pan_env/bin/activate   # o: uv venv pan_env && source pan_env/bin/activate
                               #    uv pip install -r requirements.txt
jupyter lab                   # abrir seismic_dataset_builder_v3.ipynb → Run All
                               # luego ML/seismic_cnn_trainer.ipynb → Run All
cp ML/models/model.h          platform_acel/acel_ubuntu/src/model.h
cp ML/models/norm_constants.h platform_acel/acel_ubuntu/src/norm_constants.h
```

### 3 — Backend

```bash
cd backend
cp .env.example .env          # completar Firebase + canales de alerta
# copiar firebase.json (cuenta de servicio) a backend/
docker-compose up --build     # expone http://localhost:8000
```

---

## Estructura del repositorio

```
PANdeMaiz/
├── platform_acel/
│   └── acel_ubuntu/              # Firmware ESP32 (PlatformIO)
│       ├── src/
│       │   ├── main.cpp          # Loop principal, ISR, TFLite inference
│       │   ├── config.h          # Todos los parámetros DSP y pines
│       │   ├── model.h           # Modelo TFLite embebido (generado)
│       │   └── norm_constants.h  # Media/std para normalización (generado)
│       ├── platformio.ini
│       ├── load_env.py           # Pre-script inyección de credenciales
│       └── .env.example
├── Data_Labeling/
│   ├── seismic_dataset_builder_v3.ipynb  # Notebook canónico
│   ├── seismic_dataset_builder_v2.ipynb  # Referencia (no modificar)
│   ├── anc_to_sim.py                     # Convertidor .anc → .bin simulación
│   ├── DatosObtenidos/                   # Raw data (gitignored)
│   └── Dataset/                          # Espectrogramas .npy (gitignored)
├── ML/
│   ├── seismic_cnn_trainer.ipynb         # Entrenamiento + exportación TFLite
│   └── models/                           # best_model.keras, *.tflite, *.h
├── backend/
│   ├── app/
│   │   ├── main.py               # FastAPI app + lifespan Firebase
│   │   ├── consensus.py          # Motor de consenso cooperativo
│   │   ├── firebase.py           # RTDB helpers + SSE listener
│   │   ├── notifier.py           # Email + Discord + Telegram
│   │   ├── converter.py          # parse_bin, bin_to_anc, bin_to_mseed
│   │   └── routers/              # stations, alerts, download, history
│   ├── static/                   # Dashboard Leaflet.js
│   ├── docker-compose.yml
│   ├── Dockerfile
│   └── .env.example
├── requirements.txt              # Entorno Python (datos + ML)
└── README.md
```

---

## Contacto

**Julian Francisco Pinchao Ortiz**
jfrancisco.pinchao@udea.edu.co

Scientific Instrumentation and Microelectronics Research Group — GICM

Physics Institute, Exact and Natural Sciences Faculty

Universidad de Antioquia UdeA

---

