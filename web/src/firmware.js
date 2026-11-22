import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";

let found;
const legacyDirectory = "public/firmware";
const currentDirectory = `${legacyDirectory}/ota-v2`;
const current = existsSync(currentDirectory);
const directory = current ? currentDirectory : legacyDirectory;
const manifestPath = `${directory}/manifest.json`;
if (current || existsSync(manifestPath)) {
  const { version, url } = JSON.parse(readFileSync(manifestPath, "utf8"));
  const file = `pico-page-${version}-read_pico-release-full.bin`;
  const imagePath = `${directory}/${file}`;
  if (current || existsSync(imagePath)) {
    execFileSync("python3", [
      "../tools/ota_manifest.py", "--check", directory, "--expect-layout", current ? "ota7" : "legacy",
      "--require-full", "--base-url", new URL(".", url).href,
    ], { stdio: ["ignore", "ignore", "inherit"] });
    const sha256 = createHash("sha256").update(readFileSync(imagePath)).digest("hex");
    found = { version, url: new URL(file, url).href, sha256, requiresLayoutMigration: current };
  }
}

export const firmware = found;
