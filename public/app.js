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
const googleLoginBtn = document.getElementById('google-login');
const loginToggle = document.getElementById('login-toggle');
const loginDropdown = document.getElementById('login-dropdown');
const expertOpenBtn = document.getElementById('expert-open');
const expertModal = document.getElementById('expert-modal');
const expertCloseBtn = document.getElementById('expert-close');
const expertModalBackdrop = document.getElementById('expert-modal-backdrop');

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
let originalFile = null;
let originalFileBuffer = null;
const MAX_FILE_SIZE = 100 * 1024 * 1024; // 100 MB
let authProfile = null;

const preventDefaults = (event) => {
  event.preventDefault();
  event.stopPropagation();
};

function nextJobId() {
  jobNonce = (jobNonce + 1) % 1000;
  return Date.now() * 1000 + jobNonce;
}

function applyLoginToggleLabel(label = 'Login') {
  if (!loginToggle) return;
  loginToggle.innerHTML = '';
  const text = document.createTextNode(label);
  const caret = document.createElement('span');
  caret.setAttribute('aria-hidden', 'true');
  caret.textContent = '▾';
  caret.className = 'caret';
  loginToggle.appendChild(text);
  loginToggle.appendChild(caret);
}

function closeLoginDropdown() {
  if (loginDropdown) {
    loginDropdown.classList.add('hidden');
  }
  if (loginToggle) {
    loginToggle.setAttribute('aria-expanded', 'false');
  }
}

function openLoginDropdown() {
  if (loginDropdown) {
    loginDropdown.classList.remove('hidden');
  }
  if (loginToggle) {
    loginToggle.setAttribute('aria-expanded', 'true');
  }
}

function closeExpertModal() {
  if (!expertModal) return;
  expertModal.classList.add('hidden');
  expertModal.setAttribute('aria-hidden', 'true');
  document.body.classList.remove('modal-open');
  if (expertOpenBtn) {
    expertOpenBtn.focus();
  }
}

function openExpertModal() {
  if (!expertModal) return;
  expertModal.classList.remove('hidden');
  expertModal.setAttribute('aria-hidden', 'false');
  document.body.classList.add('modal-open');
  if (expertCloseBtn) {
    expertCloseBtn.focus();
  }
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

document.addEventListener('DOMContentLoaded', () => {
  applyLoginToggleLabel('Login');
  initGoogleLogin();
});

if (loginToggle && loginDropdown) {
  loginToggle.addEventListener('click', (event) => {
    event.stopPropagation();
    if (loginDropdown.classList.contains('hidden')) {
      openLoginDropdown();
    } else {
      closeLoginDropdown();
    }
  });

  document.addEventListener('click', (event) => {
    if (!loginDropdown.contains(event.target) && !loginToggle.contains(event.target)) {
      closeLoginDropdown();
    }
  });
}

if (expertOpenBtn) {
  expertOpenBtn.addEventListener('click', () => openExpertModal());
}

[expertCloseBtn, expertModalBackdrop].forEach((element) => {
  if (element) {
    element.addEventListener('click', closeExpertModal);
  }
});

document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && expertModal && !expertModal.classList.contains('hidden')) {
    closeExpertModal();
  }
});

