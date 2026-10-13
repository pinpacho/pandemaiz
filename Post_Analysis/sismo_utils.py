"""
Funciones comunes de los notebooks del sismo (`plantilla_analisis_sismo.ipynb`, `plantilla_espectros_sismo.ipynb`
y `mapa_sismo.ipynb`).

- Acelerógrafo UdeA: lectura de los .bin del ESP32 + ADXL345 y reconstrucción de la señal continua.
- Estaciones del SGC: lectura de .anc y .mseed, calibración del .mseed contra el .anc y carga de cada estación
  con la mejor fuente disponible.
- Ventanas de tiempo, conversión de unidades y piezas comunes de las gráficas en formato SGC.

Convenciones: los datos van en columnas [EW, NS, VER] (`LABELS`), las filas de las gráficas en Z, N, E
(`ORDEN_FILAS`) y los tiempos en datetime64[us] UTC. Las muestras de los .bin se ubican con su reloj
millis() (`timestamp_ms`) anclado a la hora local del nombre de los archivos (ver `reconstruct_full_hour`).

Si se edita este archivo con un notebook abierto, hay que reiniciar el kernel para que lo vuelva a importar.
"""
import re
import struct
import pathlib
import unicodedata
import warnings
from datetime import datetime, timedelta, timezone

import numpy as np
import matplotlib.pyplot as plt
from obspy import Stream, read as obspy_read
from scipy.signal import correlate, correlation_lags

# -- Formato .bin del ESP32 + ADXL345 -------------------------------------------
MAGIC     = 0xDA7A1345   # número mágico del formato .bin
SCALE_G   = 0.0039       # g/LSB -- ADXL345 +-2g full-resolution
G_TO_CMS2 = 980.665      # g -> cm/s²
FS        = 200          # Hz, frecuencia de muestreo del Acelerógrafo UdeA

# -- Componentes: orden de las columnas de datos y orden de las filas en las gráficas --
LABELS         = ["EW", "NS", "VER"]
CANAL_POR_COMP = {"EW": "E", "NS": "N", "VER": "Z"}
ORDEN_FILAS    = ["VER", "NS", "EW"]          # Z, N, E como en las gráficas del SGC
BANDA_SGC      = "HN"                         # canales acelerográficos del SGC: HNE/HNN/HNZ

# -- Valores por defecto de la configuración (los del Chocó) ----------------------
TZ_OFFSET_HORAS     = -5    # hora local de Colombia = UTC-5
BASELINE_S          = 20    # segundos iniciales para quitar la línea base del Acelerógrafo UdeA
ANC_INICIO_OFFSET_S = -30   # inicio estimado del .anc respecto al origen, si no hay .mseed

MODOS_VENTANA = ("hora_completa", "estacion", "primera_alerta", "relativa_origen", "manual")

# -- Estilo (formato de las gráficas oficiales del SGC) --------------------------
COLOR_ACEL   = "black"
COLOR_SGC    = "#D32F2F"
COLOR_ALERTA = "#FFB300"
FIGSIZE, DPI = (6, 6), 150                     # 900x900 px


def _avisar(avisos, mensaje):
    """Agrega el aviso a la lista `avisos` o, si no hay lista, lo imprime."""
    if avisos is None:
        print(f"[AVISO] {mensaje}")
    else:
        avisos.append(mensaje)


# ==========================================================================================================
# Utilidades y configuración
# ==========================================================================================================
def slug(texto):
    s = unicodedata.normalize("NFKD", str(texto)).encode("ascii", "ignore").decode()
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")


def nombre_ventana(v):
    return slug(v.get("nombre") or v["modo"])


def unir_nombres(nombres):
    return nombres[0] if len(nombres) == 1 else ", ".join(nombres[:-1]) + " y " + nombres[-1]


def _contar(carpeta, patron):
    carpeta = pathlib.Path(carpeta)
    return len(list(carpeta.glob(patron))) if carpeta.is_dir() else None


def zonas_horarias(zonas, tz_offset_horas=TZ_OFFSET_HORAS, etiqueta_local="COT"):
    """[(etiqueta, sufijo, desfase respecto a UTC)] de cada zona de `zonas` ("UTC", "LOCAL"), en ese orden."""
    disponibles = {
        "UTC":   ("UTC", timedelta(0)),
        "LOCAL": (f"Local ({etiqueta_local}, UTC{tz_offset_horas:+d})", timedelta(hours=tz_offset_horas)),
    }
    return [(disponibles[z][0], z, disponibles[z][1]) for z in zonas]


