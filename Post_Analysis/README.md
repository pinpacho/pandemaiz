# Post_Analysis — Análisis post-captura de registros acelerométricos

Herramienta de análisis **offline** para explorar los archivos `.bin` grabados por los nodos ESP32 del proyecto PANdeMaiz Quake. No requiere conexión a Firebase ni al backend.

---

## Propósito

Una vez descargados los archivos `.bin` de la tarjeta SD (o del backend), este módulo permite:

1. **Inspeccionar** el contenido del archivo: número de muestras, duración, versión, coordenadas GPS (v3).
2. **Convertir** a formatos estándar:
   - **MiniSEED** (`.mseed`) — para análisis con ObsPy, SeisComp, etc.
   - **SGC-ANC** (`.anc`) — compatible con el `seismic_dataset_builder_v3.ipynb`.
3. **Visualizar** con 6 tipos de gráfica por archivo:
   - Serie de tiempo en **g** · eje relativo (desde 0 s)
   - Serie de tiempo en **g** · eje fecha/hora real
   - Serie de tiempo en **cm/s²** · eje relativo
   - Serie de tiempo en **cm/s²** · eje fecha/hora real
   - Espectrogramas STFT en las 3 componentes (EW, NS, VER)
   - Detección STA/LTA con zonas de evento resaltadas
4. **Resumir** estadísticas: duración, PGA por eje, número de triggers detectados.

---

## Estructura de carpetas

```
Post_Analysis/
├── post_analysis.ipynb        ← notebook principal (ejecutar en orden)
├── README.md                  ← este archivo
├── data/
│   ├── aceleraciones/         ← coloca aquí los .bin de grabación continua
│   └── eventos/               ← coloca aquí los .bin de eventos detectados
└── output/                    ← generado automáticamente al ejecutar el notebook
    ├── mseed/                 ← archivos .mseed convertidos
    ├── anc/                   ← archivos .anc convertidos
    └── plots/                 ← figuras PNG guardadas
```

> `data/` y `output/` están en `.gitignore` — los datos crudos y las figuras no se suben al repositorio.

---

## Uso rápido

```bash
# 1. Activar el entorno virtual del proyecto
source pan_env/bin/activate          # desde la raíz del repositorio

# 2. Copiar tus archivos .bin a las carpetas correspondientes
cp /ruta/a/mis/archivos/*.bin Post_Analysis/data/aceleraciones/
cp /ruta/a/eventos/*.bin      Post_Analysis/data/eventos/

# 3. Abrir Jupyter Lab
jupyter lab

# 4. Abrir post_analysis.ipynb y ejecutar todas las celdas en orden (Run All)
```

---

## Nombre de archivo recomendado

Para que el eje de **fecha/hora real** funcione, el nombre del archivo debe contener el patrón `YYYYMMDD_HHMMSS`:

```
PAN01_20260515_143000.bin       ✓
accel_20260515_143000.bin       ✓
evento_20260515_143512.bin      ✓
registro.bin                    ✗  (usa eje relativo como fallback)
```

El firmware del ESP32 genera archivos con este patrón por defecto.

---

## Descripción de cada sección del notebook

| Sección | Contenido |
|---|---|
| §1 | Título, descripción y tabla de mapeo de ejes |
| §2 | Importaciones (numpy, matplotlib, scipy, obspy) |
| §3 | **Configuración** — rutas, station ID, parámetros STA/LTA |
| §4 | `parse_bin()` — lectura del formato binario del ESP32 |
| §5 | `to_mseed()` — conversión a obspy.Stream |
| §6 | `to_anc()` — conversión al formato SGC con cabecera de 20 líneas |
| §7 | `discover_bins()` — listado y tabla de archivos |
| §8 | Conversión en lote (`.bin` → `.mseed` + `.anc`) |
| §9 | Funciones de eje de tiempo (relativo y fecha/hora real) |
| §10 | Funciones de graficación (`plot_timeseries`, `plot_spectrogram`, `plot_sta_lta`) |
| §11–16 | Gráficas de la carpeta **aceleraciones** (6 celdas, un tipo por celda) |
| §17–22 | Gráficas de la carpeta **eventos** (6 celdas, un tipo por celda) |
| §23 | Tabla resumen con PGA, duración, triggers y coordenadas |

