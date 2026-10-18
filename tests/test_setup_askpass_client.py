#!/usr/bin/env python3
"""Real loopback/child-process checks for the owned askpass protocol, no SSH."""
from __future__ import annotations

from contextlib import contextmanager
import importlib.util
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / 'setup/steam_link_setup_askpass_client.py'
LAUNCHER = ROOT / 'setup/steam-link-askpass.sh'
TOKEN = '0123456789abcdef' * 4


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


def receive(connection, count):
    result = b''
    while len(result) < count:
        part = connection.recv(count - len(result))
        if not part:
            raise ValueError('Incomplete synthetic fixture request.')
        result += part
    return result


@contextmanager
def broker(response, *, pause=0, chunks=False):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(('127.0.0.1', 0));listener.listen(1);listener.settimeout(5)
    observed = {'port': listener.getsockname()[1]}
    def serve():
        try:
            with listener.accept()[0] as connection:
                connection.settimeout(5)
                header = receive(connection, 76)
                count = struct.unpack('!I', header[72:76])[0]
                if count > 4096:
                    raise ValueError('Prompt bound exceeded.')
                observed['request'] = header + receive(connection, count)
                if pause:
                    time.sleep(pause)
                if chunks:
                    for byte in response:
                        connection.sendall(bytes((byte,)))
                else:
                    connection.sendall(response)
        except (OSError, ValueError) as error:
            observed['error'] = type(error).__name__
        finally:
            listener.close()
    thread = threading.Thread(target=serve, daemon=True);thread.start()
    try:
        yield observed
    finally:
        thread.join(6)
        if thread.is_alive():
            raise RuntimeError('Synthetic broker was not reaped.')


