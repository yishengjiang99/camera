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
const barEl = document.querySelector("#bar");
const toggle = document.querySelector("#toggle");
const threshold = document.querySelector("#threshold");

const overlayCtx = overlay.getContext("2d");
const sampleCtx = sample.getContext("2d", { willReadFrequently: true });

let wasm = null;
let detector = 0;
let framePtr = 0;
let frameBytes = 0;
let stream = null;
let running = false;
let processing = false;
let frames = 0;
let thresholdValue = Number(threshold.value);

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

function setStatus(text) {
  statusEl.textContent = text;
}

function resizeOverlay() {
  const rect = video.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  overlay.width = Math.max(1, Math.round(rect.width * dpr));
  overlay.height = Math.max(1, Math.round(rect.height * dpr));
  overlayCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
}

function drawOverlay(motion, level) {
  const width = overlay.clientWidth;
  const height = overlay.clientHeight;
  overlayCtx.clearRect(0, 0, width, height);

  if (!motion) {
    return;
  }

  overlayCtx.lineWidth = 4;
  overlayCtx.strokeStyle = `rgba(255, 93, 93, ${Math.min(0.95, 0.35 + level * 4)})`;
  overlayCtx.strokeRect(10, 10, width - 20, height - 20);
}

function updateStats(result) {
  const score = wasm._md_score(detector);
  const level = wasm._md_level(detector);
  const changedPixels = wasm._md_changed_pixels(detector);
  const motion = result === 1;

  document.body.classList.toggle("motion", motion);
  stateEl.textContent = motion ? "Motion" : "Still";
  scoreEl.textContent = score.toFixed(4);
  changedEl.textContent = `${Math.round(level * 100)}%`;
  framesEl.textContent = String(frames);
  barEl.style.width = `${Math.min(100, level * 100)}%`;
  setStatus(motion ? `Motion: ${changedPixels} changed pixels` : "Monitoring");
  drawOverlay(motion, level);
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

  if (!cameraSupported()) {
    throw new Error(cameraSupportMessage());
  }

  stream = await requestCameraStream({
    video: {
      facingMode: "environment",
      width: { ideal: 1280 },
      height: { ideal: 720 },
    },
    audio: false,
  });

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
  setStatus("Monitoring");
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

  video.srcObject = null;
  document.body.classList.remove("motion");
  drawOverlay(false, 0);
  setStatus("Stopped");
  stateEl.textContent = "Idle";
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
