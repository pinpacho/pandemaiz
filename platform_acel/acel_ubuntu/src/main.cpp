// ============================================================
//  main.cpp — DataLogger / Acelerógrafo Sismológico v2.0
//  Hardware : TTGO LILYGO T3 V1.6.1 (ESP32) + ADXL345
//  Versión  : 2.0 — Instrumento de medición sismológica
// ============================================================
//
//  CAMBIOS v2.0 respecto a v1.0:
//  ─────────────────────────────────────────────────────────
//  1. ODR del ADXL345 corregido a 200 Hz (era 100 Hz, timer
//     ya estaba a 200 Hz → v1 registraba muestras duplicadas)
//  2. Bug fix: DEVID se comparaba contra 0xE6 (incorrecto);
//     el datasheet especifica 0xE5.
//  3. Filtro de gravedad RC de 1er orden reemplazado por
//     filtro paso-banda Butterworth 2do orden (0.1–20 Hz).
//     Se aplica a los tres ejes (X, Y, Z).
//  4. Autocalibración de offset al arranque: promedia 2 s de
//     reposo, resta el bias por software antes de filtrar.
//  5. BIN_VERSION subido a 0x0002 (todos los ejes filtrados).
//
//  ARQUITECTURA DE SEÑAL:
//  ┌──────────┐  I2C @400kHz  ┌──────────────────────────────┐
//  │ ADXL345  │ ────────────► │ adxl_readXYZ()  (ax,ay,az)   │
//  │ ODR 200Hz│               │    │                          │
//  └──────────┘               │    ▼                          │
//                             │ bias = ax - g_sw_bias[axis]   │
//                             │    │                          │
//                             │    ▼                          │
//                             │ HP biquad 0.1 Hz (por eje)   │
//                             │    │  elimina DC y gravedad   │
//                             │    ▼                          │
//                             │ LP biquad 20 Hz  (por eje)   │
//                             │    │  elimina ruido mecánico  │
//                             │    ▼                          │
//                             │ writeSample() → SD Card      │
//                             └──────────────────────────────┘
//
//  El ISR del timer NO realiza I2C. Solo marca un flag.
//  Loop() atiende el flag con latencia < 100 µs.
//  El filtro biquad DF2T cuesta ~20 multiplicaciones/muestra
//  por los 3 ejes = < 5 µs @ 240 MHz → sin impacto en SD.
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_wpa2.h>
#include <ESPAsyncWebServer.h>
#include <time.h>
#include "config.h"

// ── GPS ──────────────────────────────────────────────────────
#include <TinyGPSPlus.h>
// ── TFLite Micro ─────────────────────────────────────────────
#include "model.h"
#include "norm_constants.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ── FFT radix-2 de 128 puntos (Cooley-Tukey, implementación propia) ──────────
// Evita dependencia externa de esp-dsp. Suficiente para PSD de ventanas de 128.
static void fft128(float* data) {
    constexpr int N = 128;
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = data[2*i]; data[2*i] = data[2*j]; data[2*j] = tr;
            float ti = data[2*i+1]; data[2*i+1] = data[2*j+1]; data[2*j+1] = ti;
        }
    }
    // Butterfly stages
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float wRe = cosf(ang), wIm = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float re = 1.0f, im = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                int u = i + j, v = i + j + len / 2;
                float uRe = data[2*u],   uIm = data[2*u+1];
                float vRe = data[2*v]*re - data[2*v+1]*im;
                float vIm = data[2*v]*im + data[2*v+1]*re;
                data[2*u]   = uRe + vRe;  data[2*u+1] = uIm + vIm;
                data[2*v]   = uRe - vRe;  data[2*v+1] = uIm - vIm;
                float nRe = re*wRe - im*wIm;
                im = re*wIm + im*wRe;
                re = nRe;
            }
        }
    }
}



#include <U8g2lib.h>   // OLED — instalar "U8g2" de olikraus en Library Manager

// ── Firebase & Firestore (Fase 3) ────────────────────────────────────────────
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <esp_task_wdt.h>

// OLED SSD1306 128×64 — modo I2C sin reiniciar el bus
// U8G2_R0 = sin rotación. El último parámetro u8x8_byte_arduino_hw_i2c
// usa la instancia Wire global que ya inicializamos nosotros.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C g_oled(
    U8G2_R0,
    /* reset= */ U8X8_PIN_NONE
);

// ============================================================
//  REGISTROS ADXL345 (datasheet Rev. G)
// ============================================================
namespace ADXL345_REG {
    constexpr uint8_t DEVID        = 0x00;
    constexpr uint8_t THRESH_TAP   = 0x1D;
    constexpr uint8_t OFSX         = 0x1E;
    constexpr uint8_t OFSY         = 0x1F;
    constexpr uint8_t OFSZ         = 0x20;
    constexpr uint8_t BW_RATE      = 0x2C;
    constexpr uint8_t POWER_CTL    = 0x2D;
    constexpr uint8_t INT_ENABLE   = 0x2E;
    constexpr uint8_t DATA_FORMAT  = 0x31;
    constexpr uint8_t DATAX0       = 0x32;
    constexpr uint8_t FIFO_CTL     = 0x38;
}

// Valores de configuración (Tabla 7 del datasheet)
// ADXL_ODR_200HZ (0x0B) definido en config.h
constexpr uint8_t MEASURE_ON  = 0x08;   // POWER_CTL: Bit D3 = Measure
constexpr uint8_t FULL_RES_2G = 0x08;   // DATA_FORMAT: FULL_RES=1, Range=±2g
// Full-resolution ±2g: sensibilidad = 256 LSB/g → 3.9 mg/LSB

// ============================================================
//  ESTRUCTURAS DE DATOS
// ============================================================

// Cabecera del archivo binario (16 bytes, v3: agrega lat/lon GPS)
struct __attribute__((packed)) FileHeader {
    uint32_t magic;        // BIN_MAGIC
    uint16_t version;      // BIN_VERSION (0x0003)
    uint16_t sample_rate;  // Hz
    float    lat;          // GPS latitud  (0.0 si sin fix)
    float    lon;          // GPS longitud (0.0 si sin fix)
};

// Una muestra (10 bytes, layout idéntico a v1 para compatibilidad)
// v2: ax, ay y az_dynamic almacenan la señal DESPUÉS del filtro BP.
struct __attribute__((packed)) Sample {
    uint32_t timestamp_ms;  // millis() en el momento de captura
    int16_t  ax;            // Aceleración X — paso-banda 0.1–20 Hz (LSB)
    int16_t  ay;            // Aceleración Y — paso-banda 0.1–20 Hz (LSB)
    int16_t  az_dynamic;    // Aceleración Z — paso-banda 0.1–20 Hz (LSB)
};

// ============================================================
//  FILTRO PASO-BANDA — Biquad Butterworth 2do orden (DF2T)
// ============================================================
//
//  Topología: Direct Form II Transpuesta
//  Ecuaciones de diferencias:
//    y[n] = b0·x[n] + w1
//    w1   = b1·x[n] - a1·y[n] + w2
//    w2   = b2·x[n] - a2·y[n]
//
//  Ventajas de DF2T sobre DF1/DF2:
//  - Mínimo redondeo acumulativo en aritmética de 32 bits
//  - Estado interno acotado independientemente de la entrada
//  - Solo 2 variables de estado (vs 4 en DF1)
//
struct Biquad {
    float b0, b1, b2;   // Coeficientes de numerador
    float a1, a2;       // Coeficientes de denominador (normalizados por a0)
    float w1, w2;       // Estado interno — inicializar a 0
};

// Procesa UNA muestra a través del biquad. Inline para mínima latencia.
static inline float biquad_process(Biquad &f, float x) {
    float y = f.b0 * x + f.w1;
    f.w1    = f.b1 * x - f.a1 * y + f.w2;
    f.w2    = f.b2 * x - f.a2 * y;
    return y;
}

// ── Estado del filtro: un biquad HP + un biquad LP por eje ───
// Índices: [0] = eje X,  [1] = eje Y,  [2] = eje Z
//
// Los coeficientes (b0..a2) se cargan desde config.h en bandpass_init().
// El estado interno (w1, w2) se mantiene entre llamadas.
//
static Biquad g_bq_hp[3];   // Etapa 1: High-Pass  0.1 Hz
static Biquad g_bq_lp[3];   // Etapa 2: Low-Pass  20.0 Hz

