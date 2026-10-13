const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const hookPath = path.resolve(__dirname, '../../payloads/RPGMaker/hook_rpgm_mv.js');
const hook = fs.readFileSync(hookPath, 'utf8');
assert.ok(hook.trimEnd().endsWith('})();'));
const code = hook.slice(0, hook.lastIndexOf('})();')) +
    'window.__dstTestCacheSafe=cacheSafeRpgmTranslation;' +
    'window.__dstTestRefresh=requestWindowRefresh;' + '})();';
const timers = [];
const requests = [];
const errors = [];
const setTimeout = callback => { timers.push(callback); return timers.length; };
function flushTimers() {
    for (let i = 0; i < 20 && timers.length; i++) timers.shift()();
    assert.strictEqual(timers.length, 0, 'unexpected timer loop');
}
function XMLHttpRequest() {}
XMLHttpRequest.prototype.open = function(method, url) { this.url = url; };
XMLHttpRequest.prototype.setRequestHeader = function() {};
XMLHttpRequest.prototype.send = function(body) { this.body = JSON.parse(body); requests.push(this); };
function answerCache(hits) {
    const pending = requests.splice(0);
    for (const request of pending) {
        if (!request.url.endsWith('/cache/lookup')) continue;
        request.status = 200;
        request.readyState = 4;
        request.responseText = JSON.stringify({ hits });
        request.onreadystatechange();
    }
    return pending;
}

function Window_Base() {
    this.visible = true;
    this.openness = 255;
    this.contents = { width: 600, height: 200,
        clearRect: (x, y, width, height) => { this.clearRects.push([x, y, width, height]); this.drawn = []; } };
    this.drawn = [];
    this.clearRects = [];
}
Window_Base.prototype.drawText = function(text) { this.drawn.push(text); return text; };
Window_Base.prototype.updateOpen = function() {};
function Window_Help() { Window_Base.call(this); this._text = ''; this.refreshes = 0; }
Window_Help.prototype = Object.create(Window_Base.prototype);
Window_Help.prototype.setText = function(text) {
    if (this._text !== text) { this._text = text; this.refresh(); }
};
Window_Help.prototype.refresh = function() { this.refreshes++; this.drawn = []; this.drawText(this._text); };
function Window_SaveInfo() {
    Window_Base.call(this);
    this._valid = true;
    this._info = { playtime: '00:05:28' };
    this._saveContents = { system: {} };
    this._currentFile = 1;
    this._waitTime = 0;
    this.diskReads = 0;
}
Window_SaveInfo.prototype = Object.create(Window_Base.prototype);
Window_SaveInfo.prototype.savefileId = function() { return 1; };
Window_SaveInfo.prototype.refresh = function() { this.diskReads++; throw new Error('save file reloaded'); };
Window_SaveInfo.prototype.drawColumnData = function(y) {
    this.drawText('Playtime:');
    this.drawText('Total Saves:');
    this.drawText('Appearance TF Tier');
    return y;
};
function Window_SavefileList() { Window_Base.call(this); this.redraws = 0; }
Window_SavefileList.prototype = Object.create(Window_Base.prototype);
Window_SavefileList.prototype.redrawCurrentItem = function() {
    this.redraws++;
    this.drawn = [];
    this.drawText('File 1');
};
function Scene_File() { this.children = []; this._mode = 'load'; }
Scene_File.prototype.mode = function() { return this._mode; };
const scene = new Scene_File();
const help = new Window_Help();
const info = new Window_SaveInfo();
const list = new Window_SavefileList();
scene._helpWindow = help;
scene._infoWindow = info;
scene._listWindow = list;
const SceneManager = { _scene: scene };
const document = { head: { appendChild() {} }, createElement() { return {}; },
    getElementsByTagName() { return []; }, addEventListener() {},
    fonts: { load() { return Promise.resolve([{}]); } } };
const consoleStub = { error(message) { errors.push(message); }, info() {} };
const window = { Window_Base, Window_Help, Window_SaveInfo, Window_SavefileList,
    Scene_File, SceneManager, document, console: consoleStub, setTimeout,
    addEventListener() {} };
const context = { window, document, console: consoleStub, XMLHttpRequest, setTimeout,
    Window_Base, Window_Help, Window_SaveInfo, Window_SavefileList, Scene_File,
    SceneManager, Set, Date, Math, JSON, Array, Number, String, Object, Error, RegExp };
vm.createContext(context);
vm.runInContext(code, context, { filename: hookPath });

help.setText('Loads the data from the saved game.');
info.drawColumnData(40);
list.redrawCurrentItem();
assert.strictEqual(help.drawn[0], 'Loads the data from the saved game.');
assert.strictEqual(info.drawn[0], 'Playtime:');
assert.strictEqual(list.drawn[0], 'File 1');
const polluted = "I'd be happy to help translate your game text to Simplified Chinese, but I don't see any text provided in your message.";
answerCache({
    'Loads the data from the saved game.': '从保存的游戏加载数据。',
    'Playtime:': '游戏时长：',
    'Total Saves:': '总存档数：',
    'Appearance TF Tier': '外观 TF 等级',
    'File': '文件',
    'File 1': polluted
});
flushTimers();
assert.strictEqual(help.drawn[0], '从保存的游戏加载数据。',
    'save help did not repaint after cache hit');
assert.deepStrictEqual(info.drawn, ['游戏时长：', '总存档数：', '外观 TF 等级'],
    'save details did not repaint from the in-memory snapshot');
assert.strictEqual(info.diskReads, 0, 'translation callback reloaded a save file');
assert.strictEqual(list.drawn[0], '文件 1', 'file slot did not repaint as a short label');
assert.strictEqual(window.__dstTestCacheSafe('File 1', polluted, 'polluted-cache'), false,
    'an unrelated provider reply must not be displayed as a file slot');
list.redrawCurrentItem();
assert.strictEqual(list.drawn[0], '文件 1');

// 新的存档帮助文本首次本地未命中后，必须将 API 工作入队；
// 非阻塞缓存轮询得到最终译文后，再重绘页面。
help.setText('A newly encountered save help line.');
answerCache({});
flushTimers();
const queuedRequest = requests.find(request => request.url.endsWith('/translate') &&
    request.body.text === 'A newly encountered save help line.');
assert.ok(queuedRequest, 'save scene did not queue a translation after cache miss');
requests.splice(requests.indexOf(queuedRequest), 1);
queuedRequest.status = 200;
queuedRequest.readyState = 4;
queuedRequest.responseText = JSON.stringify({
    translated_text: 'A newly encountered save help line.', source: 'queued'
});
queuedRequest.onreadystatechange();
assert.ok(timers.length, 'queued save translation did not start cache polling');
timers.shift()();
answerCache({ 'A newly encountered save help line.': '新遇到的存档说明。' });
flushTimers();
assert.strictEqual(help.drawn[0], '新遇到的存档说明。');
assert.strictEqual(info.diskReads, 0);

// 相邻普通场景继续使用原有重绘路径。
const ordinary = new Window_Base();
let ordinaryRefreshes = 0;
ordinary.refresh = function() { ordinaryRefreshes++; };
ordinary.drawText('An ordinary menu entry.');
SceneManager._scene = { children: [ordinary] };
assert.strictEqual(window.__dstTestCacheSafe('An ordinary menu entry.', '普通菜单项。', 'normal-scene'), true);
window.__dstTestRefresh();
flushTimers();
assert.strictEqual(ordinaryRefreshes, 1, 'normal scene redraw regressed');
assert.deepStrictEqual(errors, []);
console.log('[PASS] RPG Maker save help, save details, slot label and no disk reload');
