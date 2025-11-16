const dropZone = document.getElementById('drop-zone');
const dropMessage = dropZone.querySelector('p');
const defaultDropMessage = dropMessage.textContent;
const fileInput = document.getElementById('file-input');
const browseBtn = document.getElementById('browse');
const resultsEl = document.getElementById('results');
const originalSection = document.getElementById('original');
const originalMeta = document.getElementById('original-meta');
const originalPreview = document.getElementById('original-preview');
const metricsSection = document.getElementById('metrics');
const metricsList = document.getElementById('metrics-list');
const consoleLog = document.getElementById('console-log');
const consoleSection = document.getElementById('console');

const keyForResult = (format = '', label = '') =>
  `${String(format).toLowerCase()}::${String(label).toLowerCase()}`;

const RESULT_PIPELINE = [
  {
    format: 'png',
    label: 'lossless',
    title: 'PNG · Pixel-perfect',
    hint: 'Zlib level 9',
    tooltip: '32-bit RGBA PNG compressed at zlib level 9. Ideal when every pixel must remain identical.',
    etaMs: 800,
  },
  {
    format: 'png',
    label: 'pngquant q80',
    title: 'PNG · Lite (lossy)',
    hint: 'Palette ≈80%',
    tooltip: 'Palette-based PNG (≈80% quality) for lightweight assets when slight color loss is acceptable.',
    etaMs: 1200,
  },
  {
    format: 'webp',
    label: 'high',
    title: 'WebP · High',
    hint: 'Modern browsers',
    tooltip: 'WebP lossy quality 90 — great for Chrome, Edge, and Firefox delivery.',
    etaMs: 700,
  },
  {
    format: 'avif',
    label: 'medium',
    title: 'AVIF · Streamlined',
    hint: 'HiDPI / mobile',
    tooltip: 'AVIF medium quality tuned for Safari, Chrome, and high-DPI mobile clients.',
    etaMs: 1600,
  },
];

const RESULT_DESCRIPTOR_MAP = new Map(
  RESULT_PIPELINE.map((descriptor) => [keyForResult(descriptor.format, descriptor.label), descriptor]),
);
const etaStore = new Map();
const resultCards = new Map();
let currentJobContext = null;
let activeEventSource = null;
let jobNonce = 0;

const preventDefaults = (event) => {
  event.preventDefault();
  event.stopPropagation();
};

function nextJobId() {
  jobNonce = (jobNonce + 1) % 1000;
  return Date.now() * 1000 + jobNonce;
}

['dragenter', 'dragover', 'dragleave', 'drop'].forEach((event) => {
  dropZone.addEventListener(event, preventDefaults, false);
});

['dragenter', 'dragover'].forEach((event) => {
  dropZone.addEventListener(event, () => dropZone.classList.add('active'));
});

['dragleave', 'drop'].forEach((event) => {
  dropZone.addEventListener(event, () => dropZone.classList.remove('active'));
});

dropZone.addEventListener('drop', (event) => {
  const file = event.dataTransfer?.files?.[0];
  if (file) handleFile(file);
});

browseBtn.addEventListener('click', () => fileInput.click());
fileInput.addEventListener('change', () => {
  const file = fileInput.files?.[0];
  if (file) handleFile(file);
});

function handleFile(file) {
  if (file.type !== 'image/png') {
    alert('Please upload a PNG file.');
    return;
  }
  const jobId = nextJobId();
  showOriginal(file);
  seedResultPlaceholders({ filename: file.name, bytes: file.size, jobId });
  subscribeToJob(jobId);
  appendLog(`queued <span>${file.name}</span> (${formatBytes(file.size)})`);
  upload(file, jobId);
}

function showOriginal(file) {
  const url = URL.createObjectURL(file);
  originalPreview.onload = () => URL.revokeObjectURL(url);
  originalPreview.src = url;
  originalMeta.textContent = `${file.name} · ${formatBytes(file.size)}`;
  originalSection.classList.remove('hidden');
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) {
    return '0 B';
  }
  const units = ['B', 'KB', 'MB', 'GB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  const fixed = unit === 0 ? 0 : unit === 1 ? 1 : 2;
  return `${value.toFixed(fixed)} ${units[unit]}`;
}

function toggleUploading(state) {
  dropZone.classList.toggle('uploading', state);
  dropMessage.textContent = state ? 'Crunching bits…' : defaultDropMessage;
  browseBtn.disabled = state;
}

