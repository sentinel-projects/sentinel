from pathlib import Path
import hashlib,json,os,shutil,socket,subprocess,tempfile,time,uuid

import argparse,sys,ast
from urllib.parse import urlparse


def fixture_snapshot(project):
    if project.is_symlink():
        raise RuntimeError("Fixture root must remain a real directory")
    result = {}
    for path in project.iterdir():
        if path.is_symlink():
            result[path.name] = "<symlink rejected>"
        elif path.is_file():
            if path.name not in {"calc.py", "test_calc.py", "unrelated.txt"}:
                result[path.name] = "<unexpected file>"
                continue
            flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
            before = path.lstat()
            with os.fdopen(os.open(path, flags), "rb") as stream:
                opened = os.fstat(stream.fileno())
                if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
                    raise RuntimeError("Fixture file identity changed")
                data = stream.read(65537)
            result[path.name] = data.decode("utf-8", errors="strict") if len(data) <= 65536 else "<oversized fixture file>"
    return result

def verify_fixture(project, trusted_test):
    # Never execute arbitrary model-written Python outside the daemon sandbox.
    try:
        source = fixture_snapshot(project)["calc.py"]
        tree = ast.parse(source.encode("utf-8"))
        allowed = [ast.dump(ast.parse("def add(a, b):\n    return a " + operator + " b\n"), include_attributes=False) for operator in ("+", "-")]
        fingerprint = ast.dump(tree, include_attributes=False)
    except (OSError, SyntaxError, KeyError, ValueError):
        return {"returncode": 2, "stdout": "", "stderr": "Fixture source could not be safely verified.", "source_ast_verified": False}
    if fingerprint not in allowed:
        return {"returncode": 2, "stdout": "", "stderr": "Independent verifier refused unexpected Python structure.", "source_ast_verified": False}
    with tempfile.TemporaryDirectory(prefix="sentinel-verify-") as temporary:
        check = Path(temporary).resolve()
        # Reconstruct the validated AST: comments/encoding cookies cannot alter execution.
        (check / "calc.py").write_text(ast.unparse(tree) + "\n", encoding="utf-8")
        (check / "test_calc.py").write_text(trusted_test, encoding="utf-8")
        environment = {key: value for key, value in os.environ.items() if not key.startswith("PYTHON")}
        tested = subprocess.run([sys.executable, "-S", "test_calc.py"], cwd=check, env=environment, capture_output=True, text=True, timeout=15)
        return {"returncode": tested.returncode, "stdout": tested.stdout.replace(str(check), "<verifier>").replace(temporary, "<verifier>"),
                "stderr": tested.stderr.replace(str(check), "<verifier>").replace(temporary, "<verifier>"), "source_ast_verified": True,
                "code_matches_expected": fingerprint == allowed[0]}

parser=argparse.ArgumentParser(description="Run an isolated opt-in Sentinel coding-agent acceptance test. Does not download models.")
parser.add_argument('--daemon', type=Path, required=True, help='Built/deployed sentinel-daemon executable')
parser.add_argument('--runtime-dir', type=Path, help='Optional deployed runtime directory to copy with DLLs/plugins; daemon must be inside it')
parser.add_argument('--model', required=True, help='Installed local model ID')
parser.add_argument('--provider', choices=['lm-studio', 'ollama', 'llama-cpp-server'], default='lm-studio')
parser.add_argument('--endpoint')
parser.add_argument('--output-dir', type=Path, help='New directory for isolated runtime and evidence; default: OS temporary directory')
parser.add_argument('--test-command', default='python3 test_calc.py', help='Exact sandbox command, e.g. py -3 test_calc.py on Windows')
parser.add_argument('--inspect-only', action='store_true')
parser.add_argument('--completion-only', action='store_true', help='Edit only calc.py and read it back; independent trusted verifier still runs')
parser.add_argument('--timeout', type=int, default=240, help='Per-case timeout in seconds')
args=parser.parse_args()
args.endpoint = args.endpoint or {'lm-studio':'http://127.0.0.1:1234', 'ollama':'http://127.0.0.1:11434', 'llama-cpp-server':'http://127.0.0.1:8080'}[args.provider]
if urlparse(args.endpoint).hostname not in {'127.0.0.1','localhost','::1'}:parser.error('--endpoint must be a loopback local server')
if args.timeout < 1:parser.error('--timeout must be positive')
if args.inspect_only and args.completion_only:parser.error('--inspect-only and --completion-only are mutually exclusive')
if not args.daemon.is_file():parser.error('--daemon must be an existing executable')
if args.runtime_dir and not args.daemon.resolve().is_relative_to(args.runtime_dir.resolve()):parser.error('--daemon must be inside --runtime-dir')
if os.name == 'nt':
    try:
        from PySide6.QtCore import QCoreApplication
        from PySide6.QtNetwork import QLocalSocket
    except ImportError:
        parser.error('Windows local IPC requires PySide6 in the test Python environment (Qt named pipes).')
