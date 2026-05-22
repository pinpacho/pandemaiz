#pragma once
// ============================================================
//  config.h — Parámetros de usuario para DataLogger ADXL345
//  Versión 2.0 — Instrumento Sismológico
//
//  Modifica SOLO este archivo para adaptar a tu entorno.
//  Los coeficientes del filtro se recalculan aquí si cambias
//  SAMPLE_RATE_HZ o las frecuencias de corte BP_HP/LP_FREQ_HZ.
// ============================================================

// ---- WiFi — credenciales en .env (load_env.py las inyecta) -
// Personal  (WPA2):  definir WIFI_SSID + WIFI_PASS
// Enterprise (PEAP): definir además WIFI_IDENTITY + WIFI_USERNAME
#ifndef WIFI_SSID
#define WIFI_SSID     "YOUR_SSID"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS     "YOUR_PASSWORD"
#endif
#ifndef WIFI_IDENTITY
#define WIFI_IDENTITY ""
#endif
#ifndef WIFI_USERNAME
#define WIFI_USERNAME ""
#endif

// ---- NTP / Zona horaria ------------------------------------
// Colombia = UTC-5 (sin horario de verano)
#define NTP_SERVER       "pool.ntp.org"
#define NTP_SERVER2      "time.google.com"
#define GMT_OFFSET_SEC   (-5 * 3600)
#define DAYLIGHT_SEC     0

// ---- Muestreo ADXL345 -------------------------------------
#define SAMPLE_RATE_HZ   200               // Hz — debe coincidir con ODR del sensor
#define SAMPLES_PER_FILE (SAMPLE_RATE_HZ * 60)  // 12000 muestras = 1 minuto

// ---- Registro BW_RATE del ADXL345 (Tabla 7 del datasheet) -
// 0x0A = 100 Hz (anterior — INCORRECTO para timer a 200 Hz)
// 0x0B = 200 Hz, modo normal (LOW_POWER=0), BW interna = 100 Hz, IDD = 145 µA
// Se usa en adxl_init() para el registro ADXL345_REG::BW_RATE.
#define ADXL_ODR_200HZ   0x0B

// ---- Filtro Paso-Banda Butterworth 2do Orden ---------------
//
//  Topología: dos biquads en cascada (SOS — Second Order Sections)
//    Etapa 1 → High-Pass:  elimina DC, gravedad y drift térmico
//    Etapa 2 → Low-Pass:   elimina vibraciones mecánicas del metro y ruido
//
//  Frecuencias de corte configurables:
#define BP_HP_FREQ_HZ    0.1f    // [Hz] Corte inferior  (0.05–1.0 Hz recomendado)
#define BP_LP_FREQ_HZ    20.0f   // [Hz] Corte superior  (10–25 Hz recomendado)
//
//  Coeficientes precalculados para fs = 200 Hz
//  Derivados con transformada bilineal + prewarping:
//    fc_pre = (2 * fs / pi) * tan(pi * fc / fs)
//  Forma directa II transpuesta (DF2T) — robusta en float de 32 bits.
//  Ecuaciones:
//    y[n] = b0*x[n] + w1
//    w1   = b1*x[n] - a1*y[n] + w2
//    w2   = b2*x[n] - a2*y[n]
//
//  ── HIGH-PASS  fc = 0.1 Hz, fs = 200 Hz, Q = 1/√2 (Butterworth) ──
//  ω₀ = 2π × 0.1 / 200 = 0.003142 rad/muestra
//  sin(ω₀) = 0.003142,  cos(ω₀) = 0.999995,  α = sin/(2Q) = 0.002221
//  a0 = 1.002221
#define BP_HP_B0   ( 0.997781f)   //  (1 + cos)/2  / a0
#define BP_HP_B1   (-1.995562f)   // -(1 + cos)    / a0
#define BP_HP_B2   ( 0.997781f)   //  (1 + cos)/2  / a0
#define BP_HP_A1   (-1.995557f)   // -2·cos        / a0  (negativo → feedback HP)
#define BP_HP_A2   ( 0.995566f)   //  (1 - α)      / a0

