"""损坏缓存不得覆盖有效记录；真实两版服务，拥有临时缓存与进程。"""
import argparse
import base64
import hashlib
import http.client
import json
import random
from pathlib import Path
import tempfile
from test_api_boundaries import ROOT,server,request

def encode(key,value):
    return base64.b64encode(key.encode())+b'\t'+base64.b64encode(value.encode())+b'\n'

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path)
    parser.add_argument('--baseline',action='store_true')
    args=parser.parse_args()
    output=(args.output or Path(tempfile.mkdtemp(prefix='dst_cache_decode_'))).resolve()
    output.mkdir(parents=True,exist_ok=True)
    original='应保留的原有译文。'
    bad={}
    bad['nul-key']=lambda k:encode(k+'\0different','不该覆盖。')
    bad['nul-value']=lambda k:encode(k,'错误半截\0后半截')
    bad['invalid-character']=lambda k:encode(k,'不该覆盖。').replace(b'\t',b'!\t')
    bad['missing-padding']=lambda k:encode(k,'a译文').replace(b'=',b'')
    bad['trailing-data']=lambda k:base64.b64encode(k.encode())+b'\t'+base64.b64encode(b'a')+b'AAAA\n'
    bad['leading-padding']=lambda k:base64.b64encode(k.encode())+b'\t=5Lit5paH\n'
    bad['excess-padding']=lambda k:base64.b64encode(k.encode())+b'\tYQ===\n'
    bad['short-quartet']=lambda k:base64.b64encode(k.encode())+b'\t5Li\n'
    bad['invalid-utf8']=lambda k:base64.b64encode(k.encode())+b'\t'+base64.b64encode(b'\xffbroken text')+b'\n'
    for name,data in [('overlong',b'\xc0\xaf'),('truncated-utf8',b'\xe4\xb8'),
                      ('surrogate',b'\xed\xa0\x80'),('too-large-codepoint',b'\xf4\x90\x80\x80'),
                      ('unexpected-continuation',b'\x80'),('wrong-continuation',b'\xe4\x41\xad')]:
        bad[name]=lambda key,raw=data:base64.b64encode(key.encode())+b'\t'+base64.b64encode(raw)+b'\n'
    bad['raw-nul']=lambda key:encode(key,'不该覆盖。').rstrip(b'\n')+b'\0trailing-corruption\n'
    bad['raw-nul-leading']=lambda key:b'\0'+encode(key,'不该覆盖。')
    bad['raw-nul-chunk-boundary']=lambda key:b' '*65530+b'\0'+encode(key,'不该覆盖。')
    expected={}
    contents=bytearray()
    for name,make in bad.items():
        key='Preserve '+name+' dialogue.'
        contents.extend(encode(key,original));contents.extend(make(key))
        expected[key]=original
    adjacent={
        'RenPy %(name)s {color=#fff}Hello{/color}':'你好 %(name)s {color=#fff}世界{/color}',
        'RPGM \\V[1] \\C[2]Hello':'RPGM \\V[1] \\C[2]你好',
        'Unity <color=red>Score {0}</color>':'Unity <color=red>分数 {0}</color>',
        'Godot [color=red]Hello %s[/color]':'Godot [color=red]你好 %s[/color]',
        'Multiline\nTab\tEmoji \U0001f600':'多行\n制表\t表情 \U0001f600',
        'Whitespace base64':'兼容空白编码',
    }
    rng=random.Random(20261107)
    alphabet=[chr(i) for i in (1,8,9,10,13,0x7f,0x80,0x7ff,0x800,0xd7ff,0xe000,0xffff,0x10000,0x10ffff)]+list('中文ABC={}[]<>/\\')
    for i in range(100):
        adjacent['Unicode vector '+str(i)+': '+''.join(rng.choices(alphabet,k=12))]='译文 '+''.join(rng.choices(alphabet,k=16))
    for key,value in adjacent.items():
        encoded=encode(key,value)
        if key=='Whitespace base64':
            left,right=encoded.rstrip(b'\n').split(b'\t')
            encoded=b' '.join(left[i:i+4] for i in range(0,len(left),4))+b'\t'+b' '.join(right[i:i+4] for i in range(0,len(right),4))+b'\n'
        contents.extend(encoded);expected[key]=value
    final_key='Preserve damaged unterminated final line.'
    contents.extend(encode(final_key,original))
    contents.extend(encode(final_key,'不该覆盖。').rstrip(b'\n')+b'\0bad tail')
    expected[final_key]=original
    checks=[]
    for binary in (ROOT/'native/dst_server.exe',ROOT/'native/dst_server_cs.exe'):
        work=output/binary.stem;work.mkdir(exist_ok=True)
        cache=work/'cache.tsv';cache.write_bytes(contents)
        before=hashlib.sha256(contents).hexdigest()
        with server(binary,work,'http://127.0.0.1:1/v1/chat/completions') as (port,process):
            connection=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
            connection.request('POST','/cache/lookup',json.dumps({'texts':list(expected)}).encode(),{'Content-Type':'application/json'})
            wire=connection.getresponse();status=wire.status;data=wire.read();connection.close()
            try:
                raw=data.decode('utf-8');valid_utf8=True
            except UnicodeDecodeError:
                raw=data.decode('utf-8',errors='replace');valid_utf8=False
            checks.append({'binary':binary.name,'case':'response remains valid UTF-8','pass':valid_utf8})
            response=json.loads(raw)
            for key,value in expected.items():
                checks.append({'binary':binary.name,'case':key,'pass':status==200 and response.get('hits',{}).get(key)==value})
            checks.append({'binary':binary.name,'case':'cache file unchanged during load','pass':hashlib.sha256(cache.read_bytes()).hexdigest()==before})
            if binary.name=='dst_server_cs.exe':
                for name,key,value in [('key','Unsafe\0key','不应入库'),('value','Unsafe value','不应\0入库')]:
                    _,raw=request(port,'/cache/import',{'entries':[{'key':key,'value':value}]})
                    parsed=json.loads(raw)
                    checks.append({'binary':binary.name,'case':'NUL import rejected '+name,'pass':parsed.get('accepted')==0 and parsed.get('rejected')==1})
            checks.append({'binary':binary.name,'case':'no invalid record persisted','pass':hashlib.sha256(cache.read_bytes()).hexdigest()==before})
        log=(work/'server.log').read_text(encoding='utf-8',errors='replace')
        checks.append({'binary':binary.name,'case':'invalid record diagnostic','pass':'load-record-invalid' in log})
    (output/'results.json').write_text(json.dumps(checks,indent=2),encoding='utf-8')
    failures=[i for i in checks if not i['pass']]
    for row in checks:print(('PASS ' if row['pass'] else 'FAIL ')+row['binary']+': '+row['case'])
    print('Cache decode: '+str(len(checks)-len(failures))+' passed, '+str(len(failures))+' failed; output='+str(output))
    if failures and not args.baseline:raise AssertionError('cache decoding corrupted or admitted invalid records')

if __name__=='__main__':main()
