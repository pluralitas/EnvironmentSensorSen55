
'use strict';

// =============================================
// EnvironmentSensorSen55 Dashboard
// ThingSpeak Channels
// =============================================

const CHANNELS = [3591940, 3591941];

const METRICS = [
  ['CO2', 'CO₂', 'ppm', 0, 1],
  ['TMP', 'Temperature', '°C', 0, 2],
  ['RH', 'Humidity', '%', 0, 3],
  ['NOx', 'NOx Index', 'index', 0, 4],
  ['VOC', 'VOC Index', 'index', 0, 5],
  ['PM1', 'PM1', 'µg/m³', 0, 6],
  ['PM25', 'PM2.5', 'µg/m³', 0, 7],
  ['PM10', 'PM10', 'µg/m³', 0, 8],
  ['PM4', 'PM4', 'µg/m³', 1, 1],
  ['VOCmax', 'VOC maximum', 'index', 1, 2],
  ['PM25max', 'PM2.5 maximum', 'µg/m³', 1, 3],
  ['PM10max', 'PM10 maximum', 'µg/m³', 1, 4]
];

const CSV_NAMES = {
  CO2: 'CO2_ppm',
  TMP: 'Temperature_C',
  RH: 'Humidity_percent',
  NOx: 'NOx_Index',
  VOC: 'VOC_Index',
  PM1: 'PM1_ugm3',
  PM25: 'PM2_5_ugm3',
  PM4: 'PM4_ugm3',
  PM10: 'PM10_ugm3',
  VOCmax: 'VOC_Max_Index',
  PM25max: 'PM2_5_Max_ugm3',
  PM10max: 'PM10_Max_ugm3'
};

const $ = id => document.getElementById(id);

const state = {
  start: new Date(Date.now() - 86400000),
  end: new Date(),
  selected: new Set([
    'VOC', 'VOCmax', 'PM25', 'PM25max'
  ]),
  rows: []
};

// =============================================
// Time handling
// =============================================

// Singapore time is UTC+8.
const SGT_OFFSET = 8 * 3600000;

function formatLocal(date) {
  return new Date(date.getTime() + SGT_OFFSET)
    .toISOString()
    .slice(0, 16);
}

function parseLocal(value) {
  return new Date(value + '+08:00');
}

function csvStamp(value) {
  const date = new Date(value);

  if (isNaN(date.getTime())) return '';

  return new Date(date.getTime() + SGT_OFFSET)
    .toISOString()
    .slice(0, 19)
    .replace('T', ' ') + '+08:00';
}

function stamp(value) {
  return csvStamp(value);
}

// Group records by UTC minute.
// This also corresponds to Singapore clock minutes.
function minute(value) {
  return Math.floor(Date.parse(value) / 60000);
}

// =============================================
// Dashboard status
// =============================================

function setStatus(message) {
  $('status').textContent = message;
}

// =============================================
// Measurement checkboxes
// =============================================

function selectUI() {
  $('fields').innerHTML = '';

  for (const metric of METRICS) {
    const label = document.createElement('label');
    const checkbox = document.createElement('input');

    checkbox.type = 'checkbox';
    checkbox.checked = state.selected.has(metric[0]);

    checkbox.onchange = () => {
      if (checkbox.checked) {
        state.selected.add(metric[0]);
      } else {
        state.selected.delete(metric[0]);
      }

      render();
    };

    label.append(
      checkbox,
      document.createTextNode(
        metric[1] + ' (' + metric[2] + ')'
      )
    );

    $('fields').append(label);
  }
}

// =============================================
// ThingSpeak API retrieval
// =============================================

// Fetch in one-day batches.
// This avoids very large API responses.

