"""Portable discovery/presenter checks. No live network or Steam process is used."""
import contextlib
import importlib.util
import io
import ipaddress
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('sfnp_ui', ROOT / 'setup/steam_link_setup_ui.py')
ui = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = ui
spec.loader.exec_module(ui)
LAN = ui.Lan('192.168.5.22', 24, 'eth0', '192.168.5.1', 2, 10)


class Sources:
    def __init__(self, lan=LAN, named=(), neighbors=(), resolved=None):
        self.value, self.names, self.near = lan, list(named), list(neighbors)
        self.resolved = resolved or {}
        self.calls = []

    def resolve(self, name):
        self.calls.append(('resolve', name))
        return self.resolved.get(name, [])

    def lan(self):
        self.calls.append(('lan',))
        return self.value

    def named(self, lan):
        self.calls.append(('named', lan))
        return self.names

    def neighbors(self, lan):
        self.calls.append(('neighbors', lan))
        return self.near


def presenter(prompting=False, response='', tty=False):
    output = io.StringIO()
    output.isatty = lambda: tty
    return ui.Presenter(output, lambda prompt: response, lang='en', input_enabled=prompting)


class Discovery(unittest.TestCase):
    def find(self, validate, **kwargs):
        kwargs.setdefault('presenter', presenter())
        kwargs.setdefault('connector', lambda *args: True)
        return ui.find_frame(validate, **kwargs)

    def test_cached_private_fast_path_reuses_verified_value(self):
        source = Sources()
        owned = object()
        calls = []
        def validate(host, interactive=False):
            calls.append((host, interactive))
            return owned
        result = self.find(validate, cached_hosts=['192.168.5.90'], sources=source)
        self.assertIs(result.value, owned)
        self.assertEqual((result.host, result.origin), ('192.168.5.90', 'cached'))
        self.assertEqual(calls, [('192.168.5.90', False)])
        self.assertEqual(source.calls, [])  # No inventory or duplicate login.

    def test_cached_dns_and_named_dns_do_not_require_lan(self):
        for cached in (True, False):
            with self.subTest(cached=cached):
                source = Sources(lan=None, named=['192.168.5.90'], resolved={'frame.local': ['192.168.5.90']})
                result = self.find(lambda *_a, **_k: 'Frame inventory', cached_hosts=['frame.local'] if cached else (), sources=source)
                self.assertEqual(result.host, '192.168.5.90')
                self.assertNotIn(('lan',), source.calls)
                self.assertEqual(result.origin, 'cached' if cached else 'named')

    def test_only_known_hint_gets_one_automatic_interactive_prompt(self):
        calls = []
        def validate(host, interactive=False):
            calls.append((host, interactive))
            return 'verified' if interactive else None
        result = self.find(validate, cached_hosts=['192.168.5.90', '192.168.5.91'], sources=Sources(), presenter=presenter(True))
        self.assertEqual(result.host, '192.168.5.90')
        self.assertEqual(calls, [('192.168.5.90', False), ('192.168.5.91', False), ('192.168.5.90', True)])

    def test_unknown_ssh_hosts_never_become_identity_or_password_prompts(self):
        calls = []
        source = Sources(neighbors=['192.168.5.90', '192.168.5.91'])
        with mock.patch.object(ui, 'scan', return_value=['192.168.5.92']):
            result = self.find(lambda host, interactive=False: calls.append((host, interactive)), sources=source, presenter=presenter(True))
        self.assertIsNone(result)
        self.assertEqual(len(calls), 3)
        self.assertTrue(all(interactive is False for _, interactive in calls))

    def test_scan_and_batch_authentication_are_bounded(self):
        calls = []
        ips = ['192.168.5.' + str(n) for n in range(30, 90)]
        with mock.patch.object(ui, 'scan', side_effect=AssertionError('authentication budget exhausted')):
            result = self.find(lambda host, interactive=False: calls.append((host, interactive)), sources=Sources(neighbors=ips))
        self.assertIsNone(result)
        self.assertEqual(len(calls), ui.MAX_VALIDATIONS)

    def test_manual_is_last_and_private_only(self):
        for text, expected in [('192.168.5.90', True), ('8.8.8.8', False), ('192.168.5.0/24', False), ('', False)]:
            with self.subTest(text=text):
                calls = []
                source = Sources(lan=None)
                result = self.find(lambda host, interactive=False: calls.append((host, interactive)) or 'verified', sources=source, presenter=presenter(True, text))
                self.assertEqual(result is not None, expected)
                self.assertEqual(calls, [('192.168.5.90', True)] if expected else [])
                self.assertIn(('lan',), source.calls)

    def test_no_prompts_when_input_disabled_even_when_output_is_tty(self):
        calls = []
        p = presenter(False, tty=True)
        with mock.patch.object(ui, 'scan', return_value=[]):
            result = self.find(lambda host, interactive=False: calls.append((host, interactive)), cached_hosts=['192.168.5.90'], sources=Sources(), presenter=p)
        self.assertIsNone(result)
        self.assertEqual(calls, [('192.168.5.90', False)])
        self.assertIsNone(p._thread)

    def test_output_redirection_does_not_suppress_explicit_input(self):
        p = presenter(True, '192.168.5.90', tty=False)
        result = self.find(lambda *_a, **_k: 'verified', sources=Sources(lan=None), presenter=p)
        self.assertIsNotNone(result)
        self.assertNotIn('\r', p.stream.getvalue())

    def test_unreachable_and_invalid_hints_never_reach_validator(self):
        source = Sources(lan=None, named=['127.0.0.1', '8.8.8.8', '192.168.5.90'])
        def connector(*args):
            raise OSError('network unavailable')
        result = self.find(mock.Mock(side_effect=AssertionError('must not validate')), sources=source, cached_hosts=['127.0.0.1'], connector=connector)
        self.assertIsNone(result)

    def test_validation_error_falls_back_without_echoing_sensitive_exception(self):
        p = presenter()
        result = self.find(mock.Mock(side_effect=ValueError('do not print this password')), sources=Sources(lan=None, named=['192.168.5.90']), presenter=p)
        self.assertIsNone(result)
        self.assertNotIn('password', p.stream.getvalue())


