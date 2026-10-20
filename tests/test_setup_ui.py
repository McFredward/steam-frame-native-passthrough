"""Portable discovery/presenter checks. No live network or Steam process is used."""
import contextlib
import importlib.util
import io
import ipaddress
import itertools
import json
import os
from pathlib import Path
import select
import socket
import string
import struct
import subprocess
import sys
import threading
import time
import tempfile
import types
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


def presenter(prompting=False, response=None, tty=False):
    output = io.StringIO()
    output.isatty = lambda: tty
    # A failed discovery now requires an explicit menu choice. The default
    # fixture chooses PC-only; manual tests supply their entire input sequence.
    answers=iter(['3'] if response is None else response if isinstance(response,(list,tuple)) else [response])
    def read(prompt):
        try:value=next(answers)
        except StopIteration:raise EOFError()
        if isinstance(value,BaseException):raise value
        return value
    return ui.Presenter(output, read, lang='en', input_enabled=prompting)


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
                result = self.find(lambda host, interactive=False: calls.append((host, interactive)) or 'verified', sources=source, presenter=presenter(True, ['2',text,'3']))
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
        p = presenter(True, ['2','192.168.5.90'], tty=False)
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


class DiscoveryRetry(unittest.TestCase):
    def scripted(self, responses, **kwargs):
        p=presenter(True,responses)
        choices=[]
        original=p.choose_action
        def choose(actions,recommended=None):
            choices.append((tuple(actions),recommended))
            return original(actions,recommended)
        p.choose_action=choose
        for key,value in kwargs.items():setattr(p,key,value)
        return p,choices

    def find(self, validate, p, source, **kwargs):
        return ui.find_frame(validate,presenter=p,sources=source,
                             connector=lambda *_:True,**kwargs)

    def test_failure_offers_retry_manual_pc_only_and_exit_before_an_ip_prompt(self):
        p,choices=self.scripted(['3'])
        source=Sources(lan=None)
        self.assertIsNone(self.find(lambda *_a,**_k:None,p,source))
        self.assertEqual(choices,[(('retry','manual','check','exit'),'retry')])
        self.assertNotIn(ui.TEXT['en']['manual'],p.stream.getvalue())
        self.assertIn(ui.TEXT['en']['discovery_pc_only'],p.stream.getvalue())
        self.assertIn(ui.TEXT['en']['pc_only'],p.stream.getvalue())
        self.assertEqual(source.calls,[('named',None),('lan',)])

    def test_explicit_retry_resets_each_pass_budget_and_seen_hints(self):
        class ResetSources(Sources):
            attempt=0
            def reset_discovery(self):
                self.attempt+=1
                self.calls.append(('reset',))
        source=ResetSources(lan=None,named=['192.168.5.'+str(i) for i in range(30,80)])
        p,choices=self.scripted(['1','3'])
        checks=[]
        self.assertIsNone(self.find(lambda address,interactive=False:checks.append((source.attempt,address,interactive)),p,source))
        self.assertEqual(choices,[(('retry','manual','check','exit'),'retry')]*2)
        self.assertEqual(source.attempt,1)
        for attempt in (0,1):
            self.assertEqual(len([r for r in checks if r[0]==attempt and not r[2]]),ui.MAX_VALIDATIONS)
            self.assertEqual(len([r for r in checks if r[0]==attempt and r[2]]),1)
        self.assertEqual([r[1:] for r in checks if r[0]==0],[r[1:] for r in checks if r[0]==1])

    def test_real_system_dns_cache_is_discarded_for_a_requested_retry(self):
        address=['192.168.5.90'];dns=[]
        def run(argv,**kwargs):
            dns.append(argv[-1]);return json.dumps(address).encode()
        source=ui.SystemSources(system='Other',run=run)
        p,choices=self.scripted(['1'])
        read=p.input_fn
        def change(prompt):
            address[0]='192.168.5.91';return read(prompt)
        p.input_fn=change
        result=self.find(lambda value,interactive=False:'verified' if value==address[0] and value.endswith('.91') else None,p,source)
        self.assertEqual(result.host,'192.168.5.91')
        self.assertEqual(len(dns),len(ui.NAMES)*2)
        self.assertEqual(source.resolve('frame.local'),['192.168.5.91'])
        self.assertEqual(len(dns),len(ui.NAMES)*2)

    def test_injected_sources_without_reset_can_still_find_a_new_device(self):
        source=Sources(lan=None)
        p,choices=self.scripted(['1'])
        read=p.input_fn
        def change(prompt):
            source.names=['192.168.5.91'];return read(prompt)
        p.input_fn=change
        result=self.find(lambda *_a,**_k:'verified',p,source)
        self.assertEqual(result.host,'192.168.5.91')
        self.assertEqual(choices,[(('retry','manual','check','exit'),'retry')])

    def test_invalid_and_unverified_manual_addresses_allow_another_explicit_try(self):
        p,choices=self.scripted(['2','8.8.8.8','2','192.168.5.90','2','192.168.5.91'])
        calls=[]
        def validate(address,interactive=False):
            calls.append((address,interactive))
            return 'verified' if address.endswith('.91') else None
        result=self.find(validate,p,Sources(lan=None))
        self.assertEqual(result.host,'192.168.5.91')
        self.assertEqual(calls,[('192.168.5.90',True),('192.168.5.91',True)])
        self.assertEqual(len(choices),3)
        self.assertIn(ui.TEXT['en']['invalid'],' '.join(p.stream.getvalue().split()))
        self.assertIn(ui.TEXT['en']['unverified'],' '.join(p.stream.getvalue().split()))
        self.assertNotIn(ui.TEXT['en']['pc_only'],p.stream.getvalue())

    def test_manual_failure_can_return_to_a_fresh_automatic_search_without_secret_output(self):
        source=Sources(lan=None)
        p,choices=self.scripted(['2','192.168.5.90','1'])
        read=p.input_fn
        def change(prompt):
            value=read(prompt)
            if value=='1':source.names=['192.168.5.91']
            return value
        p.input_fn=change
        def validate(address,interactive=False):
            if interactive:raise ValueError('password or untrusted SSH output must not leak')
            return 'verified'
        result=self.find(validate,p,source)
        self.assertEqual(result.host,'192.168.5.91')
        self.assertEqual(result.origin,'named')
        self.assertNotIn('password or untrusted',p.stream.getvalue())
        self.assertEqual(len(choices),2)

    def test_empty_manual_entry_goes_back_without_implicitly_continuing(self):
        p,choices=self.scripted(['2','','3'])
        source=Sources(lan=None)
        self.assertIsNone(self.find(mock.Mock(side_effect=AssertionError('no validation')),p,source))
        self.assertEqual(len(choices),2)
        self.assertEqual(source.calls,[('named',None),('lan',)])

    def test_exit_escape_and_input_interrupt_cancel_instead_of_pc_only(self):
        for response in (['4'],['0'],[KeyboardInterrupt()]):
            with self.subTest(response=response):
                p,choices=self.scripted(response)
                with self.assertRaises(KeyboardInterrupt):
                    self.find(lambda *_a,**_k:None,p,Sources(lan=None))
                self.assertEqual(len(choices),1)
                self.assertNotIn(ui.TEXT['en']['pc_only'],p.stream.getvalue())

    def test_inline_authentication_keeps_the_textual_application_on_screen(self):
        p,choices=self.scripted([],inline_ssh=True)
        p.suspend=mock.Mock(side_effect=AssertionError('native Textual authentication must remain visible'))
        def validate(address,interactive=False):
            return 'verified' if interactive else None
        result=self.find(validate,p,Sources(lan=None,named=['192.168.5.90']))
        self.assertEqual(result.host,'192.168.5.90')
        self.assertEqual(choices,[])
        p.suspend.assert_not_called()

    def test_plain_authentication_still_protects_its_terminal_prompt(self):
        p,choices=self.scripted([])
        self.assertFalse(p.inline_ssh)
        def validate(address,interactive=False):
            if interactive:
                self.assertGreater(p._paused,0)
                return 'verified'
            self.assertEqual(p._paused,0)
        result=self.find(validate,p,Sources(lan=None,named=['192.168.5.90']))
        self.assertEqual(result.host,'192.168.5.90')
        self.assertEqual(p._paused,0)

    def test_password_cancellation_is_not_swallowed_or_retried(self):
        p,choices=self.scripted([],inline_ssh=True)
        calls=[]
        def validate(address,interactive=False):
            calls.append(interactive)
            if interactive:raise KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            self.find(validate,p,Sources(lan=None,named=['192.168.5.90']))
        self.assertEqual(calls,[False,True])
        self.assertEqual(choices,[])

    def test_noninteractive_error_is_bounded_and_cannot_prompt_or_retry(self):
        p=presenter(False)
        p.choose_action=mock.Mock(side_effect=AssertionError('never prompt'))
        p.ask=mock.Mock(side_effect=AssertionError('never prompt'))
        source=Sources(lan=None,named=['192.168.5.90'])
        calls=[]
        def validate(address,interactive=False):
            calls.append(interactive);raise OSError('sensitive exception text')
        self.assertIsNone(self.find(validate,p,source))
        self.assertEqual(calls,[False])
        self.assertNotIn('sensitive',p.stream.getvalue())
        p.choose_action.assert_not_called()
        p.ask.assert_not_called()

    def test_no_manual_entry_is_offered_when_the_caller_disallows_it(self):
        p,choices=self.scripted(['2'])
        self.assertIsNone(self.find(lambda *_a,**_k:None,p,Sources(lan=None),allow_manual=False))
        self.assertEqual(choices,[(('retry','check','exit'),'retry')])

    def test_unreachable_and_infinite_hint_sources_stay_bounded_without_writes(self):
        consumed=[]
        def resolve(_name):
            for value in itertools.repeat('192.168.5.90'):
                consumed.append(value);yield value
        source=types.SimpleNamespace(resolve=resolve,named=lambda _:(),lan=lambda:None)
        checks=[]
        with tempfile.TemporaryDirectory(prefix='sfnp-discovery-readonly-') as directory:
            folder=Path(directory);sentinel=folder/'original.txt';sentinel.write_bytes(b'unchanged')
            before={p.relative_to(folder).as_posix():p.read_bytes() for p in folder.rglob('*') if p.is_file()}
            result=ui.find_frame(mock.Mock(side_effect=AssertionError('unreachable')),sources=source,
                cached_hosts=itertools.repeat('frame.local'),presenter=presenter(False),
                connector=lambda *args:checks.append(args) or False)
            after={p.relative_to(folder).as_posix():p.read_bytes() for p in folder.rglob('*') if p.is_file()}
            self.assertIsNone(result);self.assertEqual(before,after)
        self.assertEqual(len(consumed),32)
        self.assertEqual(len(checks),1)

    def test_source_error_offers_one_choice_instead_of_an_automatic_loop(self):
        p,choices=self.scripted(['3'])
        source=Sources(lan=None)
        source.named=mock.Mock(side_effect=OSError('private diagnostic'))
        self.assertIsNone(self.find(lambda *_a,**_k:None,p,source))
        self.assertEqual(source.named.call_count,1)
        self.assertEqual(len(choices),1)
        self.assertNotIn('private diagnostic',p.stream.getvalue())


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
    def test_installer_and_patch_versions_are_separate_in_inventory(self):
        for lang in ('en','de'):
            output=io.StringIO()
            presenter=ui.Presenter(lang=lang,stream=output,input_enabled=False)
            presenter.inventory(pc={'installed':True,'installed_version':'0.2.11','target_version':'0.2.11'},
                frame={'installed':True,'installed_version':'0.2.11','target_version':'0.2.11'},report={'installer_version':'0.2.12'})
            text=output.getvalue()
            self.assertIn('Installer: 0.2.12',text)
            self.assertIn(presenter.text('version_installed').format(version='0.2.11'),text)
            self.assertIn(presenter.text('version_target').format(version='0.2.11'),text)
            self.assertNotIn(presenter.text('version_target').format(version='0.2.12'),text)
            self.assertIsNone(ui.installer_label(presenter.text,{'installer_version':'0.2.12\nFORGED'}))

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
        self.assertIn('manually', ui.TEXT['en']['remove_pending'])
        self.assertIn('manuell', ui.TEXT['de']['remove_pending'])

    def test_localized_format_fields_match_for_every_installer_message(self):
        formatter = string.Formatter()
        for key in ui.TEXT['en']:
            with self.subTest(message=key):
                fields = lambda value: sorted(field for _, field, _, _ in formatter.parse(value)
                                              if field is not None)
                self.assertEqual(fields(ui.TEXT['en'][key]), fields(ui.TEXT['de'][key]))

    def test_pipe_has_plain_bounded_output_and_no_progress_thread(self):
        p = presenter()
        p.banner()
        with p.progress('search'):
            with p.progress('verify'):
                pass
        text = p.stream.getvalue()
        self.assertNotIn('\r', text)
        self.assertNotIn('\x1b', text)
        self.assertIn('no patch will be installed', text)
        self.assertLess(len(text), 600)
        self.assertIsNone(p._thread)
        box = [line for line in text.splitlines() if line.startswith(('  +', '  |'))]
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
        child = subprocess.run([sys.executable, '-I', '-B', '-c', code], input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
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
        child = subprocess.Popen([sys.executable, '-I', '-B', '-c', code], stdin=slave, stdout=slave, stderr=slave)
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
            read_until(b'Frame IP from')
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


class ActionChooser(unittest.TestCase):
    def make(self, responses=(), *, lang='en', width=32, prompting=True):
        answers = iter(responses)
        output = io.StringIO()
        prompts = []
        def read(prompt):
            prompts.append(prompt)
            try:
                value = next(answers)
            except StopIteration:
                raise EOFError()
            if isinstance(value, BaseException):
                raise value
            return value
        p = ui.Presenter(output, read, lang=lang, input_enabled=prompting, width=width)
        return p, output, prompts

    def assert_width(self, text, width):
        self.assertTrue(all(len(line) < width for line in text.splitlines()), text)
        self.assertNotIn('\x1b', text)
        self.assertNotIn('\r', text)

    def test_narrow_en_de_sections_status_and_paths_are_separate_and_wrapped(self):
        for lang in ('en', 'de'):
            for width in (20, 32, 80):
                with self.subTest(lang=lang, width=width):
                    p, output, _ = self.make(lang=lang, width=width)
                    p.banner()
                    p.section('pc_check')
                    p.status('pc_partial', state='warning')
                    p.path(r'C:\Games\Steam\very-long-report-folder\setup-inspection.json')
                    text = output.getvalue()
                    self.assert_width(text, width)
                    self.assertIn('[!]', text)
                    self.assertIn('\n\n', text)
                    label = p.text('report').strip()
                    if len(label) + 2 < width:
                        self.assertIn('  ' + label + '\n    C:', text)

    def test_numbered_uninstall_is_usable_even_with_redirected_output(self):
        p, output, prompts = self.make(['2'])
        result = p.choose_action(['check', 'uninstall', 'exit'], recommended='check')
        self.assertEqual(result, 'uninstall')
        self.assertIn('2. Uninstall', output.getvalue())
        self.assertNotIn('1. Install', output.getvalue())
        self.assertEqual(len(prompts), 1)
        self.assert_width(output.getvalue(), 32)

    def test_enter_defaults_to_check_unless_offered_recommendation(self):
        for actions, recommended, expected in [
            (['uninstall', 'check', 'exit'], None, 'check'),
            (['uninstall', 'check', 'exit'], 'uninstall', 'uninstall'),
            (['check', 'exit'], 'install', 'check'),
            (['repair', 'exit'], None, 'exit'),
            (['install'], None, None),
        ]:
            with self.subTest(actions=actions, recommended=recommended):
                p, output, _ = self.make([''])
                self.assertEqual(p.choose_action(actions, recommended), expected)
                self.assertNotIn('Install [recommended]', output.getvalue())

    def test_offered_install_update_repair_labels_have_localized_descriptions(self):
        actions = ['install', 'update', 'repair', 'uninstall', 'check', 'exit', 'manual']
        for lang in ('en', 'de'):
            p, output, _ = self.make(['6'], lang=lang, width=42)
            self.assertEqual(p.choose_action(actions), 'exit')
            text = output.getvalue()
            for action in actions:
                self.assertIn(p.text('action_' + action), text)
            self.assert_width(text, 42)

    def test_cancel_eof_interrupt_and_long_input_never_start_an_action(self):
        for responses in (['0'], ['q'], ['cancel'], ['abbrechen'], [], [KeyboardInterrupt()], ['9', 'x' * 10000, '1' * 10000]):
            p, output, prompts = self.make(responses)
            self.assertIsNone(p.choose_action(['uninstall', 'check'], recommended='uninstall'))
            self.assertLessEqual(len(prompts), 3)
            self.assertNotIn('x' * 20, output.getvalue())

    def test_invalid_then_valid_selection_and_empty_set(self):
        p, output, prompts = self.make(['oops', '2'])
        self.assertEqual(p.choose_action(['repair', 'check']), 'check')
        self.assertEqual(len(prompts), 2)
        self.assertIn('numbers shown', output.getvalue())
        p, output, prompts = self.make([])
        self.assertIsNone(p.choose_action([]))
        self.assertEqual(prompts, [])
        self.assertEqual(output.getvalue(), '')
        for actions in (['unknown'], ['check', 'check'], 'uninstall'):
            with self.assertRaises(ValueError):
                p.choose_action(actions)

    def test_noninteractive_never_selects_recommended_mutating_action(self):
        p, output, prompts = self.make(['1'], prompting=False)
        self.assertIsNone(p.choose_action(['uninstall', 'check'], recommended='uninstall'))
        self.assertFalse(p.confirm_uninstall())
        self.assertEqual(prompts, [])
        self.assertEqual(output.getvalue(), '')

    def test_uninstall_confirmation_is_explicit_localized_and_cancel_safe(self):
        for lang, answer, expected in [('en', 'REMOVE', True), ('de', 'entfernen', True),
                                       ('en', '', False), ('de', 'yes', False),
                                       ('en', EOFError(), False)]:
            p, output, _ = self.make([answer], lang=lang)
            self.assertEqual(p.confirm_uninstall(), expected)
            self.assert_width(output.getvalue(), 32)

    def test_untested_confirmation_shows_detected_versions_and_requires_explicit_yes(self):
        runtime={'steamvr':{'version':'2.19.1','build':'1800000000'},'steam_link':{}}
        for lang in ('en','de'):
            for answer,accepted in (('',False),('n',False),('y',True)):
                p,output,_=self.make([answer],lang=lang,width=80)
                p.untested_runtime(pc=runtime,frame=runtime)
                self.assertEqual(p.confirm('bridge_untested_confirm'),accepted)
                text=output.getvalue()
                self.assertIn('2.19.1 (1800000000)',text)
                self.assertIn(p.text('version_pc'),text)
                self.assertIn(p.text('version_frame'),text)
                self.assertIn(p.text('bridge_untested_versions'),text)
                self.assertIn(p.text('runtime_bundled'),text)
                self.assertNotIn('Steam Link: '+p.text('runtime_unknown'),text)
        p,output,_=self.make([],width=80)
        p.untested_runtime(pc={'steamvr':{'version':'2.19.1\nFORGED'}},frame=None)
        self.assertNotIn('FORGED',output.getvalue())

    def test_menu_and_confirmation_keep_parent_spinner_paused(self):
        p, output, _ = self.make(['1', 'REMOVE'])
        def read(prompt):
            self.assertGreater(p._paused, 0)
            return '1' if 'choice' in prompt.lower() else 'REMOVE'
        p.input_fn = read
        with p.progress('pc_check'):
            self.assertEqual(p.choose_action(['uninstall']), 'uninstall')
            self.assertTrue(p.confirm_uninstall())
        self.assertEqual(p._paused, 0)
        self.assertIsNone(p._thread)

    def test_terminal_width_is_read_from_output_stream_and_spinner_stays_on_one_line(self):
        p, output, _ = self.make([])
        p._terminal_width = None
        output.fileno = lambda: 41
        with mock.patch.object(ui.os, 'get_terminal_size', return_value=os.terminal_size((26, 24))) as size:
            p.say('pc_partial')
            size.assert_called_with(41)
        self.assert_width(output.getvalue(), 26)
        with mock.patch.object(ui.os, 'get_terminal_size', return_value=os.terminal_size((0, 0))):
            self.assertEqual(p.columns(), 79)
        p._terminal_width = 24
        p.interactive = True
        output.seek(0)
        output.truncate()
        with p.progress('pc_partial'):
            time.sleep(.15)
        self.assertTrue(all(len(chunk) < 24 for line in output.getvalue().split('\n') for chunk in line.split('\r')))
        self.assertIsNone(p._thread)

    def test_real_pipe_action_chooser_can_select_uninstall_without_ansi(self):
        code = ('import sys;sys.path.insert(0,' + repr(str(ROOT / 'setup')) + ');'
                'from steam_link_setup_ui import Presenter;'
                'p=Presenter(lang="en",input_enabled=True,width=32);'
                'print("SELECTED="+str(p.choose_action(["check","uninstall","exit"])))')
        child = subprocess.run([sys.executable, '-I', '-B', '-c', code], input=b'2\n', stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
        self.assertEqual(child.returncode, 0, child.stderr)
        self.assertIn(b'SELECTED=uninstall', child.stdout)
        self.assertNotIn(b'\x1b', child.stdout)
        # Windows text stdout uses CRLF; reject cursor-control CR only.
        self.assertNotIn(b'\r', child.stdout.replace(b'\r\n', b'\n'))


if __name__ == '__main__':
    unittest.main()