//  ── LOW-PASS   fc = 20 Hz, fs = 200 Hz, Q = 1/√2 (Butterworth) ──
//  ω₀ = 2π × 20 / 200 = 0.628319 rad/muestra
//  sin(ω₀) = 0.587785, cos(ω₀) = 0.809017, α = sin/(2Q) = 0.415666
//  a0 = 1.415666
#define BP_LP_B0   ( 0.067456f)   //  (1 - cos)/2  / a0
#define BP_LP_B1   ( 0.134911f)   //  (1 - cos)    / a0
#define BP_LP_B2   ( 0.067456f)   //  (1 - cos)/2  / a0
#define BP_LP_A1   (-1.142980f)   // -2·cos        / a0  (negativo → feedback LP)
#define BP_LP_A2   ( 0.412823f)   //  (1 - α)      / a0

// ---- Autocalibración de offset de hardware -----------------
// Número de muestras para estimar el bias de reposo.
// A 200 Hz, 400 muestras = 2 segundos. El sensor debe estar
// completamente estático durante este período al arrancar.
#define AUTOCAL_SAMPLES  400     // 2 s a 200 Hz

// ---- Filtro de gravedad (LEGACY — reemplazado por el BP) ---
// Mantenido solo para referencia histórica. No se usa en v2.0.
// #define GRAVITY_ALPHA   0.99968f

// ---- Puertos I2C (ADXL345) --------------------------------
#define PIN_I2C_SDA     21
#define PIN_I2C_SCL     22

// ---- Pines MicroSD (VSPI — ESP32 D1 R32) ------------------
24

// ---- Dirección I2C del ADXL345 ----------------------------
// SDO/ALT_ADDRESS → GND  ⟹  0x53
// SDO/ALT_ADDRESS → VCC  ⟹  0x1D
#define ADXL345_I2C_ADDR  0x53

// ---- Puerto Web Server ------------------------------------
#define WEB_SERVER_PORT  80

// ---- Formato binario del archivo --------------------------
// Header  (16 bytes): MAGIC(4) + VERSION(2) + SAMPLE_RATE(2) + lat(4) + lon(4)
// Muestra (10 bytes): timestamp_ms(4) + ax_bp(2) + ay_bp(2) + az_bp(2)
//   ax_bp / ay_bp / az_bp = señal con filtro paso-banda aplicado (LSB)
// Tamaño por minuto: 16 + 12000×10 = 120016 bytes ≈ 117 KB
#define BIN_MAGIC        0xDA7A1345UL
#define BIN_VERSION      0x0003         // v3 — agrega lat/lon GPS al header

// ---- Buffer de escritura SD --------------------------------
#define WRITE_FLUSH_EVERY 200  // flush SD cada N muestras (~1 s a 200 Hz)

// ---- GPS Neo 6M -------------------------------------------
// GPIO4  = libre en D1 R32 (RX: recibe TX del Neo 6M)
// GPIO25 = DAC1, libre en D1 R32 (TX: envía al Neo 6M)
#define GPS_RX_PIN        4           // GPIO4  ← TX del Neo 6M
#define GPS_TX_PIN       25           // GPIO25 → RX del Neo 6M
#define GPS_BAUD         9600
#define GPS_TIMEOUT_MS   5000         // sin fix en 5 s → lat=0.0, lon=0.0

// ---- Coordenadas estáticas (fallback sin GPS) ---------------
// Definir en .env como STATIC_LAT y STATIC_LON (float, ej: 6.26649).
// Se usan si gps_init() no obtiene fix; gps_update() las reemplaza
// automáticamente en cuanto el módulo consiga señal real.
// Si ambas son 0.0 no se aplica ningún fallback.
#ifndef STATIC_LAT
#define STATIC_LAT 0.0f
#endif
#ifndef STATIC_LON
#define STATIC_LON 0.0f
#endif