def verificar_configuracion(dir_acel, dir_eventos, dir_sgc, estaciones_sgc, grupos, ventanas, zonas, unidades,
                            avisos=None):
    """
    Cuenta los archivos de cada carpeta de entrada y revisa grupos, ventanas, zonas y unidades de la
    configuración. Retorna la lista de errores (vacía si todo está bien); una carpeta opcional que no existe
    va a `avisos`.
    """
    errores = []
    for etiqueta, carpeta, patron, obligatoria in [
            ("aceleraciones", dir_acel, "*.bin", True),
            ("eventos", dir_eventos, "*.bin", False),
            ("SGC .anc", dir_sgc, "*.anc", bool(estaciones_sgc)),
            ("SGC .mseed", dir_sgc, "*.mseed", False)]:
        n = _contar(carpeta, patron)
        if n is None:
            print(f"  {etiqueta:<13}: NO EXISTE  {carpeta}")
            if obligatoria:
                errores.append(f"no existe la carpeta {carpeta}")
            else:
                _avisar(avisos, f"no existe la carpeta {carpeta}")
        else:
            print(f"  {etiqueta:<13}: {n:3d} archivo(s) {patron:<8} en {carpeta}")
    if _contar(dir_acel, "*.bin") == 0:
        errores.append(f"no hay archivos .bin en {dir_acel}")

    conocidas = {e["nombre"].lower() for e in estaciones_sgc} | {e["codigo"].lower() for e in estaciones_sgc}
    for grupo, nombres in grupos.items():
        for n in nombres:
            if n.lower() not in conocidas:
                errores.append(f"el grupo '{grupo}' incluye '{n}', que no está en ESTACIONES_SGC")
    for v in ventanas:
        if v.get("modo") not in MODOS_VENTANA:
            errores.append(f"ventana '{v.get('nombre')}': modo '{v.get('modo')}' no es uno de {MODOS_VENTANA}")
    errores += [f"zona '{z}' no es UTC ni LOCAL" for z in zonas if z not in ("UTC", "LOCAL")]
    errores += [f"unidad '{u}' no es 'g' ni 'cm/s²'" for u in unidades if u not in ("g", "cm/s²")]
    return errores


# ==========================================================================================================
# Tiempo
# ==========================================================================================================
def get_starttime_local(stem):
    """Fecha y hora local (naive) del patrón 'YYYYMMDD_HHMMSS' del nombre, o None."""
    m = re.search(r"(\d{8})_(\d{6})", stem)
    if m:
        try:
            return datetime.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S")
        except ValueError:
            pass
    return None


def local_to_utc(dt_local_naive, tz_offset_horas=TZ_OFFSET_HORAS):
    """Hora local (naive) -> UTC (aware)."""
    return dt_local_naive.replace(tzinfo=timezone(timedelta(hours=tz_offset_horas))).astimezone(timezone.utc)


def a_datetime64(valor, zona="UTC", tz_offset_horas=TZ_OFFSET_HORAS):
    """Texto 'YYYY-MM-DD HH:MM:SS', datetime o datetime64 -> datetime64[us] en UTC."""
    if isinstance(valor, np.datetime64):
        return valor.astype("datetime64[us]")
    dt = datetime.fromisoformat(valor) if isinstance(valor, str) else valor
    if dt.tzinfo is None:
        tz = timezone(timedelta(hours=tz_offset_horas)) if zona == "LOCAL" else timezone.utc
        dt = dt.replace(tzinfo=tz)
    return np.datetime64(dt.astimezone(timezone.utc).replace(tzinfo=None), "us")


def _eje_tiempo(inicio, n, dt):
    """Eje de tiempo datetime64[us] de n muestras separadas dt segundos desde `inicio`."""
    return a_datetime64(inicio) + (np.arange(n) * dt * 1e6).astype("int64").astype("timedelta64[us]")


# ==========================================================================================================
# Acelerógrafo UdeA: archivos .bin
# ==========================================================================================================
_HEADER_V2   = 8    # MAGIC(4) + VERSION(2) + SAMPLE_RATE(2)
_HEADER_V3   = 16   # V2 + lat(4) + lon(4)
_SAMPLE_SIZE = 10   # timestamp_ms(4) + ax(2) + ay(2) + az(2)
_DTYPE_MUESTRA = np.dtype([("t_ms", "<u4"), ("ax", "<i2"), ("ay", "<i2"), ("az", "<i2")])


def parse_bin(path):
    """
    Lee un archivo .bin del ESP32 (formato PANdeMaiz Quake).

    Retorna header {version, sample_rate, lat, lon} y data (N, 4) float32 [t_s, ax_g, ay_g, az_g].
    Ojo: en float32 t_s pierde resolución (≈0.1 s con el reloj en ~10⁶ s); para ubicar muestras en el
    tiempo usar `_muestras_bin()`.
    """
    header, m = _leer_bin(path)
    data = np.column_stack([m["t_ms"] / 1000.0, m["ax"] * SCALE_G, m["ay"] * SCALE_G, m["az"] * SCALE_G])
    return header, data.astype(np.float32)


def _muestras_bin(path):
    """(t_s float64 con resolución de ms, aceleraciones (N, 3) float64 en g [EW, NS, VER]) de un .bin."""
    _, m = _leer_bin(path)
    return (m["t_ms"].astype(np.float64) / 1000.0,
            np.column_stack([m["ax"], m["ay"], m["az"]]).astype(np.float64) * SCALE_G)


def _leer_bin(path):
    """Cabecera y arreglo estructurado de muestras (t_ms, ax, ay, az) de un .bin, sin conversiones."""
    raw = pathlib.Path(path).read_bytes()
    if len(raw) < _HEADER_V2:
        raise ValueError(f"{path}: archivo demasiado corto ({len(raw)} bytes)")

    magic, version, sr = struct.unpack_from("<IHH", raw, 0)
    if magic != MAGIC:
        raise ValueError(f"{path}: magic inválido 0x{magic:08X} (esperado 0x{MAGIC:08X})")

    if version >= 3 and len(raw) >= _HEADER_V3:
        lat, lon = struct.unpack_from("<ff", raw, 8)
        hdr_size = _HEADER_V3
    else:
        lat, lon = 0.0, 0.0
        hdr_size = _HEADER_V2
    header = {"version": version, "sample_rate": int(sr), "lat": lat, "lon": lon}

    n_samples = (len(raw) - hdr_size) // _SAMPLE_SIZE
    if n_samples == 0:
        raise ValueError(f"{path}: no contiene muestras")
    return header, np.frombuffer(raw, dtype=_DTYPE_MUESTRA, count=n_samples, offset=hdr_size)


