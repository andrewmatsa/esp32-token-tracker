// @ts-check
const { defineConfig } = require('@playwright/test');

// Mode selection: set TT_DEVICE_URL to run against real hardware.
//   mock   (default) — a Node mock of the ESP32 is auto-started (no hardware).
//   device           — TT_DEVICE_URL=http://token-tracker.local (snapshot+restore).
const DEVICE_URL = process.env.TT_DEVICE_URL;
const MOCK_PORT = Number(process.env.MOCK_PORT || 8791);
const BASE_URL = DEVICE_URL || `http://localhost:${MOCK_PORT}`;
const IS_DEVICE = !!DEVICE_URL;

module.exports = defineConfig({
  testDir: '.',
  fullyParallel: false,          // one device / one mock server — keep tests serial
  workers: 1,
  timeout: 30_000,
  expect: { timeout: 8_000 },
  retries: process.env.CI ? 1 : 0,
  reporter: [['list'], ['html', { open: 'never' }]],

  // Device mode snapshots device state before the run and restores it after.
  globalSetup: IS_DEVICE ? require.resolve('./global-setup.js') : undefined,
  globalTeardown: IS_DEVICE ? require.resolve('./global-teardown.js') : undefined,

  use: {
    baseURL: BASE_URL,
    trace: 'on-first-retry',
    // The device answers slowly compared to localhost; give actions room.
    actionTimeout: 10_000,
    navigationTimeout: 15_000,
  },

  projects: [
    { name: IS_DEVICE ? 'device' : 'mock' },
  ],

  // Only auto-start the mock server in mock mode.
  webServer: IS_DEVICE ? undefined : {
    command: `node mock-device.js --port ${MOCK_PORT}`,
    url: `http://localhost:${MOCK_PORT}/state`,
    reuseExistingServer: !process.env.CI,
    timeout: 10_000,
  },
});