// Inicializa ambas etapas del filtro con los coeficientes de config.h.
// Debe llamarse UNA sola vez en setup(), después de la calibración.
//
// La pre-carga del estado HP con la primera muestra evita el
// transitorio de convergencia (~10 s si arrancara desde w1=w2=0).
static void bandpass_init(float ax0, float ay0, float az0) {
    const float hp_b0 = BP_HP_B0, hp_b1 = BP_HP_B1, hp_b2 = BP_HP_B2;
    const float hp_a1 = BP_HP_A1, hp_a2 = BP_HP_A2;
    const float lp_b0 = BP_LP_B0, lp_b1 = BP_LP_B1, lp_b2 = BP_LP_B2;
    const float lp_a1 = BP_LP_A1, lp_a2 = BP_LP_A2;

    float init_vals[3] = { ax0, ay0, az0 };

    for (int i = 0; i < 3; i++) {
        // Cargar coeficientes HP (idénticos para los tres ejes)
        g_bq_hp[i] = { hp_b0, hp_b1, hp_b2, hp_a1, hp_a2, 0.0f, 0.0f };

        // Cargar coeficientes LP (idénticos para los tres ejes)
        g_bq_lp[i] = { lp_b0, lp_b1, lp_b2, lp_a1, lp_a2, 0.0f, 0.0f };

        // Pre-carga del estado HP con la señal inicial para reducir transitorio.
        // El HP tiene ganancia DC = 0 → su estado converge a 0 con la entrada DC.
        // Iterar con la misma muestra acerca el estado al punto de operación.
        float x0 = init_vals[i];
        for (int k = 0; k < 8; k++) {
            biquad_process(g_bq_hp[i], x0);
        }
        // Estado LP: ya en 0, no hay transitorio para frecuencias altas
        g_bq_lp[i].w1 = 0.0f;
        g_bq_lp[i].w2 = 0.0f;
    }

    Serial.printf("[BP] Filtro paso-banda listo: HP=%.2f Hz → LP=%.1f Hz "
                  "(Butterworth 2do orden, fs=%d Hz)\n",
                  (double)BP_HP_FREQ_HZ, (double)BP_LP_FREQ_HZ, SAMPLE_RATE_HZ);
}

// Aplica el filtro paso-banda (HP → LP) a los tres ejes en una sola llamada.
// Entrada:  ax_in, ay_in, az_in  — valores en LSB (float, post-bias)
// Salida:   ax_out, ay_out, az_out — señal filtrada, truncada a int16_t
//
// Costo computacional @ 240 MHz:
//   6 biquads × ~10 FPU ops = ~60 FPU ops ≈ 0.25 µs → despreciable vs I2C
static void bandpassFilter(float ax_in, float ay_in, float az_in,
                           int16_t &ax_out, int16_t &ay_out, int16_t &az_out) {
    float inputs[3]  = { ax_in, ay_in, az_in };
    int16_t *outs[3] = { &ax_out, &ay_out, &az_out };

    for (int i = 0; i < 3; i++) {
        float hp = biquad_process(g_bq_hp[i], inputs[i]);   // quita DC
        float bp = biquad_process(g_bq_lp[i], hp);          // quita HF
        *outs[i] = (int16_t)bp;
    }
}

// ============================================================
//  VARIABLES GLOBALES
// ============================================================

// --- Timer --------------------------------------------------
hw_timer_t       *g_timer     = nullptr;
portMUX_TYPE      g_timerMux  = portMUX_INITIALIZER_UNLOCKED;
volatile bool     g_sampleDue = false;

// --- Calibración de offset por software ---------------------
// Estimado durante los primeros AUTOCAL_SAMPLES en setup().
// Se resta de cada lectura cruda ANTES de entrar al filtro BP.
float g_sw_bias[3] = { 0.0f, 0.0f, 0.0f };   // [0]=X, [1]=Y, [2]=Z

// --- SD Card ------------------------------------------------
SPIClass g_sdSPI(VSPI);
File     g_dataFile;
char     g_currentFile[80];
uint32_t g_sampleCount = 0;

// --- Web Server ---------------------------------------------
AsyncWebServer g_server(WEB_SERVER_PORT);

// --- Estadísticas en tiempo real ----------------------------
volatile uint32_t g_totalSamples = 0;
volatile uint32_t g_dropCount    = 0;

// --- GPS Neo 6M ---------------------------------------------
static TinyGPSPlus g_gps;
static float       g_gps_lat   = 0.0f;
static float       g_gps_lon   = 0.0f;
static bool        g_gps_valid = false;

// --- Ventana de inferencia (800 muestras × 3 ejes = 4.8 KB) -
static int16_t  g_win_ax[SEISMIC_WIN_SAMPLES];
static int16_t  g_win_ay[SEISMIC_WIN_SAMPLES];
static int16_t  g_win_az[SEISMIC_WIN_SAMPLES];
static uint32_t g_win_ptr = 0;

// --- STA/LTA trigger (EMA, mismos parámetros que seismic_dataset_builder_v3)
static float    g_sta        = 0.0f;
static float    g_lta        = 1e-6f;   // init > 0 para evitar div/0 en primer ratio
static bool     g_sta_trig   = false;
static int      g_lta_warmup = STA_LTA_WARMUP;
static uint32_t g_sta_hold   = 0;       // hold-off: trigger permanece activo ≥ 1 ventana

// --- TFLite Micro
// tensor_arena en heap para no saturar la BSS (límite dram0_0_seg ~124 KB).
// g_spec=8.6KB, g_fft_buf=1KB, g_hann128=0.5KB en BSS.
static uint8_t*  g_tensor_arena = nullptr;
static float     g_spec[SEISMIC_FREQ_BINS][SEISMIC_TIME_BINS][3];
static float     g_fft_buf[256];
static float     g_hann128[128];
static tflite::MicroErrorReporter                 g_tflite_error_reporter;
// Ops del modelo dual (spec CNN + pga Dense): Conv2D, MaxPool, Reshape,
// Transpose, Mean, FullyConnected, Logistic, Relu + Concatenation para fusión
static tflite::MicroMutableOpResolver<13>         g_tflite_resolver;
static tflite::MicroInterpreter*                  g_tflite_interp = nullptr;

// ── Fase 3: Comunicación ─────────────────────────────────────────────────────
struct UploadReq {
    char   path[80];   // ruta SD completa, ej: /Aceleraciones/accel_....bin
    bool   is_event;   // true → /Eventos/ + escribe en /alertas/ de RTDB
    float  score;      // CNN score (válido solo si is_event)
    float  lat, lon;
    time_t ts;
};

static char              g_station_id[32];
static FirebaseData      g_fbdo;
static FirebaseAuth      g_fbAuth;
static FirebaseConfig    g_fbConfig;
static QueueHandle_t     g_upload_queue  = nullptr;
static SemaphoreHandle_t g_sd_mutex      = nullptr;

#define SD_LOCK()   xSemaphoreTake(g_sd_mutex, portMAX_DELAY)
#define SD_UNLOCK() xSemaphoreGive(g_sd_mutex)


// ============================================================
//  ADXL345 — Funciones de bajo nivel I2C
// ============================================================

static void adxl_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ADXL345_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[ADXL] ⚠ Error escritura reg=0x%02X\n", reg);
    }
}

