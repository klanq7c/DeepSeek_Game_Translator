/* ================================================================
 * deploy.c — 翻译 hook 与插件 payload 部署实现
 * ----------------------------------------------------------------
 * 本文件负责将翻译器 hook 注入到不同引擎的游戏目录中。
 * 包含：
 *   - Ren'Py Python hook（嵌入字符串，运行时写入 .rpy 文件）
 *   - RPG Maker MV/MZ JavaScript hook（嵌入字符串，运行时写入 .js 文件）
 *   - Unity Mono BepInEx 插件部署（DLL 复制 + BepInEx 运行时安装）
 *   - Unity IL2CPP BepInEx be.755 + XUnity AutoTranslator 全套部署
 *   - CJK 字体部署（从系统 Fonts 复制到游戏目录供 hook 使用）
 * ================================================================ */

#include "deploy.h"
#include "fsutil.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ----------------------------------------------------------------
 * RENPY_HOOK — Ren'Py 翻译 Python 脚本（嵌入源码）
 *
 * 功能概述：
 *   1. Hook renpy.exports.say，将对话文本发送到本地 C 服务器翻译
 *   2. 使用 cache_only 模式避免阻塞游戏：命中缓存立即返回，否则返回原文
 *   3. 后台轮询线程（_ds_poll_loop）定期重试未命中的翻译
 *   4. 部署 CJK 字体（ds_font.ttf/otf/ttc）替换所有 style 的 font 属性
 *   5. replace_text hook 翻译 UI 界面文本（菜单、按钮等）
 *
 * 关键设计：
 *   - _ds_memo: 内存缓存，避免重复调用本地服务器
 *   - _ds_pending: 待翻译队列，后台线程批量提交
 *   - _ds_state['down_until']: 熔断机制，服务器不可用时暂停 30 秒
 *   - font_replacement_map: 将原字体映射到 CJK 字体
 * ---------------------------------------------------------------- */
static const char RENPY_HOOK[] =
"init 999 python:\n"
"    import json, os, time, threading\n"
"    try:\n"
"        from urllib.request import Request, urlopen\n"
"    except Exception:\n"
"        from urllib2 import Request, urlopen\n"
"    _ds_old_say = renpy.exports.say\n"
"    _ds_memo = {}\n"
"    _ds_pending = {}\n"
"    _ds_lock = threading.Lock()\n"
"    _ds_state = {'down_until': 0.0, 'poller': False}\n"
"    def _ds_has_cjk(s):\n"
"        try:\n"
"            return any(u'\\u4e00' <= ch <= u'\\u9fff' for ch in s)\n"
"        except Exception:\n"
"            return False\n"
"    def _ds_http(path, payload, timeout):\n"
"        data = json.dumps(payload).encode('utf-8')\n"
"        req = Request('http://127.0.0.1:19999' + path, data=data, headers={'Content-Type':'application/json'})\n"
"        raw = urlopen(req, timeout=timeout).read()\n"
"        if not isinstance(raw, str):\n"
"            raw = raw.decode('utf-8')\n"
"        return json.loads(raw)\n"
"    def _ds_note_pending(s):\n"
"        try:\n"
"            with _ds_lock:\n"
"                if s in _ds_pending or len(_ds_pending) >= 300:\n"
"                    return\n"
"                _ds_pending[s] = 0\n"
"            _ds_ensure_poller()\n"
"        except Exception:\n"
"            pass\n"
"    def _ds_fetch(s):\n"
"        now = time.time()\n"
"        if now < _ds_state['down_until']:\n"
"            return None\n"
"        try:\n"
"            out = _ds_http('/translate', {'text': s, 'cache_only': True}, 0.25).get('translated_text')\n"
"            if out and out != s:\n"
"                return out\n"
"            _ds_note_pending(s)\n"
"            return None\n"
"        except Exception:\n"
"            _ds_state['down_until'] = now + 30.0\n"
"            return None\n"
"    def _ds_poll_loop():\n"
"        while True:\n"
"            try:\n"
"                time.sleep(2.0)\n"
"                if time.time() < _ds_state['down_until']:\n"
"                    continue\n"
"                with _ds_lock:\n"
"                    batch = list(_ds_pending.keys())[:48]\n"
"                if not batch:\n"
"                    continue\n"
"                got = _ds_http('/batch', {'texts': batch, 'cache_only': True}, 2.0).get('translations') or {}\n"
"                healed = 0\n"
"                now = time.time()\n"
"                for k in batch:\n"
"                    v = got.get(k)\n"
"                    if v and v != k:\n"
"                        _ds_memo[k] = (v, now)\n"
"                        with _ds_lock:\n"
"                            _ds_pending.pop(k, None)\n"
"                        healed += 1\n"
"                with _ds_lock:\n"
"                    for k in batch:\n"
"                        if k in _ds_pending:\n"
"                            _ds_pending[k] += 1\n"
"                            if _ds_pending[k] > 60:\n"
"                                del _ds_pending[k]\n"
"                if healed:\n"
"                    try:\n"
"                        renpy.restart_interaction()\n"
"                    except Exception:\n"
"                        pass\n"
"            except Exception:\n"
"                pass\n"
"    def _ds_ensure_poller():\n"
"        with _ds_lock:\n"
"            if _ds_state['poller']:\n"
"                return\n"
"            _ds_state['poller'] = True\n"
"        try:\n"
"            _ds_t = threading.Thread(target=_ds_poll_loop)\n"
"            _ds_t.daemon = True\n"
"            _ds_t.start()\n"
"        except Exception:\n"
"            _ds_state['poller'] = False\n"
"    def _ds_translate(s):\n"
"        try:\n"
"            if not s or _ds_has_cjk(s):\n"
"                return s\n"
"            return _ds_fetch(s) or s\n"
"        except Exception:\n"
"            return s\n"
"    def _ds_say(who, what, *args, **kwargs):\n"
"        return _ds_old_say(who, _ds_translate(what), *args, **kwargs)\n"
"    renpy.exports.say = _ds_say\n"
"    try:\n"
"        renpy.say = _ds_say\n"
"    except Exception:\n"
"        pass\n"
"    _ds_font = None\n"
"    for _ds_cand, _ds_spec in (('ds_font.ttf', u'ds_font.ttf'), ('ds_font.otf', u'ds_font.otf'), ('ds_font.ttc', u'0@ds_font.ttc')):\n"
"        if os.path.exists(os.path.join(renpy.config.gamedir, _ds_cand)):\n"
"            _ds_font = _ds_spec\n"
"            break\n"
"    if _ds_font:\n"
"        try:\n"
"            _ds_all_styles = list(renpy.style.styles.values())\n"
"        except Exception:\n"
"            _ds_all_styles = []\n"
"        for _ds_st in _ds_all_styles:\n"
"            try:\n"
"                _ds_st.font = _ds_font\n"
"            except Exception:\n"
"                pass\n"
"        for _ds_style_name in ('default', 'say_dialogue', 'say_label', 'say_thought', 'centered_text', 'nvl_dialogue', 'nvl_label', 'nvl_thought'):\n"
"            try:\n"
"                getattr(style, _ds_style_name).font = _ds_font\n"
"            except Exception:\n"
"                pass\n"
"        try:\n"
"            _ds_fonts = set([u'DejaVuSans.ttf'])\n"
"            for _ds_gv in ('text_font', 'name_text_font', 'interface_text_font', 'button_text_font', 'choice_button_text_font', 'label_text_font', 'prompt_text_font'):\n"
"                try:\n"
"                    _ds_val = getattr(gui, _ds_gv, None)\n"
"                    if _ds_val:\n"
"                        _ds_fonts.add(_ds_val)\n"
"                except Exception:\n"
"                    pass\n"
"            try:\n"
"                for _ds_fn in os.listdir(renpy.config.gamedir):\n"
"                    _ds_low = _ds_fn.lower()\n"
"                    if (_ds_low.endswith('.ttf') or _ds_low.endswith('.otf')) and not _ds_low.startswith('ds_font'):\n"
"                        _ds_fonts.add(_ds_fn)\n"
"            except Exception:\n"
"                pass\n"
"            for _ds_fn in _ds_fonts:\n"
"                for _ds_b in (False, True):\n"
"                    for _ds_i in (False, True):\n"
"                        renpy.config.font_replacement_map[(_ds_fn, _ds_b, _ds_i)] = (_ds_font, _ds_b, _ds_i)\n"
"        except Exception:\n"
"            pass\n"
"    def _ds_wants_ui_text(s):\n"
"        if not s or len(s) < 2 or len(s) > 1200 or _ds_has_cjk(s):\n"
"            return False\n"
"        alpha = 0\n"
"        digits = 0\n"
"        for ch in s:\n"
"            if ('a' <= ch <= 'z') or ('A' <= ch <= 'Z'):\n"
"                alpha += 1\n"
"            elif '0' <= ch <= '9':\n"
"                digits += 1\n"
"        return alpha >= 2 and digits < 4\n"
"    def _ds_replace_text(s):\n"
"        try:\n"
"            if not _ds_wants_ui_text(s):\n"
"                return s\n"
"            now = time.time()\n"
"            hit = _ds_memo.get(s)\n"
"            if hit is not None:\n"
"                val, ts = hit\n"
"                if val != s:\n"
"                    return val\n"
"                if now - ts < 5.0:\n"
"                    return s\n"
"            if len(_ds_memo) > 4000:\n"
"                _ds_memo.clear()\n"
"            out = _ds_fetch(s)\n"
"            if out:\n"
"                _ds_memo[s] = (out, now)\n"
"                return out\n"
"            _ds_memo[s] = (s, now)\n"
"            return s\n"
"        except Exception:\n"
"            return s\n"
"    if _ds_font:\n"
"        try:\n"
"            if hasattr(renpy.config, 'replace_text'):\n"
"                _ds_prev_replace = renpy.config.replace_text\n"
"                def _ds_chain_replace(s):\n"
"                    if _ds_prev_replace is not None:\n"
"                        try:\n"
"                            s = _ds_prev_replace(s)\n"
"                        except Exception:\n"
"                            pass\n"
"                    return _ds_replace_text(s)\n"
"                renpy.config.replace_text = _ds_chain_replace\n"
"        except Exception:\n"
"            pass\n";