def discover_bins(folder):
    folder = pathlib.Path(folder)
    files = sorted(folder.glob("*.bin")) if folder.is_dir() else []
    if not files:
        print(f"[AVISO] No se encontraron archivos .bin en: {folder}")
    return files


def extraer_score(stem):
    m = re.search(r"_s(\d+)$", stem)
    return m.group(1) if m else "?"


MICROPAUSA_S = 0.5   # huecos más cortos que esto: pausas breves del lazo del nodo (pantalla OLED, SD)


def _anclar_reloj(nombres_rel, t_s):
    """
    Ancla el reloj millis() del nodo a la hora de los nombres de archivo.

    `nombres_rel[k]`: hora del nombre del archivo k en segundos desde una referencia; `t_s[k]`: marcas
    timestamp_ms/1000 de sus muestras. Los archivos van ordenados por hora.

    El firmware nombra cada archivo con la hora (truncada al segundo) en que lo abre y toma la primera
    muestra después, así que para cada archivo la hora real de una muestra t cumple
    hora >= nombre - t[0] + t. El ancla de un arranque del nodo es la cota inferior más ajustada de todos
    sus archivos (error típico < 0.5 s). Un reinicio (millis vuelve a empezar) o un salto de reloj de más
    de 5 s abre un tramo nuevo con su propia ancla.

    Retorna el ancla de cada archivo (segundos): hora = ancla + t.
    """
    tramos = [[0]]
    for k in range(1, len(t_s)):
        reinicio = t_s[k][0] < t_s[k - 1][-1]
        salto = abs((nombres_rel[k] - nombres_rel[k - 1]) - (t_s[k][0] - t_s[k - 1][0])) > 5.0
        if reinicio or salto:
            tramos.append([k])
        else:
            tramos[-1].append(k)
    anclas = [0.0] * len(t_s)
    for tramo in tramos:
        ancla = max(nombres_rel[k] - float(t_s[k][0]) for k in tramo)
        for k in tramo:
            anclas[k] = ancla
    return anclas


def _indices_grilla(rel, rel0, fs):
    """
    Índice en la grilla de `fs` Hz de cada muestra, con hora real `rel` (s) y origen de la grilla `rel0`.

    El temporizador del nodo marca exactamente 1/fs, pero millis() registra la lectura con ~1 ms de jitter;
    redondear cada marca por separado haría chocar muestras vecinas. Dentro de un tramo sin pausas (paso
    < 1.5/fs) las muestras se ponen consecutivas, y al comienzo de cada tramo se usa la hora real
    redondeada (sin chocar con el tramo anterior), así que no se pierde ninguna muestra ni se acumula deriva.
    """
    inicio = np.concatenate([[True], np.diff(rel) >= 1.5 / fs])
    primeros = np.flatnonzero(inicio)
    ancla = np.rint((rel[primeros] - rel0) * fs).astype(np.int64)
    largos = np.diff(np.append(primeros, len(rel)))
    for j in range(1, len(primeros)):                        # sin choques entre tramos
        ancla[j] = max(ancla[j], ancla[j - 1] + largos[j - 1])
    tramo = np.cumsum(inicio) - 1
    return ancla[tramo] + (np.arange(len(rel)) - primeros[tramo])


