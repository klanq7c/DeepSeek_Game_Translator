// 经过正式钩子的逐字绘制入口验证独立插件渲染，不依赖任何游戏名称或任务词表。
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const hookPath = path.resolve(__dirname, '../../payloads/RPGMaker/hook_rpgm_mv.js');

function fixture(buffered = false) {
    let now = 0, serial = 0;
    const timers = [], requests = [], errors = [], hits = Object.create(null), listeners = {};
    function setTimeout(callback, delay) { timers.push({ callback, due: now + (Number(delay) || 0), serial: ++serial }); }
    function advance(ms) {
        const end = now + ms;
        for (let steps = 0; ; steps++) {
            timers.sort((a, b) => a.due - b.due || a.serial - b.serial);
            if (!timers.length || timers[0].due > end) break;
            assert.ok(steps < 2000, 'unbounded renderer/request loop');
            const timer = timers.shift(); now = timer.due; timer.callback();
        }
        now = end;
    }
    class Clock extends Date { static now() { return now; } }
    function XMLHttpRequest() {}
    XMLHttpRequest.prototype.open = function(method, url) { this.url = url; };
    XMLHttpRequest.prototype.setRequestHeader = function() {};
    XMLHttpRequest.prototype.send = function(body) {
        const payload = JSON.parse(body); requests.push({ url: this.url, payload });
        setTimeout(() => {
            const found = Object.create(null);
            for (const key of payload.texts || []) if (hits[key]) found[key] = hits[key];
            this.status = 200; this.readyState = 4;
            this.responseText = JSON.stringify(this.url.endsWith('/cache/lookup') ? { hits: found } :
                { translated_text: hits[payload.text] || payload.text, source: hits[payload.text] ? 'cache' : 'queued' });
            this.onreadystatechange();
        }, 1);
    };
    function Bitmap() {
        this.width = 300; this.height = 1200; this.fontFace = 'GameFont'; this.fontSize = 28;
        this.textColor = 'color0'; this.fontBold = false; this.fontItalic = false; this.drawn = [];
    }
    Bitmap.prototype.drawText = function(text, x, y) {
        // MZ 会提交空缓冲；它不产生可见字形，不能参与颜色或行位置断言。
        if (!String(text).length) return;
        this.drawn.push({ text, x, y, color: this.textColor, size: this.fontSize, bold: this.fontBold, italic: this.fontItalic });
    };
    Bitmap.prototype.measureTextWidth = function(text) { return String(text).length * 14; };
    function Window_Base() { this.visible = true; this.openness = 255; this.contents = new Bitmap(); }
    Window_Base.prototype.resetFontSettings = function() {
        this.contents.fontSize = 28; this.contents.textColor = 'color0'; this.contents.fontBold = false; this.contents.fontItalic = false;
    };
    Window_Base.prototype.textWidth = function(text) { return this.contents.measureTextWidth(text); };
    Window_Base.prototype.lineHeight = function() { return 36; };
    Window_Base.prototype.calcTextHeight = function() { return 36; };
    Window_Base.prototype.textColor = function(index) { return 'color' + index; };
    Window_Base.prototype.changeTextColor = function(color) { this.contents.textColor = color; };
    Window_Base.prototype.convertEscapeCharacters = function(text) { return String(text).replace(/\\/g, '\x1b').replace(/<br>/gi, '\n').replace(/<WordWrap>/gi, ''); };
    Window_Base.prototype.processNewLine = function(state) { state.x = state.left; state.y += state.height; state.index++; };
    Window_Base.prototype.processNormalCharacter = function(state) {
        const text = state.text[state.index++]; this.contents.drawText(text, state.x, state.y); state.x += this.textWidth(text);
    };
    Window_Base.prototype.processCharacter = function(state) {
        if (state.text[state.index] === '\n') return this.processNewLine(state);
        if (state.text[state.index] === '\x1b') {
            const match = /^\x1b([A-Za-z]+)(?:\[(\d+)\])?/.exec(state.text.slice(state.index));
            if (!match) { state.index++; return; }
            state.index += match[0].length;
            const code = match[1].toUpperCase();
            if (code === 'C') this.changeTextColor(this.textColor(Number(match[2])));
            if (code === 'FS') this.contents.fontSize = Number(match[2]);
            if (code === 'FB') this.contents.fontBold = !this.contents.fontBold;
            if (code === 'FI') this.contents.fontItalic = !this.contents.fontItalic;
            return;
        }
        this.processNormalCharacter(state);
    };
    Window_Base.prototype.drawTextEx = function(text, x, y) {
        const state = { index: 0, text: this.convertEscapeCharacters(text), x, y, left: x, height: 36 };
        this.resetFontSettings();
        while (state.index < state.text.length) this.processCharacter(state);
        return state.x - x;
    };
    Window_Base.prototype.drawText = function(text, x, y) { this.contents.drawText(text, x, y); };
    Window_Base.prototype.drawTextAutoWrap = function(text, x, y) { this.drawTextEx(text, x, y); return 1; };
    if (buffered) {
        // MZ 缓冲连续字符后再提交绘制，没有 MV 的 processNormalCharacter。
        const escapeParser = Window_Base.prototype.processCharacter;
        delete Window_Base.prototype.processNormalCharacter;
        Window_Base.prototype.createTextState = function(text, x, y, width) {
            return { index: 0, text: this.convertEscapeCharacters(text), x, y, startX: x, startY: y, width,
                height: 36, buffer: '', drawing: true, outputWidth: 0, outputHeight: 0 };
        };
        Window_Base.prototype.flushTextState = function(state) {
            if (!state.buffer) return;
            if (state.drawing) this.contents.drawText(state.buffer, state.x, state.y);
            state.x += this.textWidth(state.buffer); state.buffer = '';
            state.outputWidth = Math.max(state.outputWidth, Math.abs(state.x - state.startX));
            state.outputHeight = state.y - state.startY + state.height;
        };
        Window_Base.prototype.processNewLine = function(state) { state.x = state.startX; state.y += state.height; };
        Window_Base.prototype.processCharacter = function(state) {
            const c = state.text[state.index++];
            if (c === '\n') { this.flushTextState(state); this.processNewLine(state); }
            else if (c === '\x1b') { this.flushTextState(state); state.index--; escapeParser.call(this, state); }
            else state.buffer += c;
        };
        Window_Base.prototype.processAllText = function(state) {
            while (state.index < state.text.length) this.processCharacter(state);
            this.flushTextState(state);
        };
    }
    function Window_Record() { Window_Base.call(this); this.source = ''; this.origin = { y: 0 }; this.refreshes = 0; }
    Window_Record.prototype = Object.create(Window_Base.prototype);
    Window_Record.prototype.constructor = Window_Record;
    // 模拟任务/图鉴插件自有的整段入口：它不调用 drawTextEx 或 drawText。
    Window_Record.prototype.drawRecord = function(text) {
        const state = { index: 0, text: this.convertEscapeCharacters(text), x: 0, y: 0, left: 0, height: 36 };
        this.resetFontSettings();
        while (state.index < state.text.length) this.processCharacter(state);
        this.textHeight = state.y + state.height;
    };
    Window_Record.prototype.refresh = function() { this.refreshes++; this.contents.drawn = []; this.drawRecord(this.source); };
    function Window_Copied() { Window_Base.call(this); }
    Window_Copied.prototype = Object.create(Window_Base.prototype);
    Window_Copied.prototype.drawTextEx = Window_Base.prototype.drawTextEx;
    const SceneManager = { _scene: { children: [] } };
    const document = { head: { appendChild() {} }, createElement() { return {}; }, getElementsByTagName() { return []; },
        addEventListener(name, cb) { (listeners[name] ||= []).push(cb); }, fonts: { load() { return Promise.resolve([{}]); } } };
    const context = { Bitmap, Window_Base, Window_Record, Window_Copied, SceneManager, document, setTimeout, XMLHttpRequest,
        Date: Clock, console: { error(text) { errors.push(text); }, info() {} }, addEventListener() {} };
    context.window = context; vm.createContext(context);
    vm.runInContext(fs.readFileSync(hookPath, 'utf8'), context, { filename: hookPath });
    const record = new Window_Record(); SceneManager._scene.children = [record];
    return { context, record, advance, requests, hits, errors, listeners,
        text() { return record.contents.drawn.map(x => x.text).join(''); } };
}