/* ----------------------------------------------------------------
 * RPGM_HOOK — RPG Maker MV/MZ 翻译 JavaScript 脚本（嵌入源码）
 *
 * 功能概述：
 *   1. Hook Window_Base.drawTextEx 和 drawText，将显示文本发送到本地服务器翻译
 *   2. 使用 cache_only 模式，仅缓存命中时替换（不阻塞游戏）
 *   3. 注入 CJK @font-face（ds_font.ttf/ttc），确保中文能正确渲染
 *   4. 覆盖 Window_Base.standardFontFace 和 Game_System.mainFontFace，
 *      将 CJK 字体设为首选
 *   5. 统一插件外部 CRLF 文本的缓存键，并为重复 miss 设置短冷却
 * ---------------------------------------------------------------- */
static const char RPGM_HOOK[] =
"(function(){\n"
"  'use strict';\n"
"  var URL='http://127.0.0.1:19999/translate';\n"
"  var CJK_FONT='DeepSeekCJK';\n"
"  var cache=Object.create(null);\n"
"  var retryAfter=Object.create(null);\n"
"  var MISS_RETRY_MS=1000;\n"
"  function installCjkFont(){\n"
"    try{var st=document.createElement('style'); st.type='text/css'; st.textContent=\"@font-face{font-family:'DeepSeekCJK';src:url('fonts/ds_font.ttf') format('truetype'),url('fonts/ds_font.ttc') format('truetype');font-weight:normal;font-style:normal;} body,canvas{font-family:'DeepSeekCJK',sans-serif;}\"; (document.head||document.documentElement).appendChild(st); if(document.fonts&&document.fonts.load){document.fonts.load('16px '+CJK_FONT);}}\n"
"    catch(e){}\n"
"    try{if(window.Window_Base&&Window_Base.prototype.standardFontFace&&!Window_Base.prototype._dsStandardFontFace){var oldFont=Window_Base.prototype.standardFontFace; Window_Base.prototype._dsStandardFontFace=oldFont; Window_Base.prototype.standardFontFace=function(){var base=oldFont.call(this)||''; return base.indexOf(CJK_FONT)>=0?base:(base?CJK_FONT+', '+base:CJK_FONT);};}}\n"
"    catch(e){}\n"
"    try{if(window.Game_System&&Game_System.prototype.mainFontFace&&!Game_System.prototype._dsMainFontFace){var oldMain=Game_System.prototype.mainFontFace; Game_System.prototype._dsMainFontFace=oldMain; Game_System.prototype.mainFontFace=function(){var base=oldMain.call(this)||''; return base.indexOf(CJK_FONT)>=0?base:(base?CJK_FONT+', '+base:CJK_FONT);};}}\n"
"    catch(e){}\n"
"  }\n"
"  installCjkFont();\n"
"  function hasCjk(s){return /[\\u4e00-\\u9fff]/.test(String(s||''));}\n"
"  function tr(s){\n"
"    s=String(s==null?'':s); if(!s||hasCjk(s)) return s; var key=s.replace(/\\r+$/,''); if(!key) return s; if(cache[key]) return cache[key]; var now=Date.now(); if(retryAfter[key]&&retryAfter[key]>now) return s;\n"
"    try{var x=new XMLHttpRequest(); x.open('POST',URL,false); x.setRequestHeader('Content-Type','application/json'); x.send(JSON.stringify({text:key,cache_only:true})); if(x.status===200){var r=JSON.parse(x.responseText); var v=r.translated_text||r.translation||key; if(r.source==='cache'||(v&&v!==key&&r.source!=='miss'&&r.source!=='queued')){cache[key]=v; delete retryAfter[key]; return v;}}}\n"
"    catch(e){}\n"
"    retryAfter[key]=now+MISS_RETRY_MS;\n"
"    return s;\n"
"  }\n"
"  if(window.Window_Message&&Window_Message.prototype.startMessage&&!Window_Message.prototype._dsStartMessage){var oldMsg=Window_Message.prototype.startMessage; Window_Message.prototype._dsStartMessage=oldMsg; Window_Message.prototype.startMessage=function(){var gm=window.$gameMessage; var original=gm&&gm._texts; if(!original||!Array.isArray(original)||!original.length){return oldMsg.apply(this,arguments);} var translated=new Array(original.length); for(var i=0;i<original.length;i++){translated[i]=tr(original[i]);} gm._texts=translated; try{return oldMsg.apply(this,arguments);} finally{gm._texts=original;}};}\n"
"  if(window.Window_Base&&Window_Base.prototype.drawTextEx){var old=Window_Base.prototype.drawTextEx; Window_Base.prototype.drawTextEx=function(text,x,y,w){return old.call(this,tr(text),x,y,w);};}\n"
"  if(window.Window_Base&&Window_Base.prototype.drawText){var old2=Window_Base.prototype.drawText; Window_Base.prototype.drawText=function(text,x,y,w,a){return old2.call(this,tr(text),x,y,w,a);};}\n"
"})();\n";