function appendLog(text) {
  if (!consoleLog) return;
  if (consoleSection && consoleSection.classList.contains('hidden')) {
    consoleSection.classList.remove('hidden');
  }
  const time = new Date().toLocaleTimeString();
  const sentiment = classifyLog(text);
  const line = document.createElement('div');
  line.className = `log-line ${sentiment.className}`;
  line.innerHTML = `<span class="log-emoji">${sentiment.emoji}</span><span class="log-text">[${time}] ${text}</span>`;
  consoleLog.appendChild(line);
  while (consoleLog.children.length > 20) {
    consoleLog.removeChild(consoleLog.firstChild);
  }
  consoleLog.scrollTop = consoleLog.scrollHeight;
}

function classifyLog(text = '') {
  const normalized = text.toLowerCase();
  if (normalized.startsWith('ready')) {
    return { emoji: '✅', className: 'log-success' };
  }
  if (normalized.startsWith('completed')) {
    return { emoji: '🎯', className: 'log-success' };
  }
  if (normalized.includes('error') || normalized.includes('failed')) {
    return { emoji: '🔥', className: 'log-error' };
  }
  if (normalized.includes('all encoder')) {
    return { emoji: '🎉', className: 'log-success' };
  }
  if (normalized.includes('status ok')) {
    return { emoji: '🟢', className: 'log-success' };
  }
  if (normalized.includes('status')) {
    return { emoji: '🛈', className: 'log-info' };
  }
  if (normalized.includes('uploading') || normalized.includes('queued')) {
    return { emoji: '📦', className: 'log-pending' };
  }
  if (normalized.includes('primed')) {
    return { emoji: '⚙️', className: 'log-info' };
  }
  if (normalized.includes('listening') || normalized.includes('stream open')) {
    return { emoji: '📡', className: 'log-info' };
  }
  return { emoji: '🌀', className: 'log-info' };
}

function updateMetrics(payload = {}) {
  if (!metricsSection) return;
  const statusText = payload.message || payload.status || 'idle';
  const items = [
    { label: 'Job ID', value: payload.jobId ? `#${payload.jobId}` : '–' },
    { label: 'Input Size', value: formatBytes(payload.inputBytes || 0) },
    { label: 'Duration', value: formatDuration(payload.durationMs || 0) },
    { label: 'Status', value: statusText },
  ];

  metricsList.innerHTML = '';
  items.forEach(({ label, value }) => {
    const li = document.createElement('li');
    const span = document.createElement('span');
    span.textContent = label;
    const strong = document.createElement('strong');
    strong.textContent = value;
    li.appendChild(span);
    li.appendChild(strong);
    metricsList.appendChild(li);
  });
  metricsSection.classList.remove('hidden');
}

function formatDuration(ms) {
  if (!ms) return '0.0 ms';
  if (ms >= 1000) {
    return `${(ms / 1000).toFixed(2)} s`;
  }
  return `${ms.toFixed(1)} ms`;
}

async function upload(file, jobId) {
  toggleUploading(true);
  appendLog(`uploading <span>${file.name}</span> → /api/compress`);
  try {
    const payloadBuffer = await file.arrayBuffer();
    const request = fetch('/api/compress', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/octet-stream',
        'X-Filename': file.name,
        ...(jobId ? { 'X-Job-ID': String(jobId) } : {}),
      },
      body: payloadBuffer,
    });
    appendLog('ferret cores crunching payload…');
    const response = await request;

    let payload = null;
    try {
      payload = await response.json();
    } catch (error) {
      if (!response.ok) throw new Error('Failed to parse server response.');
      throw error;
    }

    if (!response.ok) {
      throw new Error(payload?.message || 'Compression failed.');
    }

    appendLog(`completed <span>#${payload.jobId || '—'}</span> in ${formatDuration(payload.durationMs || 0)}`);
    updateMetrics(payload);
    renderResults(payload);
  } catch (error) {
    appendLog(`error → ${error.message || error}`);
    alert(error.message || 'Failed to compress image.');
    markResultCardsAsError(error.message || 'Compression failed.');
  } finally {
    toggleUploading(false);
  }
}