def reconstruct_full_hour(accel_paths, evento_paths, fs=FS, baseline_s=BASELINE_S, tz_offset_horas=TZ_OFFSET_HORAS):
    """
    Reconstruye la señal continua del Acelerógrafo UdeA en una grilla UTC a `fs` Hz.

    1. Cada muestra de la grabación continua se ubica en su hora real: el reloj millis() de sus
       `timestamp_ms`, anclado a la hora de los nombres de archivo (`_anclar_reloj`). La hora del nombre es
       hora local UTC`tz_offset_horas`. No se supone que un archivo de 12 000 muestras dure 60 s: el nodo
       deja de muestrear durante sus pausas (clasificación de una alerta, refresco de la pantalla, SD) y esas
       pausas quedan como huecos en su lugar real.
    2. Los .bin de eventos NO rellenan huecos. Cada uno contiene la ventana de 4 s que clasificó la red,
       es decir, las muestras que la grabación continua ya tiene justo ANTES de la pausa de clasificación,
       y sus marcas temporales las reconstruye el firmware al escribirlos (después de clasificar). Se usan
       para ubicar la alerta: su ventana es la de las `len(evento)` muestras continuas que terminan
       inmediatamente antes de la hora de escritura del evento.
    3. Los huecos se rellenan por interpolación lineal. Los de `MICROPAUSA_S` o más se marcan como
       interpolados (status 3); los más cortos, como micropausas (status 4), para no saturar las gráficas.
    4. Se quita la línea base (media de los primeros `baseline_s` segundos).

    Retorna dict: t_utc (datetime64[us]), data_g (N, 3) [EW, NS, VER] en g sin línea base,
    status (1=continuo 3=interpolado 4=micropausa interpolada; 2 ya no se usa), alert_windows
    [(inicio_utc, fin_utc, score)] con la ventana realmente clasificada, ordenadas por hora, resumen y
    tz_offset_horas.
    """
    if not accel_paths:
        raise ValueError("reconstruct_full_hour: no hay archivos de aceleraciones")
    dt = 1.0 / fs

    accel_info = []
    for p in accel_paths:
        t_local = get_starttime_local(p.stem)
        if t_local is None:
            warnings.warn(f"{p.name}: el nombre no tiene fecha/hora YYYYMMDD_HHMMSS, se omite.")
            continue
        accel_info.append((local_to_utc(t_local, tz_offset_horas), *_muestras_bin(p)))
    if not accel_info:
        raise ValueError("reconstruct_full_hour: ningún archivo de aceleraciones pudo procesarse")
    accel_info.sort(key=lambda x: x[0])

    # 1) hora real de cada muestra, en segundos desde `ref`
    ref = accel_info[0][0]
    nombres_rel = [(nombre - ref).total_seconds() for nombre, _, _ in accel_info]
    t_s = [t for _, t, _ in accel_info]
    anclas = _anclar_reloj(nombres_rel, t_s)
    rel = [a + t for a, t in zip(anclas, t_s)]

    rel0 = min(float(r[0]) for r in rel)
    t_inicio = ref + timedelta(seconds=rel0)
    idx_por_archivo = [_indices_grilla(r, rel0, fs) for r in rel]
    n_total = int(max(int(i[-1]) for i in idx_por_archivo)) + 1

    data_g = np.full((n_total, 3), np.nan, dtype=np.float64)
    status = np.zeros(n_total, dtype=np.int8)
    for idx, (_, _, acel) in zip(idx_por_archivo, accel_info):
        data_g[idx] = acel                                    # ax, ay, az -> EW, NS, VER
        status[idx] = 1
    seg_huecos = float(np.sum(status == 0)) * dt

    # 2) alertas: ventana continua que termina justo antes de la escritura de cada evento
    alert_windows = []
    for p in evento_paths:
        t_local = get_starttime_local(p.stem)
        if t_local is None:
            continue
        nombre_ev = local_to_utc(t_local, tz_offset_horas)
        t_ev, _ = _muestras_bin(p)
        n_ev = len(t_ev)
        escritura = float(t_ev[0]) + n_ev * dt                # millis (s) al escribir la muestra 0
        nombre_ev_rel = (nombre_ev - ref).total_seconds()
        k = min(range(len(rel)), key=lambda j: abs(anclas[j] + escritura - nombre_ev_rel))
        mismo_tramo = [j for j in range(len(rel)) if anclas[j] == anclas[k]]
        t_tramo = np.concatenate([t_s[j] for j in mismo_tramo])
        rel_tramo = np.concatenate([rel[j] for j in mismo_tramo])
        previas = np.flatnonzero(t_tramo < escritura)
        if (abs(anclas[k] + escritura - nombre_ev_rel) < 10.0 and len(previas)
                and escritura - t_tramo[previas[-1]] < 30.0):
            fin_rel = float(rel_tramo[previas[-1]])
            ini_rel = float(rel_tramo[previas[max(len(previas) - n_ev, 0)]])
            a0, a1 = ref + timedelta(seconds=ini_rel), ref + timedelta(seconds=fin_rel)
        else:
            warnings.warn(f"{p.name}: no se encontró su ventana en la grabación continua; "
                          f"se marca la alerta en la hora del nombre.")
            a0, a1 = nombre_ev, nombre_ev + timedelta(seconds=n_ev * dt)
        alert_windows.append((a0, a1, extraer_score(p.stem)))
    alert_windows.sort(key=lambda a: a[0])

    # 3) interpolación lineal de los huecos; los cortos se marcan aparte como micropausas
    huecos = status == 0
    idx_todos = np.arange(n_total)
    for c in range(3):
        vacio = np.isnan(data_g[:, c])
        if vacio.any() and (~vacio).sum() >= 2:
            data_g[vacio, c] = np.interp(idx_todos[vacio], idx_todos[~vacio], data_g[~vacio, c])
    d = np.diff(np.concatenate([[0], huecos.astype(np.int8), [0]]))
    n_largos = 0
    for i0, i1 in zip(np.flatnonzero(d == 1), np.flatnonzero(d == -1)):
        largo = (i1 - i0) * dt >= MICROPAUSA_S
        status[i0:i1] = 3 if largo else 4
        n_largos += int(largo)

    n_base = min(int(baseline_s * fs), n_total)              # 4) línea base: media del tramo inicial
    data_g = data_g - np.nanmean(data_g[:n_base], axis=0)

    return {
        "t_utc": _eje_tiempo(t_inicio, n_total, dt),
        "data_g": data_g,
        "status": status,
        "alert_windows": alert_windows,
        "resumen": {
            "segundos_totales": n_total * dt,
            "segundos_hueco_original": seg_huecos,
            "segundos_rellenados_evento": 0.0,               # se conserva la clave; los eventos ya no rellenan
            "segundos_interpolados": float(np.sum(status == 3)) * dt,
            "segundos_micropausas": float(np.sum(status == 4)) * dt,
            "huecos_largos": n_largos,
        },
        "tz_offset_horas": tz_offset_horas,
    }