static uint8_t adxl_read(uint8_t reg) {
    Wire.beginTransmission(ADXL345_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)ADXL345_I2C_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

// Lectura en ráfaga de 6 bytes (X0,X1,Y0,Y1,Z0,Z1) — más eficiente
static bool adxl_readXYZ(int16_t &ax, int16_t &ay, int16_t &az) {
#ifdef SIM_REPLAY_FILE
    // Reproduce un archivo .bin de la SD en lugar de leer el I2C.
    // Formato: 8-byte header (magic+ver+fs+reserved) + muestras de 10 bytes.
    // Cuando el archivo se agota, cae al I2C normal (s_sim_done=true).
    {
        static File     s_sim_file;
        static bool     s_sim_open  = false;
        static bool     s_sim_done  = false;
        static uint32_t s_sim_count = 0;   // muestras reproducidas
        static uint32_t s_total_samples = 0;
        if (!s_sim_done) {
            if (!s_sim_open) {
                // Esperar a que SD esté montada (adxl_readXYZ se llama durante
                // autocal, ANTES de SD.begin() en setup()).
                if (SD.cardType() == CARD_NONE) goto sim_i2c_fallback;
                s_sim_file = SD.open(SIM_REPLAY_FILE, FILE_READ);
                if (s_sim_file) {
                    s_sim_file.seek(8);   // saltar header de 8 bytes
                    s_sim_open = true;
                    s_total_samples = (s_sim_file.size() - 8) / 10;
                    Serial.println();
                    Serial.println("================================================");
                    Serial.printf ("[SIM] Archivo reconocido: " SIM_REPLAY_FILE "\n");
                    Serial.printf ("[SIM] Duracion: %lu s  (%lu muestras @ 200 Hz)\n",
                                   s_total_samples / 200, s_total_samples);
                    Serial.println("================================================");
                    Serial.println();
                } else {
                    s_sim_done = true;
                    Serial.println("[SIM] ERROR: no se encontro " SIM_REPLAY_FILE " en SD");
                }
            }
            if (s_sim_open && s_sim_file.available() >= 10) {
                uint32_t ts;
                s_sim_file.read((uint8_t*)&ts, 4);
                s_sim_file.read((uint8_t*)&ax, 2);
                s_sim_file.read((uint8_t*)&ay, 2);
                s_sim_file.read((uint8_t*)&az, 2);
                s_sim_count++;
                // Progreso cada 10 s (2000 muestras)
                if (s_sim_count % 2000 == 0) {
                    uint32_t elapsed_s = s_sim_count / 200;
                    uint32_t total_s   = s_total_samples / 200;
                    Serial.printf("[SIM] Progreso: %lu/%lu s\n", elapsed_s, total_s);
                }
                return true;
            }
            if (s_sim_open) {
                s_sim_file.close();
                s_sim_open = false;
                Serial.println();
                Serial.println("================================================");
                Serial.printf ("[SIM] Simulacion completada (%lu muestras)\n", s_sim_count);
                Serial.println("[SIM] Volviendo a sensor I2C");
                Serial.println("================================================");
                Serial.println();
            }
            s_sim_done = true;
        }
        // s_sim_done: caer al I2C normal
    }
    sim_i2c_fallback:;   // destino del goto cuando SD aún no está montada
#endif
    Wire.beginTransmission(ADXL345_I2C_ADDR);
    Wire.write(ADXL345_REG::DATAX0);
    if (Wire.endTransmission(false) != 0) return false;

    uint8_t n = Wire.requestFrom((uint8_t)ADXL345_I2C_ADDR, (uint8_t)6);
    if (n < 6) return false;

    ax = (int16_t)(Wire.read() | (Wire.read() << 8));
    ay = (int16_t)(Wire.read() | (Wire.read() << 8));
    az = (int16_t)(Wire.read() | (Wire.read() << 8));
    return true;
}

// ============================================================
//  AUTOCALIBRACIÓN DE OFFSET (software)
// ============================================================
//
//  Captura AUTOCAL_SAMPLES lecturas con el sensor en reposo
//  y calcula el promedio por eje. Ese promedio es el "bias"
//  que se restará de cada muestra en loop() antes de filtrar.
//
//  El bias de X e Y debería ser ~0 g si el sensor está nivelado.
//  El bias de Z incluye la componente DC de 1 g de la gravedad
//  (≈ +256 LSB en full-res ±2g), que el filtro HP también elimina;
//  se resta igualmente por consistencia y para reducir saturación.
//
//  PRECONDICIÓN: El sensor debe estar completamente estático
//  durante los ~2 segundos de la captura.
//
//  NOTA: Esta función realiza el muestreo manualmente (sin timer).
//  Se llama en setup() ANTES de activar el timer de 200 Hz.
//
static void adxl_autocalibrate() {
    Serial.println("[CAL] ════════════════════════════════════════");
    Serial.printf ("[CAL] Autocalibración — %d muestras (%.0f s)\n",
                   AUTOCAL_SAMPLES, AUTOCAL_SAMPLES / (float)SAMPLE_RATE_HZ);
    Serial.println("[CAL] Mantén el sensor COMPLETAMENTE QUIETO...");

    double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
    int16_t x, y, z;
    uint32_t dt_us = 1000000UL / SAMPLE_RATE_HZ;  // 5000 µs @ 200 Hz

    for (int i = 0; i < AUTOCAL_SAMPLES; i++) {
        uint32_t t0 = micros();
        if (adxl_readXYZ(x, y, z)) {
            sumX += (double)x;
            sumY += (double)y;
            sumZ += (double)z;
        }
        // Esperar hasta el siguiente período de muestreo
        while ((micros() - t0) < dt_us) { /* spin */ }

        if ((i + 1) % (AUTOCAL_SAMPLES / 4) == 0) {
            Serial.printf("[CAL] %3d%%...\n", (i + 1) * 100 / AUTOCAL_SAMPLES);
        }
    }

    g_sw_bias[0] = (float)(sumX / AUTOCAL_SAMPLES);
    g_sw_bias[1] = (float)(sumY / AUTOCAL_SAMPLES);
    g_sw_bias[2] = (float)(sumZ / AUTOCAL_SAMPLES);

    Serial.println("[CAL] ────────────────────────────────────────");
    Serial.printf("[CAL] Bias X: %+.4f LSB  (%+.3f mg)\n",
                  g_sw_bias[0], g_sw_bias[0] * 3.9f);
    Serial.printf("[CAL] Bias Y: %+.4f LSB  (%+.3f mg)\n",
                  g_sw_bias[1], g_sw_bias[1] * 3.9f);
    Serial.printf("[CAL] Bias Z: %+.4f LSB  (%+.3f mg, incluye ~256 LSB de 1g)\n",
                  g_sw_bias[2], g_sw_bias[2] * 3.9f);
    Serial.println("[CAL] ✓ Calibración completa");
    Serial.println("[CAL] ════════════════════════════════════════");
}

// ============================================================
//  INICIALIZACIÓN DEL ADXL345
// ============================================================

static bool adxl_init() {
    // 1. Verificar Device ID (debe ser 0xE5 — Tabla 20 del datasheet)
    uint8_t devId = adxl_read(ADXL345_REG::DEVID);
    if (devId != 0xE5) {
        // Nota: v1.0 comparaba incorrectamente con 0xE6 → siempre fallaba en HW real
        Serial.printf("[ADXL] ERROR: DEVID=0x%02X (esperado 0xE5). "
                      "Verifica cableado I2C y dirección (ADXL345_I2C_ADDR).\n", devId);
        return false;
    }
    Serial.printf("[ADXL] ✓ ADXL345 detectado (DEVID=0xE5) en 0x%02X\n",
                  ADXL345_I2C_ADDR);

    // 2. Standby para configurar (recomendado por datasheet §Power Sequencing)
    adxl_write(ADXL345_REG::POWER_CTL, 0x00);
    delay(5);

    // 3. Limpiar registros de offset hardware (se usará bias por software)
    adxl_write(ADXL345_REG::OFSX, 0x00);
    adxl_write(ADXL345_REG::OFSY, 0x00);
    adxl_write(ADXL345_REG::OFSZ, 0x00);

    // 4. BW_RATE: 200 Hz ODR, modo normal (LOW_POWER=0 → menor ruido)
    //    ADXL_ODR_200HZ = 0x0B (definido en config.h)
    //    Bandwidth interna = 100 Hz → aliasing protegido para Nyquist a 100 Hz
    adxl_write(ADXL345_REG::BW_RATE, ADXL_ODR_200HZ);

    // 5. DATA_FORMAT: Full-resolution, ±2g, right-justified (sign-extend MSB)
    //    FULL_RES=1 → 256 LSB/g = 3.9 mg/LSB (máxima sensibilidad)
    adxl_write(ADXL345_REG::DATA_FORMAT, FULL_RES_2G);

    // 6. FIFO: Bypass mode (lectura directa de registros DATA*)
    adxl_write(ADXL345_REG::FIFO_CTL, 0x00);

    // 7. Deshabilitar interrupts (no usados en esta implementación)
    adxl_write(ADXL345_REG::INT_ENABLE, 0x00);

    // 8. Activar medición (Measure bit D3 = 1 en POWER_CTL)
    adxl_write(ADXL345_REG::POWER_CTL, MEASURE_ON);
    delay(12);  // Turn-on time: ≈ 11.1 ms a 200 Hz (datasheet nota 7)

    Serial.printf("[ADXL] Configurado: ODR=200Hz, Full-Res ±2g (3.9mg/LSB), "
                  "BW_interna=100Hz\n");
    return true;
}

// ============================================================
//  TIMER ISR — 200 Hz (período = 5000 µs)
// ============================================================
void IRAM_ATTR onTimerISR() {
    portENTER_CRITICAL_ISR(&g_timerMux);
    g_sampleDue = true;
    portEXIT_CRITICAL_ISR(&g_timerMux);
}

// ============================================================
//  GPS Neo 6M
// ============================================================

static void ensure_sd_dirs() {
    if (!SD.exists(SD_DIR_ACEL))    SD.mkdir(SD_DIR_ACEL);
    if (!SD.exists(SD_DIR_EVENTOS)) SD.mkdir(SD_DIR_EVENTOS);
}

// Declaración adelantada — definición completa cerca de buildFileName()
static bool ensureHourDir(const char* base, char* buf, size_t buf_size);

static bool gps_init() {
    Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    Serial.printf("[GPS] Buscando fix (timeout %d s)...\n", GPS_TIMEOUT_MS / 1000);
    uint32_t deadline = millis() + GPS_TIMEOUT_MS;
    while (millis() < deadline) {
        while (Serial2.available()) g_gps.encode(Serial2.read());
        if (g_gps.location.isValid()) {
            g_gps_lat   = (float)g_gps.location.lat();
            g_gps_lon   = (float)g_gps.location.lng();
            g_gps_valid = true;
            Serial.printf("[GPS] Fix: lat=%.6f  lon=%.6f\n", g_gps_lat, g_gps_lon);
            return true;
        }
        delay(10);
    }
    Serial.println("[GPS] Sin fix — coordenadas: 0.0, 0.0");
    return false;
}

static void gps_update() {
    while (Serial2.available()) g_gps.encode(Serial2.read());
    if (g_gps.location.isValid() && g_gps.location.isUpdated()) {
        g_gps_lat   = (float)g_gps.location.lat();
        g_gps_lon   = (float)g_gps.location.lng();
        g_gps_valid = true;
    }
}

// ============================================================
//  TFLite Micro — Inferencia sísmica
// ============================================================

static void seismic_inference_init() {
    for (int n = 0; n < 128; n++)
        g_hann128[n] = 0.5f * (1.0f - cosf(2.0f * M_PI * n / 127.0f));

    g_tensor_arena = (uint8_t*)malloc(TENSOR_ARENA_SIZE);
    if (!g_tensor_arena) {
        ESP_LOGE("TFLite", "malloc tensor_arena %u B falló — heap libre: %u B",
                 TENSOR_ARENA_SIZE, (unsigned)esp_get_free_heap_size());
        return;
    }

    // Ops del CNN dual (spec Conv2D + pga Dense) + cuantización
    g_tflite_resolver.AddConv2D();
    g_tflite_resolver.AddMaxPool2D();
    g_tflite_resolver.AddReshape();
    g_tflite_resolver.AddTranspose();
    g_tflite_resolver.AddMean();
    g_tflite_resolver.AddFullyConnected();
    g_tflite_resolver.AddLogistic();
    g_tflite_resolver.AddRelu();
    g_tflite_resolver.AddQuantize();
    g_tflite_resolver.AddDequantize();    // INT8→float32 en output del modelo
    g_tflite_resolver.AddShape();
    g_tflite_resolver.AddStridedSlice();
    g_tflite_resolver.AddConcatenation();

    const tflite::Model* mdl = tflite::GetModel(seismic_model_data);
    tflite::MicroAllocator* allocator = tflite::MicroAllocator::Create(
        g_tensor_arena, TENSOR_ARENA_SIZE, &g_tflite_error_reporter);
    static tflite::MicroInterpreter static_interp(
        mdl, g_tflite_resolver, allocator, &g_tflite_error_reporter);
    g_tflite_interp = &static_interp;

    TfLiteStatus st = g_tflite_interp->AllocateTensors();
    if (st != kTfLiteOk) {
        g_tflite_interp = nullptr;   // deshabilita inferencia — run_inference() devuelve 0
    }
    Serial.printf("[TFLite] AllocateTensors: %s  (arena=%d KB)\n",
                  st == kTfLiteOk ? "OK" : "FALLO — inferencia deshabilitada",
                  TENSOR_ARENA_SIZE / 1024);
}

// STFT de un eje: llena g_spec[freq][time][ch] con log10(PSD)
static void _spec_axis(const int16_t* buf, int ch) {
    constexpr float LSB_G = 1.0f / 256.0f;           // 256 LSB/g, ±2g full-res
    constexpr float NORM  = SEISMIC_FS * SEISMIC_HANN_POWER;

    for (int t = 0; t < SEISMIC_TIME_BINS; t++) {
        int start = t * 64;                            // hop = 64 muestras
        for (int n = 0; n < 128; n++) {
            g_fft_buf[2*n]   = buf[start + n] * LSB_G * g_hann128[n];
            g_fft_buf[2*n+1] = 0.0f;
        }
        fft128(g_fft_buf);
        for (int k = 0; k < SEISMIC_FREQ_BINS; k++) {
            float psd = g_fft_buf[2*k]*g_fft_buf[2*k]
                      + g_fft_buf[2*k+1]*g_fft_buf[2*k+1];
            if (k > 0 && k < 64) psd *= 2.0f;         // espectro one-sided
            g_spec[k][t][ch] = log10f(psd / NORM + 1e-12f);
        }
    }
}

// Devuelve score CNN: 0.0 = ruido, 1.0 = sismo
static float run_inference() {
    if (!g_tflite_interp) return 0.0f;

    // ── PGA: máximo absoluto por eje en la ventana filtrada (0.1–20 Hz) ──
    constexpr float LSB_G = 1.0f / 256.0f;
    float pga[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < SEISMIC_WIN_SAMPLES; i++) {
        float ax = fabsf(g_win_ax[i] * LSB_G);
        float ay = fabsf(g_win_ay[i] * LSB_G);
        float az = fabsf(g_win_az[i] * LSB_G);
        if (ax > pga[0]) pga[0] = ax;
        if (ay > pga[1]) pga[1] = ay;
        if (az > pga[2]) pga[2] = az;
    }

    _spec_axis(g_win_ax, 0);   // canal 0: EW  (ax)
    _spec_axis(g_win_az, 1);   // canal 1: VER (az)
    _spec_axis(g_win_ay, 2);   // canal 2: NS  (ay)

    // TFLite pone PGA como input(0) y espectrograma como input(1)
    // (orden determinado por get_input_details() tras model.export())
    float* inp_pga = g_tflite_interp->input(0)->data.f;
    for (int c = 0; c < 3; c++)
        inp_pga[c] = (pga[c] - SEISMIC_PGA_MEAN[c]) / SEISMIC_PGA_STD[c];

    float* inp_spec = g_tflite_interp->input(1)->data.f;
    int idx = 0;
    for (int f = 0; f < SEISMIC_FREQ_BINS; f++)
        for (int t = 0; t < SEISMIC_TIME_BINS; t++)
            for (int c = 0; c < 3; c++)
                inp_spec[idx++] = (g_spec[f][t][c] - SEISMIC_NORM_MEAN) / SEISMIC_NORM_STD;

    g_tflite_interp->Invoke();
    return g_tflite_interp->output(0)->data.f[0];
}

// ============================================================
//  FASE 3: COMUNICACIÓN (station, Firebase, upload_task)
// ============================================================

static void station_init() {
    if (strlen(STATION_NAME) > 0) {
        strncpy(g_station_id, STATION_NAME, sizeof(g_station_id) - 1);
    } else {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(g_station_id, sizeof(g_station_id),
                 "PAN_%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    Serial.printf("[Station] ID: %s\n", g_station_id);
}

static void firebase_init() {
    g_fbConfig.api_key                   = FIREBASE_API_KEY;
    g_fbConfig.database_url              = RTDB_URL;
    g_fbAuth.user.email                  = FIREBASE_USER_EMAIL;
    g_fbAuth.user.password               = FIREBASE_USER_PASS;
    g_fbConfig.token_status_callback     = tokenStatusCallback;

    Firebase.begin(&g_fbConfig, &g_fbAuth);
    Firebase.reconnectNetwork(true);
    // RX=16384 (default Firebase_ESP_Client): certificate chain + fragmentación SSL
    // TX=512 es suficiente para ClientHello mbedTLS (< 300 bytes)
    g_fbdo.setBSSLBufferSize(16384, 512);
    Serial.println("[Firebase] Inicializado");
}

// ── RTDB: alerta sísmica (JSON con IP y URL de descarga) ─────────────────────
static void firebase_alert_rtdb(const UploadReq& req) {
    struct tm ti; localtime_r(&req.ts, &ti);
    char key[120];
    snprintf(key, sizeof(key),
             "/alertas/%s/%04d/%04d-%02d/%04d-%02d-%02d/%02d/%lld",
             g_station_id,
             ti.tm_year + 1900,
             ti.tm_year + 1900, ti.tm_mon + 1,
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, (long long)req.ts);
    char ip[20];
    strncpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = '\0';
    char url[128];
    snprintf(url, sizeof(url), "http://%s/download?f=%s", ip, req.path);
    FirebaseJson j;
    j.set("estacion",     g_station_id);
    j.set("timestamp",    (int)req.ts);
    j.set("lat",          req.lat);
    j.set("lon",          req.lon);
    j.set("score",        req.score);
    j.set("ip",           ip);
    j.set("url_descarga", url);
    bool ok = Firebase.RTDB.setJSON(&g_fbdo, key, &j);
    Serial.printf("[RTDB] Alerta %s: %s\n", key,
                  ok ? "OK" : g_fbdo.errorReason().c_str());
}

static void rtdb_log_file(const UploadReq& req) {
    char ip[20];
    strncpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = '\0';
    char url[128];
    snprintf(url, sizeof(url), "http://%s/download?f=%s", ip, req.path);

    // Extraer nombre de archivo sin extensión como hoja del árbol RTDB
    // req.path = "/Aceleraciones/2026/2026-05/2026-05-15/14/accel_20260515_140000.bin"
    const char* slash = strrchr(req.path, '/');
    char leaf[48];
    strncpy(leaf, slash ? slash + 1 : req.path, sizeof(leaf) - 1);
    leaf[sizeof(leaf) - 1] = '\0';
    char* dot = strrchr(leaf, '.');
    if (dot) *dot = '\0';   // quitar ".bin"

    struct tm ti; localtime_r(&req.ts, &ti);
    const char* col = req.is_event ? "logs_eventos" : "logs_acel";
    char key[140];
    snprintf(key, sizeof(key),
             "/estaciones/%s/%s/%04d/%04d-%02d/%04d-%02d-%02d/%02d/%s",
             g_station_id, col,
             ti.tm_year + 1900,
             ti.tm_year + 1900, ti.tm_mon + 1,
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, leaf);
    FirebaseJson j;
    j.set("archivo",   req.path);
    j.set("url",       url);
    j.set("lat",       req.lat);
    j.set("lon",       req.lon);
    j.set("timestamp", (int)req.ts);
    if (req.is_event) j.set("score", req.score);
    bool ok = Firebase.RTDB.setJSON(&g_fbdo, key, &j);
    Serial.printf("[RTDB] %s: %s\n", key, ok ? "OK" : g_fbdo.errorReason().c_str());
}

// Task en Core 0: RTDB para alertas + metadatos de archivos.
// Core 1 sigue muestreando a 200 Hz sin interferencia.
static void upload_task(void* param) {
    // Sacar IDLE0 del watchdog: RTDB/SSL puede bloquear Core 0 durante auth.
    // Core 1 (200 Hz) sigue vigilado por IDLE1.
    {
        TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCPU(0);
        if (idle0) esp_task_wdt_delete(idle0);
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
    firebase_init();

    while (!Firebase.ready()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Reportar conexión con IP en RTDB
    {
        char ip[20];
        strncpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip) - 1);
        ip[sizeof(ip) - 1] = '\0';
        char key[80];
        snprintf(key, sizeof(key), "/estaciones/%s/status", g_station_id);
        FirebaseJson j;
        j.set("estado",    "Conectada");
        j.set("ip",        ip);
        j.set("lat",       g_gps_lat);
        j.set("lon",       g_gps_lon);
        time_t now_t; time(&now_t);
        j.set("timestamp", (int)now_t);
        bool ok = Firebase.RTDB.setJSON(&g_fbdo, key, &j);
        Serial.printf("[RTDB] Estacion Conectada — IP: %s: %s\n",
                      ip, ok ? "OK" : g_fbdo.errorReason().c_str());
    }

    UploadReq req;
    uint32_t fb_retry_ms = 1000;
    for (;;) {
        if (!Firebase.ready()) {
            vTaskDelay(pdMS_TO_TICKS(fb_retry_ms));
            fb_retry_ms *= 2;
            if (fb_retry_ms > 30000) fb_retry_ms = 30000;
            continue;
        }
        fb_retry_ms = 1000;
        if (xQueueReceive(g_upload_queue, &req, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (req.is_event) firebase_alert_rtdb(req);
            rtdb_log_file(req);
        }
    }
}

static void saveEventFile(float score) {
    // ensureHourDir toma SD_LOCK internamente; debe llamarse ANTES del SD_LOCK externo.
    char day_dir[48];
    ensureHourDir(SD_DIR_EVENTOS, day_dir, sizeof(day_dir));
    time_t now; time(&now); struct tm tm;
    localtime_r(&now, &tm);
    char path[80];
    snprintf(path, sizeof(path),
             "%s/evento_%04d%02d%02d_%02d%02d%02d_s%02d.bin",
             day_dir,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)(score * 100));

    SD_LOCK();
    File ef = SD.open(path, FILE_WRITE);
    if (!ef) {
        SD_UNLOCK();
        Serial.println("[EVENTO] Error abriendo archivo de evento");
        return;
    }

    FileHeader hdr = {BIN_MAGIC, BIN_VERSION, SAMPLE_RATE_HZ, g_gps_lat, g_gps_lon};
    ef.write((uint8_t*)&hdr, sizeof(hdr));

    for (uint32_t i = 0; i < SEISMIC_WIN_SAMPLES; i++) {
        Sample s;
        s.timestamp_ms = millis() - (uint32_t)(SEISMIC_WIN_SAMPLES - i) * 5;
        s.ax           = g_win_ax[i];
        s.ay           = g_win_ay[i];
        s.az_dynamic   = g_win_az[i];
        ef.write((uint8_t*)&s, sizeof(s));
    }
    ef.flush(); ef.close();
    SD_UNLOCK();

    if (g_upload_queue) {
        UploadReq req;
        strncpy(req.path, path, sizeof(req.path) - 1);
        req.path[sizeof(req.path) - 1] = '\0';
        req.is_event = true;
        req.score    = score;
        req.lat      = g_gps_lat;
        req.lon      = g_gps_lon;
        time(&req.ts);
        xQueueSend(g_upload_queue, &req, 0);
    }
    Serial.printf("[SISMO] Evento → %s  score=%.3f  GPS=%.5f,%.5f\n",
                  path, score, g_gps_lat, g_gps_lon);
}

// ============================================================
//  GESTIÓN DE ARCHIVOS SD
// ============================================================

// Crea base/YYYY/YYYY-MM/YYYY-MM-DD/HH si no existe y escribe la ruta en buf.
// Ejemplo: base="/Aceleraciones" → buf="/Aceleraciones/2026/2026-05/2026-05-15/14"
// Debe llamarse ANTES de tomar SD_LOCK en el llamador (toma/libera el lock internamente).
static bool ensureHourDir(const char* base, char* buf, size_t buf_size) {
    time_t now; time(&now);
    struct tm ti; localtime_r(&now, &ti);
    char year_dir[32], month_dir[40], day_dir[48];
    snprintf(year_dir,  sizeof(year_dir),  "%s/%04d",             base,      ti.tm_year + 1900);
    snprintf(month_dir, sizeof(month_dir), "%s/%04d-%02d",        year_dir,  ti.tm_year + 1900, ti.tm_mon + 1);
    snprintf(day_dir,   sizeof(day_dir),   "%s/%04d-%02d-%02d",   month_dir, ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
    snprintf(buf,       buf_size,          "%s/%02d",              day_dir,   ti.tm_hour);
    SD_LOCK();
    if (!SD.exists(year_dir))  SD.mkdir(year_dir);
    if (!SD.exists(month_dir)) SD.mkdir(month_dir);
    if (!SD.exists(day_dir))   SD.mkdir(day_dir);
    bool ok = SD.exists(buf) || SD.mkdir(buf);
    SD_UNLOCK();
    return ok;
}

static void buildFileName(char *buf, size_t len) {
    char day_dir[48];
    ensureHourDir(SD_DIR_ACEL, day_dir, sizeof(day_dir));
    time_t    now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    snprintf(buf, len, "%s/accel_%04d%02d%02d_%02d%02d%02d.bin",
             day_dir,
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
}

static bool openNewFile() {
    // buildFileName llama a ensureHourDir que toma SD_LOCK internamente.
    // Debe ejecutarse ANTES del SD_LOCK externo para evitar mutex anidado.
    char newFile[80];
    buildFileName(newFile, sizeof(newFile));

    SD_LOCK();
    if (g_dataFile) {
        if (g_upload_queue) {
            UploadReq req;
            strncpy(req.path, g_currentFile, sizeof(req.path) - 1);
            req.path[sizeof(req.path) - 1] = '\0';
            req.is_event = false;
            req.score    = 0.0f;
            req.lat      = g_gps_lat;
            req.lon      = g_gps_lon;
            time(&req.ts);
            xQueueSend(g_upload_queue, &req, 0);
        }
        g_dataFile.flush();
        g_dataFile.close();
    }

    strncpy(g_currentFile, newFile, sizeof(g_currentFile) - 1);
    g_currentFile[sizeof(g_currentFile) - 1] = '\0';
    g_dataFile = SD.open(g_currentFile, FILE_WRITE);

    if (!g_dataFile) {
        SD_UNLOCK();
        Serial.printf("[SD] ⚠ No se pudo abrir: %s\n", g_currentFile);
        return false;
    }

    FileHeader hdr = { BIN_MAGIC, BIN_VERSION, (uint16_t)SAMPLE_RATE_HZ, g_gps_lat, g_gps_lon };
    g_dataFile.write((uint8_t*)&hdr, sizeof(hdr));
    g_sampleCount = 0;
    SD_UNLOCK();

    Serial.printf("[SD] ✓ Nuevo archivo: %s\n", g_currentFile);
    return true;
}

static void writeSample(const Sample &s) {
    if (!g_dataFile) return;

    SD_LOCK();
    g_dataFile.write((uint8_t*)&s, sizeof(s));
    g_sampleCount++;
    g_totalSamples++;

    if (g_sampleCount % WRITE_FLUSH_EVERY == 0) {
        g_dataFile.flush();
    }
    bool rotate = (g_sampleCount >= SAMPLES_PER_FILE);
    SD_UNLOCK();

    if (rotate) {
        Serial.printf("[SD] Archivo completo: %s (%lu muestras, %.1f KB)\n",
                      g_currentFile, g_sampleCount,
                      (sizeof(FileHeader) + g_sampleCount * sizeof(Sample)) / 1024.0f);
        openNewFile();  // openNewFile() toma su propio SD_LOCK
    }
}

// ============================================================
//  WIFI WPA2-ENTERPRISE (PEAP / MSCHAPv2)
// ============================================================
static void connectWiFiEnterprise() {
    Serial.printf("[WiFi] Conectando a SSID: %s\n", WIFI_SSID);

    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_STA);

    /*esp_wifi_sta_wpa2_ent_set_identity(
        (uint8_t*)WIFI_IDENTITY, strlen(WIFI_IDENTITY));
    esp_wifi_sta_wpa2_ent_set_username(
        (uint8_t*)WIFI_USERNAME, strlen(WIFI_USERNAME));
    esp_wifi_sta_wpa2_ent_set_password(
        (uint8_t*)WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    esp_wifi_sta_wpa2_ent_enable();
    WiFi.begin(WIFI_SSID);*/

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.print("[WiFi] Esperando conexión");
    uint8_t retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 40) {
        delay(500);
        Serial.print(".");
        retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] ✓ Conectado! IP: %s\n",
                      WiFi.localIP().toString().c_str());
        configTime(GMT_OFFSET_SEC, DAYLIGHT_SEC, NTP_SERVER, NTP_SERVER2);
        Serial.print("[NTP] Sincronizando");
        struct tm ti;
        uint8_t ntpRetry = 0;
        while (!getLocalTime(&ti, 1000) && ntpRetry++ < 10) {
            Serial.print(".");
        }
        Serial.printf("\n[NTP] ✓ Hora: %04d-%02d-%02d %02d:%02d:%02d\n",
                      ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday,
                      ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        Serial.println("\n[WiFi] ✗ Sin conexión. Nombres de archivo usarán millis().");
    }
}

// ============================================================
//  WEB SERVER
// ============================================================

static String buildIndexHTML() {
    uint64_t totalMB = SD.totalBytes() / (1024ULL * 1024ULL);
    uint64_t usedMB  = SD.usedBytes()  / (1024ULL * 1024ULL);
    uint32_t freePct = totalMB > 0 ? (uint32_t)(100 - (usedMB * 100 / totalMB)) : 0;

    String html = R"rawhtml(<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>DataLogger ADXL345</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Exo+2:wght@300;600&display=swap');
    :root {
      --bg:    #060a0f;
      --panel: #0d1520;
      --glow:  #00ffe0;
      --dim:   #007a6e;
      --warn:  #ff6b35;
      --text:  #cde8f0;
      --bord:  #1a3040;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: var(--bg);
      color: var(--text);
      font-family: 'Exo 2', sans-serif;
      font-weight: 300;
      min-height: 100vh;
    }
    body::before {
      content: '';
      position: fixed; inset: 0;
      background: repeating-linear-gradient(
        to bottom,
        transparent 0px, transparent 2px,
        rgba(0,255,224,0.015) 2px, rgba(0,255,224,0.015) 4px
      );
      pointer-events: none; z-index: 9999;
    }
    header {
      background: linear-gradient(135deg, #060f1a 0%, #0a1e2e 100%);
      border-bottom: 1px solid var(--bord);
      padding: 20px 32px;
      display: flex; align-items: center; gap: 20px;
    }
    .logo {
      font-family: 'Share Tech Mono', monospace;
      font-size: 1.4rem;
      color: var(--glow);
      text-shadow: 0 0 12px rgba(0,255,224,0.6);
      letter-spacing: 2px;
    }
    .logo span { color: var(--warn); }
    .badge {
      font-family: 'Share Tech Mono', monospace;
      font-size: 0.7rem;
      background: rgba(0,255,224,0.08);
      border: 1px solid var(--dim);
      color: var(--dim);
      padding: 3px 10px;
      border-radius: 3px;
      letter-spacing: 1px;
    }
    .stats-bar {
      display: flex; gap: 24px; flex-wrap: wrap;
      padding: 14px 32px;
      background: var(--panel);
      border-bottom: 1px solid var(--bord);
    }
    .stat { display: flex; flex-direction: column; gap: 2px; }
    .stat-label {
      font-family: 'Share Tech Mono', monospace;
      font-size: 0.65rem; color: var(--dim);
      letter-spacing: 1px; text-transform: uppercase;
    }
    .stat-value {
      font-family: 'Share Tech Mono', monospace;
      font-size: 1rem; color: var(--glow);
    }
    .stat-value.warn { color: var(--warn); }
    main { padding: 28px 32px; }
    h2 {
      font-family: 'Share Tech Mono', monospace;
      font-size: 0.8rem;
      color: var(--dim);
      letter-spacing: 3px;
      text-transform: uppercase;
      margin-bottom: 16px;
    }
    .file-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
      gap: 12px;
    }
    .file-card {
      background: var(--panel);
      border: 1px solid var(--bord);
      border-radius: 4px;
      padding: 16px;
      display: flex; flex-direction: column; gap: 8px;
      transition: border-color 0.2s, box-shadow 0.2s;
    }
    .file-card:hover {
      border-color: var(--dim);
      box-shadow: 0 0 20px rgba(0,255,224,0.05);
    }
    .file-name {
      font-family: 'Share Tech Mono', monospace;
      font-size: 0.78rem;
      color: var(--text);
      word-break: break-all;
    }
    .file-meta {
      font-family: 'Share Tech Mono', monospace;
      font-size: 0.68rem;
      color: var(--dim);
    }
    .file-meta b { color: #5599aa; }
    .dl-btn {
      display: inline-block;
      margin-top: 6px;
      padding: 7px 18px;
      background: rgba(0,255,224,0.06);
      border: 1px solid var(--dim);
      color: var(--glow);
      font-family: 'Share Tech Mono', monospace;
      font-size: 0.75rem;
      letter-spacing: 1px;
      text-decoration: none;
      border-radius: 3px;
      transition: background 0.15s, box-shadow 0.15s;
    }
    .dl-btn:hover {
      background: rgba(0,255,224,0.12);
      box-shadow: 0 0 10px rgba(0,255,224,0.15);
    }
    .empty {
      font-family: 'Share Tech Mono', monospace;
      color: var(--dim);
      font-size: 0.85rem;
      padding: 32px;
      text-align: center;
      border: 1px dashed var(--bord);
      border-radius: 4px;
    }
    .progress-bar {
      height: 3px;
      background: var(--bord);
      border-radius: 2px;
      overflow: hidden;
      width: 120px;
    }
    .progress-fill {
      height: 100%;
      background: linear-gradient(90deg, var(--dim), var(--glow));
      border-radius: 2px;
    }
    footer {
      padding: 16px 32px;
      font-family: 'Share Tech Mono', monospace;
      font-size: 0.65rem;
      color: #1e3a4a;
      border-top: 1px solid var(--bord);
      margin-top: 32px;
    }
    /* ── Árbol de directorios ─────────────────────────────── */
    .tree-section { margin-bottom: 32px; }
    details { margin: 4px 0; }
    summary.ts {
      font-family: 'Share Tech Mono', monospace;
      font-size: 0.82rem;
      color: var(--text);
      cursor: pointer;
      padding: 7px 12px;
      list-style: none;
      border: 1px solid var(--bord);
      border-radius: 3px;
      background: var(--panel);
      display: flex; align-items: center; gap: 8px;
      user-select: none;
    }
    summary.ts:hover { border-color: var(--dim); color: var(--glow); }
    summary.ts::marker, summary.ts::-webkit-details-marker { display: none; }
    summary.ts::before { content: "▶"; font-size: 0.6rem; color: var(--dim); min-width: 10px; }
    details[open] > summary.ts::before { content: "▼"; }
    .ts-icon { color: var(--warn); }
    .tree-ch { padding-left: 18px; border-left: 1px solid var(--bord); margin: 3px 0 3px 8px; }
  </style>
</head>
<body>
  <header>
    <div class="logo">DATA<span>LOG</span> &bull; ADXL345</div>
    <div class="badge">ESP32 &bull; 200Hz &bull; I2C</div>
    <div class="badge">TTGO T3 V1.6.1 &bull; BP 0.1-20Hz</div>
  </header>
  <div class="stats-bar">
)rawhtml";

    html += "<div class='stat'><div class='stat-label'>Muestras Totales</div>";
    html += "<div class='stat-value'>" + String(g_totalSamples) + "</div></div>";

    html += "<div class='stat'><div class='stat-label'>Archivo Actual</div>";
    html += "<div class='stat-value'>" + String(g_sampleCount) + " / " + String(SAMPLES_PER_FILE) + "</div></div>";

    html += "<div class='stat'><div class='stat-label'>SD Libre</div>";
    html += "<div class='stat-value " + String(freePct < 10 ? "warn" : "") + "'>";
    html += String(totalMB - usedMB) + " MB (" + String(freePct) + "%)</div>";
    html += "<div class='progress-bar'><div class='progress-fill' style='width:" + String(100 - freePct) + "%'></div></div></div>";

    html += "<div class='stat'><div class='stat-label'>Drops</div>";
    html += "<div class='stat-value " + String(g_dropCount > 0 ? "warn" : "") + "'>";
    html += String(g_dropCount) + "</div></div>";

    html += "<div class='stat'><div class='stat-label'>IP</div>";
    html += "<div class='stat-value'>" + WiFi.localIP().toString() + "</div></div>";

    html += R"rawhtml(
  </div>
  <main>
)rawhtml";

    const char* topDirs[]  = { SD_DIR_ACEL,       SD_DIR_EVENTOS };
    const char* topLabels[] = { "Aceleraciones",   "Eventos" };

    SD_LOCK();
    for (int d = 0; d < 2; d++) {
        html += "<div class='tree-section'><h2>&#x25A0; ";
        html += topLabels[d];
        html += "</h2>";

        File topDir = SD.open(topDirs[d]);
        bool hasTop = false;
        if (topDir) {
            // Nivel 1: YYYY
            File yearDir = topDir.openNextFile();
            while (yearDir) {
                if (yearDir.isDirectory()) {
                    hasTop = true;
                    String yname = String(yearDir.name());
                    yname = yname.substring(yname.lastIndexOf('/') + 1);
                    html += "<details open><summary class='ts'><span class='ts-icon'>&#x1F4C1;</span>" + yname + "</summary><div class='tree-ch'>";

                    // Nivel 2: YYYY-MM
                    File monthDir = yearDir.openNextFile();
                    while (monthDir) {
                        if (monthDir.isDirectory()) {
                            String mname = String(monthDir.name());
                            mname = mname.substring(mname.lastIndexOf('/') + 1);
                            html += "<details open><summary class='ts'><span class='ts-icon'>&#x1F4C2;</span>" + mname + "</summary><div class='tree-ch'>";

                            // Nivel 3: YYYY-MM-DD
                            File dayDir = monthDir.openNextFile();
                            while (dayDir) {
                                if (dayDir.isDirectory()) {
                                    String dname = String(dayDir.name());
                                    dname = dname.substring(dname.lastIndexOf('/') + 1);
                                    html += "<details><summary class='ts'><span class='ts-icon'>&#x1F4C5;</span>" + dname + "</summary><div class='tree-ch'>";

                                    // Nivel 4: HH
                                    File hourDir = dayDir.openNextFile();
                                    while (hourDir) {
                                        if (hourDir.isDirectory()) {
                                            String hname = String(hourDir.name());
                                            hname = hname.substring(hname.lastIndexOf('/') + 1);
                                            html += "<details><summary class='ts'><span class='ts-icon'>&#x1F552;</span>" + hname + ":00h</summary><div class='tree-ch'><div class='file-grid'>";

                                            // Nivel 5: archivos .bin
                                            File binFile = hourDir.openNextFile();
                                            while (binFile) {
                                                String bpath = String(binFile.name());
                                                if (!binFile.isDirectory() && bpath.endsWith(".bin")) {
                                                    uint32_t fsz   = binFile.size();
                                                    uint32_t nSamp = fsz > sizeof(FileHeader)
                                                                     ? (fsz - sizeof(FileHeader)) / sizeof(Sample)
                                                                     : 0;
                                                    float    dur   = nSamp / (float)SAMPLE_RATE_HZ;
                                                    String   fname = bpath.substring(bpath.lastIndexOf('/') + 1);
                                                    html += "<div class='file-card'>";
                                                    html += "<div class='file-name'>&#x1F4BE; " + fname + "</div>";
                                                    html += "<div class='file-meta'><b>" + String(fsz / 1024) + " KB</b> &bull; ";
                                                    html += "<b>" + String(nSamp) + "</b> muestras &bull; ";
                                                    html += "<b>" + String((int)dur) + "s</b></div>";
                                                    html += "<a class='dl-btn' href='/download?f=" + bpath + "'>&#x2B07; Descargar</a>";
                                                    html += "</div>";
                                                }
                                                binFile.close();
                                                binFile = hourDir.openNextFile();
                                            }
                                            html += "</div></div></details>";  // file-grid / tree-ch / details(hour)
                                        }
                                        hourDir.close();
                                        hourDir = dayDir.openNextFile();
                                    }
                                    html += "</div></details>";  // tree-ch / details(day)
                                }
                                dayDir.close();
                                dayDir = monthDir.openNextFile();
                            }
                            html += "</div></details>";  // tree-ch / details(month)
                        }
                        monthDir.close();
                        monthDir = yearDir.openNextFile();
                    }
                    html += "</div></details>";  // tree-ch / details(year)
                }
                yearDir.close();
                yearDir = topDir.openNextFile();
            }
            topDir.close();
        }
        if (!hasTop) {
            html += "<div class='empty'>Sin archivos en " + String(topLabels[d]) + ".</div>";
        }
        html += "</div>";  // tree-section
    }
    SD_UNLOCK();

    html += R"rawhtml(
  </main>
  <footer>
    DataLogger ADXL345 v2.0 &bull; TTGO T3 V1.6.1 &bull; ODR=200Hz &bull;
    Filtro BP Butterworth 2do orden 0.1–20 Hz &bull;
    Formato v2: MAGIC(4B)+VER(2B)+ODR(2B)+[ts(4B)+ax_bp(2B)+ay_bp(2B)+az_bp(2B)]×N
    &bull; <a href="/" style="color:#1e5a6a">Actualizar</a>
  </footer>
</body>
</html>)rawhtml";

    return html;
}

