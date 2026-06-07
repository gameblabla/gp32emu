const VIDEO_WIDTH = 320;
const VIDEO_HEIGHT = 240;
const canvas = document.getElementById('screen');
let ctx = null;
let sourceCanvas = null;
let sourceCtx = null;
let imageData = null;
let gl = null;
let glProgram = null;
let glTexture = null;
let glPositionBuffer = null;
let glTexcoordBuffer = null;
let glSamplerLocation = null;
let glLastFilter = '';
let videoRenderer = '2d';
let lastFrameRgbaCopy = null;
let frameImageReady = false;
let lastCanvasBackingWidth = 0;
let lastCanvasBackingHeight = 0;
let lastVideoCssWidth = 0;
let lastVideoCssHeight = 0;
const hud = document.getElementById('hud');
const logEl = document.getElementById('log');
const stateText = document.getElementById('stateText');
const frameText = document.getElementById('frameText');
const pcText = document.getElementById('pcText');
const audioText = document.getElementById('audioText');
const gamepadStatusText = document.getElementById('gamepadStatusText');
const hleMode = document.getElementById('hleMode');
const startBtn = document.getElementById('startBtn');
const pauseBtn = document.getElementById('pauseBtn');
const dropZone = document.getElementById('dropZone');
const screenWrap = document.getElementById('screenWrap');
const menuToggle = document.getElementById('menuToggle');
const panelClose = document.getElementById('panelClose');
const fullscreenToggle = document.getElementById('fullscreenToggle');
const stateSlot = document.getElementById('stateSlot');
const saveStateBtn = document.getElementById('saveStateBtn');
const loadStateBtn = document.getElementById('loadStateBtn');
const exportStateBtn = document.getElementById('exportStateBtn');
const storageText = document.getElementById('storageText');
const keyBindingsEl = document.getElementById('keyBindings');
const resetKeyMapBtn = document.getElementById('resetKeyMap');
const gamepadSelect = document.getElementById('gamepadSelect');
const gamepadText = document.getElementById('gamepadText');
const gamepadBindingsEl = document.getElementById('gamepadBindings');
const resetGamepadMapBtn = document.getElementById('resetGamepadMap');
const touchGamepad = document.getElementById('touchGamepad');
const touchDpad = document.getElementById('touchDpad');
const touchKnob = document.getElementById('touchKnob');
const touchOpacity = document.getElementById('touchOpacity');
const touchOverlayEnabled = document.getElementById('touchOverlayEnabled');
const fpsHudEnabled = document.getElementById('fpsHudEnabled');
const scaleMode = document.getElementById('scaleMode');
const filterMode = document.getElementById('filterMode');
const pauseWhenHidden = document.getElementById('pauseWhenHidden');
const lcdPersistence = document.getElementById('lcdPersistence');
const frameInterpolation = document.getElementById('frameInterpolation');

const GP32_BUTTON_A      = 1 << 0;
const GP32_BUTTON_B      = 1 << 1;
const GP32_BUTTON_L      = 1 << 2;
const GP32_BUTTON_R      = 1 << 3;
const GP32_BUTTON_START  = 1 << 4;
const GP32_BUTTON_SELECT = 1 << 5;
const GP32_BUTTON_UP     = 1 << 6;
const GP32_BUTTON_DOWN   = 1 << 7;
const GP32_BUTTON_LEFT   = 1 << 8;
const GP32_BUTTON_RIGHT  = 1 << 9;

const BUTTONS = [
  { name: 'Up', label: '↑', mask: GP32_BUTTON_UP },
  { name: 'Down', label: '↓', mask: GP32_BUTTON_DOWN },
  { name: 'Left', label: '←', mask: GP32_BUTTON_LEFT },
  { name: 'Right', label: '→', mask: GP32_BUTTON_RIGHT },
  { name: 'A', label: 'A', mask: GP32_BUTTON_A },
  { name: 'B', label: 'B', mask: GP32_BUTTON_B },
  { name: 'L', label: 'L', mask: GP32_BUTTON_L },
  { name: 'R', label: 'R', mask: GP32_BUTTON_R },
  { name: 'Start', label: 'Start', mask: GP32_BUTTON_START },
  { name: 'Select', label: 'Select', mask: GP32_BUTTON_SELECT }
];
const BUTTON_BY_NAME = new Map(BUTTONS.map(b => [b.name, b]));

const DEFAULT_KEYS = Object.freeze({
  Up: 'ArrowUp', Down: 'ArrowDown', Left: 'ArrowLeft', Right: 'ArrowRight',
  A: 'KeyZ', B: 'KeyX', L: 'KeyA', R: 'KeyS', Start: 'Enter', Select: 'ShiftLeft'
});
const KEY_LABELS = new Map([
  ['ArrowUp', 'Arrow Up'], ['ArrowDown', 'Arrow Down'], ['ArrowLeft', 'Arrow Left'], ['ArrowRight', 'Arrow Right'],
  ['ShiftLeft', 'Left Shift'], ['ShiftRight', 'Right Shift'], ['Enter', 'Enter'], ['Space', 'Space'], ['Escape', 'Esc'], ['Backspace', 'Backspace']
]);
const DEFAULT_GAMEPAD = Object.freeze({
  Up: ['axis:1:-', 'button:12'],
  Down: ['axis:1:+', 'button:13'],
  Left: ['axis:0:-', 'button:14'],
  Right: ['axis:0:+', 'button:15'],
  A: ['button:0'],
  B: ['button:1'],
  L: ['button:4'],
  R: ['button:5'],
  Start: ['button:9'],
  Select: ['button:8']
});
const GAMEPAD_AXIS_DEADZONE = 0.42;
const GAMEPAD_REMAP_THRESHOLD = 0.65;
const SCALE_MODES = new Set(['integer', 'aspect', 'stretch']);
const FILTER_MODES = new Set(['nearest', 'linear', 'sharp-bilinear']);
const GP32_FRAME_RATE = 60;
const GP32_FRAME_MS = 1000 / GP32_FRAME_RATE;
const MAX_CATCHUP_FRAMES_PER_RAF = 2;
const MAX_FRAME_ACCUM_MS = GP32_FRAME_MS * 3;
const AUDIO_CRITICAL_MS = 28;
const SHARP_BILINEAR_MAX_PRESCALE = 3;
const STATUS_DOM_UPDATE_MS = 250;
const AUDIO_DOM_UPDATE_MS = 250;
function defaultGamepadMap() { return Object.fromEntries(BUTTONS.map(b => [b.name, [...DEFAULT_GAMEPAD[b.name]]])); }
function enumValue(value, allowed, fallback) { return allowed.has(value) ? value : fallback; }
const STORAGE_CONFIG = 'gp32.wasm.config.v2';
const STORAGE_STATE_PREFIX = 'gp32.wasm.savestate.v1.';
const DB_NAME = 'gp32emu-wasm';
const DB_VERSION = 1;
const STORE_FILES = 'files';
const DB_STATE_PREFIX = 'state:';

let wasm, memory, exp;
let db = null;
let dbReady = false;
let running = false;
let buttons = 0;
let keyboardButtons = 0;
let panelButtons = 0;
let touchButtons = 0;
let gamepadButtons = 0;
let raf = 0;
let gamepadRaf = 0;
let biosLoaded = false;
let gameLoaded = false;
let biosMeta = null;
let gameMeta = null;
let currentLaunchUsesHle = true;
let lastFpsTime = performance.now();
let framesThisSecond = 0;
let displayFramesThisSecond = 0;
let remapTarget = null;
let padRemapTarget = null;
let gamepadRemapBaseline = new Set();
let lastGamepadListSignature = '';
let lastGamepadStatusText = '';
let activeDpadPointer = null;
let activeDpadRect = null;
let dpadMask = 0;
let audioCtx = null;
let scriptNode = null;
let audioWorkletNode = null;
let audioQueue = [];
let audioQueuedFrames = 0;
let audioUnderruns = 0;
let audioStarted = false;
let pseudoFullscreen = false;
let hiddenPauseWasRunning = false;
let frameLimiterLastTime = 0;
let frameLimiterAccumMs = 0;
let videoLayoutDirty = true;
let currentVideoLayout = { cssW: VIDEO_WIDTH, cssH: VIDEO_HEIGHT, backingW: VIDEO_WIDTH, backingH: VIDEO_HEIGHT, sharp: false };
let lastDrawnCoreFrame = -1;
let lastStatsDomTime = 0;
let lastAudioDomTime = 0;

const coarsePointerMedia = typeof window !== 'undefined' && window.matchMedia ? window.matchMedia('(pointer: coarse)') : null;