---

## Formato `.bin` del ESP32

### Cabecera

| Campo | Tipo | Bytes | Descripción |
|---|---|---|---|
| MAGIC | uint32 LE | 4 | `0xDA7A1345` — identificador del formato |
| VERSION | uint16 LE | 2 | 2 = sin GPS · 3 = con lat/lon |
| SAMPLE_RATE | uint16 LE | 2 | Hz (normalmente 200) |
| lat | float32 LE | 4 | Solo versión 3 |
| lon | float32 LE | 4 | Solo versión 3 |

### Por muestra (10 bytes)

| Campo | Tipo | Bytes | Escala |
|---|---|---|---|
| timestamp_ms | uint32 LE | 4 | ms desde inicio de grabación |
| ax | int16 LE | 2 | 1 LSB = 3.9 mg (EW) |
| ay | int16 LE | 2 | 1 LSB = 3.9 mg (NS) |
| az | int16 LE | 2 | 1 LSB = 3.9 mg (VER) |

### Mapeo de ejes

| Canal firmware | Canal FDSN (MiniSEED) | Canal SGC (`.anc`) | Unidades |
|---|---|---|---|
| ax | HNE | EW (col 0) | g → cm/s² ×980.665 |
| ay | HNN | NS (col 2) | g → cm/s² ×980.665 |
| az | HNZ | VER (col 1) | g → cm/s² ×980.665 |

---

## Parámetros STA/LTA

Los siguientes valores son **idénticos** a los del firmware (`config.h`) y al dataset builder (`seismic_dataset_builder_v3.ipynb`):

| Parámetro | Valor | Equivalencia temporal |
|---|---|---|
| STA | 100 muestras | 0.5 s |
| LTA | 2000 muestras | 10.0 s |
| Umbral ON | 2.5 | — |
| Umbral OFF | 1.5 | — |

---

## Dependencias

Las mismas del entorno principal del proyecto (`pan_env`):

```
numpy
matplotlib
scipy
obspy
```

Instalar si no están:
```bash
uv pip install numpy matplotlib scipy obspy
```

---

## Outputs generados

| Tipo | Ubicación | Descripción |
|---|---|---|
| MiniSEED | `output/mseed/{stem}.mseed` | Stream de 3 trazas (HNE, HNN, HNZ) |
| ANC | `output/anc/{stem}.anc` | Texto SGC con cabecera de 20 líneas |
| PNG | `output/plots/{stem}_*.png` | Figuras de series de tiempo y espectrogramas |

Los archivos `.mseed` pueden abrirse con `obspy.read("output/mseed/archivo.mseed")`.
Los archivos `.anc` son compatibles con `parse_anc()` del `seismic_dataset_builder_v3.ipynb`.

---

## Plantillas para analizar cualquier sismo

Tres notebooks analizan un sismo con el **Acelerógrafo UdeA** (PANUDEA01, Medellín) y las estaciones acelerográficas del **Servicio Geológico Colombiano (SGC)**. Los tres leen los mismos datos, se configuran de la misma forma y toman sus funciones comunes de `sismo_utils.py`:

| Archivo | Qué hace | Salidas |
|---|---|---|
| `plantilla_analisis_sismo.ipynb` | Compara los registros **en el tiempo**: gráficas en formato SGC y animaciones GIF, en g y cm/s², en hora UTC y hora local | `output/<sismo>/` |
| `plantilla_espectros_sismo.ipynb` | Compara los registros **en frecuencia**: espectros de Fourier, espectrogramas y los espectrogramas con que la CNN del nodo disparó cada alerta | `output/<sismo>/espectros/` |
| `mapa_sismo.ipynb` | Mapa de Colombia y mapa regional con el epicentro y las estaciones | `output/<sismo>/mapa/` |
| `sismo_utils.py` | Funciones comunes: lectura de `.bin`, `.anc` y `.mseed`, reconstrucción de la señal, calibración, ventanas de tiempo y formato de las gráficas | — |