static void setupWebServer() {
    g_server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html", buildIndexHTML());
    });

    g_server.on("/download", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("f")) {
            req->send(400, "text/plain", "Parametro 'f' requerido");
            return;
        }
        String filename = req->getParam("f")->value();
        if (filename.indexOf("..") >= 0) {
            req->send(403, "text/plain", "Acceso denegado");
            return;
        }
        if (!filename.startsWith("/")) filename = "/" + filename;
        if (!SD.exists(filename)) {
            req->send(404, "text/plain", "Archivo no encontrado: " + filename);
            return;
        }
        AsyncWebServerResponse *resp = req->beginResponse(SD, filename,
            "application/octet-stream", true);
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    });

    g_server.on("/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        char json[320];
        snprintf(json, sizeof(json),
            "{\"total_samples\":%lu,\"file_samples\":%lu,"
            "\"drops\":%lu,\"file\":\"%s\","
            "\"sd_free_mb\":%llu,\"uptime_s\":%lu,"
            "\"bias_x_mg\":%.2f,\"bias_y_mg\":%.2f,\"bias_z_mg\":%.2f,"
            "\"filter\":\"BP_Butterworth_2ord_%.1fHz-%.1fHz\"}",
            g_totalSamples, g_sampleCount, g_dropCount,
            g_currentFile,
            (SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL),
            millis() / 1000UL,
            g_sw_bias[0] * 3.9f,
            g_sw_bias[1] * 3.9f,
            g_sw_bias[2] * 3.9f,
            (double)BP_HP_FREQ_HZ, (double)BP_LP_FREQ_HZ);
        req->send(200, "application/json", json);
    });

    g_server.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "No encontrado");
    });

    g_server.begin();
    Serial.printf("[Web] ✓ Servidor en http://%s/\n",
                  WiFi.localIP().toString().c_str());
}


