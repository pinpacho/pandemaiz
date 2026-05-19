/**
 * app.js — Dashboard PANdeMaiz Quake
 *
 * Polling de /api/stations cada 5 s.
 * Marcadores verde (normal) o rojo (alerta) sobre mapa Leaflet.
 */

const POLL_INTERVAL_MS = 5000;

// ── Mapa centrado en Colombia ─────────────────────────────────────────────────
const map = L.map("map", { zoomControl: true }).setView([4.5, -74.0], 6);

L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
  attribution: '© <a href="https://openstreetmap.org">OpenStreetMap</a>',
  maxZoom: 18,
}).addTo(map);

// ── Estado local ──────────────────────────────────────────────────────────────
const markers = {};          // station_id → L.Marker
let   globalAlertActive = false;
let   stationsData = [];     // copia del último fetch; usada por el panel de descarga

// ── Iconos ────────────────────────────────────────────────────────────────────
function makeIcon(color) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 36" width="24" height="36">
    <path d="M12 0C5.4 0 0 5.4 0 12c0 9 12 24 12 24S24 21 24 12C24 5.4 18.6 0 12 0z"
          fill="${color}" stroke="#fff" stroke-width="1.5"/>
    <circle cx="12" cy="12" r="5" fill="#fff" opacity="0.9"/>
  </svg>`;
  return L.divIcon({
    html: svg,
    iconSize: [24, 36],
    iconAnchor: [12, 36],
    popupAnchor: [0, -36],
    className: "",
  });
}

// Colores alineados con la paleta de los logos institucionales
const ICON_GREEN = makeIcon("#006837");   // Verde UdeA
const ICON_RED   = makeIcon("#9b1c1c");   // Rojo sísmico PANdeMaiz
const ICON_GREY  = makeIcon("#3a5a40");   // Verde oscuro apagado

// ── Construcción del popup ────────────────────────────────────────────────────
function buildPopup(s) {
  const lastSeen = s.last_seen
    ? new Date(s.last_seen * 1000).toLocaleString("es-CO")
    : "—";

  const downloadLinks = s.ip
    ? `<div class="popup-links">
         <a class="popup-btn" href="/download?station_id=${s.station_id}&file=/Aceleraciones&format=bin"
            target="_blank">↓ .bin</a>
         <a class="popup-btn" href="/download?station_id=${s.station_id}&file=/Aceleraciones&format=mseed"
            target="_blank">↓ .mseed</a>
         <a class="popup-btn" href="/download?station_id=${s.station_id}&file=/Aceleraciones&format=anc"
            target="_blank">↓ .anc</a>
         <a class="popup-btn" href="http://${s.ip}" target="_blank">🌐 ESP32</a>
       </div>`
    : "";

  return `
    <div class="popup-title">${s.station_id}</div>
    <div class="popup-row"><span>Estado:</span> ${s.estado}</div>
    <div class="popup-row"><span>IP:</span> ${s.ip || "—"}</div>
    <div class="popup-row"><span>Lat/Lon:</span> ${s.lat.toFixed(5)}, ${s.lon.toFixed(5)}</div>
    <div class="popup-row"><span>Última vez:</span> ${lastSeen}</div>
    <div class="popup-row"><span>Alerta:</span> ${s.alerta ? "🔴 SÍ" : "🟢 No"}</div>
    ${downloadLinks}
  `;
}

// ── Actualización de marcadores y panel lateral ───────────────────────────────
function updateStations(stations) {
  const list = document.getElementById("station-list");
  const known = new Set(Object.keys(markers));
  let anyAlert = false;

  stations.forEach((s) => {
    if (!s.lat && !s.lon) return;   // sin coordenadas aún
    known.delete(s.station_id);

    const icon = s.alerta ? ICON_RED : (s.estado === "Conectada" ? ICON_GREEN : ICON_GREY);
    if (s.alerta) anyAlert = true;

    if (markers[s.station_id]) {
      markers[s.station_id].setIcon(icon);
      markers[s.station_id].setPopupContent(buildPopup(s));
    } else {
      const m = L.marker([s.lat, s.lon], { icon })
        .addTo(map)
        .bindPopup(buildPopup(s));
      markers[s.station_id] = m;
    }
  });

  // Eliminar marcadores de estaciones que ya no existen
  known.forEach((id) => {
    map.removeLayer(markers[id]);
    delete markers[id];
  });

  // Panel lateral
  list.innerHTML = stations
    .map((s) => {
      const dotClass = s.alerta ? "dot-red" : (s.estado === "Conectada" ? "dot-green" : "dot-grey");
      return `
        <div class="station-card" onclick="focusStation('${s.station_id}', ${s.lat}, ${s.lon})">
          <div class="station-header">
            <div class="dot ${dotClass}"></div>
            <div class="station-name">${s.station_id}</div>
          </div>
          <div class="station-meta">${s.ip || "Sin IP"} · ${s.estado}</div>
        </div>`;
    })
    .join("");

  // Banner de alerta global
  const banner = document.getElementById("global-alert-banner");
  banner.style.display = anyAlert ? "block" : "none";
  globalAlertActive = anyAlert;

  // Guardar referencia y sincronizar dropdown del panel de descarga histórica
  stationsData = stations;
  syncStationDropdown(stations);
}

function focusStation(stationId, lat, lon) {
  if (!lat && !lon) return;
  map.setView([lat, lon], 30);
  const m = markers[stationId];
  if (m) m.openPopup();
}

// ── Polling ───────────────────────────────────────────────────────────────────
async function fetchStations() {
  try {
    const resp = await fetch("/api/stations");
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const data = await resp.json();
    updateStations(data);
    document.getElementById("status-bar").textContent =
      `${data.length} estación(es) · ${new Date().toLocaleTimeString("es-CO")}`;
  } catch (err) {
    document.getElementById("status-bar").textContent = `Error: ${err.message}`;
  }
}

fetchStations();
setInterval(fetchStations, POLL_INTERVAL_MS);


// ══════════════════════════════════════════════════════════════════════════════
// PANEL DE DESCARGA HISTÓRICA (Fase 5)
// ══════════════════════════════════════════════════════════════════════════════

const selStation  = document.getElementById("sel-station");
const selDate     = document.getElementById("sel-date");
const hourGrid    = document.getElementById("hour-grid");
const hourHint    = document.getElementById("hour-hint");
const btnDownload = document.getElementById("btn-download-zip");
const dlStatus    = document.getElementById("download-status");

// Establece la fecha máxima al día de hoy
selDate.max = new Date().toISOString().slice(0, 10);

let selectedHour = null;   // HH seleccionada por el usuario

// ── Sincronizar dropdown de estaciones con el mapa ───────────────────────────
// Se llama desde updateStations() cada vez que llegan datos nuevos.
function syncStationDropdown(stations) {
  const current = selStation.value;
  selStation.innerHTML = '<option value="">— Selecciona —</option>';
  stations.forEach((s) => {
    const opt = document.createElement("option");
    opt.value = s.station_id;
    opt.textContent = s.station_id;
    selStation.appendChild(opt);
  });
  // Restaurar selección previa si sigue existiendo
  if (current && [...selStation.options].some((o) => o.value === current)) {
    selStation.value = current;
  }
}

// ── Obtener horas disponibles desde el backend ───────────────────────────────
async function fetchAvailableHours() {
  const station = selStation.value;
  const date    = selDate.value;

  // Limpiar estado previo
  hourGrid.innerHTML = "";
  selectedHour = null;
  btnDownload.disabled = true;
  dlStatus.textContent = "";

  if (!station || !date) {
    hourGrid.appendChild(hourHint);
    hourHint.textContent = "Elige estación y fecha";
    return;
  }

  hourHint.textContent = "Consultando Firebase…";
  hourGrid.appendChild(hourHint);

  try {
    const resp = await fetch(
      `/api/v1/estaciones/${encodeURIComponent(station)}/disponibilidad?fecha=${date}`
    );
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const hours = await resp.json();   // ["00", "01", "13", ...]

    hourGrid.innerHTML = "";

    if (hours.length === 0) {
      hourHint.textContent = "Sin datos para esa fecha";
      hourGrid.appendChild(hourHint);
      return;
    }

    // Pintar botón por cada hora disponible
    hours.forEach((h) => {
      const btn = document.createElement("button");
      btn.className = "hour-btn";
      btn.textContent = `${h}h`;
      btn.dataset.hour = h;
      btn.addEventListener("click", () => selectHour(h, btn));
      hourGrid.appendChild(btn);
    });

  } catch (err) {
    hourHint.textContent = `Error: ${err.message}`;
    hourGrid.appendChild(hourHint);
  }
}

// ── Selección de una hora ─────────────────────────────────────────────────────
function selectHour(hour, clickedBtn) {
  // Desactivar botones previos
  hourGrid.querySelectorAll(".hour-btn").forEach((b) => b.classList.remove("active"));
  clickedBtn.classList.add("active");
  selectedHour = hour;
  btnDownload.disabled = false;
  dlStatus.textContent = "";
}

// ── Descarga del ZIP ──────────────────────────────────────────────────────────
btnDownload.addEventListener("click", async () => {
  const station = selStation.value;
  const date    = selDate.value;
  const format  = document.querySelector('input[name="fmt"]:checked')?.value || "bin";

  if (!station || !date || !selectedHour) return;

  btnDownload.disabled = true;
  dlStatus.textContent = "⏳ Generando ZIP…";

  try {
    const url =
      `/api/v1/download-hour?station_id=${encodeURIComponent(station)}` +
      `&date=${date}&hour=${selectedHour}&format=${format}`;

    const resp = await fetch(url);

    if (!resp.ok) {
      const body = await resp.json().catch(() => ({ detail: resp.statusText }));
      throw new Error(body.detail || resp.statusText);
    }

    // Información de cuántos archivos se descargaron (cabeceras custom)
    const ok  = resp.headers.get("X-Files-Downloaded") ?? "?";
    const err = resp.headers.get("X-Files-Errors") ?? "0";

    // Forzar descarga del blob
    const blob = await resp.blob();
    const zipName =
      `PANdeMaiz_${station}_${date}_${selectedHour}h.zip`;
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = zipName;
    a.click();
    URL.revokeObjectURL(a.href);

    dlStatus.textContent =
      `✓ ${ok} archivos descargados${err > 0 ? ` (${err} con error)` : ""}`;
  } catch (err) {
    dlStatus.textContent = `✗ ${err.message}`;
  } finally {
    btnDownload.disabled = false;
  }
});

// ── Listeners del formulario ──────────────────────────────────────────────────
selStation.addEventListener("change", () => {
  fetchAvailableHours();

  // Zoom al marcador de la estación seleccionada
  const s = stationsData.find((x) => x.station_id === selStation.value);
  if (s && (s.lat || s.lon)) focusStation(s.station_id, s.lat, s.lon);
});

selDate.addEventListener("change", fetchAvailableHours);

