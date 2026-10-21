"""Explicit PC SteamVR lifecycle, restricted to one verified installation/user.

Call snapshot/stop/start only after the user's Apply confirmation. This module
never kills by executable name, touches Steam/the game, edits registration, or
claims a headset picture is correct. Recovery retains the durable original
baseline across interrupted runs, but leaves PC SteamVR closed. Linux PC
installation is not supported; the adapter port exists for portable tests.
"""
from __future__ import annotations

from contextlib import contextmanager
import ctypes
import hashlib
import math
import os
from pathlib import Path
import subprocess
import time

SCHEMA = 'sfnp-pc-lifecycle-v1'
NAMES = ('vrmonitor.exe', 'vrcompositor.exe', 'vrserver.exe')
MAX_PROCESSES = 64


def _duration(engine, value):
    engine.require(type(value) in (int,float) and math.isfinite(value) and 0 < value <= 300,
                   'A finite bounded PC lifecycle timeout is required.')
    return value


class Lifecycle:
    """Own one durable baseline; callers own workflow and file rollback order.

    ``local`` supplies .system and private .root; ``steamvr`` is the already
    reviewed SteamVR root. Native authority always rechecks current process
    path, owner SID and creation time using a live process handle before stop.
    Adapter ports: images(), inventory(), graceful(identity), terminate(identity),
    launch(), clock(), sleep(seconds). Images are trusted file pins; inventory
    records contain exact pid/created/path. An injected adapter is a test port,
    never an alternate manifest-supplied command or executable.
    """
    def __init__(self, engine, local, steamvr, *, adapter=None):
        self.e, self.local = engine, local
        engine.require(local.system == 'Windows', 'PC runtime installation currently requires Windows.')
        self.root = engine.no_links(Path(steamvr).absolute())
        self.path = engine.no_links(local.root / 'lifecycle.json')
        self.adapter = adapter or WindowsAdapter(engine, self.root)
        self._observed_images = None

    def _images(self):
        # Each new setup operation re-verifies the current signed Valve files.
        # A historical receipt need not prevent stop-only recovery after a real
        # SteamVR update, but changes during this operation must still abort.
        images = self.adapter.images()
        self.e.require(self._observed_images is None or images == self._observed_images,
                       'SteamVR executables changed during lifecycle control.')
        self._observed_images = images
        return images

    def _inventory(self):
        result = self.adapter.inventory()
        self.e.require(type(result) is list and len(result) <= MAX_PROCESSES, 'PC runtime process inventory exceeded its bound.')
        paths = {str(self.root / 'bin/win64' / name) for name in NAMES}
        seen = set()
        for item in result:
            self.e.require(type(item) is dict and set(item) == {'pid','created','path'} and
                           type(item['pid']) is int and 0 < item['pid'] <= 0xffffffff and
                           type(item['created']) is int and 0 < item['created'] <= 0xffffffffffffffff and
                           item['path'] in paths and (item['pid'],item['created']) not in seen,
                           'An unverified PC runtime process was refused.')
            seen.add((item['pid'],item['created']))
        return result

    def _baseline(self, *, stopping=False):
        raw = self.e.read(self.path, 65536, private=True)
        value = self.e.parse(raw)
        self.e.require(set(value) == {'schema','steamvr','images','running','processes'} and value['schema'] == SCHEMA and
                       value['steamvr'] == str(self.root) and type(value['running']) is bool and
                       type(value['processes']) is list and len(value['processes']) <= MAX_PROCESSES and
                       bool(value['processes']) == value['running'],
                       'PC lifecycle recovery belongs to a different or changed SteamVR installation; it was preserved.')
        pins=value['images']
        self.e.require(type(pins) is dict and set(pins)==set(NAMES) and all(
            type(pin) is dict and set(pin)=={'bytes','sha256'} and type(pin['bytes']) is int and
            0<pin['bytes']<=256*1024*1024 and type(pin['sha256']) is str and len(pin['sha256'])==64 and
            all(c in '0123456789abcdef' for c in pin['sha256']) for pin in pins.values()),
            'Unknown PC lifecycle recovery data was preserved.')
        images=self._images()
        self.e.require(stopping or pins==images,
                       'PC lifecycle recovery belongs to a different or changed SteamVR installation; it was preserved.')
        paths = {str(self.root / 'bin/win64' / name) for name in NAMES}
        for item in value['processes']:
            self.e.require(type(item) is dict and set(item) == {'pid','created','path'} and
                           type(item['pid']) is int and 0 < item['pid'] <= 0xffffffff and
                           type(item['created']) is int and 0 < item['created'] <= 0xffffffffffffffff and item['path'] in paths,
                           'Unknown PC lifecycle recovery data was preserved.')
        return raw,value

    def snapshot(self):
        """Persist before the first stop. Re-entry never replaces that baseline."""
        if os.path.lexists(self.path):
            return self._baseline(stopping=True)[1]
        images = self._images()
        processes = self._inventory()
        value = {'schema':SCHEMA,'steamvr':str(self.root),'images':images,
                 'running':bool(processes),'processes':processes}
        self.e.put(self.path,self.e.encoded(value))
        return value

    def stop(self, *, grace=5, timeout=8):
        """Graceful close, then terminate only freshly reverified owned handles."""
        _duration(self.e,grace); _duration(self.e,timeout)
        self._baseline(stopping=True)
        start = self.adapter.clock(); forced = start + grace; end = forced + timeout
        notified = set()
        while True:
            processes = self._inventory()
            if not processes:
                return {'stopped':True,'running':False,'health_scope':'owned_process_presence'}
            for item in processes:
                key = (item['pid'],item['created'])
                if key not in notified:
                    self.e.require(len(notified)<MAX_PROCESSES*4,'PC runtime kept respawning; lifecycle control stopped with recovery data retained.')
                    self.adapter.graceful(item); notified.add(key)
                if self.adapter.clock() >= forced:
                    self.adapter.terminate(item)
            self.e.require(self.adapter.clock() < end,
                           'The owned PC SteamVR processes did not stop; recovery data was retained.')
            self.adapter.sleep(min(.1,max(0,end-self.adapter.clock())))

    def start(self, *, timeout=45):
        """Start canonical signed vrmonitor; only process presence is confirmed."""
        _duration(self.e,timeout); self._baseline()
        if not self._inventory():
            self.adapter.launch()
        end = self.adapter.clock() + timeout
        previous = None
        while True:
            processes = self._inventory()
            names = {Path(item['path']).name for item in processes}
            identity = sorted((item['pid'],item['created'],item['path']) for item in processes)
            if {'vrserver.exe','vrcompositor.exe'} <= names and identity == previous:
                return {'running':True,'processes':processes,'health_scope':'owned_process_presence',
                        'headset_picture_verified':False}
            previous = identity
            self.e.require(self.adapter.clock() < end,
                           'PC SteamVR startup could not be confirmed; recovery data was retained.')
            self.adapter.sleep(min(.1,max(0,end-self.adapter.clock())))

    def restore(self, *, grace=5, timeout=45):
        """Reconcile recovery without launching PC SteamVR, even if it ran before.

        Keep the historical receipt unchanged until file recovery is verified.
        The user/game owns the next runtime launch on every setup outcome.
        """
        return {**self.stop(grace=grace,timeout=timeout),'baseline_restored':True}

    def finish(self):
        """Forget our exact receipt; a reconciled finalization may repeat this.

        Absence means an earlier finish already removed it. A dangling link,
        changed receipt or changed runtime still goes through strict validation.
        """
        if not os.path.lexists(self.path):return
        raw,_ = self._baseline(stopping=True)
        self.e.remove_owned(self.path,raw)


