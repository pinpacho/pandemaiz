"""
firebase.py — Inicialización de firebase-admin y listener en tiempo real de /alertas.

El SDK de firebase-admin en Python ejecuta el listener en un hilo de fondo.
Para pasar los eventos al loop asyncio de FastAPI se usa
asyncio.run_coroutine_threadsafe().
"""

import asyncio
import logging
import threading
from typing import Callable, Awaitable

import firebase_admin
from firebase_admin import credentials, db

from .config import settings

logger = logging.getLogger(__name__)

_app: firebase_admin.App | None = None
# Flag que evita procesar el dump inicial completo que Firebase envía al conectar
_listener_initialized = False


def init_firebase() -> None:
    global _app
    cred = credentials.Certificate(settings.firebase_credentials_path)
    _app = firebase_admin.initialize_app(
        cred, {"databaseURL": settings.rtdb_url}
    )
    logger.info("Firebase Admin inicializado. RTDB: %s", settings.rtdb_url)


def start_listener(
    loop: asyncio.AbstractEventLoop,
    on_alert: Callable[[str, dict], Awaitable[None]],
) -> None:
    """
    Abre un SSE stream en /alertas y despacha cada alerta nueva al callback.
    Corre en un hilo daemon para no bloquear el loop de FastAPI.
    """

    def _handle_event(event) -> None:
        global _listener_initialized

        # El primer evento es siempre un put en '/' con todos los datos históricos.
        # Lo descartamos para no re-disparar alertas viejas al reiniciar el servidor.
        if not _listener_initialized:
            _listener_initialized = True
            logger.info("Listener /alertas activo — datos históricos omitidos.")
            return

        if event.data is None or event.event_type not in ("put", "patch"):
            return

        # Ruta relativa al nodo /alertas:
        # /{station_id}/{YYYY}/{YYYY-MM}/{YYYY-MM-DD}/{HH}/{ts}
        parts = event.path.strip("/").split("/")
        if len(parts) < 1 or not parts[0]:
            return

        station_id = parts[0]
        data = event.data

        # Si el payload es el nodo hoja (dict con campo 'score') lo procesamos.
        # Si es un dict anidado, buscamos el primer nodo hoja.
        alert_data = _find_leaf_alert(data)
        if alert_data is None:
            return

        logger.info("[Firebase] Nueva alerta de %s  score=%.2f", station_id, alert_data.get("score", 0))
        asyncio.run_coroutine_threadsafe(on_alert(station_id, alert_data), loop)

    def _start_streaming() -> None:
        db.reference("/alertas").listen(_handle_event)

    t = threading.Thread(target=_start_streaming, daemon=True, name="firebase-listener")
    t.start()


def _find_leaf_alert(data) -> dict | None:
    """Devuelve el primer dict que contenga el campo 'score' (nodo hoja de alerta)."""
    if isinstance(data, dict):
        if "score" in data:
            return data
        for v in data.values():
            result = _find_leaf_alert(v)
            if result:
                return result
    return None


# ── Consultas de snapshot (síncronas, llamar desde endpoints) ─────────────────

def get_all_stations() -> dict:
    """Lee /estaciones y devuelve el snapshot completo."""
    return db.reference("/estaciones").get() or {}


def get_station_ip(station_id: str) -> str | None:
    """Devuelve la IP local de una estación desde /estaciones/{id}/status/ip."""
    return db.reference(f"/estaciones/{station_id}/status/ip").get()


def get_recent_alerts(limit: int = 50) -> list[dict]:
    """Devuelve las últimas `limit` alertas de /alertas, ordenadas por timestamp desc."""
    raw = db.reference("/alertas").get() or {}
    alerts: list[dict] = []
    _flatten_alerts(raw, alerts)
    alerts.sort(key=lambda x: x.get("timestamp", 0), reverse=True)
    return alerts[:limit]


def _flatten_alerts(node, result: list) -> None:
    if isinstance(node, dict):
        if "estacion" in node or "score" in node:
            result.append(node)
        else:
            for v in node.values():
                _flatten_alerts(v, result)


# ── Consultas históricas (Fase 5) ─────────────────────────────────────────────

def get_available_hours(station_id: str, date_str: str) -> list[str]:
    """
    Devuelve las horas (strings "HH") que tienen archivos registrados
    en logs_acel para la estación y fecha dadas.

    Ruta RTDB: /estaciones/{station_id}/logs_acel/{YYYY}/{YYYY-MM}/{date_str}
    """
    year = date_str[:4]
    month = date_str[:7]   # YYYY-MM
    path = f"/estaciones/{station_id}/logs_acel/{year}/{month}/{date_str}"
    node = db.reference(path).get()
    if not isinstance(node, dict):
        return []
    return sorted(node.keys())


def get_files_for_hour(
    station_id: str, date_str: str, hour: str
) -> list[dict]:
    """
    Devuelve la lista de metadatos de archivo para la hora indicada.

    Cada elemento es un dict con campos: archivo, url, lat, lon, timestamp.
    Ruta RTDB: /estaciones/{station_id}/logs_acel/{YYYY}/{YYYY-MM}/{date_str}/{hour}
    """
    year = date_str[:4]
    month = date_str[:7]
    path = f"/estaciones/{station_id}/logs_acel/{year}/{month}/{date_str}/{hour}"
    node = db.reference(path).get()
    if not isinstance(node, dict):
        return []
    return [v for v in node.values() if isinstance(v, dict) and v.get("archivo")]