async function getChannel(id, start, end) {
  const records = [];
  let cursor = start.getTime();

  while (cursor <= end.getTime()) {
    const batchEnd = Math.min(
      end.getTime(),
      cursor + 86400000 - 1000
    );

    const url = new URL(
      `https://api.thingspeak.com/channels/${id}/feeds.json`
    );

    url.searchParams.set(
      'start',
      new Date(cursor).toISOString().slice(0, 19) + 'Z'
    );

    url.searchParams.set(
      'end',
      new Date(batchEnd).toISOString().slice(0, 19) + 'Z'
    );

    url.searchParams.set('results', '8000');

    const response = await fetch(url);

    if (!response.ok) {
      throw new Error(
        `ThingSpeak channel ${id}: HTTP ${response.status}`
      );
    }

    const data = await response.json();

    if (Array.isArray(data.feeds)) {
      records.push(...data.feeds);
    }

    cursor = batchEnd + 1000;
  }

  return records;
}

// =============================================
// Combine both ThingSpeak channels
// =============================================

function combineChannels(channelData) {
  const map = new Map();

  for (let channelIndex = 0;
       channelIndex < CHANNELS.length;
       channelIndex++) {

    const feeds = channelData[channelIndex];

    for (const feed of feeds) {
      const key = minute(feed.created_at);

      if (!Number.isFinite(key)) continue;

      let row = map.get(key);

      if (!row) {
        row = {
          time: new Date(key * 60000).toISOString(),
          values: {}
        };

        map.set(key, row);
      }

      for (const metric of METRICS) {
        if (metric[3] !== channelIndex) continue;

        const raw = feed['field' + metric[4]];

        if (
          raw === null ||
          raw === undefined ||
          raw === ''
        ) {
          continue;
        }

        const value = Number(raw);

        if (Number.isFinite(value)) {
          row.values[metric[0]] = value;
        }
      }
    }
  }

  return [...map.values()].sort(
    (a, b) => a.time.localeCompare(b.time)
  );
}

// =============================================
// Load data
// =============================================

let loadSequence = 0;

async function load() {
  if (
    !Number.isFinite(state.start.getTime()) ||
    !Number.isFinite(state.end.getTime()) ||
    state.start >= state.end
  ) {
    setStatus('Invalid date range');
    return;
  }

  const sequence = ++loadSequence;

  setStatus('Loading ThingSpeak data...');

  try {
    const results = await Promise.all(
      CHANNELS.map(
        id => getChannel(id, state.start, state.end)
      )
    );

    // Ignore outdated requests.
    if (sequence !== loadSequence) return;

    state.rows = combineChannels(results);

    setStatus(
      'Updated: ' + stamp(new Date())
    );

    render();

  } catch (error) {
    if (sequence !== loadSequence) return;

    setStatus('Error: ' + error.message);
  }
}

// =============================================
// Plotly graph
// =============================================

function render() {
  const selected = METRICS.filter(
    metric => state.selected.has(metric[0])
  );

  const traces = selected.map((metric, index) => ({
    x: state.rows.map(row => row.time),

    y: state.rows.map(
      row => row.values[metric[0]] ?? null
    ),

    type: 'scatter',
    mode: 'lines',
    name: metric[1],
    connectgaps: false,

    yaxis: index === 0 ? 'y' : 'y' + (index + 1),

    hovertemplate:
      '%{x}<br>%{y} ' + metric[2] +
      '<extra>' + metric[1] + '</extra>'
  }));

  const layout = {
    margin: {
      l: 55,
      r: 65,
      t: 25,
      b: 60
    },

    hovermode: 'x unified',

    legend: {
      orientation: 'h',
      y: -0.22
    },

    xaxis: {
      type: 'date',
      title: 'Time (UTC)'
    },

    yaxis: {
      title: selected[0]?.[2] || 'Value'
    },

    paper_bgcolor: '#ffffff',
    plot_bgcolor: '#ffffff'
  };

  selected.forEach((metric, index) => {
    if (index === 0) return;

    layout['yaxis' + (index + 1)] = {
      overlaying: 'y',
      side: index % 2 ? 'right' : 'left',
      showticklabels: false,
      showgrid: false
    };
  });

  Plotly.react(
    'plot',
    traces,
    layout,
    {
      responsive: true,
      displaylogo: false
    }
  );

  $('count').textContent =
    state.rows.length + ' reporting minutes loaded';
}

