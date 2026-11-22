import { defineConfig } from "astro/config";
import { rename, rmdir } from "node:fs/promises";
import { translatedLocales } from "./src/locales.js";

export default defineConfig({
  site: "https://picopage.wulu.zone",
  output: "static",
  compressHTML: false,
  integrations: [{
    name: "localized-not-found",
    hooks: {
      "astro:build:done": async ({ dir }) => {
        for (const [, { prefix }] of translatedLocales) {
          await rename(new URL(`${prefix}/404/index.html`, dir), new URL(`${prefix}/404.html`, dir));
          await rmdir(new URL(`${prefix}/404/`, dir));
        }
      },
    },
  }],
});
