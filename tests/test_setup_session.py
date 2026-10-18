#!/usr/bin/env python3
"""Actual single-process RPC through a synthetic SSH executable; no SSH/device."""
from __future__ import annotations

import argparse
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sfnp_setup_session_tested', ROOT / 'setup/steam_link_setup_session.py')
session = importlib.util.module_from_spec(spec); spec.loader.exec_module(session)
OUT = None

FAKE_SSH = r'''
import json,os,pathlib,shlex,sys,time
with pathlib.Path(sys.argv[1]).open('a',encoding='utf-8') as log:
 log.write(json.dumps({'pid':os.getpid(),'argv':sys.argv[2:]})+'\n')
if sys.argv[2]=='blocked':time.sleep(30);raise SystemExit(7)
if sys.argv[2]=='bad-ready':print('{"schema":"sfnp-setup-session-v1","ready":1}',flush=True);raise SystemExit(0)
args=shlex.split(sys.argv[-1])
assert args[:5]==['python3','-I','-B','-u','-c'] and len(args)==6
exec(compile(args[5],'<actual-fixed-remote-session>','exec'),{'__name__':'__main__'})
'''
AGENT = r'''
import os,sys,time
calls=0
if __name__=='__main__':raise RuntimeError('Agent must not execute a main entry point.')
def dispatch(request):
 global calls
 calls+=1
 action=request.get('action')
 if action=='wait':time.sleep(30)
 if action=='eof':os._exit(9)
 if action=='raise':
  print(request['secret']);sys.stderr.write(request['secret']);raise ValueError(request['secret'])
 if action=='large':return {'large':'x'*20000}
 if action=='nan':return {'number':float('nan')}
 if action=='inject':
  sys.__stdout__.write('{"id":999,"ok":true,"result":{}}\n');sys.__stdout__.flush()
 if action=='duplicate':
  sys.__stdout__.write('{"id":1,"id":1,"ok":true,"result":{}}\n');sys.__stdout__.flush()
 return {'calls':calls,'request':request,'pid':os.getpid(),'name':__name__}
'''


