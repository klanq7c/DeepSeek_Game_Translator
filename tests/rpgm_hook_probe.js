"use strict";

const fs = require("fs");
const vm = require("vm");

if (process.argv.length !== 3) {
  console.error("usage: node rpgm_hook_probe.js <hook_rpgm_mv.js>");
  process.exit(2);
}

const translations = Object.create(null);
translations["Quest title"] = "任务标题";
translations["This is my Phone, P is the Hotkey!"] = "这是我的手机，P键是快捷键！";
translations["There is some useful stuff in there."] = "里面有些有用的东西。";
translations["\\C[20]Auntie Daisy\\C[0] <br>"] = "\\C[20]黛西阿姨\\C[0] <br>";

translations["very own Theriari business."] = "CN Theriari business";
translations[[
  "Announced as the new, surefire way of becoming rich and renowned,",
  "thousands of people take up Chichikawa on their offer:",
  "Take out a huge loan, and you get a farm, ready to start your",
  "very own Theriari business."
].join("\n")] = "FULL CN Theriari business block";
translations["Partial cached line."] = "CN partial line";
translations["Male appearance"] = "CN Male appearance";
translations["Female appearance"] = "CN Female appearance";
translations["Items"] = "CN Items";
translations["Weapons"] = "CN Weapons";
translations["Key Items"] = "CN Key Items";
translations["Oh, hello there!"] = "CN hello there";
translations["You must be the new owner of"] = "CN new farm owner";
translations["this farm, right?"] = "CN this farm right";
translations["This is a Teleport Crystal."] = "CN teleport crystal";
translations["If you ever get lost while exploring the wilds,"] = "CN lost wilds";
translations["this will take you back to safety."] = "CN back safety";
translations["A primer of a Lactine. It is Pure Blood and Female."] = "CN primer Lactine";
translations["Oversized fixed box label"] = "\u8d85\u957f\u56fa\u5b9a\u6846\u6807\u7b7e";
translations["Focus"] = "\u4e13\u6ce8";
translations["Focus on heavy tools, physical vigor, and harvesting speed."] =
  "\u4e13\u6ce8\u6c89\u91cd\u5de5\u5177\u7269\u7406\u6d3b\u529b\u548c\u91c7\u96c6\u901f\u5ea6\u3002";
translations["Delayed dynamic line"] = "CN delayed dynamic line";
translations["Delayed custom persistence line"] = "CN custom persistence line";
translations["The Secret Resident"] = "CN secret resident";
translations["After the first day of school 0/11"] = "CN school progress 0/11";
translations["Ms. Emmi"] = "CN Ms. Emmi";
translations["\\C[25]Ms. Megan\\C[0] <br>"] = "\\C[25]\u6885\u6839\u5973\u58eb\\C[0] <br>";
translations["- Peep on Megan Undress <br>"] = "- \u5077\u770b\u6885\u6839\u66f4\u8863 <br>";
translations["<WordWrap>\\C[25]Large Page Cached\\C[0] <br>"] =
  "<WordWrap>\\C[25]\u5927\u9875\u5df2\u7f13\u5b58\\C[0] <br>";
translations["\\c[2]Color Guard\\c[0]"] = "\u989c\u8272\u4fdd\u62a4";
translations["\\C[2]Long colored quest description for a narrow help window.\\C[0]"] =
  "\\C[2]\u8fd9\u662f\u4e00\u6bb5\u9700\u8981\u5728\u72ed\u7a84\u5e2e\u52a9\u7a97\u53e3\u4e2d\u81ea\u52a8\u6362\u884c\u7684\u5f88\u957f\u4efb\u52a1\u8bf4\u660e\u3002\\C[0]";

global.window = global;
const pluginScripts = [];
global.document = {
  createElement() {
    return { type: "", textContent: "" };
  },
  head: { appendChild() {} },
  documentElement: { appendChild() {} },
  fonts: { load() {} },
  getElementsByTagName(name) {
    return name === "script" ? pluginScripts : [];
  }
};
global.PluginManager = {
  loadScript(name) {
    pluginScripts.push({
      src: `js/plugins/${name}`,
      listeners: Object.create(null),
      addEventListener(event, fn) { this.listeners[event] = fn; }
    });
  }
};
const scheduledTimers = [];
global.setTimeout = function(fn) {
  scheduledTimers.push(fn);
  return scheduledTimers.length;
};