// ---- Inferencia TFLite Micro ------------------------------
#define SEISMIC_WIN_SAMPLES  800      // 4.0 s × 200 Hz
#define SEISMIC_TIME_BINS    11       // 1 + (800-128)/64  (fórmula scipy)
#define SEISMIC_FREQ_BINS    65       // nperseg/2 + 1
#define TENSOR_ARENA_SIZE    (64 * 1024)
// Media mínima del log10-PSD (promedio sobre todos los bins) para invocar la CNN.
// Entrenamiento: media = -7.989, std = 1.374 → umbral = -11.0 ≈ media - 2.2σ.
// Señal casi-cero (cuantización ADXL345) da media ≈ -12.0 → inferencia omitida.
// Sismo mínimo detectable (>1 mg RMS) da media ≈ -8.8 → inferencia permitida.
#define CNN_MIN_LOG_PSD  -11.0f

// ---- STA/LTA trigger (mismos parámetros que seismic_dataset_builder_v3) ----
// EMA (exponential moving average): evita ring buffer, 5 flops/muestra.
// τ_STA = 0.5 s · τ_LTA = 10 s — igual que builder Python.
// Warmup: 30 s para que g_lta converja (3×τ_LTA → error < 5 %).
#define STA_SAMPLES    100      // 0.5 s @ 200 Hz
#define LTA_SAMPLES    2000     // 10.0 s @ 200 Hz
#define STA_LTA_ON       2.5f    // ratio para activar trigger
#define STA_LTA_OFF      1.5f    // ratio para desactivar trigger
#define STA_LTA_WARMUP   6000    // 30 s de calentamiento al arrancar
// Energía mínima del STA para que el trigger pueda activarse.
// Equivale a ~0.7 mg RMS sostenidos durante 0.5 s.
// Un pico de 1 LSB (3.9 mg) aislado solo eleva el STA ~1.5e-7 g² → insuficiente.
#define STA_LTA_MIN_STA  5e-7f   // g² — guarda contra falsos positivos por cuantización

// ---- Rutas SD (logger dual) --------------------------------
#define SD_DIR_ACEL      "/Aceleraciones"
#define SD_DIR_EVENTOS   "/Eventos"

// ---- Simulación desde SD (solo para validación, comentar en producción) ----
// Descomentar para reproducir un sismo real almacenado en la SD.
// Generar el archivo con: Data_Labeling/anc_to_sim.py
//#define SIM_REPLAY_FILE   "/sim_sismo.bin"

// ---- Identificación de estación ----------------------------
// Valor real proviene de .env vía load_env.py.
// Fallback: genera "PAN_XXYYZZ" desde los últimos 3 bytes MAC.
#ifndef STATION_NAME
#define STATION_NAME        ""
#endif

// ---- Firebase ----------------------------------------------
// Valores reales en .env (nunca commitear credenciales aquí).
#ifndef FIREBASE_PROJECT_ID
#define FIREBASE_PROJECT_ID "YOUR_PROJECT_ID"
#endif
#ifndef FIREBASE_API_KEY
#define FIREBASE_API_KEY    "YOUR_WEB_API_KEY"
#endif
#ifndef FIREBASE_USER_EMAIL
#define FIREBASE_USER_EMAIL "device@pandemaiz.com"
#endif
#ifndef FIREBASE_USER_PASS
#define FIREBASE_USER_PASS  "YOUR_DEVICE_PASSWORD"
#endif
#ifndef STORAGE_BUCKET_ID
#define STORAGE_BUCKET_ID   "YOUR_PROJECT_ID.appspot.com"
#endif
#ifndef RTDB_URL
#define RTDB_URL            "https://YOUR_PROJECT_ID-default-rtdb.firebaseio.com/"
#endif

// ---- Cola de uploads ---------------------------------------
#define UPLOAD_QUEUE_SIZE   8           // archivos pendientes máx.