/* ----------------------------------------------------------------
 * deploy_renpy_font — 为 Ren'Py 游戏部署 CJK 字体
 *
 * Ren'Py 默认字体不含 CJK 字形，翻译后的中文会显示为方块。
 * 从 Windows 系统 Fonts 目录复制一个 CJK 字体（优先 simhei.ttf 黑体，
 * 回退 msyh.ttc 微软雅黑）到游戏的 game/ 目录，hook 脚本的
 * style 覆盖会引用此字体。
 * ---------------------------------------------------------------- */
static void deploy_renpy_font(const WCHAR *game) {
    WCHAR ttc_dst[MAX_PATH * 4], ttf_dst[MAX_PATH * 4];
    path_join(ttc_dst, MAX_PATH * 4, game, L"ds_font.ttc");
    path_join(ttf_dst, MAX_PATH * 4, game, L"ds_font.ttf");
    if (exists_path(ttc_dst) || exists_path(ttf_dst)) return;

    WCHAR windir[MAX_PATH];
    if (!GetWindowsDirectoryW(windir, MAX_PATH)) return;

    /* 优先 TTF 格式："0@file.ttc" 集合索引语法并非所有 Ren'Py 版本都支持，
       而普通 TTF 文件名在任何版本都可以工作。 */
    WCHAR src[MAX_PATH * 4];
    _snwprintf(src, MAX_PATH * 4, L"%s\\Fonts\\simhei.ttf", windir);
    src[MAX_PATH * 4 - 1] = 0;
    if (exists_path(src) && copy_file_safe(src, ttf_dst)) {
        append_log(L"Ren'Py：已部署中文字体（黑体）：%s", ttf_dst);
        return;
    }
    _snwprintf(src, MAX_PATH * 4, L"%s\\Fonts\\msyh.ttc", windir);
    src[MAX_PATH * 4 - 1] = 0;
    if (exists_path(src) && copy_file_safe(src, ttc_dst)) {
        append_log(L"Ren'Py：已部署中文字体（微软雅黑）：%s", ttc_dst);
        return;
    }
    append_log(L"Ren'Py：未找到系统中文字体（simhei.ttf/msyh.ttc），翻译文本可能显示为方块。");
}

/* ----------------------------------------------------------------
 * deploy_rpgm_font — 为 RPG Maker MV/MZ 部署 CJK 字体
 *
 * RPG Maker 在 canvas 上渲染文本，需要 @font-face 声明。
 * 将系统 CJK 字体复制到 www/fonts/ 目录供 hook 脚本引用。
 * ---------------------------------------------------------------- */
static void deploy_rpgm_font(const WCHAR *dir) {
    WCHAR font_dir[MAX_PATH * 4], ttf_dst[MAX_PATH * 4], ttc_dst[MAX_PATH * 4];
    path_join(font_dir, MAX_PATH * 4, dir, L"www\\fonts");
    ensure_dir(font_dir);
    path_join(ttf_dst, MAX_PATH * 4, font_dir, L"ds_font.ttf");
    path_join(ttc_dst, MAX_PATH * 4, font_dir, L"ds_font.ttc");
    if (exists_path(ttf_dst) || exists_path(ttc_dst)) return;

    WCHAR windir[MAX_PATH];
    if (!GetWindowsDirectoryW(windir, MAX_PATH)) return;

    WCHAR src[MAX_PATH * 4];
    _snwprintf(src, MAX_PATH * 4, L"%s\\Fonts\\simhei.ttf", windir);
    src[MAX_PATH * 4 - 1] = 0;
    if (exists_path(src) && copy_file_safe(src, ttf_dst)) {
        append_log(L"RPGM MV/MZ: deployed CJK font: %s", ttf_dst);
        return;
    }

    _snwprintf(src, MAX_PATH * 4, L"%s\\Fonts\\msyh.ttc", windir);
    src[MAX_PATH * 4 - 1] = 0;
    if (exists_path(src) && copy_file_safe(src, ttc_dst)) {
        append_log(L"RPGM MV/MZ: deployed CJK font: %s", ttc_dst);
        return;
    }

    append_log(L"RPGM MV/MZ: no system CJK font found (simhei.ttf/msyh.ttc); translated text may render as boxes.");
}