function Window_Base() {}
Window_Base.prototype.standardFontFace = function() { return "GameFont"; };
Window_Base.prototype.drawText = function(text, x, y, width, align) {
  this.lastDrawText = text;
  this.drawCalls = this.drawCalls || [];
  this.drawCalls.push({ text, x, y, width, align });
  return text;
};
Window_Base.prototype.drawTextEx = function(text, x, y, width) {
  this.lastDrawTextEx = text;
  const converted = this.convertEscapeCharacters(text);
  const state = {
    text: converted,
    index: 0,
    x: Number(x) || 0,
    y: Number(y) || 0,
    left: Number(x) || 0,
    startX: Number(x) || 0,
    width,
    height: this.lineHeight(),
    controls: [],
    glyphs: []
  };
  while (state.index < state.text.length) {
    const ch = state.text.charAt(state.index);
    if (ch === "\n") {
      this.processNewLine(state);
    } else if (ch.charCodeAt(0) === 27) {
      const control = /^\x1b[A-Za-z]+(?:\[[^\]]*\])?/.exec(state.text.slice(state.index));
      const token = control ? control[0] : ch;
      state.controls.push(token);
      state.index += token.length;
    } else {
      this.processNormalCharacter(state);
    }
  }
  this.lastTextState = state;
  return text;
};
Window_Base.prototype.processNormalCharacter = function(textState) {
  const ch = textState.text.charAt(textState.index++);
  const width = this.textWidth(ch);
  textState.glyphs.push({ ch, x: textState.x, y: textState.y, width });
  textState.x += width;
};
Window_Base.prototype.processNewLine = function(textState) {
  textState.x = textState.startX == null ? textState.left : textState.startX;
  textState.y += textState.height || this.lineHeight();
  textState.index++;
};
Window_Base.prototype.textWidth = function(text) {
  return String(text == null ? "" : text).length * 10;
};
Window_Base.prototype.lineHeight = function() {
  return 36;
};
Window_Base.prototype.techTreeLineHeight = function() {
  return 34;
};
Window_Base.prototype.convertEscapeCharacters = function(text) {
  return String(text == null ? "" : text).replace(/\\/g, "\x1b");
};
Window_Base.prototype.drawTextAutoWrap = function(baseText, x, y, maxWidth) {
  const words = String(baseText == null ? "" : baseText).split(" ");
  let lines = 1;
  let x2 = 0;
  for (const word of words) {
    const token = word + " ";
    const width = this.textWidth(token);
    if (x2 + width >= maxWidth) {
      lines++;
      x2 = 0;
    }
    this.drawText(token, x + x2, y + (lines - 1) * this.techTreeLineHeight(), width, "left");
    x2 += width;
  }
  return lines;
};
global.Window_Base = Window_Base;

function Window_Help() {
  Window_Base.call(this);
  this._text = "";
}
Window_Help.prototype = Object.create(Window_Base.prototype);
Window_Help.prototype.constructor = Window_Help;
Window_Help.prototype.setText = function(text) {
  this._text = String(text == null ? "" : text);
  this.refresh();
};
Window_Help.prototype.setItem = function(item) {
  this.setText(item ? item.description : "");
};
Window_Help.prototype.refresh = function() {
  this.drawTextEx(this._text, 0, 0);
};
global.Window_Help = Window_Help;

function Bitmap() {}
Bitmap.prototype.drawText = function(text) {
  this.lastDrawText = text;
  this.fontSizeAtDraw = this.fontSize;
  return text;
};
Bitmap.prototype.fontSize = 28;
Bitmap.prototype.measureTextWidth = function(text) {
  return String(text == null ? "" : text).length * this.fontSize;
};
global.Bitmap = Bitmap;

function Sprite_HUDTextEx() {
  this.visible = true;
  this.refreshCount = 0;
}
Sprite_HUDTextEx.prototype.drawTextEx = Window_Base.prototype.drawTextEx;
Sprite_HUDTextEx.prototype.convertEscapeCharacters = Window_Base.prototype.convertEscapeCharacters;
Sprite_HUDTextEx.prototype.processNormalCharacter = Window_Base.prototype.processNormalCharacter;
Sprite_HUDTextEx.prototype.processNewLine = Window_Base.prototype.processNewLine;
Sprite_HUDTextEx.prototype.textWidth = Window_Base.prototype.textWidth;
Sprite_HUDTextEx.prototype.lineHeight = Window_Base.prototype.lineHeight;
Sprite_HUDTextEx.prototype.refresh = function() { this.refreshCount++; };
global.HUDManager = { types: { TextEx: { class: Sprite_HUDTextEx } } };

function Window_Message() {
  Window_Base.call(this);
}
Window_Message.prototype = Object.create(Window_Base.prototype);
Window_Message.prototype.constructor = Window_Message;
Window_Message.prototype.startMessage = function() {
  this._textState = { text: global.$gameMessage.allText() };
};
global.Window_Message = Window_Message;