def imprimir_reconstruccion(recon, n_aceleraciones, n_eventos):
    """Resumen de reconstruct_full_hour(): tramo reconstruido, huecos y alertas (UTC y hora local)."""
    r = recon["resumen"]
    local = timedelta(hours=recon["tz_offset_horas"])
    print(f"Archivos: {n_aceleraciones} de aceleraciones, {n_eventos} de eventos")
    print(f"Señal reconstruida: {recon['t_utc'][0]} -> {recon['t_utc'][-1]} UTC  ({r['segundos_totales']:.1f} s)")
    print(f"  hueco original (sin datos)        : {r['segundos_hueco_original']:.2f} s "
          f"({100 * r['segundos_hueco_original'] / r['segundos_totales']:.1f} %)")
    print(f"  interpolado, huecos >= {MICROPAUSA_S:g} s      : {r['segundos_interpolados']:.2f} s "
          f"en {r['huecos_largos']} hueco(s)")
    print(f"  interpolado, micropausas < {MICROPAUSA_S:g} s  : {r['segundos_micropausas']:.2f} s")
    print(f"Alertas detectadas: {len(recon['alert_windows'])} (ventana de 4 s realmente clasificada)")
    for a0, a1, score in recon["alert_windows"]:
        print(f"  {a0:%Y-%m-%d %H:%M:%S} UTC  ({(a0 + local):%H:%M:%S} local)  ->  {a1:%H:%M:%S} UTC   score s{score}")


# ==========================================================================================================
# Estaciones del SGC: archivos .anc y .mseed
# ==========================================================================================================
def _leer_texto(path):
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="latin-1")