model=args.model
root=args.output_dir.resolve() if args.output_dir else Path(tempfile.mkdtemp(prefix='sentinel-tools-')).resolve()
if args.output_dir:root.mkdir(mode=0o700,parents=True,exist_ok=False)
base=root
binary=root/'bin'
if args.runtime_dir:
    shutil.copytree(args.runtime_dir,binary)
    daemon=binary/args.daemon.resolve().relative_to(args.runtime_dir.resolve())
else:
    binary.mkdir();daemon=binary/args.daemon.name;shutil.copy2(args.daemon,daemon)
project=root/'project';project.mkdir()
(daemon.parent/'settings.json').write_text(json.dumps({{'lm-studio':'lmStudioEndpoint','ollama':'ollamaEndpoint','llama-cpp-server':'llamaCppEndpoint'}[args.provider]:args.endpoint,'defaultPermissionPolicyState':'Ask'}),encoding='utf-8')
(project/'calc.py').write_text('def add(a, b):\n    return a - b\n',encoding='utf-8')
(project/'test_calc.py').write_text('from calc import add\nassert add(2, 3) == 5\nassert add(-1, 1) == 0\nprint("ALL_TESTS_PASSED")\n',encoding='utf-8')
(project/'unrelated.txt').write_text('Preserve this file exactly.\n',encoding='utf-8')
subprocess.run(['git','init','--quiet',str(project)],check=True)
subprocess.run(['git','add','--','calc.py','test_calc.py','unrelated.txt'],cwd=project,check=True)
sockdir=Path(tempfile.mkdtemp(prefix='st-tools-')).resolve();os.chmod(sockdir,0o700)
sockpath=sockdir/'daemon.sock'
log=(root/'daemon.log').open('w',encoding='utf-8')
process=subprocess.Popen([str(daemon),'--portable','--profile-name','SentinelTools-'+uuid.uuid4().hex[:8],'--socket',str(sockpath)],cwd=root,stdout=log,stderr=log)
events=[];records=[];cases=[];seq=0;connection=None;stream=None;failed=False
def public_value(value):
    if isinstance(value,str):
        try:
            nested = json.loads(value)
            if isinstance(nested,(dict,list)):
                return json.dumps(public_value(nested),separators=(',',':'))
        except (ValueError,TypeError):
            pass
        for path,label in ((project,'<workspace>'),(root,'<test-root>'),(sockdir,'<ipc-temp>')):
            value=value.replace(str(path),label).replace(path.as_posix(),label)
        return value.replace(str(Path.home()),"<home>").replace(Path.home().as_posix(),"<home>")
    if isinstance(value,list):return [public_value(x) for x in value]
    if isinstance(value,dict):return {k:public_value(v) for k,v in value.items()}
    return value

def save():
    (base/'live-qwen-result.json').write_text(json.dumps(public_value({'root':str(root),'model':model,'records':records,'cases':cases,'events':events}),indent=2),encoding='utf-8')
