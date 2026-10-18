"""In-memory OpenSSH askpass bridge to the active setup presenter.

OpenSSH still owns authentication and known-host verification. Its small
askpass child obtains one answer from this loopback-only, nonce-authenticated
broker. Passwords never enter argv, environment, logs or files. The shared
cache exists only until this invocation's final cleanup.
"""
from __future__ import annotations

import hmac
import os
from pathlib import Path
import re
import secrets
import socket
import struct
import sys
import threading

MAX_PROMPT = 4096
MAX_ANSWER = 1024
CANCEL = struct.pack('!I', 0xffffffff)


class Credentials:
    def __init__(self, presenter, setup_dir=None):
        self.presenter = presenter
        self.setup_dir = Path(setup_dir or Path(__file__).parent).resolve()
        self._values = {}
        self._brokers = []
        self._closed = False

    def open(self, address, login):
        if self._closed:
            raise ValueError('The setup sign-in session is closed.')
        broker = Broker(self, (address, login))
        self._brokers.append(broker)
        return broker

    def close(self):
        self._closed = True
        for broker in self._brokers:
            broker.close()
        self._brokers.clear()
        self._values.clear()


def _receive(peer, size):
    result = bytearray()
    while len(result) < size:
        part = peer.recv(size - len(result))
        if not part:
            raise ValueError('Incomplete sign-in request.')
        result.extend(part)
    return bytes(result)


class Broker:
    def __init__(self, credentials, identity):
        self.credentials, self.identity = credentials, identity
        self.prompting = threading.Event()
        self.cancelled = threading.Event()
        self._stop = threading.Event()
        self._token = secrets.token_hex(32)
        self._server = self._thread = self._peer = None
        self._password_prompts = 0

    def environment(self):
        root = self.credentials.setup_dir
        client = root / 'steam_link_setup_askpass_client.py'
        helper = root / ('steam-link-askpass.exe' if os.name == 'nt' else 'steam-link-askpass.sh')
        for path in (client, helper):
            if path.is_symlink() or not path.is_file():
                raise ValueError('The setup download is missing its included sign-in helper. Run the setup command again.')
        if os.name != 'nt' and not os.access(helper, os.X_OK):
            raise ValueError('The included sign-in helper is not executable. Run the setup command again.')
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if os.name == 'nt':
            self._server.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        self._server.bind(('127.0.0.1', 0))
        self._server.listen(2)
        self._server.settimeout(.2)
        env = dict(os.environ)
        env.setdefault('DISPLAY', ':0')
        env.update(SSH_ASKPASS=str(helper), SSH_ASKPASS_REQUIRE='force',
                   SFNP_ASKPASS_PYTHON=sys.executable, SFNP_ASKPASS_CLIENT=str(client),
                   SFNP_ASKPASS_PORT=str(self._server.getsockname()[1]), SFNP_ASKPASS_TOKEN=self._token)
        self._thread = threading.Thread(target=self._serve, name='sfnp-ssh-sign-in', daemon=False)
        self._thread.start()
        return env

    def _answer(self, prompt):
        # Every answer other than the explicit known-host question is masked.
        # Unknown/changed keys are never accepted by weakening SSH policy.
        if self._stop.is_set():return None
        confirm = ('are you sure you want to continue connecting' in prompt.casefold() or
                   prompt.casefold().startswith('allow '))
        password = bool(re.search(r"(?i)(?:'s password:|password:)\s*$", prompt))
        cache = self.credentials._values
        if password:
            self._password_prompts += 1
            if self._password_prompts == 1 and self.identity in cache:
                return cache[self.identity]
            # A second password prompt means the preceding answer was refused.
            cache.pop(self.identity, None)
        self.prompting.set()
        try:
            answer = self.credentials.presenter.ssh_prompt(prompt, secret=not confirm, confirm=confirm)
        finally:
            self.prompting.clear()
        if answer is None:
            self.cancelled.set()
            return None
        if type(answer) is not str or any(c in answer for c in ('\r', '\n', '\x00')):
            return None
        encoded = answer.encode('utf-8')
        if not encoded or len(encoded) > MAX_ANSWER or confirm and answer != 'yes':
            return None
        if password:
            cache[self.identity] = answer
        return answer

    def _serve(self):
        # Sequential, capped prompts; malformed local clients cannot create
        # unbounded threads or display a prompt without the invocation nonce.
        accepted = 0
        while not self._stop.is_set() and accepted < 16:
            try:
                peer, address = self._server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self._peer = peer
            try:
                peer.settimeout(3)
                header = _receive(peer, 76)
                token = header[8:72]
                length, = struct.unpack('!I', header[72:76])
                if (address[0] != '127.0.0.1' or header[:8] != b'SFNPASK1' or
                        not hmac.compare_digest(token, self._token.encode('ascii')) or not 0 < length <= MAX_PROMPT):
                    continue
                prompt = _receive(peer, length).decode('utf-8', errors='strict')
                if '\x00' in prompt:
                    continue
                accepted += 1
                answer = self._answer(prompt)
                if self._stop.is_set():
                    answer = None
                raw = answer.encode('utf-8') if answer is not None else None
                peer.sendall(CANCEL if raw is None else struct.pack('!I', len(raw)) + raw)
            except (OSError, ValueError, UnicodeError):
                pass
            finally:
                self._peer = None
                peer.close()

    def close(self):
        self._stop.set()
        if self.prompting.is_set():
            dismiss = getattr(self.credentials.presenter, 'cancel_ssh_prompt', None)
            if dismiss:
                dismiss()
        if self._server is not None:
            self._server.close()
        peer = self._peer
        if peer is not None:
            try:
                peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(timeout=3)
        self._token = ''