def _cabecera_anc(meta, path):
    """Diccionario con las 19 líneas de cabecera `clave: valor` de un .anc."""
    def _valor_float(linea):
        return float(re.search(r"[-+]?\d+\.?\d*", linea.split(":")[-1]).group())

    m = re.search(r"(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2}:\d{2})\s+M=([\d.]+)", meta[1])
    if not m:
        raise ValueError(f"{path}: no se pudo leer fecha/hora/magnitud de la línea 2 de la cabecera")
    fecha, hora, mag = m.groups()
    header = {
        "evento_datetime_utc":   datetime.strptime(f"{fecha} {hora}", "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc),
        "magnitud":              float(mag),
        "descripcion_evento":    meta[1][:m.start()].strip(" -"),
        "lat_evento":            _valor_float(meta[2]),
        "lon_evento":            _valor_float(meta[3]),
        "profundidad_km":        _valor_float(meta[4]),
        "codigo_estacion":       meta[5].split(":")[-1].strip(),
        "lat_estacion":          _valor_float(meta[7]),
        "lon_estacion":          _valor_float(meta[8]),
        "distancia_epicentral":  _valor_float(meta[9]),
        "distancia_hipocentral": _valor_float(meta[10]),
        "sample_dt":             _valor_float(meta[11]),
        "n_datos":               int(_valor_float(meta[12])),
        "duracion_s":            _valor_float(meta[13]),
    }
    header["sample_rate"] = round(1.0 / header["sample_dt"])
    return header


def leer_cabecera_anc(path):
    """Solo la cabecera de un .anc (las mismas claves que el header de parse_anc())."""
    path = pathlib.Path(path)
    return _cabecera_anc(_leer_texto(path).splitlines()[:19], path)


def parse_anc(path, inicio_registro_utc=None):
    """
    Lee un .anc del SGC. `inicio_registro_utc` es la hora real de la primera muestra (el archivo no la
    trae); si es None no se arma el eje de tiempo.

    Retorna header, data_cms2 (N, 3) [EW, NS, VER], data_g, t_utc (o None) y cols (orden del archivo).
    """
    path = pathlib.Path(path)
    lineas = _leer_texto(path).splitlines()
    meta, linea_cols, datos_lineas = lineas[:19], lineas[19], lineas[20:]
    header = _cabecera_anc(meta, path)

    cols = [c.strip().upper() for c in linea_cols.split()]
    datos_raw = np.array([[float(x) for x in l.split()] for l in datos_lineas if l.strip()], dtype=np.float64)
    if datos_raw.shape[1] != len(cols):
        raise ValueError(f"{path}: {datos_raw.shape[1]} columnas de datos vs. {len(cols)} en el encabezado")

    data_cms2 = datos_raw[:, [cols.index(c) for c in LABELS]]
    t_utc = (_eje_tiempo(inicio_registro_utc, len(data_cms2), header["sample_dt"])
             if inicio_registro_utc is not None else None)
    return header, data_cms2, data_cms2 / G_TO_CMS2, t_utc, cols


def leer_mseed_carpeta(carpeta, avisos=None):
    """Todos los .mseed de la carpeta en un Stream, con los segmentos de cada canal unidos."""
    st = Stream()
    archivos = sorted(pathlib.Path(carpeta).glob("*.mseed")) if pathlib.Path(carpeta).is_dir() else []
    for p in archivos:
        try:
            st += obspy_read(str(p))
        except Exception as e:
            _avisar(avisos, f"{p.name}: no se pudo leer como miniSEED ({e})")
    if len(st):
        st.merge(method=1, fill_value="interpolate")
    return st, archivos


def buscar_anc(carpeta, codigo, loc, avisos=None):
    """El .anc `*_<codigo>_<loc>.anc` de la carpeta, o None."""
    candidatos = sorted(pathlib.Path(carpeta).glob(f"*_{codigo}_{loc}.anc"))
    if len(candidatos) > 1:
        _avisar(avisos, f"hay {len(candidatos)} .anc para {codigo}_{loc}; se usa {candidatos[0].name}")
    return candidatos[0] if candidatos else None


def calibrar_canal_mseed(tr, señal_anc_cms2, dt_anc):
    """
    Escala cuentas -> cm/s² de un canal .mseed por correlación cruzada contra el mismo canal del .anc,
    usando solo el tramo en que ambos se solapan. También da la hora real de inicio del .anc.
    """
    x = tr.data.astype(np.float64)
    x = x - np.median(x)                                   # offset DC del digitalizador
    y = np.asarray(señal_anc_cms2, dtype=np.float64)

    corr = correlate(y / (np.std(y) + 1e-9), x / (np.std(x) + 1e-9), mode="full", method="fft")
    lags = correlation_lags(len(y), len(x), mode="full")
    lag = int(lags[np.argmax(np.abs(corr))])               # y[n] = x[n - lag]

    n0, n1 = max(0, lag), min(len(y), len(x) + lag)
    if n1 - n0 < 100:
        raise ValueError(f"{tr.id}: el .mseed y el .anc casi no se solapan")
    x_al, y_al = x[n0 - lag: n1 - lag], y[n0:n1]
    escala, offset = np.linalg.lstsq(np.vstack([x_al, np.ones_like(x_al)]).T, y_al, rcond=None)[0]
    r2 = 1 - np.var(y_al - (escala * x_al + offset)) / np.var(y_al)
    inicio_anc = tr.stats.starttime.datetime.replace(tzinfo=timezone.utc) - timedelta(seconds=lag * dt_anc)
    return {"lag": lag, "escala": escala, "offset": offset, "r2": r2, "inicio_anc_implicito": inicio_anc}


def _alinear_canales(trazas):
    """Recorta los canales {comp: Trace} a su tramo común, quita la mediana y devuelve (t_utc, cuentas (N,3), dt)."""
    tasas = {round(tr.stats.sampling_rate, 6) for tr in trazas.values()}
    if len(tasas) > 1:
        raise ValueError(f"los canales tienen frecuencias distintas: {tasas}")
    dt = 1.0 / tasas.pop()
    inicio = max(tr.stats.starttime for tr in trazas.values())
    fin    = min(tr.stats.endtime for tr in trazas.values())
    n = int(np.floor((fin - inicio) / dt)) + 1
    columnas = []
    for comp in LABELS:
        tr = trazas[comp]
        x = tr.data.astype(np.float64)
        i0 = int(round((inicio - tr.stats.starttime) / dt))
        columnas.append((x - np.median(x))[i0:i0 + n])
    n = min(len(c) for c in columnas)
    return _eje_tiempo(np.datetime64(inicio.datetime, "us"), n, dt), np.column_stack([c[:n] for c in columnas]), dt


def cargar_estacion(cfg, st_sgc, dir_sgc, anc_inicio_offset_s=ANC_INICIO_OFFSET_S, avisos=None):
    """
    Carga una estación con la mejor fuente disponible (§1.7 de la plantilla): .mseed calibrado con el .anc,
    solo .anc o solo .mseed. Retorna un dict con nombre, codigo, red, loc, color, id, slug, fuente, unidad
    ('cm/s²' o 'cuentas'), calibrada, t_utc, datos (N,3) [EW, NS, VER], dt, header (del .anc o None),
    calibracion (por componente o None) e inicio_anc_real.
    """
    est = {k: cfg[k] for k in ("nombre", "codigo", "red", "loc")}
    est["color"] = cfg.get("color", "#9E9E9E")
    est["id"]    = f"{cfg['red']}.{cfg['codigo']}.{cfg['loc']}"
    est["slug"]  = f"{slug(cfg['nombre'])}_{cfg['codigo']}_{cfg['loc']}"
    pedida = cfg.get("fuente", "auto")
    if pedida not in ("auto", "mseed", "anc"):
        raise ValueError(f"fuente '{pedida}' no es 'auto', 'mseed' ni 'anc'")

    ruta_anc = buscar_anc(dir_sgc, cfg["codigo"], cfg["loc"], avisos)
    st = st_sgc.select(network=cfg["red"], station=cfg["codigo"], location=cfg["loc"], channel=f"{BANDA_SGC}?")
    trazas = {}
    for comp in LABELS:
        sel = st.select(channel=f"{BANDA_SGC}{CANAL_POR_COMP[comp]}")
        if len(sel):
            trazas[comp] = sel[0]
    hay_mseed = len(trazas) == len(LABELS)

    header = anc_cms2 = None
    if ruta_anc is not None:
        header, anc_cms2, _, _, _ = parse_anc(ruta_anc)
    est.update(header=header, ruta_anc=ruta_anc, calibracion=None, inicio_anc_real=None)

    if pedida == "mseed" and not hay_mseed:
        raise FileNotFoundError(f"se pidió .mseed pero no hay canales {BANDA_SGC}* de {est['id']} en {dir_sgc}")
    if pedida == "anc" and anc_cms2 is None:
        raise FileNotFoundError(f"se pidió .anc pero no hay *_{cfg['codigo']}_{cfg['loc']}.anc en {dir_sgc}")
    if not hay_mseed and anc_cms2 is None:
        raise FileNotFoundError(f"no hay *_{cfg['codigo']}_{cfg['loc']}.anc ni canales {BANDA_SGC}* de {est['id']} en {dir_sgc}")

    usar_mseed = hay_mseed and pedida in ("auto", "mseed")
    escalas, fuente = None, None
    if usar_mseed:
        escala_fija = cfg.get("escala_cms2_por_cuenta")
        if escala_fija is not None:
            escalas = escala_fija if isinstance(escala_fija, dict) else {c: float(escala_fija) for c in LABELS}
            fuente = ".mseed (escala fija de la configuración)"
        elif anc_cms2 is not None and pedida == "auto":
            dt_anc = header["sample_dt"]
            if all(abs(tr.stats.sampling_rate * dt_anc - 1) < 1e-6 for tr in trazas.values()):
                cal = {c: calibrar_canal_mseed(trazas[c], anc_cms2[:, i], dt_anc) for i, c in enumerate(LABELS)}
                escalas = {c: r["escala"] for c, r in cal.items()}
                inicios = sorted(a_datetime64(r["inicio_anc_implicito"]) for r in cal.values())
                est["calibracion"], est["inicio_anc_real"] = cal, inicios[len(inicios) // 2]
                fuente = ".mseed calibrado con .anc"
                if min(r["r2"] for r in cal.values()) < 0.9:
                    _avisar(avisos, f"{est['nombre']}: R² de calibración bajo ({min(r['r2'] for r in cal.values()):.3f})")
            else:
                _avisar(avisos, f"{est['nombre']}: el .mseed y el .anc tienen distinta frecuencia; se usa el .anc")
                usar_mseed = False
        else:
            fuente = ".mseed sin calibrar (cuentas)"

    if usar_mseed:
        t, cuentas, dt = _alinear_canales(trazas)
        if escalas is None:
            datos, unidad = cuentas, "cuentas"
        else:
            datos, unidad = np.column_stack([cuentas[:, i] * escalas[c] for i, c in enumerate(LABELS)]), "cm/s²"
    else:
        if cfg.get("inicio_anc_utc"):
            inicio, como = a_datetime64(cfg["inicio_anc_utc"], "UTC"), "configuración"
        else:
            inicio = a_datetime64(header["evento_datetime_utc"]) + np.timedelta64(int(anc_inicio_offset_s * 1e6), "us")
            como = f"origen {anc_inicio_offset_s:+g} s, estimado"
        dt = header["sample_dt"]
        t, datos, unidad = _eje_tiempo(inicio, len(anc_cms2), dt), anc_cms2, "cm/s²"
        fuente = f".anc (inicio: {como})"
        est["inicio_anc_real"] = inicio

    est.update(fuente=fuente, unidad=unidad, calibrada=(unidad == "cm/s²"), t_utc=t, datos=datos, dt=dt)
    return est


def cargar_estaciones(estaciones_sgc, st_sgc, dir_sgc, anc_inicio_offset_s=ANC_INICIO_OFFSET_S, avisos=None):
    """cargar_estacion() de cada estación de `estaciones_sgc`, con un resumen impreso. Omite las que fallan."""
    estaciones = []
    for cfg in estaciones_sgc:
        try:
            est = cargar_estacion(cfg, st_sgc, dir_sgc, anc_inicio_offset_s, avisos)
        except Exception as e:
            print(f"[AVISO] {cfg['nombre']} ({cfg['codigo']}) omitida: {e}\n")
            if avisos is not None:
                avisos.append(f"{cfg['nombre']} omitida: {e}")
            continue
        estaciones.append(est)
        h = est["header"]
        print(f"{est['nombre']} ({est['id']})")
        print(f"  fuente   : {est['fuente']}   unidades: {est['unidad']}")
        print(f"  registro : {est['t_utc'][0]} -> {est['t_utc'][-1]} UTC  ({len(est['t_utc'])} muestras, {1 / est['dt']:.0f} Hz)")
        if h:
            print(f"  distancia: {h['distancia_epicentral']} km epicentral, {h['distancia_hipocentral']} km hipocentral")
        if est["calibracion"]:
            for comp in ORDEN_FILAS:
                c = est["calibracion"][comp]
                print(f"  {BANDA_SGC}{CANAL_POR_COMP[comp]}: escala = {c['escala']:.4e} cm/s² por cuenta   R² = {c['r2']:.5f}")
        if est["inicio_anc_real"] is not None:
            print(f"  inicio real del .anc: {est['inicio_anc_real']} UTC")
        print()
    return estaciones


def datos_evento(estaciones, origen_utc=None, magnitud=None, titulo_ubicacion=None, sismo=""):
    """
    (origen, ubicacion, magnitud) del evento: la hora de origen en datetime64 UTC (o None) y los textos de
    ubicación y magnitud de los títulos. Lo que no se dé se toma de la cabecera del primer .anc cargado.
    """
    hdr = next((e["header"] for e in estaciones if e["header"]), None)
    if origen_utc:
        origen = a_datetime64(origen_utc, "UTC")
    elif hdr:
        origen = a_datetime64(hdr["evento_datetime_utc"])
    else:
        origen = None
    mag = magnitud if magnitud is not None else (hdr["magnitud"] if hdr else None)
    ubicacion = f"Terremoto — {titulo_ubicacion or (hdr['descripcion_evento'] if hdr else sismo)}"
    return origen, ubicacion, (f"Magnitud {mag}" if mag is not None else "")


def buscar_estacion(nombre, estaciones):
    """La estación cargada con ese nombre o código (sin distinguir mayúsculas)."""
    for e in estaciones:
        if str(nombre).lower() in (e["nombre"].lower(), e["codigo"].lower()):
            return e
    raise KeyError(f"estación '{nombre}' no cargada; disponibles: {[e['nombre'] for e in estaciones]}")


# ==========================================================================================================
# Ventanas de tiempo y unidades
# ==========================================================================================================
def resolver_ventana(ventana, recon, estaciones=(), origen_utc=None):
    """
    (t0, t1) datetime64[us] UTC de una ventana (§1.6 de la plantilla). `recon` es la reconstrucción del
    Acelerógrafo UdeA, `estaciones` las estaciones que se grafican y `origen_utc` la hora de origen del sismo.
    La hora local de una ventana manual usa el mismo desfase con que se leyeron los .bin.
    """
    modo = ventana["modo"]
    t_acel = recon["t_utc"]
    if modo == "hora_completa" or (modo == "estacion" and not estaciones):
        t0, t1 = t_acel[0], t_acel[-1]
    elif modo == "estacion":
        t0 = max(e["t_utc"][0] for e in estaciones)
        t1 = min(e["t_utc"][-1] for e in estaciones)
    elif modo == "primera_alerta":
        if not recon["alert_windows"]:
            raise ValueError("modo 'primera_alerta': el Acelerógrafo UdeA no tiene alertas")
        t0 = a_datetime64(recon["alert_windows"][0][0])
        t1 = min(e["t_utc"][-1] for e in estaciones) if estaciones else t_acel[-1]
    elif modo == "relativa_origen":
        if origen_utc is None:
            raise ValueError("modo 'relativa_origen': no se conoce la hora de origen (ORIGEN_UTC en la configuración)")
        t0 = origen_utc - np.timedelta64(int(ventana.get("antes_s", 60) * 1e6), "us")
        t1 = origen_utc + np.timedelta64(int(ventana.get("despues_s", 300) * 1e6), "us")
    elif modo == "manual":
        zona = ventana.get("zona", "LOCAL")
        t0 = a_datetime64(ventana["inicio"], zona, recon["tz_offset_horas"])
        t1 = a_datetime64(ventana["fin"], zona, recon["tz_offset_horas"])
    else:
        raise ValueError(f"modo de ventana desconocido: {modo}")
    if not t0 < t1:
        raise ValueError(f"ventana '{nombre_ventana(ventana)}' vacía: {t0} -> {t1}")
    return t0, t1


def recortar(t, datos, t0=None, t1=None):
    m = np.ones(len(t), dtype=bool)
    if t0 is not None:
        m &= t >= t0
    if t1 is not None:
        m &= t <= t1
    return t[m], datos[m]


def convertir(datos, origen, destino):
    """Pasa `datos` de la unidad `origen` a `destino` ('g' o 'cm/s²')."""
    if origen == destino:
        return datos
    if (origen, destino) == ("g", "cm/s²"):
        return datos * G_TO_CMS2
    if (origen, destino) == ("cm/s²", "g"):
        return datos / G_TO_CMS2
    raise ValueError(f"no se puede convertir de {origen} a {destino}")


# ==========================================================================================================
# Piezas comunes de las gráficas en formato SGC
# ==========================================================================================================
def alertas_en_rango(alert_windows, t_min, t_max):
    """Alertas [(inicio, fin, score)] en datetime64 UTC, recortadas a [t_min, t_max]."""
    salida = []
    for a0, a1, score in alert_windows or []:
        a0, a1 = max(a_datetime64(a0), t_min), min(a_datetime64(a1), t_max)
        if a0 < a1:
            salida.append((a0, a1, score))
    return salida


def encabezado(fig, ubicacion, magnitud, estaciones, linea_tiempo, linea_parametros):
    """Encabezado de 5 líneas (fig.text con va="top"; un suptitle real desordena tight_layout)."""
    fig.text(0.5, 0.995, ubicacion, ha="center", va="top", fontsize=12, fontweight="bold")
    if magnitud:
        fig.text(0.5, 0.950, magnitud, ha="center", va="top", fontsize=10, fontweight="bold")
    if estaciones:
        fig.text(0.5, 0.910, estaciones, ha="center", va="top", fontsize=9)
    fig.text(0.5, 0.874, linea_tiempo, ha="center", va="top", fontsize=8, color="dimgray")
    fig.text(0.5, 0.840, linea_parametros, ha="center", va="top", fontsize=7.5, style="italic", color="dimgray")


def casilla_ids(ax, ids, n_trazas):
    """Casilla con los códigos Red.Estación.Loc.Canal en la esquina superior izquierda del panel."""
    fuente_id, interlineado = (7.5, 1.5) if n_trazas <= 2 else (6.5, 1.3)
    ax.text(0.012, 0.94, "\n".join(ids), transform=ax.transAxes, va="top", ha="left", fontsize=fuente_id,
            family="monospace", linespacing=interlineado,
            bbox=dict(boxstyle="square,pad=0.3", fc="white", ec="black", linewidth=0.8))


def mostrar_o_cerrar(fig, mostrar):
    if mostrar:
        plt.show()
    else:
        plt.close(fig)