class BoundsAndSources(unittest.TestCase):
    def test_only_physical_private_lans_can_sweep(self):
        invalid = [
            ui.Lan('8.8.8.2', 24, 'eth0', '8.8.8.1'),
            ui.Lan('127.0.0.2', 24, 'eth0', '127.0.0.1'),
            ui.Lan('169.254.1.2', 24, 'eth0', '169.254.1.1'),
            ui.Lan('192.168.5.22', 24, 'WireGuard', '192.168.5.1'),
            ui.Lan('192.168.5.22', 24, 'vEthernet (WSL)', '192.168.5.1'),
            ui.Lan('192.168.5.22', 8, 'eth0', '192.168.5.1'),
            ui.Lan('192.168.5.22', 24, 'eth0', '192.168.6.1'),
        ]
        callback = mock.Mock(side_effect=AssertionError('must not scan'))
        for lan in invalid:
            with self.subTest(lan=lan):
                self.assertFalse(ui.eligible_lan(lan))
                self.assertEqual(ui.scan(lan, callback), [])
        self.assertTrue(ui.eligible_lan(LAN))

    def test_broad_private_lan_scans_only_own_24_with_at_most_16_workers(self):
        seen, threads = [], set()
        lock = threading.Lock()
        def connect(address, port, timeout):
            with lock:
                seen.append((address, port, timeout))
                threads.add(threading.get_ident())
            return address == '10.21.9.90'
        result = ui.scan(ui.Lan('10.21.9.22', 8, 'wlan0', '10.0.0.1'), connect)
        self.assertEqual(result, ['10.21.9.90'])
        self.assertLessEqual(len(seen), 254)
        self.assertLessEqual(len(threads), 16)
        self.assertTrue(all(ipaddress.ip_address(a) in ipaddress.ip_network('10.21.9.0/24') for a, _, _ in seen))
        self.assertTrue(all(port == 22 and timeout == .15 for _, port, timeout in seen))
        self.assertNotIn('10.21.9.22', [a for a, _, _ in seen])

    def test_windows_uses_fixed_system_powershell_and_rejects_virtual(self):
        calls = []
        rows = [dict(address='192.168.5.22', prefix=24, interface='Ethernet', gateway='192.168.5.1', index=2, metric=10, physical=True, virtual=False),
                dict(address='10.0.0.2', prefix=24, interface='VPN', gateway='10.0.0.1', index=3, metric=1, physical=True, virtual=False)]
        def run(argv, **kwargs):
            calls.append(argv)
            return json.dumps(rows).encode()
        source = ui.SystemSources(system='Windows', run=run, system_directory=r'C:\Windows\System32')
        result = source.lan()
        self.assertEqual(result.interface, 'Ethernet')
        self.assertEqual(calls[0][0], r'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe')
        self.assertEqual(calls[0][1:5], ['-NoLogo', '-NoProfile', '-NonInteractive', '-Command'])
        self.assertEqual(calls[0][-1], ui.WINDOWS_LAN)
        self.assertNotIn('Set-', ui.WINDOWS_LAN)

    def test_windows_primary_interface_lowest_metric_and_matching_neighbors(self):
        rows = [dict(address='192.168.5.22', prefix=24, interface='Ethernet', gateway='192.168.5.1', index=2, metric=10, physical=True, virtual=False),
                dict(address='192.168.5.23', prefix=24, interface='Wi-Fi', gateway='192.168.5.1', index=3, metric=40, physical=True, virtual=False)]
        def run(argv, **kwargs):
            values = rows if argv[-1] == ui.WINDOWS_LAN else [{'address': '192.168.5.90', 'index': 2}, {'address': '192.168.5.91', 'index': 3}]
            return json.dumps(values).encode()
        source = ui.SystemSources(system='Windows', run=run, system_directory=r'C:\Windows\System32')
        lan = source.lan()
        self.assertEqual(lan.index, 2)
        self.assertEqual(source.neighbors(lan), ['192.168.5.90'])

    def test_dns_resolution_bounded_reaped_child_and_private_results(self):
        calls = []
        def run(argv, **kwargs):
            calls.append((argv, kwargs))
            return b'["8.8.8.8","192.168.5.90","192.168.5.90"]'
        source = ui.SystemSources(system='Other', run=run)
        self.assertEqual(source.resolve('frame.local'), ['192.168.5.90'])
        self.assertEqual(source.resolve('frame.local'), ['192.168.5.90'])
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][1], dict(timeout=1.25, limit=2048))
        self.assertEqual(calls[0][0][-1], 'frame.local')
        self.assertEqual(source.resolve('frame.local; touch nope'), [])
        self.assertEqual(len(calls), 1)

    def test_common_dns_survives_missing_lan_table(self):
        source = ui.SystemSources(system='Other', run=lambda argv, **kw: b'["192.168.5.90"]')
        self.assertEqual(source.named(None), ['192.168.5.90'])

    def test_linux_requires_hardware_device_and_rejects_unknown_metadata(self):
        calls = []
        routes = [{'dev': 'tailscale0', 'gateway': '10.0.0.1'}, {'dev': 'eth0', 'gateway': '192.168.5.1', 'metric': 10}]
        addresses = [{'ifindex': 2, 'link_type': 'ether', 'addr_info': [{'family': 'inet', 'local': '192.168.5.22', 'prefixlen': 24}]}]
        def run(argv, **kwargs):
            calls.append(argv)
            return json.dumps(routes if 'route' in argv else addresses).encode()
        source = ui.SystemSources(system='Linux', run=run, device_exists=lambda name: name == 'eth0')
        source.ip_tool = '/usr/sbin/ip'
        self.assertEqual(source.lan(), LAN)
        self.assertNotIn('tailscale0', [a for call in calls for a in call])
        self.assertTrue(all(call[0] == '/usr/sbin/ip' for call in calls))
        addresses[0]['linkinfo'] = {'info_kind': 'bridge'}
        self.assertIsNone(source.lan())
        addresses[0]['linkinfo'] = None
        self.assertIsNone(source.lan())
        addresses[0].pop('linkinfo')
        routes[1]['metric'] = 'invalid'
        self.assertIsNone(source.lan())
        source.device_exists = lambda name: False
        self.assertIsNone(source.lan())

    def test_json_bounds_and_private_input_types(self):
        self.assertEqual(ui.json_rows(b'[' + b'{} ,' * 100 + b'{}]'), [])
        self.assertEqual(ui.json_rows(b'not JSON'), [])
        self.assertEqual(ui.json_rows(b'[]' * 40000), [])
        self.assertIsNone(ui.private_ip(3232235777))

    def test_actual_fixed_child_output_limit_timeout_and_reaping(self):
        self.assertEqual(ui.command_output([sys.executable, '-I', '-c', 'print("okay")']), b'okay' + os.linesep.encode())
        self.assertEqual(ui.command_output([sys.executable, '-I', '-c', 'print("x"*1000000)'], limit=32), b'')
        start = time.monotonic()
        self.assertEqual(ui.command_output([sys.executable, '-I', '-c', 'import time;time.sleep(5)'], timeout=.1), b'')
        self.assertLess(time.monotonic() - start, 2)
        self.assertFalse(any(t.name == 'sfnp-discovery-output' for t in threading.enumerate()))


