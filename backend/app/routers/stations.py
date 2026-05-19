from fastapi import APIRouter

from ..consensus import engine
from ..firebase import get_all_stations

router = APIRouter()


@router.get("/api/stations")
async def list_stations() -> list[dict]:
    """
    Devuelve todas las estaciones registradas en Firebase con su estado actual.
    El campo `alerta` es True si la estación ha reportado alerta en los últimos
    CONSENSUS_WINDOW_S segundos.
    """
    raw = get_all_stations()
    alerted = engine.get_recent_station_ids()
    result = []

    for station_id, node in raw.items():
        status = node.get("status", {}) if isinstance(node, dict) else {}
        result.append(
            {
                "station_id": station_id,
                "ip": status.get("ip", ""),
                "lat": status.get("lat", 0.0),
                "lon": status.get("lon", 0.0),
                "estado": status.get("estado", "Desconocida"),
                "last_seen": status.get("timestamp", 0),
                "alerta": station_id in alerted,
            }
        )

    return result
