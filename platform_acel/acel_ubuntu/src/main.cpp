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

#include <U8g2lib.h>   // OLED — instalar "U8g2" de olikraus en Library Manager

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

// Cabecera del archivo binario (8 bytes, sin cambios de layout)
struct __attribute__((packed)) FileHeader {
    uint32_t magic;        // BIN_MAGIC
    uint16_t version;      // BIN_VERSION (ahora 0x0002)
    uint16_t sample_rate;  // Hz
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
char     g_currentFile[48];
uint32_t g_sampleCount = 0;

// --- Web Server ---------------------------------------------
AsyncWebServer g_server(WEB_SERVER_PORT);

// --- Estadísticas en tiempo real ----------------------------
volatile uint32_t g_totalSamples = 0;
volatile uint32_t g_dropCount    = 0;

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
//  GESTIÓN DE ARCHIVOS SD
// ============================================================

static void buildFileName(char *buf, size_t len) {
    time_t    now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    snprintf(buf, len, "/accel_%04d%02d%02d_%02d%02d%02d.bin",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
}

static bool openNewFile() {
    if (g_dataFile) {
        g_dataFile.flush();
        g_dataFile.close();
    }

    buildFileName(g_currentFile, sizeof(g_currentFile));
    g_dataFile = SD.open(g_currentFile, FILE_WRITE);

    if (!g_dataFile) {
        Serial.printf("[SD] ⚠ No se pudo abrir: %s\n", g_currentFile);
        return false;
    }

    FileHeader hdr = { BIN_MAGIC, BIN_VERSION, (uint16_t)SAMPLE_RATE_HZ };
    g_dataFile.write((uint8_t*)&hdr, sizeof(hdr));
    g_sampleCount = 0;

    Serial.printf("[SD] ✓ Nuevo archivo: %s\n", g_currentFile);
    return true;
}

static void writeSample(const Sample &s) {
    if (!g_dataFile) return;

    g_dataFile.write((uint8_t*)&s, sizeof(s));
    g_sampleCount++;
    g_totalSamples++;

    if (g_sampleCount % WRITE_FLUSH_EVERY == 0) {
        g_dataFile.flush();
    }

    if (g_sampleCount >= SAMPLES_PER_FILE) {
        Serial.printf("[SD] Archivo completo: %s (%lu muestras, %.1f KB)\n",
                      g_currentFile, g_sampleCount,
                      (sizeof(FileHeader) + g_sampleCount * sizeof(Sample)) / 1024.0f);
        openNewFile();
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
    <h2>&#x25A0; Archivos de Datos (.bin)</h2>
    <div class="file-grid">
)rawhtml";

    File root = SD.open("/");
    bool hasFiles = false;
    if (root) {
        File f = root.openNextFile();
        while (f) {
            String fname = String(f.name());
            if (!f.isDirectory() && fname.endsWith(".bin")) {
                hasFiles = true;
                uint32_t fsz   = f.size();
                uint32_t nSamp = fsz > sizeof(FileHeader)
                                 ? (fsz - sizeof(FileHeader)) / sizeof(Sample)
                                 : 0;
                float durSec = nSamp / (float)SAMPLE_RATE_HZ;

                html += "<div class='file-card'>";
                html += "<div class='file-name'>&#x1F4BE; " + fname + "</div>";
                html += "<div class='file-meta'>";
                html += "<b>" + String(fsz / 1024) + " KB</b> &bull; ";
                html += "<b>" + String(nSamp) + "</b> muestras &bull; ";
                html += "<b>" + String((int)durSec) + "s</b> duración</div>";
                html += "<a class='dl-btn' href='/download?f=" + fname + "'>&#x2B07; Descargar</a>";
                html += "</div>\n";
            }
            f = root.openNextFile();
        }
        root.close();
    }

    if (!hasFiles) {
        html += "<div class='empty'>Sin archivos aún. El logger genera uno nuevo cada minuto.</div>";
    }

    html += R"rawhtml(
    </div>
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

    // ── WiFi y NTP ────────────────────────────────────────
    connectWiFiEnterprise();

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

    // ── 5. Diagnóstico serial cada 5 segundos ─────────────
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