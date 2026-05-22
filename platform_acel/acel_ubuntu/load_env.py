"""
load_env.py — PlatformIO pre-build script
Lee el archivo .env en este directorio e inyecta cada variable
como un -D flag de preprocesador C, sobreescribiendo los
placeholders definidos en config.h via #ifndef.

Uso: poner credenciales reales en .env (nunca commitear).
     El .gitignore raíz ya excluye .env y .env.*.
"""
Import("env")  # noqa: F821 — SCons global
import os

_ENV_FILE = os.path.join(env.subst("$PROJECT_DIR"), ".env")  # noqa: F821

if not os.path.isfile(_ENV_FILE):
    print("[load_env] Archivo .env no encontrado "
          "— se usan los placeholders de config.h")
else:
    count = 0
    with open(_ENV_FILE, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            key = key.strip()
            val = val.strip()

            # Valores enteros (ej: MQTT_PORT=1883)   → sin comillas C
            # Valores float   (ej: STATIC_LAT=6.26) → literal con sufijo f
            # Cadenas         (ej: WIFI_SSID=MiRed)  → comillas C escapadas
            try:
                int(val)
                flag = f"-D{key}={val}"
            except ValueError:
                try:
                    float(val)
                    flag = f"-D{key}={val}f"
                except ValueError:
                    escaped = val.replace("\\", "\\\\").replace('"', '\\"')
                    flag = f'-D{key}=\\"{escaped}\\"'

            env.Append(CCFLAGS=[flag])  # noqa: F821
            count += 1

    print(f"[load_env] {count} variable(s) cargadas desde .env")