class DnsPackets(unittest.TestCase):
    @staticmethod
    def packet(name='frame.local', address='192.168.5.90', pointer=False):
        encoded = b''.join(bytes([len(part)]) + part.encode() for part in name.split('.')) + b'\0'
        if pointer:
            header = struct.pack('!6H', 0, 0x8400, 1, 1, 0, 0)
            question = encoded + struct.pack('!HH', 1, 1)
            encoded = b'\xc0\x0c'
        else:
            header = struct.pack('!6H', 0, 0x8400, 0, 1, 0, 0)
            question = b''
        return header + question + encoded + struct.pack('!HHIH', 1, 0x8001, 120, 4) + socket.inet_aton(address)

    def test_requested_a_record_and_compression(self):
        for pointer in (False, True):
            self.assertEqual(ui.mdns_addresses(self.packet(pointer=pointer), ui.NAMES), ['192.168.5.90'])

    def test_no_foreign_public_truncated_or_pointer_cycle_records(self):
        for packet in (self.packet('printer.local'), self.packet(address='8.8.8.8'), self.packet()[:-1],
                       struct.pack('!6H', 0, 0x8400, 0, 1, 0, 0) + b'\xc0\x0c', b'x' * 4097):
            self.assertEqual(ui.mdns_addresses(packet, ui.NAMES), [])

    def test_multicast_bound_to_one_physical_interface_and_fixed_names(self):
        class Client:
            def __init__(self):
                self.sent, self.options, self.bound = [], [], None
            def __enter__(self):
                return self
            def __exit__(self, *args):
                pass
            def bind(self, value):
                self.bound = value
            def setsockopt(self, *args):
                self.options.append(args)
            def settimeout(self, value):
                self.timeout = value
            def sendto(self, *args):
                self.sent.append(args)
            def recvfrom(self, limit):
                if len(self.sent) == 3:
                    self.sent.append(('received',))
                    return DnsPackets.packet(), ('192.168.5.90', 5353)
                raise OSError('done')
        client = Client()
        with mock.patch.object(ui.socket, 'socket', return_value=client):
            ui.mdns(LAN)
        self.assertEqual(client.bound, (LAN.address, 0))
        self.assertEqual(client.options[0], (socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(LAN.address)))
        self.assertEqual(len(client.sent[:3]), 3)
        self.assertTrue(all(value[1] == ('224.0.0.251', 5353) for value in client.sent[:3]))
        self.assertEqual(client.timeout, .15)


