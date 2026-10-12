import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";

// Installing needs the full-flash image of the newest release beside its update manifest in
// public/firmware. Until a release carries one, the site shows the steps without the installer.
let found;
try {
  const { version } = JSON.parse(readFileSync("public/firmware/manifest.json", "utf8"));
  const file = `pico-page-${version}-read_pico-release-full.bin`;
  const image = readFileSync(`public/firmware/${file}`);
  // The installer erases the flash before it writes, so an image that cannot fit is never offered.
  if (image.length > 16 * 1024 * 1024) throw new Error(`${file} is larger than the 16 MiB flash of Read Pico`);
  const sha256 = createHash("sha256").update(image).digest("hex");
  found = { version, url: `/firmware/${file}`, sha256 };
} catch (error) {
  if (error.code !== "ENOENT") throw error;
}

/** The installable release as { version, url, sha256 }, or undefined when none is published. */
export const firmware = found;