function normalizeGamepadMap(raw) {
  const pads = defaultGamepadMap();
  for (const b of BUTTONS) {
    const value = raw?.[b.name];
    if (Array.isArray(value)) {
      const clean = value.filter(v => typeof v === 'string' && /^(button:\d+|axis:\d+:[+-])$/.test(v)).slice(0, 4);
      if (clean.length) pads[b.name] = clean;
    } else if (typeof value === 'string' && /^(button:\d+|axis:\d+:[+-])$/.test(value)) {
      pads[b.name] = [value];
    }
  }
  return pads;
}
function loadConfig() {
  try {
    const raw = localStorage.getItem(STORAGE_CONFIG);
    if (raw) {
      const parsed = JSON.parse(raw);
      const keys = { ...DEFAULT_KEYS };
      for (const b of BUTTONS) if (typeof parsed.keys?.[b.name] === 'string') keys[b.name] = parsed.keys[b.name];
      return {
        keys,
        gamepad: normalizeGamepadMap(parsed.gamepad),
        gamepadIndex: Number.isInteger(parsed.gamepadIndex) ? Math.max(-1, Math.min(15, parsed.gamepadIndex)) : -1,
        stateSlot: Number.isInteger(parsed.stateSlot) ? Math.max(0, Math.min(9, parsed.stateSlot)) : 0,
        touchOpacity: Math.max(0.25, Math.min(1, Number(parsed.touchOpacity ?? 0.76))),
        touchOverlayEnabled: parsed.touchOverlayEnabled !== false,
        showFpsHud: parsed.showFpsHud === true,
        scaleMode: enumValue(parsed.scaleMode, SCALE_MODES, 'integer'),
        filterMode: enumValue(parsed.filterMode, FILTER_MODES, 'nearest'),
        pauseWhenHidden: parsed.pauseWhenHidden !== false,
        lcdPersistence: parsed.lcdPersistence === true,
        frameInterpolation: parsed.frameInterpolation === true
      };
    }
  } catch (_) {}
  return { keys: { ...DEFAULT_KEYS }, gamepad: defaultGamepadMap(), gamepadIndex: -1, stateSlot: 0, touchOpacity: 0.76, touchOverlayEnabled: true, showFpsHud: false, scaleMode: 'integer', filterMode: 'nearest', pauseWhenHidden: true, lcdPersistence: false, frameInterpolation: false };
}
const config = loadConfig();

function saveConfig() {
  try {
    localStorage.setItem(STORAGE_CONFIG, JSON.stringify({ keys: config.keys, gamepad: config.gamepad, gamepadIndex: config.gamepadIndex, stateSlot: config.stateSlot, touchOpacity: config.touchOpacity, touchOverlayEnabled: config.touchOverlayEnabled, showFpsHud: config.showFpsHud === true, scaleMode: config.scaleMode, filterMode: config.filterMode, pauseWhenHidden: config.pauseWhenHidden !== false, lcdPersistence: config.lcdPersistence === true, frameInterpolation: config.frameInterpolation === true }));
  } catch (_) {}
}

function log(msg) { logEl.textContent = msg || ''; }
function setState(msg) { stateText.textContent = msg; if (config.showFpsHud === true) hud.textContent = msg; }
function setStorageStatus(msg, tone = 'muted') {
  if (!storageText) return;
  storageText.textContent = msg || '';
  storageText.className = tone === 'ok' ? 'status-ok' : tone === 'bad' ? 'status-bad' : tone === 'warn' ? 'status-warn' : 'hint';
}
function refreshAudioStatus(force = false) {
  if (!audioText) return;
  const now = performance.now();
  if (!force && now - lastAudioDomTime < AUDIO_DOM_UPDATE_MS) return;
  lastAudioDomTime = now;
  audioText.textContent = `${Math.max(0, audioQueuedFrames)} queued${audioUnderruns ? `, ${audioUnderruns} underruns` : ''}`;
}
function hex8(v) { return (v >>> 0).toString(16).padStart(8, '0'); }
function cstr(ptr) {
  if (!ptr || !memory) return '';
  const u8 = new Uint8Array(memory.buffer);
  let end = ptr;
  while (end < u8.length && u8[end]) end++;
  return new TextDecoder().decode(u8.subarray(ptr, end));
}
function errorText() {
  const msg = exp ? cstr(exp.gp32_wasm_get_error_ptr()) : '';
  const clean = (msg || '').replace(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]+/g, ' ').trim();
  return clean || (exp ? `error ${exp.gp32_wasm_get_error()}` : 'WASM not ready');
}
function wasmU8() { return new Uint8Array(memory.buffer); }
function copyToWasm(bytes) {
  const p = exp.gp32_wasm_malloc(bytes.byteLength || 1);
  new Uint8Array(memory.buffer, p, bytes.byteLength).set(bytes);
  return p;
}
function copyStringToWasm(s) {
  const bytes = new TextEncoder().encode(s || '');
  const p = exp.gp32_wasm_malloc(bytes.byteLength || 1);
  new Uint8Array(memory.buffer, p, bytes.byteLength).set(bytes);
  return { ptr: p, len: bytes.byteLength };
}
async function fileBytes(file) {
  if (!file) return new Uint8Array(0);
  if (file instanceof Uint8Array) return file.slice();
  if (file instanceof ArrayBuffer) return new Uint8Array(file);
  if (ArrayBuffer.isView(file)) return new Uint8Array(file.buffer, file.byteOffset, file.byteLength).slice();
  if (typeof file.bytes === 'function') {
    const b = await file.bytes();
    if (b instanceof Uint8Array) return b.slice();
    if (b instanceof ArrayBuffer) return new Uint8Array(b);
    if (ArrayBuffer.isView(b)) return new Uint8Array(b.buffer, b.byteOffset, b.byteLength).slice();
  }
  return new Uint8Array(await file.arrayBuffer());
}
function isBios(name) { return /bios|gp32.*\.(bin|rom)$/i.test(name || ''); }
function isGame(name) { return /\.(smc|fpk|fxe|gxb|gxe|bin)$/i.test(name || ''); }

function hashBytesForId(bytes) {
  let h = 2166136261 >>> 0;
  const mix = v => { h ^= v & 0xff; h = Math.imul(h, 16777619) >>> 0; };
  const len = bytes.byteLength >>> 0;
  mix(len); mix(len >>> 8); mix(len >>> 16); mix(len >>> 24);
  const sample = Math.min(bytes.byteLength, 1024 * 1024);
  for (let i = 0; i < sample; i++) mix(bytes[i]);
  if (bytes.byteLength > sample) {
    const start = Math.max(sample, bytes.byteLength - sample);
    for (let i = start; i < bytes.byteLength; i++) mix(bytes[i]);
  }
  return h.toString(16).padStart(8, '0');
}
function mediaMeta(file, bytes, kind) {
  return { kind, name: file?.name || kind, size: bytes.byteLength >>> 0, hash: hashBytesForId(bytes) };
}
function mediaStateFingerprint() {
  const m = gameMeta || biosMeta || { kind: 'empty', name: 'none', size: 0, hash: '00000000' };
  const mode = gameMeta ? (currentLaunchUsesHle ? 'hle' : 'bios') : 'bios-only';
  return ['gp32', mode, m.kind, String(m.size), m.hash, (m.name || '').toLowerCase()].join('.');
}
function stateKey() { return `${STORAGE_STATE_PREFIX}${mediaStateFingerprint()}.${config.stateSlot}`; }
function stateDbKey() { return `${DB_STATE_PREFIX}${stateKey()}`; }
function stateExportName() {
  const base = (gameMeta?.name || biosMeta?.name || 'gp32').replace(/\.[^.]+$/, '');
  const clean = base.normalize('NFKD').replace(/[^a-z0-9._-]+/gi, '_').replace(/^_+|_+$/g, '').slice(0, 80) || 'gp32';
  return `${clean}_slot${String(config.stateSlot).padStart(2, '0')}.gp32st`;
}

function looksLikeGp32State(bytes) {
  if (!bytes || bytes.byteLength < 16) return false;
  const magic = [0x47, 0x50, 0x33, 0x32, 0x53, 0x54, 0x41, 0x54, 0x45, 0x76, 0x30, 0x30, 0x30, 0x32, 0x00, 0x00];
  for (let i = 0; i < magic.length; i++) if (bytes[i] !== magic[i]) return false;
  return true;
}

