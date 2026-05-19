"""
history.py — Descarga histórica por hora con empaquetado ZIP en memoria.

Endpoints:
  GET /api/v1/estaciones/{station_id}/disponibilidad?fecha=YYYY-MM-DD
      → lista de horas (HH) que tienen datos en logs_acel

  GET /api/v1/download-hour?station_id=&date=&hour=&format=
      → ZIP en memoria con todos los archivos de esa hora, convertidos al vuelo

Flujo del ZIP:
  1. Leer metadata de Firebase (logs_acel) para la estación/fecha/hora.
  2. Para cada archivo: GET http://{ip}/download?f={path} con timeout 20 s.
     Si falla → log warning + continuar (resiliencia de red).
  3. Convertir al vuelo según format: bin (raw) / anc (texto) / mseed (ObsPy).
  4. Escribir en io.BytesIO con zipfile.ZipFile (sin tocar disco).
  5. Devolver como Response con Content-Type application/zip.
"""

import io
import logging
import re
import zipfile

import httpx
from fastapi import APIRouter, HTTPException, Path, Query
from fastapi.responses import Response

from ..converter import bin_to_anc, bin_to_mseed
from ..firebase import get_available_hours, get_files_for_hour, get_station_ip

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/api/v1")

_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_HOUR_RE = re.compile(r"^\d{2}$")

VALID_FORMATS = {"bin", "anc", "mseed"}
# Timeout por archivo individual; si el ESP32 tarda más se omite ese minuto.
FILE_TIMEOUT_S = 20


# ── Endpoint 1: disponibilidad ────────────────────────────────────────────────

@router.get(
    "/estaciones/{station_id}/disponibilidad",
    summary="Horas con datos disponibles para una fecha",
    response_description="Lista ordenada de horas HH con archivos en logs_acel",
)
async def disponibilidad(
    station_id: str = Path(..., description="ID de la estación, ej. PANUDEA01"),
    fecha: str = Query(..., description="Fecha en formato YYYY-MM-DD"),
) -> list[str]:
    if not _DATE_RE.match(fecha):
        raise HTTPException(status_code=400, detail="Formato de fecha inválido. Usar YYYY-MM-DD")

    hours = get_available_hours(station_id, fecha)
    return hours


# ── Endpoint 2: descarga masiva en ZIP ───────────────────────────────────────

@router.get(
    "/download-hour",
    summary="Descarga ZIP con todos los archivos de una hora específica",
    response_description="Archivo .zip con los minutos de la hora en el formato elegido",
)
async def download_hour(
    station_id: str = Query(..., description="ID de la estación"),
    date: str = Query(..., description="Fecha YYYY-MM-DD"),
    hour: str = Query(..., description="Hora HH (00-23)"),
    format: str = Query(default="bin", description="Formato de salida: bin | anc | mseed"),
) -> Response:
    # ── Validaciones ──────────────────────────────────────────────────────────
    if not _DATE_RE.match(date):
        raise HTTPException(400, "Formato de fecha inválido. Usar YYYY-MM-DD")
    if not _HOUR_RE.match(hour):
        raise HTTPException(400, "Hora inválida. Usar HH (ej. 00, 13, 23)")
    if format not in VALID_FORMATS:
        raise HTTPException(400, f"Formato '{format}' no válido. Usar: {', '.join(VALID_FORMATS)}")

    # ── Obtener IP actual de la estación ──────────────────────────────────────
    ip = get_station_ip(station_id)
    if not ip:
        raise HTTPException(404, f"Estación '{station_id}' sin IP registrada en Firebase")

    # ── Obtener metadatos de archivos para esa hora ───────────────────────────
    files_meta = get_files_for_hour(station_id, date, hour)
    if not files_meta:
        raise HTTPException(
            404,
            f"No hay archivos en logs_acel para {station_id} / {date} / {hour}h",
        )

    ext = "mseed" if format == "mseed" else format
    zip_name = f"PANdeMaiz_{station_id}_{date}_{hour}h.zip"

    # ── Construcción del ZIP en memoria ───────────────────────────────────────
    buf = io.BytesIO()
    downloaded = 0
    errors = 0

    async with httpx.AsyncClient(timeout=FILE_TIMEOUT_S) as client:
        with zipfile.ZipFile(buf, mode="w", compression=zipfile.ZIP_DEFLATED) as zf:
            for meta in files_meta:
                filepath: str = meta.get("archivo", "")
                if not filepath or ".." in filepath:
                    continue

                filename = filepath.split("/")[-1]
                esp32_url = f"http://{ip}/download?f={filepath}"

                # Descarga individual con resiliencia: si falla, continúa con el siguiente
                try:
                    resp = await client.get(esp32_url)
                    resp.raise_for_status()
                    raw: bytes = resp.content
                except httpx.TimeoutException:
                    logger.warning("Timeout descargando %s — omitido", filename)
                    errors += 1
                    continue
                except Exception as exc:
                    logger.warning("Error descargando %s: %s — omitido", filename, exc)
                    errors += 1
                    continue

                # Conversión al vuelo
                try:
                    if format == "bin":
                        zf.writestr(filename, raw)

                    elif format == "anc":
                        anc_text = bin_to_anc(raw, station_id, filename)
                        out_name = filename.replace(".bin", ".anc")
                        zf.writestr(out_name, anc_text.encode("utf-8"))

                    else:  # mseed
                        mseed_bytes = bin_to_mseed(raw, station_id, filename)
                        out_name = filename.replace(".bin", ".mseed")
                        zf.writestr(out_name, mseed_bytes)

                    downloaded += 1

                except Exception as exc:
                    logger.warning("Error convirtiendo %s: %s — omitido", filename, exc)
                    errors += 1

    logger.info(
        "ZIP listo: %s — %d archivos OK, %d errores (%.1f KB)",
        zip_name,
        downloaded,
        errors,
        buf.tell() / 1024,
    )

    if downloaded == 0:
        raise HTTPException(
            502,
            "No se pudo descargar ningún archivo del ESP32. "
            "Verifica que la estación esté encendida y accesible.",
        )

    buf.seek(0)
    return Response(
        content=buf.read(),
        media_type="application/zip",
        headers={
            "Content-Disposition": f'attachment; filename="{zip_name}"',
            "X-Files-Downloaded": str(downloaded),
            "X-Files-Errors": str(errors),
        },
    )