/* ----------------------------------------------------------------
 * deploy_renpy — 部署 Ren'Py 翻译 hook
 *
 * 将 RENPY_HOOK Python 脚本写入 game/iron_deepseek.rpy，
 * 并调用 deploy_renpy_font 部署 CJK 字体。
 * init 999 保证 hook 在所有其他游戏脚本之后加载。
 * ---------------------------------------------------------------- */
int deploy_renpy(const WCHAR *dir) {
    WCHAR game[MAX_PATH * 4], hook[MAX_PATH * 4];
    path_join(game, MAX_PATH * 4, dir, L"game");
    if (!is_dir(game)) return 0;
    path_join(hook, MAX_PATH * 4, game, L"iron_deepseek.rpy");
    if (!write_text_file_utf8(hook, RENPY_HOOK)) return 0;
    deploy_renpy_font(game);
    append_log(L"已部署 Ren'Py hook：%s", hook);
    return 1;
}

/* ----------------------------------------------------------------
 * deploy_rpgm — 部署 RPG Maker MV/MZ 翻译 hook
 *
 * 1. 将 RPGM_HOOK JS 脚本写入 www/js/hook_rpgm_mv.js
 * 2. 调用 deploy_rpgm_font 部署 CJK 字体到 www/fonts/
 * 3. 修改 www/index.html，在 </body> 前插入 <script> 标签引用 hook
 * ---------------------------------------------------------------- */
int deploy_rpgm(const WCHAR *dir) {
    WCHAR jsdir[MAX_PATH * 4], hook[MAX_PATH * 4], index[MAX_PATH * 4];
    path_join(jsdir, MAX_PATH * 4, dir, L"www\\js");
    if (!is_dir(jsdir)) return 0;
    path_join(hook, MAX_PATH * 4, jsdir, L"hook_rpgm_mv.js");
    if (!write_text_file_utf8(hook, RPGM_HOOK)) return 0;
    deploy_rpgm_font(dir);

    /* 在 index.html 中注入 hook 脚本引用（仅当尚未注入时） */
    path_join(index, MAX_PATH * 4, dir, L"www\\index.html");
    char *html = NULL;
    DWORD sz = 0;
    if (read_file_bytes(index, &html, &sz)) {
        if (!strstr(html, "hook_rpgm_mv.js")) {
            const char *script = "\n<script type=\"text/javascript\" src=\"js/hook_rpgm_mv.js\"></script>\n";
            const char *body = strstr(html, "</body>");
            ByteBuf out;
            out.cap = sz + 256;
            out.len = 0;
            out.data = (char *)malloc(out.cap);
            if (out.data) {
                out.data[0] = 0;
                if (body) {
                    bb_add(&out, html, (size_t)(body - html));
                    bb_add(&out, script, strlen(script));
                    bb_add(&out, body, strlen(body));
                } else {
                    bb_add(&out, html, sz);
                    bb_add(&out, script, strlen(script));
                }
                write_file_bytes(index, out.data, (DWORD)out.len);
                free(out.data);
            }
        }
        free(html);
    }
    append_log(L"已部署 RPGM MV/MZ hook：%s", hook);
    return 1;
}

/* ======================== Unity Mono 部署辅助 ======================== */

/* 在 payloads/UnityTranslator 中查找指定文件 */
static int find_unity_payload_file(WCHAR *out, size_t cap, const WCHAR *leaf) {
    WCHAR base[MAX_PATH * 4];
    path_join(base, MAX_PATH * 4, g_root, L"payloads\\UnityTranslator");
    path_join(out, cap, base, leaf);
    return exists_path(out);
}

static int files_equal(const WCHAR *a, const WCHAR *b);

/* 查找 UnityTranslator.dll 模板（BepInEx 5 版） */
int find_unity_template(WCHAR *out, size_t cap) {
    WCHAR p[MAX_PATH * 4];
    if (!find_unity_payload_file(p, MAX_PATH * 4, L"UnityTranslator.dll")) return 0;
    size_t need = wcslen(p) + 1;
    if (need > cap) return 0;
    memcpy(out, p, need * sizeof(WCHAR));
    return 1;
}

/* 查找 UnityTranslator.BepInEx6.dll 模板（Unity 6+ BepInEx 6 版） */
static int find_unity_bepinex6_template(WCHAR *out, size_t cap) {
    WCHAR p[MAX_PATH * 4];
    if (!find_unity_payload_file(p, MAX_PATH * 4, L"UnityTranslator.BepInEx6.dll")) return 0;
    size_t need = wcslen(p) + 1;
    if (need > cap) return 0;
    memcpy(out, p, need * sizeof(WCHAR));
    return 1;
}

/* 检查文件是否为本工具内置的 Unity Mono 插件（用于 IL2CPP 部署时禁用旧文件） */
static int is_bundled_unity_mono_plugin(const WCHAR *path) {
    WCHAR src[MAX_PATH * 4];
    if (find_unity_template(src, MAX_PATH * 4) && files_equal(path, src)) return 1;
    if (find_unity_bepinex6_template(src, MAX_PATH * 4) && files_equal(path, src)) return 1;
    return 0;
}

