#!/usr/bin/env node
// Compares the native player's pictures with the Chrome pictures of the browser port.
//
//   node tools/compare_goldens.mjs --chrome DIR --native DIR [--mean N] [--gross FRACTION] [--block N] [--diffs DIR] [--quiet]
//
// DIR/frame_<milliseconds>.png (Chrome, written by tools/capture_chrome_goldens.mjs) against DIR/frame_<milliseconds>.ppm (debris --times ...
// --screenshot-dir). The two renderers are not bit identical - SwiftShader against Mesa llvmpipe or a GPU, Chrome's canvas fonts against the
// player's built-in glyph rasterizer, one-ulp differences of the texture generators - so a picture passes when it is close:
//   mean   the mean absolute difference per channel, 0..255, must stay at or below --mean (default 3.0),
//   gross  the share of pixels where some channel differs by more than 48 must stay at or below --gross (default 0.02),
//   block  the mean absolute difference of the worst 16x16 block must stay at or below --block (default 60): an object that is missing or in
//          the wrong place shows there even when it is small.
// The limits were set from the 60 pictures of the two golden sets (worst mean 2.3, worst gross 1.3 %, worst block 41).
// --control proves that the limits can tell a wrong picture from a right one: every Chrome picture is compared with the native picture of the
// next time, and the run succeeds only when every one of those comparisons fails.
// Exit 0 when every picture passes, 1 otherwise; --diffs DIR writes an amplified difference image (PPM) of every failing picture.
import { readFileSync, readdirSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { inflateSync } from 'node:zlib';
import { join, resolve } from 'node:path';

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// 8 bit RGB / RGBA PNG without interlacing -> { width, height, rgb: Uint8Array(w*h*3) }
export function decodePng(buffer) {
  const signature = [137, 80, 78, 71, 13, 10, 26, 10];
  if (buffer.length < 8 || signature.some((b, i) => buffer[i] !== b)) throw new Error('not a PNG');
  let offset = 8, width = 0, height = 0, depth = 0, colorType = 0, interlace = 0;
  const data = [];
  while (offset + 8 <= buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString('latin1', offset + 4, offset + 8);
    const body = buffer.subarray(offset + 8, offset + 8 + length);
    if (type === 'IHDR') {
      width = body.readUInt32BE(0); height = body.readUInt32BE(4);
      depth = body[8]; colorType = body[9]; interlace = body[12];
    } else if (type === 'IDAT') data.push(body);
    else if (type === 'IEND') break;
    offset += 12 + length;
  }
  if (depth !== 8 || (colorType !== 2 && colorType !== 6) || interlace !== 0 || !width || !height) {
    throw new Error(`unsupported PNG (depth ${depth}, color type ${colorType}, interlace ${interlace})`);
  }
  const channels = colorType === 6 ? 4 : 3;
  const stride = width * channels;
  const raw = inflateSync(Buffer.concat(data));
  if (raw.length !== (stride + 1) * height) throw new Error('PNG data has the wrong size');
  const pixels = new Uint8Array(stride * height);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const out = pixels.subarray(y * stride, (y + 1) * stride);
    const prev = y ? pixels.subarray((y - 1) * stride, y * stride) : null;
    for (let x = 0; x < stride; x++) {
      const left = x >= channels ? out[x - channels] : 0;
      const up = prev ? prev[x] : 0;
      const upLeft = prev && x >= channels ? prev[x - channels] : 0;
      let value = line[x];
      if (filter === 1) value += left;
      else if (filter === 2) value += up;
      else if (filter === 3) value += (left + up) >> 1;
      else if (filter === 4) value += paeth(left, up, upLeft);
      else if (filter !== 0) throw new Error(`PNG filter ${filter}`);
      out[x] = value & 255;
    }
  }
  const rgb = new Uint8Array(width * height * 3);
  for (let i = 0; i < width * height; i++) {
    rgb[i * 3] = pixels[i * channels]; rgb[i * 3 + 1] = pixels[i * channels + 1]; rgb[i * 3 + 2] = pixels[i * channels + 2];
  }
  return { width, height, rgb };
}

export function decodePpm(buffer) {
  let offset = 0;
  const token = () => {
    while (buffer[offset] === 32 || buffer[offset] === 10 || buffer[offset] === 13 || buffer[offset] === 9) offset++;
    const start = offset;
    while (offset < buffer.length && ![32, 10, 13, 9].includes(buffer[offset])) offset++;
    return buffer.toString('latin1', start, offset);
  };
  if (token() !== 'P6') throw new Error('not a P6 PPM');
  const width = Number(token()), height = Number(token());
  if (token() !== '255') throw new Error('PPM maxval is not 255');
  offset++;
  const rgb = new Uint8Array(buffer.subarray(offset, offset + width * height * 3));
  if (rgb.length !== width * height * 3) throw new Error('PPM data is short');
  return { width, height, rgb };
}

