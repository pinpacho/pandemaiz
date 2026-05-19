"""
converter.py — Conversión de archivos .bin del ESP32 a MiniSEED y .anc.

Reutiliza la misma lógica de parse_bin() del notebook
Data_Labeling/seismic_dataset_builder_v3.ipynb.

Mapeo de ejes (heredado de Data_Labeling/anc_to_sim.py línea 96):
  firmware ax  ←→  EW   (columna 0 en .anc)
  firmware ay  ←→  NS   (columna 2 en .anc)
  firmware az  ←→  VER  (columna 1 en .anc)
"""

import io
import re
import struct
from datetime import datetime, timezone

import numpy as np
from obspy import Stream, Trace, UTCDateTime

MAGIC = 0xDA7A1345
SCALE_G = 0.0039          # g/LSB — ADXL345 ±2g full-resolution
G_TO_CMS2 = 980.665       # factor de conversión

# Tamaños de header según versión del firmware
_HEADER_V2 = 8   # MAGIC(4) + VERSION(2) + SAMPLE_RATE(2)
_HEADER_V3 = 16  # V2 + lat(4,float) + lon(4,float)
_SAMPLE_SIZE = 10  # timestamp_ms(4) + ax(2) + ay(2) + az(2)


def parse_bin(raw: bytes) -> tuple[dict, np.ndarray]:
    """
    Parsea los bytes crudos de un .bin del ESP32.

    Retorna:
        header: dict con version, sample_rate, lat, lon
        data:   ndarray (N, 4) float32 — columnas [t_s, ax_g, ay_g, az_g]
    """
    if len(raw) < _HEADER_V2:
        raise ValueError("Archivo .bin demasiado corto")

    magic, version, sr = struct.unpack_from("<IHH", raw, 0)
    if magic != MAGIC:
        raise ValueError(f"Magic inválido: 0x{magic:08X} (esperado 0x{MAGIC:08X})")

    if version >= 3 and len(raw) >= _HEADER_V3:
        lat, lon = struct.unpack_from("<ff", raw, 8)
        header_size = _HEADER_V3
    else:
        lat, lon = 0.0, 0.0
        header_size = _HEADER_V2

    header = {"version": version, "sample_rate": int(sr), "lat": lat, "lon": lon}

    samples = []
    pos = header_size
    while pos + _SAMPLE_SIZE <= len(raw):
        ts_ms, ax, ay, az = struct.unpack_from("<Ihhh", raw, pos)
        samples.append([ts_ms / 1000.0, ax * SCALE_G, ay * SCALE_G, az * SCALE_G])
        pos += _SAMPLE_SIZE

    if not samples:
        raise ValueError("El archivo .bin no contiene muestras")

    return header, np.array(samples, dtype=np.float32)


def _starttime_from_filename(filename: str) -> UTCDateTime:
    """Extrae la fecha/hora del nombre del archivo (ej. accel_20260515_140000.bin)."""
    m = re.search(r"(\d{8})_(\d{6})", filename)
    if m:
        try:
            dt = datetime.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S")
            return UTCDateTime(dt.replace(tzinfo=timezone.utc))
        except ValueError:
            pass
    return UTCDateTime(datetime.now(timezone.utc))


def bin_to_mseed(raw: bytes, station_id: str, filename: str = "") -> bytes:
    """
    Convierte bytes de un .bin a MiniSEED con tres trazas (HNE, HNN, HNZ).

    Los canales siguen el estándar FDSN:
      HNE = East-West  = ax
      HNN = North-South = ay
      HNZ = Vertical   = az
    """
    header, data = parse_bin(raw)
    sr = header["sample_rate"]
    t0 = _starttime_from_filename(filename)
    sta = station_id[:5].upper()

    st = Stream()
    for comp, col in [("E", 1), ("N", 2), ("Z", 3)]:
        tr = Trace(data=data[:, col].astype(np.float32))
        tr.stats.network = "PAN"
        tr.stats.station = sta
        tr.stats.channel = f"HN{comp}"
        tr.stats.sampling_rate = sr
        tr.stats.starttime = t0
        st.append(tr)

    buf = io.BytesIO()
    st.write(buf, format="MSEED")
    return buf.getvalue()


def bin_to_anc(
    raw: bytes,
    station_id: str,
    filename: str = "",
    meta: dict | None = None,
) -> str:
    """
    Convierte bytes de un .bin al formato .anc del SGC/PANdeMaiz.

    La cabecera sigue las mismas 20 líneas que parse_anc() del notebook
    para que el dataset builder pueda leer el archivo generado.
    Datos en cm/s², orden de columnas: EW  VER  NS.
    """
    header, data = parse_bin(raw)
    meta = meta or {}
    fecha = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    # Cabecera de 20 líneas — compatible con parse_anc()
    lines = [
        "PANDEMAIZ QUAKE — REGISTRO ACELEROMETRICO",
        f"ESTACION: {station_id}",
        f"CODIGO DE LA ESTACION: {station_id}",
        "RED: PANdeMaiz / Universidad de Antioquia — GICM",
        f"LATITUD DE LA ESTACION: {header['lat']:.6f}",
        f"LONGITUD DE LA ESTACION: {header['lon']:.6f}",
        f"LATITUD DEL EVENTO: {meta.get('lat_evento', '0.000000')}",
        f"LONGITUD DEL EVENTO: {meta.get('lon_evento', '0.000000')}",
        f"PROFUNDIDAD (km): {meta.get('profundidad_km', '0')}",
        f"M={meta.get('magnitud', '?')}",
        f"DISTANCIA EPICENTRAL: {meta.get('dist_epicentral', '?')} km",
        f"INTERVALO DE MUESTREO (s): {1.0 / header['sample_rate']:.6f}",
        f"NUMERO DE DATOS: {len(data)}",
        "INSTRUMENTO: ESP32 + ADXL345  +-2g  3.9 mg/LSB  BP 0.1-20 Hz",
        "UNIDADES: cm/s^2  (EW  VER  NS)",
        f"VERSION BINARIO: {header['version']}",
        "PROCESAMIENTO: Filtro Butterworth 2do orden HP 0.1 Hz LP 20 Hz",
        "GENERADO POR: PANdeMaiz Quake Backend v1.0",
        f"ARCHIVO ORIGEN: {filename}",
        f"FECHA CONVERSION: {fecha}",
    ]
    assert len(lines) == 20, "La cabecera debe tener exactamente 20 líneas"

    # Datos en cm/s² — columnas EW (ax), VER (az), NS (ay)
    # Mismo orden que el .anc original del SGC que usa parse_anc()
    rows = []
    for row in data:
        ew = row[1] * G_TO_CMS2   # ax → EW
        ver = row[3] * G_TO_CMS2  # az → VER
        ns = row[2] * G_TO_CMS2   # ay → NS
        rows.append(f"{ew:12.4f}  {ver:12.4f}  {ns:12.4f}")

    return "\n".join(lines) + "\n" + "\n".join(rows) + "\n"
