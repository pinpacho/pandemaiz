"""
consensus.py — Motor de consenso para alerta global.

Lógica: si ≥ MIN_STATIONS estaciones distintas reportan alerta dentro de
CONSENSUS_WINDOW_S segundos, se dispara una Alerta Global (sujeto al cooldown).

El engine es un singleton de módulo accesible desde main.py y los routers.
"""

import asyncio
import logging
from datetime import datetime, timedelta, timezone

from .config import settings

logger = logging.getLogger(__name__)


class ConsensusEngine:
    def __init__(self, s):
        self._s = s
        self._lock = asyncio.Lock()
        # station_id → datetime_utc del último reporte de alerta
        self._recent: dict[str, datetime] = {}
        self._last_fired: datetime | None = None

    async def record_alert(
        self, station_id: str, alert_data: dict
    ) -> tuple[bool, list[str]]:
        """
        Registra una alerta entrante y evalúa si se debe disparar alerta global.

        Retorna (True, [station_ids]) si se dispara alerta global,
        (False, []) en caso contrario.
        """
        async with self._lock:
            now = datetime.now(timezone.utc)
            cutoff = now - timedelta(seconds=self._s.consensus_window_s)

            # Limpiar estaciones fuera de la ventana de consenso
            self._recent = {k: v for k, v in self._recent.items() if v > cutoff}
            self._recent[station_id] = now

            active = list(self._recent.keys())
            n = len(active)

            cooldown_ok = (
                self._last_fired is None
                or (now - self._last_fired).total_seconds() > self._s.global_alert_cooldown_s
            )

            if n >= self._s.min_stations and cooldown_ok:
                self._last_fired = now
                logger.warning(
                    "[CONSENSO] ALERTA GLOBAL — %d estaciones en %ds: %s",
                    n,
                    self._s.consensus_window_s,
                    active,
                )
                return True, active

        logger.debug(
            "[CONSENSO] Pre-alerta: %d/%d estaciones — %s",
            len(self._recent),
            self._s.min_stations,
            list(self._recent.keys()),
        )
        return False, []

    def get_recent_station_ids(self) -> set[str]:
        """Devuelve las estaciones actualmente en estado de alerta (dentro de la ventana)."""
        now = datetime.now(timezone.utc)
        cutoff = now - timedelta(seconds=self._s.consensus_window_s)
        return {k for k, v in self._recent.items() if v > cutoff}


# Singleton de módulo
engine = ConsensusEngine(settings)
