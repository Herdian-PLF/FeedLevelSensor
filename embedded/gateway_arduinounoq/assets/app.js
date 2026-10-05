const GRID_SIDE = 8;
const STORAGE_KEY = 'silometer.selectedEndpoint';

// Viridis, near (0) to far (1). Must match the .bar gradient in style.css.
const VIRIDIS = [
  [68, 1, 84], [72, 40, 120], [62, 73, 137], [49, 104, 142], [38, 130, 142],
  [31, 158, 137], [53, 183, 121], [110, 206, 88], [181, 222, 43], [253, 231, 37],
];

const endpoints = new Map();
let selected = loadSelection();

const select = document.querySelector('#endpoint');
const grid = document.querySelector('#grid');
const cells = [];
for (let i = 0; i < GRID_SIDE * GRID_SIDE; ++i) {
  const cell = document.createElement('div');
  cell.className = 'cell';
  grid.appendChild(cell);
  cells.push(cell);
}

const ui = new WebUI();
ui.on_connect(() => {
  setLink(true);
  // Also runs on every reconnect, so a socket dropped while readings arrived
  // catches up instead of showing a stale map.
  ui.send_message('snapshot');
});
ui.on_disconnect(() => setLink(false));
ui.on_message('snapshot_response', (data) => {
  endpoints.clear();
  for (const view of data.endpoints) {
    endpoints.set(view.endpoint, view);
  }
  refreshList();
  render();
});
ui.on_message('endpoint_update', (view) => {
  const isNew = !endpoints.has(view.endpoint);
  endpoints.set(view.endpoint, view);
  if (isNew) {
    refreshList();
  }
  if (view.endpoint === selected) {
    render();
  }
});

select.addEventListener('change', () => {
  selected = Number(select.value);
  saveSelection(selected);
  render();
});

// Keeps the "n min ago" part honest between messages.
setInterval(renderStatus, 10000);

function setLink(up) {
  const link = document.querySelector('#link');
  link.textContent = up ? 'live' : 'disconnected';
  link.className = `link ${up ? 'up' : 'down'}`;
}

function hex(endpoint) {
  return `0x${endpoint.toString(16).padStart(4, '0').toUpperCase()}`;
}

function refreshList() {
  const ids = [...endpoints.keys()].sort((a, b) => a - b);
  if (ids.length === 0) {
    select.disabled = true;
    return;
  }
  if (!endpoints.has(selected)) {
    selected = ids[0];
  }
  select.replaceChildren(
    ...ids.map((id) => {
      const option = document.createElement('option');
      option.value = String(id);
      option.textContent = hex(id);
      return option;
    }),
  );
  select.value = String(selected);
  select.disabled = false;
}

function render() {
  renderStatus();
  renderMap();
}

function renderStatus() {
  const view = endpoints.get(selected);
  document.querySelector('#last-message').textContent = view
    ? formatTime(view.last_message_at)
    : '—';

  const map = view && view.map;
  let info = view ? 'none received yet' : '—';
  if (map) {
    const state = map.complete ? 'complete' : 'partial';
    const zones = map.valid_zones === null ? '' : `, ${map.valid_zones}/64 valid`;
    info = `seq ${map.seq}, ${state}${zones}, ${formatTime(map.received_at)}`;
  }
  document.querySelector('#map-info').textContent = info;
}

function renderMap() {
  const view = endpoints.get(selected);
  const map = view && view.map;
  const lit = map
    ? map.distance_mm.filter((d, i) => map.zone_received[i] && d !== 0)
    : [];
  const min = lit.length ? Math.min(...lit) : 0;
  const max = lit.length ? Math.max(...lit) : 0;
  document.querySelector('#scale-min').textContent = lit.length ? min : '—';
  document.querySelector('#scale-max').textContent = lit.length ? max : '—';

  cells.forEach((cell, i) => {
    cell.className = 'cell';
    cell.style.background = '';
    cell.style.color = '';
    if (!map) {
      cell.textContent = '';
      cell.title = '';
      return;
    }
    const distance = map.distance_mm[i];
    if (!map.zone_received[i]) {
      cell.classList.add('lost');
      cell.textContent = '?';
      cell.title = `zone ${i}: fragment lost`;
    } else if (distance === 0) {
      // 0 is the sensor's own no-target sentinel, not a distance.
      cell.classList.add('none');
      cell.textContent = '–';
      cell.title = `zone ${i}: no target`;
    } else {
      const t = max > min ? (distance - min) / (max - min) : 0.5;
      const [r, g, b] = viridis(t);
      cell.style.background = `rgb(${r}, ${g}, ${b})`;
      cell.style.color = t < 0.6 ? '#ffffff' : '#111111';
      cell.textContent = distance;
      cell.title = `zone ${i}: ${distance} mm`;
    }
  });
}

function viridis(t) {
  const x = Math.min(Math.max(t, 0), 1) * (VIRIDIS.length - 1);
  const i = Math.min(Math.floor(x), VIRIDIS.length - 2);
  const f = x - i;
  return VIRIDIS[i].map((c, k) => Math.round(c + (VIRIDIS[i + 1][k] - c) * f));
}

function formatTime(epochS) {
  const when = new Date(epochS * 1000);
  const ago = Math.max(0, Math.round((Date.now() - when.getTime()) / 1000));
  let rel;
  if (ago < 60) {
    rel = `${ago} s ago`;
  } else if (ago < 3600) {
    rel = `${Math.floor(ago / 60)} min ago`;
  } else {
    rel = `${Math.floor(ago / 3600)} h ago`;
  }
  return `${when.toLocaleString()} (${rel})`;
}

// Storage can be unavailable (private window, blocked site data); the page then
// just falls back to the first endpoint.
function loadSelection() {
  try {
    const value = localStorage.getItem(STORAGE_KEY);
    return value === null ? null : Number(value);
  } catch {
    return null;
  }
}

function saveSelection(endpoint) {
  try {
    localStorage.setItem(STORAGE_KEY, String(endpoint));
  } catch {
    // Selection simply is not remembered.
  }
}