class Tests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='sfnp-session-', dir=OUT)
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name)
        self.fake = self.path / 'ssh.py'; self.fake.write_text(FAKE_SSH, encoding='utf-8')
        self.record = self.path / 'auth.jsonl'
        self.sessions = []
        self.addCleanup(self.close_sessions)

    def close_sessions(self):
        for connection in self.sessions: connection.close()

    def connection(self, source=AGENT, mode='normal', **kwargs):
        connection = session.Session([sys.executable, '-I', '-B', str(self.fake), str(self.record), mode],
                                     source, timeout=5, **kwargs)
        self.sessions.append(connection)
        return connection

    def authentications(self):
        return [json.loads(line) for line in self.record.read_text(encoding='utf-8').splitlines()] if self.record.exists() else []

    def assert_reaped(self, connection):
        self.assertTrue(connection.closed)
        self.assertIsNotNone(connection._process.poll())
        self.assertFalse(connection._worker and connection._worker.is_alive())
        self.assertTrue(connection._process.stdin.closed)
        self.assertTrue(connection._process.stdout.closed)

    def test_one_process_and_agent_state_across_operations(self):
        with self.connection() as connection:
            result = [connection.call({'action':value}) for value in ('inventory','prepare','commit')]
            self.assertEqual([value['calls'] for value in result], [1,2,3])
            self.assertEqual(len({value['pid'] for value in result}), 1)
            self.assertEqual(result[0]['name'], 'sfnp_session_agent')
            self.assertFalse(connection._worker.is_alive(), 'No idle pipe reader thread survives a completed call')
        self.assertEqual(len(self.authentications()), 1, 'One actual child process simulates one authentication')
        self.assert_reaped(connection)

    def test_lazy_close_and_context_reentry_refused(self):
        connection = self.connection(); connection.close(); connection.close()
        self.assertEqual(self.authentications(), [])
        with self.assertRaises(session.SessionError): connection.call({'action':'inventory'})
        with self.assertRaises(session.SessionError): connection.__enter__()

    def test_requests_and_agent_source_never_enter_command_arguments(self):
        secret = 'SYNTHETIC_PASSWORD_NOT_A_REAL_CREDENTIAL'
        source = AGENT + '\nsource_only_secret=' + repr(secret)
        payload = {'data': '";$(touch NEVER);\n\\quotes\\', 'secret':secret, 'nested':{'code':'raise SystemExit(1)'}}
        with self.connection(source) as connection: result = connection.call(payload)
        self.assertEqual(result['request'], payload)
        arguments = json.dumps(self.authentications())
        self.assertNotIn(secret, arguments); self.assertNotIn('NEVER', arguments)
        self.assertNotIn('source_only_secret', arguments)
        self.assertNotIn('dispatch(request)', arguments, 'Only fixed RPC bootstrap code is a remote command')

    def test_handler_rejection_retains_same_authenticated_process_for_explicit_undo(self):
        with self.connection() as connection:
            with self.assertRaises(session.SessionError) as failure:
                connection.call({'action':'raise','secret':'DO_NOT_PRINT_THIS_PAYLOAD'})
            self.assertNotIn('DO_NOT_PRINT', str(failure.exception))
            self.assertFalse(connection.closed)
            result = connection.call({'action':'undo'})
            self.assertEqual(result['calls'], 2)
        self.assertEqual(len(self.authentications()), 1)

    def test_timeout_kills_and_reaps_without_retry(self):
        connection = self.connection(); connection.call({'action':'inventory'})
        started = time.monotonic()
        with self.assertRaisesRegex(session.SessionError, 'timed out'):
            connection.call({'action':'wait'}, timeout=.15)
        self.assertLess(time.monotonic()-started, 4)
        self.assert_reaped(connection)
        with self.assertRaises(session.SessionError): connection.call({'action':'undo'})
        self.assertEqual(len(self.authentications()), 1)

    def test_full_input_pipe_is_cancellable(self):
        connection = self.connection(mode='blocked', max_request=256*1024)
        started = time.monotonic()
        with self.assertRaises(session.SessionError): connection.call({'body':'x'*(192*1024)}, timeout=.2)
        self.assertLess(time.monotonic()-started, 4)
        self.assert_reaped(connection)
        self.assertEqual(len(self.authentications()), 1)

    def test_eof_is_terminal_and_diagnostics_hide_remote_details(self):
        connection = self.connection()
        with self.assertRaises(session.SessionError) as failure: connection.call({'action':'eof','secret':'SYNTHETIC_SECRET'})
        self.assertNotIn('SYNTHETIC_SECRET', str(failure.exception))
        self.assert_reaped(connection)
        with self.assertRaises(session.SessionError): connection.call({'action':'inventory'})
        self.assertEqual(len(self.authentications()), 1)

    def test_ready_requires_a_real_boolean(self):
        connection = self.connection(mode='bad-ready')
        with self.assertRaises(session.SessionError): connection.call({})
        self.assert_reaped(connection)

    def test_bounded_reply_overflow_closes_the_session(self):
        connection = self.connection(max_reply=512)
        with self.assertRaises(session.SessionError): connection.call({'action':'large'})
        self.assert_reaped(connection)

    def test_nonfinite_result_closes_the_session(self):
        connection = self.connection()
        with self.assertRaises(session.SessionError): connection.call({'action':'nan'})
        self.assert_reaped(connection)

    def test_mismatched_and_duplicate_reply_ids_close_the_session(self):
        for action in ('inject','duplicate'):
            with self.subTest(action=action):
                connection = self.connection()
                with self.assertRaises(session.SessionError): connection.call({'action':action})
                self.assert_reaped(connection)

    def test_bad_or_main_only_agent_refused_without_source_diagnostic(self):
        for source in ("raise ValueError('DO_NOT_EXPOSE_AGENT')", 'def nope(request):return {}',
                       'if __name__=="__main__":\n def dispatch(request):return {}'):
            with self.subTest(source_kind=source[:5]):
                connection = self.connection(source)
                with self.assertRaises(session.SessionError) as failure: connection.call({})
                self.assertNotIn('DO_NOT_EXPOSE_AGENT', str(failure.exception))
                self.assert_reaped(connection)

    def test_local_invalid_payloads_do_not_start_ssh(self):
        connection = self.connection(max_request=256)
        for value in ([], {'payload':'x'*300}, {'number':float('nan')}, {'object':object()}):
            with self.subTest(value_type=type(value).__name__), self.assertRaises(session.SessionError): connection.call(value)
        self.assertEqual(self.authentications(), [])
        self.assertFalse(connection.closed)
        with connection: self.assertEqual(connection.call({})['calls'],1)

    def test_concurrent_requests_serialize_without_extra_authentication(self):
        with self.connection() as connection:
            with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
                result = list(pool.map(lambda value:connection.call({'number':value}),range(12)))
            self.assertEqual(sorted(value['calls'] for value in result),list(range(1,13)))
            self.assertEqual([value['request']['number'] for value in result],list(range(12)))
        self.assertEqual(len(self.authentications()),1)
        self.assert_reaped(connection)

    def test_explicit_close_cancels_an_active_operation(self):
        connection = self.connection(); connection.call({})
        failures = []
        def pending():
            try: connection.call({'action':'wait'})
            except session.SessionError as error: failures.append(str(error))
        worker = threading.Thread(target=pending); worker.start()
        time.sleep(.1)
        started = time.monotonic(); connection.close(); worker.join(timeout=3)
        self.assertLess(time.monotonic()-started,4)
        self.assertFalse(worker.is_alive()); self.assertEqual(len(failures),1)
        self.assert_reaped(connection)

    def test_strict_configuration_refusals(self):
        base = [sys.executable, str(self.fake), str(self.record), 'normal']
        for kwargs in ({'timeout':float('nan')},{'timeout':True},{'timeout':0},{'timeout':3601},
                       {'max_request':True},{'max_request':session.MAX_REQUEST+1},
                       {'max_reply':127},{'max_reply':session.MAX_REPLY+1}):
            with self.subTest(kwargs=kwargs), self.assertRaises(session.SessionError): session.Session(base, AGENT, **kwargs)
        for args in ([], ['bad\x00path'], 'ssh'):
            with self.subTest(args_kind=type(args).__name__), self.assertRaises(session.SessionError): session.Session(args,AGENT)
        with self.assertRaises(session.SessionError): session.Session(base,'x'*(session.MAX_SOURCE+1))
        self.assertEqual(self.authentications(),[])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path)
    args,tests=parser.parse_known_args();OUT=args.output
    if OUT:OUT.mkdir(parents=True,exist_ok=True)
    unittest.main(argv=[__file__,*tests])