function openDb() {
  return new Promise((resolve, reject) => {
    if (!('indexedDB' in window)) return reject(new Error('IndexedDB unavailable'));
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      if (!req.result.objectStoreNames.contains(STORE_FILES)) req.result.createObjectStore(STORE_FILES);
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error || new Error('IndexedDB open failed'));
  });
}
function dbPut(key, value) {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_FILES, 'readwrite');
    tx.objectStore(STORE_FILES).put(value, key);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error || new Error(`IndexedDB put failed: ${key}`));
  });
}
function dbGet(key) {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_FILES, 'readonly');
    const req = tx.objectStore(STORE_FILES).get(key);
    req.onsuccess = () => resolve(req.result || null);
    req.onerror = () => reject(req.error || new Error(`IndexedDB get failed: ${key}`));
  });
}
function bytesToBase64(bytes) {
  let text = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) text += String.fromCharCode(...bytes.subarray(i, i + chunk));
  return btoa(text);
}
function base64ToBytes(raw) {
  const text = atob(raw);
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i);
  return bytes;
}
function normalizeStoredBytes(value) {
  if (!value) return null;
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  return null;
}
async function putStateBytes(bytes) {
  if (dbReady && db) {
    await dbPut(stateDbKey(), bytes);
    try { localStorage.removeItem(stateKey()); } catch (_) {}
    return 'IndexedDB';
  }
  localStorage.setItem(stateKey(), bytesToBase64(bytes));
  return 'localStorage fallback';
}
async function getStateBytes() {
  if (dbReady && db) {
    const bytes = normalizeStoredBytes(await dbGet(stateDbKey()));
    if (bytes) return bytes;
  }
  try {
    const raw = localStorage.getItem(stateKey());
    return raw ? base64ToBytes(raw) : null;
  } catch (_) { return null; }
}
async function initStorage() {
  if (stateSlot) {
    stateSlot.innerHTML = '';
    for (let i = 0; i < 10; i++) {
      const opt = document.createElement('option');
      opt.value = String(i);
      opt.textContent = `Slot ${i}`;
      stateSlot.append(opt);
    }
    stateSlot.value = String(config.stateSlot);
  }
  try {
    db = await openDb();
    dbReady = true;
    setStorageStatus('IndexedDB ready for save states; localStorage stores controls/config.', 'ok');
  } catch (err) {
    db = null;
    dbReady = false;
    setStorageStatus('IndexedDB unavailable; save states will use localStorage fallback and may hit quota.', 'warn');
  }
}

async function initWasm() {
  await initStorage();
  renderKeyBindings();
  renderGamepadBindings();
  initVideoRenderer();
  applyTouchOpacity();
  applyHudVisibility();
  applyVideoSettings();
  updateTouchControlsAvailability();
  updateFullscreenButton();
  const imports = { pcfx: { read_host_file: () => 0 } };
  const res = await fetch('gp32_wasm_core.wasm');
  const bin = await res.arrayBuffer();
  const mod = await WebAssembly.instantiate(bin, imports);
  wasm = mod.instance;
  exp = wasm.exports;
  memory = exp.memory;
  const rate = await ensureAudio(false);
  exp.gp32_wasm_init(rate || 44100);
  applyVideoEffectSettings();
  setState('ready');
}