function handleFile(file) {
  if (file.type !== 'image/png') {
    alert('Please upload a PNG file.');
    return;
  }
  if (file.size > MAX_FILE_SIZE) {
    alert('Please upload a PNG that is 100 MB or smaller.');
    return;
  }
  originalFile = file;
  originalFileBuffer = null;
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

function setAuthProfile(profile) {
  authProfile = profile;
  if (profile && loginToggle) {
    applyLoginToggleLabel(`Hi, ${profile.name || profile.email || 'user'}`);
    loginToggle.classList.add('logged-in');
    closeLoginDropdown();
  } else if (loginToggle) {
    applyLoginToggleLabel('Login');
    loginToggle.classList.remove('logged-in');
  }
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

function initGoogleLogin() {
  const clientId =
    window.FP_GOOGLE_CLIENT_ID ||
    document.querySelector('meta[name="google-client-id"]')?.content ||
    '';
  if (!clientId || !googleLoginBtn) {
    return;
  }
  const renderButton = () => {
    if (!window.google || !google.accounts || !google.accounts.id) {
      return;
    }
    google.accounts.id.initialize({
      client_id: clientId,
      callback: handleGoogleCredential,
      auto_select: false,
    });
    google.accounts.id.renderButton(googleLoginBtn, {
      theme: 'outline',
      size: 'medium',
      shape: 'pill',
    });
  };
  if (window.google && window.google.accounts) {
    renderButton();
  } else {
    const interval = setInterval(() => {
      if (window.google && window.google.accounts) {
        clearInterval(interval);
        renderButton();
      }
    }, 300);
    setTimeout(() => clearInterval(interval), 10000);
  }
}

async function handleGoogleCredential(response) {
  const credential = response?.credential;
  if (!credential) {
    appendLog('login failed: missing credential');
    return;
  }
  try {
    const res = await fetch('/auth/google', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ credential }),
    });
    const data = await res.json();
    if (!res.ok || data.status !== 'ok') {
      throw new Error(data.message || 'Auth failed');
    }
    appendLog(`login success for ${data.email || 'unknown'}`);
    setAuthProfile({ email: data.email, name: data.name, picture: data.picture });
  } catch (err) {
    console.error(err);
    appendLog(`login failed: ${err.message}`);
  }
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

function nowMs() {
  return typeof performance !== 'undefined' && typeof performance.now === 'function'
    ? performance.now()
    : Date.now();
}

async function loadOriginalBuffer() {
  if (originalFileBuffer) {
    return originalFileBuffer;
  }
  if (!originalFile) {
    return null;
  }
  originalFileBuffer = await originalFile.arrayBuffer();
  return originalFileBuffer;
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
  const placeholder = document.createElement('p');
  placeholder.className = 'result-placeholder-text';
  placeholder.textContent = 'Estimating ETA…';
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

  const actions = document.createElement('div');
  actions.className = 'result-actions';

  const moreButton = document.createElement('button');
  moreButton.type = 'button';
  moreButton.className = 'result-action-btn';
  moreButton.textContent = 'Compress more';
  moreButton.title = 'Push compression harder for this output';
  moreButton.disabled = true;
  moreButton.addEventListener('click', () => handleCompressionFeedback('more', descriptor));

  const lessButton = document.createElement('button');
  lessButton.type = 'button';
  lessButton.className = 'result-action-btn ghost';
  lessButton.textContent = 'Compress less';
  lessButton.title = 'Ease compression to preserve fidelity';
  lessButton.disabled = true;
  lessButton.addEventListener('click', () => handleCompressionFeedback('less', descriptor));

  const revertButton = document.createElement('button');
  revertButton.type = 'button';
  revertButton.className = 'result-action-btn ghost hidden';
  revertButton.textContent = 'Restore original';
  revertButton.title = 'Go back to the first compression result';
  revertButton.disabled = true;
  revertButton.addEventListener('click', () => handleRevertToOriginal(descriptor));

  download.classList.add('result-download');

  actions.appendChild(download);
  actions.appendChild(moreButton);
  actions.appendChild(lessButton);
  actions.appendChild(revertButton);

  card.appendChild(title);
  card.appendChild(meta);
  card.appendChild(gauge);
  card.appendChild(preview);
  card.appendChild(actions);
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
    actions,
    moreButton,
    lessButton,
    revertButton,
    initialResult: null,
    initialBaseline: null,
    tuneLocked: false,
    lastBytes: null,
    ghostFrame: null,
    etaProgress: null,
    etaProgressFill: null,
    etaProgressText: null,
    etaStartTime: null,
    etaTargetMs: null,
  };
}

function getEtaMs(descriptor) {
  const key = descriptor ? resultKey(descriptor.format, descriptor.label) : '';
  if (key && etaStore.has(key)) {
    return etaStore.get(key);
  }
  return descriptor?.etaMs || 1500;
}

