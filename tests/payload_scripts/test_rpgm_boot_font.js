// 使用正式 MV 钩子测试后加载插件的覆盖行为及 Bitmap 绘制路径。
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const hookPath = path.resolve(__dirname, '../../payloads/RPGMaker/hook_rpgm_mv.js');
const hook = fs.readFileSync(hookPath, 'utf8');
const tail = '})();';
assert.ok(hook.trimEnd().endsWith(tail));
const hookCode = hook.slice(0, hook.lastIndexOf(tail)) + 'window.__dstTestCache=cache;window.__dstTestCacheSafe=cacheSafeRpgmTranslation;window.__dstTestRefreshMessage=refreshActiveRpgmMessage;' + tail;
const source = 'The following game contains mature and sexual content.\\.';
const translated = '以下游戏包含成人及性内容。';
const rendered = translated + '\\.';
const otherSources = [
    'It is only intended for those of legal age that',
    'consent to the consumption of said content.'
];
const otherTranslations = ['仅限达到法定年龄的人士使用', '同意消费所述内容。'];
const listeners = Object.create(null);
const injectedStyles = [];
const document = {
    head: { appendChild(node) { injectedStyles.push(node.textContent || ''); } },
    createElement() { return { style: {}, addEventListener() {} }; },
    getElementsByTagName() { return []; },
    addEventListener(name, callback) { (listeners[name] ||= []).push(callback); },
    fonts: { load() { return Promise.resolve([{}]); } }
};
const errors = [];
const timers = [];
let holdLocalLookup = false;
const consoleStub = { error(message) { errors.push(message); }, info() {} };
function XMLHttpRequest() {}
XMLHttpRequest.prototype.open = function(method, url) { this.url = url; };
XMLHttpRequest.prototype.setRequestHeader = function() {};
XMLHttpRequest.prototype.send = function() {
    if (holdLocalLookup && this.url.includes('/cache/lookup')) return;
    this.status = 200;
    this.readyState = 4;
    this.responseText = this.url.includes('/cache/lookup')
        ? JSON.stringify({ hits: {} })
        : JSON.stringify({ translated_text: 'Original text', source: 'miss' });
    if (this.onreadystatechange) this.onreadystatechange();
};

function Window_Base() {}
Window_Base.prototype.standardFontFace = function() { return 'GameFont, Syncopate-Regular'; };
function Window_Message() { this.visible = true; this.openness = 255; }
Window_Message.prototype = Object.create(Window_Base.prototype);
Window_Message.prototype.standardFontFace = function() { return 'GameFont, Syncopate-Regular'; };
function Game_Message() { this._texts = [source].concat(otherSources); }
Game_Message.prototype.allText = function() { return this._texts.join('\n'); };
const gameMessage = new Game_Message();
Window_Message.prototype.startMessage = function() {
    this._textState = { text: gameMessage.allText() };
    this.visibleText = '';
    return this._textState.text;
};
Window_Message.prototype.updateMessage = function() {
    this.visibleText = this._textState && this._textState.text;
    return true;
};
Window_Message.prototype.update = function() { return true; };
function Bitmap() { this.fontFace = 'GameFont, Syncopate-Regular'; this.fontSize = 28; }
Bitmap.prototype.drawText = function(text) { return { text, face: this.fontFace }; };
Bitmap.prototype.measureTextWidth = function(text) { return { text, face: this.fontFace }; };

const window = { Window_Base, Window_Message, Game_Message, Bitmap,
    $gameMessage: gameMessage, document, console: consoleStub,
    addEventListener(name, callback) { (listeners[name] ||= []).push(callback); },
    setTimeout(callback) { timers.push(callback); } };
const context = { window, document, console: consoleStub, XMLHttpRequest, Window_Base,
    Window_Message, Game_Message, Bitmap, setTimeout: window.setTimeout,
    $gameMessage: gameMessage, Set, Date, Math, JSON, Array, Number, String,
    Object, Error, RegExp };
vm.createContext(context);
vm.runInContext(hookCode, context, { filename: hookPath });
assert.ok(injectedStyles.some(css => css.includes("url('fonts/ds_font.otf') format('opentype')")),
    'an existing user-provided OTF fallback font was not registered');

