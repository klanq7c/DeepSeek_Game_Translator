"""源链接 C 校验器，并执行真实 C# 场景收集、格式校验和资源覆盖链。"""
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
CASES = [
    ('[url=buy]Buy potion[/url] for %d coins', '花费 %d 金币购买药水', False),
    ('[url=buy]Buy potion[/url] for %d coins', '[url=sell]购买药水[/url]，%d金币', False),
    ('[url=buy][b]Buy potion[/b][/url] for %d coins', '[url=buy][b]购买药水[/b][/url]，%d金币', True),
    ('[color=red]Warning[/color]', '[color=blue]警告[/color]', False),
    ('Hello traveler', '[b]你好旅行者[/b]', False),
    ('[custom_fx strength=2]Hello[/custom_fx]', '你好', False),
    ('[custom_fx]Hello[/custom_fx]', '你好[/custom_fx]', False),
    ('Use [lb] and [rb] to show brackets', '使用 [lb] 和 [rb] 显示括号', True),
    ('Choose [a longer phrase] from [100] entries', '从 [100] 项中选择 [较长的短语]', True),
    ('You have %d coins', '你拥有金币', False),
    ('You have %d coins', '你拥有 %d 金币', True),
]


def c_method(source, name):
    match = re.search(r'^static [^;\n]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    i, depth, quote = match.end(), 1, None
    while depth:
        ch = source[i]
        if quote:
            if ch == '\\':
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in ('"', "'"):
            quote = ch
        elif source.startswith('//', i):
            i = source.index('\n', i)
            continue
        elif source.startswith('/*', i):
            i = source.index('*/', i) + 2
            continue
        elif ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        i += 1
    return source[match.start():i] + '\n'


def main():
    out = ROOT / 'build/godot_text_format'
    out.mkdir(parents=True, exist_ok=True)
    gcc = shutil.which('gcc') or str(ROOT / 'native/toolchain/w64devkit/bin/gcc.exe')
    os.environ['PATH'] = str(Path(gcc).parent) + os.pathsep + os.environ['PATH']
    production = (ROOT / 'native/src/launcher/godot_patch.c').read_text(encoding='utf-8')
    c = '#include <stdio.h>\n#include <string.h>\n'
    for name in ('count_substr', 'godot_next_bbcode', 'godot_text_translation_preserves_format'):
        if name == 'godot_next_bbcode' and name not in production:
            continue
        c += c_method(production, name)
    c += 'int main(void) { int failed = 0;\n'
    for index, (source, translated, expected) in enumerate(CASES):
        c += ('if (godot_text_translation_preserves_format(%s, %s) != %d) '
              '{ printf("FAIL native case %d\\n"); failed++; }\n') % (
                  json.dumps(source, ensure_ascii=False), json.dumps(translated, ensure_ascii=False), expected, index)
    c += 'return failed ? 1 : 0; }\n'
    (out / 'probe.c').write_text(c, encoding='utf-8')
    subprocess.run([gcc, '-std=c17', '-O2', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    str(out / 'probe.c'), '-o', str(out / 'native.exe')], check=True)
    native = subprocess.run([str(out / 'native.exe')]).returncode

    cs = '''using System; using System.Reflection; using System.Text;
class Probe {
 static int Main(string[] args) {
  var type = Assembly.LoadFrom(args[0]).GetType("DstLauncher.GodotPatch", true);
  var flags = BindingFlags.Static | BindingFlags.NonPublic;
  var validate = type.GetMethod("TextTranslationPreservesFormat", flags);
  var collect = type.GetMethod("CollectSceneTextPatches", flags);
  var build = type.GetMethod("BuildTextResourceOverride", flags);
  int failed = 0;
  foreach (var row in Cases) {
   string source = row[0], translated = row[1]; bool expected = row[2] == "True";
   // C# 启动器的资源管道使用一字符一字节的 UTF-8 表示。
   Func<string,string> bytes = s => Encoding.GetEncoding(28591).GetString(Encoding.UTF8.GetBytes(s));
   string scene = "[gd_scene format=3]\\n[node name=\\\"Menu\\\" type=\\\"RichTextLabel\\\"]\\nbbcode_enabled = true\\ntext = \\\"" + source + "\\\"\\n";
   var texts = Activator.CreateInstance(type.GetNestedType("StrList", BindingFlags.NonPublic));
   var patches = Activator.CreateInstance(type.GetNestedType("TextPatchList", BindingFlags.NonPublic));
   collect.Invoke(null, new object[] { scene, scene.Length, texts, patches });
   object[] call = { scene, scene.Length, patches, new[] { bytes(translated) }, texts, 0 };
   var result = (byte[])build.Invoke(null, call);
   bool accepted = result != null && (int)call[5] == 1;
   bool valid = (bool)validate.Invoke(null, new object[] { bytes(source), bytes(translated) });
   if (accepted != expected || valid != expected) { Console.WriteLine("FAIL managed " + source); failed++; }
  }
  return failed == 0 ? 0 : 1;
 }
 static string[][] Cases = new string[][] {
'''
    for source, translated, expected in CASES:
        cs += 'new string[] {' + ','.join(json.dumps(s, ensure_ascii=False) for s in (source, translated, str(expected))) + '},\n'
    cs += '}; }\n'
    (out / 'Probe.cs').write_text(cs, encoding='utf-8-sig')
    csc = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    subprocess.run([str(csc), '/nologo', '/out:' + str(out / 'managed.exe'), str(out / 'Probe.cs')], check=True)
    managed = subprocess.run([str(out / 'managed.exe'), str(ROOT / 'build/launcher_cs/dst_launcher_cs.exe')]).returncode
    assert native == 0 and managed == 0, (native, managed)
    print(f'[PASS] Godot text format: {len(CASES)} native/managed cases, real scene overrides')


if __name__ == '__main__':
    main()
