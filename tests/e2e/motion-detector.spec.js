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
