"""源链接真实收包循环；只替换 socket 读取/发送和内存池边界。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent

def method(source, name):
    match = re.search(r'^[ \t]*(?:private|internal|public|static)[^\n;=]*\b'+re.escape(name)+r'\s*\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    start=source.index('{',match.start());depth=1;i=start+1;quote=None
    while depth:
        ch=source[i]
        if quote:
            if ch=='\\': i+=2;continue
            if ch==quote: quote=None
        elif ch in ('"',"'"):quote=ch
        elif source.startswith('//',i):i=source.index('\n',i);continue
        elif source.startswith('/*',i):i=source.index('*/',i+2)+2;continue
        elif ch=='{':depth+=1
        elif ch=='}':depth-=1
        i+=1
    return source[match.start():i]

def run(command, **kw):
    result=subprocess.run([str(x) for x in command],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,**kw)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors='replace'))
    return result.stdout.decode(errors='replace')

def build(out, native, managed):
    c=native.read_text(encoding='utf-8-sig')
    cs=managed.read_text(encoding='utf-8-sig')
    methods=method(c,'mem_ieq')+'\n'+method(c,'parse_content_length').replace('static const char name[]', 'parse_calls++;\n    static const char name[]',1)
    if 'static char *find_header_end(' in c:
        scanner=method(c,'find_header_end').replace('{','{ search_calls++;',1)
        scanner=scanner.replace('while (', 'while (',1).replace('if (req[i] ==', 'search_bytes++;\n        if (req[i] ==',1)
        methods+='\n'+scanner
    receiver=method(c,'serve_one')
    body_start=receiver.index('    char *body =')
    body_setup=receiver[body_start:receiver.index('    Buf out;',body_start)]
    receiver=receiver[:receiver.index('    char *headers_end =')]
    receiver=receiver.replace('serve_one','probe_receive',1)
    receiver+=body_setup.replace('body','end')+r'''
    if(!end)abort();
    body_len=n-(size_t)(end-req);
    body_hash=14695981039346656037ULL;
    for(size_t i=0;i<body_len;i++) { body_hash^=(unsigned char)end[i];body_hash*=1099511628211ULL; }
    request_buffer_release(req,cap);
}
'''
    (out/'probe.c').write_text((HERE/'Probe.c.txt').read_text().replace('@METHODS@',methods).replace('@RECEIVER@',receiver),encoding='utf-8')
    methods=method(cs,'ParseContentLength').replace('{','{ ParseCalls++;',1)
    framing=ROOT/'native/src/core/HttpRequestFraming.cs'
    if 'HttpRequestFraming.FindHeaderEnd' in cs:
        scanner=method(framing.read_text(encoding='utf-8-sig'),'FindHeaderEnd').replace('{','{ Probe.SearchCalls++;',1)
        scanner=scanner.replace('if (buffer[i] ==', 'Probe.SearchBytes++;\n                if (buffer[i] ==',1)
        methods+='\nprivate static class HttpRequestFraming {\n'+scanner+'\n}\n'
    receiver=method(cs,'ReadRequestAsync')
    receiver=receiver[:receiver.index('                    // ---- 解析 Origin')]
    receiver=receiver.replace('Task<Request>','Task<Result>',1)
    receiver+=r'''
                    ulong hash=14695981039346656037UL;
                    for(int i=0;i<(int)contentLength;i++) { hash^=buffer[headerEnd+i];hash*=1099511628211UL; }
                    return new Result { Length=(int)contentLength, Hash=hash };
                }
            } finally { if(poolSafe) PoolRelease(buffer); }
        }
'''
    (out/'Probe.cs').write_text((HERE/'Probe.cs.txt').read_text(encoding='utf-8').replace('@METHODS@',methods).replace('@RECEIVER@',receiver),encoding='utf-8')
    gcc=ROOT/'native/toolchain/w64devkit/bin/gcc.exe'
    env=os.environ.copy();env['PATH']=str(gcc.parent)+os.pathsep+env['PATH']
    run([gcc,'-std=c17','-O2','-Wall','-Wextra','-Werror',out/'probe.c','-o',out/'probe-c.exe'],env=env)
    # 系统 csc 不支持生产源中的 out var；使用当前 dotnet 编译器和 net472。
    project='<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net472</TargetFramework><LangVersion>latest</LangVersion><EnableDefaultCompileItems>false</EnableDefaultCompileItems><AssemblyName>probe-cs</AssemblyName></PropertyGroup><ItemGroup><Compile Include="Probe.cs" /></ItemGroup></Project>'
    (out/'probe.csproj').write_text(project)
    run(['dotnet','build',out/'probe.csproj','-c','Release','-nologo','-v','quiet','-p:OutputPath='+str(out/'cs')+'\\'])
    return [out/'probe-c.exe',out/'cs/probe-cs.exe']

def fnv(data):
    value=14695981039346656037
    for byte in data:value=((value^byte)*1099511628211)&((1<<64)-1)
    return value

def cases():
    def request(body=b'',extra=b''):
        return b'POST /cache/lookup HTTP/1.1\r\nHost: localhost\r\n'+extra+b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body
    yield 'short',request(b'{"texts":["Hello world."]}'),200,b'{"texts":["Hello world."]}',[1,7,64,65536]
    body=b' '+b'{"texts":["<color=red>Score: {0}</color>","RenPy %(name)s","RPGM \\\\V[1]","Godot %s \xe4\xb8\xad\xe6\x96\x87"]}'
    yield 'unicode-markup',request(body),200,body,[1,3,7,64]
    body=b'x'*131072
    yield 'fragmented-body',request(body),200,body,[7,64,4096,65536]
    yield 'long-header',request(b'{}',b'X-Fixture: '+b'x'*32768+b'\r\n'),200,b'{}',[1,7,4096,65536]
    yield 'delimiter-prefixes',request(b'{}',b'X-Fixture: '+b'\r\r\n'*100+b'z\r\n'),200,b'{}',[1,2,3,7]
    for name,raw in [
        ('missing-length',b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n'),
        ('zero-length',request()),
    ]:yield name,raw,200,b'',[1,7,4096]
    for name,extra in [('negative',b'Content-Length: -1'),('text',b'Content-Length: xyz'),
        ('duplicate',b'Content-Length: 0\r\nContent-Length: 0'),('overflow',b'Content-Length: 999999999999999999999999')]:
        yield name,b'POST / HTTP/1.1\r\n'+extra+b'\r\n\r\n',400,None,[1,7,4096]
    yield 'oversize-declared',b'POST / HTTP/1.1\r\nContent-Length: 2097152\r\n\r\n',413,None,[1,7,4096]
    yield 'incomplete-header',b'GET /health HTTP/1.1\r\nHost: x\r\n',400,None,[1,7,4096]
    yield 'incomplete-body',b'POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nab',400,None,[1,7,4096]
    # 精确 2 MiB 含头部，以及只差一个字节的超限。
    header=b'POST / HTTP/1.1\r\nContent-Length: 2097108\r\n\r\n'
    size=2097152-len(header)
    header=b'POST / HTTP/1.1\r\nContent-Length: '+str(size).encode()+b'\r\n\r\n'
    assert len(header)+size==2097152
    body=b'x'*size
    yield 'exact-limit',header+body,200,body,[7,65535,65536]
    yield 'unterminated-limit',b'x'*2097152,413,None,[4096,65536]
    yield 'trailing-request',request(b'{}')+b'GET /health HTTP/1.1\r\n\r\n',200,b'{}',[1,7,65536]
    yield 'header-nul',b'GET /health HTTP/1.1\r\nX-Test: a\0b\r\n\r\n',{'c':400,'cs':200},b'',[1,7,65536]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path)
    parser.add_argument('--native-source',type=Path,default=ROOT/'native/src/server/http.c')
    parser.add_argument('--managed-source',type=Path,default=ROOT/'native/src/server_cs/Program.cs')
    parser.add_argument('--baseline',action='store_true')
    args=parser.parse_args()
    out=(args.output or Path(tempfile.mkdtemp(prefix='dst_http_framing_'))).resolve()
    out.mkdir(parents=True,exist_ok=True)
    binaries=build(out,args.native_source,args.managed_source)
    results=[];failures=[];checks=0
    for name,raw,expected,body,chunks in cases():
        path=out/(name+'.http');path.write_bytes(raw)
        for chunk in chunks:
            for language,binary in zip(('c','cs'),binaries):
                rounds=1000 if name=='short' else 1
                result=json.loads(run([binary,path,chunk,rounds]))
                wanted=expected.get(language) if isinstance(expected,dict) else expected
                assert result['status']==wanted,(name,chunk,language,result)
                if wanted==200:
                    assert result['bodyBytes']==len(body) and result['bodyHash']==fnv(body),(name,language,result)
                    checks+=1
                checks+=1
                # 检查收包循环的解析工作；不是网络耗时阈值。
                if name in ('short','long-header','fragmented-body','exact-limit'):
                    bounded=result['parseCalls']==1
                    if language=='c':bounded &= result['searchBytes']<=len(raw)+4
                    else:
                        bounded &= result['decodedBytes']<=raw.index(b'\r\n\r\n')+4
                        bounded &= result['searchBytes']<=raw.index(b'\r\n\r\n')+4
                    if not bounded:failures.append((name,chunk,language))
                result.update(scenario=name,chunk=chunk,language=language,requestBytes=len(raw),rounds=rounds)
                results.append(result)
    (out/'results.json').write_text(json.dumps({'checks':checks,'budgetFailures':failures,'samples':results},indent=2))
    sources=[args.native_source.resolve(),args.managed_source.resolve()]
    if 'HttpRequestFraming.FindHeaderEnd' in args.managed_source.read_text(encoding='utf-8-sig'):
        sources.append(ROOT/'native/src/core/HttpRequestFraming.cs')
    (out/'sources.json').write_text(json.dumps({str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},indent=2))
    print(json.dumps({'checks':checks,'budgetFailures':len(failures),'samples':len(results),'output':str(out)}))
    if failures and not args.baseline:raise AssertionError('unbounded framing work: '+str(failures))

if __name__=='__main__':main()
