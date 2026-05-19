# Dashboard y Conversión Sismológica — PANdeMaiz Quake

Esta carpeta contiene el frontend del sistema: un dashboard cartográfico en tiempo real construido con Leaflet.js, y la lógica de descarga histórica masiva con conversión de formato en memoria. El backend FastAPI sirve estos archivos estáticos en la ruta raíz `/`.

---

## Dashboard (`index.html` + `app.js`)

### Mapa interactivo

- **Biblioteca:** Leaflet.js (tiles OpenStreetMap)
- **Centro inicial:** Colombia (4.5°N, 74.0°W), zoom 6
- **Actualización:** polling cada 5 s a `GET /api/stations`

### Estados de los pines

| Color | Significado | Condición |
|-------|-------------|-----------|
| Verde oscuro (`#006837`) | Conectada, sin alerta | `estado === "Conectada"  &&  !alerta` |
| Rojo (`#9b1c1c`) | Alerta activa | `alerta === true` |
| Gris (`#3a5a40`) | Offline / desconocida | cualquier otro caso |

Los pines son `L.divIcon` con un SVG de teardrop personalizado para mayor visibilidad sobre el mapa.

### Banner de Alerta Global

Un banner rojo aparece en la parte superior del dashboard si **al menos una estación** devuelve `alerta: true` en la respuesta de `/api/stations`. Desaparece automáticamente en el siguiente ciclo de polling cuando el motor de consenso ya no registra estaciones activas.

---

## Flujo de descarga histórica

El panel lateral del dashboard permite descargar datos históricos por hora en tres formatos:

```
Usuario
  │
  ▼  Selecciona estación + fecha
  │
  ├─► GET /api/v1/estaciones/{id}/disponibilidad?fecha=YYYY-MM-DD
  │       ← ["00", "01", "13", ...]   (horas con datos en Firebase)
  │
  ▼  Selecciona hora + formato (bin | anc | mseed)
  │
  └─► GET /api/v1/download-hour?station_id=&date=&hour=&format=
          │
          ▼  Backend (FastAPI)
          ├─ Firebase RTDB: lee logs_acel[station_id][date][hour]
          │                → lista de metadatos {archivo, url, lat, lon}
          ├─ Para cada archivo:
          │     GET http://{ip}/download?f={path}   (timeout 20 s)
          │     → bytes crudos del ESP32
          │     → convertir al formato solicitado
          │     → escribir en zipfile.ZipFile (io.BytesIO, ZIP_DEFLATED)
          │     Si falla → log warning + continuar (resiliente)
          └─ Response: application/zip
              Headers:
                Content-Disposition: attachment; filename="PANdeMaiz_ID_DATE_HHh.zip"
                X-Files-Downloaded: N
                X-Files-Errors: E

          ▼  Browser
          Blob → URL.createObjectURL() → <a>.click() → descarga automática
```

---

## Conversión sismológica (`app/converter.py`)

El módulo `converter.py` implementa la conversión en memoria del formato binario propietario del ESP32 a formatos estándar de sismología.

### Escala de conversión física

```
a [g]     = LSB_raw × 0.0039          (ADXL345 ±2g full-resolution, 256 LSB/g)
a [cm/s²] = a [g] × 980.665          (1 g = 980.665 cm/s²)
```

### `parse_bin(raw: bytes)`

Parsea el binario del ESP32. Detecta automáticamente versión 2 (cabecera 8 bytes) y versión 3 (cabecera 16 bytes con GPS):

```
Header v3 (16 bytes):
  [0:4]  MAGIC     = 0xDA7A1345  (uint32_le)
  [4:6]  VERSION   = 0x0003      (uint16_le)
  [6:8]  SAMPLE_RATE = 200       (uint16_le)
  [8:12] lat                     (float32_le)
  [12:16] lon                    (float32_le)

Por muestra (10 bytes):
  [0:4]  timestamp_ms (uint32_le)
  [4:6]  ax (int16_le, EW)
  [6:8]  ay (int16_le, NS)
  [8:10] az (int16_le, VER)

Retorna: (header: dict, data: ndarray (N,4) float32 [t_s, ax_g, ay_g, az_g])
```

### `bin_to_anc(raw, station_id, filename)`

Genera texto en formato `.anc` compatible con el notebook `seismic_dataset_builder_v3.ipynb`:

- **Cabecera:** exactamente 20 líneas (invariante requerida por `parse_anc()`).
- **Datos:** columnas `EW  VER  NS` en cm/s² con 4 decimales.
- **Conversión de ejes:**
  - ax → EW: `ew  [cm/s²] = ax_g × 980.665`
  - az → VER: `ver [cm/s²] = az_g × 980.665`
  - ay → NS: `ns  [cm/s²] = ay_g × 980.665`

### `bin_to_mseed(raw, station_id, filename)`

Genera MiniSEED estándar FDSN usando ObsPy:

| Canal | Eje físico | Componente |
|-------|-----------|-----------|
| HNE | EW (ax) | East-West |
| HNN | NS (ay) | North-South |
| HNZ | VER (az) | Vertical |

- Red: `PAN`
- Estación: primeros 5 caracteres de `station_id` en mayúsculas
- `starttime`: extraído del nombre de archivo (`accel_YYYYMMDDHHmmss.bin`) o `now()` si no parsea
- Retorna bytes MiniSEED listos para escribir o transmitir

---

## API de conversión individual

Para descargar y convertir un archivo específico (sin ZIP):

```
GET /download?station_id=PANUDEA01&file=/Eventos/2026/05/...&format=anc
```

| Parámetro | Valores | Default |
|-----------|---------|---------|
| `station_id` | ID de la estación | requerido |
| `file` | Ruta en la SD del ESP32 | requerido |
| `format` | `bin` \| `anc` \| `mseed` | `bin` |

El backend lee la IP del nodo desde Firebase RTDB y hace proxy directo al servidor web embebido del ESP32 (`http://{ip}/download?f={path}`).

> Rutas con `..` son rechazadas con HTTP 403 (prevención de path traversal).
