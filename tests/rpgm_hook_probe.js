"use strict";

const fs = require("fs");
const vm = require("vm");

if (process.argv.length !== 3) {
  console.error("usage: node rpgm_hook_probe.js <hook_rpgm_mv.js>");
  process.exit(2);
}

const translations = Object.create(null);
const liveBatchTranslations = Object.create(null);
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
translations["First uncached dialogue."] = "\u9996\u6b21\u5bf9\u8bdd\u76f4\u63a5\u663e\u793a\u4e2d\u6587\u3002";
const queuedLiveDialogueSource = "Queued live dialogue must refresh while it is still visible.";
const queuedLiveDialogueTranslation =
  "\u6392\u961f\u7ffb\u8bd1\u5b8c\u6210\u540e\uff0c\u5f53\u524d\u5bf9\u8bdd\u5fc5\u987b\u81ea\u52a8\u5237\u65b0\u3002";
const delayedLocalDialogueSource = [
  "Delayed local cached dialogue first line.",
  "Delayed local cached dialogue second line."
].join("\n");
translations[delayedLocalDialogueSource] =
  "\u5df2\u6709\u7f13\u5b58\u7684\u591a\u884c\u5bf9\u8bdd\u9996\u6b21\u76f4\u63a5\u663e\u793a\u4e2d\u6587\u3002";
translations["Delayed title cache hit"] = "\u5ef6\u8fdf\u7f13\u5b58\u547d\u4e2d\u6807\u9898";
translations["Delayed opening title cache hit"] = "\u5f00\u542f\u540e\u7f13\u5b58\u6807\u9898";
translations["First draw cached label"] = "\u9996\u5e27\u5df2\u7f13\u5b58\u6807\u7b7e";
translations["First bitmap cached label"] = "\u9996\u5e27\u5df2\u7f13\u5b58\u4f4d\u56fe\u6807\u7b7e";
translations["The hero must return to Casia's house before sunset."] =
  "\u82f1\u96c4\u5fc5\u987b return to Casia's house before sunset.";
translations["Ask Casia to open Inventory."] =
  "\u8bf7 Casia \u6253\u5f00 Inventory\u3002";
const liveOnlyTranslationKeys = new Set([
  "Delayed dynamic line",
  "Delayed custom persistence line",
  "First uncached dialogue."
]);
translations["The Secret Resident"] = "CN secret resident";
translations["After the first day of school 0/11"] = "CN school progress 0/11";
translations["Ms. Emmi"] = "CN Ms. Emmi";
translations["\\C[25]Ms. Megan\\C[0] <br>"] = "\\C[25]\u6885\u6839\u5973\u58eb\\C[0] <br>";
translations["- Peep on Megan Undress <br>"] = "- \u5077\u770b\u6885\u6839\u66f4\u8863 <br>";
translations["<WordWrap>\\C[25]Large Page Cached\\C[0] <br>"] =
  "<WordWrap>\\C[25]\u5927\u9875\u5df2\u7f13\u5b58\\C[0] <br>";
liveBatchTranslations["Large uncached row 1 <br>"] = "\u5927\u9875\u6279\u91cf\u7b2c\u4e00\u884c <br>";
liveBatchTranslations["Large uncached row 2 <br>"] = "\u4e22\u5931\u6362\u884c\u63a7\u5236\u7801";
translations["\\c[2]Color Guard\\c[0]"] = "\u989c\u8272\u4fdd\u62a4";
translations["\\C[3]Unsafe Live\\C[0]"] = "CN unsafe live";
translations["\\C[9]Unsafe Quest\\C[0]"] = "CN unsafe quest";
translations["\\C[2]Long colored quest description for a narrow help window.\\C[0]"] =
  "\\C[2]\u8fd9\u662f\u4e00\u6bb5\u9700\u8981\u5728\u72ed\u7a84\u5e2e\u52a9\u7a97\u53e3\u4e2d\u81ea\u52a8\u6362\u884c\u7684\u5f88\u957f\u4efb\u52a1\u8bf4\u660e\u3002\\C[0]";
