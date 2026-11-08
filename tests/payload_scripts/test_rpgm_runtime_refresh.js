// 通过公开的 MV/MZ 窗口方法和异步 HTTP 执行正式钩子。
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const hookPath = path.resolve(__dirname, '../../payloads/RPGMaker/hook_rpgm_mv.js');

function fixture(extra = {}) {
    let now = 0, sequence = 0, online = true, responseDelay = () => 1;
    const timers = [], requests = [], errors = [], hits = Object.create(null);
    function setTimeout(callback, delay) {
        timers.push({ callback, due: now + (Number(delay) || 0), sequence: ++sequence });
        return sequence;
    }
    function advance(ms) {
        const end = now + ms;
        for (let steps = 0; ; steps++) {
            timers.sort((a, b) => a.due - b.due || a.sequence - b.sequence);
            if (!timers.length || timers[0].due > end) break;
            assert.ok(steps < 2000, 'unbounded timer or renderer loop');
            const timer = timers.shift();
            now = timer.due;
            timer.callback();
        }
        now = end;
    }
    class Clock extends Date { static now() { return now; } }
    function XMLHttpRequest() {}
    XMLHttpRequest.prototype.open = function(method, url) { this.url = url; };
    XMLHttpRequest.prototype.setRequestHeader = function() {};
    XMLHttpRequest.prototype.send = function(body) {
        const payload = JSON.parse(body);
        requests.push({ url: this.url, payload, time: now });
        setTimeout(() => {
            this.status = online ? 200 : 0;
            this.readyState = 4;
            let result;
            if (this.url.endsWith('/cache/lookup')) {
                const found = Object.create(null);
                for (const key of payload.texts) if (hits[key]) found[key] = hits[key];
                result = { hits: found };
            } else if (this.url.endsWith('/prefetch')) {
                result = { status: 'queued' };
            } else if (this.url.endsWith('/batch')) {
                result = { results: payload.texts.map(key => hits[key] || key),
                    sources: payload.texts.map(key => hits[key] ? 'cache' : 'queued') };
            } else {
                result = { translated_text: hits[payload.text] || payload.text,
                    source: hits[payload.text] ? 'cache' : 'queued' };
            }
            this.responseText = JSON.stringify(result);
            this.onreadystatechange();
        }, responseDelay(this.url, payload));
    };
    function Window_Base() {
        this.visible = true; this.openness = 255; this.pause = false;
        this.contents = { width: 600, height: 200 }; this.drawn = '';
    }
    Window_Base.prototype.drawText = function(text) { this.drawn = text; };
    Window_Base.prototype.drawTextEx = function(text) { this.drawn = text; };
    Window_Base.prototype.updateOpen = function() {};
    function Bitmap() { this.fontFace = 'GameFont'; this.fontSize = 20; this.drawn = ''; }
    Bitmap.prototype.drawText = function(text) { this.drawn = text; };
    function Menu(source) { Window_Base.call(this); this.source = source; this.refreshes = 0; }
    Menu.prototype = Object.create(Window_Base.prototype);
    Menu.prototype.refresh = function() { this.refreshes++; this.drawText(this.source); };
    function Game_Message() { this._texts = []; }
    Game_Message.prototype.allText = function() { return this._texts.join('\n'); };
    const gameMessage = new Game_Message();
    function Window_Message() { Window_Base.call(this); this.starts = 0; this._waitCount = 0; }
    Window_Message.prototype = Object.create(Window_Base.prototype);
    Window_Message.prototype.startMessage = function() {
        this.starts++; this.drawn = ''; this.pause = false;
        this._textState = { text: gameMessage.allText(), index: 0 };
    };
    Window_Message.prototype.updateMessage = function() {
        if (!this._textState) return;
        this.drawn = this._textState.text;
        this._textState = null; this.pause = true;
    };
    Window_Message.prototype.update = function() {};
    Window_Message.prototype.isAnySubWindowActive = function() { return !!this.choiceActive; };
    const SceneManager = { _scene: { children: [] } };
    const document = { head: { appendChild() {} }, createElement() { return {}; },
        getElementsByTagName() { return []; }, addEventListener() {},
        fonts: { load() { return Promise.resolve([{}]); } } };
    const consoleStub = { error(message) { errors.push(message); }, info() {} };
    const window = { Bitmap, Window_Base, Window_Message, Game_Message, SceneManager,
        $gameMessage: gameMessage, document, console: consoleStub, setTimeout,
        addEventListener() {} };
    const context = { window, document, console: consoleStub, XMLHttpRequest, setTimeout,
        Bitmap, Window_Base, Window_Message, Game_Message, SceneManager, Date: Clock,
        $gameMessage: gameMessage };
    Object.assign(window, extra); Object.assign(context, extra);
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(hookPath, 'utf8'), context, { filename: hookPath });
    return { advance, requests, errors, hits, Bitmap, Menu, Window_Message, gameMessage, SceneManager,
        offline() { online = false; }, online() { online = true; },
        delayResponses(callback) { responseDelay = callback; } };
}