Cada notebook tiene **una sola celda de configuración**: se edita y se hace **Run All**. La configuración por defecto es la del sismo del Chocó (2026-08-10, M7.4), así que los tres funcionan tal cual como ejemplo. Cada uno trae además una guía detallada en su sección §1.

### Pasos para un sismo nuevo

1. **Carpetas**: en `plantilla_analisis_sismo.ipynb`, ejecutar `preparar_carpetas_sismo("<sismo>")` (§3).
2. **Datos**: copiar los `.bin` del Acelerógrafo UdeA en `aceleraciones/` y `eventos/`, y los `.anc` / `.mseed` del SGC en `sgc/` (ver *Datos de entrada*).
3. **Análisis en el tiempo**: en la configuración de `plantilla_analisis_sismo.ipynb` (§4), cambiar `SISMO`, borrar las 3 líneas de rutas del Chocó y llenar `ESTACIONES_SGC`, `TITULO_UBICACION` (o `None`), `VENTANAS` y `GRUPOS_ESTACIONES`. Ejecutar hasta §5, que verifica carpetas y configuración, y luego **Run All**. Revisar en §13 la fuente y el R² de cada estación, y en §17 el resumen de PGA y de archivos generados.
4. **Espectros**: en la configuración de `plantilla_espectros_sismo.ipynb` (§3), copiar de la plantilla `SISMO`, las carpetas, `ESTACIONES_SGC`, `TITULO_UBICACION`, `VENTANAS` y `GRUPOS_ESTACIONES`, y revisar las claves espectrales. **Run All** y revisar en §15 las frecuencias pico, la banda útil y los avisos.
5. **Mapa**: en la configuración de `mapa_sismo.ipynb` (§3), llenar `SISMO`, `DIR_SGC`, `EVENTO` y `ESTACIONES`. Las coordenadas del Acelerógrafo UdeA se escriben a mano; lo demás puede quedar en `None` y se lee de los `.anc`. **Run All**.

---

### Datos de entrada

#### Estructura de carpetas

```
data/sismos/<sismo>/
├── aceleraciones/   accel_YYYYMMDD_HHMMSS.bin        grabación continua del Acelerógrafo UdeA
├── eventos/         evento_YYYYMMDD_HHMMSS_sNN.bin   alertas del detector (sNN = puntaje)
└── sgc/             .anc y .mseed de cada estación, tal como se descargan
output/<sismo>/      lo generan los notebooks (no poner nada aquí)
```

- `<sismo>` es un nombre corto, sin espacios ni tildes (p. ej. `choco_2026-08-10`). Es el valor de `SISMO` en los tres notebooks y el nombre de la carpeta de salida.
- Los datos del Chocó están en una ubicación anterior a esta estructura (`data/aceleraciones/terremoto_choco/`, `data/eventos/terremoto_choco/` y `data/Datos_SGC_Terremoto_Choco/`) y la configuración por defecto apunta ahí. No hace falta moverlos.

#### Acelerógrafo UdeA (`.bin`)

| Carpeta | Archivo | Qué es |
|---|---|---|
| `aceleraciones/` | `accel_YYYYMMDD_HHMMSS.bin` | grabación continua, un archivo por minuto (60 s a 200 Hz) |
| `eventos/` | `evento_YYYYMMDD_HHMMSS_sNN.bin` | los 4 s que guarda el nodo cuando el detector dispara una alerta; `sNN` es el puntaje × 100 |

- Copiar los archivos **tal como salen de la tarjeta SD, sin renombrarlos**. La fecha y hora del nombre, en **hora local de Colombia (UTC−5)**, es la hora que se usa para cada archivo.
- Copiar al menos los minutos que cubren el sismo, con margen antes y después: la línea base se calcula con los primeros `BASELINE_S` segundos (20 s), que deben ser tranquilos.
- Mientras el nodo guarda un evento deja de grabar la señal continua. La reconstrucción rellena esos huecos con los eventos que caen en ellos y el resto por interpolación lineal, e informa cuántos segundos quedaron interpolados.
- Si el detector no disparó ninguna alerta, `eventos/` puede quedar vacía.
- El formato binario está descrito arriba, en *Formato `.bin` del ESP32*.

#### Estaciones del SGC (`.anc` y `.mseed`)