/* 将文件重命名为 .disabled 后缀（不删除，保留备份） */
static int disable_existing_file(const WCHAR *path) {
    if (!exists_path(path)) return 0;
    WCHAR disabled[MAX_PATH * 4];
    _snwprintf(disabled, MAX_PATH * 4, L"%s.disabled", path);
    disabled[MAX_PATH * 4 - 1] = 0;
    DeleteFileW(disabled);
    return MoveFileExW(path, disabled, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
}

/* 二进制比较两个文件是否完全相同 */
static int files_equal(const WCHAR *a, const WCHAR *b) {
    char *ab = NULL, *bb = NULL;
    DWORD asz = 0, bsz = 0;
    int ok = 0;
    if (read_file_bytes(a, &ab, &asz) && read_file_bytes(b, &bb, &bsz)) {
        ok = asz == bsz && memcmp(ab, bb, asz) == 0;
    }
    free(ab);
    free(bb);
    return ok;
}

/* ======================== Unity IL2CPP 部署辅助 ======================== */

/* 在 payloads/UnityIL2CPP 中查找指定子目录 */
static int find_il2cpp_payload(WCHAR *out, size_t cap, const WCHAR *leaf) {
    WCHAR base[MAX_PATH * 4];
    path_join(base, MAX_PATH * 4, g_root, L"payloads\\UnityIL2CPP");
    path_join(out, cap, base, leaf);
    return is_dir(out);
}

/* 从 payload 目录复制单个文件到游戏目录 */
static int copy_payload_file(const WCHAR *payload_root, const WCHAR *rel, const WCHAR *game_dir) {
    WCHAR src[MAX_PATH * 4], dst[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, payload_root, rel);
    path_join(dst, MAX_PATH * 4, game_dir, rel);
    if (!exists_path(src)) return 0;
    return copy_file_safe(src, dst);
}

/* 从 payload 目录复制整个子目录树到游戏目录 */
static int copy_payload_tree(const WCHAR *payload_root, const WCHAR *rel, const WCHAR *game_dir) {
    WCHAR src[MAX_PATH * 4], dst[MAX_PATH * 4];
    path_join(src, MAX_PATH * 4, payload_root, rel);
    path_join(dst, MAX_PATH * 4, game_dir, rel);
    if (!is_dir(src)) return 0;
    return copy_tree_safe(src, dst);
}

/* ----------------------------------------------------------------
 * pe_machine — 读取 PE 文件的机器类型
 *
 * 解析 PE 头获取 IMAGE_FILE_HEADER.Machine 字段。
 * 用于判断 GameAssembly.dll 是 x64 (0x8664) 还是其他架构。
 * pe 偏移量来自文件自身，做边界检查防止溢出。
 * ---------------------------------------------------------------- */
static int pe_machine(const WCHAR *path) {
    char *buf = NULL;
    DWORD size = 0;
    int machine = 0;
    if (!read_file_bytes(path, &buf, &size)) return 0;
    if (size >= 0x40 && buf[0] == 'M' && buf[1] == 'Z') {
        DWORD pe = *(DWORD *)(buf + 0x3c);
        /* pe 偏移量是文件控制的；做边界检查防止 (pe + 6) 溢出 */
        if (pe < size && size - pe > 6 && !memcmp(buf + pe, "PE\0\0", 4)) {
            machine = *(unsigned short *)(buf + pe + 4);
        }
    }
    free(buf);
    return machine;
}

/* ----------------------------------------------------------------
 * write_xunity_config — 生成 XUnity.AutoTranslator 配置文件
 *
 * 写入 BepInEx/config/AutoTranslatorConfig.ini，
 * 配置 XUnity 使用本地 DeepSeek 端点 (http://127.0.0.1:19999)，
 * 语言方向 auto→zh-CN，并启用所有 UI 文本框架（UGUI/TMP/NGUI 等）。
 * ---------------------------------------------------------------- */
static void write_xunity_config(const WCHAR *dir) {
    WCHAR cfgdir[MAX_PATH * 4], cfg[MAX_PATH * 4];
    path_join(cfgdir, MAX_PATH * 4, dir, L"BepInEx\\config");
    ensure_dir(cfgdir);
    path_join(cfg, MAX_PATH * 4, cfgdir, L"AutoTranslatorConfig.ini");

    static const char XUNITY_CONFIG_FMT[] =
        "[Service]\n"
        "Endpoint=DeepSeekTranslate\n"
        "FallbackEndpoint=\n"
        "\n"
        "[General]\n"
        "Language=zh-CN\n"
        "FromLanguage=auto\n"
        "\n"
        "[Files]\n"
        "Directory=Translation\\{Lang}\\Text\n"
        "OutputFile=Translation\\{Lang}\\Text\\_AutoGeneratedTranslations.txt\n"
        "SubstitutionFile=Translation\\{Lang}\\Text\\_Substitutions.txt\n"
        "PreprocessorsFile=Translation\\{Lang}\\Text\\_Preprocessors.txt\n"
        "PostprocessorsFile=Translation\\{Lang}\\Text\\_Postprocessors.txt\n"
        "\n"
        "[TextFrameworks]\n"
        "EnableIMGUI=False\n"
        "EnableUGUI=True\n"
        "EnableUIElements=True\n"
        "EnableNGUI=True\n"
        "EnableTextMeshPro=True\n"
        "EnableTextMesh=False\n"
        "EnableFairyGUI=True\n"
        "\n"
        "[Behaviour]\n"
        "MaxCharactersPerTranslation=400\n"
        "IgnoreWhitespaceInDialogue=True\n"
        "MinDialogueChars=20\n"
        "ForceSplitTextAfterCharacters=0\n"
        "CopyToClipboard=False\n"
        "MaxClipboardCopyCharacters=2500\n"
        "ClipboardDebounceTime=1.25\n"
        "EnableUIResizing=False\n"
        "EnableBatching=True\n"
        "UseStaticTranslations=True\n"
        "OverrideFont=Microsoft YaHei\n"
        "OverrideFontSize=\n"
        "OverrideFontTextMeshPro=\n"
        "FallbackFontTextMeshPro=\n"
        "ResizeUILineSpacingScale=\n"
        "ForceUIResizing=False\n"
        "IgnoreTextStartingWith=\\u180e;Confidence increased;Confidence decreased;Confidence lowered;Confidence reduced;Confidence changed;\n"
        "TextGetterCompatibilityMode=False\n"
        "GameLogTextPaths=\n"
        "RomajiPostProcessing=ReplaceMacronWithCircumflex;RemoveApostrophes;ReplaceHtmlEntities\n"
        "TranslationPostProcessing=ReplaceMacronWithCircumflex;ReplaceHtmlEntities\n"
        "RegexPostProcessing=\n"
        "CacheRegexPatternResults=False\n"
        "PersistRichTextMode=Final\n"
        "CacheRegexLookups=False\n"
        "CacheWhitespaceDifferences=False\n"
        "GenerateStaticSubstitutionTranslations=False\n"
        "GeneratePartialTranslations=False\n"
        "EnableTranslationScoping=True\n"
        "EnableSilentMode=True\n"
        "BlacklistedIMGUIPlugins=\n"
        "EnableTextPathLogging=False\n"
        "OutputUntranslatableText=False\n"
        "IgnoreVirtualTextSetterCallingRules=False\n"
        "MaxTextParserRecursion=1\n"
        "HtmlEntityPreprocessing=True\n"
        "HandleRichText=True\n"
        "EnableTranslationHelper=False\n"
        "ForceMonoModHooks=False\n"
        "InitializeHarmonyDetourBridge=False\n"
        "RedirectedResourceDetectionStrategy=AppendMongolianVowelSeparatorAndRemoveAll\n"
        "OutputTooLongText=False\n"
        "TemplateAllNumberAway=True\n"
        "ReloadTranslationsOnFileChange=True\n"
        "DisableTextMeshProScrollInEffects=False\n"
        "CacheParsedTranslations=False\n"
        "\n"
        "[Texture]\n"
        "TextureDirectory=Translation\\{Lang}\\Texture\n"
        "EnableTextureTranslation=False\n"
        "EnableTextureDumping=False\n"
        "EnableTextureToggling=False\n"
        "EnableTextureScanOnSceneLoad=False\n"
        "EnableSpriteRendererHooking=False\n"
        "LoadUnmodifiedTextures=False\n"
        "DetectDuplicateTextureNames=False\n"
        "DuplicateTextureNames=\n"
        "EnableLegacyTextureLoading=False\n"
        "TextureHashGenerationStrategy=FromImageName\n"
        "CacheTexturesInMemory=True\n"
        "EnableSpriteHooking=False\n"
        "\n"
        "[ResourceRedirector]\n"
        "PreferredStoragePath=Translation\\{Lang}\\RedirectedResources\n"
        "EnableTextAssetRedirector=False\n"
        "LogAllLoadedResources=False\n"
        "EnableDumping=False\n"
        "CacheMetadataForAllFiles=True\n"
        "\n"
        "[Http]\n"
        "UserAgent=\n"
        "DisableCertificateValidation=True\n"
        "\n"
        "[TranslationAggregator]\n"
        "Width=400\n"
        "Height=100\n"
        "EnabledTranslators=\n"
        "\n"
        "[Debug]\n"
        "EnableConsole=False\n"
        "\n"
        "[Migrations]\n"
        "Enable=True\n"
        "Tag=5.6.1\n"
        "\n"
        "[Custom]\n"
        "Url=http://127.0.0.1:19999/translate\n"
        "EnableShortDelay=False\n"
        "DisableSpamChecks=False\n"
        "\n"
        "[DeepSeek]\n"
        "Url=http://127.0.0.1:19999\n"
        "MaxBatchSize=16\n"
        "MaxConcurrency=8\n"
        "TranslationDelay=0.1\n"
        "DisplaySafePunctuation=True\n";
    write_text_file_utf8(cfg, XUNITY_CONFIG_FMT);
}

/* ----------------------------------------------------------------
 * detect_unity_major — 从 globalgamemanagers 文件读取 Unity 大版本号
 *
 * <game>\*_Data\globalgamemanagers 文件头部包含版本字符串
 * （如 "6000.4.0f1" 或 "2022.3.62f3"）。扫描前 4096 字节中
 * 符合 Unity 版本模式的数字序列（5、2017-2023 或 6000+），
 * 提取大版本号。用于决定部署 BepInEx 5 还是 BepInEx 6 运行时。
 * ---------------------------------------------------------------- */
static int detect_unity_major(const WCHAR *dir) {
    WCHAR pat[MAX_PATH * 4];
    path_join(pat, MAX_PATH * 4, dir, L"*_Data");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int major = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        WCHAR ggm[MAX_PATH * 4], datadir[MAX_PATH * 4];
        path_join(datadir, MAX_PATH * 4, dir, fd.cFileName);
        path_join(ggm, MAX_PATH * 4, datadir, L"globalgamemanagers");
        char *buf = NULL;
        DWORD sz = 0;
        if (read_file_bytes(ggm, &buf, &sz)) {
            DWORD lim = sz < 4096 ? sz : 4096;
            for (DWORD i = 0; i + 5 < lim; i++) {
                if (buf[i] < '0' || buf[i] > '9') continue;
                int v = 0, d = 0;
                DWORD j = i;
                while (j < lim && buf[j] >= '0' && buf[j] <= '9' && d < 5) { v = v * 10 + (buf[j] - '0'); j++; d++; }
                if (d >= 1 && j < lim && buf[j] == '.' && (v == 5 || (v >= 2017 && v <= 2023) || v >= 6000)) { major = v; break; }
            }
            free(buf);
        }
        if (major) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return major;
}

/* 检查游戏是否已有 BepInEx 6 Unity.Mono 运行时 */
static int unity_has_bepinex6_mono(const WCHAR *dir) {
    WCHAR p[MAX_PATH * 4];
    path_join(p, MAX_PATH * 4, dir, L"BepInEx\\core\\BepInEx.Unity.Mono.dll");
    return exists_path(p);
}

/* payload 缺失时只记录可复制执行的修复命令，不在部署路径中自动联网下载。 */
static void log_payload_install_command(const WCHAR *flag) {
    append_log(L"Install runtime payloads with:");
    append_log(L"  powershell -ExecutionPolicy Bypass -File scripts\\install_runtime_payloads.ps1 %s", flag);
}

/* ----------------------------------------------------------------
 * install_bepinex_mono_runtime — 安装 BepInEx Mono 运行时到游戏目录
 *
 * 复制 winhttp.dll（doorstop 加载器）、doorstop_config.ini、
 * .doorstop_version 以及 BepInEx/core 整个目录树。
 * use_bepinex6 为真时使用 payloads/UnityMonoRuntime6（Unity 6+），
 * 否则使用 payloads/UnityMonoRuntime（BepInEx 5）。
 * ---------------------------------------------------------------- */
static int install_bepinex_mono_runtime(const WCHAR *dir, int use_bepinex6) {
    WCHAR mono_rt[MAX_PATH * 4];
    path_join(mono_rt, MAX_PATH * 4, g_root, use_bepinex6 ? L"payloads\\UnityMonoRuntime6" : L"payloads\\UnityMonoRuntime");
    if (!is_dir(mono_rt)) {
        append_log(use_bepinex6
            ? L"Unity: missing BepInEx 6 Mono runtime payload (payloads\\UnityMonoRuntime6)."
            : L"Unity: missing BepInEx 5 Mono runtime payload (payloads\\UnityMonoRuntime).");
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6" : L"-UnityMono5");
        return 0;
    }

    int ok = 1;
    ok &= copy_payload_file(mono_rt, L"winhttp.dll", dir);
    ok &= copy_payload_file(mono_rt, L"doorstop_config.ini", dir);
    ok &= copy_payload_file(mono_rt, L".doorstop_version", dir);
    ok &= copy_payload_tree(mono_rt, L"BepInEx\\core", dir);
    if (!ok) {
        append_log(use_bepinex6
            ? L"Unity: BepInEx 6 Mono runtime deployment is incomplete; check payloads\\UnityMonoRuntime6."
            : L"Unity: BepInEx 5 Mono runtime deployment is incomplete; check payloads\\UnityMonoRuntime.");
        log_payload_install_command(use_bepinex6 ? L"-UnityMono6 -Force" : L"-UnityMono5 -Force");
        return 0;
    }
    append_log(use_bepinex6
        ? L"Unity: deployed BepInEx 6 (Mono) runtime for Unity 6+."
        : L"Unity: deployed BepInEx 5 (Mono) runtime.");
    return 1;
}

/* ----------------------------------------------------------------
 * ensure_bepinex_mono — 确保游戏有 BepInEx Mono 运行时
 *
 * 自动安装策略：
 *   - Unity 6+ (major >= 6000) 需要 BepInEx 6 Unity.Mono 运行时
 *   - 旧版 Unity 保持使用 BepInEx 5（兼容已有安装）
 *   - 如果用户已自行安装 BepInEx，保留不覆盖（除非版本不匹配）
 * ---------------------------------------------------------------- */
static int ensure_bepinex_mono(const WCHAR *dir) {
    int major = detect_unity_major(dir);
    int use_bepinex6 = major >= 6000;
    WCHAR bep[MAX_PATH * 4];
    path_join(bep, MAX_PATH * 4, dir, L"BepInEx");
    if (is_dir(bep)) {
        if (use_bepinex6 && !unity_has_bepinex6_mono(dir)) {
            append_log(L"Unity %d (Unity 6+): existing BepInEx is not Unity.Mono 6; updating runtime files.", major);
            return install_bepinex_mono_runtime(dir, 1);
        }
        return 1; /* 非Unity6路径保留用户已有的BepInEx */
    }
    return install_bepinex_mono_runtime(dir, use_bepinex6);
}

/* ----------------------------------------------------------------
 * deploy_unity — 部署 Unity Mono 翻译插件
 *
 * 流程：
 *   1. ensure_bepinex_mono — 安装/检查 BepInEx 运行时
 *   2. 根据运行时版本选择 BepInEx 5 或 6 的插件 DLL
 *   3. 复制 UnityTranslator.dll + Newtonsoft.Json.dll 到 BepInEx/plugins/
 *   4. 复制 TMP 字体资源包（如有）到 BepInEx/font/
 * ---------------------------------------------------------------- */
int deploy_unity(const WCHAR *dir) {
    WCHAR plugins[MAX_PATH * 4], dll[MAX_PATH * 4], src[MAX_PATH * 4], json_src[MAX_PATH * 4], json_dst[MAX_PATH * 4], fontfix[MAX_PATH * 4];
    path_join(plugins, MAX_PATH * 4, dir, L"BepInEx\\plugins");

    if (!ensure_bepinex_mono(dir)) return 0;

    int use_bepinex6 = unity_has_bepinex6_mono(dir);
    int found_template = use_bepinex6
        ? find_unity_bepinex6_template(src, MAX_PATH * 4)
        : find_unity_template(src, MAX_PATH * 4);
    if (!found_template) {
        append_log(use_bepinex6
            ? L"Unity: missing UnityTranslator.BepInEx6.dll template."
            : L"Unity: missing UnityTranslator.dll template.");
        return 0;
    }
    path_join(dll, MAX_PATH * 4, plugins, L"UnityTranslator.dll");
    if (!copy_file_safe(src, dll)) return 0;
    if (!find_unity_payload_file(json_src, MAX_PATH * 4, L"Newtonsoft.Json.dll")) {
        append_log(L"Unity: missing Newtonsoft.Json.dll dependency; UnityTranslator cannot start.");
        log_payload_install_command(L"-Newtonsoft");
        return 0;
    }
    path_join(json_dst, MAX_PATH * 4, plugins, L"Newtonsoft.Json.dll");
    if (!copy_file_safe(json_src, json_dst)) return 0;
    if (find_il2cpp_payload(fontfix, MAX_PATH * 4, L"TMPFontAssetBundles")) {
        if (!copy_payload_tree(fontfix, L"BepInEx\\font", dir)) return 0;
    } else {
        append_log(L"Unity Mono: TMP font asset bundle payload missing; Chinese TMP glyphs may use overlay fallback.");
    }
    append_log(use_bepinex6
        ? L"Unity: deployed BepInEx 6 compatible Unity plugin: %s"
        : L"Unity: deployed BepInEx 5 compatible Unity plugin: %s", dll);
    return 1;
}

/* ----------------------------------------------------------------
 * deploy_unity_il2cpp — 部署 Unity IL2CPP 翻译插件
 *
 * 完整部署流程：
 *   1. PE 机器类型检查：仅支持 x64（0x8664）
 *   2. 禁用旧的 Mono 版 UnityTranslator.dll（如有）
 *   3. 从 payloads/UnityIL2CPP/BepInExRuntime 复制运行时：
 *      - doorstop_config.ini, winhttp.dll, .doorstop_version
 *      - dotnet/ 目录（自包含 .NET 运行时）
 *      - BepInEx/core/ 和 BepInEx/patchers/
 *   4. 从 payloads/UnityIL2CPP/XUnityAutoTranslator 复制：
 *      - XUnity.Common.dll → BepInEx/core/
 *      - XUnity.AutoTranslator/ → BepInEx/plugins/
 *      - XUnity.ResourceRedirector/ → BepInEx/plugins/
 *   5. 复制 TMP 字体资源包和字体回退插件
 *   6. 禁用旧的 Il2Cppmscorlib.dll（避免遮挡新 interop）
 *   7. 写入 XUnity 配置（AutoTranslatorConfig.ini）
 * ---------------------------------------------------------------- */
int deploy_unity_il2cpp(const WCHAR *dir) {
    WCHAR dll[MAX_PATH * 4], pdb[MAX_PATH * 4], core[MAX_PATH * 4], il2cpp_mscorlib[MAX_PATH * 4], doorstop[MAX_PATH * 4], runtime[MAX_PATH * 4], xunity[MAX_PATH * 4], fontfix[MAX_PATH * 4], fontplugin[MAX_PATH * 4], gameasm[MAX_PATH * 4], endpoint_src[MAX_PATH * 4], endpoint_dst[MAX_PATH * 4];
    path_join(dll, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.dll");
    path_join(pdb, MAX_PATH * 4, dir, L"BepInEx\\plugins\\UnityTranslator.pdb");
    path_join(core, MAX_PATH * 4, dir, L"BepInEx\\core\\BepInEx.IL2CPP.dll");
    path_join(il2cpp_mscorlib, MAX_PATH * 4, dir, L"BepInEx\\core\\Il2Cppmscorlib.dll");
    path_join(doorstop, MAX_PATH * 4, dir, L"doorstop_config.ini");
    path_join(gameasm, MAX_PATH * 4, dir, L"GameAssembly.dll");

    /* 只支持 x64 IL2CPP 构建 */
    int machine = pe_machine(gameasm);
    if (machine && machine != 0x8664) {
        append_log(L"Unity IL2CPP：当前只内置 x64 插件运行时，已跳过非 x64 游戏。");
        return 0;
    }

    /* 如果存在旧的 Mono 版插件，禁用它避免冲突 */
    if (is_bundled_unity_mono_plugin(dll) && disable_existing_file(dll)) {
        append_log(L"Unity IL2CPP：已禁用旧的 Mono UnityTranslator.dll：%s.disabled", dll);
        disable_existing_file(pdb);
    } else if (exists_path(dll)) {
        append_log(L"Unity IL2CPP：保留现有 UnityTranslator.dll（不是内置 Mono 模板）。");
    }

    /* 查找 IL2CPP payload 目录 */
    if (!find_il2cpp_payload(runtime, MAX_PATH * 4, L"BepInExRuntime")) {
        append_log(L"Unity IL2CPP：找不到 BepInEx IL2CPP payload。");
        log_payload_install_command(L"-UnityIL2CPP");
        return 0;
    }
    if (!find_il2cpp_payload(xunity, MAX_PATH * 4, L"XUnityAutoTranslator")) {
        append_log(L"Unity IL2CPP：找不到 XUnity AutoTranslator payload。");
        log_payload_install_command(L"-UnityIL2CPP");
        return 0;
    }

    /* 复制运行时和插件文件 */
    int ok = 1;
    ok &= copy_payload_file(runtime, L"doorstop_config.ini", dir);
    ok &= copy_payload_file(runtime, L"winhttp.dll", dir);
    ok &= copy_payload_file(runtime, L".doorstop_version", dir);
    ok &= copy_payload_tree(runtime, L"dotnet", dir);
    ok &= copy_payload_tree(runtime, L"BepInEx\\core", dir);
    ok &= copy_payload_tree(runtime, L"BepInEx\\patchers", dir);
    ok &= copy_payload_file(xunity, L"BepInEx\\core\\XUnity.Common.dll", dir);
    ok &= copy_payload_tree(xunity, L"BepInEx\\plugins\\XUnity.AutoTranslator", dir);
    ok &= copy_payload_tree(xunity, L"BepInEx\\plugins\\XUnity.ResourceRedirector", dir);
    path_join(endpoint_src, MAX_PATH * 4, g_root, L"payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll");
    path_join(endpoint_dst, MAX_PATH * 4, dir, L"BepInEx\\plugins\\XUnity.AutoTranslator\\Translators\\DeepSeekTranslate.dll");
    if (exists_path(endpoint_src)) {
        ok &= copy_file_safe(endpoint_src, endpoint_dst);
    } else {
        append_log(L"Unity IL2CPP: missing DeepSeek XUnity endpoint payload (payloads\\UnityIL2CPP\\DeepSeekXUnityTranslator\\DeepSeekTranslate.dll).");
        append_log(L"Download the full program package or run build_native.bat before deploying Unity IL2CPP.");
        ok = 0;
    }
    if (find_il2cpp_payload(fontfix, MAX_PATH * 4, L"TMPFontAssetBundles")) {
        ok &= copy_payload_tree(fontfix, L"BepInEx\\font", dir);
    } else {
        append_log(L"Unity IL2CPP: TMP font asset bundle payload missing; Chinese TMP glyphs may show as boxes.");
    }
    if (find_il2cpp_payload(fontplugin, MAX_PATH * 4, L"DeepSeekTMPFontFallback")) {
        ok &= copy_payload_tree(fontplugin, L"BepInEx\\plugins\\DeepSeekTMPFontFallback", dir);
    } else {
        append_log(L"Unity IL2CPP: TMP font fallback plugin payload missing; Chinese TMP glyphs may show as boxes.");
    }

    if (!ok) {
        append_log(L"Unity IL2CPP：插件运行时部署不完整，请检查 payloads\\UnityIL2CPP。");
        log_payload_install_command(L"-UnityIL2CPP -Force");
        return 0;
    }

    /* 禁用旧的 Il2Cppmscorlib.dll，避免遮挡新 interop 层 */
    if (exists_path(il2cpp_mscorlib) && disable_existing_file(il2cpp_mscorlib)) {
        append_log(L"Unity IL2CPP：已禁用旧的 core\\Il2Cppmscorlib.dll，避免遮挡新 interop。");
    }

    /* 生成 XUnity 配置文件 */
    write_xunity_config(dir);
    append_log(L"Unity IL2CPP: deployed TMP Chinese system font fallback.");
    append_log(L"Unity IL2CPP：已部署 BepInEx be.755 + XUnity AutoTranslator。");
    append_log(L"Unity IL2CPP：XUnity 已配置为使用本地 DeepSeek 批量端点 http://127.0.0.1:19999。");
    (void)core;
    (void)doorstop;
    return 1;
}