function renderResults(payload) {
  if (!resultsEl) return;
  const { results = [], inputBytes = null, filename = '' } = payload || {};
  if (!results.length) {
    markResultCardsAsError('No outputs returned');
    return;
  }

  const baselineBytes = typeof inputBytes === 'number' && inputBytes > 0
    ? inputBytes
    : currentJobContext?.inputBytes || 0;
  const name = filename || currentJobContext?.filename || '';

  resultsEl.classList.remove('hidden');
  if (!resultsEl.childElementCount) {
    seedResultPlaceholders({ filename: name, bytes: baselineBytes }, { silent: true });
  }

  results.forEach((result) => {
    populateResultCard(result, baselineBytes, name);
  });
}

function subscribeToJob(jobId) {
  if (!Number.isFinite(jobId)) {
    return;
  }
  if (typeof EventSource === 'undefined') {
    appendLog('ℹ️  SSE unsupported; results will land together.');
    return;
  }
  closeEventStream();
  appendLog(`listening for <span>#${jobId}</span> stream updates`);
  const source = new EventSource(`/api/jobs/${jobId}/events`);
  source.onopen = () => appendLog(`stream open for <span>#${jobId}</span>`);
  source.addEventListener('result', handleStreamResult);
  source.addEventListener('status', handleStreamStatus);
  source.onerror = () => {
    if (source.readyState === EventSource.CLOSED) {
      closeEventStream();
    }
  };
  activeEventSource = source;
}

function closeEventStream() {
  if (activeEventSource) {
    activeEventSource.close();
    activeEventSource = null;
  }
}

function parseEventPayload(event) {
  try {
    return JSON.parse(event.data);
  } catch (err) {
    console.warn('Unable to parse stream payload', err);
    return null;
  }
}

function handleStreamResult(event) {
  const payload = parseEventPayload(event);
  if (!payload || payload.jobId !== currentJobContext?.jobId) {
    return;
  }
  const baseline = payload.inputBytes || currentJobContext?.inputBytes || 0;
  populateResultCard(payload, baseline, currentJobContext?.filename || '');
  updateEtaStore(payload);
}

function handleStreamStatus(event) {
  const payload = parseEventPayload(event);
  if (!payload || payload.jobId !== currentJobContext?.jobId) {
    return;
  }
  const statusLabel = (payload.status || 'update').toUpperCase();
  const detail = payload.message && payload.message !== payload.status ? ` – ${payload.message}` : '';
  const duration = typeof payload.durationMs === 'number' ? ` in ${formatDuration(payload.durationMs)}` : '';
  appendLog(`status <span>${statusLabel}</span>${detail}${duration}`);
  if (payload.status === 'error') {
    markResultCardsAsError(payload.message || 'Job failed');
  }
  if (payload.status === 'ok' || payload.status === 'error') {
    closeEventStream();
  }
}

function resultKey(format = '', label = '') {
  return keyForResult(format, label);
}

function seedResultPlaceholders(meta = {}, options = {}) {
  if (!resultsEl) return;
  const filename = meta.filename || currentJobContext?.filename || '';
  const bytes = typeof meta.bytes === 'number' ? meta.bytes : currentJobContext?.inputBytes || 0;
  const jobId = meta.jobId || currentJobContext?.jobId || null;
  currentJobContext = {
    jobId,
    filename,
    inputBytes: bytes,
    startTime: Date.now(),
    pendingKeys: new Set(RESULT_PIPELINE.map((descriptor) => resultKey(descriptor.format, descriptor.label))),
    deliveredLogged: false,
  };
  resultCards.clear();
  resultsEl.innerHTML = '';
  resultsEl.classList.remove('hidden');

  RESULT_PIPELINE.forEach((descriptor) => {
    const entry = createResultCard(descriptor);
    resultCards.set(resultKey(descriptor.format, descriptor.label), entry);
    startGhostProgress(entry, descriptor);
  });

  if (!options.silent) {
    appendLog(`primed <span>${RESULT_PIPELINE.length}</span> encoder slots.`);
  }
}