export function compare(a, b, blockSize = 16) {
  if (a.width !== b.width || a.height !== b.height) throw new Error(`size ${a.width}x${a.height} against ${b.width}x${b.height}`);
  const { width, height } = a;
  let sum = 0, gross = 0;
  const blocksX = Math.ceil(width / blockSize), blocksY = Math.ceil(height / blockSize);
  const blockSums = new Float64Array(blocksX * blocksY), blockCounts = new Uint32Array(blocksX * blocksY);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 3;
      const d0 = Math.abs(a.rgb[i] - b.rgb[i]), d1 = Math.abs(a.rgb[i + 1] - b.rgb[i + 1]), d2 = Math.abs(a.rgb[i + 2] - b.rgb[i + 2]);
      const d = d0 + d1 + d2;
      sum += d;
      if (Math.max(d0, d1, d2) > 48) gross++;
      const block = Math.floor(y / blockSize) * blocksX + Math.floor(x / blockSize);
      blockSums[block] += d;
      blockCounts[block]++;
    }
  }
  let worstBlock = 0;
  for (let k = 0; k < blockSums.length; k++) worstBlock = Math.max(worstBlock, blockSums[k] / (blockCounts[k] * 3));
  const mean = sum / (width * height * 3);
  return { mean, gross: gross / (width * height), block: worstBlock, psnr: mean === 0 ? Infinity : null };
}

function diffImage(a, b) {
  const out = Buffer.alloc(a.width * a.height * 3);
  for (let i = 0; i < out.length; i++) out[i] = Math.min(255, Math.abs(a.rgb[i] - b.rgb[i]) * 4);
  return Buffer.concat([Buffer.from(`P6\n${a.width} ${a.height}\n255\n`), out]);
}

function main(argv) {
  const options = { chrome: null, native: null, mean: 3.0, gross: 0.02, block: 60, diffs: null, quiet: false, control: false };
  for (let i = 0; i < argv.length; i++) {
    const name = argv[i];
    if (name === '--quiet') { options.quiet = true; continue; }
    if (name === '--control') { options.control = true; continue; }
    const value = argv[++i];
    if (value === undefined) throw new Error(`${name} needs a value`);
    if (name === '--chrome') options.chrome = resolve(value);
    else if (name === '--native') options.native = resolve(value);
    else if (name === '--diffs') options.diffs = resolve(value);
    else if (name === '--mean' || name === '--gross' || name === '--block') {
      options[name.slice(2)] = Number(value);
      if (!(options[name.slice(2)] >= 0)) throw new Error(`${name} wants a number`);
    } else throw new Error(`unknown option ${name}`);
  }
  if (!options.chrome || !options.native) throw new Error('--chrome DIR and --native DIR are required');
  const names = readdirSync(options.chrome).filter(n => /^frame_\d+\.png$/.test(n)).sort();
  if (!names.length) throw new Error(`no frame_*.png in ${options.chrome}`);
  let failures = 0, worstMean = 0, worstGross = 0, worstBlock = 0, totalMean = 0;
  if (options.diffs) mkdirSync(options.diffs, { recursive: true });
  for (let index = 0; index < names.length; index++) {
    const name = names[index];
    const stem = name.slice(0, -4);
    // the control pairs every Chrome picture with the native picture of the next time (the last one with the first)
    const nativeStem = options.control ? names[(index + 1) % names.length].slice(0, -4) : stem;
    const nativePath = join(options.native, `${nativeStem}.ppm`);
    if (!existsSync(nativePath)) { console.log(`${stem}: no native picture`); failures++; continue; }
    const chrome = decodePng(readFileSync(join(options.chrome, name)));
    const native = decodePpm(readFileSync(nativePath));
    let result;
    try { result = compare(chrome, native); } catch (error) { console.log(`${stem}: ${error.message}`); failures++; continue; }
    const within = result.mean <= options.mean && result.gross <= options.gross && result.block <= options.block;
    const ok = options.control ? !within : within;                // in the control a wrong pair must fail the limits
    if (!ok) failures++;
    worstMean = Math.max(worstMean, result.mean); worstGross = Math.max(worstGross, result.gross); worstBlock = Math.max(worstBlock, result.block);
    totalMean += result.mean;
    if (!options.quiet || !ok) {
      console.log(`${stem}${options.control ? ` against ${nativeStem}` : ''}: mean ${result.mean.toFixed(3)}  gross ${(result.gross * 100).toFixed(3)} %  worst 16x16 block ${result.block.toFixed(2)}  ${ok ? 'ok' : 'FAIL'}`);
    }
    if (!ok && options.diffs && !options.control) writeFileSync(join(options.diffs, `${stem}.diff.ppm`), diffImage(chrome, native));
  }
  console.log(`${names.length} pictures${options.control ? ' (control: each against the next time, all must fail the limits)' : ''}: worst mean ${worstMean.toFixed(3)}, average mean ${(totalMean / names.length).toFixed(3)}, worst gross ${(worstGross * 100).toFixed(3)} %, ` +
    `worst block ${worstBlock.toFixed(2)}; limits ${options.mean} / ${(options.gross * 100).toFixed(2)} % / ${options.block}; ${failures} ${options.control ? 'wrongly passing' : 'failing'}`);
  return failures ? 1 : 0;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  try { process.exitCode = main(process.argv.slice(2)); } catch (error) { console.error(`compare_goldens: ${error.message}`); process.exitCode = 2; }
}
