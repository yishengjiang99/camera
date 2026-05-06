const { expect, test } = require("@playwright/test");

test("loads WASM and processes fake camera frames", async ({ page }) => {
  const consoleErrors = [];
  page.on("console", (message) => {
    if (message.type() === "error") {
      consoleErrors.push(message.text());
    }
  });

  await page.goto("/");

  await expect(page.locator("#status")).toContainText("Ready");
  await expect(page.locator("#toggle")).toBeEnabled();

  const wasmResponse = await page.request.get("/motion_wasm.wasm");
  expect(wasmResponse.ok()).toBeTruthy();
  expect(wasmResponse.headers()["content-type"]).toContain("application/wasm");

  await page.locator("#toggle").click();

  await expect(page.locator("#toggle")).toHaveText("Stop Camera");
  await expect(page.locator("#state")).not.toHaveText("Idle");
  await expect(page.locator("#hum")).toContainText("%");

  await expect
    .poll(async () => Number(await page.locator("#frames").textContent()), {
      message: "frame counter should advance",
      timeout: 15000,
    })
    .toBeGreaterThan(2);

  await expect
    .poll(async () => {
      const score = await page.locator("#score").textContent();
      return Number(score);
    }, {
      message: "motion score should be a finite number",
      timeout: 5000,
    })
    .not.toBeNaN();

  await expect(page.locator("#status")).not.toContainText("error", {
    ignoreCase: true,
  });
  expect(consoleErrors).toEqual([]);
});

test("falls back to demo stream when camera permission is denied", async ({ browser }) => {
  const context = await browser.newContext({
    baseURL: "http://127.0.0.1:8080",
    permissions: [],
  });
  const page = await context.newPage();
  await page.addInitScript(() => {
    Object.defineProperty(navigator, "mediaDevices", {
      configurable: true,
      value: {
        getUserMedia: () => {
          const error = new Error("Permission denied");
          error.name = "NotAllowedError";
          return Promise.reject(error);
        },
      },
    });
  });

  await page.goto("/");
  await expect(page.locator("#status")).toContainText("Ready");

  await page.locator("#toggle").click();

  await expect(page.locator("#toggle")).toHaveText("Stop Camera");

  await expect
    .poll(async () => Number(await page.locator("#frames").textContent()), {
      message: "demo stream should drive frame processing",
      timeout: 15000,
    })
    .toBeGreaterThan(2);

  await context.close();
});