// ============================================================
//  OLED — Actualización de pantalla
// ============================================================
//  Llamar desde loop() con baja frecuencia (cada 1-2 s).
//  La transferencia I2C del frame completo toma ~25 ms a 400 kHz
//  (128×64 bits / 400000 bps ≈ 20 ms + overhead).
//  Por eso se actualiza con un intervalo, no en cada muestra.
//  El bus I2C es bloqueante pero compartido correctamente —
//  el ADXL no pierde muestras porque la ISR del timer solo
//  levanta el flag g_sampleDue; la lectura real ocurre en loop()
//  después de que oled_update() haya terminado su transferencia.
// ============================================================
static void oled_update() {
    g_oled.clearBuffer();

    // ── Logos (arriba, lado a lado) ─────────────────────────
    g_oled.drawXBMP(0, 0, 60, 30, logo);   // izquierda
    g_oled.drawXBMP(65, 0, 60, 30, logo_gicm_negro);   // izquierda


    // ── Texto central ───────────────────────────────────────
    g_oled.setFont(u8g2_font_6x10_tf);

    // Centrado aproximado
    g_oled.drawStr(25, 40, "PANdemaiz Quake");

    // ── URL ─────────────────────────────────────────────────
    g_oled.setFont(u8g2_font_5x8_tf); // más pequeña
    g_oled.drawStr(10, 52,  WiFi.localIP().toString().c_str());

    // ── Línea separadora ────────────────────────────────────
    g_oled.drawHLine(0, 54, 128);

    // ── Estado (abajo) ─────────────────────────────────────
    char buf[32];
    snprintf(buf, sizeof(buf), "S:%lu D:%lu",
             g_totalSamples, g_dropCount);

    g_oled.drawStr(0, 63, buf);

    // ── Indicador SD ───────────────────────────────────────
    if (g_dataFile) {
        g_oled.drawStr(100, 63, "SD");
    }

    g_oled.sendBuffer();
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("\n╔══════════════════════════════════════════╗");
    Serial.println("║  DataLogger ADXL345 v2.0 — TTGO T3 V1.6.1 ║");
    Serial.println("║  Instrumento Sismológico  ODR=200Hz        ║");
    Serial.println("╚══════════════════════════════════════════════╝");

    // ── I2C: SDA=21 SCL=22 @400kHz ───────────────────────
    // Recuperación del bus antes de Wire.begin(): si el ESP32 se resetó a mitad
    // de una transacción I2C, el slave puede retener SDA en LOW. El protocolo
    // estándar de recuperación es enviar 9 pulsos en SCL hasta que SDA quede libre,
    // seguido de una condición STOP. Así el ADXL345 no devuelve 0xFF en el boot.
    {
        pinMode(PIN_I2C_SDA, INPUT_PULLUP);
        pinMode(PIN_I2C_SCL, OUTPUT);
        for (int i = 0; i < 9; i++) {
            digitalWrite(PIN_I2C_SCL, LOW);  delayMicroseconds(5);
            digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
            if (digitalRead(PIN_I2C_SDA)) break;  // SDA libre → terminar
        }
        // Condición STOP: SDA sube mientras SCL está alto
        pinMode(PIN_I2C_SDA, OUTPUT);
        digitalWrite(PIN_I2C_SDA, LOW);  delayMicroseconds(5);
        digitalWrite(PIN_I2C_SCL, HIGH); delayMicroseconds(5);
        digitalWrite(PIN_I2C_SDA, HIGH); delayMicroseconds(5);
        pinMode(PIN_I2C_SCL, INPUT_PULLUP);
        pinMode(PIN_I2C_SDA, INPUT_PULLUP);
        delayMicroseconds(20);
    }
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    Serial.printf("[I2C] Bus en SDA=%d SCL=%d @400kHz\n", PIN_I2C_SDA, PIN_I2C_SCL);

// ── OLED — inicializar DESPUÉS de Wire.begin(), ANTES de ADXL ──
// setI2CAddress() por si la dirección no es la default de U8g2
g_oled.setI2CAddress(OLED_I2C_ADDR * 2); // U8g2 espera la dirección desplazada 1 bit
g_oled.begin();
g_oled.setFont(u8g2_font_6x10_tf);
// Imagen 1
g_oled.clearBuffer();
g_oled.drawXBMP(0, 0, 100, 40, escudo); // ajusta tamaño
g_oled.sendBuffer();
delay(3000);

// Imagen 2
g_oled.clearBuffer();
g_oled.drawXBMP(0, 0, 100, 40, logo_gicm); // segundo logo
g_oled.sendBuffer();
delay(3000);
g_oled.clearBuffer();
g_oled.sendBuffer();

delay(2000); // 3 segundos (puedes cambiarlo)

Serial.println("[OLED] ✓ Pantalla inicializada");

    // ── ADXL345 — init y verificación ────────────────────
    if (!adxl_init()) {
        Serial.println("HALT: ADXL345 no encontrado. Revisa cableado.");
        for (;;) { delay(1000); }
    }

    // ── Autocalibración de offset (2 segundos en reposo) ─
    // Calcula g_sw_bias[0..2] que se restan en cada muestra
    adxl_autocalibrate();

    // ── Inicializar estado del filtro paso-banda ──────────
    // Pre-cargar con primera muestra post-calibración para
    // evitar el transitorio del HP de 0.1 Hz (≈ 10 s desde 0)
    {
        int16_t x0, y0, z0;
        adxl_readXYZ(x0, y0, z0);
        bandpass_init(
            (float)x0 - g_sw_bias[0],
            (float)y0 - g_sw_bias[1],
            (float)z0 - g_sw_bias[2]
        );
    }

    // ── SD Card ───────────────────────────────────────────
    g_sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, g_sdSPI, 20000000)) {
        Serial.println("HALT: SD Card no montada. Verifica pines.");
        for (;;) { delay(1000); }
    }
    uint64_t cardMB = SD.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("[SD]  ✓ Tarjeta: %llu MB (tipo: %d)\n", cardMB, SD.cardType());

    // ── Crear directorios SD ──────────────────────────────
    ensure_sd_dirs();

    // ── GPS Neo 6M (5 s timeout → 0.0, 0.0 si sin fix) ──
    //gps_init();

    // ── TFLite Micro — cargar modelo + Hanning + FFT ─────
    seismic_inference_init();

    // ── WiFi y NTP ────────────────────────────────────────
    connectWiFiEnterprise();

    // ── Fase 3: Identificación, mutex SD, cola y task de upload ──
    station_init();
    g_sd_mutex     = xSemaphoreCreateMutex();
    g_upload_queue = xQueueCreate(UPLOAD_QUEUE_SIZE, sizeof(UploadReq));
    // Stack de 20 KB — mbedTLS (Firebase HTTPS) necesita ~15-18 KB para el handshake SSL
    xTaskCreatePinnedToCore(upload_task, "upload", 20480, nullptr, 1, nullptr, 0);
    Serial.println("[Upload] Task iniciada en Core 0");

    // ── Abrir primer archivo de datos ─────────────────────
    if (!openNewFile()) {
        Serial.println("HALT: No se pudo crear archivo en SD.");
        for (;;) { delay(1000); }
    }

    // ── Web Server ────────────────────────────────────────
    if (WiFi.status() == WL_CONNECTED) {
        setupWebServer();
    }

    // ── Hardware Timer 0: dispara ISR a 200 Hz ────────────
    // APB clock = 80 MHz, prescaler = 80 → 1 tick = 1 µs
    // Período de 200 Hz = 5000 µs → alarmValue = 5000
    g_timer = timerBegin(0, 80, true);
    timerAttachInterrupt(g_timer, &onTimerISR, true);
    timerAlarmWrite(g_timer, 5000, true);   // 5000 µs = 200 Hz
    timerAlarmEnable(g_timer);

    Serial.println("[TMR] ✓ Timer 0 @ 200 Hz (período = 5000 µs)");
    Serial.println("══════════════════ Logger activo ═══════════════════");


}

