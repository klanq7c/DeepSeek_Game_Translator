'use strict';
// 软件像素画布只替代浏览器边界；识别筛选、背景恢复及生命周期均执行正式模块。
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const EventEmitter = require('events');
const moduleFile = path.resolve(__dirname, '../../payloads/RPGMaker/rpgm_image_text.js');
const imageText = require(moduleFile);
class Canvas {
    constructor(w = 1, h = 1) { this._width = w; this._height = h; this.bytes = new Uint8ClampedArray(w * h * 4); this.ctx = new Context(this); }
    get width() { return this._width; }
    set width(value) { this._width = value; this.bytes = new Uint8ClampedArray(this._width * this._height * 4); }
    get height() { return this._height; }
    set height(value) { this._height = value; this.bytes = new Uint8ClampedArray(this._width * this._height * 4); }
    getContext() { if (this.bytes.length !== this.width * this.height * 4) this.bytes = new Uint8ClampedArray(this.width * this.height * 4); return this.ctx; }
    toDataURL() { return 'data:image/png;base64,' + Buffer.from(this.bytes).toString('base64'); }
}
class Context {
    constructor(c) { this.canvas = c; this.font = '16px fixture'; this.stack = []; this.writes = 0; }
    save() { this.stack.push(this.font); }
    restore() { this.font = this.stack.pop(); }
    beginPath() {}
    rect() {}
    clip() {}
    createImageData(w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; }
    getImageData(x, y, w, h) { const out = this.createImageData(w, h); for (let row = 0; row < h; row++) for (let col = 0; col < w; col++) for (let c = 0; c < 4; c++) out.data[(row * w + col) * 4 + c] = this.canvas.bytes[((row + y) * this.canvas.width + col + x) * 4 + c]; return out; }
    putImageData(data, x, y) { this.writes++; for (let row = 0; row < data.height; row++) for (let col = 0; col < data.width; col++) for (let c = 0; c < 4; c++) this.canvas.bytes[((row + y) * this.canvas.width + col + x) * 4 + c] = data.data[(row * data.width + col) * 4 + c]; }
    drawImage(source) { this.writes++; this.canvas.bytes.set(source.bytes); }
    measureText(text) { return { width: text.length * parseInt(this.font) }; }
    strokeText() {}
    fillText(text, x, y) { if (this.throwDraw) throw new Error('external font draw error'); const i = (Math.floor(y) * this.canvas.width + Math.floor(x)) * 4; this.canvas.bytes.set([255, 0, 128, 255], i); }
}
class Bitmap {
    constructor(w = 200, h = 60) { this.resize(w, h); this._listeners = []; this.loaded = true; this.smooth = true; this.dirties = 0; }
    resize(w, h) { this.width = w; this.height = h; this._canvas = new Canvas(w, h); this._context = this._canvas.getContext('2d'); }
    blt(source) { this._context.drawImage(source._canvas); }
    isReady() { return this.loaded; }
    addLoadListener(fn) { if (this.isReady()) fn(this); else this._listeners.push(fn); }
    _callLoadListeners() { while (this._listeners.length) this._listeners.shift()(this); }
    _setDirty() { this.dirties++; }
    isError() { return false; }
    destroy() { this.destroyed = true; }
}
const word = (text = 'Resolve', x = 30, y = 20, w = 70, h = 18) => ({ text, x, y, w, h });
const result = (a = [word()], b = a) => ({ width: 200, height: 60, passes: [{ lines: a.map(w => ({ words: [w] })) }, { lines: b.map(w => ({ words: [w] })) }] });
function sourceBitmap() { const bitmap = new Bitmap(); for (let y = 20; y < 38; y++) for (let x = 30; x < 100; x++) bitmap._canvas.bytes.set([240, 230, 210, 255], (y * 200 + x) * 4); return bitmap; }
function fixture(extra = {}, api = imageText) {
    let source = sourceBitmap(), pending, fontResolve;
    const errors = [], queries = [], values = new Map(), canvases = [];
    const document = { createElement() { const c = new Canvas(); canvases.push(c); return c; }, fonts: { load() { return new Promise(resolve => { fontResolve = resolve; }); } } };
    const manager = extra.ImageManager || { loadBitmap(folder, name, hue, smooth) { source.smooth = smooth; return source; } };
    const adapter = api.create({ root: 'C:/fixture', Bitmap, ImageManager: manager, document, font: 'fixture', system: () => ({ terms: { params: ['Resolve'] } }), diagnose(context, error) { errors.push({ context, message: error.message }); }, translate(text) { queries.push(text); return values.get(text) || text; }, recognize(png, callback) { pending = callback; }, ...extra });
    adapter.install();
    const bitmap = manager.loadBitmap('img/ui/unrelated/', 'panel-938', 12, true);
    const scene = { visible: true, children: [{ bitmap }] };
    return { adapter, manager, scene, bitmap, errors, queries, values, canvases, get source() { return source; }, replace() { source = sourceBitmap(); }, recognize(data = result()) { pending(null, data); }, fail() { pending(new Error('system OCR unavailable')); }, font() { fontResolve([{}]); } };
}
const tick = () => new Promise(resolve => setImmediate(resolve));
const cases = {
    async no_recognized_regions_skip_scene_discovery_and_late_results_resume_it() {
        const f = fixture(); let visits = 0;
        const scene = { get visible() { visits++; return true; }, children: Array.from({ length: 2047 }, () => ({ get visible() { visits++; return true; } })) };
        for (let frame = 0; frame < 300; frame++) { f.adapter.update(scene, 0); assert.strictEqual(f.adapter.hasVisibleTargets(scene), false); }
        assert.strictEqual(visits, 0, 'pending OCR cannot yield a render target; idle frames must not traverse the scene');
        f.recognize(result([]));
        for (let frame = 0; frame < 60; frame++) f.adapter.update(scene, frame);
        assert.strictEqual(visits, 0, 'empty OCR results must keep scene discovery idle');
        const live = fixture(); live.font(); await tick(); live.values.set('Resolve', '决心');
        live.adapter.update(live.scene, 0); live.recognize(); live.adapter.update(live.scene, 0);
        assert.strictEqual(live.bitmap.dirties, 1, 'late OCR must immediately enable discovery without a scene or cache change');
        live.bitmap.destroy();
        live.adapter.update(scene, 0); assert.strictEqual(live.adapter.hasVisibleTargets(scene), false);
        assert.strictEqual(visits, 0, 'destroying the last usable bitmap must release the discovery gate');
        const cancelled = fixture(); cancelled.bitmap.destroy(); cancelled.recognize(); cancelled.adapter.update(scene, 0);
        assert.strictEqual(visits, 0, 'late results for a destroyed bitmap must not reopen discovery');
    },
    metadata_and_two_pass_agreement_do_not_require_game_profiles() {
        const dictionary = imageText.lexicon({ terms: { params: ['Resolve'] }, equipTypes: ['Amber Focus'] });
        assert.strictEqual(dictionary.resolve, 'Resolve'); assert.strictEqual(dictionary.focus, 'Focus');
        assert.strictEqual(dictionary.focuses, undefined);
        assert.deepStrictEqual(imageText.selectRegions(result([word('Uncharted')]), dictionary).map(r => r.text), ['Uncharted']);
        assert.deepStrictEqual(imageText.selectRegions(result([word('Uncharted')], [word('Wrong')]), dictionary), []);
        assert.deepStrictEqual(imageText.selectRegions(result([word('Resolve')], [word('garbled')]), dictionary).map(r => r.text), ['Resolve']);
        assert.deepStrictEqual(imageText.selectRegions(result([word('(Amber)')]), dictionary).map(r => r.text), ['(Amber)']);
        assert.deepStrictEqual(imageText.selectRegions(result([word('Resolve')], [word('Focus')]), dictionary), []);
        assert.deepStrictEqual(imageText.selectRegions(result([word('Resolve', 30, 20, Infinity)]), dictionary), []);
        assert.deepStrictEqual(imageText.selectRegions(result([word('\\C[2]Resolve')]), dictionary), []);
        assert.ok(imageText.eligible('img/pictures/', 'unknown-card'));
        assert.ok(imageText.eligible('img/menus/custom/', 'any-layout'));
        assert.ok(!imageText.eligible('img/characters/', 'hero'));
        assert.ok(!imageText.eligible('img/system/', 'IconSet'));
    },
    unsafe_background_and_out_of_bounds_keep_the_original() {
        const source = sourceBitmap();
        assert.ok(imageText.prepareRegion(source._canvas, word()));
        assert.strictEqual(imageText.prepareRegion(source._canvas, word('Resolve', 1)), null);
        for (let y = 17; y < 41; y++) source._canvas.bytes[(y * 200 + 105) * 4] = 200;
        assert.strictEqual(imageText.prepareRegion(source._canvas, word()), null);
    },
    dense_ocr_results_have_a_bounded_layout_budget() {
        const lines = Array.from({ length: 30 }, (_, row) => ({ words: Array.from({ length: 20 }, (_, col) => word('Glyph', 10 + col * 80, 10 + row * 30, 20, 8)) }));
        assert.throws(() => imageText.selectRegions({ width: 2000, height: 1000, passes: [{ lines }, { lines }] }, {}), /OCR candidate budget/);
    },
    async_load_has_one_readiness_notification_and_preserves_arguments() {
        const source = sourceBitmap(); source.loaded = false; const seen = [];
        const f = fixture({ ImageManager: { loadBitmap() { seen.push(Array.from(arguments)); return source; } } });
        assert.strictEqual(f.bitmap.isReady(), false);
        let loaded = 0; f.bitmap.addLoadListener(() => loaded++);
        assert.strictEqual(loaded, 0, 'a pending engine bitmap must not notify before its source loads');
        source.loaded = true; source._callLoadListeners();
        assert.strictEqual(loaded, 1); assert.strictEqual(f.bitmap.isReady(), true);
        assert.strictEqual(f.bitmap.width, source.width); assert.strictEqual(f.bitmap.height, source.height);
        assert.deepStrictEqual(seen[0], ['img/ui/unrelated/', 'panel-938', 12, true]);
    },
    async_failure_has_a_diagnostic_and_never_changes_the_source() {
        const f = fixture(), original = Buffer.from(f.source._canvas.bytes); f.fail();
        assert.ok(f.errors.some(e => e.context === 'image-text-recognize'));
        assert.deepStrictEqual(Buffer.from(f.bitmap._canvas.bytes), original);
        assert.strictEqual(f.adapter.hasVisibleTargets(f.scene), false);
    },
    async draw_waits_for_font_then_survives_sixty_refreshes_and_cache_misses() {
        const f = fixture(), original = Buffer.from(f.source._canvas.bytes); f.recognize(); f.values.set('Resolve', '决心');
        f.adapter.update(f.scene, 0); assert.strictEqual(f.bitmap.dirties, 0);
        f.font(); await tick(); f.adapter.update(f.scene, 0);
        assert.strictEqual(f.bitmap.dirties, 1);
        const allocations = f.canvases.length;
        const translated = Buffer.from(f.bitmap._canvas.bytes); assert.notDeepStrictEqual(translated, original);
        f.values.clear();
        for (let i = 0; i < 60; i++) { f.adapter.refresh(); f.adapter.update(f.scene, i); assert.strictEqual(f.manager.loadBitmap('img/ui/unrelated/', 'panel-938', 12, true), f.bitmap); assert.deepStrictEqual(Buffer.from(f.bitmap._canvas.bytes), translated); }
        assert.strictEqual(f.bitmap.dirties, 1); assert.deepStrictEqual(Buffer.from(f.source._canvas.bytes), original);
        assert.strictEqual(f.canvases.length, allocations, 'unchanged translations must not allocate measurement or staging canvases');
        assert.deepStrictEqual([...new Set(f.queries)], ['Resolve']);
        const region = f.bitmap._dsRpgmAutoImageState.regions[0];
        for (let y = 0; y < 60; y++) for (let x = 0; x < 200; x++) if (x < region.x || x >= region.x + region.w || y < region.y || y >= region.y + region.h) assert.strictEqual(translated[(y * 200 + x) * 4], original[(y * 200 + x) * 4]);
        assert.deepStrictEqual(f.errors, []);
    },
    async rejected_later_translation_keeps_the_last_successful_pixels() {
        const f = fixture(); f.recognize(); f.font(); await tick();
        f.values.set('Resolve', '决心'); f.adapter.update(f.scene, 1);
        const translated = Buffer.from(f.bitmap._canvas.bytes);
        f.values.set('Resolve', '过长'.repeat(100)); f.adapter.update(f.scene, 2);
        assert.ok(Buffer.from(f.bitmap._canvas.bytes).equals(translated), 'an unrenderable update erased an already displayed translation');
        assert.strictEqual(f.bitmap._dsRpgmAutoImageState.applied[0], '决心', 'a rejected translation became the remembered display value');
        f.values.clear(); f.adapter.refresh(); f.adapter.update(f.scene, 3);
        assert.deepStrictEqual(Buffer.from(f.bitmap._canvas.bytes), translated);
        assert.ok(f.errors.some(e => e.context === 'image-text-fit'));
    },
    async rejected_region_does_not_erase_itself_when_a_neighbor_updates() {
        const source = sourceBitmap();
        for (let y = 20; y < 38; y++) for (let x = 130; x < 175; x++) source._canvas.bytes.set([240, 230, 210, 255], (y * 200 + x) * 4);
        const f = fixture({ ImageManager: { loadBitmap: () => source }, system: () => ({ terms: { params: ['Resolve', 'Guard'] } }) });
        f.recognize(result([word(), word('Guard', 130, 20, 45)])); f.font(); await tick();
        f.values.set('Resolve', '决心'); f.values.set('Guard', '防御'); f.adapter.update(f.scene, 1);
        const first = f.bitmap._context.getImageData(27, 17, 76, 24).data;
        f.values.set('Resolve', '长'.repeat(100)); f.values.set('Guard', '格挡'); f.adapter.update(f.scene, 2);
        assert.deepStrictEqual(f.bitmap._context.getImageData(27, 17, 76, 24).data, first);
        assert.strictEqual(f.bitmap._dsRpgmAutoImageState.applied[0], '决心'); assert.strictEqual(f.bitmap._dsRpgmAutoImageState.applied[1], '格挡');
        assert.strictEqual(f.bitmap.dirties, 2);
    },
    async mz_texture_notification_uses_the_committed_canvas() {
        class MzBitmap extends Bitmap {
            constructor(w, h) { super(w, h); this._setDirty = undefined; this.updates = 0; this.baseTexture = { update: () => { this.updates++; } }; }
        }
        const f = fixture({ Bitmap: MzBitmap }); f.recognize(); f.font(); await tick();
        f.values.set('Resolve', '决心'); f.adapter.update(f.scene, 1);
        assert.strictEqual(f.bitmap.updates, 1); assert.strictEqual(f.bitmap._dsRpgmAutoImageState.applied[0], '决心');
        f.values.clear(); for (let frame = 0; frame < 60; frame++) { f.adapter.refresh(); f.adapter.update(f.scene, frame + 2); }
        assert.strictEqual(f.bitmap.updates, 1, 'stable MZ frames must not repeatedly upload the texture');
    },
    async hidden_or_released_scenes_do_not_request_translations() {
        const f = fixture(); f.recognize(); f.font(); await tick();
        f.scene.children[0].visible = false; f.adapter.update(f.scene, 0); assert.deepStrictEqual(f.queries, []);
        f.scene.children[0].visible = true; f.adapter.update(f.scene, 0); assert.deepStrictEqual(f.queries, ['Resolve']);
        for (let frame = 0; frame < 60; frame++) f.adapter.update(f.scene, 0);
        assert.deepStrictEqual(f.queries, ['Resolve'], 'stable frames must not re-query or re-render');
        f.adapter.update({ children: [] }, 1); assert.deepStrictEqual(f.queries, ['Resolve']);
        const bad = {}; Object.defineProperty(bad, 'bitmap', { get() { throw new Error('plugin getter failed'); } });
        f.adapter.update({ children: [bad] }, 2); assert.ok(f.errors.some(e => e.context === 'image-text-scene-node'));
    },
    async queued_originals_and_bad_control_sequences_never_erase_english() {
        const f = fixture(), original = Buffer.from(f.bitmap._canvas.bytes); f.recognize(); f.font(); await tick();
        for (const value of ['Resolve', null, '决心\n破坏', '\\C[1]决心']) { f.values.set('Resolve', value); f.adapter.refresh(); f.adapter.update(f.scene, 0); assert.deepStrictEqual(Buffer.from(f.bitmap._canvas.bytes), original); }
        f.values.set('Resolve', '长'.repeat(200)); f.adapter.refresh(); f.adapter.update(f.scene, 1);
        assert.deepStrictEqual(Buffer.from(f.bitmap._canvas.bytes), original); assert.ok(f.errors.some(e => e.context === 'image-text-fit'));
    },
    async rendering_failure_does_not_commit_partial_pixels() {
        const f = fixture(), original = Buffer.from(f.bitmap._canvas.bytes); f.recognize(); f.font(); await tick(); f.values.set('Resolve', '决心');
        const create = f.canvases;
        // 正式模块在独立暂存画布绘制，注入第三方字体故障后目标位图必须完整保留。
        const save = Context.prototype.fillText; Context.prototype.fillText = function() { throw new Error('external draw failure'); };
        try { f.adapter.update(f.scene, 0); } finally { Context.prototype.fillText = save; }
        assert.ok(create.length); assert.deepStrictEqual(Buffer.from(f.bitmap._canvas.bytes), original);
        assert.ok(f.errors.some(e => e.context === 'image-text-region')); assert.strictEqual(f.bitmap._dsRpgmAutoImageState.signature, '');
        assert.strictEqual(f.bitmap._dsRpgmAutoImageState.applied[0], undefined, 'failed rendering must not remember an uncommitted translation');
    },
    cache_eviction_destroyed_clones_and_smoothing_follow_engine_ownership() {
        const f = fixture(); f.recognize();
        assert.strictEqual(f.manager.loadBitmap('img/ui/unrelated/', 'panel-938', 12, false), f.bitmap); assert.strictEqual(f.bitmap.smooth, false);
        f.bitmap.destroy(); const recreated = f.manager.loadBitmap('img/ui/unrelated/', 'panel-938', 12, true);
        assert.notStrictEqual(recreated, f.bitmap); assert.ok(!f.source.destroyed);
        f.replace(); const evicted = f.manager.loadBitmap('img/ui/unrelated/', 'panel-938', 12, true); assert.notStrictEqual(evicted, recreated);
        assert.strictEqual(evicted._dsRpgmAutoImageState.regions.length, 1, 'content hash should reuse OCR without reusing the old bitmap');
    },
    late_plugin_wrapper_does_not_clone_an_already_translated_bitmap() {
        const f = fixture(), old = f.manager.loadBitmap; let calls = 0;
        f.manager.loadBitmap = function() { calls++; return old.apply(this, arguments); };
        f.adapter.install();
        assert.strictEqual(f.manager.loadBitmap('img/ui/unrelated/', 'panel-938', 12, false), f.bitmap);
        assert.strictEqual(calls, 1); assert.strictEqual(f.bitmap._dsRpgmAutoImageState.source, f.source);
        assert.strictEqual(f.bitmap.smooth, false);
    },
    production_hook_installs_module_after_plugins_and_updates_current_scene() {
        let options, installed = 0, updated = 0, original = 0, closed = 0; const events = {}, errors = [];
        const manager = { loadBitmap() { return {}; } }, scene = { children: [] };
        const adapter = { install() { installed++; }, update(value) { assert.strictEqual(value, scene); assert.strictEqual(original, 1); updated++; }, hasVisibleTargets: () => true, refresh() {}, close() { closed++; } };
        const document = { createElement: () => ({}), head: { appendChild() {} }, fonts: { load: () => Promise.resolve([{}]) }, getElementsByTagName: () => [], addEventListener() {} };
        const context = { document, Bitmap, ImageManager: manager, SceneManager: { _scene: scene, updateScene() { original++; } }, process: { platform: 'win32', versions: { nw: 'fixture' }, mainModule: { filename: 'C:\\unrelated-fixture\\index.html' } }, require(name) { if (name === 'fs') return { realpathSync: value => value, statSync() { throw Object.assign(new Error('optional manifest absent'), { code: 'ENOENT' }); } }; if (name.endsWith('rpgm_image_text.js')) return { create(value) { options = value; return adapter; } }; return require(name); }, console: { error(e) { errors.push(e); }, info() {} }, addEventListener(name, fn) { events[name] = fn; }, setTimeout() { throw new Error('module installation must use lifecycle events'); } };
        context.window = context; vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../../payloads/RPGMaker/hook_rpgm_mv.js'), 'utf8'), context);
        assert.ok(installed); assert.strictEqual(options.Bitmap, Bitmap); assert.strictEqual(options.ImageManager, manager); assert.strictEqual(options.font, 'DeepSeekCJK'); assert.strictEqual(options.root, 'C:\\unrelated-fixture');
        context.SceneManager.updateScene(); assert.strictEqual(updated, 1); events.unload(); assert.strictEqual(closed, 1); assert.deepStrictEqual(errors, []);
    },
    async worker_protocol_is_hidden_bounded_and_reports_bad_external_data() {
        const children = [], timers = new Map(); let counter = 0;
        function spawn(file, args, options) { assert.ok(file.endsWith('powershell.exe')); assert.strictEqual(options.windowsHide, true); assert.strictEqual(options.shell, undefined); assert.ok(Buffer.from(args[args.indexOf('-EncodedCommand') + 1], 'base64').toString('utf16le').includes('[Text.Encoding]::UTF8')); const child = new EventEmitter(); child.stdin = new EventEmitter(); child.stdout = new EventEmitter(); child.stderr = new EventEmitter(); child.sent = []; child.stdin.write = line => child.sent.push(JSON.parse(line)); child.stdin.end = () => { child.ended = true; }; child.kill = () => { child.killed = true; }; children.push(child); return child; }
        const context = { module: { exports: {} }, exports: {}, Buffer, process: { env: { SystemRoot: 'C:/Windows' } }, setTimeout(fn, ms) { const id = ++counter; timers.set(id, { fn, ms }); return id; }, clearTimeout(id) { timers.delete(id); }, require(name) { if (name === 'child_process') return { spawn }; if (name === 'fs') return { statSync() { return { isFile: () => true }; } }; return require(name); } };
        vm.runInNewContext(fs.readFileSync(moduleFile, 'utf8'), context);
        const f = fixture({ recognize: undefined }, context.module.exports); f.font(); await tick();
        const child = children[0]; child.stdout.emit('data', Buffer.from('{"ready":true,"version":1}\n'));
        assert.strictEqual(child.sent.length, 1); const id = child.sent[0].id;
        child.stdout.emit('data', Buffer.from(JSON.stringify({ id, result: result() }) + '\n')); assert.strictEqual(f.adapter.hasVisibleTargets(f.scene), true);
        const idle = [...timers.values()].find(t => t.ms === 10000); assert.ok(idle); idle.fn(); assert.ok(child.ended);
        child.stdin.emit('error', new Error('late closed pipe')); assert.deepStrictEqual(f.errors, [], 'old child must not disable the next worker');
        const g = fixture({ recognize: undefined }, context.module.exports); const second = children[1];
        second.stdout.emit('data', Buffer.from('{bad json}\n')); assert.ok(second.killed); assert.ok(g.errors.some(e => e.context === 'image-ocr-worker')); assert.ok(g.bitmap.isReady()); g.adapter.close(); f.adapter.close();
        const h = fixture({ recognize: undefined }, context.module.exports); const third = children[2];
        assert.doesNotThrow(() => third.stdout.emit('data', Buffer.from('null\n')), 'valid JSON with an invalid protocol shape must not crash the game');
        assert.ok(third.killed); assert.ok(h.errors.some(e => e.context === 'image-ocr-worker')); assert.ok(h.bitmap.isReady()); h.adapter.close();
        const invalidId = fixture({ recognize: undefined }, context.module.exports); const fourth = children[3];
        fourth.stdout.emit('data', Buffer.from('{"ready":true,"version":1}\n'));
        assert.doesNotThrow(() => fourth.stdout.emit('data', Buffer.from('{"id":{"toString":null,"valueOf":null},"result":{}}\n')));
        assert.ok(fourth.killed); assert.ok(invalidId.errors.some(e => e.context === 'image-ocr-worker')); invalidId.adapter.close();
    }
};
(async () => { let failed = 0; for (const [name, test] of Object.entries(cases)) { try { await test(); console.log('PASS ' + name); } catch (error) { failed++; console.error('FAIL ' + name, error); } } console.log((failed ? 'FAIL ' : 'PASS ') + Object.keys(cases).length + ' automatic image text cases, failures=' + failed); if (failed) process.exitCode = 1; })().catch(error => { console.error(error); process.exitCode = 1; });
