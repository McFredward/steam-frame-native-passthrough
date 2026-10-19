#!/usr/bin/env python3
"""PC lifecycle with real isolated children; native Windows ports are separate."""
from __future__ import annotations
import argparse
import copy
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import types
import unittest

ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);sys.modules[name]=value;spec.loader.exec_module(value);return value
pc=module('sfnp_pc_lifecycle_under_test',ROOT/'setup/steam_link_setup_pc.py')
e=module('sfnp_pc_engine_under_test',ROOT/'setup/steam_link_setup.py')
OUT=None


class ChildAdapter:
    """Explicit synthetic authority over only Popen objects created by a test.

    Canonical-image/SID admission here is simulated; the Windows-only test below
    exercises those native public APIs against copied, isolated cmd fixtures.
    """
    def __init__(self,root):
        self.root=root;self.children=[];self.counter=100;self.graceful_calls=[];self.terminate_calls=[];self.launches=0;self.incomplete=False;self.before_kill=None
        for name in pc.NAMES:
            path=root/'bin/win64'/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(('NONEXECUTABLE IMAGE FIXTURE '+name).encode())
    def images(self):
        return {name:{'bytes':len((self.root/'bin/win64'/name).read_bytes()),'sha256':hashlib.sha256((self.root/'bin/win64'/name).read_bytes()).hexdigest()} for name in pc.NAMES}
    def spawn(self,name='vrserver.exe',*,graceful=True,foreign=False):
        code='import sys;sys.stdin.buffer.readline()' if graceful else 'import time;time.sleep(60)'
        process=subprocess.Popen([sys.executable,'-I','-B','-c',code],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        self.counter+=1
        path=(self.root/'another-install' if foreign else self.root)/'bin/win64'/name
        value={'pid':process.pid,'created':self.counter,'path':str(path)}
        self.children.append((process,value,graceful));return value
    def inventory(self):
        paths={str(self.root/'bin/win64'/name) for name in pc.NAMES}
        return [copy.deepcopy(value) for process,value,_ in self.children if process.poll() is None and value['path'] in paths]
    def _owned(self,value):
        return next((process for process,current,_ in self.children if current==value and process.poll() is None),None)
    def graceful(self,value):
        self.graceful_calls.append(copy.deepcopy(value));process=self._owned(value)
        if process is None:return False
        permitted=next(graceful for child,_,graceful in self.children if child is process)
        if permitted:
            try:process.stdin.write(b'close\n');process.stdin.flush()
            except OSError:pass
        return True
    def terminate(self,value):
        if self.before_kill:self.before_kill(value)
        self.terminate_calls.append(copy.deepcopy(value));process=self._owned(value)
        if process is None:return False
        process.terminate();return True
    def launch(self):
        self.launches+=1;self.spawn('vrmonitor.exe');self.spawn('vrserver.exe')
        if not self.incomplete:self.spawn('vrcompositor.exe')
    @staticmethod
    def clock():return time.monotonic()
    @staticmethod
    def sleep(value):time.sleep(value)
    def close(self):
        for process,_,_ in self.children:
            if process.poll() is None:process.kill()
            process.wait(timeout=5)
            process.stdin.close()


class Tests(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='sfnp-pc-life-',dir=OUT);self.addCleanup(self.temporary.cleanup)
        self.base=Path(self.temporary.name);self.root=self.base/'SteamVR';self.data=self.base/'private';self.data.mkdir()
        self.local=types.SimpleNamespace(system='Windows',root=self.data)
        self.adapter=ChildAdapter(self.root);self.addCleanup(self.adapter.close)
        self.life=pc.Lifecycle(e,self.local,self.root,adapter=self.adapter)
    def stopped(self):return self.life.stop(grace=.15,timeout=2)

    def test_snapshot_is_private_durable_and_never_rebaselined_after_stop(self):
        self.adapter.spawn();baseline=self.life.snapshot();raw=self.life.path.read_bytes()
        self.assertTrue(baseline['running']);self.stopped()
        again=pc.Lifecycle(e,self.local,self.root,adapter=self.adapter)
        self.assertEqual(again.snapshot(),baseline);self.assertEqual(raw,self.life.path.read_bytes())
        if os.name!='nt':self.assertEqual(self.life.path.stat().st_mode&0o777,0o600)
        else:e.verify_private_acl(self.life.path)

    def test_graceful_stop_uses_exact_current_child_and_preserves_foreign_runtime(self):
        own=self.adapter.spawn();foreign=self.adapter.spawn(foreign=True)
        self.life.snapshot();result=self.stopped()
        self.assertFalse(result['running']);self.assertTrue(result['stopped'])
        self.assertIsNone(self.adapter._owned(own));self.assertIsNotNone(self.adapter._owned(foreign))
        self.assertEqual(self.adapter.terminate_calls,[])
        self.assertEqual(self.adapter.graceful_calls,[own])

    def test_bounded_forced_stop_only_after_grace_and_original_receipt_exists(self):
        own=self.adapter.spawn(graceful=False);self.life.snapshot()
        def inspect(value):
            self.assertEqual(value,own);self.assertTrue(e.parse(e.read(self.life.path,65536,private=True))['running'])
        self.adapter.before_kill=inspect
        self.assertTrue(self.stopped()['stopped']);self.assertEqual(self.adapter.terminate_calls,[own])

    def test_start_requires_stable_server_and_compositor_and_never_claims_picture(self):
        self.life.snapshot();result=self.life.start(timeout=4)
        self.assertEqual(self.adapter.launches,1);self.assertTrue(result['running'])
        self.assertFalse(result['headset_picture_verified']);self.assertEqual(result['health_scope'],'owned_process_presence')
        self.life.start(timeout=4);self.assertEqual(self.adapter.launches,1)

    def test_partial_start_times_out_and_retains_original_off_baseline(self):
        self.life.snapshot();self.adapter.incomplete=True
        with self.assertRaises(e.SetupError):self.life.start(timeout=.3)
        self.assertTrue(self.life.path.exists());self.assertFalse(self.life.snapshot()['running'])
        result=self.life.restore(grace=.1,timeout=2)
        self.assertTrue(result['baseline_restored']);self.assertFalse(result['running']);self.assertEqual(self.adapter.inventory(),[])

    def test_restore_original_running_starts_after_file_rollback_and_preserves_foreign(self):
        self.adapter.spawn();foreign=self.adapter.spawn(foreign=True);original=self.life.snapshot();self.stopped()
        result=self.life.restore(grace=.1,timeout=4)
        self.assertTrue(result['running']);self.assertTrue(result['baseline_restored'])
        self.assertEqual(self.adapter.launches,1);self.assertIsNotNone(self.adapter._owned(foreign))
        self.assertEqual(self.life.snapshot(),original)
        self.life.finish();self.assertFalse(self.life.path.exists())

    def test_finish_reconciles_already_removed_receipt_without_process_actions(self):
        self.life.snapshot();self.life.finish()
        self.assertFalse(self.life.path.exists())
        self.life.finish()
        self.assertEqual(self.adapter.launches,0)
        self.assertEqual(self.adapter.graceful_calls,[])
        self.assertEqual(self.adapter.terminate_calls,[])

    def test_finish_does_not_treat_dangling_link_as_an_already_removed_receipt(self):
        target=self.data/'missing-own-target'
        try:self.life.path.symlink_to(target)
        except (OSError,NotImplementedError):self.skipTest('symbolic link creation unavailable')
        with self.assertRaises(e.SetupError):self.life.finish()
        self.assertTrue(self.life.path.is_symlink())
        self.assertFalse(target.exists())

    def test_finish_removes_only_verified_unchanged_baseline(self):
        self.life.snapshot();value=e.parse(self.life.path.read_bytes());value['running']=True
        self.life.path.write_bytes(e.encoded(value))
        with self.assertRaises(e.SetupError):self.life.finish()
        self.assertEqual(e.parse(self.life.path.read_bytes()),value)

    def test_changed_or_corrupt_baseline_refuses_before_any_stop(self):
        for field,value in [('steamvr','elsewhere'),('schema','unknown'),('running',False)]:
            with self.subTest(field=field):
                self.adapter.close();self.adapter.children=[];self.life.path.unlink(missing_ok=True)
                own=self.adapter.spawn();self.life.snapshot();receipt=e.parse(self.life.path.read_bytes());receipt[field]=value;self.life.path.write_bytes(e.encoded(receipt))
                self.adapter.graceful_calls.clear()
                with self.assertRaises(e.SetupError):self.stopped()
                self.assertEqual(self.adapter.graceful_calls,[]);self.assertIsNotNone(self.adapter._owned(own))

    def test_changed_executable_refuses_before_any_stop(self):
        own=self.adapter.spawn();self.life.snapshot();path=self.root/'bin/win64/vrmonitor.exe';path.write_bytes(b'CHANGED FIXTURE')
        with self.assertRaises(e.SetupError):self.stopped()
        self.assertIsNotNone(self.adapter._owned(own));self.assertEqual(self.adapter.graceful_calls,[])

    def test_pid_reuse_creation_mismatch_is_not_authority(self):
        own=self.adapter.spawn(graceful=False);stale={**own,'created':own['created']-1}
        self.assertFalse(self.adapter.terminate(stale));self.assertIsNotNone(self.adapter._owned(own))
        self.life.snapshot();self.assertTrue(self.stopped()['stopped'])

    def test_process_inventory_bounds_and_unknown_path_refused(self):
        self.life.snapshot();normal=self.adapter.inventory
        try:
            for bad in ([{'pid':1,'created':1,'path':'foreign'}], [dict(pid=1,created=1,path=str(self.root/'bin/win64/vrserver.exe'))]*65,
                        [{'pid':True,'created':1,'path':str(self.root/'bin/win64/vrserver.exe')}],
                        [{'pid':1,'created':1,'path':str(self.root/'bin/win64/vrserver.exe')}]*2):
                self.adapter.inventory=lambda:bad
                with self.subTest(length=len(bad)),self.assertRaises(e.SetupError):self.stopped()
                self.assertEqual(self.adapter.graceful_calls,[])
        finally:self.adapter.inventory=normal

    def test_no_stop_or_launch_without_durable_baseline(self):
        own=self.adapter.spawn()
        with self.assertRaises(OSError):self.stopped()
        with self.assertRaises(OSError):self.life.start(timeout=1)
        self.assertIsNotNone(self.adapter._owned(own));self.assertEqual(self.adapter.launches,0)

    def test_linux_pc_rejected_and_timeout_values_strict(self):
        with self.assertRaises(e.SetupError):pc.Lifecycle(e,types.SimpleNamespace(system='Linux',root=self.data),self.root,adapter=self.adapter)
        self.life.snapshot()
        for value in (float('nan'),0,True,301):
            with self.subTest(value=str(value)),self.assertRaises(e.SetupError):self.life.start(timeout=value)

    def path_adapter(self, expand):
        # Only the public GetLongPathNameW output port is synthetic. Path-link
        # rejection and every normalization decision are actual production code.
        adapter=object.__new__(pc.WindowsAdapter);adapter.e=e
        def public_long(source,destination,bound):
            value=expand(source)
            if type(value) is int:return value
            destination.value=value;return len(value)
        adapter.longpath=public_long
        return adapter

    def test_short_and_long_path_forms_use_the_same_public_canonical_name(self):
        short=self.base/'RUNNER~1'/'SteamVR'/'vrmonitor.exe'
        long=self.base/'runner-user'/'SteamVR'/'vrmonitor.exe'
        called=[]
        def expand(value):
            called.append(value);return str(long) if value==str(short) else value
        adapter=self.path_adapter(expand)
        self.assertEqual(adapter._normalize(short),adapter._normalize(long))
        self.assertEqual(called,[str(short),str(long)])
        foreign=self.base/'another-install'/'vrmonitor.exe'
        self.assertNotEqual(adapter._normalize(short),adapter._normalize(foreign))

    def test_long_path_failure_or_overflow_never_falls_back_to_alias_comparison(self):
        for result in (0,32768,40000):
            adapter=self.path_adapter(lambda value:result)
            with self.subTest(result=result),self.assertRaises(e.SetupError):adapter._normalize(self.root/'bin/win64/vrmonitor.exe')

    @unittest.skipUnless(os.name=='posix','Portable symlink refusal without Windows symlink privileges')
    def test_path_alias_expansion_does_not_authorize_a_redirected_path(self):
        real=self.base/'real';real.mkdir();linked=self.base/'redirected';linked.symlink_to(real,target_is_directory=True)
        called=[]
        adapter=self.path_adapter(lambda value:called.append(value) or value)
        with self.assertRaises(e.SetupError):adapter._normalize(linked/'vrmonitor.exe')
        self.assertEqual(called,[])
        adapter=self.path_adapter(lambda value:str(linked/'vrmonitor.exe'))
        with self.assertRaises(e.SetupError):adapter._normalize(real/'vrmonitor.exe')

    @unittest.skipUnless(os.name=='nt','Windows ctypes process and Authenticode APIs only')
    def test_native_windows_owned_handles_sid_creation_time_and_foreign_pid_refusal(self):
        # The default production trust gate MUST refuse a Microsoft-publisher
        # fixture. Only the explicit subclass below authorizes our copied cmd
        # images to test real native process operations, never installed SteamVR.
        source=Path(os.environ['SystemRoot'])/'System32/cmd.exe'
        for name in pc.NAMES:shutil.copyfile(source,self.root/'bin/win64'/name)
        actual=pc.WindowsAdapter(e,self.root)
        with self.assertRaises(e.SetupError):actual.images()
        class FixtureOnly(pc.WindowsAdapter):
            def _publisher(inner,path,handle):
                self.assertEqual(Path(path).parent,self.root/'bin/win64')
        native=FixtureOnly(e,self.root);native.images()
        target=self.root/'bin/win64/vrmonitor.exe'
        # Reproduce the hosted runner's real short-temp-root/long-handle-path
        # difference. Filesystems without 8.3 aliases may return the same name.
        short_path=native.k.GetShortPathNameW
        short_path.argtypes=[native.w.LPCWSTR,native.w.LPWSTR,native.w.DWORD];short_path.restype=native.w.DWORD
        short=ctypes.create_unicode_buffer(32768);length=short_path(str(target),short,len(short))
        self.assertTrue(0<length<len(short))
        self.assertEqual(native._normalize(short.value),native._normalize(target))
        # Prove the signature/file-verification paths released their deny-delete
        # handles before starting any process. No retry or ignored error here.
        probe=target.with_suffix('.unlocked')
        target.rename(probe);probe.rename(target)
        child=subprocess.Popen([str(target),'/q','/k'],stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                               creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            observed=next(item for item in native.inventory() if item['pid']==child.pid)
            self.assertEqual(observed['path'],str(target));self.assertGreater(observed['created'],0)
            self.assertFalse(native.terminate({**observed,'created':observed['created']-1}));self.assertIsNone(child.poll())
            with self.assertRaises(e.SetupError):native.terminate({**observed,'path':str(source)})
            self.assertIsNone(child.poll());self.assertTrue(native.terminate(observed));child.wait(timeout=5)
        finally:
            if child.poll() is None:child.kill();child.wait(timeout=5)
            try:child.stdin.close()
            finally:
                # wait() observes termination but Popen retains its Windows
                # process HANDLE. Release it while this fixture still owns the
                # Popen object, before asking Windows to delete its image.
                child._handle.Close()
        # The earlier rename proves our verification handles were released.
        # A terminated executable can remain transiently mapped by Windows or
        # its scanner even after the process object signals. Bound that separate
        # teardown race; a leaked persistent handle must still fail this test.
        deadline=time.monotonic()+3
        while True:
            try:
                target.unlink();break
            except PermissionError as error:
                if error.winerror not in (5,32,33) or time.monotonic()>=deadline:raise
                time.sleep(.025)
        self.assertFalse(target.exists(),'The copied image must be deletable before TemporaryDirectory cleanup')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path)
    args,tests=parser.parse_known_args();OUT=args.output
    if OUT:OUT.mkdir(parents=True,exist_ok=True)
    unittest.main(argv=[__file__,*tests])
