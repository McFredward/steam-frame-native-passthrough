"""One explicitly owned OpenSSH process with cancellable bounded RPC waits.

The caller supplies reviewed source defining ``dispatch(request) -> dict``.
It is loaded once, over SSH stdin, under a non-main module name. Requests may
contain setup credentials, so neither source, payloads nor remote diagnostics
are included in exceptions, process arguments or logs. OpenSSH owns authentication;
an optional private askpass broker supplies GUI answers without console handoff.
No reconnect or mutation retry is attempted.
"""
from __future__ import annotations

import base64
import json
import math
import os
import shlex
import subprocess
import threading
import time

MAX_SOURCE = 1024 * 1024
MAX_INITIAL = 2 * 1024 * 1024
MAX_REQUEST = 90 * 1024 * 1024
MAX_REPLY = 128 * 1024
SCHEMA = 'sfnp-setup-session-v1'


class SessionError(ValueError):
    """Fixed, non-secret transport diagnostics; an operation may be uncertain."""
    def __init__(self, message, *, reason='remote_unavailable'):
        super().__init__(message)
        self.reason=reason


class SessionCancelled(KeyboardInterrupt):
    """Explicit abort; retain uncertain-operation records without automatic undo."""


# The command is fixed, independently of the agent, requests, host and secrets.
# stdout is protocol-only; ordinary Python output from the reviewed dispatcher
# is discarded with a bound. No remote files, keys or services are changed by
# this transport. Actual operations remain the dispatcher's responsibility.
REMOTE_SESSION = r'''
import base64,contextlib,json,sys
SCHEMA='sfnp-setup-session-v1'
MAX_INITIAL=2097152
MAX_SOURCE=1048576
MAX_REQUEST=94371840
MAX_REPLY=131072
def need(value):
 if not value:raise ValueError('protocol')
def pairs(items):
 result={}
 for key,value in items:
  need(key not in result);result[key]=value
 return result
def parse(raw):
 value=json.loads(raw,object_pairs_hook=pairs,parse_constant=lambda _:need(False))
 need(type(value)is dict);return value
def line(limit):
 raw=sys.stdin.buffer.readline(limit+1)
 need(raw.endswith(b'\n') and len(raw)<=limit);return raw
def encoded(value):return json.dumps(value,sort_keys=True,separators=(',',':'),ensure_ascii=False,allow_nan=False).encode()+b'\n'
class Sink:
 def __init__(self):self.remaining=MAX_REPLY
 def write(self,value):
  need(type(value)is str and len(value)<=self.remaining)
  size=len(value.encode('utf-8'));self.remaining-=size;need(self.remaining>=0);return len(value)
 def flush(self):pass
try:
 initial=parse(line(MAX_INITIAL))
 need(set(initial)=={'schema','source','request_limit','reply_limit'} and initial['schema']==SCHEMA)
 request_limit=initial['request_limit'];reply_limit=initial['reply_limit']
 need(type(request_limit)is int and 128<=request_limit<=MAX_REQUEST and type(reply_limit)is int and 128<=reply_limit<=MAX_REPLY)
 need(type(initial['source'])is str)
 source=base64.b64decode(initial['source'],validate=True);need(0<len(source)<=MAX_SOURCE)
 namespace={'__name__':'sfnp_session_agent','__file__':'<verified-session-agent>'}
 with contextlib.redirect_stdout(Sink()),contextlib.redirect_stderr(Sink()):exec(compile(source,namespace['__file__'],'exec'),namespace)
 dispatch=namespace.get('dispatch');need(callable(dispatch))
 sys.stdout.buffer.write(encoded({'schema':SCHEMA,'ready':True}));sys.stdout.buffer.flush()
 expected=1
 while True:
  raw=sys.stdin.buffer.readline(request_limit+1)
  if not raw:break
  need(raw.endswith(b'\n') and len(raw)<=request_limit)
  item=parse(raw)
  need(set(item)=={'id','request'} and type(item['id'])is int and item['id']==expected and expected<9223372036854775807 and type(item['request'])is dict)
  expected+=1
  try:
   with contextlib.redirect_stdout(Sink()),contextlib.redirect_stderr(Sink()):result=dispatch(item['request'])
   need(type(result)is dict)
   response={'id':item['id'],'ok':True,'result':result}
  except Exception:response={'id':item['id'],'ok':False}
  raw=encoded(response);need(len(raw)<=reply_limit)
  sys.stdout.buffer.write(raw);sys.stdout.buffer.flush()
except BaseException:raise SystemExit(2)
'''


