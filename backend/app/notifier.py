"""
notifier.py — Envío de notificaciones de Alerta Global por tres canales.

Canales:
  - Email vía SMTP + STARTTLS  (stdlib smtplib, sin dependencias extra)
  - Discord vía Incoming Webhook
  - Telegram Canal vía Bot API  (un solo POST → todos los suscriptores del canal)
"""

import asyncio
import logging
import smtplib
import ssl
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

import httpx

from .config import settings

logger = logging.getLogger(__name__)


# ── Email ─────────────────────────────────────────────────────────────────────

def _smtp_send(msg: MIMEMultipart, recipients: list[str]) -> None:
    ctx = ssl.create_default_context()
    with smtplib.SMTP(settings.smtp_host, settings.smtp_port) as server:
        server.ehlo()
        server.starttls(context=ctx)
        server.login(settings.smtp_user, settings.smtp_pass)
        server.sendmail(settings.smtp_user, recipients, msg.as_string())


async def send_email(subject: str, body: str) -> None:
    if not settings.smtp_user or not settings.smtp_pass:
        logger.debug("Email desactivado (sin SMTP_USER/SMTP_PASS)")
        return

    recipients = [r.strip() for r in settings.smtp_recipients.split(",") if r.strip()]
    if not recipients:
        logger.debug("Email desactivado (SMTP_RECIPIENTS vacío)")
        return

    msg = MIMEMultipart()
    msg["From"] = settings.smtp_user
    msg["To"] = ", ".join(recipients)
    msg["Subject"] = subject
    msg.attach(MIMEText(body, "plain", "utf-8"))

    loop = asyncio.get_event_loop()
    try:
        await loop.run_in_executor(None, _smtp_send, msg, recipients)
        logger.info("Email enviado a %s", recipients)
    except Exception as exc:
        logger.error("Error enviando email: %s", exc)


# ── Discord ───────────────────────────────────────────────────────────────────

async def send_discord(message: str) -> None:
    if not settings.discord_webhook_url:
        logger.debug("Discord desactivado (sin DISCORD_WEBHOOK_URL)")
        return

    try:
        async with httpx.AsyncClient(timeout=10) as client:
            resp = await client.post(
                settings.discord_webhook_url,
                json={"content": message},
            )
            resp.raise_for_status()
        logger.info("Notificación Discord enviada")
    except Exception as exc:
        logger.error("Error enviando Discord: %s", exc)


# ── Telegram ──────────────────────────────────────────────────────────────────

async def send_telegram(message: str) -> None:
    if not settings.telegram_bot_token or not settings.telegram_chat_id:
        logger.debug("Telegram desactivado (sin TOKEN o CHAT_ID)")
        return

    url = f"https://api.telegram.org/bot{settings.telegram_bot_token}/sendMessage"
    try:
        async with httpx.AsyncClient(timeout=10) as client:
            resp = await client.post(
                url,
                json={
                    "chat_id": settings.telegram_chat_id,
                    "text": message,
                    "parse_mode": "Markdown",
                },
            )
            resp.raise_for_status()
        logger.info("Notificación Telegram enviada al canal %s", settings.telegram_chat_id)
    except Exception as exc:
        logger.error("Error enviando Telegram: %s", exc)


# ── Alerta global (dispara los tres canales en paralelo) ──────────────────────

async def fire_global_alert(stations: list[str], alert_data: dict) -> None:
    station_list = ", ".join(stations)
    score = alert_data.get("score", "?")
    lat = alert_data.get("lat", "?")
    lon = alert_data.get("lon", "?")

    email_subject = "ALERTA SISMICA — PANdeMaiz Quake"
    email_body = (
        "ALERTA GLOBAL DETECTADA\n\n"
        f"Estaciones que reportan: {station_list}\n"
        f"Score CNN: {score}\n"
        f"Lat/Lon: {lat}, {lon}\n\n"
        "Sistema: PANdeMaiz Quake — Universidad de Antioquia / GICM\n"
        "Este mensaje fue generado automaticamente."
    )

    discord_msg = (
        "🔴 **ALERTA SÍSMICA** — PANdeMaiz Quake\n"
        f"Estaciones: `{station_list}` · Score CNN: `{score}`\n"
        f"Lat/Lon: `{lat}, {lon}`"
    )

    telegram_msg = (
        "🔴 *ALERTA SÍSMICA* — PANdeMaiz Quake\n"
        f"Estaciones: `{station_list}` · Score CNN: `{score}`\n"
        f"Lat/Lon: `{lat}, {lon}`"
    )

    await asyncio.gather(
        send_email(email_subject, email_body),
        send_discord(discord_msg),
        send_telegram(telegram_msg),
        return_exceptions=True,
    )
    logger.warning("Alerta global disparada — estaciones: %s", station_list)