const nameInputMessage = [
  "Type in this character's name.",
  "Press \\c[5]ENTER\\c[0] when you're done.",
  "",
  "-or-",
  "",
  "Press \\c[5]arrow keys\\c[0]/\\c[5]TAB\\c[0] to switch",
  "to manual character entry.",
  "",
  "Press \\c[5]ESC\\c[0]/\\c[5]TAB\\c[0] to use to keyboard."
].join("\n");
const translatedNameInputMessage = [
  "\u8bf7\u8f93\u5165\u89d2\u8272\u59d3\u540d\u3002",
  "\u8f93\u5165\u5b8c\u6210\u540e\u6309 \\c[5]ENTER\\c[0]\u3002",
  "",
  "-\u6216\u8005-",
  "",
  "\u6309 \\c[5]\u65b9\u5411\u952e\\c[0]/\\c[5]TAB\\c[0] \u5207\u6362",
  "\u5230\u624b\u52a8\u8f93\u5165\u3002",
  "",
  "\u6309 \\c[5]ESC\\c[0]/\\c[5]TAB\\c[0] \u4f7f\u7528\u952e\u76d8\u3002"
].join("\n");
translations[nameInputMessage] = translatedNameInputMessage;

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
Window_Base.prototype.isOpen = function() {
  return Number(this.openness) >= 255;
};
Window_Base.prototype.open = function() {
  if (!this.isOpen()) this._opening = true;
  this._closing = false;
};
Window_Base.prototype.updateOpen = function() {
  if (this._opening) {
    this.openness = Math.min(255, Number(this.openness || 0) + 32);
    if (this.isOpen()) this._opening = false;
  }
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
const firstDialogueRequests = [];
const queuedDialogueRequests = [];
const delayedLocalLookupRequests = [];
const delayedTitleLookupRequests = [];
const delayedOpeningTitleLookupRequests = [];
const delayedTitleMissLookupRequests = [];
const messagePageRequests = [];
const singleRequestTexts = [];
const batchRequests = [];
const cacheLookupRequests = [];
XMLHttpRequest.prototype.open = function(method, url, async) {
  if (method !== "POST" || async !== true) {
    throw new Error("RPG Maker hook HTTP, including local-cache lookup, must never block the render thread");
  }
  this.readyState = 1;
  this.url = url;
  this.async = async;
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
function completeQueuedRequest(xhr, request) {
  xhr.status = 200;
  xhr.responseText = JSON.stringify({
    translation: request.text,
    translated_text: request.text,
    source: "queued"
  });
  xhr.readyState = 4;
  if (typeof xhr.onreadystatechange === "function") xhr.onreadystatechange();
  if (typeof xhr.onload === "function") xhr.onload();
}
XMLHttpRequest.prototype.send = function(body) {
  requestCount++;
  const request = JSON.parse(body);
  if (Array.isArray(request.texts) && /\/cache\/lookup$/.test(this.url || "")) {
    cacheLookupRequests.push(request.texts.slice());
    if (request.texts.includes(delayedLocalDialogueSource)) {
      delayedLocalLookupRequests.push({ xhr: this, request });
      return;
    }
    if (request.texts.includes("Delayed title cache hit")) {
      delayedTitleLookupRequests.push({ xhr: this, request });
      return;
    }
    if (request.texts.includes("Delayed opening title cache hit")) {
      delayedOpeningTitleLookupRequests.push({ xhr: this, request });
      return;
    }
    if (request.texts.includes("Delayed title cache miss")) {
      delayedTitleMissLookupRequests.push({ xhr: this, request });
      return;
    }
    completeCacheLookup(this, request);
    return;
  }
  if (Array.isArray(request.texts)) {
    batchRequests.push(request.texts.slice());
    const results = request.texts.map(text => liveBatchTranslations[text] || translations[text] || text);
    this.status = 200;
    this.responseText = JSON.stringify({
      results,
      sources: request.texts.map(text =>
        liveBatchTranslations[text] || translations[text] ? "api_batch" : "miss")
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
  if (request.text === "First uncached dialogue.") {
    firstDialogueRequests.push({ xhr: this, request });
    return;
  }
  if (request.text === queuedLiveDialogueSource) {
    queuedDialogueRequests.push({ xhr: this, request });
    return;
  }
  if (request.text === "Delayed dynamic line" ||
      request.text === "Delayed custom persistence line" ||
      request.text === "First draw cached label" ||
      request.text === "First bitmap cached label") {
    delayedRequests.push({ xhr: this, request });
    return;
  }
  completeRequest(this, request);
};
function completeCacheLookup(xhr, request) {
    const hits = Object.create(null);
    for (const text of request.texts) {
      if (translations[text] && !liveOnlyTranslationKeys.has(text)) hits[text] = translations[text];
    }
    xhr.status = 200;
    xhr.responseText = JSON.stringify({ hits });
    xhr.readyState = 4;
    if (typeof xhr.onreadystatechange === "function") xhr.onreadystatechange();
    if (typeof xhr.onload === "function") xhr.onload();
}

const hook = fs.readFileSync(process.argv[2], "utf8");

function expectDynamicMainBootstrap() {
  const scriptElements = [];
  function makeElement(tagName) {
    return {
      tagName: String(tagName || "").toUpperCase(),
      type: "",
      src: "",
      textContent: "",
      listeners: Object.create(null),
      addEventListener(event, fn) {
        (this.listeners[event] || (this.listeners[event] = [])).push(fn);
      }
    };
  }
  const context = {
    console,
    setTimeout(fn) {
      fn();
      return 1;
    },
    document: {
      createElement(name) {
        return makeElement(name);
      },
      head: { appendChild() {} },
      documentElement: { appendChild() {} },
      fonts: { load() {} },
      getElementsByTagName(name) {
        return String(name).toLowerCase() === "script" ? scriptElements : [];
      }
    }
  };
  context.window = context;
  context.XMLHttpRequest = function() {
    this.status = 0;
    this.readyState = 0;
  };
  context.XMLHttpRequest.prototype.open = function(method, url, async) {
    this.url = url;
    this.async = async;
  };
  context.XMLHttpRequest.prototype.setRequestHeader = function() {};
  context.XMLHttpRequest.prototype.send = function(body) {
    const request = JSON.parse(body || "{}");
    const hits = Object.create(null);
    for (const text of request.texts || []) {
      if (text === "Dynamic bootstrap dialogue") hits[text] = "动态启动对话";
    }
    this.status = 200;
    this.readyState = 4;
    this.responseText = JSON.stringify({ hits });
    if (this.async && typeof this.onreadystatechange === "function") this.onreadystatechange();
    if (this.async && typeof this.onload === "function") this.onload();
  };
  vm.createContext(context);
  vm.runInContext(hook, context, { filename: process.argv[2] });

  context.Game_Message = function() {
    this._texts = [];
  };
  context.Game_Message.prototype.allText = function() {
    return this._texts.join("\n");
  };
  const objectsScript = context.document.createElement("script");
  objectsScript.src = "js/rmmz_objects.js";
  scriptElements.push(objectsScript);
  if (!objectsScript.listeners.load || !objectsScript.listeners.load.length) {
    throw new Error("RPG Maker hook must observe core scripts created by a dynamic main.js bootstrap");
  }
  for (const listener of objectsScript.listeners.load) listener.call(objectsScript);

  context.Window_Message = function() {};
  context.Window_Message.prototype.startMessage = function() {
    this.renderedText = context.$gameMessage.allText();
  };
  const windowsScript = context.document.createElement("script");
  windowsScript.src = "js/rmmz_windows.js";
  scriptElements.push(windowsScript);
  for (const listener of windowsScript.listeners.load || []) listener.call(windowsScript);

  const message = new context.Game_Message();
  message._texts = ["Dynamic bootstrap dialogue"];
  context.$gameMessage = message;
  const windowMessage = new context.Window_Message();
  windowMessage.startMessage();
  if (windowMessage.renderedText !== "动态启动对话" ||
      !context.Game_Message.prototype.allText._dsRpgmMessageHook ||
      !context.Window_Message.prototype.startMessage._dsRpgmMessageHook) {
    throw new Error("RPG Maker hooks must install as dynamically bootstrapped engine classes load");
  }
}

expectDynamicMainBootstrap();

function expectNwNodeHttpTransport() {
  const scheduled = [];
  const nodeRequests = [];
  let xhrRequests = 0;
  const context = {
    console,
    Buffer,
    location: { href: "chrome-extension://probe/index.html" },
    process: {
      versions: { nw: "0.95.0" },
      cwd() { return "C:\\rpgm-probe"; }
    },
    setTimeout(fn) {
      scheduled.push(fn);
      return scheduled.length;
    },
    document: {
      createElement() {
        return {
          type: "",
          textContent: "",
          addEventListener() {}
        };
      },
      head: { appendChild() {} },
      documentElement: { appendChild() {} },
      fonts: { load() {} },
      getElementsByTagName() { return []; }
    }
  };
  context.require = function(name) {
    if (name === "fs") return { appendFileSync() {} };
    if (name === "path") return { join() { return "C:\\rpgm-probe\\trace.log"; } };
    if (name !== "http") throw new Error(`unexpected module ${name}`);
    return {
      request(options, onResponse) {
        const requestHandlers = Object.create(null);
        const record = { options, body: "", respond: null };
        const request = {
          setTimeout() {},
          on(event, fn) {
            requestHandlers[event] = fn;
            return request;
          },
          end(body) {
            record.body = String(body || "");
            record.respond = function(status, responseBody) {
              const responseHandlers = Object.create(null);
              const response = {
                statusCode: status,
                setEncoding() {},
                on(event, fn) {
                  responseHandlers[event] = fn;
                  return response;
                }
              };
              onResponse(response);
              if (responseHandlers.data) responseHandlers.data(String(responseBody || ""));
              if (responseHandlers.end) responseHandlers.end();
            };
          },
          destroy(error) {
            if (requestHandlers.error) requestHandlers.error(error || new Error("destroyed"));
          }
        };
        nodeRequests.push(record);
        return request;
      }
    };
  };
  context.XMLHttpRequest = function() {
    this.status = 0;
    this.readyState = 0;
  };
  context.XMLHttpRequest.prototype.open = function() {};
  context.XMLHttpRequest.prototype.setRequestHeader = function() {};
  context.XMLHttpRequest.prototype.send = function() {
    xhrRequests++;
    // Matches the real failing NW.js path: the renderer never receives a
    // completion callback for the localhost browser request.
  };
  context.Scene_Title = function() {
    this.children = [];
  };
  context.Bitmap = function() {
    this.fontSize = 28;
    this.lastDrawText = "";
  };
  context.Bitmap.prototype.measureTextWidth = function(text) {
    return String(text == null ? "" : text).length * this.fontSize;
  };
  context.Bitmap.prototype.drawText = function(text) {
    this.lastDrawText = String(text == null ? "" : text);
    return text;
  };
  context.window = context;
  const scene = new context.Scene_Title();
  const titleWindow = {
    visible: true,
    openness: 255,
    refreshCount: 0,
    contents: new context.Bitmap(),
    refresh() {
      this.refreshCount++;
      this.contents.drawText("NW cached title", 0, 0, 240, 36, "center");
    }
  };
  scene._commandWindow = titleWindow;
  context.SceneManager = { _scene: scene };

  vm.createContext(context);
  vm.runInContext(hook, context, { filename: process.argv[2] });
  titleWindow.refresh();
  if (nodeRequests.length !== 1 || xhrRequests !== 0) {
    throw new Error(
      "NW.js RPG Maker cache lookup must bypass a hanging browser XHR through the embedded Node HTTP transport"
    );
  }
  const requestBody = JSON.parse(nodeRequests[0].body);
  if (!Array.isArray(requestBody.texts) ||
      !requestBody.texts.includes("NW cached title")) {
    throw new Error("NW.js Node transport did not preserve the local cache lookup payload");
  }
  nodeRequests[0].respond(
    200,
    JSON.stringify({ hits: { "NW cached title": "\u5df2\u7f13\u5b58\u6807\u9898" } })
  );
  while (scheduled.length) scheduled.shift()();
  if (titleWindow.refreshCount < 2 ||
      titleWindow.contents.lastDrawText !== "\u5df2\u7f13\u5b58\u6807\u9898") {
    throw new Error("NW.js Node cache completion must refresh the live title window");
  }
}

expectNwNodeHttpTransport();

const preexistingPluginScript = {
  src: "js/plugins/YEP_MessageCore.js",
  listeners: Object.create(null),
  addEventListener(event, fn) { this.listeners[event] = fn; }
};
pluginScripts.push(preexistingPluginScript);
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
questPrimeObjectives.push("\\C[9]Unsafe Quest\\C[0]");
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

// RPG Maker MV's plugins.js creates every plugin <script> before the
// translator tag runs. A still-loading message plugin can therefore replace
// both methods after the translator's initial install.
Game_Message.prototype.allText = function() { return this._texts.join("\n"); };
Window_Message.prototype.startMessage = function() {
  this._textState = { text: global.$gameMessage.allText() };
};
if (typeof preexistingPluginScript.listeners.load !== "function") {
  throw new Error("RPG Maker hook must observe plugin scripts created before its own tag");
}
preexistingPluginScript.listeners.load();
const preexistingMessage = new Game_Message();
preexistingMessage._texts = ["Quest title"];
global.$gameMessage = preexistingMessage;
const preexistingWindow = new Window_Message();
preexistingWindow.startMessage();
if (!preexistingWindow._textState ||
    preexistingWindow._textState.text !== translations["Quest title"] ||
    !Game_Message.prototype.allText._dsRpgmMessageHook ||
    !Window_Message.prototype.startMessage._dsRpgmMessageHook) {
  throw new Error("RPG Maker hooks must reinstall when an already-created plugin script finishes loading");
}

const convertedHudText = "\x1bC[14]Ms. Emmi\x1bC[0]";
const copiedHudLookupCount = cacheLookupRequests.length;
copiedHudText.drawTextEx(convertedHudText, 0, 0);
if (copiedHudText.lastDrawTextEx !== "\x1bC[14]CN Ms. Emmi\x1bC[0]" ||
    copiedHudText._dsRpgmTranslationTarget !== true) {
  throw new Error("RPG Maker must bridge copied drawTextEx renderers and preserve converted color controls");
}
if (cacheLookupRequests.length !== copiedHudLookupCount + 1 ||
    !cacheLookupRequests[copiedHudLookupCount].includes("Ms. Emmi")) {
  throw new Error("Copied RPG Maker HUD text must use one asynchronous local-only cache lookup without blocking the render thread");
}

const base = new Window_Base();
const firstDrawLookupStart = cacheLookupRequests.length;
const firstDrawSingleStart = singleRequestTexts.length;
base.drawText("First draw cached label", 0, 0, 200);
if (base.lastDrawText !== "\u9996\u5e27\u5df2\u7f13\u5b58\u6807\u7b7e" ||
    cacheLookupRequests.length !== firstDrawLookupStart + 1 ||
    !cacheLookupRequests[firstDrawLookupStart].includes("First draw cached label") ||
    singleRequestTexts.length !== firstDrawSingleStart) {
  throw new Error("Window_Base.drawText must use an existing local-cache hit on its first draw without flashing English");
}
const firstBitmapLookupStart = cacheLookupRequests.length;
const firstBitmapSingleStart = singleRequestTexts.length;
const firstBitmap = new Bitmap();
firstBitmap.drawText("First bitmap cached label", 0, 0, 240, 36, "left");
if (firstBitmap.lastDrawText !== "\u9996\u5e27\u5df2\u7f13\u5b58\u4f4d\u56fe\u6807\u7b7e" ||
    cacheLookupRequests.length !== firstBitmapLookupStart + 1 ||
    !cacheLookupRequests[firstBitmapLookupStart].includes("First bitmap cached label") ||
    singleRequestTexts.length !== firstBitmapSingleStart) {
  throw new Error("Bitmap.drawText must use an existing local-cache hit on its first draw without flashing English");
}
base.drawText("Quest title", 0, 0, 100);
if (base.lastDrawText !== "任务标题") {
  throw new Error("Window_Base.drawText cache hit was not translated");
}
base.drawText("very own Theriari business. ", 0, 0, 100);
if (base.lastDrawText !== "CN Theriari business") {
  throw new Error("RPG Maker hook must normalize trailing whitespace the same way warmup does");
}
base.drawTextEx(nameInputMessage, 0, 0, 760);
if (base.lastDrawTextEx !== translatedNameInputMessage) {
  throw new Error("VisuMZ name-input instructions must translate without dropping color controls");
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
const unsafeQuestSingleStart = singleRequestTexts.length;
base.drawText("\\C[9]Unsafe Quest\\C[0]", 0, 0, 160);
if (base.lastDrawText !== "\\C[9]Unsafe Quest\\C[0]" ||
    !singleRequestTexts.slice(unsafeQuestSingleStart).includes("\\C[9]Unsafe Quest\\C[0]")) {
  throw new Error("RPG Maker quest prime must not retain translations that drop renderer controls");
}
if (nestedQuestWindow.refreshCount !== 0 || copiedHudText.refreshCount !== 0) {
  throw new Error("RPG Maker batch completions should coalesce scene refreshes onto the next tick");
}
for (const fn of scheduledTimers.splice(0)) fn();
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

const firstDialogueSource = "First uncached dialogue.";
const firstDialogueMessage = new Game_Message();
firstDialogueMessage._texts = [firstDialogueSource];
global.$gameMessage = firstDialogueMessage;
const firstDialogueWindow = new Window_Message();
firstDialogueWindow.visible = true;
firstDialogueWindow.openness = 255;
SceneManager._scene.children[0].children.push(firstDialogueWindow);
firstDialogueWindow.startMessage();
if (!firstDialogueWindow._textState ||
    firstDialogueWindow._textState.text !== firstDialogueSource ||
    firstDialogueRequests.length !== 1) {
  throw new Error("Window_Message.startMessage must remain responsive and show source while a true remote translation is in flight");
}
completeRequest(firstDialogueRequests[0].xhr, firstDialogueRequests[0].request);
for (const fn of scheduledTimers.splice(0)) fn();
if (!firstDialogueWindow._textState ||
    firstDialogueWindow._textState.text !== "\u9996\u6b21\u5bf9\u8bdd\u76f4\u63a5\u663e\u793a\u4e2d\u6587\u3002" ||
    firstDialogueMessage._texts[0] !== firstDialogueSource) {
  throw new Error("An active Window_Message must update itself when its asynchronous translation becomes ready");
}

const queuedDialogueMessage = new Game_Message();
queuedDialogueMessage._texts = [queuedLiveDialogueSource];
global.$gameMessage = queuedDialogueMessage;
const queuedDialogueWindow = new Window_Message();
queuedDialogueWindow.visible = true;
queuedDialogueWindow.openness = 255;
SceneManager._scene.children[0].children.push(queuedDialogueWindow);
queuedDialogueWindow.startMessage();
for (const fn of scheduledTimers.splice(0)) fn();
if (!queuedDialogueWindow._textState ||
    queuedDialogueWindow._textState.text !== queuedLiveDialogueSource ||
    queuedDialogueRequests.length !== 1) {
  throw new Error("The queued-live dialogue probe must begin as one active source page");
}
completeQueuedRequest(queuedDialogueRequests[0].xhr, queuedDialogueRequests[0].request);
translations[queuedLiveDialogueSource] = queuedLiveDialogueTranslation;
for (let pass = 0; pass < 12 && scheduledTimers.length; pass++) {
  for (const fn of scheduledTimers.splice(0)) fn();
}
if (!queuedDialogueWindow._textState ||
    queuedDialogueWindow._textState.text !== queuedLiveDialogueTranslation ||
    queuedDialogueMessage._texts[0] !== queuedLiveDialogueSource) {
  throw new Error("A queued server result must be discovered from local cache and refresh the still-visible source message");
}

const delayedLocalDialogueMessage = new Game_Message();
delayedLocalDialogueMessage._texts = delayedLocalDialogueSource.split("\n");
global.$gameMessage = delayedLocalDialogueMessage;
const delayedLocalDialogueWindow = new Window_Message();
delayedLocalDialogueWindow.visible = true;
delayedLocalDialogueWindow.openness = 255;
SceneManager._scene.children[0].children.push(delayedLocalDialogueWindow);
const delayedLocalSingleStart = singleRequestTexts.length;
delayedLocalDialogueWindow.startMessage();
if (delayedLocalDialogueWindow._textState ||
    delayedLocalDialogueWindow._waitCount !== 1 ||
    delayedLocalLookupRequests.length !== 1 ||
    !delayedLocalLookupRequests[0].request.texts.includes(delayedLocalDialogueSource) ||
    singleRequestTexts.length !== delayedLocalSingleStart) {
  throw new Error("A known-cache multiline dialogue must yield MZ's synchronous message loop while its local lookup is pending");
}
completeCacheLookup(delayedLocalLookupRequests[0].xhr, delayedLocalLookupRequests[0].request);
for (const fn of scheduledTimers.splice(0)) fn();
if (!delayedLocalDialogueWindow._textState ||
    delayedLocalDialogueWindow._textState.text !== "\u5df2\u6709\u7f13\u5b58\u7684\u591a\u884c\u5bf9\u8bdd\u9996\u6b21\u76f4\u63a5\u663e\u793a\u4e2d\u6587\u3002") {
  throw new Error("A deferred first message must resume itself after its nonblocking local-cache lookup completes");
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
    !cacheLookupRequests.some(texts =>
      texts.includes("\\C[25]Ms. Megan\\C[0] <br>") &&
      texts.includes("- Peep on Megan Undress <br>"))) {
  throw new Error("RPG Maker message pages must asynchronously read existing local-cache hits before their first draw");
}

const largePageLines = ["<WordWrap>\\C[25]Large Page Cached\\C[0] <br>"];
for (let i = 1; i < 82; i++) largePageLines.push(`<WordWrap>Large uncached row ${i} <br>`);
const largePageBatchStart = batchRequests.length;
const largePageSingleStart = singleRequestTexts.length;
const largePageMessage = new Game_Message();
largePageMessage._texts = largePageLines.slice();
global.$gameMessage = largePageMessage;
const largePageText = largePageMessage.allText();
if (largePageText !== largePageLines.join("\n") ||
    singleRequestTexts.some(text => typeof text === "string" && text.includes("\n<WordWrap>Large uncached row"))) {
  throw new Error("Large RPG Maker message pages must stay language-atomic until every visible line is translated");
}
const largePageBatches = batchRequests.slice(largePageBatchStart);
const largePageSingles = singleRequestTexts.slice(largePageSingleStart);
if (largePageBatches.length < 2 ||
    largePageBatches.length > 4 ||
    largePageBatches.some(texts => texts.length > 32) ||
    !largePageBatches.some(texts => texts.includes("Large uncached row 1 <br>")) ||
    largePageSingles.some(text => typeof text === "string" && text.includes("Large uncached row"))) {
  throw new Error(`Large RPG Maker message pages must prime bounded live batches instead of ${largePageSingles.length} single requests`);
}

const mixedResidueSource = "The hero must return to Casia's house before sunset.";
const mixedResidueMessage = new Game_Message();
mixedResidueMessage._texts = [mixedResidueSource];
global.$gameMessage = mixedResidueMessage;
if (mixedResidueMessage.allText() !== mixedResidueSource) {
  throw new Error("RPG Maker must reject CJK translations that retain a copied English source clause");
}
if (!(global.__deepSeekRpgmDiagnostics || []).some(record =>
      record.context === "mixed-language-guard")) {
  throw new Error("RPG Maker mixed-language rejection must retain bounded diagnostic context");
}
const protectedTermsMessage = new Game_Message();
protectedTermsMessage._texts = ["Ask Casia to open Inventory."];
global.$gameMessage = protectedTermsMessage;
if (protectedTermsMessage.allText() !== "\u8bf7 Casia \u6253\u5f00 Inventory\u3002") {
  throw new Error("RPG Maker mixed-language guard must preserve isolated names and game terms");
}

const colorGuardMessage = new Game_Message();
colorGuardMessage._texts = ["\\c[2]Color Guard\\c[0]"];
global.$gameMessage = colorGuardMessage;
const colorGuardSingleStart = singleRequestTexts.length;
if (colorGuardMessage.allText() !== "\\c[2]Color Guard\\c[0]") {
  throw new Error("RPG Maker must reject cached translations that drop source color controls");
}
if (!singleRequestTexts.slice(colorGuardSingleStart).includes("\\c[2]Color Guard\\c[0]")) {
  throw new Error("RPG Maker sync cache lookup must not poison local cache with control-dropping translations");
}
base.drawText("\\c[2]Color Guard\\c[0]", 0, 0, 160);
if (base.lastDrawText !== "\\c[2]Color Guard\\c[0]") {
  throw new Error("RPG Maker sync cache rejection must continue to preserve source during retry cooldown");
}

const unsafeLiveSingleStart = singleRequestTexts.length;
base.drawText("\\C[3]Unsafe Live\\C[0]", 0, 0, 160);
if (base.lastDrawText !== "\\C[3]Unsafe Live\\C[0]" ||
    !singleRequestTexts.slice(unsafeLiveSingleStart).includes("\\C[3]Unsafe Live\\C[0]")) {
  throw new Error("RPG Maker live single requests must reject translations that drop renderer controls before display/cache");
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
const beforeMissLookups = cacheLookupRequests.length;
const beforeMissSingles = singleRequestTexts.length;
base.drawText("A never cached dynamic line", 0, 0, 100);
base.drawText("A never cached dynamic line", 0, 0, 100);
base.drawText("A never cached dynamic line", 0, 0, 100);
if (requestCount - beforeMisses !== 2 ||
    cacheLookupRequests.length !== beforeMissLookups + 1 ||
    singleRequestTexts.length !== beforeMissSingles + 1) {
  throw new Error("Repeated RPG Maker cache misses must use one local lookup, one live request, and a short retry cooldown");
}

const beforeDelayed = requestCount;
const beforeDelayedLookups = cacheLookupRequests.length;
base.drawText("Delayed dynamic line", 0, 0, 100);
base.drawText("Delayed dynamic line", 0, 0, 100);
if (base.lastDrawText !== "Delayed dynamic line" ||
    requestCount - beforeDelayed !== 2 ||
    cacheLookupRequests.length !== beforeDelayedLookups + 1 ||
    delayedRequests.length !== 1) {
  throw new Error("RPG Maker delayed cache lookups must return source immediately and deduplicate in-flight requests");
}
if (delayedRequests[0].request.cache_only === true) {
  throw new Error("Visible RPG Maker cache misses must use the foreground translation queue instead of waiting behind warmup work");
}
const normalScene = SceneManager._scene;
const delayedTitleWindow = new Window_Base();
delayedTitleWindow.visible = true;
delayedTitleWindow.openness = 255;
delayedTitleWindow.contents = new Bitmap();
delayedTitleWindow.refreshCount = 0;
delayedTitleWindow.refresh = function() {
  this.refreshCount++;
  this.contents.drawText("Delayed title cache hit", 0, 0, 240, 36, "center");
};
SceneManager._scene = {
  children: [],
  _commandWindow: delayedTitleWindow
};
delayedTitleWindow.refresh();
if (delayedTitleWindow.contents.lastDrawText !== "Delayed title cache hit" ||
    delayedTitleLookupRequests.length !== 1) {
  throw new Error("The title-window probe must begin with one delayed asynchronous local-cache hit");
}
completeCacheLookup(delayedTitleLookupRequests[0].xhr, delayedTitleLookupRequests[0].request);
for (const fn of scheduledTimers.splice(0)) fn();
if (delayedTitleWindow.refreshCount !== 2 ||
    delayedTitleWindow.contents.lastDrawText !== "\u5ef6\u8fdf\u7f13\u5b58\u547d\u4e2d\u6807\u9898") {
  throw new Error("A cached title command must refresh through its Scene_Title-owned window even when a plugin keeps it outside the normal child walk");
}
SceneManager._scene = normalScene;

const delayedOpeningTitleWindow = new Window_Base();
delayedOpeningTitleWindow.visible = true;
delayedOpeningTitleWindow.openness = 0;
delayedOpeningTitleWindow._opening = false;
delayedOpeningTitleWindow.contents = new Bitmap();
delayedOpeningTitleWindow.refreshCount = 0;
delayedOpeningTitleWindow.refresh = function() {
  this.refreshCount++;
  this.contents.drawText("Delayed opening title cache hit", 0, 0, 240, 36, "center");
};
SceneManager._scene = {
  children: [],
  _commandWindow: delayedOpeningTitleWindow
};
delayedOpeningTitleWindow.refresh();
if (delayedOpeningTitleLookupRequests.length !== 1) {
  throw new Error("The closed title-window probe must begin one asynchronous local-cache lookup");
}
completeCacheLookup(
  delayedOpeningTitleLookupRequests[0].xhr,
  delayedOpeningTitleLookupRequests[0].request
);
for (const fn of scheduledTimers.splice(0)) fn();
if (delayedOpeningTitleWindow.refreshCount !== 1) {
  throw new Error("A still-closed title window must not redraw before its engine-owned opening lifecycle");
}
delayedOpeningTitleWindow.open();
for (let frame = 0; frame < 8; frame++) delayedOpeningTitleWindow.updateOpen();
if (delayedOpeningTitleWindow.refreshCount !== 2 ||
    delayedOpeningTitleWindow.contents.lastDrawText !== "\u5f00\u542f\u540e\u7f13\u5b58\u6807\u9898") {
  throw new Error("A cache hit that arrives while a title window is closed must redraw when RPG Maker finishes opening it");
}
SceneManager._scene = normalScene;

const delayedTitleMissWindow = new Window_Base();
delayedTitleMissWindow.visible = true;
delayedTitleMissWindow.openness = 255;
delayedTitleMissWindow.contents = new Bitmap();
delayedTitleMissWindow.refreshCount = 0;
delayedTitleMissWindow.refresh = function() {
  this.refreshCount++;
  this.contents.drawText("Delayed title cache miss", 0, 0, 240, 36, "center");
};
const delayedTitleMissSingleStart = singleRequestTexts.length;
SceneManager._scene = {
  children: [],
  _commandWindow: delayedTitleMissWindow
};
delayedTitleMissWindow.refresh();
if (delayedTitleMissLookupRequests.length !== 1 ||
    singleRequestTexts.length !== delayedTitleMissSingleStart) {
  throw new Error("The title miss probe must wait only for its delayed local-cache lookup");
}
completeCacheLookup(delayedTitleMissLookupRequests[0].xhr, delayedTitleMissLookupRequests[0].request);
for (const fn of scheduledTimers.splice(0)) fn();
if (delayedTitleMissWindow.refreshCount !== 2 ||
    !singleRequestTexts.slice(delayedTitleMissSingleStart).includes("Delayed title cache miss")) {
  throw new Error("A completed local-cache miss must redraw its title window once so the live translation request can start");
}
SceneManager._scene = normalScene;

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