function Game_Message() {
  this._texts = [];
  this._choices = [];
  this._choiceCancelType = 0;
}
Game_Message.prototype.allText = function() {
  return this._texts.join("\n");
};
Game_Message.prototype.choices = function() {
  return this._choices;
};
Game_Message.prototype.setChoices = function(choices, defaultType, cancelType) {
  this._choices = choices;
  this._choiceDefaultType = defaultType;
  this._choiceCancelType = cancelType;
};
Game_Message.prototype.choiceCancelType = function() {
  return this._choiceCancelType;
};
global.Game_Message = Game_Message;

function Window_Command() {
  Window_Base.call(this);
  this._list = [];
}
Window_Command.prototype = Object.create(Window_Base.prototype);
Window_Command.prototype.constructor = Window_Command;
Window_Command.prototype.addCommand = function(name, symbol, enabled, ext) {
  this._list.push({ name, symbol, enabled: enabled !== false, ext });
};
Window_Command.prototype.textPadding = function() { return 0; };
Window_Command.prototype.textWidthEx = function(text) {
  return String(text == null ? "" : text).length;
};
global.Window_Command = Window_Command;

function Window_ChoiceList() {
  Window_Command.call(this);
}
Window_ChoiceList.prototype = Object.create(Window_Command.prototype);
Window_ChoiceList.prototype.constructor = Window_ChoiceList;
Window_ChoiceList.prototype.makeCommandList = function() {
  const choices = global.$gameMessage.choices();
  for (let i = 0; i < choices.length; i++) {
    this.addCommand(choices[i], "choice", true, i);
  }
};
Window_ChoiceList.prototype.maxChoiceWidth = function() {
  let max = 0;
  const choices = global.$gameMessage.choices();
  for (let i = 0; i < choices.length; i++) {
    max = Math.max(max, this.textWidthEx(choices[i]) + this.textPadding() * 2);
  }
  return max;
};
global.Window_ChoiceList = Window_ChoiceList;

global.$dataItems = [];
global.$dataItems[79] = { name: "Teleport Crystal", iconIndex: 52 };
global.$dataWeapons = [];
global.$dataArmors = [];
global.$dataSkills = [];
global.$dataStates = [];

function Game_System() {}
Game_System.prototype.mainFontFace = function() { return "GameFont"; };
global.Game_System = Game_System;

function Scene_File() {
  this.children = [];
}
global.Scene_File = Scene_File;

global.XMLHttpRequest = function() {
  this.status = 0;
  this.responseText = "";
  this.readyState = 0;
};
let requestCount = 0;
const delayedRequests = [];
const messagePageRequests = [];
const singleRequestTexts = [];
const batchRequests = [];
const syncLookupRequests = [];
XMLHttpRequest.prototype.open = function(method, url, async) {
  const localSyncLookup = method === "POST" && async === false && /\/cache\/lookup$/.test(url);
  if ((method !== "POST" || async !== true) && !localSyncLookup) {
    throw new Error("RPG Maker hook HTTP must be asynchronous POST");
  }
  this.readyState = 1;
  this.url = url;
};
XMLHttpRequest.prototype.setRequestHeader = function() {};
function completeRequest(xhr, request) {
  const translated = translations[request.text];
  xhr.status = 200;
  xhr.responseText = JSON.stringify({
    translation: translated || request.text,
    translated_text: translated || request.text,
    source: translated ? "cache" : "miss"
  });
  xhr.readyState = 4;
  if (typeof xhr.onreadystatechange === "function") xhr.onreadystatechange();
  if (typeof xhr.onload === "function") xhr.onload();
}
XMLHttpRequest.prototype.send = function(body) {
  requestCount++;
  const request = JSON.parse(body);
  if (Array.isArray(request.texts) && /\/cache\/lookup$/.test(this.url || "")) {
    syncLookupRequests.push(request.texts.slice());
    const hits = Object.create(null);
    for (const text of request.texts) {
      if (translations[text]) hits[text] = translations[text];
    }
    this.status = 200;
    this.responseText = JSON.stringify({ hits });
    this.readyState = 4;
    return;
  }
  if (Array.isArray(request.texts)) {
    batchRequests.push(request.texts.slice());
    const results = request.texts.map(text => translations[text] || text);
    this.status = 200;
    this.responseText = JSON.stringify({
      results,
      sources: request.texts.map(text => translations[text] ? "api_batch" : "miss")
    });
    this.readyState = 4;
    if (typeof this.onreadystatechange === "function") this.onreadystatechange();
    if (typeof this.onload === "function") this.onload();
    return;
  }
  singleRequestTexts.push(request.text);
  if (request.text === "\\C[25]Ms. Megan\\C[0] <br>" ||
      request.text === "- Peep on Megan Undress <br>") {
    messagePageRequests.push({ xhr: this, request });
    return;
  }
  if (request.text === "Delayed dynamic line" ||
      request.text === "Delayed custom persistence line") {
    delayedRequests.push({ xhr: this, request });
    return;
  }
  completeRequest(this, request);
};

