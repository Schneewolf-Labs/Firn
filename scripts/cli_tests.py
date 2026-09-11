#!/usr/bin/env python3
"""CLI transport and instance isolation tests. Uses only a private socket."""
import json, os, pathlib, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = pathlib.Path(sys.argv[1] if len(sys.argv)>1 else ROOT/'build').resolve()
APP = drive.binary(str(BUILD), 'app', 'firn')
with tempfile.TemporaryDirectory(prefix='firn-cli-') as session:
    path = pathlib.Path(session)
    sock = str(path/'api.sock')
    env = dict(os.environ, XDG_CONFIG_HOME=session, FIRN_WINDOW='900x700', FIRN_DRIVE=sock)
    env.pop('FIRN_APP', None)
    cli = drive.binary(str(BUILD), 'tools', 'firn-cli')
    passed = 0
    def run(*args, ok=True):
        result = subprocess.run([cli,'--socket',sock,*args],env=env,capture_output=True,text=True,timeout=30)
        assert (result.returncode==0)==ok, (args,result.stdout,result.stderr)
        return json.loads(result.stdout) if ok else result.stderr
    def check(condition, message):
        global passed
        assert condition,message
        passed += 1
        print('ok  '+message,flush=True)
    try:
        result=run('--launch','describe','draw.polygon')
        check(result['actions'][0]['name']=='draw.polygon' and len(result['actions'])==1,'launch finds the adjacent app and describes one action')
        run('do','file.new','{"width":80,"height":80}')
        check(run('--launch','state')['documents']==1,'--launch reuses the existing socket instance')
        recipe=path/'batch.json'
        recipe.write_text(json.dumps({'name':'CLI triangle','actions':[{'action':'draw.polygon','params':{'points':[[10,60],[40,10],[70,60]],'fill':'#ff8800'}}]},indent=2))
        check(run('do','app.batch','--file',str(recipe))['count']==1,'multiline JSON files are compacted into one request')
        check(run('state')['image']['last']=='CLI triangle','the batch reaches the normal undo history')
        malformed=path/'bad.json';malformed.write_text('{bad')
        check('JSON object' in run('do','app.batch','--file',str(malformed),ok=False),'malformed JSON is rejected locally')
        check('cannot read' in run('do','app.batch','--file',str(path/'missing'),ok=False),'missing input files are reported')
        collision=subprocess.run([APP],env=env,capture_output=True,text=True,timeout=20)
        check(collision.returncode!=0 and 'already in use' in collision.stderr,'a second process cannot steal the live socket')
        check(run('state')['documents']==1,'the original API remains usable after a collision')
        # A stale filesystem object at a chosen address is not disposable.
        regular=path/'keep.txt';regular.write_text('keep me')
        collision=subprocess.run([APP],env=dict(env,FIRN_DRIVE=str(regular)),capture_output=True,text=True,timeout=20)
        check(collision.returncode!=0 and regular.read_text()=='keep me','an existing regular file is never removed as a stale socket')
        disconnected=drive.open_socket(sock)
        disconnected.sendall(b'do app.describe {}\n')
        disconnected.close()
        check(run('state')['documents']==1,'disconnecting a client leaves the editor alive')
        if drive.WINDOWS:
            # The loopback port is reachable by anyone on the machine; only a
            # client holding the address file's token may drive the program.
            import socket
            port=int(open(sock).read().split()[1])
            stranger=socket.create_connection(('127.0.0.1',port),timeout=10)
            stranger.sendall(b'state_json\n')
            try:
                answer=stranger.recv(4096)
            except ConnectionResetError:
                answer=b''
            stranger.close()
            check(answer==b'','a connection without the token is dropped unanswered')
            check(run('state')['documents']==1,'and the editor carries on')
        run('do','file.close')
        check(run('quit')['ok'],'quit acknowledges success before closing the connection')
    finally:
        subprocess.run([cli,'--socket',sock,'quit'],env=env,capture_output=True,timeout=10)
    print(f'{passed} CLI checks passed')