def read():
    line=read_line()
    if not line:raise RuntimeError('Daemon disconnected')
    item=json.loads(line)
    if item.get('type')=='event':events.append(item)
    return item
def request(name,payload):
    global seq
    seq+=1;rid=str(seq)
    send_bytes((json.dumps({'version':{'major':1,'minor':1},'type':'request','id':rid,'name':name,'payload':payload})+'\n').encode())
    while True:
        item=read()
        if item.get('id')==rid:
            records.append({'command':name,'response':item});save()
            if item.get('type')=='error':raise RuntimeError(f'{name}: {item}')
            return item.get('payload',{})
def run_case(label,prompt,edit=False):
    sid=request('session.create',{'title':'Tools live '+label})['session_id']
    wid=request('workspace.create',{'name':'Tools live '+label,'template':'Coding'})['workspace_id']
    request('workspace.root',{'workspace_id':wid,'path':str(project)})
    request('workspace.select',{'workspace_id':wid})
    before=fixture_snapshot(project)
    first=len(events);submitted=request('agent.start',{'session_id':sid,'text':prompt})
    runid=submitted.get('run_id');cursor=first;end=time.monotonic()+args.timeout;terminal=None;approvals=[]
    allowed_commands={args.test_command}
    digests={hashlib.sha256(c.encode()).hexdigest() for c in allowed_commands}
    while time.monotonic()<end and terminal is None:
        while cursor < len(events):
            item=events[cursor];cursor+=1
            payload=item.get('payload',{});name=item.get('name')
            if name=='run.started' and payload.get('session_id')==sid:runid=payload.get('run_id')
            if payload.get('run_id')!=runid:continue
            if name=='approval.requested':
                tool=payload.get('tool');resources=[r.get('resource','') for r in payload.get('resources',[])]
                paths=[x for x in resources if Path(x).is_absolute()]
                allow=tool in {'read-file','list-directory','glob','grep','stat-path','git-status','git-diff'} and all(Path(x).resolve().is_relative_to(project.resolve()) for x in paths)
                if edit and tool in {'edit-file','write-file','apply-patch'}:
                    allow=bool(paths) and all(Path(x).resolve()==(project/'calc.py').resolve() for x in paths)
                if edit and tool=='run-command':allow=bool(resources) and all(x in digests for x in resources)
                if tool in {'todo-write','todo-read'}:allow=True
                known_digests = digests | {hashlib.sha256(x.encode()).hexdigest() for x in (".", str(project), str(project.resolve()))}
                if tool not in {"todo-read", "todo-write"} and any(not Path(x).is_absolute() and x not in known_digests for x in resources):allow=False
                approvals.append({'tool':tool,'resources':resources,'allowed':allow})
                request('approval.respond',{'run_id':runid,'approval_id':payload['approval_id'],'allow':allow})
            if name in {'run.completed','run.failed','run.cancelled'}:terminal=item
        if terminal is None:
            if os.name != 'nt':connection.settimeout(max(.1,end-time.monotonic()))
            read()
    if terminal is None:
        request('run.cancel',{'run_id':runid})
    after=fixture_snapshot(project)
    result={'label':label,'run_id':runid,'terminal':terminal,'approvals':approvals,'before':before,'after':after,'events':events[first:]}
    if edit:
        result['independent_test']=verify_fixture(project, before['test_calc.py'])
        result['only_expected_source_changed']={k:v for k,v in before.items() if k!='calc.py'}=={k:v for k,v in after.items() if k!='calc.py'}
        result['unrelated_preserved']=before['unrelated.txt']==after['unrelated.txt']
        result['test_preserved']=before['test_calc.py']==after['test_calc.py']
    cases.append(result);save();print(label,terminal and terminal.get('name'),flush=True)