const hook = fs.readFileSync(process.argv[2], "utf8");
const nestedQuestWindow = new Window_Base();
nestedQuestWindow.visible = true;
nestedQuestWindow._dsRpgmTranslationTarget = true;
nestedQuestWindow.refreshCount = 0;
nestedQuestWindow.refresh = function() { this.refreshCount++; };
const copiedHudText = new Sprite_HUDTextEx();
const closedSaveConfirm = new Window_Base();
closedSaveConfirm.visible = true;
closedSaveConfirm.openness = 0;
closedSaveConfirm._dsRpgmTranslationTarget = true;
closedSaveConfirm.refreshCount = 0;
closedSaveConfirm.refresh = function() { this.refreshCount++; };
const closingSaveInfo = new Window_Base();
closingSaveInfo.visible = true;
closingSaveInfo.openness = 255;
closingSaveInfo._closing = true;
closingSaveInfo._dsRpgmTranslationTarget = true;
closingSaveInfo.refreshCount = 0;
closingSaveInfo.refresh = function() { this.refreshCount++; };
const openingQuestWindow = new Window_Base();
openingQuestWindow.visible = true;
openingQuestWindow.openness = 0;
openingQuestWindow._opening = true;
openingQuestWindow._dsRpgmTranslationTarget = true;
openingQuestWindow.refreshCount = 0;
openingQuestWindow.refresh = function() { this.refreshCount++; };
global.SceneManager = {
  _scene: {
    children: [{
      children: [
        nestedQuestWindow,
        copiedHudText,
        closedSaveConfirm,
        closingSaveInfo,
        openingQuestWindow
      ]
    }]
  }
};
global.$gameVariables = { value(id) { return id === 53 ? 0 : 0; } };
const questPrimeObjectives = ["After the first day of school \\c[1]\\v[53]\\c[0]/11"];
for (let i = 0; i < 70; i++) {
  questPrimeObjectives.push(`Quest prime objective ${i}`);
}
global.$gameSystem = {
  _quests: {
    quest: [{
      name() { return "Moo Questline #1 - The Secret Resident"; },
      objectives() { return questPrimeObjectives; },
      desc() { return []; },
      resoTxtArray() { return []; }
    }]
  }
};
vm.runInThisContext(hook, { filename: process.argv[2] });

const convertedHudText = "\x1bC[14]Ms. Emmi\x1bC[0]";
copiedHudText.drawTextEx(convertedHudText, 0, 0);
if (copiedHudText.lastDrawTextEx !== "\x1bC[14]CN Ms. Emmi\x1bC[0]" ||
    copiedHudText._dsRpgmTranslationTarget !== true) {
  throw new Error("RPG Maker must bridge copied drawTextEx renderers and preserve converted color controls");
}
if (syncLookupRequests.length !== 1 || !syncLookupRequests[0].includes("Ms. Emmi")) {
  throw new Error("Copied RPG Maker HUD text must use one local-only cache lookup before its first draw");
}

const base = new Window_Base();
base.drawText("Quest title", 0, 0, 100);
if (base.lastDrawText !== "任务标题") {
  throw new Error("Window_Base.drawText cache hit was not translated");
}
base.drawText("very own Theriari business. ", 0, 0, 100);
if (base.lastDrawText !== "CN Theriari business") {
  throw new Error("RPG Maker hook must normalize trailing whitespace the same way warmup does");
}

