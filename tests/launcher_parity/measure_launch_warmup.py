"""源链接 C/C# 预热流程的阶段测量；仅使用自有目录和动态端口，保留原始请求。"""
import argparse, hashlib, json, os, pathlib, struct, subprocess, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORK = None
C = ROOT / "native/src/launcher"
CS = ROOT / "native/src/launcher_cs"

def replace_once(text, old, new):
    assert text.count(old) == 1, ("instrumentation seam changed", old, text.count(old))
    return text.replace(old, new, 1)

def run(command, **kwargs):
    result = subprocess.run([str(x) for x in command], capture_output=True, timeout=120, **kwargs)
    if result.returncode:
        raise RuntimeError((result.returncode, result.stdout.decode("utf-8", "replace"), result.stderr.decode("utf-8", "replace")))
    return result

def build():
    gen = WORK / "probe"
    gen.mkdir(exist_ok=True)
    text = (C / "warmup.c").read_text(encoding="utf-8-sig")
    text = text.replace('#include "../core/mixed_text.h"', '#include "' + (ROOT / "native/src/core/mixed_text.h").as_posix() + '"')
    text = 'extern int probe_port;\nextern void probe_phase(const char *name);\n' + text
    text = replace_once(text, 'L"127.0.0.1", 19999, 0)', 'L"127.0.0.1", (INTERNET_PORT)probe_port, 0)')
    assert '19999' not in text, 'measurement must not connect to the user service'
    text = replace_once(text, '    scan_unity_assets(dir, &prefetch);', '    probe_phase("translation_files");\n    scan_unity_assets(dir, &prefetch);\n    probe_phase("resource_scan");')
    text = replace_once(text, '            for (size_t i = 0; i < prefetch.n; i += WARMUP_BATCH_ITEMS)', '            probe_phase("ready_import");\n            for (size_t i = 0; i < prefetch.n; i += WARMUP_BATCH_ITEMS)')
    text = replace_once(text, '    warmup_scan_rpgm_resources(dir, &prefetch);', '    warmup_scan_rpgm_resources(dir, &prefetch);\n    probe_phase("resource_scan");')
    (gen / "warmup.c").write_text(text,encoding="utf-8")
    flow = (C/"ui.c").read_text(encoding="utf-8-sig")
    flow = flow[flow.index("int run_engine_launch_flow("):flow.index("/* 本地翻译服务未就绪")]
    native = r'''
#include "warmup.h"
#include "fsutil.h"
#include "ui.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
int probe_port;
static LARGE_INTEGER initial, frequency;
void probe_phase(const char *name) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    printf("phase=%s|%.6f\n", name, (double)(now.QuadPart-initial.QuadPart)*1000.0/frequency.QuadPart);
}
void write_stdout_utf8(const WCHAR *s) {
    char bytes[16384];
    int n=WideCharToMultiByte(CP_UTF8,0,s,-1,bytes,sizeof bytes,NULL,NULL);
    if(n>0) fputs(bytes,stdout);
}
void append_log(const WCHAR *fmt, ...) {
    WCHAR line[4096]; va_list args; va_start(args,fmt);
    _vsnwprintf(line,4095,fmt,args); va_end(args); line[4095]=0;
    write_stdout_utf8(line); fputc('\n',stdout);
}
void set_status(const WCHAR *text) { write_stdout_utf8(text); fputc('\n',stdout); }
static int g_godot_runtime_verified;
static int godot_patch_source_current(const WCHAR *d,const WCHAR *p) {(void)d;(void)p;abort();}
static int start_godot_patch_worker(const WCHAR *d) {(void)d;abort();}
static int launch_game_for_engine(const WCHAR *dir,Engine engine) {
    (void)dir;(void)engine;probe_phase("launch");return 1;
}
/* FLOW */
int wmain(int argc,WCHAR **argv) {
    if(argc!=4)return 2;
    probe_port=_wtoi(argv[3]);
    Engine engine=(Engine)_wtoi(argv[2]);
    if(engine!=ENGINE_UNITY && engine!=ENGINE_RPGM_MV)return 2;
    QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&initial);
    int ok=run_engine_launch_flow(argv[1],engine);
    probe_phase("done");
    return ok?0:1;
}
'''
    (gen/"probe.c").write_text(native.replace("/* FLOW */",flow),encoding="utf-8")
    gcc=ROOT/"native/toolchain/w64devkit/bin/gcc.exe"
    env=dict(os.environ,PATH=str(gcc.parent)+os.pathsep+os.environ["PATH"])
    command=[gcc,"-std=c17","-O2","-Wall","-Wextra","-Werror","-municode","-D_CRT_SECURE_NO_WARNINGS","-I"+str(C),
        gen/"probe.c",gen/"warmup.c",C/"godot_warmup.c",C/"fsutil.c",C/"globals.c",C/"engine.c","-lwinhttp","-lgdi32","-o",gen/"c.exe"]
    result=run(command,env=env)
    (WORK/"build-c.log").write_bytes(result.stdout+result.stderr)
    for file in CS.glob("*.cs"):
        text=file.read_text(encoding="utf-8-sig")
        if file.name=="Warmup.cs":
            assert text.count("Contract.DefaultBaseUrl") == 3
            text=text.replace("Contract.DefaultBaseUrl", '("http://127.0.0.1:" + WarmupMeasure.Port)')
            assert "Contract.DefaultBaseUrl" not in text and "19999" not in text
        if file.name=="WarmupUnity.cs":
            text=replace_once(text,"            ScanUnityAssets(dir, prefetch);",'            WarmupMeasure.Phase("translation_files");\n            ScanUnityAssets(dir, prefetch);\n            WarmupMeasure.Phase("resource_scan");')
            text=replace_once(text,"                    for (int i = 0; i < prefetch.Count; i += BatchItems)",'                    WarmupMeasure.Phase("ready_import");\n                    for (int i = 0; i < prefetch.Count; i += BatchItems)')
        if file.name=="WarmupRpgm.cs":
            text=replace_once(text,"            ScanRpgmResources(dir, prefetch);",'            ScanRpgmResources(dir, prefetch);\n            WarmupMeasure.Phase("resource_scan");')
        (gen/file.name).write_text(text,encoding="utf-8")
    managed = r'''
using System;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using DstLauncher;
internal static class WarmupMeasure {
    internal static int Port;
    static Stopwatch watch;
    internal static void Phase(string name) { Console.WriteLine("phase="+name+"|"+watch.Elapsed.TotalMilliseconds.ToString("F6",CultureInfo.InvariantCulture)); }
    static int Main(string[] args) {
        if(args.Length!=3)return 2;
        Port=int.Parse(args[2]);
        Console.OutputEncoding=new UTF8Encoding(false);
        Log.SetSink(Console.WriteLine);
        Log.SetStatusSink(Console.WriteLine);
        LaunchFlow.SpawnSink=(kind,exe,cmd,cwd)=>Phase("launch");
        watch=Stopwatch.StartNew();
        bool ok=LaunchFlow.RunEngineLaunchFlow(args[0],(Engine)int.Parse(args[1]));
        Phase("done");return ok?0:1;
    }
}
namespace DstLauncher { internal static class BuildInfo { internal const string TranslatorVersion="measurement"; } }
'''
    (gen/"WarmupMeasure.cs").write_text(managed,encoding="utf-8")
    project = """<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net472</TargetFramework><OutputType>Exe</OutputType><StartupObject>WarmupMeasure</StartupObject><LangVersion>latest</LangVersion><AssemblyName>cs</AssemblyName><DebugType>none</DebugType></PropertyGroup><ItemGroup><Compile Include="CORE/*.cs" /></ItemGroup></Project>"""
    (gen/"probe.csproj").write_text(project.replace("CORE",(ROOT/"native/src/core").as_posix()),encoding="utf-8")
    result=run(["dotnet","build",gen/"probe.csproj","-c","Release","--nologo","-v","quiet"])
    (WORK/"build-cs.log").write_bytes(result.stdout+result.stderr)
    return gen/"c.exe",gen/"bin/Release/net472/cs.exe"

