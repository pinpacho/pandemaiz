"""
main.py — Punto de entrada de la API PANdeMaiz Quake.

Lifespan:
  - Inicializa Firebase Admin SDK.
  - Arranca el listener de streaming en /alertas (hilo daemon).
  - Al recibir una alerta, la procesa en el motor de consenso; si se
    alcanza quórum de estaciones, dispara notificaciones multicanal.

Rutas registradas:
  GET /api/stations                                     → lista de estaciones con estado
  GET /api/alerts                                       → últimas alertas sísmicas
  GET /download                                         → proxy + conversión .bin del ESP32
  GET /api/v1/estaciones/{id}/disponibilidad?fecha=...  → horas con datos (Fase 5)
  GET /api/v1/download-hour?...                         → ZIP de una hora (Fase 5)
  GET /                                                 → dashboard Leaflet.js (estáticos)
"""

import asyncio
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from .consensus import engine
from .firebase import init_firebase, start_listener
from .notifier import fire_global_alert
from .routers import alerts, download, history, stations

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-8s  %(name)s — %(message)s",
)
logger = logging.getLogger(__name__)


async def _on_alert(station_id: str, alert_data: dict) -> None:
    """Callback invocado por el listener de Firebase para cada alerta nueva."""
    fired, active_stations = await engine.record_alert(station_id, alert_data)
    if fired:
        await fire_global_alert(active_stations, alert_data)


@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("Iniciando PANdeMaiz Quake Backend…")
    init_firebase()
    loop = asyncio.get_event_loop()
    start_listener(loop, _on_alert)
    logger.info("Listener Firebase activo.")
    yield
    logger.info("Backend detenido.")


app = FastAPI(
    title="PANdeMaiz Quake API",
    description="Backend de la red acelerográfica distribuida PANdeMaiz — UdeA / GICM",
    version="1.0.0",
    lifespan=lifespan,
)

app.include_router(stations.router)
app.include_router(alerts.router)
app.include_router(download.router)
app.include_router(history.router)

# Los logos se sirven desde /logos (mount antes del catch-all /).
app.mount("/logos", StaticFiles(directory="logos"), name="logos")

# El mount de archivos estáticos debe ir al final: captura todo lo que
# no coincida con las rutas de API anteriores.
app.mount("/", StaticFiles(directory="static", html=True), name="static")