const longTextExSource = "\\C[2]Long colored quest description for a narrow help window.\\C[0]";
const longTextExTranslation = translations[longTextExSource];
const longTextExGlyphs = longTextExTranslation.replace(/\\C\[[^\]]*\]/gi, "");
base.drawTextEx(longTextExSource, 0, 0, 80);
if (base.lastDrawTextEx !== longTextExTranslation ||
    !base.lastTextState ||
    base.lastTextState.y <= 0 ||
    base.lastTextState.glyphs.some(glyph => glyph.x + glyph.width > 80) ||
    base.lastTextState.glyphs.map(glyph => glyph.ch).join("") !== longTextExGlyphs ||
    base.lastTextState.controls.join("") !== "\x1bC[2]\x1bC[0]") {
  throw new Error("RPG Maker drawTextEx must wrap long translated CJK inside the supplied width without losing renderer controls");
}
base.contents = { width: 80 };
base.drawTextEx(longTextExSource, 0, 0);
if (!base.lastTextState ||
    base.lastTextState.y <= 0 ||
    base.lastTextState.glyphs.some(glyph => glyph.x + glyph.width > 80)) {
  throw new Error("RPG Maker MV drawTextEx must derive its wrap width from the live contents bitmap");
}
const mzBase = new Window_Base();
mzBase.contents = { width: 80 };
mzBase.processNewLine = function(textState) {
  textState.x = textState.startX == null ? textState.left : textState.startX;
  textState.y += textState.height || this.lineHeight();
};
mzBase.drawTextEx(longTextExSource, 0, 0, 80);
if (!mzBase.lastTextState ||
    mzBase.lastTextState.y <= 0 ||
    mzBase.lastTextState.glyphs.some(glyph => glyph.x + glyph.width > 80) ||
    mzBase.lastTextState.glyphs.map(glyph => glyph.ch).join("") !== longTextExGlyphs) {
  throw new Error("RPG Maker MZ drawTextEx wrapping must not duplicate or skip glyphs when processNewLine preserves the index");
}
const layoutDiagnostics = global.__deepSeekRpgmDiagnostics || [];
if (!layoutDiagnostics.some(record =>
      record.context === "draw-text-ex-auto-wrap" &&
      /width=80/.test(record.detail) &&
      record.detail.indexOf(longTextExTranslation) === -1)) {
  throw new Error("RPG Maker CJK layout fallback must retain width-only diagnostics without recording translated text");
}

const help = new Window_Help();
help.setItem({ description: "A primer of a Lactine. It is Pure Blood and Female." });
if (help._text !== "CN primer Lactine" || help.lastDrawTextEx !== "CN primer Lactine") {
  throw new Error("RPG Maker help/item descriptions should be translated before help windows store text");
}
help.contents = { width: 80 };
help.setItem({ description: longTextExSource });
if (help._text !== longTextExTranslation ||
    !help.lastTextState ||
    help.lastTextState.y <= 0 ||
    help.lastTextState.glyphs.some(glyph => glyph.x + glyph.width > 80) ||
    help.lastTextState.glyphs.map(glyph => glyph.ch).join("") !== longTextExGlyphs) {
  throw new Error("RPG Maker help windows must wrap long CJK that was translated before drawTextEx");
}

const commandWindow = new Window_Command();
commandWindow.addCommand("Items", "item", true);
commandWindow.addCommand("Weapons", "weapon", true);
commandWindow.addCommand("Key Items", "keyItem", true);
if (commandWindow._list[0].name !== "Items" ||
    commandWindow._list[1].name !== "Weapons" ||
    commandWindow._list[2].name !== "Key Items") {
  throw new Error("RPG Maker command internals must preserve original names for plugin image/resource lookups");
}

const bitmap = new Bitmap();
bitmap.drawText("A primer of a Lactine. It is Pure Blood and Female.", 0, 0, 200, 24, "left");
if (bitmap.lastDrawText !== "CN primer Lactine") {
  throw new Error("RPG Maker bitmap-level drawText fallback should translate complete plugin-rendered strings");
}
bitmap.fontSize = 28;
bitmap.drawText("Oversized fixed box label", 0, 0, 80, 24, "center");
if (bitmap.lastDrawText !== "\u8d85\u957f\u56fa\u5b9a\u6846\u6807\u7b7e" ||
    bitmap.fontSizeAtDraw >= 28 ||
    bitmap.fontSize !== 28) {
  throw new Error("RPG Maker bitmap-level drawText fallback should shrink translated CJK text to fixed UI widths and restore font size");
}
const beforeSingleChar = requestCount;
bitmap.drawText("A", 0, 0, 20, 24, "left");
if (bitmap.lastDrawText !== "A" || requestCount !== beforeSingleChar) {
  throw new Error("RPG Maker bitmap-level drawText fallback must skip per-character rendering");
}

base.drawCalls = [];
const wrapLines = base.drawTextAutoWrap(
  "Focus on heavy tools, physical vigor, and harvesting speed.",
  0,
  0,
  80
);
if (wrapLines <= 1 ||
    base.drawCalls.length <= 1 ||
    base.drawCalls.some(call => call.text === "\u4e13\u6ce8\u6c89\u91cd\u5de5\u5177\u7269\u7406\u6d3b\u529b\u548c\u91c7\u96c6\u901f\u5ea6\u3002")) {
  throw new Error("RPG Maker CJK auto-wrap should split translated no-space descriptions across fixed-width UI lines");
}

base.drawCalls = [];
base.drawTextAutoWrap("Focus unavailable whole cache", 0, 0, 160);
if (base.drawCalls.some(call => String(call.text).indexOf("\u4e13\u6ce8") !== -1)) {
  throw new Error("RPG Maker auto-wrap cache misses must not translate split word fragments");
}

