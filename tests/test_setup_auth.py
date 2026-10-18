"""Actual OpenSSH/askpass authentication against an isolated local test server.

Paramiko is a test-only dependency. Neither device discovery nor Frame services
are contacted. Production authentication still uses OpenSSH, never Paramiko.
"""
from __future__ import annotations
import argparse
import faulthandler
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
def load(name,file):
    spec=importlib.util.spec_from_file_location(name,ROOT/'setup'/file)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module);return module

auth=load('auth_test_broker','steam_link_setup_auth.py')
session=load('auth_test_session','steam_link_setup_session.py')
try:import paramiko
except ImportError:paramiko=None
SSH=None
SETUP_DIR=ROOT/'setup'

class Screen:
    def __init__(self, answers=()):
        self.answers=iter(answers);self.prompts=[];self.waiting=threading.Event()
        self.release=threading.Event();self.block=False
    def ssh_prompt(self,prompt,*,secret=False,confirm=False):
        self.prompts.append((secret,confirm))
        if confirm:return 'yes'
        self.waiting.set()
        if self.block:self.release.wait(5)
        return next(self.answers,None)
    def cancel_ssh_prompt(self):self.release.set()

class LocalServer:
    def __init__(self):
        self.key=paramiko.RSAKey.generate(2048)
        self.socket=socket.socket();self.socket.bind(('127.0.0.1',0));self.socket.listen(8);self.socket.settimeout(.1)
        self.port=self.socket.getsockname()[1];self.stopped=threading.Event();self.connections=[];self.workers=[]
        self.passwords=0;self.authenticated=0;self.requests=0;self.stall=False
        self.worker=threading.Thread(target=self._accept);self.worker.start()
    def _accept(self):
        while not self.stopped.is_set():
            try:peer,_=self.socket.accept()
            except socket.timeout:continue
            except OSError:break
            transport=paramiko.Transport(peer);self.connections.append(transport)
            worker=threading.Thread(target=self._connection,args=(transport,));self.workers.append(worker);worker.start()
    def _connection(self,transport):
        outer=self
        ready=threading.Event();commands=[]
        class Handler(paramiko.ServerInterface):
            def get_allowed_auths(self,user):return 'password'
            def check_auth_password(self,user,password):
                outer.passwords+=1
                accepted=user=='fixture' and password=='synthetic quiet password'
                if accepted:outer.authenticated+=1
                return paramiko.AUTH_SUCCESSFUL if accepted else paramiko.AUTH_FAILED
            def check_channel_request(self,kind,channelid):
                return paramiko.OPEN_SUCCEEDED if kind=='session' else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED
            def check_channel_exec_request(self,channel,command):
                commands.append(command);ready.set();return True
        process=None;writer=None
        try:
            transport.add_server_key(self.key);transport.start_server(server=Handler())
            # A real human/Pilot password dialog must not race a synthetic
            # server-side channel deadline before authentication has finished.
            channel=transport.accept(30)
            if channel is None or not ready.wait(4):return
            outer.requests+=1
            if outer.stall:
                self.stopped.wait(4);return
            args=shlex.split(commands[0].decode())
            if args[:5]!=['python3','-I','-B','-u','-c'] or args[5]!=session.REMOTE_SESSION:return
            process=subprocess.Popen([sys.executable,'-I','-B','-u','-c',args[5]],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
            def feed():
                try:
                    while not self.stopped.is_set():
                        data=channel.recv(8192)
                        if not data:break
                        process.stdin.write(data);process.stdin.flush()
                except (OSError,EOFError):pass
                finally:
                    try:process.stdin.close()
                    except OSError:pass
            writer=threading.Thread(target=feed);writer.start()
            while not self.stopped.is_set():
                data=process.stdout.readline()
                if not data:break
                channel.sendall(data)
            channel.send_exit_status(process.wait(timeout=2))
        except (OSError,EOFError,paramiko.SSHException,subprocess.SubprocessError):pass
        finally:
            transport.close()
            if process is not None:
                if process.poll() is None:process.kill()
                process.wait(timeout=2)
                if writer:writer.join(2)
                for stream in (process.stdin,process.stdout):
                    if stream and not stream.closed:stream.close()
    def close(self):
        self.stopped.set();self.socket.close()
        for transport in self.connections:transport.close()
        self.worker.join(2)
        for worker in self.workers:worker.join(5)
        assert not self.worker.is_alive() and not any(w.is_alive() for w in self.workers)

class BrokerBoundary(unittest.TestCase):
    def test_untrusted_local_client_cannot_open_prompt_or_receive_cached_secret(self):
        screen=Screen(['synthetic quiet password']);credentials=auth.Credentials(screen,SETUP_DIR)
        try:
            broker=credentials.open('192.168.1.5','fixture');env=broker.environment()
            with socket.create_connection(('127.0.0.1',int(env['SFNP_ASKPASS_PORT'])),timeout=1) as peer:
                peer.sendall(b'SFNPASK1'+b'f'*64+b'\x00\x00\x00\x01'+b'x')
                try:answer=peer.recv(1)
                except ConnectionResetError:answer=b''
                self.assertEqual(answer,b'')
            self.assertEqual(screen.prompts,[])
            self.assertNotIn('synthetic quiet password',repr(env))
        finally:credentials.close()
        self.assertFalse(broker._thread.is_alive())
    def test_missing_distribution_helper_refuses_before_listening(self):
        with tempfile.TemporaryDirectory(prefix='sfnp-missing-askpass-') as root:
            credentials=auth.Credentials(Screen(),root)
            broker=credentials.open('192.168.1.5','fixture')
            with self.assertRaisesRegex(ValueError,'included sign-in helper'):broker.environment()
            self.assertIsNone(broker._server);credentials.close()

@unittest.skipUnless(paramiko is not None,'test-only Paramiko local server not installed')
class ActualSSH(unittest.TestCase):
    def setUp(self):
        self.ssh=SSH or shutil.which('ssh')
        if not self.ssh:self.skipTest('OpenSSH client unavailable')
        self.tmp=tempfile.TemporaryDirectory(prefix='sfnp-actual-ssh-');self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.server=LocalServer();self.addCleanup(self.server.close)
        self.screen=Screen(['synthetic quiet password']);self.credentials=auth.Credentials(self.screen,SETUP_DIR)
        self.addCleanup(self.credentials.close)
        self.connections=[];self.addCleanup(self.close_sessions)
    def close_sessions(self):
        for connection in self.connections:connection.close()
    def arguments(self, interactive=True):
        return [self.ssh,'-F','none','-p',str(self.server.port),'-o','StrictHostKeyChecking='+('ask' if interactive else 'yes'),
              '-o','UserKnownHostsFile="'+(self.root/'known_hosts').as_posix()+'"','-o','GlobalKnownHostsFile=none',
              '-o','BatchMode='+('no' if interactive else 'yes'),'-o','PubkeyAuthentication=no','-o','PreferredAuthentications=password',
              '-o','NumberOfPasswordPrompts=2','-o','ConnectTimeout=2','fixture@127.0.0.1']
    def connection(self,*,cancelled=None,timeout=5,source=None):
        args=self.arguments()
        value=session.Session(args,source or 'def dispatch(request):return {"ok":True,"value":request.get("value")}\n',
              interactive=True,auth=self.credentials.open('127.0.0.1','fixture'),cancelled=cancelled,timeout=timeout)
        self.connections.append(value);return value
    def test_real_openssh_password_rpc_and_same_invocation_cache(self):
        first=self.connection();self.assertEqual(first.call({'value':'first'}),{'ok':True,'value':'first'});first.close()
        second=self.connection();self.assertEqual(second.call({'value':'second'})['value'],'second')
        self.assertEqual(self.screen.prompts,[(False,True),(True,False)])
        self.assertEqual(self.server.passwords,2);self.assertEqual(self.server.requests,2)
        self.credentials.close();self.assertEqual(self.credentials._values,{})
    def test_real_wrong_password_reprompts_inside_presenter(self):
        self.screen.answers=iter(['wrong synthetic answer','synthetic quiet password'])
        connection=self.connection();self.assertTrue(connection.call({})['ok'])
        self.assertEqual(self.server.passwords,2)
        self.assertEqual(self.screen.prompts,[(False,True),(True,False),(True,False)])
    def test_agent_larger_than_ssh_pipe_buffer_and_followup_rpc_do_not_stall(self):
        # The production bootstrap embeds several full modules. A tiny echo
        # agent alone would miss a blocked initial source write after sign-in.
        source='padding='+repr('x'*(512*1024))+'\ndef dispatch(request):return {"bytes":len(padding),"step":request["step"]}\n'
        connection=self.connection(source=source)
        self.assertEqual(connection.call({'step':1}),{'bytes':512*1024,'step':1})
        self.assertEqual(connection.call({'step':2}),{'bytes':512*1024,'step':2})
    def test_cancel_during_real_password_wait_reaps_every_worker(self):
        self.screen.block=True;stop=threading.Event();connection=self.connection(cancelled=stop.is_set)
        outcomes=[]
        def call():
            try:connection.call({})
            except BaseException as error:outcomes.append(type(error))
        worker=threading.Thread(target=call);worker.start()
        self.assertTrue(self.screen.waiting.wait(4));started=time.monotonic();stop.set();worker.join(3)
        self.assertFalse(worker.is_alive());self.assertLess(time.monotonic()-started,3)
        self.assertEqual(outcomes,[session.SessionCancelled]);self.assertTrue(connection.closed)
        self.assertIsNotNone(connection._process.poll());self.assertFalse(connection._worker.is_alive())
        self.assertFalse(connection._auth._thread.is_alive())
    def test_declining_fingerprint_never_asks_for_password(self):
        self.screen.ssh_prompt=lambda *a,**kw:None
        connection=self.connection()
        with self.assertRaises(session.SessionCancelled):connection.call({})
        self.assertEqual(self.server.passwords,0);self.assertFalse((self.root/'known_hosts').exists())
    def test_human_input_does_not_expire_rpc_network_deadline(self):
        original=self.screen.ssh_prompt
        def prompt(*a,**kw):
            if kw.get('secret'):time.sleep(.7)
            return original(*a,**kw)
        self.screen.ssh_prompt=prompt
        connection=self.connection(timeout=.5);self.assertTrue(connection.call({})['ok'])
    def test_agent_startup_stall_has_bounded_cancellable_timeout(self):
        self.server.stall=True;connection=self.connection(timeout=.4)
        with self.assertRaisesRegex(session.SessionError,'startup timed out'):connection.call({})
        self.assertIsNotNone(connection._process.poll())
    def test_wrong_saved_host_key_is_never_replaced(self):
        other=paramiko.RSAKey.generate(2048)
        raw=('[127.0.0.1]:'+str(self.server.port)+' '+other.get_name()+' '+other.get_base64()+'\n').encode()
        (self.root/'known_hosts').write_bytes(raw)
        connection=self.connection()
        with self.assertRaisesRegex(session.SessionError,'identity could not be verified'):connection.call({})
        self.assertEqual((self.root/'known_hosts').read_bytes(),raw);self.assertEqual(self.server.passwords,0)

try:
    from textual.widgets import Input
    textual=load('_sfnp_setup_textual','steam_link_setup_textual.py')
except ImportError:textual=None

@unittest.skipUnless(paramiko is not None and textual is not None,'actual SSH/UI test dependencies absent')
class ActualTextualSSH(unittest.IsolatedAsyncioTestCase):
    async def test_real_password_rpc_keeps_ui_active_and_password_private(self):
        await self.exercise(False)
    async def test_ctrl_c_cancels_actual_blocked_ssh_without_waiting_for_timeout(self):
        await self.exercise(True)
    async def exercise(self, stall):
        fixture=ActualSSH('test_real_openssh_password_rpc_and_same_invocation_cache');fixture.setUp()
        fixture.server.stall=stall
        def backend():
            fixture.credentials.presenter=textual.active_presenter()
            connection=fixture.connection(cancelled=textual.active_presenter().cancelled)
            try:return 0 if connection.call({})['ok'] else 2
            finally:fixture.credentials.close()
        app=textual.SetupApp(backend,'en')
        async def until(pilot,predicate):
            for _ in range(250):
                if predicate():return
                await pilot.pause(.01)
            self.fail('Actual SSH/UI fixture did not advance: '+repr({'result':app.backend_result,'finished':app.backend_finished,'requests':fixture.server.requests,'password_attempts':fixture.server.passwords,'screen':type(app.screen).__name__,'detail':app.detail_history[-4:]}))
        try:
            async with app.run_test(size=(100,36)) as pilot:
                await until(pilot,lambda:isinstance(app.screen,textual.ConfirmScreen))
                await pilot.press('tab','enter')
                await until(pilot,lambda:isinstance(app.screen,textual.SSHInputScreen))
                await pilot.pause()
                field=app.screen.query_one(Input);self.assertTrue(field.password)
                await pilot.press(*'synthetic quiet password','enter')
                if stall:
                    await until(pilot,lambda:fixture.server.requests==1)
                    await pilot.press('ctrl+c')
                await until(pilot,lambda:app.backend_finished)
                self.assertEqual(app.backend_result,130 if stall else 0)
                self.assertNotIn('synthetic quiet password','\n'.join(app.detail_history))
                self.assertEqual(fixture.credentials._values,{})
                await pilot.press('enter')
            app.backend.join(3);self.assertFalse(app.backend.is_alive())
        finally:fixture.doCleanups()

@unittest.skipUnless(paramiko is not None and textual is not None,'actual wizard test dependencies absent')
class ActualWizardEntry(unittest.IsolatedAsyncioTestCase):
    async def test_automatic_discovery_uses_actual_loader_and_shows_password_outside_cwd(self):
        await self.exercise(manual=False)
    async def test_manual_ip_uses_actual_loader_and_shows_password_with_log_already_created(self):
        await self.exercise(manual=True)
    async def exercise(self, manual):
        # Use the production main -> bridge.run -> factory -> load_helper ->
        # Credentials path, not the direct class imports of lower-level tests.
        fixture=ActualSSH('test_real_openssh_password_rpc_and_same_invocation_cache');fixture.setUp()
        spec=importlib.util.spec_from_file_location('actual_entry_files',ROOT/'tests/test_stream_bridge_install.py')
        files=importlib.util.module_from_spec(spec);spec.loader.exec_module(files)
        data=files.Tests();data.setUp()
        source_dir=SETUP_DIR if (SETUP_DIR/'steam_link_setup.py').is_file() else ROOT/'setup'
        spec=importlib.util.spec_from_file_location('actual_wizard_engine',source_dir/'steam_link_setup.py')
        engine=importlib.util.module_from_spec(spec);sys.modules[spec.name]=engine;spec.loader.exec_module(engine)
        old_cwd=Path.cwd();unrelated=fixture.root/'unrelated current directory';unrelated.mkdir();os.chdir(unrelated)
        calls=[]
        def ssh_args(address,login,*,interactive=False,inline_auth=False):
            calls.append((interactive,inline_auth));return fixture.arguments(interactive)
        def finder(validate,**kwargs):
            presentation=engine.load_presentation()
            sources=type('Hints',(),{'resolve':lambda *a:[], 'named':lambda *a:[] if manual else ['192.168.0.25'], 'lan':lambda *a:None})()
            return presentation.find_frame(validate,cached_hosts=(),sources=sources,presenter=kwargs['presenter'],connector=lambda *a:True)
        def backend():
            return engine.main(['--bundle',str(data.bundle_root),'--lang','en'],local=data.local,
                discover_frame=finder,_textual_session=True)
        ui=engine.load_textual();app=ui.SetupApp(backend,'en')
        async def until(pilot,predicate):
            for _ in range(200):
                if predicate():return
                if app.backend_finished:self.fail('The real wizard exited before its SSH prompt: '+repr(app.detail_history[-3:]))
                await pilot.pause(.01)
            self.fail('The real wizard did not show its SSH prompt: '+repr(app.detail_history[-3:]))
        try:
            with mock.patch.object(engine.sys.stdin,'isatty',return_value=True),mock.patch.object(engine,'ssh_args',side_effect=ssh_args):
                async with app.run_test(size=(100,36)) as pilot:
                    if manual:
                        await until(pilot,lambda:app.action_names==['retry','manual','check','exit'])
                        await pilot.press('down','enter')
                        await until(pilot,lambda:isinstance(app.screen,ui.InputScreen))
                        await pilot.pause();await pilot.press(*'192.168.0.25','enter')
                    await until(pilot,lambda:isinstance(app.screen,ui.ConfirmScreen))
                    self.assertTrue(list((data.local.root/'logs').glob('setup-*.jsonl')))
                    self.assertTrue(any('Setup log:' in label for label,path in app.paths))
                    await pilot.press('tab','enter')
                    await until(pilot,lambda:isinstance(app.screen,ui.SSHInputScreen))
                    self.assertTrue(app.screen.query_one(Input).password)
                    await pilot.press('ctrl+c')
                    # SSH shutdown has bounded process/reader joins. Let those
                    # finish before unmounting the UI; a 1.5s polling budget
                    # raced legitimate Windows cleanup and could carry its
                    # backend into the next case. This is not a network retry.
                    deadline=time.monotonic()+12
                    while not app.backend_finished and time.monotonic()<deadline:
                        await pilot.pause(.02)
                    self.assertTrue(app.backend_finished);self.assertEqual(app.backend_result,130)
                    self.assertNotIn('bridge_rolled_back',app.detail_history)
                    await pilot.press('enter')
                app.backend.join(3);self.assertFalse(app.backend.is_alive())
            self.assertIn((True,True),calls)
            self.assertEqual(fixture.server.authenticated,0)
            self.assertEqual(data.commands,[])
            self.assertFalse((data.local.root/'setup-restore.json').exists())
        finally:
            os.chdir(old_cwd);fixture.doCleanups();data.doCleanups()

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--ssh');parser.add_argument('--setup-dir',type=Path);parser.add_argument('--require-real-ssh',action='store_true')
    parser.add_argument('--diagnostic-timeout',type=int,help='CI-only: dump thread stacks and fail this isolated synthetic test process.')
    args,rest=parser.parse_known_args();SSH=args.ssh
    if args.setup_dir:SETUP_DIR=args.setup_dir
    if args.require_real_ssh and (paramiko is None or not (SSH or shutil.which('ssh'))):raise SystemExit('Actual OpenSSH fixture dependencies missing')
    if args.diagnostic_timeout:
        faulthandler.dump_traceback_later(args.diagnostic_timeout,exit=True)
    try:unittest.main(argv=[sys.argv[0],*rest])
    finally:
        if args.diagnostic_timeout:faulthandler.cancel_dump_traceback_later()
