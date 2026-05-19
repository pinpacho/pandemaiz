from fastapi import APIRouter, Query

from ..firebase import get_recent_alerts

router = APIRouter()


@router.get("/api/alerts")
async def list_alerts(limit: int = Query(default=50, ge=1, le=200)) -> list[dict]:
    """
    Devuelve las últimas alertas sísmicas registradas en Firebase,
    ordenadas por timestamp descendente.
    """
    return get_recent_alerts(limit=limit)