PluginManager.loadScript("LateAutoWrapPlugin.js");
Window_Base.prototype.drawTextAutoWrap = function(baseText, x, y, maxWidth) {
  const words = String(baseText == null ? "" : baseText).split(" ");
  let x2 = 0;
  for (const word of words) {
    const token = word + " ";
    this.drawText(token, x + x2, y);
    x2 += this.textWidth(token);
  }
  return 1;
};
const lateAutoWrapScript = pluginScripts[pluginScripts.length - 1];
if (lateAutoWrapScript.listeners.load) lateAutoWrapScript.listeners.load();
const primedQuestTexts = [].concat(...batchRequests);
if (!primedQuestTexts.includes("The Secret Resident") ||
    !primedQuestTexts.includes("After the first day of school 0/11")) {
  throw new Error("RPG Maker must prime current Galv quest display text before the first quest-window draw");
}
if (batchRequests.length < 3 || batchRequests.some(texts => texts.length > 16)) {
  throw new Error("RPG Maker Galv quest prime must use concurrent small batches for faster first-page translation");
}
base.drawText("The Secret Resident", 0, 0, 160);
if (base.lastDrawText !== "CN secret resident") {
  throw new Error("RPG Maker Galv quest prime did not populate the synchronous renderer cache");
}
if (nestedQuestWindow.refreshCount < 1) {
  throw new Error("RPG Maker translation completion must refresh nested scene windows outside _windowLayer");
}
if (copiedHudText.refreshCount < 1) {
  throw new Error("RPG Maker translation completion must redraw copied HUD text targets");
}
if (closedSaveConfirm.refreshCount !== 0 || closingSaveInfo.refreshCount !== 0) {
  throw new Error("RPG Maker translation completion must not refresh closed or closing save windows");
}
if (openingQuestWindow.refreshCount < 1) {
  throw new Error("RPG Maker translation completion must refresh visible windows while they are opening");
}
base.drawCalls = [];
base.drawTextAutoWrap("Focus unavailable whole cache", 0, 0, 160);
if (base.drawCalls.some(call => String(call.text).indexOf("\u4e13\u6ce8") !== -1)) {
  throw new Error("RPG Maker auto-wrap hook should reinstall after plugin overwrites");
}

const originalLines = [
  "This is my Phone, P is the Hotkey!",
  "There is some useful stuff in there.",
  "\\C[20]Auntie Daisy\\C[0] <br>",
  "There is some useful stuff in there.\r"
];
global.$gameMessage = {
  _texts: originalLines.slice(),
  allText() {
    return this._texts.join("\n");
  }
};

const message = new Window_Message();
message.startMessage();
const expected = [
  "这是我的手机，P键是快捷键！",
  "里面有些有用的东西。",
  "\\C[20]黛西阿姨\\C[0] <br>",
  "里面有些有用的东西。"
].join("\n");

if (!message._textState || message._textState.text !== expected) {
  throw new Error("Window_Message.startMessage did not translate complete message lines");
}
if (global.$gameMessage._texts.join("\n") !== originalLines.join("\n")) {
  throw new Error("RPG Maker message source was mutated permanently");
}

const blockLines = [
  "Announced as the new, surefire way of becoming rich and renowned,",
  "thousands of people take up Chichikawa on their offer:",
  "Take out a huge loan, and you get a farm, ready to start your",
  "very own Theriari business. "
];
global.$gameMessage = {
  _texts: blockLines.slice(),
  allText() {
    return this._texts.join("\n");
  }
};
const blockMessage = new Window_Message();
blockMessage.startMessage();
if (!blockMessage._textState || blockMessage._textState.text !== "FULL CN Theriari business block") {
  throw new Error("Window_Message.startMessage should prefer complete RPG Maker message block translations");
}
if (global.$gameMessage._texts.join("\n") !== blockLines.join("\n")) {
  throw new Error("RPG Maker block translation mutated message source permanently");
}

const partialLines = ["Partial cached line.", "Partial uncached line."];
global.$gameMessage = {
  _texts: partialLines.slice(),
  allText() {
    return this._texts.join("\n");
  }
};
const partialMessage = new Window_Message();
partialMessage.startMessage();
if (!partialMessage._textState || partialMessage._textState.text !== partialLines.join("\n")) {
  throw new Error("Window_Message.startMessage must not render mixed translated/original message blocks");
}

const layoutBlockLines = [
  "\\C[25]Ms. Megan\\C[0] <br>",
  " <br>",
  "- Peep on Megan Undress <br>"
];
const layoutBlockMessage = new Game_Message();
layoutBlockMessage._texts = layoutBlockLines.slice();
global.$gameMessage = layoutBlockMessage;
if (layoutBlockMessage.allText() !== [
  "\\C[25]\u6885\u6839\u5973\u58eb\\C[0] <br>",
  " <br>",
  "- \u5077\u770b\u6885\u6839\u66f4\u8863 <br>"
].join("\n")) {
  throw new Error("RPG Maker layout-only <br> rows must not force a fully cached message page back to English");
}
if (messagePageRequests.length !== 0 ||
    !syncLookupRequests.some(texts =>
      texts.includes("\\C[25]Ms. Megan\\C[0] <br>") &&
      texts.includes("- Peep on Megan Undress <br>"))) {
  throw new Error("RPG Maker message pages must synchronously read existing local-cache hits before their first draw");
}