class Server(ThreadingHTTPServer):
    daemon_threads=True
    def __init__(self):
        super().__init__(("127.0.0.1",0),Handler)
        self.requests=[]
        self.hits={}
        self.mode="pending"
        self.delay=0
        self.disconnects=0
        self.errors=[]

    def handle_error(self, request, client_address):
        import traceback
        self.errors.append(traceback.format_exc())

class Handler(BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    def log_message(self,*args):
        return # 请求逐条记入 server.requests，避免混入测量 stdout。
    def setup(self):
        super().setup()
        import socket
        self.connection.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
    def handle_one_request(self):
        try:
            super().handle_one_request()
        except (ConnectionResetError,ConnectionAbortedError):
            self.server.disconnects+=1
            self.close_connection=True
    def do_GET(self):
        assert self.path == '/health', self.path
        self.server.requests.append({"path":self.path,"body":None})
        self.reply({"status":"ok"})
    def do_POST(self):
        raw=self.rfile.read(int(self.headers["Content-Length"]))
        data=json.loads(raw)
        self.server.requests.append({"path":self.path,"body":data,"raw_sha256":hashlib.sha256(raw).hexdigest()})
        if self.server.delay:
            time.sleep(self.server.delay)
        if self.path=="/cache/lookup":
            hits={k:self.server.hits[k] for k in data["texts"] if k in self.server.hits}
            self.reply({"hits":hits,"hit_count":len(hits),"miss_count":len(data["texts"])-len(hits)})
        elif self.path=="/cache/import":
            for row in data["entries"]:
                self.server.hits[row["key"]]=row["value"]
            self.reply({"status":"ok","imported":len(data["entries"])})
        elif self.path=="/prefetch":
            if self.server.mode=="instant":
                for key in data["texts"]:
                    self.server.hits[key]="译："+key
            self.reply({"accepted":len(data["texts"])})
        else:
            raise AssertionError(self.path)
    def reply(self,data):
        body=json.dumps(data,ensure_ascii=False,separators=(",",":")).encode()
        self.send_response(200)
        self.send_header("Content-Length",str(len(body)))
        self.send_header("Content-Type","application/json")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (ConnectionResetError,ConnectionAbortedError,BrokenPipeError):
            self.server.disconnects+=1

def fixture(engine,scale):
    game=WORK/"fixtures"/(engine+"-"+scale)
    game.mkdir(parents=True,exist_ok=True)
    (game/"Game.exe").write_bytes(b"owned dry-run placeholder")
    count=1000 if scale=="small" else (7000 if engine=="unity" else 35000)
    resource=[f"Welcome to synthetic dialogue number {i:06d}." for i in range(count)]
    import_keys=[f"Previously translated sentence number {i:06d}." for i in range(count)] if engine=="unity" else []
    lookup_keys=[f"Cached empty sentence number {i:06d}." for i in range(50)] if engine=="unity" else []
    original=None
    if engine=="unity":
        data=game/"Game_Data";data.mkdir(exist_ok=True)
        total=8*1024*1024 if scale=="small" else 64*1024*1024
        payload=b"".join(struct.pack("<I",len(s))+s.encode()+b"\0"*4 for s in resource)
        (data/"resources.assets").write_bytes(payload+b"\0"*(total-len(payload)))
        trans=game/"BepInEx/Translation/zh-CN/Text/dialogue.txt"
        trans.parent.mkdir(parents=True,exist_ok=True)
        original=("\n".join(k+"=已有译文："+str(i) for i,k in enumerate(import_keys))+"\n"+"\n".join(k+"=" for k in lookup_keys)+"\n").encode()
        trans.write_bytes(original)
    else:
        for relative in ("www/js","www/data"):
            (game/relative).mkdir(parents=True,exist_ok=True)
        (game/"www/index.html").write_text("<html></html>")
        (game/"www/js/rpg_core.js").write_text("// owned fixture")
        (game/"www/js/main.js").write_text("// owned fixture")
        (game/"www/data/System.json").write_text("{}")
        (game/"www/data/Items.json").write_text(json.dumps([None]+[{"name":s} for s in resource]),encoding="utf-8")
    return game,resource,import_keys,lookup_keys,original

def main():
    global WORK
    parser=argparse.ArgumentParser()
    parser.add_argument("--rounds",type=int,default=5)
    parser.add_argument("--output",type=pathlib.Path)
    args=parser.parse_args()
    if not 1 <= args.rounds <= 30:
        parser.error('--rounds must be 1..30')
    # 指定输出必须是尚不存在的目录；不覆盖历史夹具，也不清理任意路径。
    if args.output:
        WORK=args.output.resolve()
        WORK.mkdir(parents=True,exist_ok=False)
    else:
        WORK=pathlib.Path(tempfile.mkdtemp(prefix='dst_launch_warmup_'))
    print('evidence='+str(WORK),flush=True)
    exes=build()
    server=Server()
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    rows=[]; request_reference={}
    try:
        for engine in ("unity","rpgm"):
            for scale in ("small","large"):
                game,resources,imports,lookups,original=fixture(engine,scale)
                for mode,delay in (("pending",0),("cached",0),("instant",0),("pending",.005)):
                    for repeat in range(args.rounds):
                        for language,exe in zip(("c","cs"),exes):
                            if original is not None:
                                trans=game/"BepInEx/Translation/zh-CN/Text/dialogue.txt"
                                trans.write_bytes(original)
                            before={p.relative_to(game).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in game.rglob("*") if p.is_file() and ".deepseek.bak" not in p.name}
                            server.hits={k:"已缓存："+k for k in lookups}
                            if mode=="cached": server.hits.update({k:"已缓存："+k for k in resources})
                            initial_hits=dict(server.hits)
                            server.mode=mode;server.delay=delay;server.requests=[];server.disconnects=0;server.errors=[]
                            result=run([exe,game,3 if engine=="unity" else 2,server.server_port])
                            stdout=result.stdout.decode("utf-8-sig")
                            phases=dict((line[6:].split("|")[0],float(line.split("|")[1])) for line in stdout.splitlines() if line.startswith("phase="))
                            assert phases["launch"]>=phases["resource_scan"]
                            transcript=list(server.requests)
                            assert not server.errors,server.errors
                            (WORK/('latest-'+language+'-requests.json')).write_text(json.dumps(transcript,ensure_ascii=False,indent=2),encoding='utf-8')
                            for p in game.rglob("*"):
                                relative=p.relative_to(game).as_posix()
                                if p.is_file() and relative in before and p.name!="dialogue.txt":
                                    assert hashlib.sha256(p.read_bytes()).hexdigest()==before[relative],relative
                            if original is not None:
                                assert trans.with_name("dialogue.txt.deepseek.bak").read_bytes()==original
                                filled=original.decode()
                                for key in lookups:
                                    filled=filled.replace(key+'=\n',key+'=已缓存：'+key+'\n')
                                assert trans.read_bytes()==filled.encode()
                            identity=(engine,scale,mode,delay)
                            if identity not in request_reference:request_reference[identity]=transcript
                            else: assert request_reference[identity]==transcript,(identity,language,"C/C# request drift")
                            queue=[r for r in transcript if r["path"]=="/prefetch"]
                            queued_keys={k for r in queue for k in r["body"]["texts"]}
                            assert set(resources)<=queued_keys
                            sample=imports[:50]+lookups[:50]+resources[:50]
                            hits_at_launch=sum(k in server.hits for k in sample)
                            expected_hits=len(sample) if mode in ('cached','instant') else (100 if engine=='unity' else 0)
                            assert hits_at_launch==expected_hits,(engine,scale,mode,hits_at_launch)
                            imported_count=sum(len(r["body"]["entries"]) for r in transcript if r["path"]=="/cache/import")
                            assert imported_count==min(1200,len(imports)+len(lookups))
                            if len(imports)+len(lookups)>1200:
                                assert '1200' in stdout and '上限' in stdout
                            row={"engine":engine,"scale":scale,"mode":mode,"ack_delay_ms":delay*1000,"round":repeat,"language":language,"phases_ms":phases,
                                "resource_bytes":sum(p.stat().st_size for p in game.rglob("*") if p.is_file() and p.suffix in (".json",".assets")),
                                "imported":imported_count,"queued_unique":len(queued_keys),"prefetch_requests":len(queue),
                                "initial_sample_hits":sum(k in initial_hits for k in sample),"launch_sample_hits":hits_at_launch,"sample_size":len(sample),
                                "request_sha256":hashlib.sha256(json.dumps(transcript,ensure_ascii=False,separators=(",",":")).encode()).hexdigest(),
                                "source_files_unchanged_except_expected_xunity_fill":True}
                            rows.append(row)
                            if repeat==0:
                                logname=f"{engine}-{scale}-{mode}-{int(delay*1000)}-{language}"
                                (WORK/(logname+".log")).write_bytes(result.stdout+result.stderr)
                                (WORK/(logname+"-requests.json")).write_text(json.dumps(transcript,ensure_ascii=False),encoding="utf-8")
                            print(json.dumps(row),flush=True)
    finally:
        server.shutdown();server.server_close();thread.join()
        (WORK/"samples.json").write_text(json.dumps(rows,indent=2),encoding="utf-8")
    sources=list(C.glob('*.c'))+list(C.glob('*.h'))+list(CS.glob('*.cs'))+list((ROOT/'native/src/core').glob('*'))+[pathlib.Path(__file__).resolve()]
    manifest={str(p.relative_to(ROOT)).replace('\\','/'):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources if p.is_file()}
    (WORK/'source-hashes.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print("MEASUREMENT_COMPLETE",flush=True)
if __name__=="__main__":
    main()