async function ensureAudio(resume = true) {
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return 44100;
  if (!audioCtx) {
    audioCtx = new AC({ latencyHint: 'interactive' });
    const sr = Math.round(audioCtx.sampleRate || 44100);
    try {
      if (audioCtx.audioWorklet) {
        await audioCtx.audioWorklet.addModule('gp32-audio-worklet.js');
        audioWorkletNode = new AudioWorkletNode(audioCtx, 'gp32-audio-processor', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
        audioWorkletNode.port.onmessage = ev => {
          const m = ev.data || {};
          if (m.type === 'stat') {
            audioQueuedFrames = Math.max(0, audioQueuedFrames - (m.consumed || 0));
            if (typeof m.queued === 'number') audioQueuedFrames = Math.max(0, m.queued | 0);
            audioUnderruns += m.underruns || 0;
            audioStarted = !!m.started;
            refreshAudioStatus(false);
          }
        };
        audioWorkletNode.port.postMessage({ type: 'config', preroll: Math.floor(sr * 0.12), lowWater: Math.floor(sr * 0.05), maxQueue: Math.floor(sr * 0.45) });
        audioWorkletNode.connect(audioCtx.destination);
      }
    } catch (err) {
      console.warn('AudioWorklet unavailable, falling back to ScriptProcessor:', err);
      audioWorkletNode = null;
    }
    if (!audioWorkletNode) {
      scriptNode = audioCtx.createScriptProcessor(2048, 0, 2);
      scriptNode.onaudioprocess = ev => {
        const l = ev.outputBuffer.getChannelData(0);
        const r = ev.outputBuffer.getChannelData(1);
        let pos = 0;
        while (pos < l.length) {
          if (!audioQueue.length) {
            for (; pos < l.length; ++pos) { l[pos] = 0; r[pos] = 0; }
            audioUnderruns++;
            break;
          }
          const head = audioQueue[0];
          const avail = head.frames - head.pos;
          const n = Math.min(avail, l.length - pos);
          for (let i = 0; i < n; ++i) {
            const si = (head.pos + i) * 2;
            l[pos + i] = head.data[si] / 32768;
            r[pos + i] = head.data[si + 1] / 32768;
          }
          head.pos += n;
          pos += n;
          audioQueuedFrames -= n;
          if (head.pos >= head.frames) audioQueue.shift();
        }
        refreshAudioStatus(false);
      };
      scriptNode.connect(audioCtx.destination);
    }
  }
  if (resume && audioCtx.state !== 'running') await audioCtx.resume();
  return Math.round(audioCtx.sampleRate || 44100);
}
function resetAudioQueues() {
  audioQueue = [];
  audioQueuedFrames = 0;
  audioUnderruns = 0;
  audioStarted = false;
  if (audioWorkletNode) audioWorkletNode.port.postMessage({ type: 'reset' });
  refreshAudioStatus(true);
}
function pushAudio() {
  const frames = exp.gp32_wasm_get_audio_frames();
  if (!frames) return;
  const ptr = exp.gp32_wasm_get_audio_ptr();
  const src = new Int16Array(memory.buffer, ptr, frames * 2);
  const copy = new Int16Array(src.length);
  copy.set(src);
  if (audioWorkletNode) {
    audioWorkletNode.port.postMessage({ type: 'audio', data: copy, frames }, [copy.buffer]);
    audioQueuedFrames += frames;
  } else {
    audioQueue.push({ data: copy, frames, pos: 0 });
    audioQueuedFrames += frames;
    const maxQueued = Math.floor((audioCtx ? audioCtx.sampleRate : 44100) * 0.45);
    while (audioQueuedFrames > maxQueued && audioQueue.length > 2) {
      audioQueuedFrames -= audioQueue[0].frames - audioQueue[0].pos;
      audioQueue.shift();
    }
  }
  exp.gp32_wasm_audio_consume();
}
function compileGlShader(type, source) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const msg = gl.getShaderInfoLog(shader) || 'shader compile failed';
    gl.deleteShader(shader);
    throw new Error(msg);
  }
  return shader;
}
function initWebGlRenderer() {
  const candidate = canvas.getContext('webgl', { alpha: false, antialias: false, depth: false, stencil: false, preserveDrawingBuffer: false }) ||
                    canvas.getContext('experimental-webgl', { alpha: false, antialias: false, depth: false, stencil: false, preserveDrawingBuffer: false });
  if (!candidate) return false;
  gl = candidate;
  const vs = compileGlShader(gl.VERTEX_SHADER, `
    attribute vec2 a_pos;
    attribute vec2 a_uv;
    varying vec2 v_uv;
    void main() {
      gl_Position = vec4(a_pos, 0.0, 1.0);
      v_uv = a_uv;
    }
  `);
  const fs = compileGlShader(gl.FRAGMENT_SHADER, `
    precision mediump float;
    varying vec2 v_uv;
    uniform sampler2D u_tex;
    void main() {
      gl_FragColor = texture2D(u_tex, v_uv);
    }
  `);
  glProgram = gl.createProgram();
  gl.attachShader(glProgram, vs);
  gl.attachShader(glProgram, fs);
  gl.linkProgram(glProgram);
  gl.deleteShader(vs);
  gl.deleteShader(fs);
  if (!gl.getProgramParameter(glProgram, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(glProgram) || 'program link failed');
  gl.useProgram(glProgram);

  glPositionBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, glPositionBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1,  1, -1, -1,  1,  -1,  1,  1, -1,  1,  1]), gl.STATIC_DRAW);
  const posLoc = gl.getAttribLocation(glProgram, 'a_pos');
  gl.enableVertexAttribArray(posLoc);
  gl.vertexAttribPointer(posLoc, 2, gl.FLOAT, false, 0, 0);

  glTexcoordBuffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, glTexcoordBuffer);
  /* Uploads are already in display orientation; flip V so WebGL's texture
     coordinate origin does not invert the final canvas image. */
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 1,  1, 1,  0, 0,  0, 0,  1, 1,  1, 0]), gl.STATIC_DRAW);
  const uvLoc = gl.getAttribLocation(glProgram, 'a_uv');
  gl.enableVertexAttribArray(uvLoc);
  gl.vertexAttribPointer(uvLoc, 2, gl.FLOAT, false, 0, 0);

  glTexture = gl.createTexture();
  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, glTexture);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, VIDEO_WIDTH, VIDEO_HEIGHT, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
  glSamplerLocation = gl.getUniformLocation(glProgram, 'u_tex');
  gl.uniform1i(glSamplerLocation, 0);
  gl.clearColor(0.04, 0.05, 0.06, 1.0);
  gl.clear(gl.COLOR_BUFFER_BIT);
  videoRenderer = 'webgl';
  return true;
}
function initCanvas2dRenderer() {
  ctx = canvas.getContext('2d', { alpha: false });
  sourceCanvas = document.createElement('canvas');
  sourceCanvas.width = VIDEO_WIDTH;
  sourceCanvas.height = VIDEO_HEIGHT;
  sourceCtx = sourceCanvas.getContext('2d', { alpha: false });
  imageData = ctx.createImageData(VIDEO_WIDTH, VIDEO_HEIGHT);
  videoRenderer = '2d';
}
function initVideoRenderer() {
  if (videoRenderer !== '2d' || ctx || gl) return;
  try {
    if (initWebGlRenderer()) return;
  } catch (err) {
    console.warn('WebGL video renderer unavailable, using 2D canvas:', err);
    gl = null;
  }
  initCanvas2dRenderer();
}
function setGlFilterForLayout(layout) {
  if (!gl) return;
  const filter = (config.filterMode === 'nearest' || layout.sharp) ? gl.NEAREST : gl.LINEAR;
  const key = String(filter);
  if (glLastFilter === key) return;
  gl.bindTexture(gl.TEXTURE_2D, glTexture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
  glLastFilter = key;
}
function drawFrameWebGl(src, layout) {
  gl.viewport(0, 0, canvas.width, canvas.height);
  gl.useProgram(glProgram);
  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, glTexture);
  setGlFilterForLayout(layout);
  gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, VIDEO_WIDTH, VIDEO_HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, src);
  gl.drawArrays(gl.TRIANGLES, 0, 6);
}
function redrawFrameWebGl(layout) {
  if (!frameImageReady || !exp || !memory) {
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clear(gl.COLOR_BUFFER_BIT);
    return;
  }
  const ptr = exp.gp32_wasm_get_framebuffer();
  const src = new Uint8Array(memory.buffer, ptr, VIDEO_WIDTH * VIDEO_HEIGHT * 4);
  drawFrameWebGl(src, layout);
}
function markVideoLayoutDirty() {
  videoLayoutDirty = true;
}
function computeVideoLayout() {
  const rect = screenWrap?.getBoundingClientRect?.() || { width: window.innerWidth || VIDEO_WIDTH, height: window.innerHeight || VIDEO_HEIGHT };
  const availW = Math.max(1, Math.floor(rect.width || VIDEO_WIDTH));
  const availH = Math.max(1, Math.floor(rect.height || VIDEO_HEIGHT));
  const scaleX = availW / VIDEO_WIDTH;
  const scaleY = availH / VIDEO_HEIGHT;
  let cssW, cssH;
  switch (config.scaleMode) {
    case 'stretch':
      cssW = availW;
      cssH = availH;
      break;
    case 'aspect': {
      const scale = Math.max(0.01, Math.min(scaleX, scaleY));
      cssW = Math.max(1, Math.floor(VIDEO_WIDTH * scale));
      cssH = Math.max(1, Math.floor(VIDEO_HEIGHT * scale));
      break;
    }
    case 'integer':
    default: {
      const fit = Math.min(scaleX, scaleY);
      const scale = fit >= 1 ? Math.max(1, Math.floor(fit)) : Math.max(0.01, fit);
      cssW = Math.max(1, Math.floor(VIDEO_WIDTH * scale));
      cssH = Math.max(1, Math.floor(VIDEO_HEIGHT * scale));
      break;
    }
  }
  return { cssW, cssH };
}
function setCanvasBacking(width, height) {
  width = Math.max(1, width | 0);
  height = Math.max(1, height | 0);
  if (lastCanvasBackingWidth !== width || lastCanvasBackingHeight !== height) {
    canvas.width = width;
    canvas.height = height;
    lastCanvasBackingWidth = width;
    lastCanvasBackingHeight = height;
  }
}
function applyVideoLayout(force = false) {
  if (!force && !videoLayoutDirty) return currentVideoLayout;
  const layout = computeVideoLayout();
  if (layout.cssW !== lastVideoCssWidth || layout.cssH !== lastVideoCssHeight) {
    canvas.style.setProperty('--screen-css-width', `${layout.cssW}px`);
    canvas.style.setProperty('--screen-css-height', `${layout.cssH}px`);
    lastVideoCssWidth = layout.cssW;
    lastVideoCssHeight = layout.cssH;
  }
  const sharp = config.filterMode === 'sharp-bilinear';
  canvas.style.setProperty('--screen-image-rendering', config.filterMode === 'nearest' ? 'pixelated' : 'auto');
  let backingW = VIDEO_WIDTH;
  let backingH = VIDEO_HEIGHT;
  if (sharp) {
    /* Sharp-bilinear is the only mode that needs a larger backing canvas.  Cap
       the integer pre-scale so high-DPI phones do not force a 4x/5x canvas
       upload every video frame.  The browser still performs the final
       fractional pass, but the hot path stays close to a 720p-sized blit. */
    const ix = Math.max(1, Math.min(SHARP_BILINEAR_MAX_PRESCALE, Math.floor(layout.cssW / VIDEO_WIDTH)));
    const iy = Math.max(1, Math.min(SHARP_BILINEAR_MAX_PRESCALE, Math.floor(layout.cssH / VIDEO_HEIGHT)));
    const integerScale = Math.max(1, Math.min(ix, iy));
    backingW = VIDEO_WIDTH * integerScale;
    backingH = VIDEO_HEIGHT * integerScale;
  }
  setCanvasBacking(backingW, backingH);
  currentVideoLayout = { ...layout, backingW, backingH, sharp };
  videoLayoutDirty = false;
  return currentVideoLayout;
}
function redrawLastFrame() {
  if (!ctx && !gl) initVideoRenderer();
  if (!ctx && !gl) return;
  const layout = applyVideoLayout(true);
  if (videoRenderer === 'webgl') {
    redrawFrameWebGl(layout);
    return;
  }
  if (!frameImageReady) {
    ctx.fillStyle = '#0a0c0f';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    return;
  }
  if (layout.sharp && (layout.backingW !== VIDEO_WIDTH || layout.backingH !== VIDEO_HEIGHT)) {
    sourceCtx.putImageData(imageData, 0, 0);
    ctx.imageSmoothingEnabled = false;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(sourceCanvas, 0, 0, layout.backingW, layout.backingH);
  } else {
    ctx.imageSmoothingEnabled = config.filterMode !== 'nearest';
    ctx.putImageData(imageData, 0, 0);
  }
}
function drawFrame() {
  const coreFrame = exp.gp32_wasm_get_frame_count ? exp.gp32_wasm_get_frame_count() : 0;
  if (coreFrame === lastDrawnCoreFrame && frameImageReady && !videoLayoutDirty) return;
  if (videoLayoutDirty) applyVideoLayout(true);
  const ptr = exp.gp32_wasm_get_framebuffer();
  const src = new Uint8Array(memory.buffer, ptr, VIDEO_WIDTH * VIDEO_HEIGHT * 4);
  frameImageReady = true;
  lastDrawnCoreFrame = coreFrame;
  const layout = currentVideoLayout;
  if (videoRenderer === 'webgl') {
    drawFrameWebGl(src, layout);
    return;
  }
  imageData.data.set(src);
  if (layout.sharp && (layout.backingW !== VIDEO_WIDTH || layout.backingH !== VIDEO_HEIGHT)) {
    sourceCtx.putImageData(imageData, 0, 0);
    ctx.imageSmoothingEnabled = false;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.drawImage(sourceCanvas, 0, 0, layout.backingW, layout.backingH);
  } else {
    ctx.imageSmoothingEnabled = config.filterMode !== 'nearest';
    ctx.putImageData(imageData, 0, 0);
  }
}
function updateStats(emulatedFrames = 1, displayedFrame = false) {
  framesThisSecond += Math.max(0, emulatedFrames | 0);
  if (displayedFrame) displayFramesThisSecond++;
  const now = performance.now();

  /* DOM writes are deliberately throttled. Little Wizard stresses the emulator
     core, canvas upload, and audio callbacks at the same time; per-frame
     status label writes can force avoidable layout work and visible frame
     skips on mobile browsers. */
  let pc = null;
  if (now - lastStatsDomTime >= STATUS_DOM_UPDATE_MS) {
    const frameCount = exp.gp32_wasm_get_frame_count();
    pc = hex8(exp.gp32_wasm_get_pc());
    frameText.textContent = String(frameCount);
    pcText.textContent = pc;
    lastStatsDomTime = now;
  }

  if (now - lastFpsTime >= 1000) {
    if (config.showFpsHud === true) {
      if (pc === null) pc = hex8(exp.gp32_wasm_get_pc());
      const seconds = Math.max(0.001, (now - lastFpsTime) / 1000);
      const emuFps = Math.min(60, Math.round(framesThisSecond / seconds));
      const videoFps = Math.min(60, Math.round(displayFramesThisSecond / seconds));
      const fpsText = emuFps === videoFps ? `${emuFps} fps` : `${emuFps} fps (${videoFps} video)`;
      hud.textContent = `GP32emu WASM • ${fpsText} • PC ${pc}`;
    }
    framesThisSecond = 0;
    displayFramesThisSecond = 0;
    lastFpsTime = now;
  }
}
function resetFrameLimiter(now = performance.now()) {
  frameLimiterLastTime = now;
  frameLimiterAccumMs = 0;
}
function audioQueuedMs() {
  const sr = audioCtx ? Math.max(8000, Math.round(audioCtx.sampleRate || 44100)) : 44100;
  return (Math.max(0, audioQueuedFrames) * 1000) / sr;
}
function runFrameBatch(frameCount) {
  if (frameCount <= 0) return 0;
  if (frameCount === 1 || !exp.gp32_wasm_run_frames) return exp.gp32_wasm_frame(buttons) ? 1 : 0;
  return exp.gp32_wasm_run_frames(buttons, frameCount, 1) | 0;
}
function loop(now) {
  if (!running) return;

  if (!frameLimiterLastTime) frameLimiterLastTime = now || performance.now();
  let elapsed = (now || performance.now()) - frameLimiterLastTime;
  frameLimiterLastTime = now || performance.now();
  if (!Number.isFinite(elapsed) || elapsed < 0) elapsed = 0;
  if (elapsed > 250) elapsed = GP32_FRAME_MS;
  frameLimiterAccumMs = Math.min(frameLimiterAccumMs + elapsed, MAX_FRAME_ACCUM_MS);

  let targetFrames = 0;
  while (frameLimiterAccumMs >= GP32_FRAME_MS && targetFrames < MAX_CATCHUP_FRAMES_PER_RAF) {
    frameLimiterAccumMs -= GP32_FRAME_MS;
    targetFrames++;
  }

  /* Avoid the previous audio-driven 3-6 frame batches.  Those kept the audio
     queue fat, but they also discarded most video frames.  Only add one
     emergency frame when the audio queue is near underrun; otherwise the video
     cadence is controlled by elapsed presentation time. */
  if (targetFrames === 0 && audioStarted && audioQueuedMs() < AUDIO_CRITICAL_MS) {
    targetFrames = 1;
  } else if (targetFrames === 1 && audioStarted && audioQueuedMs() < AUDIO_CRITICAL_MS) {
    targetFrames = 2;
  }

  let done = 0;
  if (targetFrames > 0) {
    done = runFrameBatch(targetFrames);
    if (!done) {
      running = false;
      document.body.classList.remove('running');
      setState('error');
      log(errorText());
      return;
    }
    pushAudio();
    drawFrame();
    updateStats(done, true);
  } else {
    updateStats(0, false);
  }
  raf = requestAnimationFrame(loop);
}
async function start() {
  if (!gameLoaded && !biosLoaded) { log('Load a BIOS or game first.'); return; }
  await ensureAudio(true);
  resetAudioQueues();
  if (!exp.gp32_wasm_start()) { setState('error'); log(errorText()); return; }
  if (!biosLoaded) log('HLE BIOS active: this can fail on some commercial games and is mainly useful for homebrew/direct-load compatible titles.');
  hiddenPauseWasRunning = false;
  running = true;
  framesThisSecond = 0;
  displayFramesThisSecond = 0;
  lastFpsTime = performance.now();
  lastStatsDomTime = 0;
  lastAudioDomTime = 0;
  lastDrawnCoreFrame = -1;
  resetFrameLimiter(lastFpsTime);
  document.body.classList.add('running');
  setState('running');
  cancelAnimationFrame(raf);
  raf = requestAnimationFrame(loop);
}
function pause() {
  hiddenPauseWasRunning = false;
  running = false;
  cancelAnimationFrame(raf);
  document.body.classList.remove('running');
  setState('paused');
}
function pauseForHiddenTab() {
  if (!running) return;
  hiddenPauseWasRunning = true;
  running = false;
  cancelAnimationFrame(raf);
  clearAllInput();
  resetAudioQueues();
  if (audioCtx?.state === 'running' && audioCtx.suspend) {
    try { audioCtx.suspend().catch?.(() => {}); } catch (_) {}
  }
  document.body.classList.remove('running');
  setState('paused (inactive tab)');
}
async function resumeAfterHiddenTab() {
  if (!hiddenPauseWasRunning) return;
  hiddenPauseWasRunning = false;
  if (!exp || exp.gp32_wasm_get_status?.() !== 2) return;
  try { await ensureAudio(true); } catch (_) {}
  resetAudioQueues();
  running = true;
  framesThisSecond = 0;
  displayFramesThisSecond = 0;
  lastFpsTime = performance.now();
  lastStatsDomTime = 0;
  lastAudioDomTime = 0;
  lastDrawnCoreFrame = -1;
  resetFrameLimiter(lastFpsTime);
  document.body.classList.add('running');
  setState('running');
  cancelAnimationFrame(raf);
  raf = requestAnimationFrame(loop);
}
function handleVisibilityChange() {
  if (config.pauseWhenHidden === false) return;
  if (document.visibilityState === 'hidden') pauseForHiddenTab();
  else resumeAfterHiddenTab();
}