class WindowsAdapter:
    """Public Windows API implementation; no shell, taskkill or name-only kill."""
    def __init__(self, engine, steamvr):
        engine.require(os.name == 'nt', 'Native PC runtime control requires Windows.')
        from ctypes import wintypes as w
        self.e,self.root,self.w = engine,Path(steamvr),w
        self.paths = {name:engine.no_links(self.root/'bin/win64'/name) for name in NAMES}
        self._pins = None
        self.k = ctypes.WinDLL('kernel32',use_last_error=True)
        self.a = ctypes.WinDLL('advapi32',use_last_error=True)
        self.u = ctypes.WinDLL('user32',use_last_error=True)
        def bind(dll,name,args,result):
            call=getattr(dll,name);call.argtypes=args;call.restype=result;return call
        H,D,P,B = w.HANDLE,w.DWORD,ctypes.c_void_p,w.BOOL
        self.close = bind(self.k,'CloseHandle',[H],B)
        self.open = bind(self.k,'OpenProcess',[D,B,D],H)
        self.query = bind(self.k,'QueryFullProcessImageNameW',[H,D,w.LPWSTR,ctypes.POINTER(D)],B)
        self.times = bind(self.k,'GetProcessTimes',[H,ctypes.POINTER(w.FILETIME),ctypes.POINTER(w.FILETIME),ctypes.POINTER(w.FILETIME),ctypes.POINTER(w.FILETIME)],B)
        self.wait = bind(self.k,'WaitForSingleObject',[H,D],D)
        self.kill = bind(self.k,'TerminateProcess',[H,w.UINT],B)
        self.current = bind(self.k,'GetCurrentProcess',[],H)
        self.token = bind(self.a,'OpenProcessToken',[H,D,ctypes.POINTER(H)],B)
        self.tokeninfo = bind(self.a,'GetTokenInformation',[H,ctypes.c_int,P,D,ctypes.POINTER(D)],B)
        self.sidlength = bind(self.a,'GetLengthSid',[P],D)
        self.snapshot = bind(self.k,'CreateToolhelp32Snapshot',[D,D],H)
        self.createfile = bind(self.k,'CreateFileW',[w.LPCWSTR,D,D,P,D,D,H],H)
        self.finalpath = bind(self.k,'GetFinalPathNameByHandleW',[H,w.LPWSTR,D,D],D)
        self.longpath = bind(self.k,'GetLongPathNameW',[w.LPCWSTR,w.LPWSTR,D],D)
        self.size = bind(self.k,'GetFileSizeEx',[H,ctypes.POINTER(ctypes.c_longlong)],B)
        self.readfile = bind(self.k,'ReadFile',[H,P,D,ctypes.POINTER(D),P],B)
        self.seek = bind(self.k,'SetFilePointerEx',[H,ctypes.c_longlong,ctypes.POINTER(ctypes.c_longlong),D],B)
        self.post = bind(self.u,'PostMessageW',[w.HWND,w.UINT,w.WPARAM,w.LPARAM],B)
        self.windowpid = bind(self.u,'GetWindowThreadProcessId',[w.HWND,ctypes.POINTER(D)],D)
        self.enumtype = ctypes.WINFUNCTYPE(B,w.HWND,w.LPARAM)
        self.enumwindows = bind(self.u,'EnumWindows',[self.enumtype,w.LPARAM],B)
        class Entry(ctypes.Structure):
            _fields_=[('size',D),('usage',D),('pid',D),('heap',ctypes.c_size_t),('module',D),('threads',D),
                      ('parent',D),('priority',w.LONG),('flags',D),('name',w.WCHAR*260)]
        self.Entry=Entry
        self.first=bind(self.k,'Process32FirstW',[H,ctypes.POINTER(Entry)],B)
        self.next=bind(self.k,'Process32NextW',[H,ctypes.POINTER(Entry)],B)
        self.user = self._sid(self.current())

    @staticmethod
    def clock():return time.monotonic()
    @staticmethod
    def sleep(seconds):time.sleep(seconds)
    def _normalize(self,path):
        value=str(path)
        if value.startswith('\\\\?\\UNC\\'):value='\\\\'+value[8:]
        elif value.startswith('\\\\?\\'):value=value[4:]
        # Handle/process queries expand 8.3 aliases (e.g. RUNNER~1), while
        # registry/temp roots may retain them. Expand BOTH representations via
        # the public OS API; never accept a basename/prefix or failed fallback.
        checked=self.e.no_links(Path(os.path.abspath(value)))
        expanded=ctypes.create_unicode_buffer(32768)
        length=self.longpath(str(checked),expanded,len(expanded))
        self.e.require(0<length<len(expanded),'A canonical SteamVR executable path could not be checked.')
        canonical=self.e.no_links(Path(expanded.value))
        return os.path.normcase(os.path.abspath(canonical))

    def _sid(self, process):
        token=self.w.HANDLE();self.e.require(self.token(process,8,ctypes.byref(token)), 'PC process ownership could not be checked.')
        try:
            size=self.w.DWORD();self.tokeninfo(token,1,None,0,ctypes.byref(size))
            self.e.require(0<size.value<=65536,'PC process ownership exceeded its bound.')
            buf=ctypes.create_string_buffer(size.value)
            self.e.require(self.tokeninfo(token,1,buf,len(buf),ctypes.byref(size)), 'PC process ownership could not be checked.')
            sid=ctypes.cast(buf,ctypes.POINTER(ctypes.c_void_p))[0];length=self.sidlength(sid)
            self.e.require(0<length<=len(buf) and ctypes.addressof(buf)<=sid<=ctypes.addressof(buf)+len(buf)-length,
                           'PC process owner identity was invalid.')
            return ctypes.string_at(sid,length)
        finally:self.close(token)

    @contextmanager
    def _file(self,path):
        path=self.e.no_links(path)
        handle=self.createfile(str(path),0x80000000,1,None,3,0x200000,None)
        self.e.require(handle not in (None,ctypes.c_void_p(-1).value),'SteamVR executable could not be locked for verification.')
        try:
            final=ctypes.create_unicode_buffer(32768);length=self.finalpath(handle,final,len(final),0)
            self.e.require(0<length<len(final) and self._normalize(final.value)==self._normalize(path),
                           'A redirected SteamVR executable was preserved.')
            yield handle
        finally:self.close(handle)

    def _publisher(self,path,handle):
        # Windows SDK WINTRUST_DATA/FILE_INFO and official WTHelper accessors.
        # Query the actual trusted SIGNER's organization, never an arbitrary
        # certificate embedded beside a different publisher's signature.
        w=self.w;wintrust=ctypes.WinDLL('wintrust',use_last_error=True);crypt=ctypes.WinDLL('crypt32',use_last_error=True)
        class Guid(ctypes.Structure):_fields_=[('a',w.DWORD),('b',w.WORD),('c',w.WORD),('d',ctypes.c_ubyte*8)]
        class File(ctypes.Structure):_fields_=[('size',w.DWORD),('path',w.LPCWSTR),('file',w.HANDLE),('subject',ctypes.c_void_p)]
        class Trust(ctypes.Structure):
            _fields_=[('size',w.DWORD),('policy',ctypes.c_void_p),('sip',ctypes.c_void_p),('ui',w.DWORD),
                      ('revoke',w.DWORD),('choice',w.DWORD),('file',ctypes.POINTER(File)),('action',w.DWORD),
                      ('state',w.HANDLE),('url',w.LPWSTR),('flags',w.DWORD),('context',w.DWORD)]
        class CertPrefix(ctypes.Structure):_fields_=[('size',w.DWORD),('cert',ctypes.c_void_p)]
        verify=wintrust.WinVerifyTrust;verify.argtypes=[w.HWND,ctypes.POINTER(Guid),ctypes.POINTER(Trust)];verify.restype=w.LONG
        data=wintrust.WTHelperProvDataFromStateData;data.argtypes=[w.HANDLE];data.restype=ctypes.c_void_p
        signer=wintrust.WTHelperGetProvSignerFromChain;signer.argtypes=[ctypes.c_void_p,w.DWORD,w.BOOL,w.DWORD];signer.restype=ctypes.c_void_p
        cert=wintrust.WTHelperGetProvCertFromChain;cert.argtypes=[ctypes.c_void_p,w.DWORD];cert.restype=ctypes.POINTER(CertPrefix)
        name=crypt.CertGetNameStringW;name.argtypes=[ctypes.c_void_p,w.DWORD,w.DWORD,ctypes.c_void_p,w.LPWSTR,w.DWORD];name.restype=w.DWORD
        guid=Guid(0xaac56b,0xcd44,0x11d0,(ctypes.c_ubyte*8)(0x8c,0xc2,0,0xc0,0x4f,0xc2,0x95,0xee))
        file=File(ctypes.sizeof(File),str(path),handle,None)
        value=Trust();value.size=ctypes.sizeof(Trust);value.ui=2;value.choice=1;value.file=ctypes.pointer(file);value.action=1;value.flags=0x1000
        try:
            self.e.require(verify(None,ctypes.byref(guid),ctypes.byref(value))==0,'SteamVR executable signature could not be verified.')
            provider=data(value.state);self.e.require(provider,'SteamVR executable signer was unavailable.')
            actual=signer(provider,0,False,0);self.e.require(actual,'SteamVR executable signer was unavailable.')
            current=cert(actual,0);self.e.require(current and current.contents.size>=ctypes.sizeof(CertPrefix) and current.contents.cert,
                                                'SteamVR executable signing certificate was unavailable.')
            oid=ctypes.c_char_p(b'2.5.4.10');text=ctypes.create_unicode_buffer(512)
            length=name(current.contents.cert,3,0,ctypes.cast(oid,ctypes.c_void_p),text,len(text))
            self.e.require(1<length<=len(text) and text.value in ('Valve Corp.','Valve Corporation'),
                           'SteamVR executable is not signed by the expected publisher.')
        finally:
            value.action=2;verify(None,ctypes.byref(guid),ctypes.byref(value))

    def _pin_handle(self,path,handle):
        self._publisher(path,handle)
        self.e.require(self.seek(handle,0,None,0),'SteamVR executable could not be read from its beginning.')
        size=ctypes.c_longlong();self.e.require(self.size(handle,ctypes.byref(size)) and 0<size.value<=256*1024*1024,
                                             'SteamVR executable exceeded its verification bound.')
        digest=hashlib.sha256();buf=ctypes.create_string_buffer(65536);count=self.w.DWORD();total=0
        while True:
            self.e.require(self.readfile(handle,buf,len(buf),ctypes.byref(count),None),'SteamVR executable could not be read.')
            if not count.value:break
            total+=count.value;self.e.require(total<=size.value,'SteamVR executable changed while checking.')
            digest.update(buf.raw[:count.value])
        self.e.require(total==size.value,'SteamVR executable changed while checking.')
        return {'bytes':total,'sha256':digest.hexdigest()}

    def _pin(self,path):
        with self._file(path) as handle:return self._pin_handle(path,handle)

    def images(self):
        pins={name:self._pin(path) for name,path in self.paths.items()}
        if self._pins is not None:self.e.require(pins==self._pins,'SteamVR executables changed during lifecycle control.')
        self._pins=pins
        return pins

    def _process(self,pid,rights=0x101000):
        handle=self.open(rights,False,pid)
        if not handle:
            if ctypes.get_last_error() in (87,1168):return None
            raise self.e.SetupError('PC runtime process identity could not be inspected; no process was terminated.')
        try:
            status=self.wait(handle,0)
            self.e.require(status in (0,0x102),'PC process liveness could not be checked.')
            if status==0:return None
            if self._sid(handle)!=self.user:return None
            path=ctypes.create_unicode_buffer(32768);size=self.w.DWORD(len(path))
            self.e.require(self.query(handle,0,path,ctypes.byref(size)),'PC runtime executable path could not be checked.')
            expected=next((value for value in self.paths.values() if self._normalize(path.value)==self._normalize(value)),None)
            if expected is None:return None
            self.e.no_links(expected)
            created,ended,kernel,user=(self.w.FILETIME() for _ in range(4))
            self.e.require(self.times(handle,ctypes.byref(created),ctypes.byref(ended),ctypes.byref(kernel),ctypes.byref(user)),
                           'PC process creation identity could not be checked.')
            stamp=(created.dwHighDateTime<<32)|created.dwLowDateTime
            self.e.require(stamp>0,'PC process creation identity was invalid.')
            result={'pid':pid,'created':stamp,'path':str(expected)}
            owned=handle;handle=None;return owned,result
        except self.e.SetupError:
            # Toolhelp may enumerate a process that exits during any identity
            # query. Only this exact handle signaling termination proves that
            # race; an unreadable live process must still fail without a kill.
            if handle and self.wait(handle,0)==0:return None
            raise
        finally:
            if handle:self.close(handle)

    def inventory(self):
        handle=self.snapshot(2,0)
        self.e.require(handle not in (None,ctypes.c_void_p(-1).value),'PC process inventory was unavailable.')
        result=[]
        try:
            item=self.Entry();item.size=ctypes.sizeof(item);has=self.first(handle,ctypes.byref(item));count=0
            while has:
                count+=1;self.e.require(count<=65536,'PC process inventory exceeded its bound.')
                if item.name.lower() in NAMES:
                    found=self._process(item.pid)
                    if found:
                        owned,value=found;self.close(owned);result.append(value)
                        self.e.require(len(result)<=MAX_PROCESSES,'Owned PC runtime process inventory exceeded its bound.')
                has=self.next(handle,ctypes.byref(item))
            self.e.require(ctypes.get_last_error()==18,'PC process inventory could not be completed.')
            return result
        finally:self.close(handle)

    @contextmanager
    def _owned(self,identity,rights):
        self.e.require(type(identity) is dict and set(identity)=={'pid','created','path'} and
                       type(identity['pid']) is int and 0<identity['pid']<=0xffffffff and
                       type(identity['created']) is int and identity['created']>0 and
                       identity['path'] in {str(p) for p in self.paths.values()},'Unverified process authority was refused.')
        found=self._process(identity['pid'],rights)
        if not found:
            yield None;return
        handle,current=found
        try:yield handle if current==identity else None
        finally:self.close(handle)

    def graceful(self,identity):
        with self._owned(identity,0x101000) as handle:
            if not handle:return False
            @self.enumtype
            def window(hwnd,unused):
                pid=self.w.DWORD();self.windowpid(hwnd,ctypes.byref(pid))
                if pid.value==identity['pid'] and self.wait(handle,0)==0x102:self.post(hwnd,0x10,0,0)
                return True
            self.e.require(self.enumwindows(window,0),'PC runtime windows could not be checked for graceful shutdown.')
            return True

    def terminate(self,identity):
        with self._owned(identity,0x101001) as handle:
            if not handle:return False
            if self.wait(handle,0)==0:return True
            if not self.kill(handle,1):
                self.e.require(self.wait(handle,0)==0,'An owned PC SteamVR process could not be stopped.')
            return True

    def launch(self):
        self.e.require(self._pins is not None,'SteamVR executable preparation is required before launch.')
        path=self.paths['vrmonitor.exe']
        # Deny write/delete while checking the exact same executable and while
        # CreateProcess opens it. WinVerifyTrust receives this open file handle.
        with self._file(path) as handle:
            self.e.require(self._pin_handle(path,handle)==self._pins['vrmonitor.exe'],'SteamVR launcher changed before startup.')
            subprocess.Popen([str(path)],cwd=str(path.parent),stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,close_fds=True)
