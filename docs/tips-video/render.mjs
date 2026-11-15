// node render.mjs <clip> stills t1,t2,...   -> still_<clip>_<t>.png
// node render.mjs <clip> video               -> tip<clip>.mp4 (30 fps)
import puppeteer from 'puppeteer-core';
import ffmpeg from 'ffmpeg-static';
import { execFileSync } from 'node:child_process';
import { mkdirSync, rmSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const [clip, mode, times] = process.argv.slice(2);
const browser = await puppeteer.launch({
  executablePath: 'C:/Program Files/imput/Helium/Application/chrome.exe',
  headless: 'new',
  args: ['--allow-file-access-from-files', '--force-device-scale-factor=1'],
});
const page = await browser.newPage();
await page.setViewport({ width: 720, height: 800, deviceScaleFactor: 1 });
await page.goto(pathToFileURL(path.join(here, 'tips.html')).href + `?clip=${clip}`);
await page.evaluate(() => document.fonts.ready);
const dur = await page.evaluate(() => window.DUR);

async function shot(t, file) {
  await page.evaluate(tt => window.render(tt), t);
  await page.screenshot({ path: file, type: 'png' });
}

if (mode === 'stills') {
  for (const t of times.split(',').map(Number)) await shot(t, path.join(here, `still_${clip}_${t}.png`));
} else {
  const dir = path.join(here, `frames${clip}`);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir);
  const fps = 30, n = Math.round(dur * fps);
  for (let i = 0; i < n; i++) await shot(i / fps, path.join(dir, String(i).padStart(4, '0') + '.png'));
  execFileSync(ffmpeg, ['-y', '-loglevel', 'error', '-framerate', String(fps), '-i', path.join(dir, '%04d.png'),
    '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-preset', 'slow', '-crf', '24', '-movflags', '+faststart',
    path.join(here, `tip${clip}.mp4`)]);
  console.log(`tip${clip}.mp4: ${n} frames, ${dur}s`);
}
await browser.close();