- **`.anc`**: `estacion_<municipio>_<idEventoSGC>_<CODIGO>_<LOC>.anc` (p. ej. `estacion_dabeiba_SGC2026pqqmro_DBB_10.anc`). Texto calibrado en cm/s², 200 Hz, 510 s. **No trae la hora de inicio del registro**, solo la hora de origen del sismo. Se busca por el final `_<CODIGO>_<LOC>.anc`, así que se deja el nombre como viene.
- **`.mseed`**: cuentas crudas del digitalizador, con hora exacta y ~20 min de registro. Sirve un archivo con todos los canales (`DBB.mseed`, `RIO2C.mseed`) o uno por canal (`CBOCA_HNE.mseed`, `CBOCA_HNN.mseed`, `CBOCA_HNZ.mseed`): se leen todos los `.mseed` de la carpeta y se separan por el código de estación que traen adentro.
- Solo se usan los canales acelerográficos `HNE/HNN/HNZ` (localización `10`). Los de banda ancha `HH*` (`00`), los de periodo corto `EH*` (`20`) y los archivos de otras estaciones se ignoran.
- Lo ideal es descargar **los dos formatos de cada estación**: el `.mseed` da la hora exacta y la ventana larga, y el `.anc` da la calibración.

#### Qué fuente usa cada estación

| Archivos en `sgc/` | Qué se grafica | Unidades | Hora de inicio |
|---|---|---|---|
| `.mseed` + `.anc` | el `.mseed`, calibrado contra el `.anc` por correlación cruzada (imprime el R² de cada canal) | g y cm/s² | la del `.mseed`; además da la hora real de inicio del `.anc` |
| solo `.anc` | el `.anc` | g y cm/s² | origen + `ANC_INICIO_OFFSET_S` (−30 s, lo medido en el Chocó) o `inicio_anc_utc` |
| solo `.mseed` | el `.mseed` en cuentas (o con `escala_cms2_por_cuenta`) | cuentas; las comparaciones se normalizan | la del `.mseed` |

En el Chocó las 3 estaciones tienen los dos formatos: R² > 0.99 en los 9 canales, y el `.anc` de las tres empieza a las 12:33:57 UTC (30 s antes del origen). Un R² bajo indica que el `.anc` y el `.mseed` no son de la misma estación o del mismo sismo.

---

### Configuración común

Las dos plantillas usan las mismas claves 1–10 en su celda de configuración; el mapa usa `SISMO` y `DIR_SGC` con los mismos nombres.

| Clave | Qué es |
|---|---|
| `SISMO` | nombre de la carpeta de datos y de la carpeta de salida |
| `DIR_ACEL`, `DIR_EVENTOS`, `DIR_SGC`, `DIR_SALIDA` | carpetas de entrada y de salida |
| `ESTACIONES_SGC` | estaciones del SGC que se comparan (ver abajo) |
| `TITULO_UBICACION`, `MAGNITUD`, `ORIGEN_UTC` | datos del evento para los títulos; `None` = tomarlos de la cabecera del `.anc` (sin ningún `.anc`, `ORIGEN_UTC` es obligatorio) |
| `TZ_OFFSET_HORAS`, `ZONAS`, `UNIDADES` | hora local (−5), zonas en que se guardan las figuras y unidades (`"g"`, `"cm/s²"`) |
| `ANC_INICIO_OFFSET_S`, `BASELINE_S` | inicio estimado del `.anc` si no hay `.mseed` (−30 s) y segundos iniciales para la línea base del Acelerógrafo UdeA (20 s) |
| `VENTANAS` | ventanas de tiempo que se analizan |
| `GRUPOS_ESTACIONES` | grupos de estaciones que se grafican juntas |

#### Nombres de las estaciones

Se declaran en `ESTACIONES_SGC`. El orden de la lista es el orden de dibujo en las gráficas de estaciones juntas (el Acelerógrafo UdeA siempre se dibuja primero):

```python
ESTACIONES_SGC = [
    {"nombre": "Riosucio", "codigo": "RIO2C", "red": "CM", "loc": "10", "color": "#A5D6A7"},
    {"nombre": "Pereira",  "codigo": "CBOCA", "red": "CM", "loc": "10", "color": "#90CAF9"},
    {"nombre": "Dabeiba",  "codigo": "DBB",   "red": "CM", "loc": "10", "color": "#EF9A9A"},
]
```