const cases = {
    custom_rich_renderer_translates_independent_sections() {
        const f = fixture();
        f.record.source = '\\c[4]From:\\c[0] Operator\n\\c[4]Description:\\c[0]\nFind the silver compass.';
        f.hits['From:'] = '来自：'; f.hits['Description:'] = '说明：';
        f.hits['Find the silver compass.'] = '找到银色指南针。';
        f.record.refresh(); f.advance(50);
        assert.ok(f.text().includes('来自：'), 'plugin-owned character renderer bypassed translation');
        assert.ok(f.text().includes('说明：'), 'one missing name blocked an independent translated section');
        assert.ok(f.text().includes('找到银色指南针。'));
        assert.ok(f.text().includes('Operator'), 'unresolved name was replaced with a fabricated translation');
        const colored = f.record.contents.drawn.filter(x => '来自：说明：'.includes(x.text));
        assert.ok(colored.every(x => x.color === 'color4'), 'section colors were lost');
        assert.ok(f.record.contents.drawn.filter(x => x.text === 'O').every(x => x.color === 'color0'));
        assert.ok(!f.requests.some(r => JSON.stringify(r.payload).includes('来自')));
        assert.deepStrictEqual(f.errors, []);
    },
    partial_hits_keep_current_style_across_refreshes() {
        const f = fixture(); f.hits['Colored entry.'] = '彩色条目。';
        f.record.source = '\\C[2]Colored entry.\\C[0]'; f.record.refresh(); f.advance(50);
        const count = f.requests.length;
        for (let frame = 0; frame < 60; frame++) {
            const color = 1 + frame % 6;
            f.record.source = '\\c[' + color + ']Colored entry.\\c[0]'; f.record.refresh();
            assert.strictEqual(f.text(), '彩色条目。');
            assert.ok(f.record.contents.drawn.every(x => x.color === 'color' + color), 'old or normal color replaced the current style');
        }
        assert.strictEqual(f.requests.length, count, 'style-only updates generated translation requests');
    },
    cjk_auto_wrap_uses_engine_style_parser() {
        const f = fixture(); f.hits['Styled entry.'] = '保留样式的条目。';
        f.record.source = 'Styled entry.'; f.record.refresh(); f.advance(50);
        f.record.contents.drawn = [];
        const lines = f.record.drawTextAutoWrap('\\FB\\FI\\c[3]\\FS[22]Styled entry.\\FI\\FB\\c[0]', 0, 0, 70);
        assert.strictEqual(f.text(), f.hits['Styled entry.'], 'style commands leaked into visible text');
        assert.ok(f.record.contents.drawn.every(x => x.color === 'color3' && x.size === 22 && x.bold && x.italic), 'auto-wrap discarded engine style controls');
        assert.ok(lines > 1, 'CJK text failed to wrap');
        assert.strictEqual(f.record.contents.textColor, 'color0');
        assert.strictEqual(f.record.contents.fontBold, false);
        assert.strictEqual(f.record.contents.fontItalic, false);
    },
    mz_buffered_auto_wrap_keeps_style_and_bounds() {
        const f = fixture(true), source = 'A buffered panel entry.';
        f.hits[source] = '连续缓冲绘制也要保留颜色并正确换行。';
        f.record.drawText(source, 0, 0); f.advance(50); f.record.contents.drawn = [];
        const lines = f.record.drawTextAutoWrap('\\c[3]' + source + '\\c[0]', 0, 0, 70);
        assert.strictEqual(f.text(), f.hits[source], 'MZ flush lost or duplicated translated characters');
        assert.ok(lines > 1, 'MZ buffered CJK text failed to wrap');
        assert.ok(f.record.contents.drawn.every(x => x.color === 'color3'), 'MZ changed the current color');
        assert.ok(f.record.contents.drawn.every(x => x.x + x.text.length * 14 <= 70), 'MZ buffered line exceeded its width');
        assert.strictEqual(f.record.contents.textColor, 'color0');
        assert.deepStrictEqual(f.errors, []);
    },
    auto_wrap_preserves_plugin_line_height_and_count() {
        for (const buffered of [false, true]) {
            const f = fixture(buffered);
            f.record.techTreeLineHeight = function() { return this.contents.fontSize + 6; };
            const single = f.record.drawTextAutoWrap('紧凑行距', 0, 0, 280);
            assert.strictEqual(single, 1, 'one compact line was counted twice');
            assert.deepStrictEqual([...new Set(f.record.contents.drawn.map(g => g.y))], [0]);
            f.record.contents.drawn = [];
            const wrapped = f.record.drawTextAutoWrap('紧凑行距', 0, 0, 28);
            assert.strictEqual(wrapped, 2, 'auto-wrap returned a height ratio instead of the line count');
            assert.deepStrictEqual([...new Set(f.record.contents.drawn.map(g => g.y))], [0, 34], 'plugin line spacing changed');
            f.record.contents.drawn = [];
            const explicit = f.record.drawTextAutoWrap('首行\n次行', 0, 0, 280);
            assert.strictEqual(explicit, 2);
            assert.deepStrictEqual([...new Set(f.record.contents.drawn.map(g => g.y))], [0, 34]);
            assert.deepStrictEqual(f.errors, []);
        }
    },
    mz_measurement_preserves_unbounded_text_width() {
        const f = fixture(true), source = 'Measure a long translated panel heading.';
        f.hits[source] = '测量长标题时需要完整宽度而不能按照当前窗口的可见宽度提前折行。';
        f.record.drawText(source, 0, 0); f.advance(50); f.record.contents.drawn = [];
        const state = f.record.createTextState(source, 0, 0, 0);
        state.drawing = false;
        f.record.processAllText(state);
        assert.strictEqual(state.outputWidth, f.hits[source].length * 14, 'hidden measurement was wrapped to the window width');
        assert.strictEqual(state.outputHeight, 36);
        assert.deepStrictEqual(f.record.contents.drawn, [], 'measurement drew visible glyphs');
    },
    mz_custom_renderer_respects_offset_and_explicit_width() {
        const f = fixture(true), source = 'An inset panel section.';
        f.hits[source] = '偏移内容应当保留窄列宽度。';
        f.record.drawText(source, 0, 0); f.advance(50); f.record.contents.drawn = [];
        const state = f.record.createTextState(source, 28, 0, 56);
        f.record.processAllText(state);
        assert.strictEqual(f.text(), f.hits[source]);
        assert.ok(f.record.contents.drawn.length > 1, 'custom panel ignored its explicit width');
        assert.ok(f.record.contents.drawn.every(g => g.x === 28 && g.x + g.text.length * 14 <= 84), 'custom panel crossed its inset bounds');
    },
    copied_renderer_suppresses_nested_measurement_translation() {
        const f = fixture(), win = new f.context.Window_Copied();
        f.hits['Visible entry.'] = '可见条目。';
        win.drawTextEx('Visible entry.', 0, 0); f.advance(50);
        win.contents.drawn = [];
        const process = f.context.Window_Base.prototype.processNormalCharacter;
        win.processNormalCharacter = function(state) {
            if (!this._measuring) {
                this._measuring = true;
                try { this.drawTextEx('Hidden measurement fragment.', 0, 10000); }
                finally { this._measuring = false; }
            }
            return process.call(this, state);
        };
        win.drawTextEx('Visible entry.', 0, 0);
        assert.strictEqual(win.contents.drawn.filter(g => g.y === 0).map(g => g.text).join(''), '可见条目。');
        assert.ok(!f.requests.some(r => JSON.stringify(r.payload).includes('Hidden measurement fragment.')), 'copied renderer queued an internal measurement');
    },
    custom_commands_are_not_split_at_font_toggle_prefixes() {
        const f = fixture(); f.record.source = '\\FIRE[3]Burn.';
        f.hits['Burn.'] = '燃烧。';
        f.record.refresh(); f.advance(50);
        assert.strictEqual(f.text(), '燃烧。', 'a custom command prefix swallowed the translatable body');
        assert.ok(!f.requests.some(r => JSON.stringify(r.payload).includes('RE[3]')), 'part of a custom command was sent for translation');
        assert.deepStrictEqual(f.errors, []);
    },
    custom_commands_survive_plain_full_cache_hits() {
        const f = fixture(), source = '\\SHAKE[2]A moving label.';
        f.hits[source] = '丢失指令的整段译文。';
        f.hits['A moving label.'] = '移动标签。';
        f.record.source = source;
        f.record.drawTextEx(source, 0, 0); f.advance(50); f.record.contents.drawn = [];
        let rendered = '';
        const process = f.record.processCharacter;
        f.record.processCharacter = function(state) { rendered = state.text; return process.call(this, state); };
        f.record.drawTextEx(source, 0, 0);
        assert.ok(rendered.includes('\x1bSHAKE[2]'), 'custom renderer command was removed by a plain cache hit');
        assert.strictEqual(f.text(), '移动标签。');
    }
};
const selected = process.argv[2] ? [process.argv[2]] : Object.keys(cases);
for (const name of selected) { assert.ok(cases[name], 'unknown case'); cases[name](); console.log('[PASS] ' + name); }