// 此游戏在静态钩子之后加载 YEP_MessageCore 和 YEP_X_MessageBacklog。
// 后加载的插件可能替换两个消息入口及其字体覆盖逻辑。
Game_Message.prototype.allText = function() { return this._texts.join('\n'); };
Window_Message.prototype.startMessage = function() {
    this._textState = { text: gameMessage.allText() };
    this.visibleText = '';
    return this._textState.text;
};
Window_Message.prototype.update = function() { return true; };
Window_Base.prototype.standardFontFace = function() { return 'GameFont, Syncopate-Regular'; };
Window_Message.prototype.standardFontFace = function() { return 'GameFont, Syncopate-Regular'; };
assert.strictEqual(new Window_Message().startMessage(), [source].concat(otherSources).join('\n'));
for (const callback of listeners.DOMContentLoaded || []) callback();
for (const callback of listeners.load || []) callback();

// 预先写入与游戏原文完全匹配的缓存。后加载插件的 startMessage 此时
// 必须使用已翻译页面，同时保持原始源文本数组不变。
assert.strictEqual(window.__dstTestCacheSafe(source, translated, 'game-cache'), true,
    'trailing RPG Maker wait command was not preserved');
assert.strictEqual(window.__dstTestCacheSafe('A \\C[2] B', '甲乙', 'unsafe-control'), false,
    'a missing style control must not be reconstructed');
assert.strictEqual(window.__dstTestCacheSafe('\\fiThat\'s a long fall.\\fi', '那可真够高的。', 'missing-italic'), false,
    'a missing italic toggle must not be cached as a complete translation');
assert.strictEqual(window.__dstTestCacheSafe('\\fiThat\'s a long fall.\\fi', '\\fi那可真够高的。\\fi', 'italic-preserved'), true,
    'an intact italic toggle must retain the cached translation');
for (let i = 0; i < otherSources.length; i++) {
    assert.strictEqual(window.__dstTestCacheSafe(otherSources[i], otherTranslations[i], 'game-cache'), true);
}
assert.ok(Window_Message.prototype.startMessage._dsRpgmMessageHook,
    'late plugin override was not rehooked');
assert.strictEqual(new Window_Message().startMessage(), [rendered].concat(otherTranslations).join('\n'));
assert.deepStrictEqual(gameMessage._texts, [source].concat(otherSources));
const italicSource = '\\fiThat\'s a long fall.\\fi';
gameMessage._texts = [italicSource];
assert.strictEqual(new Window_Message().startMessage(), '\\fi那可真够高的。\\fi');
assert.deepStrictEqual(gameMessage._texts, [italicSource]);
const pageLines = [
    'My eyes fluttered open as I picked myself up off the',
    'ground.\\. Pain shot from the mid-left of my torso.'
];
const pageTranslation = '我睁开双眼，从地上爬了起来。\n疼痛从我躯干中左侧传来。';
assert.strictEqual(window.__dstTestCacheSafe(pageLines.join('\n'), pageTranslation, 'middle-wait'), true,
    'cached whole-page translation with a missing middle wait did not recover');
assert.strictEqual(window.__dstTestCacheSafe('It hurt\\. badly.', '疼得厉害。', 'single-line-wait'), true,
    'single-line translation should keep a missing wait command');
assert.strictEqual(window.__dstTestCacheSafe(pageLines[1], pageTranslation, 'polluted-line'), false,
    'a one-line key must not accept an entire translated page');
gameMessage._texts = pageLines;
assert.ok(new Window_Message().startMessage().includes('我睁开双眼'));
assert.deepStrictEqual(gameMessage._texts, pageLines);

const uncachedSource = 'A newly encountered uncached line.';
gameMessage._texts = [uncachedSource];
holdLocalLookup = true;
const pendingWindow = new Window_Message();
assert.strictEqual(pendingWindow.startMessage(), uncachedSource,
    'pending local cache lookup must show source immediately');
assert.strictEqual(pendingWindow._textState.text, uncachedSource,
    'pending lookup left the message window blank');
