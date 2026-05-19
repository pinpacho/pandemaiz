# Backend — PANdeMaiz Quake API

Servidor FastAPI que conecta la red de nodos ESP32 con los canales de alerta y el dashboard. Escucha en tiempo real los eventos de Firebase RTDB, evalúa el quórum de estaciones mediante un motor de consenso cooperativo y dispara alertas multicanal de forma asíncrona.

---

## Arquitectura

```
Nodos ESP32  ──(Wi-Fi)──►  Firebase RTDB /alertas
                                    │
                         SSE listener (hilo daemon)
                                    │
                         asyncio.run_coroutine_threadsafe()
                                    │
                         ConsensusEngine.record_alert()
                                    │
                    ┌───────────────┼───────────────┐
                    │               │               │
               < MIN_STATIONS  ≥ MIN_STATIONS  Cooldown activo
                (pre-alerta)   en WINDOW_S      (ignorar)
                                    │
                         fire_global_alert()
                                    │
              ┌──────────────┬──────┴──────┐
              │              │             │
           Email           Discord      Telegram
          (SMTP)         (Webhook)    (Bot API)
              └──────────────┴──────┬──────┘
                                    │
                         asyncio.gather() — paralelo

Rutas HTTP:
  GET /api/stations                              → snapshot /estaciones
  GET /api/alerts?limit=50                       → últimas alertas
  GET /download?station_id=&file=&format=        → proxy ESP32 → bin/anc/mseed
  GET /api/v1/estaciones/{id}/disponibilidad     → horas con datos
  GET /api/v1/download-hour?...                  → ZIP de una hora
  GET /                                          → Dashboard Leaflet.js
  GET /logos/*                                   → Assets de marca
```

---

## Motor de Consenso Cooperativo

### El problema

Un solo nodo puede disparar una alerta por vibración local (camión, construcción, interferencia). Para reducir falsas alarmas, el sistema **exige que al menos `MIN_STATIONS` estaciones distintas reporten un sismo dentro de `CONSENSUS_WINDOW_S` segundos**.

### Algoritmo

```
Para cada alerta entrante de station_id:
  1. Registrar station_id → timestamp_utc en el diccionario _recent
  2. Eliminar entradas con timestamp < now − CONSENSUS_WINDOW_S
  3. Contar estaciones activas n = len(_recent)
  4. Si n ≥ MIN_STATIONS  Y  cooldown_ok:
       → _last_fired = now
       → retornar (True, lista_de_estaciones)  # dispara alerta global
  5. Si no: retornar (False, [])              # pre-alerta, esperar más nodos
```

El acceso al estado es protegido por `asyncio.Lock()` — el engine es seguro para uso concurrente en el loop de FastAPI.

### Parámetros de consenso

| Variable de entorno | Valor por defecto | Descripción |
|--------------------|-------------------|-------------|
| `CONSENSUS_WINDOW_S` | 30 | Ventana temporal (s) para agregar alertas de distintas estaciones |
| `MIN_STATIONS` | 2 | Mínimo de estaciones para disparar alerta global |
| `GLOBAL_ALERT_COOLDOWN_S` | 300 | Intervalo mínimo entre alertas globales consecutivas (5 min) |

---

## Estructura RTDB Firebase

```
/estaciones/{station_id}/
    status/
        ip          ← IP local del ESP32 (para proxy de descarga)
        estado      ← "Conectada" / "Desconectada"
        lat, lon    ← coordenadas GPS del nodo
        timestamp   ← último heartbeat (Unix)
    logs_acel/{YYYY}/{YYYY-MM}/{YYYY-MM-DD}/{HH}/{filename}/
        archivo     ← ruta en SD: /Aceleraciones/...
        url         ← http://{ip}/download?f=...
        lat, lon    ← coordenadas en el momento de captura
        timestamp
    logs_eventos/{YYYY}/{YYYY-MM}/{YYYY-MM-DD}/{HH}/{filename}/
        archivo, url, lat, lon, timestamp, score

/alertas/{station_id}/{YYYY}/{YYYY-MM}/{YYYY-MM-DD}/{HH}/{ts}/
    score       ← CNN score (float, 0–1)
    lat, lon    ← coordenadas del nodo que detectó el evento
    estacion    ← station_id
    timestamp
    ip
```

---

## Canales de notificación

Todos los canales son **opcionales**. Si las credenciales no están configuradas, el canal se omite silenciosamente (sin error).