try:
    deadline=time.monotonic()+15
    if os.name == 'nt':
        qt_app=QCoreApplication.instance() or QCoreApplication([])
        connection=QLocalSocket()
        while time.monotonic()<deadline:
            connection.connectToServer(str(sockpath))
            if connection.waitForConnected(200):break
            connection.abort();time.sleep(.1)
        else:raise RuntimeError('Daemon named-pipe startup failed; see daemon.log')
        def send_bytes(data):
            if connection.write(data) != len(data):raise RuntimeError('IPC write failed')
            if connection.bytesToWrite() and not connection.waitForBytesWritten(5000):raise RuntimeError('IPC write timeout')
        def read_line():
            limit=time.monotonic()+args.timeout
            while not connection.canReadLine():
                remaining=int((limit-time.monotonic())*1000)
                if remaining<=0 or not connection.waitForReadyRead(min(remaining,1000)):
                    if remaining<=0:raise TimeoutError('IPC read timeout')
                    if connection.state()==QLocalSocket.UnconnectedState:raise RuntimeError('Daemon disconnected')
            return bytes(connection.readLine())
    else:
        connection=socket.socket(socket.AF_UNIX)
        while time.monotonic()<deadline:
            try:connection.connect(str(sockpath));break
            except (FileNotFoundError,ConnectionRefusedError):
                if process.poll() is not None:raise RuntimeError('Daemon startup failed; see daemon.log')
                time.sleep(.1)
        else:raise RuntimeError('Daemon socket startup timeout')
        connection.settimeout(args.timeout);stream=connection.makefile('rb')
        send_bytes=connection.sendall
        read_line=stream.readline
    request('hello',{'client_id':'tools-live-validation','major':1,'minor':1,'capabilities':[]})
    request('provider.select',{'provider_id':args.provider})
    request('desktop.action',{'session_id':'','action':'refreshModelDiscovery','arguments':[]})
    for attempt in range(40):
        catalog=request('model.list',{})
        if model in json.dumps(catalog):break
        time.sleep(.25)
    request('model.select',{'provider_id':args.provider,'model_id':model})
    print(f'Local binding selected: {args.provider}/{model}',flush=True)
    if not args.completion_only:
        run_case('inspect','Read calc.py and test_calc.py in the active workspace using tools. Explain the actual arithmetic bug. Do not modify files or run commands. Cite the actual source lines.')
    if args.completion_only:
        run_case('write-readback', f'In the active workspace, change only {project / "calc.py"} so add(a, b) returns a + b. Read back calc.py and report the observed final content. Do not run commands or modify any other file.', edit=True)
    elif not args.inspect_only:
        run_case('fix-and-test',f'Active workspace root: {project}. Use the absolute path {project / "calc.py"} for all edits. File paths are relative to the active workspace root, so do not prefix project/. Fix the arithmetic bug in calc.py so add returns the sum of a and b. Read the files first. Change only calc.py and preserve test_calc.py and unrelated.txt. Run exactly `{args.test_command}` using run-command and report the observed test result. Inspect the resulting change using git-diff. Do not claim success without a successful test.',edit=True)
except Exception as error:
    failed=True
    records.append({'harness_error':str(error)});save();print(type(error).__name__,str(error),flush=True)
finally:
    if connection:
        try:request('daemon.shutdown',{})
        except Exception:pass
        connection.close()
    if process.poll() is None:
        process.terminate()
        try:process.wait(timeout=15)
        except subprocess.TimeoutExpired:process.kill();process.wait()
    log.close();save();shutil.rmtree(sockdir,ignore_errors=True);print('Evidence:',root,flush=True)
    if stream:stream.close()
if failed or not cases or any(not c.get('terminal') or c['terminal'].get('name')!='run.completed' for c in cases):sys.exit(1)
if any(c.get('independent_test',{}).get('returncode',0)!=0 or not c.get('test_preserved',True) or not c.get('unrelated_preserved',True) or not c.get('only_expected_source_changed',True) for c in cases):sys.exit(1)