// =============================================
// CSV formatting
// =============================================

// Properly escape commas, quotes and newlines.
function csvCell(value) {
  const text = String(value ?? '');

  if (/[",\r\n]/.test(text)) {
    return '"' + text.replace(/"/g, '""') + '"';
  }

  return text;
}

// =============================================
// CSV export
// =============================================

function csv() {
  if (!state.rows.length) {
    setStatus('No data available to export');
    return;
  }

  const selected = METRICS.filter(
    metric => state.selected.has(metric[0])
  );

  if (!selected.length) {
    setStatus('Select at least one measurement');
    return;
  }

  const headers = [
    'timestamp_SGT',
    ...selected.map(metric => CSV_NAMES[metric[0]])
  ];

  const lines = [
    headers.map(csvCell).join(',')
  ];

  for (const row of state.rows) {
    const values = [
      csvStamp(row.time),

      ...selected.map(
        metric => row.values[metric[0]] ?? ''
      )
    ];

    lines.push(
      values.map(csvCell).join(',')
    );
  }

  // UTF-8 BOM for Excel compatibility.
  const content =
    '\uFEFF' + lines.join('\r\n') + '\r\n';

  const blob = new Blob(
    [content],
    { type: 'text/csv;charset=utf-8' }
  );

  const url = URL.createObjectURL(blob);

  const anchor = document.createElement('a');

  anchor.href = url;

  anchor.download =
    'BedroomAirQuality_' +
    formatLocal(state.start).replace(/[:T]/g, '-') +
    '_to_' +
    formatLocal(state.end).replace(/[:T]/g, '-') +
    '.csv';

  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();

  setTimeout(
    () => URL.revokeObjectURL(url),
    1000
  );
}

// =============================================
// Time-range buttons
// =============================================

for (
  const button of
  document.querySelectorAll('[data-hours]')
) {
  button.onclick = () => {
    document.querySelectorAll('[data-hours]')
      .forEach(item => item.classList.remove('active'));

    button.classList.add('active');

    const hours = Number(button.dataset.hours);

    state.end = new Date();

    state.start = new Date(
      state.end.getTime() - hours * 3600000
    );

    $('start').value = formatLocal(state.start);
    $('end').value = formatLocal(state.end);

    load();
  };
}

// =============================================
// Custom date range
// =============================================

$('custom').onclick = () => {
  state.start = parseLocal($('start').value);
  state.end = parseLocal($('end').value);

  document.querySelectorAll('[data-hours]')
    .forEach(button => button.classList.remove('active'));

  load();
};

// =============================================
// Other controls
// =============================================

$('reload').onclick = load;
$('csv').onclick = csv;

$('all').onclick = () => {
  state.selected = new Set(
    METRICS.map(metric => metric[0])
  );

  selectUI();
  render();
};

$('none').onclick = () => {
  state.selected.clear();

  selectUI();
  render();
};

// =============================================
// Initialisation
// =============================================

$('start').value = formatLocal(state.start);
$('end').value = formatLocal(state.end);

selectUI();
load();

// =============================================
// Automatic refresh every 60 seconds
// =============================================

setInterval(() => {
  if (document.visibilityState !== 'visible') {
    return;
  }

  const active = document.querySelector(
    '[data-hours].active'
  );

  if (active) {
    const hours = Number(active.dataset.hours);

    state.end = new Date();

    state.start = new Date(
      state.end.getTime() - hours * 3600000
    );

    $('start').value = formatLocal(state.start);
    $('end').value = formatLocal(state.end);
  }

  load();

}, 60000);