- `nombre`: municipio, como se quiere ver en títulos, leyendas y carpetas de salida.
- `codigo`: el de la línea `CODIGO DE LA ESTACION` del `.anc`, o el `station` del `.mseed` (`CM.DBB.10.HNZ` → `DBB`).
- `red` y `loc`: primera y tercera parte de ese identificador (`CM`, `10`).
- `color`: color suave de la estación en las gráficas de estaciones juntas.
- Opcionales: `"fuente"` (`"auto"`, `"mseed"` o `"anc"`), `"inicio_anc_utc"` (hora real de inicio del `.anc`, si se conoce) y `"escala_cms2_por_cuenta"` (calibración del `.mseed`, si se conoce y no hay `.anc`).

#### Hora local y hora UTC

Los `.bin` del Acelerógrafo UdeA tienen hora local de Colombia en el nombre y el SGC trabaja en UTC. Internamente todo se pasa a UTC con `TZ_OFFSET_HORAS = -5`. En el Chocó se comprobó así: al pasar la hora local a UTC, las alertas del detector caen justo en la llegada de las ondas P y S calculada con la distancia epicentral. Las figuras con eje de tiempo se guardan en las dos zonas de `ZONAS` (`_UTC` y `_LOCAL`).

#### Ventanas de tiempo

`VENTANAS` es una lista. Cada ventana genera su propio juego de figuras en `output/<sismo>/<ventana>/` (o `output/<sismo>/espectros/<ventana>/`) y recorta todas las trazas:

| `modo` | Intervalo | Parámetros |
|---|---|---|
| `hora_completa` | toda la señal reconstruida del Acelerógrafo UdeA | — |
| `estacion` | el registro de la estación (en estaciones juntas, el tramo común a las elegidas) | — |
| `primera_alerta` | desde la primera alerta del detector hasta el fin del registro de la estación | — |
| `relativa_origen` | alrededor de la hora de origen del sismo | `antes_s`, `despues_s` |
| `manual` | un intervalo cualquiera | `inicio`, `fin` (`"YYYY-MM-DD HH:MM:SS"`) y `zona` (`"LOCAL"` o `"UTC"`) |

```python
VENTANAS = [
    {"nombre": "registro_sgc",         "modo": "estacion"},
    {"nombre": "desde_primera_alerta", "modo": "primera_alerta"},
    {"nombre": "sismo",  "modo": "relativa_origen", "antes_s": 60, "despues_s": 300},
    {"nombre": "manual", "modo": "manual", "inicio": "2026-08-10 07:34:00", "fin": "2026-08-10 07:40:00", "zona": "LOCAL"},
]
```

Cada plantilla tiene su propia lista, así que pueden analizar ventanas distintas (por defecto, la de espectros agrega la ventana `sismo`, desde el origen hasta 300 s después).

---

### `plantilla_analisis_sismo.ipynb`: comparación en el tiempo

#### Qué hace

1. **Reconstruye la señal del Acelerógrafo UdeA**: une los `.bin` de grabación continua por la hora de su nombre en una grilla UTC a 200 Hz, rellena los huecos con los `.bin` de eventos y el resto por interpolación lineal, quita la línea base y marca las alertas del detector.
2. **Carga cada estación del SGC con la mejor fuente disponible** y toma del `.anc` el origen, la magnitud y el lugar del sismo para los títulos.
3. **Gráficas en formato SGC** (900×900 px, filas Z/N/E, código `Red.Estación.Loc.Canal`, misma escala en los 3 ejes, PGA de cada traza, franjas ámbar "Alerta Detectada"):
   - el Acelerógrafo UdeA solo, sobre la hora completa y recortado a cada ventana;
   - por estación y ventana: el Acelerógrafo UdeA solo (`propia`), la estación sola (`sgc`) y ambos superpuestos (`junta`, UdeA en negro y SGC en rojo);
   - **estaciones juntas**: el Acelerógrafo UdeA al fondo y las estaciones elegidas encima, cada una con su color.
