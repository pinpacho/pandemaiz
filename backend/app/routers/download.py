"""
download.py — Proxy de descarga + conversión de archivos .bin del ESP32.

GET /download?station_id=PAN_A1B2&file=/Eventos/...&format=bin|mseed|anc

Flujo:
  1. Lee la IP local de la estación desde Firebase RTDB.
  2. Hace proxy HTTP al servidor web del ESP32 (GET /download?f=<path>).
  3. Convierte en memoria al formato solicitado con converter.py.
"""

import logging

import httpx
from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import Response

from ..converter import bin_to_anc, bin_to_mseed
from ..firebase import get_station_ip

logger = logging.getLogger(__name__)
router = APIRouter()


@router.get("/download")
async def download_file(
    station_id: str = Query(..., description="ID de la estación, ej. PAN_A1B2C3"),
    file: str = Query(..., description="Ruta del archivo en la SD, ej. /Eventos/2026/..."),
    format: str = Query(default="bin", description="Formato de salida: bin | mseed | anc"),
) -> Response:
    # Prevención de path traversal
    if ".." in file:
        raise HTTPException(status_code=403, detail="Ruta no permitida")

    valid_formats = {"bin", "mseed", "anc"}
    if format not in valid_formats:
        raise HTTPException(
            status_code=400,
            detail=f"Formato '{format}' no válido. Usar: {', '.join(valid_formats)}",
        )

    ip = get_station_ip(station_id)
    if not ip:
        raise HTTPException(
            status_code=404,
            detail=f"Estación '{station_id}' no encontrada o sin IP registrada en Firebase",
        )

    esp32_url = f"http://{ip}/download?f={file}"
    logger.info("Proxy descarga: %s → %s", station_id, esp32_url)

    try:
        async with httpx.AsyncClient(timeout=30) as client:
            resp = await client.get(esp32_url)
    except httpx.ConnectError:
        raise HTTPException(status_code=502, detail=f"No se pudo conectar al ESP32 en {ip}")
    except httpx.TimeoutException:
        raise HTTPException(status_code=504, detail="Timeout descargando del ESP32")

    if resp.status_code != 200:
        raise HTTPException(
            status_code=502,
            detail=f"ESP32 respondió HTTP {resp.status_code}",
        )

    raw = resp.content
    filename = file.split("/")[-1]

    if format == "bin":
        return Response(
            content=raw,
            media_type="application/octet-stream",
            headers={"Content-Disposition": f'attachment; filename="{filename}"'},
        )

    if format == "mseed":
        try:
            data = bin_to_mseed(raw, station_id, filename)
        except ValueError as exc:
            raise HTTPException(status_code=422, detail=str(exc))
        out_name = filename.replace(".bin", ".mseed")
        return Response(
            content=data,
            media_type="application/octet-stream",
            headers={"Content-Disposition": f'attachment; filename="{out_name}"'},
        )

    # format == "anc"
    try:
        text = bin_to_anc(raw, station_id, filename)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc))
    out_name = filename.replace(".bin", ".anc")
    return Response(
        content=text.encode("utf-8"),
        media_type="text/plain; charset=utf-8",
        headers={"Content-Disposition": f'attachment; filename="{out_name}"'},
    )
