import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";

let found;
try {
  const { version, url } = JSON.parse(readFileSync("public/firmware/manifest.json", "utf8"));
  const file = `pico-page-${version}-read_pico-release-full.bin`;
  const image = readFileSync(`public/firmware/${file}`);
  if (image.length > 16 * 1024 * 1024) throw new Error(`${file} is larger than the 16 MiB flash of Read Pico`);
  const sha256 = createHash("sha256").update(image).digest("hex");
  found = { version, url: new URL(file, url).href, sha256 };
} catch (error) {
  if (error.code !== "ENOENT") throw error;
}

/** The installable release as { version, url, sha256 }, or undefined when its full image is unavailable. */
export const firmware = found;
