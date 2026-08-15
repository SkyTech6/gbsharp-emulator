/*
 * Copyright (C) 2026 GB# contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

/*
 * Runs a ROM through the wasm build and prints the SHA1 of the screen, in
 * exactly the format binjgb's own tester.c writes it.
 *
 * That hash is recorded per ROM in scripts/test.json, so this answers the only
 * question worth asking about a second build of an emulator: does it produce
 * the same pixels as the first one. Without it, "the wasm target compiles" is
 * all CI could claim.
 *
 *   node web/check-wasm.mjs out-web/gbsharp.js test/blargg/cpu_instrs.gb 1780
 */
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

import { GameBoy, Screen } from './gbsharp-runtime.js';

const [modulePath, romPath, frameCount] = process.argv.slice(2);

if (!modulePath || !romPath) {
  console.error('usage: node check-wasm.mjs <gbsharp.js> <rom> [frames]');
  process.exit(2);
}

const frames = Number.parseInt(frameCount ?? '60', 10);

const { default: factory } = await import(pathToFileURL(modulePath).href);
const game = await GameBoy.create(factory);

if (!game.loadRom(new Uint8Array(readFileSync(romPath)))) {
  console.error('the runtime rejected this ROM');
  process.exit(1);
}

for (let i = 0; i < frames; i++) {
  game.runFrame();
}

/* The same P3 text tester.c writes, so the hash is comparable. */
const pixels = game.framebuffer;
let text = `P3\n${Screen.width} ${Screen.height}\n255\n`;

for (let y = 0; y < Screen.height; y++) {
  for (let x = 0; x < Screen.width; x++) {
    const pixel = pixels[(y * Screen.width) + x];
    const r = String(pixel & 0xff).padStart(3);
    const g = String((pixel >> 8) & 0xff).padStart(3);
    const b = String((pixel >> 16) & 0xff).padStart(3);
    text += `${r} ${g} ${b} `;
  }
  text += '\n';
}

console.log(createHash('sha1').update(text, 'ascii').digest('hex'));
game.destroy();
