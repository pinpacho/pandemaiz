from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    # ── Firebase ──────────────────────────────────────────────
    firebase_credentials_path: str = "firebase.json"
    rtdb_url: str = ""

    # ── Email SMTP ────────────────────────────────────────────
    smtp_host: str = "smtp.gmail.com"
    smtp_port: int = 587
    smtp_user: str = ""
    smtp_pass: str = ""
    smtp_recipients: str = ""  # CSV de destinatarios

    # ── Discord ───────────────────────────────────────────────
    discord_webhook_url: str = ""

    # ── Telegram ──────────────────────────────────────────────
    telegram_bot_token: str = ""
    telegram_chat_id: str = ""  # @nombre_canal o ID numérico

    # ── Motor de consenso ─────────────────────────────────────
    consensus_window_s: int = 30        # ventana para agregar alertas
    min_stations: int = 2               # estaciones mínimas para alerta global
    global_alert_cooldown_s: int = 300  # cooldown entre alertas globales

    model_config = {"env_file": ".env"}


settings = Settings()