def _parse(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError()
            result[key] = value
        return result
    value = json.loads(raw, object_pairs_hook=pairs,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
    if type(value) is not dict:
        raise ValueError()
    return value


def _encode(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'),
                       ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')


class Session:
    """Lazy single-process RPC, serialized calls and explicit bounded cleanup.

    ``ssh_args`` is the already validated argument list from engine.ssh_args;
    it must contain the destination but no remote command. ``agent_source``
    is trusted package source, never a manifest-supplied function or command.
    ``interactive`` only inherits SSH stderr for terminal prompts; no password
    or prompt text is ever read by Python. A handler rejection keeps this same
    session usable for an explicitly requested Check/Undo. A transport failure
    permanently closes it, including when an operation's result is uncertain.
    """
    def __init__(self, ssh_args, agent_source, *, timeout=120, interactive=False,
                 max_request=MAX_REQUEST, max_reply=MAX_REPLY, auth=None, cancelled=None):
        if (type(ssh_args) not in (list, tuple) or not ssh_args or
                not all(type(arg) is str and arg and '\x00' not in arg for arg in ssh_args)):
            raise SessionError('A validated SSH argument list is required.')
        if type(agent_source) is not str:
            raise SessionError('Reviewed session agent source is required.')
        try:
            source = agent_source.encode('utf-8')
        except UnicodeError:
            raise SessionError('Reviewed session agent source is required.') from None
        if not 0 < len(source) <= MAX_SOURCE:
            raise SessionError('Session agent exceeds its source bound.')
        if (type(max_request) is not int or not 128 <= max_request <= MAX_REQUEST or
                type(max_reply) is not int or not 128 <= max_reply <= MAX_REPLY):
            raise SessionError('Session message limits are invalid.')
        self._duration(timeout)
        self._args = list(ssh_args) + ['python3 -I -B -u -c ' + shlex.quote(REMOTE_SESSION)]
        self._initial = _encode({'schema': SCHEMA, 'source': base64.b64encode(source).decode('ascii'),
                                 'request_limit': max_request, 'reply_limit': max_reply})
        if len(self._initial) > MAX_INITIAL:
            raise SessionError('Session agent exceeds its transport bound.')
        self._timeout, self._interactive = timeout, bool(interactive)
        self._max_request, self._max_reply = max_request, max_reply
        self._state = threading.Lock()
        self._calls = threading.Lock()
        self._closing = threading.Lock()
        self._process = self._worker = None
        self._closed = False
        self._ready = False
        self._next = 1
        self._auth, self._cancelled = auth, cancelled
        self._stderr = bytearray()
        self._stderr_worker = None

    @staticmethod
    def _duration(value):
        if type(value) not in (int, float) or not math.isfinite(value) or not 0 < value <= 3600:
            raise SessionError('Session timeout must be finite and bounded.')
        return value

    @property
    def closed(self):
        with self._state:
            return self._closed

    def __enter__(self):
        if self.closed:
            raise SessionError('The SSH session is closed; no automatic reconnect was attempted.')
        return self

    def __exit__(self, *unused):
        self.close()

    def close(self):
        # Do not wait on _calls: cancellation must also unblock a pending read
        # or a full stdin pipe. Killing SSH releases its inherited pipe handles.
        with self._closing:
            with self._state:
                self._closed = True
                process, worker = self._process, self._worker
            if self._auth is not None:
                self._auth.close()
            if process is not None:
                if process.poll() is None:
                    try:
                        process.kill()
                    except OSError:
                        pass
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    # An uninterruptible OS process is not an invitation to
                    # retry or reopen it. Never block setup shutdown forever.
                    pass
            if worker is not None and worker is not threading.current_thread():
                worker.join(timeout=2)
            if self._stderr_worker is not None:
                self._stderr_worker.join(timeout=2)
            if process is not None and (worker is None or not worker.is_alive()):
                for stream in (process.stdin, process.stdout):
                    if stream is not None:
                        try:
                            stream.close()
                        except (OSError, ValueError):
                            pass
            # A child of OpenSSH can retain its inherited stderr handle after
            # SSH exits. BufferedReader.close() waits for a pending read's lock,
            # so do not turn the bounded reader join into an unbounded close.
            # The status reader closes its own pipe when that read eventually
            # ends; an already stopped reader can be closed here idempotently.
            if process is not None and (self._stderr_worker is None or not self._stderr_worker.is_alive()):
                if process.stderr is not None:
                    try:
                        process.stderr.close()
                    except (OSError, ValueError):
                        pass

    def call(self, request, *, timeout=None):
        duration = self._duration(self._timeout if timeout is None else timeout)
        if ((self._cancelled is not None and self._cancelled()) or
                self._auth is not None and self._auth.cancelled.is_set()):
            self.close()
            raise SessionCancelled()
        if type(request) is not dict:
            raise SessionError('A bounded JSON request object is required.')
        with self._calls:
            try:
                raw = _encode({'id': self._next, 'request': request})
            except (TypeError, ValueError, UnicodeError, RecursionError):
                raise SessionError('A bounded JSON request object is required.') from None
            if len(raw) > self._max_request:
                raise SessionError('Frame request exceeds its transport bound; it was not sent.')
            done = threading.Event()
            outcome = {}
            with self._state:
                if self._closed:
                    raise SessionError('The SSH session is closed; no automatic reconnect was attempted.')
                if self._process is None:
                    try:
                        try:
                            environment = self._auth.environment() if self._auth is not None else None
                        except ValueError:
                            raise SessionError('The included sign-in helper is unavailable. Download the complete setup again.',reason='missing') from None
                        self._process = subprocess.Popen(self._args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE if self._auth is not None else None if self._interactive else subprocess.DEVNULL,
                            **({'env':environment, 'creationflags':subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0,
                                'start_new_session':os.name!='nt'}
                               if environment is not None else {}))
                    except OSError:
                        self._closed = True
                        raise SessionError('SSH could not start. No automatic retry was made.') from None
                    if self._auth is not None:
                        def drain_errors():
                            try:
                                while True:
                                    raw = self._process.stderr.read(1024)
                                    if not raw:break
                                    self._stderr.extend(raw)
                                    if len(self._stderr)>8192:del self._stderr[:-8192]
                            except (OSError,ValueError):pass
                            finally:
                                try:self._process.stderr.close()
                                except (OSError,ValueError):pass
                        self._stderr_worker=threading.Thread(target=drain_errors,name='sfnp-ssh-status',daemon=True)
                        self._stderr_worker.start()
                process, sequence, ready = self._process, self._next, self._ready
                self._next += 1

                def receive(limit):
                    line = process.stdout.readline(limit + 1)
                    if not line.endswith(b'\n') or len(line) > limit:
                        raise ValueError()
                    return _parse(line)

                def exchange():
                    try:
                        if not ready:
                            process.stdin.write(self._initial); process.stdin.flush()
                            hello = receive(self._max_reply)
                            if (set(hello) != {'schema', 'ready'} or hello['schema'] != SCHEMA or
                                    type(hello['ready']) is not bool or hello['ready'] is not True):
                                raise ValueError()
                            outcome['ready'] = True
                        process.stdin.write(raw); process.stdin.flush()
                        reply = receive(self._max_reply)
                        if (type(reply.get('id')) is not int or reply['id'] != sequence or
                                type(reply.get('ok')) is not bool or
                                set(reply) != ({'id', 'ok', 'result'} if reply['ok'] else {'id', 'ok'}) or
                                reply['ok'] and type(reply['result']) is not dict):
                            raise ValueError()
                        outcome['reply'] = reply
                    except Exception:
                        # Do not retain an exception, command, source or payload
                        # in a diagnostic object that the UI could accidentally log.
                        outcome['failed'] = True
                    finally:
                        done.set()

                self._worker = threading.Thread(target=exchange, name='sfnp-ssh-request', daemon=True)
                try:
                    self._worker.start()
                except RuntimeError:
                    self._worker = None
                    self._closed = True
                    process.kill()
                    process.wait(timeout=2)
                    process.stdin.close(); process.stdout.close()
                    raise SessionError('The SSH request worker could not start. No automatic retry was made.') from None
            # Network deadlines exclude time spent waiting for a visible human
            # sign-in prompt. Cancellation polls independently of pipe I/O.
            phase_ready = ready
            remaining = duration if ready else min(duration, 20)
            previous = time.monotonic()
            try:
                while not done.wait(.05):
                    if ((self._cancelled is not None and self._cancelled()) or
                            self._auth is not None and self._auth.cancelled.is_set()):
                        raise SessionCancelled()
                    now = time.monotonic()
                    if not phase_ready and outcome.get('ready'):
                        phase_ready=True; remaining=duration
                    elif self._auth is None or not self._auth.prompting.is_set():
                        remaining-=now-previous
                    previous=now
                    if remaining<=0:
                        self.close()
                        if not phase_ready:
                            raise SessionError('Frame sign-in or setup-agent startup timed out. No setup request was confirmed; try connecting again.',reason='timeout')
                        raise SessionError('Frame operation timed out; its outcome is unknown. Check retained recovery records. No automatic retry was made.',reason='timeout')
            except BaseException:
                self.close()
                raise
            self._worker.join()
            if outcome.get('failed'):
                self.close()
                if self._auth is not None and self._auth.cancelled.is_set():
                    raise SessionCancelled()
                errors=bytes(self._stderr).lower()
                if b'permission denied' in errors or b'authentication failed' in errors:
                    raise SessionError('Frame sign-in was refused. Try again and check the SSH password.',reason='permission_denied')
                if b'host key verification failed' in errors or b'remote host identification has changed' in errors:
                    raise SessionError('The Frame SSH identity could not be verified. Its saved host key was preserved.')
                raise SessionError('The SSH session ended or returned an invalid or excessive reply. Check retained recovery records; no automatic retry was made.')
            with self._state:
                if self._closed:
                    raise SessionError('The SSH session was closed; its operation may be unconfirmed. No automatic retry was made.')
                self._ready = True
            reply = outcome['reply']
            if not reply['ok']:
                raise SessionError('The Frame refused this operation. Existing recovery records remain available; no automatic retry was made.')
            return reply['result']
