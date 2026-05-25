# Firmware — Nodo Acelerométrico PANdeMaiz Quake

Firmware en C++ para el nodo de adquisición sísmica. Implementa la cadena completa de procesamiento de señal en el microcontrolador ESP32: adquisición a 200 Hz, filtrado Butterworth en tiempo real, detección STA/LTA, inferencia TFLite INT8 y publicación de alertas en Firebase.

---

## Hardware requerido

| Componente | Modelo | Interfaz | Parámetros |
|------------|--------|----------|------------|
| Microcontrolador | ESP32 (240 MHz dual-core) | — | 4 MB Flash, 520 KB SRAM |
| Acelerómetro | ADXL345 | I2C @ 400 kHz | SDA=21, SCL=22, addr=0x53, ±2g full-res |
| Almacenamiento | MicroSD | SPI (VSPI) | CS=5, MOSI=23, SCK=18, MISO=19 |
| GPS | u-blox Neo 6M | UART @ 9600 bps | RX=4, TX=25, timeout 5 s |

### Diagrama de conexión resumido

```
ESP32 GPIO
──────────────────────────────────────────────
GPIO 21 (SDA) ──── ADXL345 SDA
GPIO 22 (SCL) ──── ADXL345 SCL
GPIO 18       ──── SD SCK
GPIO 19       ──── SD MISO
GPIO 23       ──── SD MOSI
GPIO  5       ──── SD CS
GPIO  4       ──── GPS TX (Neo 6M → ESP32 RX)
GPIO 25       ──── GPS RX (ESP32 TX → Neo 6M)
──────────────────────────────────────────────
```

---

## Cadena de señal (200 Hz)

```
ADXL345 ODR=200 Hz  (registro BW_RATE=0x0B)
    ↓  Lectura burst I2C: 6 bytes (ax, ay, az  int16)
Autocalibracion de bias  (AUTOCAL_SAMPLES=400 → 2 s en reposo)
    ax_c = ax_raw − g_sw_bias[i]
    ↓
Filtro Butterworth DF2T cascado por eje (3 ejes × 2 biquads)
    ├─ Biquad HP  fc=0.1 Hz  (elimina gravedad + deriva DC)
    └─ Biquad LP  fc=20 Hz   (elimina ruido mecánico > 20 Hz)
    ↓
Escritura en SD card  (/Aceleraciones/..., 1 min por archivo)
    ↓
Detector STA/LTA  (EMA, warmup 30 s)
    ↓  ratio ≥ 2.5  → trigger
Acumulación de ventana de 800 muestras (4 s)
    ↓
STFT Hanning 128 pt, hop 64  →  espectrograma (65, 11, 3)
    ↓  log₁₀(PSD + 1e-12), Z-score global
Inferencia TFLite INT8  (arena 64 KB)
    ↓  score > 0.05  → SISMO
Guardar en /Eventos + Upload Firebase RTDB
```

> El ISR del timer solo activa el flag `g_sampleDue`. Toda la lectura I2C y el procesamiento ocurren en `loop()` para evitar problemas de reentrancia en I2C.

---

## Parámetros de configuración (`config.h`)

El archivo `src/config.h` es el punto de entrada único para ajustar cualquier parámetro de DSP o hardware. **No modificar los coeficientes manualmente** — fueron calculados con el script de Python de la carpeta `ML/`.

### Muestreo

| Constante | Valor | Descripción |
|-----------|-------|-------------|
| `SAMPLE_RATE_HZ` | 200 | Hz |
| `SAMPLES_PER_FILE` | 12 000 | 200 × 60 = 1 min por archivo |
| `AUTOCAL_SAMPLES` | 400 | 2 s de autocalibracion al arrancar |

### Filtro Butterworth 2.° orden (DF2T)

Ecuaciones del biquad (Direct Form II Transposed):
```
y[n] = b0·x[n] + w1
w1   = b1·x[n] − a1·y[n] + w2
w2   = b2·x[n] − a2·y[n]
```

**Pasa-alto HP (fc = 0.1 Hz, fs = 200 Hz)**

| Coef | Valor |
|------|-------|
| b0 |  0.997781 |
| b1 | −1.995562 |
| b2 |  0.997781 |
| a1 | −1.995557 |
| a2 |  0.995566 |

**Pasa-bajo LP (fc = 20 Hz, fs = 200 Hz)**

| Coef | Valor |
|------|-------|
| b0 |  0.067456 |
| b1 |  0.134911 |
| b2 |  0.067456 |
| a1 | −1.142980 |
| a2 |  0.412823 |

### Detector STA/LTA (EMA)

| Constante | Valor | Equivalencia temporal |
|-----------|-------|-----------------------|
| `STA_SAMPLES` | 100 | 0.5 s |
| `LTA_SAMPLES` | 2 000 | 10.0 s |
| `STA_LTA_WARMUP` | 6 000 | 30 s (3× τ_LTA) |
| `STA_LTA_ON` | 2.5 | Umbral de disparo |
| `STA_LTA_OFF` | 1.5 | Umbral de detrigger |

La energía instantánea se calcula como `e = ax² + ay² + az²` (sobre señal filtrada).

