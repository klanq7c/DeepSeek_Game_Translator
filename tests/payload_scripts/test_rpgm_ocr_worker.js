'use strict';
const assert = require('assert');
const path = require('path');
const { spawnSync } = require('child_process');
if (process.platform !== 'win32') { console.log('SKIP Windows OCR worker: Windows runtime required'); process.exit(0); }
const powershell = path.join(process.env.SystemRoot, 'System32/WindowsPowerShell/v1.0/powershell.exe');
function run(script, input = '') {
    return spawnSync(powershell, ['-NoLogo', '-NoProfile', '-NonInteractive', '-InputFormat', 'Text', '-OutputFormat', 'Text', '-EncodedCommand', Buffer.from("$ProgressPreference='SilentlyContinue';$ErrorActionPreference='Stop';" + script, 'utf16le').toString('base64')], { input, encoding: 'utf8', timeout: 45000, windowsHide: true, maxBuffer: 4 * 1024 * 1024 });
}
const fixture = run(`
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null=[Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]
if (-not ([Windows.Media.Ocr.OcrEngine]::AvailableRecognizerLanguages | Where-Object {$_.LanguageTag -match '^en-'})) {exit 77}
Add-Type -AssemblyName System.Drawing
$b=[Drawing.Bitmap]::new(620,120);$g=[Drawing.Graphics]::FromImage($b)
try {$g.Clear([Drawing.Color]::FromArgb(56,38,73));$f=[Drawing.Font]::new('Arial',28,[Drawing.FontStyle]::Regular,[Drawing.GraphicsUnit]::Pixel)
try {$g.DrawString('Journal',$f,[Drawing.Brushes]::White,24,14);$g.DrawString('Return',$f,[Drawing.Brushes]::White,410,64)}finally{$f.Dispose()}
$s=[IO.MemoryStream]::new();try{$b.Save($s,[Drawing.Imaging.ImageFormat]::Png);[Console]::WriteLine([Convert]::ToBase64String($s.ToArray()))}finally{$s.Dispose()}
}finally{$g.Dispose();$b.Dispose()}
`);
if (fixture.status === 77) { console.log('SKIP Windows OCR worker: English OCR language pack is not installed; renderer failure guards still run'); process.exit(0); }
assert.strictEqual(fixture.status, 0, fixture.stderr || String(fixture.error));
const png = fixture.stdout.trim(); assert.ok(png.startsWith('iVBOR'));
const helper = path.resolve(__dirname, '../../payloads/RPGMaker/dst_rpgm_ocr.ps1');
const input = [{ id: 1, png }, { id: 2, png: 'invalid PNG' }, { id: 3, png }].map(v => JSON.stringify(v)).join('\n') + '\n';
const response = run("& ([scriptblock]::Create([IO.File]::ReadAllText('" + helper.replace(/'/g, "''") + "',[Text.Encoding]::UTF8)))", input);
assert.strictEqual(response.status, 0, response.stderr || String(response.error));
const lines = response.stdout.trim().split(/\r?\n/).map(line => JSON.parse(line));
assert.deepStrictEqual(lines[0], { ready: true, version: 1 }); assert.strictEqual(lines.length, 4);
const imageText = require('../../payloads/RPGMaker/rpgm_image_text');
for (const index of [1, 3]) {
    assert.strictEqual(lines[index].id, index); const result = lines[index].result;
    assert.strictEqual(result.width, 620); assert.strictEqual(result.height, 120);
    assert.deepStrictEqual(imageText.selectRegions(result, {}).map(r => r.text).sort(), ['Journal', 'Return']);
    assert.ok(result.passes.every(pass => pass.lines.every(line => line.words.every(w => w.x >= 0 && w.y >= 0 && w.x + w.w <= 620 && w.y + w.h <= 120))));
}
assert.strictEqual(lines[2].id, 2); assert.ok(lines[2].error); assert.ok(response.stderr.includes('image-ocr-decode-recognize id=2'));
console.log('PASS actual Windows OCR: independent texture positions, two backgrounds, JSONL ownership and decode failure recovery');