const largePageLines = ["<WordWrap>\\C[25]Large Page Cached\\C[0] <br>"];
for (let i = 1; i < 82; i++) largePageLines.push(`<WordWrap>Large uncached row ${i} <br>`);
const largePageMessage = new Game_Message();
largePageMessage._texts = largePageLines.slice();
global.$gameMessage = largePageMessage;
const largePageText = largePageMessage.allText();
if (!largePageText.startsWith("<WordWrap>\\C[25]\u5927\u9875\u5df2\u7f13\u5b58\\C[0] <br>\n") ||
    singleRequestTexts.some(text => typeof text === "string" && text.includes("\n<WordWrap>Large uncached row"))) {
  throw new Error("Large RPG Maker message pages must use per-line cache results without requesting a control-destroying whole-page translation");
}

const colorGuardMessage = new Game_Message();
colorGuardMessage._texts = ["\\c[2]Color Guard\\c[0]"];
global.$gameMessage = colorGuardMessage;
if (colorGuardMessage.allText() !== "\\c[2]Color Guard\\c[0]") {
  throw new Error("RPG Maker must reject cached translations that drop source color controls");
}

const directBlockMessage = new Game_Message();
directBlockMessage._texts = blockLines.slice();
global.$gameMessage = directBlockMessage;
if (directBlockMessage.allText() !== "FULL CN Theriari business block") {
  throw new Error("Game_Message.allText should expose full-block translations to message plugins");
}
if (directBlockMessage._texts.join("\n") !== blockLines.join("\n")) {
  throw new Error("Game_Message.allText must not mutate original RPG Maker message lines");
}

const directPartialMessage = new Game_Message();
directPartialMessage._texts = partialLines.slice();
global.$gameMessage = directPartialMessage;
if (directPartialMessage.allText() !== partialLines.join("\n")) {
  throw new Error("Game_Message.allText must not expose mixed translated/original message blocks");
}

const popMessage = new Game_Message();
popMessage._texts = ["\\pop[14]\\n<Mio>Oh, hello there!"];
global.$gameMessage = popMessage;
if (popMessage.allText() !== "\\pop[14]\\n<Mio>CN hello there") {
  throw new Error("RPG Maker pop/namebox message prefixes must be preserved while translating visible dialogue");
}
if (popMessage._texts[0] !== "\\pop[14]\\n<Mio>Oh, hello there!") {
  throw new Error("RPG Maker prefixed dialogue source line must not be mutated");
}

const popBlockMessage = new Game_Message();
popBlockMessage._texts = ["\\pop[14]\\n<Mio>You must be the new owner of", "this farm, right?"];
global.$gameMessage = popBlockMessage;
if (popBlockMessage.allText() !== "\\pop[14]\\n<Mio>CN new farm owner\nCN this farm right") {
  throw new Error("RPG Maker multi-line pop/namebox dialogue should translate per visible line");
}

const iconNameMessage = new Game_Message();
iconNameMessage._texts = [
  "\\pop[15]\\n<Ariadne>This is a \\c[1]\\ii[79]\\c[0].",
  "If you ever get lost while exploring the wilds,",
  "this will take you back to safety. "
];
global.$gameMessage = iconNameMessage;
if (iconNameMessage.allText() !== "\\pop[15]\\n<Ariadne>CN teleport crystal\nCN lost wilds\nCN back safety") {
  throw new Error("RPG Maker icon-name text codes should translate using their visible item names");
}
if (iconNameMessage._texts.join("\n") !== [
  "\\pop[15]\\n<Ariadne>This is a \\c[1]\\ii[79]\\c[0].",
  "If you ever get lost while exploring the wilds,",
  "this will take you back to safety. "
].join("\n")) {
  throw new Error("RPG Maker icon-name source lines must not be mutated");
}