holdLocalLookup = false;

const refreshSource = 'A second fresh message.';
const refreshTarget = '另一条新消息。';
gameMessage._texts = [refreshSource];
const activeWindow = new Window_Message();
activeWindow.startMessage();
activeWindow.updateMessage();
assert.strictEqual(activeWindow.visibleText, refreshSource);
assert.strictEqual(window.__dstTestCacheSafe(refreshSource, refreshTarget, 'late-hit'), true);
activeWindow.pause = true;
window.__dstTestRefreshMessage(activeWindow);
assert.strictEqual(activeWindow.visibleText, refreshSource,
    'refresh cleared a paused message');
activeWindow.pause = false;
activeWindow._waitCount = 3;
activeWindow.update();
assert.strictEqual(activeWindow.visibleText, refreshSource,
    'refresh cleared a message while its wait command was active');
activeWindow._waitCount = 0;
activeWindow.update();
assert.strictEqual(activeWindow.visibleText, refreshTarget,
    'refresh left a blank frame instead of drawing the cached translation');

const completedSource = 'A finished message waiting for input.';
const completedTarget = '等待翻页的已结束消息。';
gameMessage._texts = [completedSource];
const completedWindow = new Window_Message();
completedWindow.startMessage();
completedWindow.updateMessage();
completedWindow._textState = null;
completedWindow.pause = true;
completedWindow.updateMessage = function() {
    this.visibleText = this._textState.text;
    this._textState = null;
    this.pause = true;
    return true;
};
assert.strictEqual(window.__dstTestCacheSafe(completedSource, completedTarget, 'completed-hit'), true);
window.__dstTestRefreshMessage(completedWindow);
assert.strictEqual(completedWindow.visibleText, completedTarget,
    'a completed visible page ignored its newly cached translation');
assert.strictEqual(completedWindow.pause, true,
    'refresh changed the finished page input state');

const waitSource = 'A finished page with a wait\\.';
gameMessage._texts = [waitSource];
const waitWindow = new Window_Message();
waitWindow._textState = null;
waitWindow.pause = true;
waitWindow.visibleText = waitSource;
waitWindow.updateMessage = function() {
    const text = this._textState.text;
    this.visibleText = text.slice(0, text.indexOf('\\.'));
    if (!this._repaintWaitSeen) {
        this._repaintWaitSeen = true;
        this._waitCount = 15;
    } else {
        this._textState = null;
        this.pause = true;
    }
    return true;
};
assert.strictEqual(window.__dstTestCacheSafe(waitSource, '等待结束。', 'completed-wait'), true);
window.__dstTestRefreshMessage(waitWindow);
assert.strictEqual(waitWindow.visibleText, '等待结束。');
assert.strictEqual(waitWindow._textState, null,
    'repainting a completed page left a replayed wait before user input');
assert.strictEqual(waitWindow._waitCount, 0);
assert.strictEqual(waitWindow.pause, true);
assert.strictEqual(new Window_Base().standardFontFace(), 'GameFont, Syncopate-Regular');
assert.strictEqual(new Window_Message().standardFontFace(), 'GameFont, Syncopate-Regular');

const bitmap = new Bitmap();
const cjkDraw = bitmap.drawText(rendered, 0, 0, 300, 36, 'left');
assert.ok(cjkDraw.face.startsWith('DeepSeekCJK,'),
    'Chinese draw did not select the CJK font');
assert.strictEqual(bitmap.fontFace, 'GameFont, Syncopate-Regular',
    'Chinese draw changed the game font for later text');
const latinDraw = bitmap.drawText('Original text', 0, 0, 300, 36, 'left');
assert.strictEqual(latinDraw.face, 'GameFont, Syncopate-Regular');
const cjkWidth = bitmap.measureTextWidth(translated);
assert.ok(cjkWidth.face.startsWith('DeepSeekCJK,'),
    'Chinese layout did not measure with the draw font');
assert.strictEqual(bitmap.fontFace, 'GameFont, Syncopate-Regular');
assert.deepStrictEqual(errors, []);
console.log('[PASS] RPG Maker message pages, pending cache, paused refresh, controls, and font preservation');