### Inferencia TFLite

| Constante | Valor | Descripción |
|-----------|-------|-------------|
| `SEISMIC_WIN_SAMPLES` | 800 | 4 s de señal filtrada |
| `SEISMIC_FREQ_BINS` | 65 | nperseg/2 + 1 (FFT 128 pt) |
| `SEISMIC_TIME_BINS` | 11 | bins de tiempo en el espectrograma |
| `TENSOR_ARENA_SIZE` | 65 536 | 64 KB para el heap de TFLite |
| `SEISMIC_THRESHOLD` | 0.05 | score CNN mínimo para clasificar como sismo |
| `SEISMIC_NORM_MEAN` | −7.98922 | Media global log₁₀(PSD) |
| `SEISMIC_NORM_STD` | 1.37405 | Desv. estándar global |

---

## Formato de archivo `.bin`

### Cabecera (16 bytes, versión 3)

```
Offset  Tipo        Campo
  0     uint32_le   MAGIC = 0xDA7A1345
  4     uint16_le   VERSION = 0x0003
  6     uint16_le   SAMPLE_RATE = 200
  8     float32_le  lat  (GPS, 0.0 si sin fix)
 12     float32_le  lon  (GPS, 0.0 si sin fix)
```

### Muestra (10 bytes, little-endian)

```
Offset  Tipo       Campo
  0     uint32_le  timestamp_ms  (millis() en captura)
  4     int16_le   ax            (EW  post-filtro, LSB = 3.9 mg)
  6     int16_le   ay            (NS  post-filtro, LSB = 3.9 mg)
  8     int16_le   az            (VER post-filtro, LSB = 3.9 mg)
```

Tamaño por archivo: 16 + 12 000 × 10 = **120 016 bytes ≈ 117 KB/min**

---

## Estructura de carpetas en la SD

```
/Aceleraciones/
  YYYY/
    YYYY-MM/
      YYYY-MM-DD/
        HH/
          accel_YYYYMMDDHHmmss.bin    ← registro continuo (1 min)

/Eventos/
  YYYY/
    YYYY-MM/
      YYYY-MM-DD/
        HH/
          evento_YYYYMMDDHHmmss_sNN.bin   ← NN = score×100
```

---

## Servidor web embebido (ESPAsyncWebServer)

El nodo expone un servidor HTTP en el puerto 80 para descarga directa de archivos desde la LAN:

| Ruta | Respuesta |
|------|-----------|
| `GET /` | Dashboard HTML (explorador de archivos SD, estado del nodo) |
| `GET /download?f=/ruta/archivo.bin` | Descarga binaria del archivo (Cache-Control: no-cache) |
| `GET /status` | JSON con samples totales, drops, bias y estado del filtro |

El backend FastAPI actúa como proxy hacia este servidor (ver [backend/README.md](../../backend/README.md)).

---

## Configuración de credenciales

Las credenciales de WiFi y Firebase **nunca se escriben en el código fuente**. Se cargan en tiempo de compilación mediante el script `load_env.py`:

```bash
cp .env.example .env
# Editar .env con los valores reales:
#   WIFI_SSID, WIFI_PASS
#   FIREBASE_PROJECT_ID, FIREBASE_API_KEY
#   FIREBASE_USER_EMAIL, FIREBASE_USER_PASS
#   RTDB_URL, STORAGE_BUCKET_ID
```

El script lee `.env` y pasa cada variable como flag `-DKEY="value"` al compilador. Los valores quedan en la sección `.rodata` del binario (no en texto plano en el repositorio).

---

## Compilación y despliegue

```bash
# Construir
pio run

# Construir y flashear (ESP32 conectado por USB)
pio run --target upload

# Monitor serie (115 200 baud, decodificador de excepciones activado)
pio run --target monitor
```

### Librerías principales (`platformio.ini`)

| Librería | Versión |
|----------|---------|
| ESPAsyncWebServer | GitHub latest |
| AsyncTCP | GitHub latest |
| U8g2 (OLED) | 2.36.18 |
| TinyGPSPlus | 1.0.3 |
| TensorFlowLite_ESP32 | 1.0.0 |
| Firebase Arduino Client Library | 4.4.14 |

Flags de compilación relevantes: `-DTF_LITE_STATIC_MEMORY`, `-O2`, `-std=gnu++17`.

---

## Simulación de evento sísmico

Para validar el firmware sin necesitar un sismo real, se puede inyectar un evento SGC real en formato `.bin`:

```bash
# Desde la raíz del repositorio
cd Data_Labeling
python anc_to_sim.py [archivo.anc] [salida.bin]
# Genera 20 s de ruido sintético + hasta 150 s del evento real (centrado en PGA)
```

Pasos post-generación:
1. Copiar `salida.bin` a la raíz de la SD card.
2. En `config.h`, descomentar: `#define SIM_REPLAY_FILE "/salida.bin"`
3. Compilar y flashear: `pio run --target upload`
4. Observar en el monitor serie la secuencia: `[SIM] → [STA/LTA] TRIGGER → [CNN] SISMO → [Firebase] Upload`