function ensureEtaProgress(entry) {
  if (entry.etaProgress && entry.etaProgressFill && entry.etaProgressText) {
    return;
  }
  const etaProgress = document.createElement('div');
  etaProgress.className = 'eta-progress';
  const track = document.createElement('div');
  track.className = 'eta-progress-track';
  const fill = document.createElement('div');
  fill.className = 'eta-progress-fill';
  track.appendChild(fill);
  const label = document.createElement('p');
  label.className = 'eta-progress-text';
  label.textContent = 'Estimating ETA…';
  etaProgress.appendChild(track);
  etaProgress.appendChild(label);
  entry.etaProgress = etaProgress;
  entry.etaProgressFill = fill;
  entry.etaProgressText = label;
}

function updateEtaProgress(entry, percent = 0, remainingMs = 0, message = 'Crunching…') {
  if (!entry) return;
  const clamped = Math.max(0, Math.min(100, percent));
  ensureEtaProgress(entry);
  if (entry.etaProgressFill) {
    entry.etaProgressFill.style.width = `${clamped}%`;
  }
  const remainingText = Number.isFinite(remainingMs) && remainingMs > 0
    ? `${formatDuration(Math.max(remainingMs, 0))} left`
    : 'finishing…';
  const label = `${clamped.toFixed(0)}% · ${remainingText}`;
  if (entry.etaProgressText) {
    entry.etaProgressText.textContent = label;
  }
  if (entry.placeholderEl) {
    entry.placeholderEl.textContent = message;
  }
}

function startGhostProgress(entry, descriptor, message = 'Crunching…') {
  if (!entry || !descriptor) return;
  stopGhostProgress(entry);
  ensureEtaProgress(entry);
  if (!entry.placeholderEl) {
    const placeholder = document.createElement('p');
    placeholder.className = 'result-placeholder-text';
    placeholder.textContent = message;
    entry.placeholderEl = placeholder;
  }
  entry.preview.classList.add('loading');
  entry.previewStage.innerHTML = '';
  entry.previewStage.appendChild(entry.etaProgress);
  entry.previewStage.appendChild(entry.placeholderEl);

  entry.gaugeFill.style.width = '0%';
  entry.gaugeFill.classList.remove('negative');
  entry.gauge.title = 'Crunching…';

  const etaMs = Math.max(300, getEtaMs(descriptor));
  entry.etaTargetMs = etaMs;
  entry.etaStartTime = nowMs();
  const tick = () => {
    const elapsed = nowMs() - entry.etaStartTime;
    const remaining = Math.max(etaMs - elapsed, 0);
    const predicted = etaMs > 0 ? Math.min(99, (elapsed / etaMs) * 100) : 10;
    updateEtaProgress(entry, predicted, remaining, message);
    entry.ghostFrame = requestAnimationFrame(tick);
  };
  updateEtaProgress(entry, 5, etaMs, message);
  entry.ghostFrame = requestAnimationFrame(tick);
}

