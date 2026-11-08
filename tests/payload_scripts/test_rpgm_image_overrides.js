// 通过正式 hook 的 ImageManager 边界验证图片适配，不恢复插件关闭的文字绘制。
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');
const hook = fs.readFileSync(path.join(__dirname, '../../payloads/RPGMaker/hook_rpgm_mv.js'), 'utf8');
const base = 'C:\\rpgm-image-fixture';
const source = 'img/menus/equip/LayoutCommand.png';
const replacement = 'img/dst_translated/menus/equip/LayoutCommand.png';
const manifestPath = path.win32.join(base, 'js/dst_rpgm_image_overrides.json');
function png(width, height, marker) {
    const b = Buffer.alloc(32);
    Buffer.from('89504e470d0a1a0a', 'hex').copy(b);
    b.writeUInt32BE(13, 8); b.write('IHDR', 12); b.writeUInt32BE(width, 16); b.writeUInt32BE(height, 20);
    b[31] = marker; return b;
}
const originalBytes = png(810, 53, 1), translatedBytes = png(810, 53, 2);
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
function entry(extra = {}) {
    return { source, replacement, sourceSha256: sha(originalBytes), replacementSha256: sha(translatedBytes), ...extra };
}
function fixture({ images = [entry()], absent = false, invalidJson = false, changed = false,
                   differentSize = false, readError = null } = {}) {
    const files = new Map(), reads = [], errors = [], infos = [], listeners = {}, cache = new Map();
    files.set(path.win32.join(base, source), originalBytes);
    files.set(path.win32.join(base, replacement), differentSize ? png(811, 53, 2) : translatedBytes);
    if (differentSize) images[0].replacementSha256 = sha(files.get(path.win32.join(base, replacement)));
    if (changed) files.set(path.win32.join(base, source), png(810, 53, 3));
    if (!absent) files.set(manifestPath, Buffer.from(invalidJson ? '{broken' : JSON.stringify({ schema: 1, images })));
    const fakeFs = {
        statSync(file) { if (!files.has(file)) throw Object.assign(new Error('missing ' + file), { code: 'ENOENT' }); return { size: files.get(file).length }; },
        readFileSync(file) {
            reads.push(file);
            if (readError) throw Object.assign(new Error('file access denied'), { code: readError });
            if (!files.has(file)) throw Object.assign(new Error('missing ' + file), { code: 'ENOENT' });
            return files.get(file);
        },
        realpathSync(file) { return file; }
    };
    const ImageManager = { loadBitmap(folder, name, hue, smooth) {
        const key = folder + name + '.png:' + (hue || 0);
        if (!cache.has(key)) cache.set(key, { key, folder, name, hue, smooth, extra: arguments[4], width: 810, height: 53 });
        return cache.get(key);
    } };
    function Window_Base() {}
    const disabledDraw = function() {};
    function Window_EquipCommand() {}
    Window_EquipCommand.prototype = Object.create(Window_Base.prototype);
    Window_EquipCommand.prototype.drawText = disabledDraw;
    function Window_EquipStatus() {}
    Window_EquipStatus.prototype.drawParamName = disabledDraw;
    function Game_Message() { this._texts = ['Keep this page.']; }
    Game_Message.prototype.allText = function() { return this._texts.join('\n'); };
    const gameMessage = new Game_Message();
    const document = { head: { appendChild() {} }, createElement() { return {}; }, getElementsByTagName() { return []; },
        addEventListener(name, fn) { (listeners[name] ||= []).push(fn); }, fonts: { load() { return Promise.resolve([{}]); } } };
    const console = { error(x) { errors.push(x); }, info(x) { infos.push(x); } };
    const context = { document, console, Buffer, ImageManager, Window_Base, Window_EquipCommand, Window_EquipStatus,
        Game_Message, $gameMessage: gameMessage, process: { versions: { nw: 'fixture' }, mainModule: { filename: path.win32.join(base, 'index.html') } },
        require(name) { if (name === 'fs') return fakeFs; if (name === 'path') return path.win32;
            if (name === 'crypto') return crypto; throw new Error('unexpected dependency: ' + name); },
        setTimeout() { throw new Error('image substitution must not create refresh timers'); },
        addEventListener(name, fn) { (listeners[name] ||= []).push(fn); } };
    context.window = context;
    vm.createContext(context); vm.runInContext(hook, context);
    return { ImageManager, cache, files, reads, errors, infos, context, gameMessage, disabledDraw,
        loaded() { for (const fn of listeners.load || []) fn(); } };
}

