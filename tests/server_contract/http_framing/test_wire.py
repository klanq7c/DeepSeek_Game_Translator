"""当前 C/C# 程序的真实 TCP 边界；仅使用自有端口、缓存和进程。"""
from pathlib import Path
import argparse
import json
import socket
import sys
import tempfile
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from test_api_boundaries import ROOT, server

MAX=2*1024*1024
def request(body=b'',extra=b'',path='/health'):
    method=b'GET ' if path=='/health' else b'POST '
    return method+path.encode()+b' HTTP/1.1\r\nHost: localhost\r\n'+extra+b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body

def send(port,raw,chunk,close_send=True):
    with socket.create_connection(('127.0.0.1',port),timeout=9) as client:
        client.settimeout(9)
        client.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        try:
            for start in range(0,len(raw),chunk):client.sendall(raw[start:start+chunk])
            if close_send:client.shutdown(socket.SHUT_WR)
        except (BrokenPipeError,ConnectionResetError) as error:
            raise AssertionError('server closed before fixture submission completed') from error
        data=bytearray()
        while True:
            part=client.recv(65536)
            if not part:break
            data.extend(part)
    head,body=bytes(data).split(b'\r\n\r\n',1)
    length=int(next(line.split(b':',1)[1] for line in head.split(b'\r\n') if line.lower().startswith(b'content-length:')))
    assert len(body)==length,'response byte length differs from Content-Length'
    return int(head.split(b' ')[1]),json.loads(body)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    directory=(args.output or Path(tempfile.mkdtemp(prefix='dst_http_wire_'))).resolve()
    directory.mkdir(parents=True,exist_ok=True)
    rows=[]
    for binary in (ROOT/'native/dst_server.exe',ROOT/'native/dst_server_cs.exe'):
        work=directory/binary.stem;work.mkdir(exist_ok=True)
        with server(binary,work,'http://127.0.0.1:1/v1/chat/completions') as (port,process):
            tests=[
                ('short',request(),200,[1,3,7,65536]),
                ('header-prefix',request(extra=b'X-Fixture: '+b'\r\r\n'*100+b'z\r\n'),200,[1,3,7]),
                ('long-header',request(extra=b'X-Fixture: '+b'x'*131072+b'\r\n'),200,[127,4096]),
                ('body-grow',request(b' '*131072),200,[7,65536]),
                ('negative',b'POST /health HTTP/1.1\r\nContent-Length: -1\r\n\r\n',400,[1,7]),
                ('duplicate',b'POST /health HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n',400,[1,7]),
                ('overflow',b'POST /health HTTP/1.1\r\nContent-Length: 999999999999999999999999\r\n\r\n',400,[1,7]),
                ('oversize-declared',b'POST /health HTTP/1.1\r\nContent-Length: 2097152\r\n\r\n',413,[1,7]),
                ('incomplete-header',b'GET /health HTTP/1.1\r\nHost: localhost\r\n',400,[1,7]),
                ('incomplete-body',b'POST /health HTTP/1.1\r\nContent-Length: 4\r\n\r\nab',400,[1,7]),
                ('untrusted-origin',request(extra=b'Origin: https://fixture.invalid\r\n'),403,[1,7]),
                ('trusted-origin',request(extra=b'Origin: null\r\n'),200,[1,7]),
            ]
            size=MAX-len(request(b'x'*MAX))+MAX
            raw=request(b' '*size)
            assert len(raw)==MAX
            tests.append(('exact-limit',raw,200,[4093,65536]))
            tests.append(('over-limit',request(b' '*(size+1)),413,[65536]))
            # 超限声明会提前回应，发送完整多余正文可能被 RST；只发送声明足以覆盖判定。
            tests[-1]=('over-limit',tests[-1][1].split(b'\r\n\r\n')[0]+b'\r\n\r\n',413,[7])
            for name,raw,wanted,chunks in tests:
                for chunk in chunks:
                    code,body=send(port,raw,chunk)
                    assert code==wanted,(binary.name,name,chunk,code,body)
                    rows.append({'binary':binary.name,'case':name,'chunk':chunk,'status':code,
                                 'error':body.get('error')})
            for name,raw in [('timeout-header',b'GET /health HTTP/1.1\r\n'),
                             ('timeout-body',b'POST /health HTTP/1.1\r\nContent-Length: 8\r\n\r\nab')]:
                start=time.monotonic()
                code,body=send(port,raw,len(raw),False)
                elapsed=time.monotonic()-start
                assert code==400 and body.get('error')=='incomplete_request' and 4<=elapsed<9,(name,code,body,elapsed)
                rows.append({'binary':binary.name,'case':name,'status':code,'error':body.get('error'),'seconds':elapsed})
            values={
                "RenPy %(name)s {color=#fff}Hello{/color}":"你好 %(name)s {color=#fff}世界{/color}",
                "RPGM \\V[1] \\C[2]Hello":"RPGM \\V[1] \\C[2]你好",
                "Unity <color=red>Score {0}</color>":"Unity <color=red>分数 {0}</color>",
                "Godot [color=red]Hello %s[/color]":"Godot [color=red]你好 %s[/color]",
            }
            encoded=json.dumps({'entries':[{'key':key,'value':value} for key,value in values.items()]},ensure_ascii=False).encode()
            code,body=send(port,request(encoded,path='/cache/import'),1)
            assert code==200 and body.get('accepted')==4,(code,body)
            lookup=json.dumps({'texts':list(values)},ensure_ascii=False).encode()
            for i in range(8):
                code,body=send(port,request(lookup,path='/cache/lookup'),[1,3,7,65536][i%4])
                assert code==200 and body.get('hits')==values,(code,body)
            rows.append({'binary':binary.name,'case':'post-timeout-pool-unicode-markup','status':200,'lookups':8})
            assert process.poll() is None
    (directory/'results.json').write_text(json.dumps(rows,indent=2),encoding='utf-8')
    print('HTTP wire: '+str(len(rows))+' cases passed; output='+str(directory))

if __name__=='__main__':main()