function createResultCard(descriptor) {
  const card = document.createElement('article');
  card.className = 'result-card pending';
  const title = document.createElement('h3');
  title.textContent = descriptor.title || `${descriptor.format?.toUpperCase()} – ${descriptor.label}`;
  title.title = descriptor.tooltip || 'Awaiting compression results';

  const meta = document.createElement('p');
  meta.className = 'result-meta';
  meta.textContent = descriptor.hint || 'Standing by…';

  const gauge = document.createElement('div');
  gauge.className = 'result-gauge';
  gauge.setAttribute('role', 'progressbar');
  gauge.setAttribute('aria-valuenow', '0');
  gauge.setAttribute('aria-valuemin', '0');
  gauge.setAttribute('aria-valuemax', '100');
  const gaugeFill = document.createElement('div');
  gaugeFill.className = 'result-gauge-fill';
  gauge.appendChild(gaugeFill);

  const preview = document.createElement('div');
  preview.className = 'result-preview loading';
  const previewStage = document.createElement('div');
  previewStage.className = 'result-preview-stage';
  const spinner = document.createElement('div');
  spinner.className = 'spinner';
  spinner.setAttribute('aria-hidden', 'true');
  const placeholder = document.createElement('p');
  placeholder.className = 'result-placeholder-text';
  placeholder.textContent = 'Encoding…';
  previewStage.appendChild(spinner);
  previewStage.appendChild(placeholder);
  preview.appendChild(previewStage);

  const download = document.createElement('a');
  download.href = '#';
  download.textContent = 'Preparing…';
  download.classList.add('disabled');
  download.setAttribute('aria-disabled', 'true');
  download.addEventListener('click', (event) => {
    if (download.classList.contains('disabled')) {
      event.preventDefault();
    }
  });

  card.appendChild(title);
  card.appendChild(meta);
  card.appendChild(gauge);
  card.appendChild(preview);
  card.appendChild(download);
  resultsEl.appendChild(card);
  return {
    card,
    meta,
    preview,
    previewStage,
    download,
    gauge,
    gaugeFill,
    titleEl: title,
    placeholderEl: placeholder,
    descriptor,
    ghostFrame: null,
  };
}

function getEtaMs(descriptor) {
  const key = descriptor ? resultKey(descriptor.format, descriptor.label) : '';
  if (key && etaStore.has(key)) {
    return etaStore.get(key);
  }
  return descriptor?.etaMs || 1500;
}

function startGhostProgress(entry, descriptor) {
  if (!entry || !descriptor) return;
  const eta = getEtaMs(descriptor);
  const update = () => {
    if (!currentJobContext?.startTime || entry.card.classList.contains('ready')) {
      entry.ghostFrame = null;
      return;
    }
    const elapsed = Date.now() - currentJobContext.startTime;
    const percent = Math.min(95, Math.max(5, (elapsed / eta) * 100));
    entry.gaugeFill.style.width = `${percent.toFixed(1)}%`;
    entry.gaugeFill.classList.remove('negative');
    entry.gauge.title = `Crunching… ~${percent.toFixed(0)}%`;
    if (entry.placeholderEl) {
      entry.placeholderEl.textContent = `Crunching… ~${percent.toFixed(0)}%`;
    }
    entry.ghostFrame = requestAnimationFrame(update);
  };
  stopGhostProgress(entry);
  entry.ghostFrame = requestAnimationFrame(update);
}

function stopGhostProgress(entry) {
  if (!entry) return;
  if (entry.ghostFrame) {
    cancelAnimationFrame(entry.ghostFrame);
    entry.ghostFrame = null;
  }
  if (entry.placeholderEl) {
    entry.placeholderEl.textContent = 'Encoding…';
  }
  entry.gauge.title = '';
}

function ensureResultCard(result) {
  const key = resultKey(result.format, result.label);
  if (resultCards.has(key)) {
    return resultCards.get(key);
  }
  const descriptor = {
    format: result.format || 'output',
    label: result.label || 'variant',
    title: `${(result.format || 'output').toUpperCase()} – ${result.label || ''}`.trim(),
    hint: 'Computed on-demand',
  };
  const entry = createResultCard(descriptor);
  resultCards.set(key, entry);
  return entry;
}

