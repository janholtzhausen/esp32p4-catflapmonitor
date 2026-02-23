import { mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { gzipSync } from "node:zlib";

const DIST_DIR = "dist";
const GZIPPED_DIR = "gzipped";

function walkFiles(dir, out = []) {
  for (const entry of readdirSync(dir)) {
    const fullPath = join(dir, entry);
    const stats = statSync(fullPath);

    if (stats.isDirectory()) {
      walkFiles(fullPath, out);
      continue;
    }

    if (stats.isFile()) {
      out.push(fullPath);
    }
  }

  return out;
}

function ensureDistHasFiles() {
  const files = walkFiles(DIST_DIR);
  if (files.length === 0) {
    throw new Error("`dist` directory is empty");
  }
  return files;
}

function main() {
  const distStats = statSync(DIST_DIR, { throwIfNoEntry: false });
  if (!distStats || !distStats.isDirectory()) {
    throw new Error("`dist` directory does not exist");
  }

  rmSync(GZIPPED_DIR, { recursive: true, force: true });
  mkdirSync(GZIPPED_DIR, { recursive: true });

  const files = ensureDistHasFiles();

  for (const file of files) {
    const relPath = relative(DIST_DIR, file);
    const outPath = join(GZIPPED_DIR, `${relPath}.gz`);

    mkdirSync(dirname(outPath), { recursive: true });

    const input = readFileSync(file);
    const gzipped = gzipSync(input, { level: 9, mtime: 0 });
    writeFileSync(outPath, gzipped);

    console.log(`Compressed: ${file} -> ${outPath}`);
  }

  console.log(`Compression completed. Processed ${files.length} files.`);
}

try {
  main();
} catch (err) {
  console.error(`Error: ${err instanceof Error ? err.message : String(err)}`);
  process.exit(1);
}