async function loadBiosFile(file) {
  const bytes = await fileBytes(file);
  const ptr = copyToWasm(bytes);
  if (!exp.gp32_wasm_load_bios(ptr, bytes.byteLength)) { setState('error'); log(errorText()); return; }
  biosLoaded = true;
  biosMeta = mediaMeta(file, bytes, 'bios');
  setState(`BIOS loaded: ${file.name}`);
  log('');
}
async function loadGameFile(file) {
  const bytes = await fileBytes(file);
  const ptr = copyToWasm(bytes);
  const name = copyStringToWasm(file.name || 'game.smc');
  const forceHle = hleMode.checked ? 1 : 0;
  currentLaunchUsesHle = !!forceHle || !biosLoaded;
  if (!exp.gp32_wasm_load_media(ptr, bytes.byteLength, name.ptr, name.len, forceHle)) { setState('error'); log(errorText()); return; }
  gameLoaded = true;
  gameMeta = mediaMeta(file, bytes, 'game');
  setState(`game loaded: ${file.name}`);
  if (!biosLoaded || forceHle) log('HLE BIOS/direct-load active. Some commercial games will not boot without a real GP32 BIOS.');
  else log('');
  await start();
}

async function saveStateToStorage() {
  if (!exp) return;
  if (exp.gp32_wasm_get_status?.() !== 2) return setStorageStatus('Start the loaded BIOS/game before saving a state.', 'warn');
  if (!exp.gp32_wasm_save_state()) return setStorageStatus(`save failed: ${errorText()}`, 'bad');
  const ptr = exp.gp32_wasm_get_save_ptr();
  const size = exp.gp32_wasm_get_save_size();
  if (!ptr || !size) return setStorageStatus('save failed: empty state buffer', 'bad');
  const bytes = wasmU8().slice(ptr, ptr + size);
  if (!looksLikeGp32State(bytes)) return setStorageStatus('save failed: generated state header was invalid', 'bad');
  try {
    const where = await putStateBytes(bytes);
    setStorageStatus(`State slot ${config.stateSlot} saved to ${where} (${size.toLocaleString()} bytes).`, 'ok');
  } catch (err) {
    setStorageStatus(`save failed: ${err.message || err}`, 'bad');
  }
}
async function loadStateFromStorage() {
  if (!exp) return;
  if (exp.gp32_wasm_get_status?.() !== 2) return setStorageStatus('Start the matching BIOS/game before loading a state.', 'warn');
  let bytes;
  try { bytes = await getStateBytes(); }
  catch (err) { return setStorageStatus(`load failed: ${err.message || err}`, 'bad'); }
  if (!bytes) return setStorageStatus(`No state in slot ${config.stateSlot} for this BIOS/game.`, 'warn');
  if (!looksLikeGp32State(bytes)) return setStorageStatus(`State slot ${config.stateSlot} is corrupt or from an older incompatible build; save the slot again.`, 'bad');
  const wasRunning = running;
  running = false;
  cancelAnimationFrame(raf);
  resetAudioQueues();
  const ptr = copyToWasm(bytes);
  const ok = exp.gp32_wasm_load_state(ptr, bytes.byteLength);
  if (!ok) {
    running = wasRunning;
    if (running) raf = requestAnimationFrame(loop);
    return setStorageStatus(`state rejected: ${errorText()}`, 'bad');
  }
  clearAllInput();
  lastDrawnCoreFrame = -1;
  drawFrame();
  updateStats(0, true);
  setStorageStatus(`State slot ${config.stateSlot} loaded.`, 'ok');
  setState(wasRunning ? 'running' : 'paused');
  running = wasRunning;
  document.body.classList.toggle('running', running);
  if (running) raf = requestAnimationFrame(loop);
}
async function exportStateToFile() {
  let bytes;
  try { bytes = await getStateBytes(); }
  catch (err) { return setStorageStatus(`export failed: ${err.message || err}`, 'bad'); }
  if (!bytes) return setStorageStatus(`No state in slot ${config.stateSlot} to export.`, 'warn');
  const blob = new Blob([bytes], { type: 'application/octet-stream' });
  const url = URL.createObjectURL(blob);
  try {
    const a = document.createElement('a');
    a.href = url;
    a.download = stateExportName();
    a.rel = 'noopener';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setStorageStatus(`Exported state slot ${config.stateSlot} (${bytes.byteLength.toLocaleString()} bytes).`, 'ok');
  } finally {
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
}

function keyLabel(code) {
  if (KEY_LABELS.has(code)) return KEY_LABELS.get(code);
  if (/^Key[A-Z]$/.test(code)) return code.slice(3);
  if (/^Digit[0-9]$/.test(code)) return code.slice(5);
  return code || 'Unassigned';
}
function renderKeyBindings() {
  if (!keyBindingsEl) return;
  keyBindingsEl.innerHTML = '';
  for (const b of BUTTONS) {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'binding-row';
    row.dataset.remap = b.name;
    row.innerHTML = `<span>${b.label}</span><kbd>${keyLabel(config.keys[b.name])}</kbd>`;
    if (remapTarget === b.name) row.classList.add('listening');
    row.addEventListener('click', () => {
      remapTarget = b.name;
      renderKeyBindings();
      setStorageStatus(`Press a key for ${b.label}. Escape cancels.`, 'warn');
    });
    keyBindingsEl.append(row);
  }
}
function buttonForKeyCode(code) {
  for (const b of BUTTONS) if (config.keys[b.name] === code) return b.mask;
  return 0;
}
function gamepadSourceLabel(source) {
  const mButton = /^button:(\d+)$/.exec(source || '');
  if (mButton) return `Button ${mButton[1]}`;
  const mAxis = /^axis:(\d+):([+-])$/.exec(source || '');
  if (mAxis) return `Axis ${mAxis[1]} ${mAxis[2] === '-' ? '−' : '+'}`;
  return source || 'Unassigned';
}
function renderGamepadBindings() {
  if (!gamepadBindingsEl) return;
  gamepadBindingsEl.innerHTML = '';
  for (const b of BUTTONS) {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'binding-row';
    row.dataset.padRemap = b.name;
    const sources = config.gamepad?.[b.name] || [];
    row.innerHTML = `<span>${b.label}</span><kbd>${sources.map(gamepadSourceLabel).join(' / ') || 'Unassigned'}</kbd>`;
    if (padRemapTarget === b.name) row.classList.add('listening');
    row.addEventListener('click', () => {
      padRemapTarget = b.name;
      gamepadRemapBaseline = collectActiveGamepadSources(selectGamepad(getGamepadsSafe()));
      renderGamepadBindings();
      setStorageStatus(`Press a controller button or move an axis for ${b.label}. Escape cancels.`, 'warn');
    });
    gamepadBindingsEl.append(row);
  }
}
function getGamepadsSafe() {
  try {
    return navigator.getGamepads ? Array.from(navigator.getGamepads()).filter(Boolean) : [];
  } catch (_) {
    return [];
  }
}
function selectGamepad(gamepads = getGamepadsSafe()) {
  if (config.gamepadIndex >= 0) {
    const selected = gamepads.find(gp => gp && gp.index === config.gamepadIndex && gp.connected !== false);
    if (selected) return selected;
  }
  return gamepads.find(gp => gp && gp.connected !== false) || null;
}
function gamepadListSignature(gamepads = getGamepadsSafe()) {
  return gamepads.map(gp => `${gp.index}:${gp.id}:${gp.connected ? 1 : 0}`).join('|');
}
function shortGamepadName(gp) {
  const raw = (gp?.id || `Gamepad ${gp?.index ?? 0}`).replace(/\s+/g, ' ').trim();
  return raw.length > 42 ? `${raw.slice(0, 39)}…` : raw;
}
function updateGamepadSelect(gamepads = getGamepadsSafe()) {
  if (!gamepadSelect) return;
  const sig = gamepadListSignature(gamepads);
  if (sig === lastGamepadListSignature && gamepadSelect.value === String(config.gamepadIndex)) return;
  lastGamepadListSignature = sig;
  const previous = String(config.gamepadIndex);
  gamepadSelect.innerHTML = '';
  const auto = document.createElement('option');
  auto.value = '-1';
  auto.textContent = 'Auto detect';
  gamepadSelect.append(auto);
  for (const gp of gamepads) {
    const opt = document.createElement('option');
    opt.value = String(gp.index);
    opt.textContent = `${gp.index}: ${shortGamepadName(gp)}`;
    gamepadSelect.append(opt);
  }
  gamepadSelect.value = Array.from(gamepadSelect.options).some(o => o.value === previous) ? previous : '-1';
}
function setGamepadStatus(text, tone = 'muted') {
  if (!gamepadText && !gamepadStatusText) return;
  if (text === lastGamepadStatusText && gamepadText?.dataset.tone === tone) return;
  lastGamepadStatusText = text;
  const cls = tone === 'ok' ? 'status-ok' : tone === 'bad' ? 'status-bad' : tone === 'warn' ? 'status-warn' : 'hint';
  if (gamepadText) {
    gamepadText.dataset.tone = tone;
    gamepadText.textContent = text;
    gamepadText.className = cls;
  }
  if (gamepadStatusText) {
    gamepadStatusText.textContent = text;
    gamepadStatusText.className = cls;
  }
}
function sourceActiveOnGamepad(gp, source, threshold = GAMEPAD_AXIS_DEADZONE) {
  if (!gp || !source) return false;
  const mButton = /^button:(\d+)$/.exec(source);
  if (mButton) {
    const b = gp.buttons[Number(mButton[1])];
    return !!b && (b.pressed || Number(b.value || 0) >= 0.5);
  }
  const mAxis = /^axis:(\d+):([+-])$/.exec(source);
  if (mAxis) {
    const v = Number(gp.axes[Number(mAxis[1])] || 0);
    return mAxis[2] === '-' ? v <= -threshold : v >= threshold;
  }
  return false;
}
function collectActiveGamepadSources(gp, threshold = GAMEPAD_REMAP_THRESHOLD) {
  const active = new Set();
  if (!gp) return active;
  for (let i = 0; i < gp.buttons.length; i++) {
    const b = gp.buttons[i];
    if (b && (b.pressed || Number(b.value || 0) >= 0.5)) active.add(`button:${i}`);
  }
  for (let i = 0; i < gp.axes.length; i++) {
    const v = Number(gp.axes[i] || 0);
    if (v <= -threshold) active.add(`axis:${i}:-`);
    else if (v >= threshold) active.add(`axis:${i}:+`);
  }
  return active;
}
function detectGamepadRemapSource(gp) {
  if (!gp) return null;
  let best = null;
  for (let i = 0; i < gp.buttons.length; i++) {
    const b = gp.buttons[i];
    const value = b ? Number(b.value || 0) : 0;
    const source = `button:${i}`;
    if ((b?.pressed || value >= 0.5) && !gamepadRemapBaseline.has(source)) {
      const score = 1 + value;
      if (!best || score > best.score) best = { source, score };
    }
  }
  for (let i = 0; i < gp.axes.length; i++) {
    const v = Number(gp.axes[i] || 0);
    if (Math.abs(v) >= GAMEPAD_REMAP_THRESHOLD) {
      const source = `axis:${i}:${v < 0 ? '-' : '+'}`;
      if (!gamepadRemapBaseline.has(source)) {
        const score = Math.abs(v);
        if (!best || score > best.score) best = { source, score };
      }
    }
  }
  return best?.source || null;
}
function pollGamepads() {
  const gamepads = getGamepadsSafe();
  updateGamepadSelect(gamepads);
  const gp = selectGamepad(gamepads);
  if (!gp) {
    gamepadButtons = 0;
    setGamepadStatus('No controller detected. Press a button on a connected controller if the browser has not exposed it yet.', 'warn');
    syncButtons();
    return;
  }
  if (padRemapTarget) {
    const source = detectGamepadRemapSource(gp);
    if (source) {
      config.gamepad[padRemapTarget] = [source];
      const label = BUTTON_BY_NAME.get(padRemapTarget)?.label || padRemapTarget;
      padRemapTarget = null;
      gamepadRemapBaseline = new Set();
      gamepadButtons = 0;
      saveConfig();
      renderGamepadBindings();
      setStorageStatus(`Controller mapping saved for ${label}: ${gamepadSourceLabel(source)}.`, 'ok');
    }
  }
  let mask = 0;
  for (const b of BUTTONS) {
    const sources = config.gamepad?.[b.name] || [];
    if (sources.some(source => sourceActiveOnGamepad(gp, source))) mask |= b.mask;
  }
  gamepadButtons = mask >>> 0;
  setGamepadStatus(`Active controller: ${gp.index}: ${shortGamepadName(gp)}.`, 'ok');
  syncButtons();
}
function startGamepadPolling() {
  if (!navigator.getGamepads) {
    setGamepadStatus('Gamepad API is not available in this browser.', 'bad');
    return;
  }
  const tick = () => {
    pollGamepads();
    gamepadRaf = requestAnimationFrame(tick);
  };
  cancelAnimationFrame(gamepadRaf);
  tick();
}
function syncButtons() {
  buttons = (keyboardButtons | panelButtons | touchButtons | gamepadButtons) >>> 0;
  if (exp?.gp32_wasm_set_input) exp.gp32_wasm_set_input(buttons);
}
function clearAllInput() {
  keyboardButtons = 0;
  panelButtons = 0;
  touchButtons = 0;
  gamepadButtons = 0;
  dpadMask = 0;
  for (const el of document.querySelectorAll('.active')) el.classList.remove('active');
  if (touchKnob) {
    touchKnob.style.setProperty('--stick-x', '0px');
    touchKnob.style.setProperty('--stick-y', '0px');
  }
  syncButtons();
}
function setTouchButton(mask, down) {
  if (down) touchButtons |= mask;
  else touchButtons &= ~mask;
  syncButtons();
}
function updateDpadFromPointer(e) {
  if (!touchDpad) return;
  const rect = activeDpadRect || touchDpad.getBoundingClientRect();
  const cx = rect.left + rect.width * 0.5;
  const cy = rect.top + rect.height * 0.5;
  const max = Math.max(1, Math.min(rect.width, rect.height) * 0.36);
  let dx = e.clientX - cx;
  let dy = e.clientY - cy;
  const mag = Math.hypot(dx, dy);
  if (mag > max) { dx *= max / mag; dy *= max / mag; }
  if (touchKnob) {
    touchKnob.style.setProperty('--stick-x', `${dx}px`);
    touchKnob.style.setProperty('--stick-y', `${dy}px`);
  }
  const dead = max * 0.32;
  let mask = 0;
  if (dx < -dead) mask |= GP32_BUTTON_LEFT;
  if (dx > dead) mask |= GP32_BUTTON_RIGHT;
  if (dy < -dead) mask |= GP32_BUTTON_UP;
  if (dy > dead) mask |= GP32_BUTTON_DOWN;
  touchButtons = (touchButtons & ~dpadMask) | mask;
  dpadMask = mask;
  syncButtons();
}
function releaseDpad() {
  touchButtons &= ~dpadMask;
  dpadMask = 0;
  activeDpadPointer = null;
  activeDpadRect = null;
  touchDpad?.classList.remove('active');
  if (touchKnob) {
    touchKnob.style.setProperty('--stick-x', '0px');
    touchKnob.style.setProperty('--stick-y', '0px');
  }
  syncButtons();
}
function applyTouchOpacity() {
  if (touchOpacity) touchOpacity.value = String(Math.round(config.touchOpacity * 100));
  if (touchOverlayEnabled) touchOverlayEnabled.checked = config.touchOverlayEnabled !== false;
  if (touchGamepad) touchGamepad.style.setProperty('--touch-opacity', String(config.touchOpacity));
}
function applyHudVisibility() {
  const enabled = config.showFpsHud === true;
  if (fpsHudEnabled) fpsHudEnabled.checked = enabled;
  if (hud) {
    hud.hidden = !enabled;
    hud.setAttribute('aria-hidden', enabled ? 'false' : 'true');
    if (enabled && !hud.textContent.trim()) hud.textContent = 'GP32emu WASM';
  }
}
function applyVideoEffectSettings() {
  if (lcdPersistence) lcdPersistence.checked = config.lcdPersistence === true;
  if (frameInterpolation) frameInterpolation.checked = config.frameInterpolation === true;
  if (exp?.gp32_wasm_set_video_effects) exp.gp32_wasm_set_video_effects(config.lcdPersistence === true ? 1 : 0, config.frameInterpolation === true ? 1 : 0);
  redrawLastFrame();
}
function applyVideoSettings() {
  if (scaleMode) scaleMode.value = enumValue(config.scaleMode, SCALE_MODES, 'integer');
  if (filterMode) filterMode.value = enumValue(config.filterMode, FILTER_MODES, 'nearest');
  if (pauseWhenHidden) pauseWhenHidden.checked = config.pauseWhenHidden !== false;
  applyVideoEffectSettings();
  markVideoLayoutDirty();
  redrawLastFrame();
}
function updateTouchControlsAvailability() {
  const coarse = !!coarsePointerMedia?.matches;
  const small = window.innerWidth <= 920;
  const available = coarse || small;
  document.body.classList.toggle('touch-controls-available', available);
  document.body.classList.toggle('touch-controls-active', available && config.touchOverlayEnabled !== false);
  if (touchOverlayEnabled) touchOverlayEnabled.disabled = !available;
}

function nativeFullscreenElement() {
  return document.fullscreenElement || document.webkitFullscreenElement || document.msFullscreenElement || null;
}
function nativeFullscreenEnabled() {
  return !!(document.fullscreenEnabled || document.webkitFullscreenEnabled || document.msFullscreenEnabled);
}
function fullscreenActive() {
  return !!nativeFullscreenElement() || pseudoFullscreen;
}
function fullscreenRequestFor(el) {
  return el?.requestFullscreen || el?.webkitRequestFullscreen || el?.msRequestFullscreen || null;
}
function fullscreenExitForDocument() {
  return document.exitFullscreen || document.webkitExitFullscreen || document.msExitFullscreen || null;
}
function updateFullscreenButton() {
  const active = fullscreenActive();
  document.body.classList.toggle('is-fullscreen', active);
  document.body.classList.toggle('pseudo-fullscreen', pseudoFullscreen);
  markVideoLayoutDirty();
  if (!fullscreenToggle) return;
  fullscreenToggle.setAttribute('aria-label', active ? 'Exit fullscreen' : 'Enter fullscreen');
  fullscreenToggle.title = active ? 'Exit fullscreen' : 'Enter fullscreen';
  redrawLastFrame();
}
async function enterFullscreen() {
  const target = dropZone || screenWrap || document.documentElement;
  const request = fullscreenRequestFor(target);
  document.body.classList.remove('panel-open');
  if (request && nativeFullscreenEnabled()) {
    try {
      await request.call(target, { navigationUI: 'hide' });
      pseudoFullscreen = false;
      updateFullscreenButton();
      screenWrap?.focus();
      return;
    } catch (err) {
      try {
        await request.call(target);
        pseudoFullscreen = false;
        updateFullscreenButton();
        screenWrap?.focus();
        return;
      } catch (err2) {
        setStorageStatus(`fullscreen failed: ${(err2 && err2.message) || err2 || err}`, 'bad');
      }
    }
  }
  pseudoFullscreen = true;
  updateFullscreenButton();
  setStorageStatus('Fullscreen API unavailable; using in-page fullscreen layout.', 'warn');
  screenWrap?.focus();
}
async function exitFullscreen() {
  if (nativeFullscreenElement()) {
    const exit = fullscreenExitForDocument();
    if (exit) {
      try { await exit.call(document); }
      catch (err) { setStorageStatus(`exit fullscreen failed: ${err.message || err}`, 'bad'); }
    }
  }
  pseudoFullscreen = false;
  updateFullscreenButton();
  screenWrap?.focus();
}
async function toggleFullscreen() {
  if (fullscreenActive()) await exitFullscreen();
  else await enterFullscreen();
}

function bindInput() {
  window.addEventListener('keydown', e => {
    if ((remapTarget || padRemapTarget) && e.code === 'Escape') {
      e.preventDefault();
      remapTarget = null;
      padRemapTarget = null;
      gamepadRemapBaseline = new Set();
      renderKeyBindings();
      renderGamepadBindings();
      setStorageStatus('Remap canceled.', 'warn');
      return;
    }
    if (padRemapTarget) {
      e.preventDefault();
      return;
    }
    if (remapTarget) {
      e.preventDefault();
      config.keys[remapTarget] = e.code;
      remapTarget = null;
      keyboardButtons = 0;
      saveConfig();
      renderKeyBindings();
      setStorageStatus('Keyboard mapping saved.', 'ok');
      syncButtons();
      return;
    }
    const b = buttonForKeyCode(e.code);
    if (b) { keyboardButtons |= b; syncButtons(); e.preventDefault(); }
  });
  window.addEventListener('keyup', e => {
    const b = buttonForKeyCode(e.code);
    if (b) { keyboardButtons &= ~b; syncButtons(); e.preventDefault(); }
  });

  for (const el of document.querySelectorAll('.controls [data-btn]')) {
    const b = Number(el.dataset.btn) >>> 0;
    const down = ev => { panelButtons |= b; syncButtons(); el.classList.add('active'); el.setPointerCapture?.(ev.pointerId); ev.preventDefault(); };
    const up = ev => { panelButtons &= ~b; syncButtons(); el.classList.remove('active'); ev.preventDefault(); };
    el.addEventListener('pointerdown', down);
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('lostpointercapture', up);
  }

  touchDpad?.addEventListener('pointerdown', e => {
    activeDpadPointer = e.pointerId;
    activeDpadRect = touchDpad.getBoundingClientRect();
    touchDpad.setPointerCapture?.(e.pointerId);
    touchDpad.classList.add('active');
    updateDpadFromPointer(e);
    e.preventDefault();
  });
  touchDpad?.addEventListener('pointermove', e => { if (e.pointerId === activeDpadPointer) { updateDpadFromPointer(e); e.preventDefault(); } });
  touchDpad?.addEventListener('pointerup', e => { if (e.pointerId === activeDpadPointer) { releaseDpad(); e.preventDefault(); } });
  touchDpad?.addEventListener('pointercancel', e => { if (e.pointerId === activeDpadPointer) { releaseDpad(); e.preventDefault(); } });
  touchDpad?.addEventListener('lostpointercapture', releaseDpad);

  for (const el of document.querySelectorAll('[data-touch-btn]')) {
    const name = el.dataset.touchBtn;
    const mask = BUTTON_BY_NAME.get(name)?.mask || 0;
    const down = ev => { setTouchButton(mask, true); el.classList.add('active'); el.setPointerCapture?.(ev.pointerId); ev.preventDefault(); };
    const up = ev => { setTouchButton(mask, false); el.classList.remove('active'); ev.preventDefault(); };
    el.addEventListener('pointerdown', down);
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('lostpointercapture', up);
  }

  resetKeyMapBtn?.addEventListener('click', () => {
    config.keys = { ...DEFAULT_KEYS };
    saveConfig();
    renderKeyBindings();
    clearAllInput();
    setStorageStatus('Keyboard mapping reset.', 'ok');
  });
  gamepadSelect?.addEventListener('change', () => {
    const value = parseInt(gamepadSelect.value, 10);
    config.gamepadIndex = Number.isFinite(value) ? Math.max(-1, Math.min(15, value)) : -1;
    saveConfig();
    clearAllInput();
    pollGamepads();
  });
  resetGamepadMapBtn?.addEventListener('click', () => {
    config.gamepad = defaultGamepadMap();
    padRemapTarget = null;
    gamepadRemapBaseline = new Set();
    saveConfig();
    renderGamepadBindings();
    clearAllInput();
    setStorageStatus('Controller mapping reset.', 'ok');
  });
  window.addEventListener('gamepadconnected', () => { updateGamepadSelect(); pollGamepads(); });
  window.addEventListener('gamepaddisconnected', () => { updateGamepadSelect(); clearAllInput(); pollGamepads(); });
  stateSlot?.addEventListener('change', () => {
    config.stateSlot = Math.max(0, Math.min(9, parseInt(stateSlot.value, 10) || 0));
    saveConfig();
    setStorageStatus(`Selected state slot ${config.stateSlot}.`, 'ok');
  });
  touchOpacity?.addEventListener('input', () => {
    config.touchOpacity = Math.max(0.25, Math.min(1, (parseInt(touchOpacity.value || '76', 10) || 76) / 100));
    saveConfig();
    applyTouchOpacity();
  });
  touchOverlayEnabled?.addEventListener('change', () => {
    config.touchOverlayEnabled = !!touchOverlayEnabled.checked;
    if (!config.touchOverlayEnabled) {
      touchButtons = 0;
      dpadMask = 0;
      releaseDpad();
    }
    saveConfig();
    applyTouchOpacity();
    updateTouchControlsAvailability();
    syncButtons();
    setStorageStatus(config.touchOverlayEnabled ? 'Mobile control overlay enabled.' : 'Mobile control overlay disabled; keyboard and controllers remain active.', 'ok');
  });
  fpsHudEnabled?.addEventListener('change', () => {
    config.showFpsHud = !!fpsHudEnabled.checked;
    saveConfig();
    applyHudVisibility();
    if (config.showFpsHud) {
      hud.textContent = `GP32emu WASM • FPS overlay enabled • PC ${exp ? hex8(exp.gp32_wasm_get_pc()) : '00000000'}`;
    }
    setStorageStatus(config.showFpsHud ? 'FPS/PC overlay enabled.' : 'FPS/PC overlay disabled.', 'ok');
  });
  scaleMode?.addEventListener('change', () => {
    config.scaleMode = enumValue(scaleMode.value, SCALE_MODES, 'integer');
    saveConfig();
    applyVideoSettings();
    setStorageStatus(`Video scaling set to ${scaleMode.options[scaleMode.selectedIndex]?.textContent || config.scaleMode}.`, 'ok');
  });
  filterMode?.addEventListener('change', () => {
    config.filterMode = enumValue(filterMode.value, FILTER_MODES, 'nearest');
    saveConfig();
    applyVideoSettings();
    setStorageStatus(`Video filter set to ${filterMode.options[filterMode.selectedIndex]?.textContent || config.filterMode}.`, 'ok');
  });
  lcdPersistence?.addEventListener('change', () => {
    config.lcdPersistence = !!lcdPersistence.checked;
    saveConfig();
    applyVideoEffectSettings();
    setStorageStatus(config.lcdPersistence ? 'LCD persistence enabled.' : 'LCD persistence disabled.', 'ok');
  });
  frameInterpolation?.addEventListener('change', () => {
    config.frameInterpolation = !!frameInterpolation.checked;
    saveConfig();
    applyVideoEffectSettings();
    setStorageStatus(config.frameInterpolation ? 'Frame interpolation enabled.' : 'Frame interpolation disabled.', 'ok');
  });
  pauseWhenHidden?.addEventListener('change', () => {
    config.pauseWhenHidden = !!pauseWhenHidden.checked;
    if (!config.pauseWhenHidden) hiddenPauseWasRunning = false;
    saveConfig();
    applyVideoSettings();
    if (config.pauseWhenHidden && document.visibilityState === 'hidden') handleVisibilityChange();
    setStorageStatus(config.pauseWhenHidden ? 'Inactive-tab pause enabled.' : 'Inactive-tab pause disabled.', 'ok');
  });
  menuToggle?.addEventListener('click', () => document.body.classList.toggle('panel-open'));
  panelClose?.addEventListener('click', () => document.body.classList.remove('panel-open'));
  fullscreenToggle?.addEventListener('click', ev => { ev.preventDefault(); toggleFullscreen(); });
  document.addEventListener('fullscreenchange', updateFullscreenButton);
  document.addEventListener('webkitfullscreenchange', updateFullscreenButton);
  document.addEventListener('msfullscreenchange', updateFullscreenButton);
  screenWrap?.addEventListener('click', () => screenWrap.focus());
  if (coarsePointerMedia?.addEventListener) coarsePointerMedia.addEventListener('change', updateTouchControlsAvailability);
  else coarsePointerMedia?.addListener?.(updateTouchControlsAvailability);
  window.addEventListener('resize', () => { updateTouchControlsAvailability(); markVideoLayoutDirty(); redrawLastFrame(); });
  window.addEventListener('blur', clearAllInput);
  document.addEventListener('visibilitychange', handleVisibilityChange);
}



function bindFilesAndControls() {
  document.getElementById('biosFile').addEventListener('change', e => { const f = e.target.files[0]; if (f) loadBiosFile(f); });
  document.getElementById('gameFile').addEventListener('change', e => { const f = e.target.files[0]; if (f) loadGameFile(f); });
  startBtn.addEventListener('click', start);
  pauseBtn.addEventListener('click', pause);
  saveStateBtn?.addEventListener('click', () => saveStateToStorage());
  loadStateBtn?.addEventListener('click', () => loadStateFromStorage());
  exportStateBtn?.addEventListener('click', () => exportStateToFile());

  window.addEventListener('dragenter', e => { e.preventDefault(); document.body.classList.add('drag-active'); });
  window.addEventListener('dragover', e => e.preventDefault());
  window.addEventListener('dragleave', e => { if (e.target === document.body || e.target === dropZone) document.body.classList.remove('drag-active'); });
  window.addEventListener('drop', async e => {
    e.preventDefault(); document.body.classList.remove('drag-active');
    const files = Array.from(e.dataTransfer.files || []);
    for (const f of files) {
      if (isBios(f.name) && !biosLoaded) await loadBiosFile(f);
      else if (isGame(f.name)) await loadGameFile(f);
    }
  });
}

bindInput();
bindFilesAndControls();
startGamepadPolling();
initWasm().catch(err => { setState('WASM load failed'); log(String(err && err.stack || err)); });