class AskpassTests(unittest.TestCase):
    def invoke(self, port, prompt='Frame password:', *, token=TOKEN, shell=False, extra=None):
        environment = dict(os.environ, SFNP_ASKPASS_PORT=str(port), SFNP_ASKPASS_TOKEN=token)
        if extra:
            environment.update(extra)
        if shell:
            environment.update(SFNP_ASKPASS_PYTHON=sys.executable, SFNP_ASKPASS_CLIENT=str(CLIENT))
            command = ['/bin/sh', str(LAUNCHER), prompt]
        else:
            command = [sys.executable, '-I', '-B', str(CLIENT), prompt]
        return subprocess.run(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)

    def test_unicode_request_and_answer_use_actual_loopback(self):
        prompt = 'Passwort für Främe 🥽:\nBitte bestätigen.'
        answer = 'Pässwört 🔐'.encode('utf8')
        with broker(struct.pack('!I', len(answer)) + answer, chunks=True) as peer:
            result = self.invoke(peer['port'], prompt)
        expected = b'SFNPASK1' + TOKEN.encode('ascii') + struct.pack('!I', len(prompt.encode('utf8'))) + prompt.encode('utf8')
        self.assertEqual(peer.get('request'), expected)
        self.assertTrue(result.returncode == 0 and result.stdout == answer + b'\n' and result.stderr == b'')

    def test_empty_and_maximum_answers(self):
        for answer in (b'', 'ü'.encode('utf8') * 512):
            with self.subTest(size=len(answer)), broker(struct.pack('!I', len(answer)) + answer) as peer:
                result = self.invoke(peer['port'])
            self.assertTrue(result.returncode == 0 and result.stdout == answer + b'\n' and result.stderr == b'')

    def test_cancel_overlong_and_truncated_headers_never_emit_answers(self):
        for payload in (b'\xff\xff\xff\xff', struct.pack('!I', 1025), b'\x00\x00',
                        struct.pack('!I', 10) + b'short'):
            with self.subTest(payload_size=len(payload)), broker(payload) as peer:
                result = self.invoke(peer['port'])
            self.assertTrue(result.returncode != 0 and result.stdout == b'' and result.stderr == b'')

    def test_invalid_utf8_or_answer_controls_never_emit_secrets(self):
        for answer in (b'synthetic\nfixture', b'synthetic\rfixture', b'synthetic\x00fixture', b'\xff', b'\xc0\x80'):
            with self.subTest(answer_size=len(answer)), broker(struct.pack('!I', len(answer)) + answer) as peer:
                result = self.invoke(peer['port'])
            self.assertTrue(result.returncode != 0 and result.stdout == b'' and result.stderr == b'')

    def test_invalid_tokens_and_ports_refuse_without_connections(self):
        for port, token in ((0, TOKEN), (65536, TOKEN), ('01', TOKEN), ('1x', TOKEN), ('１２', TOKEN),
                            (1, ''), (1, 'a' * 63), (1, 'a' * 65), (1, 'g' * 64), (1, 'ü' * 64)):
            with self.subTest(port=port, token_length=len(token)):
                result = self.invoke(port, token=token)
                self.assertTrue(result.returncode != 0 and result.stdout == b'' and result.stderr == b'')

    def test_prompt_byte_bound(self):
        prompt = 'ä' * 2048
        with broker(struct.pack('!I', 3) + b'yes') as peer:
            result = self.invoke(peer['port'], prompt)
        self.assertTrue(result.returncode == 0 and result.stdout == b'yes\n')
        result = self.invoke(1, prompt + 'x')
        self.assertTrue(result.returncode != 0 and result.stdout == b'' and result.stderr == b'')

    def test_missing_arguments_are_silent(self):
        for arguments in ([], ['one', 'two']):
            result = subprocess.run([sys.executable, '-I', '-B', str(CLIENT), *arguments],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
            self.assertTrue(result.returncode != 0 and result.stdout == b'' and result.stderr == b'')

    def test_confirm_hint_uses_the_same_raw_prompt_protocol(self):
        with broker(struct.pack('!I', 3) + b'yes') as peer:
            result = self.invoke(peer['port'], 'Approve host key?', extra={'SSH_ASKPASS_PROMPT': 'confirm'})
        self.assertTrue(result.returncode == 0 and result.stdout == b'yes\n')
        self.assertTrue(peer['request'].endswith(b'Approve host key?'))

    @unittest.skipUnless(os.name != 'nt', 'POSIX launcher only')
    def test_shell_launcher_uses_the_isolated_python_client(self):
        with broker(struct.pack('!I', 9) + b'synthetic') as peer:
            result = self.invoke(peer['port'], shell=True)
        self.assertTrue(result.returncode == 0 and result.stdout == b'synthetic\n' and result.stderr == b'')

    def test_total_deadline_is_bounded_and_broker_close_cancels(self):
        client = load('sfnp_askpass_deadline', CLIENT)
        client.TIMEOUT_SECONDS = 0.05
        with broker(struct.pack('!I', 1) + b'x', pause=0.2) as peer:
            answer = client.request('Synthetic timeout', {'SFNP_ASKPASS_PORT': str(peer['port']), 'SFNP_ASKPASS_TOKEN': TOKEN})
        self.assertIsNone(answer)
        with broker(b'') as peer:
            result = self.invoke(peer['port'])
        self.assertTrue(result.returncode != 0 and result.stdout == b'' and result.stderr == b'')


class BuildTests(unittest.TestCase):
    def test_actual_windows_helper_build_imports_and_subsystem(self):
        builder = load('sfnp_askpass_builder', ROOT / 'tools/build_setup_askpass.py')
        existing = os.environ.get('SFNP_ASKPASS_EXE')
        if existing:
            raw = Path(existing).read_bytes()
            result = builder.inspect(raw)
        elif shutil.which('x86_64-w64-mingw32-gcc') or shutil.which('cl.exe') or shutil.which('cl'):
            with tempfile.TemporaryDirectory(prefix='sfnp-askpass-test-') as directory:
                path = Path(directory) / 'askpass.exe'
                result = builder.build(path)
                self.assertEqual(result['sha256'], builder.inspect(path.read_bytes())['sha256'])
                raw = path.read_bytes()
        else:
            self.skipTest('Distribution C compiler or prebuilt helper not supplied.')
        self.assertEqual(result['architecture'], 'windows-x86_64')
        self.assertEqual(result['subsystem'], 'windows-gui')
        self.assertTrue(all(name in builder.SYSTEM_IMPORTS or name.startswith('api-ms-win-crt-')
                            for name in result['imports']))
        for changed in (b'bad', raw[:128], b'MZ' + b'\0' * 126):
            with self.assertRaises((ValueError, struct.error)):
                builder.inspect(changed)


@unittest.skipUnless(os.name == 'nt' or os.environ.get('SFNP_ASKPASS_WINE'),
                     'Actual Windows process or explicitly supplied Wine runtime required.')
class WindowsChildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = None
        supplied = os.environ.get('SFNP_ASKPASS_EXE')
        if supplied:
            cls.executable = Path(supplied).resolve()
        else:
            cls.temporary = tempfile.TemporaryDirectory(prefix='sfnp-askpass-native-')
            cls.executable = Path(cls.temporary.name) / 'askpass.exe'
            load('sfnp_native_builder', ROOT / 'tools/build_setup_askpass.py').build(cls.executable)
        cls.command = ([os.environ['SFNP_ASKPASS_WINE']] if os.environ.get('SFNP_ASKPASS_WINE') else []) + [str(cls.executable)]

    @classmethod
    def tearDownClass(cls):
        if cls.temporary:
            cls.temporary.cleanup()

    def invoke(self, port, prompt, token=TOKEN):
        return subprocess.run(self.command + [prompt], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env=dict(os.environ, SFNP_ASKPASS_PORT=str(port), SFNP_ASKPASS_TOKEN=token), timeout=15)

    def test_real_windows_socket_unicode_and_binary_stdout(self):
        prompt = 'Passwort für Främe 🥽: '
        answer = 'synthetisch 🔐'.encode('utf8')
        with broker(struct.pack('!I', len(answer)) + answer, chunks=True) as peer:
            result = self.invoke(peer['port'], prompt)
        self.assertTrue(result.returncode == 0 and result.stdout == answer + b'\n' and result.stderr == b'')
        self.assertEqual(peer.get('request'), b'SFNPASK1' + TOKEN.encode('ascii')
                         + struct.pack('!I', len(prompt.encode('utf8'))) + prompt.encode('utf8'))

    def test_real_windows_cancel_malformed_and_invalid_environment(self):
        for payload in (b'\xff\xff\xff\xff', struct.pack('!I', 1025), b'\x00',
                        struct.pack('!I', 6) + b'short', struct.pack('!I', 1) + b'\xff',
                        struct.pack('!I', 3) + b'a\nb', struct.pack('!I', 3) + b'a\x00b'):
            with self.subTest(response_size=len(payload)), broker(payload) as peer:
                result = self.invoke(peer['port'], 'Synthetic malformed broker')
            self.assertTrue(result.returncode != 0 and result.stdout == b'' and result.stderr == b'')
        for port, token in ((0, TOKEN), (65536, TOKEN), (1, 'g' * 64), (1, 'a' * 63)):
            result = self.invoke(port, 'Synthetic invalid configuration', token)
            self.assertTrue(result.returncode != 0 and result.stdout == b'' and result.stderr == b'')


if __name__ == '__main__':
    unittest.main()