// ============================================================
//  LOOP — Atiende muestras y aplica filtro BP + bias
// ============================================================
void loop() {
    // Leer flag del timer (sección crítica mínima)
    bool doSample = false;
    portENTER_CRITICAL(&g_timerMux);
    if (g_sampleDue) {
        g_sampleDue = false;
        doSample    = true;
    }
    portEXIT_CRITICAL(&g_timerMux);

    if (!doSample) return;

    // ── 1. Leer ADXL345 (burst de 6 bytes vía I2C) ────────
    int16_t ax_raw, ay_raw, az_raw;
    if (!adxl_readXYZ(ax_raw, ay_raw, az_raw)) {
        g_dropCount++;
        return;
    }

    // ── 2. Restar bias de offset (calibrado en setup) ─────
    // Centra la señal en 0 antes de entrar al filtro.
    // La resta es aritmética de float para máxima precisión.
    float ax_c = (float)ax_raw - g_sw_bias[0];
    float ay_c = (float)ay_raw - g_sw_bias[1];
    float az_c = (float)az_raw - g_sw_bias[2];

    // ── 3. Filtro paso-banda 0.1–20 Hz (HP→LP en cascada) ─
    // Resultado en LSB de int16_t — misma escala que las crudas.
    // Costo: ~6 multiplicaciones FPU ≈ 0.3 µs @ 240 MHz (< 0.006% del período)
    int16_t ax_f, ay_f, az_f;
    bandpassFilter(ax_c, ay_c, az_c, ax_f, ay_f, az_f);

    // ── 4. Construir muestra y escribir en SD ─────────────
    // El campo az_dynamic ahora contiene la señal BP del eje Z
    // (en v1 contenía solo el resultado del filtro HP de gravedad)
    Sample s = {
        .timestamp_ms = millis(),
        .ax           = ax_f,
        .ay           = ay_f,
        .az_dynamic   = az_f
    };
    writeSample(s);

    // ── 3b. STA/LTA energy detector (EMA inline) ──────────
    // Activa inferencia CNN solo ante actividad sísmica potencial.
    // τ_STA=0.5 s · τ_LTA=10 s — mismos parámetros que el builder Python.
    // Reset de ventana en el onset asegura que el espectrograma
    // siempre empieza en el momento del evento (= patrón de entrenamiento).
    {
        float e = (float)ax_f*(float)ax_f
                + (float)ay_f*(float)ay_f
                + (float)az_f*(float)az_f;
        g_sta += (e - g_sta) * (1.0f / STA_SAMPLES);
        g_lta += (e - g_lta) * (1.0f / LTA_SAMPLES);
        if (g_lta_warmup > 0) {
            --g_lta_warmup;
        } else {
            float ratio = g_sta / g_lta;
            if (!g_sta_trig && ratio >= STA_LTA_ON) {
                g_sta_trig = true;
                g_win_ptr  = 0;                    // ventana fresca desde el onset
                g_sta_hold = SEISMIC_WIN_SAMPLES;  // hold ≥ 1 ventana completa (4 s)
                Serial.printf("[STA/LTA] TRIGGER  ratio=%.2f\n", ratio);
            } else if (g_sta_trig) {
                if (g_sta_hold > 0) {
                    --g_sta_hold;
                } else if (ratio < STA_LTA_OFF) {
                    g_sta_trig = false;
                    Serial.printf("[STA/LTA] detrigger ratio=%.2f\n", ratio);
                }
            }
        }
    }

    // ── 5. Acumular ventana e inferencia CNN (solo si STA/LTA activo) ─────
    g_win_ax[g_win_ptr] = ax_f;
    g_win_ay[g_win_ptr] = ay_f;
    g_win_az[g_win_ptr] = az_f;
    if (++g_win_ptr >= SEISMIC_WIN_SAMPLES) {
        g_win_ptr = 0;
        if (g_sta_trig) {
            float score = run_inference();    // ~80–120 ms, solo ante evento
            Serial.printf("[CNN] score=%.3f  %s\n",
                          score, score > SEISMIC_THRESHOLD ? "SISMO" : "ruido");
            if (score > SEISMIC_THRESHOLD) {
                saveEventFile(score);
            }
        }
    }

    // ── Actualizar GPS (no bloqueante) ────────────────────
    //gps_update();

    // ── 6. Diagnóstico serial cada 5 segundos ─────────────
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 5000) {
        lastPrint = millis();
        float ax_g = ax_f * 0.0039f;
        float ay_g = ay_f * 0.0039f;
        float az_g = az_f * 0.0039f;
        Serial.printf("[LOG] ax=%.4fg ay=%.4fg az=%.4fg | "
                      "samples=%lu drops=%lu\n",
                      ax_g, ay_g, az_g,
                      g_totalSamples, g_dropCount);
    }
// ── 6. Actualizar OLED cada 1500 ms ──────────────────────
//
//  Se actualiza FUERA del bloque if(!doSample) para que
//  el refresco ocurra incluso si hubo un drop de muestra.
//  El intervalo de 1500 ms es un compromiso entre fluidez
//  visual y tiempo de bus ocupado:
//    - Cada update ocupa el bus ~25 ms
//    - A 200 Hz hay una muestra cada 5 ms
//    - La actualización puede "robar" hasta 5 períodos de muestreo
//    - Con 1500 ms de intervalo, el overhead es 25/1500 = 1.6%
//
static uint32_t lastOled = 0;
if (millis() - lastOled > 1500) {
    lastOled = millis();
    oled_update();
}
}