const cases = {
    original_bug_image_labels_have_a_separate_translation_entry() {
        const f = fixture();
        const bitmap = f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 0, true);
        assert.strictEqual(bitmap.folder, 'img/dst_translated/menus/equip/', 'baked image labels still bypass translation');
        assert.strictEqual(bitmap.name, 'LayoutCommand');
        assert.deepStrictEqual(f.files.get(path.win32.join(base, source)), originalBytes, 'original image bytes changed');
        assert.deepStrictEqual(f.errors, []);
    },
    sixty_refreshes_preserve_bitmap_identity_hue_geometry_and_disabled_draws() {
        const f = fixture(), first = f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 42, false);
        const reads = f.reads.length;
        for (let frame = 0; frame < 60; frame++) {
            f.loaded();
            assert.strictEqual(f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 42, false), first);
            assert.strictEqual(first.width, 810); assert.strictEqual(first.height, 53);
            assert.strictEqual(first.hue, 42); assert.strictEqual(first.smooth, false);
            assert.strictEqual(f.context.Window_EquipCommand.prototype.drawText, f.disabledDraw);
            assert.strictEqual(f.context.Window_EquipStatus.prototype.drawParamName, f.disabledDraw);
            assert.strictEqual(f.gameMessage.allText(), 'Keep this page.');
            assert.deepStrictEqual(f.gameMessage._texts, ['Keep this page.']);
        }
        assert.strictEqual(f.reads.length, reads, 'refresh reread or regenerated image data');
        assert.strictEqual(first.folder, 'img/dst_translated/menus/equip/', 'translation disappeared after refresh');
        f.cache.clear();
        const reopened = f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 42, false);
        assert.notStrictEqual(reopened, first, 'the engine did not own cache replacement');
        assert.strictEqual(reopened.folder, first.folder, 'translation disappeared after image cache eviction');
        assert.strictEqual(f.ImageManager.loadBitmap('img/system/', 'Window', 0, true).folder, 'img/system/');
    },
    missing_map_keeps_existing_engine_behavior() {
        const f = fixture({ absent: true });
        assert.strictEqual(f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 0, true).folder, 'img/menus/equip/');
        assert.deepStrictEqual(f.errors, []);
    },
    plugin_arguments_and_late_wrappers_are_preserved() {
        const f = fixture(), options = { pluginOption: true };
        const first = f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 0, true, options);
        assert.strictEqual(first.extra, options, 'plugin-specific loader options were lost');
        const old = f.ImageManager.loadBitmap;
        let calls = 0;
        f.ImageManager.loadBitmap = function() { calls++; return old.apply(this, arguments); };
        f.loaded();
        assert.strictEqual(f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 0, true, options), first);
        assert.strictEqual(calls, 1, 'late plugin loader was bypassed or called twice');
    },
    encrypted_runtime_keeps_the_original_decrypt_path() {
        const f = fixture(); f.context.Decrypter = { hasEncryptedImages: true };
        assert.strictEqual(f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 0, true).folder, 'img/menus/equip/');
        assert.ok(f.errors.some(e => e.includes('image-overrides-encrypted-runtime')));
    },
    changed_images_bad_geometry_and_file_errors_keep_originals_and_diagnose() {
        for (const options of [{ changed: true }, { differentSize: true }, { invalidJson: true }, { readError: 'EACCES' }]) {
            const f = fixture(options);
            assert.strictEqual(f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 0, true).folder, 'img/menus/equip/');
            assert.ok(f.errors.some(e => e.includes('image-overrides')), 'image failure was hidden: ' + JSON.stringify(options));
        }
    },
    traversal_and_recursive_replacement_are_rejected() {
        for (const image of [entry({ replacement: '../save/file1.rpgsave' }), entry({ replacement: source }), entry({ source: '../config/api.ini' })]) {
            const f = fixture({ images: [image] });
            assert.strictEqual(f.ImageManager.loadBitmap('img/menus/equip/', 'LayoutCommand', 0, true).folder, 'img/menus/equip/');
            assert.ok(f.errors.length, 'unsafe manifest was accepted');
        }
    }
};
for (const [name, run] of Object.entries(cases)) { run(); console.log('[PASS] ' + name); }