function populateResultCard(result, baselineBytes, filename) {
  const entry = ensureResultCard(result);
  const alreadyReady = entry.card.classList.contains('ready');
  stopGhostProgress(entry);
  entry.card.classList.remove('pending', 'error');
  entry.card.classList.add('ready');

  const savings = baselineBytes ? ((baselineBytes - result.bytes) / baselineBytes) * 100 : 0;
  const savingsText = savings ? ` (${savings.toFixed(1)}% smaller)` : '';
  const statLine = `${formatBytes(result.bytes)}${savingsText}`;
  entry.meta.textContent = statLine;
  entry.titleEl.title = formatResultTooltip(result, baselineBytes);

  const dataUri = `data:${result.mime};base64,${result.data}`;
  entry.preview.classList.remove('loading');
  entry.previewStage.innerHTML = '';
  const img = document.createElement('img');
  img.src = dataUri;
  img.alt = `${result.format} preview`;
  img.loading = 'lazy';
  entry.previewStage.appendChild(img);

  entry.download.href = dataUri;
  entry.download.download = resolveDownloadName(filename, result.extension || result.format);
  entry.download.textContent = 'Download';
  entry.download.classList.remove('disabled');
  entry.download.removeAttribute('aria-disabled');

  const percent = baselineBytes ? ((baselineBytes - result.bytes) / baselineBytes) * 100 : 0;
  const clamped = Math.max(0, Math.min(100, percent < 0 ? 0 : percent));
  entry.gaugeFill.style.width = `${clamped}%`;
  entry.gaugeFill.classList.toggle('negative', percent < 0);
  entry.gauge.setAttribute('aria-valuenow', percent.toFixed(1));
  entry.gauge.title = percent
    ? `${percent.toFixed(1)}% smaller vs original`
    : baselineBytes
      ? 'No size change versus original'
      : 'Awaiting baseline metrics';

  if (!alreadyReady) {
    const durationText = typeof result.durationMs === 'number' && result.durationMs >= 0
      ? ` in ${formatDuration(result.durationMs)}`
      : '';
    appendLog(`ready <span>${result.format.toUpperCase()} – ${result.label}</span> → ${statLine}${durationText}`);
    updateEtaStore(result);
    if (currentJobContext?.pendingKeys) {
      currentJobContext.pendingKeys.delete(resultKey(result.format, result.label));
      if (currentJobContext.pendingKeys.size === 0 && !currentJobContext.deliveredLogged) {
        appendLog('all encoder variants delivered.');
        currentJobContext.deliveredLogged = true;
      }
    }
  }
}

function resolveDownloadName(filename, extension) {
  const base = filename ? filename.replace(/\.[^.]+$/, '') : 'compressed';
  const safeExtension = extension || 'img';
  return `${base}.${safeExtension}`;
}

function markResultCardsAsError(message) {
  if (!resultsEl) return;
  if (!resultsEl.childElementCount) {
    seedResultPlaceholders({}, { silent: true });
  }
  resultsEl.classList.remove('hidden');
  resultCards.forEach((entry) => {
    entry.card.classList.remove('ready');
    entry.card.classList.add('error');
    stopGhostProgress(entry);
    entry.meta.textContent = message;
    entry.preview.classList.remove('loading');
    entry.previewStage.innerHTML = `<p class="result-placeholder-text">${message}</p>`;
    entry.download.textContent = 'Unavailable';
    entry.download.classList.add('disabled');
    entry.download.setAttribute('aria-disabled', 'true');
    entry.download.removeAttribute('href');
  });
}

function formatResultTooltip(result, baselineBytes) {
  const baseline = baselineBytes ? `${formatBytes(baselineBytes)} original` : 'unknown input';
  const percent = baselineBytes ? (((baselineBytes - result.bytes) / baselineBytes) * 100).toFixed(1) : '0.0';
  const descriptor = RESULT_DESCRIPTOR_MAP.get(resultKey(result.format, result.label));
  const description = descriptor?.tooltip || 'Encoder output';
  const durationLine = typeof result.durationMs === 'number'
    ? `Finished in ${formatDuration(result.durationMs)}`
    : null;
  return [
    `${(result.format || '').toUpperCase()} – ${result.label || 'variant'}`,
    description,
    `MIME: ${result.mime || 'n/a'}`,
    `Profile: ${result.label || 'n/a'}`,
    `Output: ${formatBytes(result.bytes)} (${percent}% vs ${baseline})`,
    durationLine,
  ].filter(Boolean).join('\n');
}

function updateEtaStore(result) {
  const key = resultKey(result.format, result.label);
  if (!key) return;
  const avg = typeof result.avgDurationMs === 'number' && result.avgDurationMs > 0
    ? result.avgDurationMs
    : (typeof result.durationMs === 'number' && result.durationMs > 0 ? result.durationMs : null);
  if (!avg) return;
  etaStore.set(key, avg);
  const descriptor = RESULT_DESCRIPTOR_MAP.get(key);
  if (descriptor) {
    descriptor.etaMs = avg;
  }
}

function updateEtaStore(result) {
  const key = resultKey(result.format, result.label);
  if (!key) return;
  const avg = typeof result.avgDurationMs === 'number' && result.avgDurationMs > 0
    ? result.avgDurationMs
    : (typeof result.durationMs === 'number' && result.durationMs > 0 ? result.durationMs : null);
  if (!avg) return;
  etaStore.set(key, avg);
}
