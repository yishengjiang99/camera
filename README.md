# Camera Motion Detector

This project builds the C convolutional self-attention block into WebAssembly
and uses it from a browser camera callback for simple motion detection.

## Layout

- `src/`: C sources and headers
- `web/`: handwritten static web source
- `build/`: generated binaries, objects, WASM, and deployable static files

## Build WebAssembly

With Docker Desktop running:

```sh
make docker-wasm
```

If Emscripten is installed locally:

```sh
make wasm
```

Both commands generate:

- `build/web/motion_wasm.js`
- `build/web/motion_wasm.wasm`

To create the full static site:

```sh
make docker-site
```

This copies the handwritten files from `web` and places all generated output in
`build/web`.

## Run

```sh
npm run build
npm run serve
```

Open `http://localhost:8080`, then start the camera. The browser downsamples
camera frames to `32x18`, copies the RGBA buffer into WASM memory, and invokes
one C function per media frame.

## Deploy

The GitHub project uses a `gh-pages` branch for static hosting. Build the
static site, then publish the contents of `build/web` to that branch.

Camera access requires a secure origin, so use the GitHub Pages `https://`
URL for the hosted demo.

## Native Tests

```sh
make test
```