const cases = {
    half_localized_quest_prefetch_does_not_skip_remaining_english() {
        const source = '请 Open the door.', localized = '任务已经完成 HP 100';
        const f = fixture({ $gameSystem: { _quests: { quest: [{ name: source, desc: [localized] }] } } });
        f.advance(10);
        const prefetch = f.requests.filter(request => request.url.endsWith('/prefetch'));
        assert.strictEqual(prefetch.length, 1);
        assert.ok(prefetch[0].payload.texts.includes(source));
        assert.ok(!prefetch[0].payload.texts.includes(localized), 'localized quest metadata was requeued');
        assert.deepStrictEqual(f.errors, []);
    },
    half_localized_bitmap_uses_cold_and_warm_cache() {
        const f = fixture(), bitmap = new f.Bitmap(), source = '请 Open the door.';
        f.hits[source] = '请 打开门。';
        bitmap.drawText(source, 0, 0, 400, 30, 'left');
        assert.ok(f.requests.some(request => request.url.endsWith('/cache/lookup') && request.payload.texts.includes(source)),
            'mixed bitmap never requested the local cache');
        f.advance(10);
        bitmap.drawText(source, 0, 0, 400, 30, 'left');
        assert.strictEqual(bitmap.drawn, f.hits[source]);
        assert.strictEqual(bitmap.fontFace, 'GameFont');
        f.hits['请 Hello John.'] = '请 你好 John。';
        bitmap.drawText('请 Hello John.', 0, 0, 400, 30, 'left');
        f.advance(10);
        bitmap.drawText('请 Hello John.', 0, 0, 400, 30, 'left');
        assert.strictEqual(bitmap.drawn, '请 你好 John。');
        const before = f.requests.length;
        bitmap.drawText(bitmap.drawn, 0, 0, 400, 30, 'left');
        bitmap.drawText('已经翻译完成 HP 100', 0, 0, 400, 30, 'left');
        assert.strictEqual(f.requests.length, before, 'completed Chinese bitmap was requeued');
    },
    adjacent_protected_markers_and_url_punctuation_leave_english_visible() {
        for (const source of ['请 __DS_TOKEN_0__Continue__DS_TOKEN_1__', '请访问 https://example.com，Continue']) {
            const f = fixture(), menu = new f.Menu(source);
            f.hits[source] = source.replace('Continue', '继续');
            f.SceneManager._scene.children = [menu];
            menu.refresh(); f.advance(1500);
            assert.strictEqual(menu.drawn, f.hits[source], 'visible English was swallowed by a protected span');
        }
        const f = fixture(), localized = new f.Menu('请访问 https://example.com/Continue，继续。');
        f.SceneManager._scene.children = [localized];
        localized.refresh(); f.advance(1500);
        assert.strictEqual(f.requests.length, 0, 'English inside a URL became visible work');
    },
    half_localized_menu_completes_and_preserves_chinese() {
        const f = fixture(), source = '请 Open the door.', menu = new f.Menu(source);
        f.SceneManager._scene.children = [menu]; f.hits[source] = '请 打开门。';
        menu.refresh(); f.advance(1500);
        assert.strictEqual(menu.drawn, '请 打开门。');
        menu.source = '请 Hello John.'; f.hits[menu.source] = '请 你好 John。';
        menu.refresh(); f.advance(1500);
        assert.strictEqual(menu.drawn, '请 你好 John。');
        menu.source = menu.drawn;
        const requests = f.requests.length;
        menu.refresh(); f.advance(1500);
        assert.strictEqual(f.requests.length, requests, 'completed result with a Latin name was queued again');
        menu.source = '已经翻译完成 HP 100'; menu.refresh(); f.advance(1500);
        assert.strictEqual(f.requests.length, requests, 'Chinese and acronym text was requeued');
    },
    completed_colored_page_keeps_controls_after_plain_cache_arrives() {
        const f = fixture(), message = new f.Window_Message();
        const lines = ['<WordWrap>\\C[4]A colored greeting.\\C[0]', 'A plain reply.'];
        f.SceneManager._scene.children = [message]; f.gameMessage._texts = lines.slice();
        f.hits[lines[0]] = '<WordWrap>\\C[4]彩色问候。\\C[0]';
        f.hits[lines[1]] = '普通回答。';
        message.startMessage(); f.advance(20); message.updateMessage();
        assert.ok(message.drawn.includes('\\C[4]彩色问候。\\C[0]'), 'initial colored translation missing');
        // 同一页面的无格式整页缓存稍后到达，不能覆盖已显示的颜色和原始等待状态。
        f.hits['A colored greeting.\nA plain reply.'] = '问候和回答的无格式整页译文。';
        f.advance(1500); message.startMessage(); message.updateMessage(); f.advance(20);
        assert.ok(message.drawn.includes('\\C[4]'), 'late plain page cache erased dialogue color');
        assert.ok(message.drawn.includes('\\C[0]'), 'late plain page cache erased color reset');
        assert.strictEqual(message.pause, true);
        assert.deepStrictEqual(Array.from(f.gameMessage._texts), lines);
        assert.deepStrictEqual(f.errors, []);
    },
    builtin_party_and_currency_tokens() {
        for (const [source, translated, expected] of [
            ['Hello \\P[1], welcome back!', '你好，欢迎回来！', null],
            ['Pay 100 \\G to proceed.', '支付100即可继续。', null],
            ['Hello \\P[1], welcome back!', '你好\\P[2]，欢迎回来！', null],
            ['Hello \\P[1], welcome back!', '你好\\P[1]，欢迎回来！', '你好\\P[1]，欢迎回来！'],
            ['Pay 100 \\G to proceed.', '支付100\\G即可继续。', '支付100\\G即可继续。'],
            ['You have \\V[1] coins.', '你拥有金币。', null],
            // 裸方括号在 RPG Maker 是正文，不应用 Ren'Py 嵌套插值恢复规则。
            ['Check [stats[player].name] label.', '检查[属性[角色].名字]标签。', '检查[属性[角色].名字]标签。'],
            ['\\C[2]Welcome\\C[0]!', '\\C[2]欢迎\\C[0]！', '\\C[2]欢迎\\C[0]！']
        ]) {
            const f = fixture(), menu = new f.Menu(source);
            f.SceneManager._scene.children = [menu]; f.hits[source] = translated;
            menu.refresh(); f.advance(200);
            assert.strictEqual(menu.drawn, expected === null ? source : expected, source);
        }
    },
    animated_menu_controls() {
        const f = fixture(), source = 'New Game', menu = new f.Menu(source);
        f.SceneManager._scene.children = [menu]; f.hits[source] = '新游戏';
        menu.refresh(); f.advance(50);
        assert.strictEqual(menu.drawn, '新游戏');
        const requests = f.requests.length;
        for (let frame = 0; frame < 60; frame++) {
            for (const escape of ['\\', '\x1b']) {
                const before = escape + 'C[' + (frame % 8) + ']' + escape + 'FS[' + (20 + frame % 4) + ']';
                const after = escape + 'C[0]';
                menu.drawTextEx(before + source + after, 0, 0, 300);
                assert.strictEqual(menu.drawn, before + '新游戏' + after, 'animation lost cached text or current controls');
            }
        }
        assert.strictEqual(f.requests.length, requests, 'style-only animation queued new translations');
        menu.drawTextEx('Untranslated new choice', 0, 0, 300);
        assert.strictEqual(menu.drawn, 'Untranslated new choice', 'new choice frozen to old translation');
        // 相邻引擎守卫：样式变化后的命中译文不能成为下一条缺失请求的原文。
        f.advance(50);
        const submitted = f.requests.flatMap(r => r.payload.texts || [r.payload.text]).filter(Boolean);
        assert.ok(submitted.includes('Untranslated new choice'), 'new choice miss was not queued');
        assert.ok(!submitted.some(text => text.includes('新游戏')), 'rendered translation leaked into request keys');
        assert.deepStrictEqual(f.errors, []);
    },
    whole_control_cache_is_preserved() {
        const f = fixture(), source = '\\C[2]Hello \\V[1]!', translated = '\\C[2]你好 \\V[1]！';
        f.hits[source] = translated;
        const menu = new f.Menu(source); f.SceneManager._scene.children = [menu];
        // 通过公开窗口/异步缓存路径预热包含控制序列的完整条目。
        menu.refresh(); f.advance(50);
        menu.drawTextEx(source, 0, 0, 300);
        assert.strictEqual(menu.drawn, translated, 'existing whole control-bearing cache entry was lost');
    },
    queued_menu() {
        const f = fixture(), source = 'Continue your adventure.';
        const menu = new f.Menu(source);
        f.SceneManager._scene.children = [menu];
        menu.refresh(); f.advance(20);
        assert.strictEqual(menu.drawn, source);
        assert.ok(f.requests.some(r => r.url.endsWith('/translate')));
        f.hits[source] = '继续你的冒险。'; f.advance(1500);
        assert.strictEqual(menu.drawn, f.hits[source], 'static menu stayed English after queued translation finished');
        const count = f.requests.length; f.advance(3000);
        assert.strictEqual(f.requests.length, count, 'resolved UI continued polling');
        assert.deepStrictEqual(f.errors, []);
    },
    reconnect_menu() {
        const f = fixture(), source = 'Open the settings menu.';
        const menu = new f.Menu(source);
        f.SceneManager._scene.children = [menu]; f.offline();
        menu.refresh(); f.advance(500);
        assert.strictEqual(menu.drawn, source);
        f.online(); f.advance(2500);
        assert.ok(f.requests.some(r => r.url.endsWith('/translate') && r.time > 500),
            'visible menu did not requeue translation after local server reconnected');
        f.hits[source] = '打开设置菜单。'; f.advance(1500);
        assert.strictEqual(menu.drawn, f.hits[source], 'reconnected menu did not redraw');
        assert.ok(f.errors.some(e => e.includes('HTTP status 0')), 'offline failure was hidden');
    },
    server_restart_loses_queued_work() {
        const f = fixture(), menu = new f.Menu('Resume the interrupted adventure.');
        f.SceneManager._scene.children = [menu]; menu.refresh(); f.advance(20);
        assert.strictEqual(f.requests.filter(r => r.url.endsWith('/translate')).length, 1);
        f.offline(); f.advance(1500);
        f.online(); f.advance(2500);
        assert.ok(f.requests.some(r => r.url.endsWith('/translate') && r.time > 1520),
            'server restart lost queued work and the visible menu never resubmitted it');
        f.hits[menu.source] = '继续中断的冒险。'; f.advance(1500);
        assert.strictEqual(menu.drawn, f.hits[menu.source]);
    },
    completed_message() {
        const f = fixture(), source = 'Wait here for my return.';
        const message = new f.Window_Message();
        f.SceneManager._scene.children = [message]; f.gameMessage._texts = [source];
        message.startMessage(); f.advance(20); message.updateMessage();
        assert.strictEqual(message.pause, true);
        assert.strictEqual(message._textState, null);
        f.hits[source] = '在这里等我回来。'; f.advance(1500);
        assert.strictEqual(message.drawn, f.hits[source], 'finished dialogue stopped polling before late translation arrived');
        assert.strictEqual(message.pause, true, 'translation consumed player input pause');
        assert.strictEqual(message._textState, null, 'completed dialogue became active again');
        assert.deepStrictEqual(Array.from(f.gameMessage._texts), [source], 'shared game source was overwritten');
        assert.deepStrictEqual(f.errors, []);
    },
    queued_poll_does_not_redraw_or_reenqueue() {
        const f = fixture(), source = 'Choose your destination.';
        const menu = new f.Menu(source);
        f.SceneManager._scene.children = [menu]; menu.refresh(); f.advance(20);
        const refreshes = menu.refreshes;
        f.advance(5000);
        assert.strictEqual(menu.refreshes, refreshes, 'cache misses repeatedly rebuilt the visible menu');
        assert.strictEqual(f.requests.filter(r => r.url.endsWith('/translate')).length, 1,
            'queued translation was repeatedly submitted');
        f.hits[source] = '选择目的地。'; f.advance(1500);
        assert.strictEqual(menu.drawn, f.hits[source]);
    },
    scene_change_cancels_old_requests() {
        const f = fixture(), source = 'Visit the old town.';
        const oldMenu = new f.Menu(source);
        f.SceneManager._scene.children = [oldMenu]; oldMenu.refresh(); f.advance(1);
        // 此时旧场景的翻译响应仍在传输中。
        const current = new f.Menu('Enter the new town.');
        f.SceneManager._scene = { children: [current] }; current.refresh();
        f.advance(10); f.hits[source] = '去旧城。'; f.advance(2000);
        assert.strictEqual(oldMenu.drawn, source, 'departed scene was repainted');
        assert.strictEqual(current.drawn, current.source, 'old reply overwrote new scene text');
        const oldQueries = f.requests.filter(r => r.url.endsWith('/cache/lookup') && r.payload.texts.includes(source));
        assert.strictEqual(oldQueries.length, 1, 'old scene response installed polling on the new scene');
        f.hits[current.source] = '进入新城。'; f.advance(1500);
        assert.strictEqual(current.drawn, f.hits[current.source]);
    },
    hidden_window_stops_polling() {
        const f = fixture(), menu = new f.Menu('A hidden menu entry.');
        f.SceneManager._scene.children = [menu]; menu.refresh(); f.advance(20);
        menu.visible = false; const count = f.requests.length;
        f.advance(3000);
        assert.strictEqual(f.requests.length, count, 'hidden scene target kept polling');
        assert.deepStrictEqual(f.errors, []);
    },
    reused_window_keeps_latest_source() {
        const f = fixture(), menu = new f.Menu('A description for the old item.');
        f.SceneManager._scene.children = [menu]; menu.refresh(); f.advance(20);
        const old = menu.source;
        menu.source = 'A description for the new item.'; menu.refresh(); f.advance(20);
        f.hits[old] = '旧物品的说明。'; f.advance(1000);
        assert.strictEqual(menu.drawn, menu.source, 'late response restored stale window text');
        f.hits[menu.source] = '新物品的说明。'; f.advance(1500);
        assert.strictEqual(menu.drawn, f.hits[menu.source]);
    },
    next_message_keeps_latest_page() {
        const f = fixture(), message = new f.Window_Message(), old = 'The previous dialogue page.';
        f.SceneManager._scene.children = [message]; f.gameMessage._texts = [old];
        message.startMessage(); f.advance(20); message.updateMessage();
        f.gameMessage._texts = ['The current dialogue page.'];
        message.startMessage(); f.advance(20); message.updateMessage();
        f.hits[old] = '上一页对话。'; f.advance(1000);
        assert.strictEqual(message.drawn, f.gameMessage._texts[0], 'late translation restored a previous page');
        f.hits[f.gameMessage._texts[0]] = '当前页对话。'; f.advance(1500);
        assert.strictEqual(message.drawn, '当前页对话。');
        assert.strictEqual(message.pause, true);
    },
    deadlines_and_retry_cooldown() {
        const f = fixture(), menu = new f.Menu('An unresolved visible entry.');
        f.SceneManager._scene.children = [menu]; f.offline(); menu.refresh(); f.advance(65000);
        assert.strictEqual(menu.drawn, menu.source, 'transport failure became a translation');
        assert.ok(f.errors.some(e => e.includes('visible-ui-cache-poll-timeout')), 'poll deadline had no diagnostic');
        const retries = f.requests.filter(r => r.url.endsWith('/translate'));
        for (let i = 1; i < retries.length; i++) {
            assert.ok(retries[i].time - retries[i - 1].time >= 1000, 'retry bypassed cooldown');
        }
        const count = f.requests.length; f.advance(20000);
        assert.strictEqual(f.requests.length, count, 'unresolved menu exceeded poll deadline');
    },
    late_failure_cannot_restart_expired_poll() {
        const f = fixture(), menu = new f.Menu('A slowly failing local request.');
        f.SceneManager._scene.children = [menu]; f.offline(); menu.refresh(); f.advance(57000);
        f.delayResponses(url => url.endsWith('/translate') ? 8000 : 1);
        f.advance(15000);
        assert.ok(f.errors.some(e => e.includes('visible-ui-cache-poll-timeout')));
        assert.ok(f.requests.some(r => r.url.endsWith('/translate') && r.time > 57000),
            'fixture did not hold an in-flight request across the deadline');
        const count = f.requests.length; f.advance(20000);
        assert.strictEqual(f.requests.length, count, 'late failure restarted an expired poll generation');
        assert.strictEqual(menu.drawn, menu.source);
    },
    faulty_plugin_window_does_not_disable_healthy_ui() {
        const f = fixture(), menu = new f.Menu('The working plugin menu.');
        const scene = { children: [] };
        Object.defineProperty(scene, '_brokenWindow', {
            enumerable: true, get() { throw new Error('plugin-owned window getter failed'); }
        });
        scene._menuWindow = menu; f.SceneManager._scene = scene;
        menu.refresh(); f.advance(20); f.hits[menu.source] = '正常的插件菜单。'; f.advance(1500);
        assert.strictEqual(menu.drawn, f.hits[menu.source], 'one plugin getter disabled polling for healthy windows');
        assert.ok(f.errors.some(e => e.includes('read-visible-ui-scene-window') &&
            e.includes('plugin-owned window getter failed')), 'plugin boundary lost its diagnostic');
    }
};
const selected = process.argv[2] ? [process.argv[2]] : Object.keys(cases);
for (const name of selected) { assert.ok(cases[name], 'unknown case'); cases[name](); console.log('[PASS] ' + name); }
