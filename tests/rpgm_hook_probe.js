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

global.window = global;
global.document = {
  createElement() {
    return { type: "", textContent: "" };
  },
  head: { appendChild() {} },
  documentElement: { appendChild() {} },
  fonts: { load() {} }
};

function Window_Base() {}
Window_Base.prototype.standardFontFace = function() { return "GameFont"; };
Window_Base.prototype.drawText = function(text) {
  this.lastDrawText = text;
  return text;
};
Window_Base.prototype.drawTextEx = function(text) {
  this.lastDrawTextEx = text;
  return text;
};
global.Window_Base = Window_Base;

function Window_Message() {
  Window_Base.call(this);
}
Window_Message.prototype = Object.create(Window_Base.prototype);
Window_Message.prototype.constructor = Window_Message;
Window_Message.prototype.startMessage = function() {
  this._textState = { text: global.$gameMessage.allText() };
};
global.Window_Message = Window_Message;

function Game_System() {}
Game_System.prototype.mainFontFace = function() { return "GameFont"; };
global.Game_System = Game_System;

global.XMLHttpRequest = function() {
  this.status = 0;
  this.responseText = "";
};
let requestCount = 0;
XMLHttpRequest.prototype.open = function() {};
XMLHttpRequest.prototype.setRequestHeader = function() {};
XMLHttpRequest.prototype.send = function(body) {
  requestCount++;
  const request = JSON.parse(body);
  const translated = translations[request.text];
  this.status = 200;
  this.responseText = JSON.stringify({
    translation: translated || request.text,
    translated_text: translated || request.text,
    source: translated ? "cache" : "miss"
  });
};

const hook = fs.readFileSync(process.argv[2], "utf8");
vm.runInThisContext(hook, { filename: process.argv[2] });

const base = new Window_Base();
base.drawText("Quest title", 0, 0, 100);
if (base.lastDrawText !== "任务标题") {
  throw new Error("Window_Base.drawText cache hit was not translated");
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

const beforeMisses = requestCount;
base.drawText("A never cached dynamic line", 0, 0, 100);
base.drawText("A never cached dynamic line", 0, 0, 100);
base.drawText("A never cached dynamic line", 0, 0, 100);
if (requestCount - beforeMisses !== 1) {
  throw new Error("Repeated RPG Maker cache misses must use a short retry cooldown");
}

console.log("rpgm hook probe passed");