function stopGhostProgress(entry, complete = false) {
  if (!entry) return;
  if (entry.ghostFrame) {
    cancelAnimationFrame(entry.ghostFrame);
    entry.ghostFrame = null;
  }
  if (complete) {
    updateEtaProgress(entry, 100, 0, 'Wrapping up…');
  }
  entry.etaStartTime = null;
  entry.etaTargetMs = null;
  entry.gaugeFill.style.width = '0%';
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
  const previousBytes = entry.lastBytes;
  const alreadyReady = entry.card.classList.contains('ready');
  stopGhostProgress(entry);
  entry.card.classList.remove('pending', 'error');
  entry.card.classList.add('ready');

  if (!entry.initialResult) {
    entry.initialResult = { ...result };
    entry.initialBaseline = baselineBytes;
  }

  const metaLine = formatResultMeta(entry, result, baselineBytes, previousBytes);
  entry.meta.textContent = metaLine;
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
  toggleActionButtons(entry, true);
  entry.lastBytes = result.bytes;
  entry.tuneLocked = Boolean(result.tuning);
  if (entry.tuneLocked) {
    toggleTuneButtons(entry, false);
    toggleRevertButton(entry, true);
  }

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
    appendLog(`ready <span>${result.format.toUpperCase()} – ${result.label}</span> → ${metaLine}${durationText}`);
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
    toggleActionButtons(entry, false);
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
  const tuningLine = result.tuning === 'more'
    ? 'Tuning: requested smaller output'
    : result.tuning === 'less'
      ? 'Tuning: requested more detail'
      : null;
  return [
    `${(result.format || '').toUpperCase()} – ${result.label || 'variant'}`,
    description,
    `MIME: ${result.mime || 'n/a'}`,
    `Profile: ${result.label || 'n/a'}`,
    `Output: ${formatBytes(result.bytes)} (${percent}% vs ${baseline})`,
    durationLine,
    tuningLine,
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

function toggleActionButtons(entry, enabled) {
  if (!entry) return;
  [entry.moreButton, entry.lessButton].forEach((button) => {
    if (!button) return;
    button.disabled = entry.tuneLocked ? true : !enabled;
  });
}

function toggleTuneButtons(entry, show) {
  if (!entry) return;
  [entry.moreButton, entry.lessButton].forEach((button) => {
    if (!button) return;
    button.classList.toggle('hidden', !show);
    button.disabled = !show;
  });
}

function toggleRevertButton(entry, show) {
  if (!entry?.revertButton) return;
  entry.revertButton.classList.toggle('hidden', !show);
  entry.revertButton.disabled = !show;
}

function buildTuneDeltaLog(descriptor, beforeBytes, afterBytes, intentLabel) {
  const formatText = descriptor?.format ? descriptor.format.toUpperCase() : 'OUTPUT';
  const labelText = descriptor?.label ? ` – ${descriptor.label}` : '';
  if (!Number.isFinite(beforeBytes) || beforeBytes <= 0) {
    return `retuned <span>${formatText}${labelText}</span> for ${intentLabel}`;
  }
  const delta = afterBytes - beforeBytes;
  const percent = beforeBytes > 0 ? (delta / beforeBytes) * 100 : 0;
  const arrow = delta === 0 ? 'no change' : delta < 0 ? 'smaller' : 'larger';
  const percentText = `${Math.abs(percent).toFixed(1)}% ${arrow}`;
  return `retuned <span>${formatText}${labelText}</span>: ${formatBytes(beforeBytes)} → ${formatBytes(afterBytes)} (${percentText})`;
}

function computeTuneDelta(beforeBytes, afterBytes, tuning) {
  if (!tuning || !Number.isFinite(beforeBytes) || beforeBytes <= 0 || !Number.isFinite(afterBytes)) {
    return '';
  }
  const delta = afterBytes - beforeBytes;
  const percent = beforeBytes ? (delta / beforeBytes) * 100 : 0;
  const direction = delta === 0 ? 'no change' : delta < 0 ? 'smaller' : 'larger';
  return ` · ${formatBytes(beforeBytes)} → ${formatBytes(afterBytes)} (${Math.abs(percent).toFixed(1)}% ${direction})`;
}

function formatResultMeta(entry, result, baselineBytes, previousBytes) {
  const original = entry?.initialBaseline || baselineBytes || currentJobContext?.inputBytes || 0;
  if (result.tuning && original > 0) {
    const parts = [`${formatBytes(original)} (original)`];
    if (Number.isFinite(previousBytes) && previousBytes > 0 && previousBytes !== original) {
      const prevPct = ((original - previousBytes) / original) * 100;
      const prefix = prevPct < 0 ? '⚠️ ' : '';
      parts.push(`→ ${prefix}${formatBytes(previousBytes)} (${prevPct.toFixed(1)}% smaller)`);
    }
    const curPct = ((original - result.bytes) / original) * 100;
    const curPrefix = curPct < 0 ? '⚠️ ' : '';
    parts.push(`→ ${curPrefix}${formatBytes(result.bytes)} (${curPct.toFixed(1)}% smaller)`);
    return parts.join(' ');
  }
  const savings = baselineBytes ? ((baselineBytes - result.bytes) / baselineBytes) * 100 : 0;
  const savingsText = savings ? ` (${savings.toFixed(1)}% smaller)` : '';
  return `${formatBytes(result.bytes)}${savingsText}`;
}

function resetResultCardForRetune(entry, intentLabel = '') {
  if (!entry) return;
  stopGhostProgress(entry);
  entry.card.classList.remove('ready', 'error');
  entry.card.classList.add('pending');
  entry.meta.textContent = intentLabel ? `Retuning for ${intentLabel}…` : 'Retuning…';
  entry.preview.classList.add('loading');
  entry.previewStage.innerHTML = '';
  const placeholder = entry.placeholderEl || document.createElement('p');
  placeholder.className = 'result-placeholder-text';
  placeholder.textContent = intentLabel ? `Retuning for ${intentLabel}…` : 'Encoding…';
  entry.placeholderEl = placeholder;
  startGhostProgress(entry, entry.descriptor, placeholder.textContent);
  entry.download.classList.add('disabled');
  entry.download.setAttribute('aria-disabled', 'true');
  entry.download.removeAttribute('href');
  entry.download.textContent = 'Preparing…';
  toggleRevertButton(entry, false);
  entry.gaugeFill.style.width = '0%';
  entry.gaugeFill.classList.remove('negative');
  entry.gauge.title = 'Crunching…';
}

function handleRevertToOriginal(descriptor = {}) {
  const targetKey = resultKey(descriptor.format, descriptor.label);
  const entry = resultCards.get(targetKey);
  if (!entry || !entry.initialResult) return;
  entry.tuneLocked = false;
  toggleActionButtons(entry, true);
  toggleRevertButton(entry, false);
  toggleTuneButtons(entry, true);
  const baseline = entry.initialBaseline || currentJobContext?.inputBytes || 0;
  populateResultCard({ ...entry.initialResult }, baseline, entry.initialResult.filename || currentJobContext?.filename || '');
}

async function handleCompressionFeedback(direction, descriptor = {}) {
  const targetKey = resultKey(descriptor.format, descriptor.label);
  const entry = resultCards.get(targetKey);
  if (!descriptor?.format || !entry) {
    appendLog('error → No matching output to retune.');
    return;
  }
  if (!originalFile) {
    appendLog('error → Original file missing; upload again to retune.');
    alert('Upload a PNG before asking for more or less compression.');
    return;
  }

  const intentLabel = direction === 'more' ? 'smaller file' : 'more detail';
  appendLog(`retuning <span>${descriptor.format?.toUpperCase() || 'OUTPUT'} – ${descriptor.label || ''}</span> for ${intentLabel}`);
  resetResultCardForRetune(entry, intentLabel);
  entry.tuneLocked = true;
  toggleActionButtons(entry, false);
  toggleTuneButtons(entry, false);

  try {
    const buffer = await loadOriginalBuffer();
    if (!buffer) {
      throw new Error('Original file unavailable.');
    }
    const jobId = nextJobId();
    const response = await fetch('/api/compress', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/octet-stream',
        'X-Filename': originalFile.name,
        'X-Job-ID': String(jobId),
        'X-Tune-Format': descriptor.format,
        ...(descriptor.label ? { 'X-Tune-Label': descriptor.label } : {}),
        'X-Tune-Intent': direction,
      },
      body: buffer,
    });

    let payload = null;
    try {
      payload = await response.json();
    } catch (error) {
      if (!response.ok) {
        throw new Error('Failed to parse server response.');
      }
      throw error;
    }
    if (!response.ok) {
      throw new Error(payload?.message || 'Compression failed.');
    }

    const baseline = typeof payload.inputBytes === 'number' && payload.inputBytes > 0
      ? payload.inputBytes
      : currentJobContext?.inputBytes || originalFile.size || 0;
    const filename = payload.filename || currentJobContext?.filename || originalFile.name || '';
    const updated = (payload.results || []).find((result) => resultKey(result.format, result.label) === targetKey);
    if (updated) {
      const previousBytes = entry.lastBytes;
      populateResultCard(updated, baseline, filename);
      appendLog(buildTuneDeltaLog(descriptor, previousBytes, updated.bytes, intentLabel));
      toggleRevertButton(entry, true);
      appendLog(`ready <span>${descriptor.format?.toUpperCase() || 'OUTPUT'} – ${descriptor.label || ''}</span> tuned for ${intentLabel}`);
    } else {
      throw new Error('Tuned output missing from server response.');
    }
  } catch (error) {
    entry.meta.textContent = error.message || 'Unable to retune output.';
    appendLog(`error → ${error.message || error}`);
  } finally {
    toggleActionButtons(entry, true);
    entry.card.classList.remove('pending');
  }
}