class Presentation(unittest.TestCase):
    def test_locale_automatic_german_and_english_fallback(self):
        for value, expected in [('de_DE', 'de'), ('German_Germany', 'de'), ('en_US', 'en'), ('fr_FR', 'en')]:
            with mock.patch.object(ui.locale, 'getlocale', return_value=(value, 'UTF-8')):
                self.assertEqual(ui.language(), expected)
        with mock.patch.object(ui.locale, 'getlocale', return_value=(None, None)), mock.patch.dict(os.environ, {'LC_ALL': 'de_DE.UTF-8'}):
            self.assertEqual(ui.language(), 'de')
        self.assertEqual(ui.language('de-DE'), 'de')
        self.assertEqual(ui.language('en'), 'en')

    def test_status_copy_has_equal_en_de_keys_and_no_manual_dependency_steps(self):
        self.assertEqual(set(ui.TEXT['en']), set(ui.TEXT['de']))
        for key in ('pc_check', 'pc_ready', 'pc_partial', 'receiver_ready', 'receiver_changed', 'check_finish', 'report', 'done_prompt'):
            self.assertTrue(ui.TEXT['en'][key])
            self.assertTrue(ui.TEXT['de'][key])
        self.assertNotIn('pip install', str(ui.TEXT))
        self.assertNotIn('apt install', str(ui.TEXT))

    def test_pipe_has_plain_bounded_output_and_no_progress_thread(self):
        p = presenter()
        p.banner()
        with p.progress('search'):
            with p.progress('verify'):
                pass
        text = p.stream.getvalue()
        self.assertNotIn('\r', text)
        self.assertNotIn('\x1b', text)
        self.assertIn('nothing will be installed', text)
        self.assertLess(len(text), 600)
        self.assertIsNone(p._thread)
        box = text.splitlines()[:3]
        self.assertEqual(len(box[0]), len(box[1]))
        self.assertEqual(len(box[1]), len(box[2]))

    def test_suspend_joins_animation_before_prompt_writes(self):
        p = presenter(True, tty=True)
        with p.progress('search'):
            with p.suspend():
                before = p.stream.getvalue()
                # Longer than two spinner iterations, but no output is legal.
                time.sleep(.27)
                self.assertEqual(p.stream.getvalue(), before)
            with p.suspend():
                self.assertEqual(p._paused, 1)
                with p.suspend():
                    self.assertEqual(p._paused, 2)
        self.assertIsNone(p._thread)
        self.assertFalse(any(t.name == 'sfnp-progress' for t in threading.enumerate()))

    def test_real_python_pipe_never_prompts_or_emits_ansi(self):
        code = ('import sys,time;sys.path.insert(0,' + repr(str(ROOT / 'setup')) + ');'
                'from steam_link_setup_ui import Presenter; p=Presenter(lang="en");p.banner();'
                '\nwith p.progress("search"):time.sleep(.15)\n'
                'print("INPUT="+p.ask("manual"))')
        child = subprocess.run([sys.executable, '-I', '-c', code], input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
        self.assertEqual(child.returncode, 0, child.stderr)
        output = child.stdout.replace(b'\r\n', b'\n')
        self.assertIn(b'INPUT=\n', output)
        self.assertNotIn(b'\r', output)
        self.assertNotIn(b'\x1b', output)

    @unittest.skipIf(os.name == 'nt', 'POSIX PTY check; Windows uses redirected-stream contract above')
    def test_real_python_pty_spinner_stops_for_input(self):
        import pty
        master, slave = pty.openpty()
        code = ('import sys,time;sys.path.insert(0,' + repr(str(ROOT / 'setup')) + ');'
                'from steam_link_setup_ui import Presenter;p=Presenter(lang="en");'
                '\nwith p.progress("search"):\n time.sleep(.2)\n p.ask("manual")\n'
                'print("FINISHED")')
        child = subprocess.Popen([sys.executable, '-I', '-c', code], stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        data = bytearray()
        def read_until(needle, timeout=3):
            deadline = time.monotonic() + timeout
            while needle not in data and time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], .1)
                if ready:
                    try:
                        data.extend(os.read(master, 4096))
                    except OSError:
                        break
            self.assertIn(needle, data)
        try:
            read_until(b'Last option -')
            # Drain the rest of the prompt, then observe its paused interval.
            while select.select([master], [], [], .05)[0]:
                data.extend(os.read(master, 4096))
            self.assertFalse(select.select([master], [], [], .3)[0])
            os.write(master, b'\n')
            read_until(b'FINISHED')
            self.assertEqual(child.wait(timeout=2), 0)
            self.assertNotIn(b'\x1b', data)
        finally:
            if child.poll() is None:
                child.kill()
            child.wait()
            os.close(master)


if __name__ == '__main__':
    unittest.main()