### Email (SMTP + STARTTLS)

```env
SMTP_HOST=smtp.gmail.com
SMTP_PORT=587
SMTP_USER=alertas@ejemplo.com
SMTP_PASS=xxxx-xxxx-xxxx-xxxx   # Contraseña de aplicación Gmail
SMTP_RECIPIENTS=admin@ejemplo.com,otro@ejemplo.com
```

Para Gmail: habilitar **Contraseñas de aplicación** en Configuración → Seguridad → Verificación en 2 pasos.

### Discord

```env
DISCORD_WEBHOOK_URL=https://discord.com/api/webhooks/ID/TOKEN
```

Crear webhook: Canal → Editar → Integraciones → Webhooks → Nuevo webhook.

### Telegram

```env
TELEGRAM_BOT_TOKEN=123456:ABCdef...
TELEGRAM_CHAT_ID=@pandemaiz_alertas   # o ID numérico del canal
```

Pasos:
1. Crear bot con `@BotFather` → `/newbot` → copiar token.
2. Crear canal público y añadir el bot como administrador con permiso **Publicar mensajes**.

---

## Variables de entorno completas

| Variable | Obligatoria | Descripción |
|----------|-------------|-------------|
| `FIREBASE_CREDENTIALS_PATH` | Sí | Ruta al JSON de cuenta de servicio |
| `RTDB_URL` | Sí | `https://PROJECT_ID-default-rtdb.firebaseio.com/` |
| `SMTP_USER` | No | Usuario SMTP para alertas Email |
| `SMTP_PASS` | No | Contraseña de aplicación |
| `SMTP_RECIPIENTS` | No | CSV de destinatarios |
| `DISCORD_WEBHOOK_URL` | No | URL del webhook de Discord |
| `TELEGRAM_BOT_TOKEN` | No | Token del bot de Telegram |
| `TELEGRAM_CHAT_ID` | No | ID o @nombre del canal |
| `CONSENSUS_WINDOW_S` | No | Ventana de consenso en segundos (default: 30) |
| `MIN_STATIONS` | No | Mínimo de estaciones (default: 2) |
| `GLOBAL_ALERT_COOLDOWN_S` | No | Cooldown entre alertas (default: 300) |

---

## Despliegue con Docker

```bash
cd backend

# 1. Copiar el JSON de cuenta de servicio de Firebase
#    (Firebase Console → Configuración → Cuentas de servicio → Generar clave)
cp /ruta/a/firebase-adminsdk.json ./firebase.json

# 2. Configurar credenciales
cp .env.example .env
# Editar .env con RTDB_URL y las credenciales de los canales deseados

# 3. Construir y levantar
docker-compose up --build

# El servidor queda disponible en http://localhost:8000
# Dashboard: http://localhost:8000/
# API docs:  http://localhost:8000/docs
```

El `docker-compose.yml` monta `firebase.json` como volumen de solo lectura en `/run/secrets/firebase.json` — asegurarse de que `FIREBASE_CREDENTIALS_PATH=/run/secrets/firebase.json` en `.env`.

---

## Ejecución local (sin Docker)

```bash
cd backend
pip install -r requirements-backend.txt   # o: uv pip install -r requirements-backend.txt

cp .env.example .env                       # completar credenciales
cp /ruta/a/firebase-adminsdk.json ./firebase.json

uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
```

---

## Descripción de módulos

| Archivo | Responsabilidad |
|---------|----------------|
| `app/main.py` | FastAPI app, lifespan (init Firebase + listener), montaje de rutas y estáticos |
| `app/consensus.py` | `ConsensusEngine` — lógica de quórum, lock asíncrono |
| `app/firebase.py` | `init_firebase()`, `start_listener()` (hilo daemon), helpers de snapshot y consulta histórica |
| `app/notifier.py` | `fire_global_alert()` — Email, Discord, Telegram en paralelo con `asyncio.gather()` |
| `app/converter.py` | `parse_bin()`, `bin_to_anc()`, `bin_to_mseed()` — conversión en memoria |
| `app/config.py` | `Settings` (pydantic-settings) — todas las variables de entorno con defaults |
| `app/routers/stations.py` | `GET /api/stations` |
| `app/routers/alerts.py` | `GET /api/alerts` |
| `app/routers/download.py` | `GET /download` — proxy + conversión individual |
| `app/routers/history.py` | `GET /api/v1/estaciones/{id}/disponibilidad`, `GET /api/v1/download-hour` |
