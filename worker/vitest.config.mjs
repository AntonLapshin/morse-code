import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    include: ["tests/morse-do.test.mjs", "tests/index.test.mjs"],
    testTimeout: 30000,
  },
});