4. Todo en **g** y **cm/s²**, y cada figura en **hora UTC** (`_UTC`) y **hora local** (`_LOCAL`).
5. **Animaciones GIF** de las gráficas del Acelerógrafo UdeA, de cada `junta` y de cada grupo de estaciones juntas: la traza se dibuja como en un sismógrafo (todas las trazas con el mismo reloj) y las franjas de alerta aparecen y crecen cuando pasa el frente de onda.
6. **Resumen final**: PGA por componente, estación y ventana, avisos y conteo de los archivos generados por carpeta.

#### Estaciones juntas

`GRUPOS_ESTACIONES` define los grupos que se grafican en cada ventana (por defecto `"todas"`). Además, `juntar_estaciones()` se puede llamar a mano con cualquier combinación de estaciones, en el orden de dibujo que se quiera y en cualquier ventana:

```python
juntar_estaciones(["Pereira", "Dabeiba"],
                  {"nombre": "sismo", "modo": "relativa_origen", "antes_s": 30, "despues_s": 240},
                  nombre_grupo="pereira_dabeiba")
```

Si alguna estación del grupo no está calibrada (solo `.mseed`), todas las trazas de esa gráfica se normalizan (σ) para que sigan siendo comparables.

#### Archivos de salida

```
output/<sismo>/
├── acelerometro/          acelerometro_hora_completa_{g,cms2}_{UTC,LOCAL}.{png,gif}
└── <ventana>/
    ├── acelerometro/      acelerometro_<ventana>_{g,cms2}_{UTC,LOCAL}.{png,gif}
    ├── <Estacion>/        <municipio>_<CODIGO>_<LOC>_{propia,sgc,junta}_{g,cms2}_{UTC,LOCAL}.png
    │                      <municipio>_<CODIGO>_<LOC>_junta_{g,cms2}_{UTC,LOCAL}.gif
    └── juntas_<grupo>/    juntas_<grupo>_{g,cms2}_{UTC,LOCAL}.{png,gif}
```

Para estaciones sin calibrar los archivos terminan en `_sgc_cuentas` y `_junta_norm`. En pantalla el notebook muestra solo la primera zona horaria de cada gráfica; todas quedan en disco.

#### Tiempo de ejecución

Con la configuración del Chocó (2 ventanas, 3 estaciones, 92 PNG y 44 GIF) tarda unos 19 minutos, casi todo en las animaciones (unos 25 s por GIF). Con `GENERAR_ANIMACIONES = False` genera solo las gráficas estáticas en menos de 2 minutos. `N_FRAMES` y `FPS` controlan la cantidad de cuadros y la velocidad de los GIF.

---

### `plantilla_espectros_sismo.ipynb`: comparación en frecuencia

#### Qué hace

1. **Espectro de amplitud de Fourier** de cada componente (Z, N, E) en cada ventana: el Acelerógrafo UdeA y cada estación superpuestos en escala log-log, suavizados con Konno-Ohmachi, cada uno con el espectro de su **ruido antes del sismo** en línea discontinua. De ahí salen la **frecuencia pico** y la **banda útil** de cada instrumento.
2. **Espectrograma comparativo**: tiempo × frecuencia del Acelerógrafo UdeA al lado de cada estación (y de cada grupo de estaciones), con la **misma escala de color**, las franjas de alerta y los tramos interpolados marcados.
3. **Vista CNN**: los espectrogramas de **65×11×3** que calculó el nodo para cada alerta, es decir, la entrada con la que la red neuronal del firmware decidió disparar. La tabla da también el puntaje, el PGA de cada eje y la media del log-PSD.

#### Cómo leer los resultados