const choiceMessage = new Game_Message();
const originalChoices = ["Male appearance", "Female appearance"];
choiceMessage.setChoices(originalChoices.slice(), 0, 1);
global.$gameMessage = choiceMessage;
const visibleChoices = choiceMessage.choices();
if (visibleChoices[0] !== "CN Male appearance" ||
    visibleChoices[1] !== "CN Female appearance") {
  throw new Error("Game_Message.choices should return translated display choices");
}
if (choiceMessage._choices.join("\n") !== originalChoices.join("\n")) {
  throw new Error("Game_Message.choices must not mutate original RPG Maker choices");
}
const choiceWindow = new Window_ChoiceList();
choiceWindow.makeCommandList();
if (choiceWindow._list[0].name !== "CN Male appearance" ||
    choiceWindow._list[1].name !== "CN Female appearance") {
  throw new Error("Window_ChoiceList.makeCommandList should translate displayed choice commands");
}
if (choiceWindow.maxChoiceWidth() < "CN Female appearance".length) {
  throw new Error("Window_ChoiceList.maxChoiceWidth should account for translated choice labels");
}
if (choiceMessage._choices.join("\n") !== originalChoices.join("\n")) {
  throw new Error("RPG Maker choice source list must not be mutated by display translation");
}

const beforeMisses = requestCount;
base.drawText("A never cached dynamic line", 0, 0, 100);
base.drawText("A never cached dynamic line", 0, 0, 100);
base.drawText("A never cached dynamic line", 0, 0, 100);
if (requestCount - beforeMisses !== 1) {
  throw new Error("Repeated RPG Maker cache misses must use a short retry cooldown");
}

const beforeDelayed = requestCount;
base.drawText("Delayed dynamic line", 0, 0, 100);
base.drawText("Delayed dynamic line", 0, 0, 100);
if (base.lastDrawText !== "Delayed dynamic line" ||
    requestCount - beforeDelayed !== 1 ||
    delayedRequests.length !== 1) {
  throw new Error("RPG Maker delayed cache lookups must return source immediately and deduplicate in-flight requests");
}
if (delayedRequests[0].request.cache_only === true) {
  throw new Error("Visible RPG Maker cache misses must use the foreground translation queue instead of waiting behind warmup work");
}
const normalScene = SceneManager._scene;
const saveSceneWindow = new Window_Base();
saveSceneWindow.visible = true;
saveSceneWindow.openness = 255;
saveSceneWindow._dsRpgmTranslationTarget = true;
saveSceneWindow.refreshCount = 0;
saveSceneWindow.refresh = function() { this.refreshCount++; };
const saveScene = new Scene_File();
saveScene.children.push(saveSceneWindow);
SceneManager._scene = saveScene;
completeRequest(delayedRequests[0].xhr, delayedRequests[0].request);
for (const fn of scheduledTimers.splice(0)) fn();
if (saveSceneWindow.refreshCount !== 0) {
  throw new Error("RPG Maker translation completion must not refresh persistence scenes");
}
SceneManager._scene = normalScene;
base.drawText("Delayed dynamic line", 0, 0, 100);
if (base.lastDrawText !== "CN delayed dynamic line") {
  throw new Error("RPG Maker asynchronous cache hits must become available to later draws");
}

base.drawText("Delayed custom persistence line", 0, 0, 100);
if (delayedRequests.length !== 2) {
  throw new Error("RPG Maker custom persistence-scene probe did not queue its delayed translation");
}
const customSaveWindow = new Window_Base();
customSaveWindow.visible = true;
customSaveWindow.openness = 255;
customSaveWindow._dsRpgmTranslationTarget = true;
customSaveWindow.refreshCount = 0;
customSaveWindow.refresh = function() { this.refreshCount++; };
SceneManager._scene = {
  children: [customSaveWindow],
  mode() { return "save"; }
};
completeRequest(delayedRequests[1].xhr, delayedRequests[1].request);
for (const fn of scheduledTimers.splice(0)) fn();
if (customSaveWindow.refreshCount !== 0) {
  throw new Error("RPG Maker translation completion must protect plugin-defined persistence scenes");
}
SceneManager._scene = normalScene;

PluginManager.loadScript("LateMessagePlugin.js");
Game_Message.prototype.allText = function() { return this._texts.join("\n"); };
Window_Message.prototype.startMessage = function() {
  this._textState = { text: global.$gameMessage.allText() };
};
const latePluginScript = pluginScripts[pluginScripts.length - 1];
if (latePluginScript.listeners.load) latePluginScript.listeners.load();
for (const fn of scheduledTimers.splice(0)) fn();
const lateMessage = new Game_Message();
lateMessage._texts = ["Quest title"];
global.$gameMessage = lateMessage;
const lateWindow = new Window_Message();
lateWindow.startMessage();
if (!lateWindow._textState || lateWindow._textState.text !== translations["Quest title"] ||
    !Game_Message.prototype.allText._dsRpgmMessageHook ||
    !Window_Message.prototype.startMessage._dsRpgmMessageHook) {
  throw new Error("RPG Maker translation hooks must reinstall after later plugins replace message methods");
}

console.log("rpgm hook probe passed");
