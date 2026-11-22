import { defineConfig } from "vitest/config";
import { cloudflareTest } from "@cloudflare/vitest-plugin";

// The test Wrangler config lives in ./test-config/ on purpose: the
// plugin/miniflare loads .dev.vars from the config's directory and lets it
// override the config's vars, so a config next to worker/.dev.vars
// (CHANGEME placeholders after `make setup`) made every /api/v1/* call
// fail with 500 config_error. The subdir has no .dev.vars, so the fake
// tokens in test-config/wrangler.test.toml win. See the comment at the
// top of that file — `miniflare: { vars }` and `envDir: false` were both
// tried and do not fix it. (Same trap as chess-watch; same fix.)
export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./test-config/wrangler.test.toml" },
    }),
  ],
  test: {
    pool: "@cloudflare/vitest-plugin",
    include: ["tests/integration.test.mjs"],
    testTimeout: 60000,
  },
});