- **Frecuencia pico**: el máximo del espectro suavizado entre 0.2 y 20 Hz, la banda donde el filtro del Acelerógrafo UdeA es plano. Se usa la misma banda en todos los instrumentos para que los picos se puedan comparar.
- **Banda útil**: el tramo continuo de frecuencias alrededor del pico donde la señal es al menos `FOURIER["snr_min"]` veces el ruido (3 por defecto). Fuera de esa banda el espectro es ruido.
- **Ruido**: se toma del propio registro de cada instrumento, `RUIDO["duracion_s"]` segundos que terminan `RUIDO["fin_antes_origen_s"]` segundos antes del origen (antes de la primera alerta si no se conoce el origen). Como la amplitud de Fourier de un ruido crece como √T, se multiplica por √(T_señal / T_ruido) antes de compararlo con la señal.
- **Filtro del Acelerógrafo UdeA**: los `.bin` continuos pasan por un pasa-altos de 0.1 Hz y un pasa-bajos de 20 Hz. Por encima de 20 Hz queda sobre todo el ruido de cuantización del ADXL345, y esa zona va sombreada en las gráficas de Fourier. Los `.bin` de eventos solo tienen el pasa-altos, porque la CNN se entrenó así; por eso la vista CNN va de 0 a 100 Hz.
- **Tramos interpolados**: una recta no tiene energía en alta frecuencia, así que en el espectrograma se ven como franjas verticales oscuras. Van marcados con una barra gris y el resumen da el porcentaje interpolado de cada ventana.
- **Resolución del espectrograma**: la da la ventana deslizante. Con `ventana_s = 5.12` la resolución es de ≈0.2 Hz y cada columna promedia 5 s; una ventana más corta da más detalle en el tiempo y menos en frecuencia.

#### Claves propias de la configuración (11–15)

| Clave | Qué controla |
|---|---|
| `RUIDO` | duración del tramo de ruido y cuánto antes del origen termina |
| `FOURIER` | rango de frecuencias, suavizado Konno-Ohmachi (`b`), fracción ahusada (`taper`) y `snr_min` de la banda útil |
| `ESPECTROGRAMA` | ventana deslizante, solape, rango de frecuencias, escala (`"log"` o `"lineal"`), rango de color en dB y unidad |
| `CNN` | alertas por figura y escala de color de la vista CNN |
| `GENERAR_FOURIER`, `GENERAR_ESPECTROGRAMAS`, `GENERAR_CNN` | qué análisis se generan |

`juntar_espectros()` es la versión espectral de `juntar_estaciones()`: pone el Acelerógrafo UdeA primero y después las estaciones elegidas, en ese orden, en la gráfica de Fourier y en el espectrograma. Se puede llamar a mano con cualquier combinación y ventana.

#### Archivos de salida

```
output/<sismo>/espectros/
├── acelerografo/        espectrograma_hora_completa_{UTC,LOCAL}.png
├── alertas_cnn/         alertas_cnn_{1,2,...}.png
└── <ventana>/
    ├── acelerografo/    fourier_acelerografo_<ventana>_{g,cms2}.png
    │                    espectrograma_acelerografo_<ventana>_{UTC,LOCAL}.png
    ├── <Estacion>/      <municipio>_<CODIGO>_<LOC>_fourier_junta_{g,cms2}.png
    │                    <municipio>_<CODIGO>_<LOC>_espectrograma_junta_{UTC,LOCAL}.png
    └── juntas_<grupo>/  juntas_<grupo>_fourier_{g,cms2}.png
                         juntas_<grupo>_espectrograma_{UTC,LOCAL}.png
```

- `junta` = el Acelerógrafo UdeA (negro) y la estación (rojo) en la misma gráfica de Fourier, o en columnas vecinas en el espectrograma.
- Si una estación no está calibrada, su espectro de Fourier sale como `_fourier_junta_norm` (cada curva dividida por su máximo) y en el espectrograma tiene su propia barra de color en cuentas.
- Las gráficas de Fourier no tienen eje de tiempo, así que no se guardan por zona horaria; el encabezado da la ventana en UTC y en hora local.

Con la configuración del Chocó (3 ventanas, 63 PNG) tarda unos 3 minutos.

---

### `mapa_sismo.ipynb`: mapas del epicentro y las estaciones

Dibuja dos mapas en PNG:

1. **Colombia**: todo el país, con un recuadro punteado que marca la zona del mapa regional.
2. **Regional**: un acercamiento automático al epicentro y las estaciones, con los municipios más poblados de sus departamentos (la capital en negrilla).

| Símbolo | Qué es |
|---|---|
| ★ estrella roja | epicentro |
| ▲ triángulo con el código adentro | estación acelerográfica del SGC |
| ■ cuadrado con el código adentro | Acelerógrafo UdeA |

