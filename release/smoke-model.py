"""Exercise text and picture inference from an isolated installed model folder."""
import argparse
import base64
import json
from pathlib import Path
import socket
import struct
import subprocess
import time
import urllib.request
import zlib


def red_picture():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = (b'\0' + b'\xff\0\0' * 128) * 128
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',128,128,8,2,0,0,0)) +
            chunk(b'IDAT',zlib.compress(pixels)) + chunk(b'IEND',b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--models', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    args = parser.parse_args()
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0))
        port = probe.getsockname()[1]
    args.log.parent.mkdir(parents=True,exist_ok=True)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    url = f'http://127.0.0.1:{port}'
    command = [str(args.server), '-m', str(args.models/'Qwen3.5-4B-Q4_K_M.gguf'),
               '--mmproj', str(args.models/'Qwen3.5-4B-mmproj-F16.gguf'),
               '--host','127.0.0.1','--port',str(port),'-ngl','0','-c','2048','-t','4',
               '--image-max-tokens','256']
    with args.log.open('wb') as log:
        process = subprocess.Popen(command,cwd=args.server.parent,stdout=log,stderr=subprocess.STDOUT,
                                   creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            deadline = time.monotonic()+240
            while True:
                if process.poll() is not None:
                    raise RuntimeError('Installed model server exited; inspect the retained smoke log.')
                try:
                    with opener.open(url+'/health',timeout=2) as response:
                        if response.status == 200: break
                except OSError:
                    pass
                if time.monotonic()>deadline:
                    raise RuntimeError('Installed model server did not become ready.')
                time.sleep(0.5)
            def ask(content):
                body = {'messages':[{'role':'user','content':content}], 'max_tokens':64,
                        'temperature':0,'chat_template_kwargs':{'enable_thinking':False}}
                request = urllib.request.Request(url+'/v1/chat/completions',data=json.dumps(body).encode(),
                                                headers={'Content-Type':'application/json'})
                with opener.open(request,timeout=240) as response: result=json.load(response)
                return result['choices'][0]['message'].get('content','').strip().lower()
            if '4' not in ask('What is 2 + 2? Reply with only the answer.'):
                raise RuntimeError('Installed model text inference did not answer the arithmetic fixture.')
            picture=base64.b64encode(red_picture()).decode()
            answer=ask([{'type':'text','text':'What is the dominant color of this image? Reply with only its color name.'},
                        {'type':'image_url','image_url':{'url':'data:image/png;base64,'+picture}}])
            if 'red' not in answer:
                raise RuntimeError('Installed picture reader did not recognize the red image fixture.')
            print('Installed CPU model passed text and picture inference using local files and loopback HTTP.')
        finally:
            process.terminate()
            try: process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=15)


if __name__=='__main__':
    main()
