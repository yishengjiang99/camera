import createMotionModule from "./motion_wasm.js";

const ANALYSIS_WIDTH = 32;
const ANALYSIS_HEIGHT = 18;

const video = document.querySelector("#video");
const overlay = document.querySelector("#overlay");
const sample = document.querySelector("#sample");
const statusEl = document.querySelector("#status");
const stateEl = document.querySelector("#state");
const scoreEl = document.querySelector("#score");
const changedEl = document.querySelector("#changed");
const framesEl = document.querySelector("#frames");
const humEl = document.querySelector("#hum");
const barEl = document.querySelector("#bar");
const toggle = document.querySelector("#toggle");
const threshold = document.querySelector("#threshold");

const overlayCtx = overlay.getContext("2d");
const sampleCtx = sample.getContext("2d", { willReadFrequently: true });
const AudioContextClass = window.AudioContext || window.webkitAudioContext;

let wasm = null;
let detector = 0;
let framePtr = 0;
let frameBytes = 0;
let stream = null;
let running = false;
let processing = false;
let frames = 0;
let thresholdValue = Number(threshold.value);
let demoCanvas = null;
let demoCtx = null;
let demoAnimationId = 0;
let demoStartTime = 0;
let audioContext = null;
let humOscillator = null;
let humGain = null;
let humFilter = null;

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function cameraSupported() {
  return Boolean(
    navigator.mediaDevices?.getUserMedia ||
      navigator.webkitGetUserMedia ||
      navigator.mozGetUserMedia ||
      navigator.msGetUserMedia
  );
}

function cameraSupportMessage() {
  if (!window.isSecureContext) {
    return "Camera requires HTTPS or localhost. Open the GitHub Pages URL or http://localhost:8080.";
  }

  return "Camera API is unavailable in this browser. Try Chrome, Edge, Firefox, or Safari.";
}

function requestCameraStream(constraints) {
  if (navigator.mediaDevices?.getUserMedia) {
    return navigator.mediaDevices.getUserMedia(constraints);
  }

  const legacyGetUserMedia =
    navigator.webkitGetUserMedia ||
    navigator.mozGetUserMedia ||
    navigator.msGetUserMedia;

  if (!legacyGetUserMedia) {
    return Promise.reject(new Error(cameraSupportMessage()));
  }

  return new Promise((resolve, reject) => {
    legacyGetUserMedia.call(navigator, constraints, resolve, reject);
  });
}

function isPermissionError(error) {
  return error?.name === "NotAllowedError" || error?.name === "SecurityError";
}

function drawDemoFrame(time) {
  if (!demoCanvas || !demoCtx) {
    return;
  }

  const seconds = (time - demoStartTime) / 1000;
  const x = 60 + Math.sin(seconds * 1.8) * 210;
  const y = 80 + Math.cos(seconds * 1.2) * 80;

  demoCtx.fillStyle = "#12151a";
  demoCtx.fillRect(0, 0, demoCanvas.width, demoCanvas.height);

  demoCtx.fillStyle = "#253241";
  for (let i = 0; i < 8; i += 1) {
    demoCtx.fillRect(i * 80 - ((seconds * 25) % 80), 0, 28, demoCanvas.height);
  }

  demoCtx.fillStyle = "#65e39b";
  demoCtx.beginPath();
  demoCtx.arc(x, y, 42, 0, Math.PI * 2);
  demoCtx.fill();

  demoCtx.fillStyle = "#f4c542";
  demoCtx.fillRect(380 - x * 0.45, 190 + Math.sin(seconds * 2.2) * 35, 82, 58);

  demoAnimationId = window.requestAnimationFrame(drawDemoFrame);
}

function createDemoStream() {
  demoCanvas = document.createElement("canvas");
  demoCanvas.width = 640;
  demoCanvas.height = 360;
  demoCtx = demoCanvas.getContext("2d");
  demoStartTime = performance.now();

  if (!demoCanvas.captureStream) {
    throw new Error("Camera permission was denied, and this browser cannot create a demo video stream.");
  }

  drawDemoFrame(demoStartTime);
  return demoCanvas.captureStream(30);
}