Los límites departamentales van en violeta y cada departamento lleva su nombre. Si dos símbolos se tapan, se separan lo justo, y una línea delgada une cada símbolo movido con su ubicación real. La leyenda trae la distancia epicentral de cada estación, y el notebook imprime también la distancia hipocentral y el azimut.

#### Configuración (§3)

- `SISMO` y `DIR_SGC`: los mismos de las plantillas.
- `EVENTO`: datos del boletín del SGC (id, lugar, magnitud, profundidad, hora local y UTC, latitud y longitud). Lo que quede en `None` se lee de la cabecera de los `.anc`.
- `ESTACIONES`: una entrada por estación con `codigo`, `red`, `loc`, `municipio`, `departamento`, `lat`, `lon`, `tipo` (`"sgc"` o `"udea"`) y `color`. Es una lista distinta de `ESTACIONES_SGC` porque incluye también al Acelerógrafo UdeA. Las coordenadas de las estaciones del SGC pueden quedar en `None` (se leen del `.anc`); las del Acelerógrafo UdeA se escriben siempre.
- `DEPARTAMENTOS_MUNICIPIOS` y `N_MUNICIPIOS`: de qué departamentos se dibujan municipios y cuántos (por defecto, 5 de cada departamento del epicentro y de las estaciones).
- `FONDO`: `"relieve"` (Esri World Shaded Relief, necesita internet) o `"plano"`. `COLOR_DEPARTAMENTOS` y `MARGEN_GRADOS` ajustan el color de los límites y el margen del mapa regional.

#### Archivos de salida

```
output/<sismo>/mapa/
├── mapa_colombia_<sismo>.png
└── mapa_regional_<sismo>.png
```

#### Descargas de la primera corrida

- los límites de Natural Earth;
- con `FONDO = "relieve"`, las teselas de relieve de Esri (sin conexión, el mapa sale con fondo plano);
- la tabla de municipios de GeoNames, que se guarda en `data/geo/municipios_colombia.csv` y de ahí se lee en adelante. Trae la población y la ubicación de la cabecera de cada municipio, y se puede corregir a mano.

---

### Funciones comunes (`sismo_utils.py`)

Las funciones que usan varios notebooks están en `sismo_utils.py`, en la raíz del proyecto. Un cambio en la lectura o el procesamiento de los datos se hace una sola vez ahí y vale para todos.

| Grupo | Funciones |
|---|---|
| Acelerógrafo UdeA | `parse_bin()`, `discover_bins()`, `reconstruct_full_hour()`, `imprimir_reconstruccion()` |
| Estaciones del SGC | `parse_anc()`, `leer_cabecera_anc()`, `buscar_anc()`, `leer_mseed_carpeta()`, `calibrar_canal_mseed()`, `cargar_estacion()`, `cargar_estaciones()`, `datos_evento()` |
| Ventanas y unidades | `resolver_ventana()`, `recortar()`, `convertir()`, `local_to_utc()`, `a_datetime64()` |
| Configuración | `verificar_configuracion()`, `zonas_horarias()`, `slug()`, `nombre_ventana()` |
| Gráficas en formato SGC | `encabezado()`, `casilla_ids()`, `alertas_en_rango()`, `mostrar_o_cerrar()` |

También trae las constantes del sensor (`MAGIC`, `SCALE_G`, `G_TO_CMS2`, `FS`), de las componentes (`LABELS`, `CANAL_POR_COMP`, `ORDEN_FILAS`) y del estilo SGC (`COLOR_*`, `FIGSIZE`, `DPI`). Las funciones reciben como argumentos lo que depende del sismo (la reconstrucción, la hora de origen, la carpeta del SGC, la zona horaria y la lista de avisos), así que el módulo no lee variables de los notebooks.

Si se edita `sismo_utils.py` con un notebook abierto, hay que reiniciar el kernel para que lo vuelva a importar.

### Dependencias

Además de `numpy`, `matplotlib`, `scipy` y `obspy`: `pillow` para guardar los GIF, y `cartopy`, `shapely` y `requests` para el mapa.

```bash
uv pip install numpy matplotlib scipy obspy pillow cartopy shapely requests
```