function setStatus(text) {
  statusEl.textContent = text;
}

async function ensureHumAudio() {
  if (!AudioContextClass) {
    return;
  }

  if (!audioContext) {
    audioContext = new AudioContextClass();
    humOscillator = audioContext.createOscillator();
    humFilter = audioContext.createBiquadFilter();
    humGain = audioContext.createGain();

    humOscillator.type = "sine";
    humOscillator.frequency.value = 72;
    humFilter.type = "lowpass";
    humFilter.frequency.value = 180;
    humFilter.Q.value = 0.65;
    humGain.gain.value = 0;

    humOscillator.connect(humFilter);
    humFilter.connect(humGain);
    humGain.connect(audioContext.destination);
    humOscillator.start();
  }

  if (audioContext.state === "suspended") {
    await audioContext.resume();
  }
}

function setHumLevel(motion, level) {
  const target = motion ? clamp(0.015 + level * 0.55, 0, 0.18) : 0;

  if (humEl) {
    humEl.textContent = `${Math.round((target / 0.18) * 100)}%`;
  }

  if (!humGain || !audioContext) {
    return;
  }

  const now = audioContext.currentTime;

  humGain.gain.cancelScheduledValues(now);
  humGain.gain.setTargetAtTime(target, now, motion ? 0.055 : 0.16);

  if (humOscillator) {
    humOscillator.frequency.setTargetAtTime(72 + clamp(level * 90, 0, 55), now, 0.08);
  }
}

function muteHum() {
  setHumLevel(false, 0);
}

function resizeOverlay() {
  const rect = video.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  overlay.width = Math.max(1, Math.round(rect.width * dpr));
  overlay.height = Math.max(1, Math.round(rect.height * dpr));
  overlayCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
}

function motionBox() {
  return {
    left: wasm._md_motion_left(detector),
    top: wasm._md_motion_top(detector),
    right: wasm._md_motion_right(detector),
    bottom: wasm._md_motion_bottom(detector),
  };
}

function drawOverlay(motion, level, box = null) {
  const width = overlay.clientWidth;
  const height = overlay.clientHeight;
  overlayCtx.clearRect(0, 0, width, height);

  if (!motion || !box || box.right < box.left || box.bottom < box.top) {
    return;
  }

  const pad = 12;
  const xScale = width / ANALYSIS_WIDTH;
  const yScale = height / ANALYSIS_HEIGHT;
  const x = Math.max(0, box.left * xScale - pad);
  const y = Math.max(0, box.top * yScale - pad);
  const rectWidth = Math.min(width - x, (box.right - box.left + 1) * xScale + pad * 2);
  const rectHeight = Math.min(height - y, (box.bottom - box.top + 1) * yScale + pad * 2);

  overlayCtx.lineWidth = 4;
  overlayCtx.strokeStyle = `rgba(255, 93, 93, ${Math.min(0.95, 0.35 + level * 4)})`;
  overlayCtx.strokeRect(x, y, rectWidth, rectHeight);
}

function updateStats(result) {
  const score = wasm._md_score(detector);
  const level = wasm._md_level(detector);
  const changedPixels = wasm._md_changed_pixels(detector);
  const motion = result === 1;
  const box = motion ? motionBox() : null;

  document.body.classList.toggle("motion", motion);
  stateEl.textContent = motion ? "Motion" : "Still";
  scoreEl.textContent = score.toFixed(4);
  changedEl.textContent = `${Math.round(level * 100)}%`;
  framesEl.textContent = String(frames);
  barEl.style.width = `${Math.min(100, level * 100)}%`;
  setStatus(motion ? `Motion: ${changedPixels} changed pixels` : "Monitoring");
  drawOverlay(motion, level, box);
  setHumLevel(motion, level);
}

function processFrame() {
  if (!running || processing || video.readyState < HTMLMediaElement.HAVE_CURRENT_DATA) {
    return;
  }

  processing = true;
  sampleCtx.drawImage(video, 0, 0, ANALYSIS_WIDTH, ANALYSIS_HEIGHT);
  const imageData = sampleCtx.getImageData(0, 0, ANALYSIS_WIDTH, ANALYSIS_HEIGHT);

  wasm.HEAPU8.set(imageData.data, framePtr);
  const result = wasm._md_process_rgba(detector);
  frames += 1;

  if (result < 0) {
    setStatus(`WASM error ${result}`);
    running = false;
    muteHum();
  } else {
    updateStats(result);
  }

  processing = false;
}

function scheduleFrame() {
  if (!running) {
    return;
  }

  if ("requestVideoFrameCallback" in video) {
    video.requestVideoFrameCallback(() => {
      processFrame();
      scheduleFrame();
    });
  } else {
    window.requestAnimationFrame(() => {
      processFrame();
      scheduleFrame();
    });
  }
}

async function startCamera() {
  toggle.disabled = true;
  setStatus("Requesting camera...");
  await ensureHumAudio();

  if (!cameraSupported()) {
    throw new Error(cameraSupportMessage());
  }

  let usingDemo = false;

  try {
    stream = await requestCameraStream({
      video: {
        facingMode: "environment",
        width: { ideal: 1280 },
        height: { ideal: 720 },
      },
      audio: false,
    });
  } catch (error) {
    if (!isPermissionError(error)) {
      throw error;
    }

    usingDemo = true;
    stream = createDemoStream();
    demoAnimationId = window.requestAnimationFrame(drawDemoFrame);
  }

  video.srcObject = stream;
  await video.play();
  resizeOverlay();

  detector = wasm._md_create(ANALYSIS_WIDTH, ANALYSIS_HEIGHT, thresholdValue);
  if (!detector) {
    throw new Error("Could not create WASM motion detector");
  }

  framePtr = wasm._md_frame_ptr(detector);
  frameBytes = wasm._md_frame_bytes(detector);
  if (!framePtr || frameBytes !== ANALYSIS_WIDTH * ANALYSIS_HEIGHT * 4) {
    throw new Error("Unexpected WASM frame buffer");
  }

  frames = 0;
  running = true;
  toggle.textContent = "Stop Camera";
  toggle.disabled = false;
  setStatus(usingDemo ? "Permission denied; running demo stream" : "Monitoring");

  scheduleFrame();
}

function stopCamera() {
  running = false;
  processing = false;

  if (detector) {
    wasm._md_destroy(detector);
    detector = 0;
  }

  if (stream) {
    for (const track of stream.getTracks()) {
      track.stop();
    }
    stream = null;
  }

  if (demoAnimationId) {
    window.cancelAnimationFrame(demoAnimationId);
    demoAnimationId = 0;
  }
  demoCanvas = null;
  demoCtx = null;

  video.srcObject = null;
  document.body.classList.remove("motion");
  drawOverlay(false, 0);
  muteHum();
  setStatus("Stopped");
  stateEl.textContent = "Idle";
  if (humEl) {
    humEl.textContent = "0%";
  }
  toggle.textContent = "Start Camera";
}

threshold.addEventListener("input", () => {
  thresholdValue = Number(threshold.value);

  if (detector) {
    wasm._md_set_threshold(detector, thresholdValue);
  }
});

toggle.addEventListener("click", () => {
  if (running) {
    stopCamera();
    return;
  }

  startCamera().catch((error) => {
    setStatus(error.message);
    toggle.disabled = false;
  });
});

window.addEventListener("resize", resizeOverlay);

createMotionModule()
  .then((module) => {
    wasm = module;
    if (!cameraSupported()) {
      setStatus(cameraSupportMessage());
      toggle.disabled = true;
      return;
    }

    setStatus("Ready");
    toggle.disabled = false;
  })
  .catch((error) => {
    setStatus(`WASM load failed: ${error.message}`);
  });
