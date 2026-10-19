"""Exact, private before/after authority for one automated setup operation.

This is separate from an installation's uninstall receipt: rolling back a repair
must restore the state before that repair, not uninstall an older installation.
Callers provide a fixed path allowlist and their own exact file/link operations.
Settings restore only the keys the operation owns, retaining unrelated changes.
No process/service actions or automatic retries occur in this module.
"""
from __future__ import annotations

import base64
import os
from pathlib import Path
import re
import secrets
import stat

SCHEMA='sfnp-setup-checkpoint-v1'
LIMIT=64*1024*1024


class Checkpoint:
    def __init__(self,engine,path,allowed,current,replace):
        self.e=engine;self.path=Path(path);self.allowed=allowed
        self.current=current;self.replace=replace

    def _sync_directory(self):
        if os.name!='posix':return  # put() already flushes the Windows file before atomic publication.
        descriptor=os.open(self.path.parent,os.O_RDONLY|os.O_DIRECTORY)
        try:os.fsync(descriptor)
        finally:os.close(descriptor)

    def value(self):
        if not self.path.exists():return None
        e=self.e;value=e.parse(e.read(self.path,LIMIT,private=True))
        e.require(type(value)is dict and set(value) in (
                  {'schema','action','operations','settings','registered'},
                  {'schema','action','operations','settings','registered','continuation'}) and
                  value['schema']==SCHEMA and value['action'] in ('install','update','repair','uninstall') and
                  type(value['operations'])is list and len(value['operations'])<=128 and
                  type(value['settings'])is list and len(value['settings'])<=4 and
                  (value['registered'] is None or type(value['registered'])is bool),'Unknown setup recovery checkpoint preserved.')
        if 'continuation' in value:
            transfer=value['continuation']
            e.require(type(transfer)is dict and set(transfer) in (
                      {'target','id','phase','previous'},{'target','id','phase','previous','retained'}) and
                      all(type(transfer[k])is str and re.fullmatch('[0-9a-f]{64}',transfer[k]) for k in ('target','previous')) and
                      type(transfer['id'])is str and re.fullmatch('[0-9a-f]{32}',transfer['id']) and
                      transfer['phase'] in ('ready','active'),'Unknown setup continuation authority preserved.')
        seen=set()
        for op in value['operations']:
            e.require(type(op)is dict and set(op)=={'path','before','after'} and type(op['path'])is str and
                      op['path'] not in seen and self.allowed(Path(op['path'])) and
                      type(op['after'])is list and 1<=len(op['after'])<=3,'Unknown setup recovery file scope preserved.')
            seen.add(op['path'])
            for item in [op['before'],*op['after']]:
                e.require(item is None or type(item)is dict and set(item) in ({'kind','bytes','mode'},{'kind','target'}),
                          'Unknown setup recovery content preserved.')
                if item is None:continue
                if item['kind']=='file':
                    e.require(set(item)=={'kind','bytes','mode'} and type(item['mode'])is int and 0<=item['mode']<=0o777 and
                              len(base64.b64decode(item['bytes'],validate=True))<=e.MAX_FILE,'Invalid setup recovery file.')
                else:e.require(item['kind']=='link' and type(item['target'])is str and len(item['target'])<=2048,
                               'Invalid setup recovery link.')
        seen_settings=set()
        for item in value['settings']:
            e.require(type(item)is dict and set(item)=={'path','section','key','before','after','file_present','section_present','mode'} and
                      type(item['path'])is str and self.allowed(Path(item['path'])) and
                      (item['section'],item['key']) in (
                          ('camera','forceSceneAlphaBlending'),
                          ('driver_steam_link_native_passthrough','enableStreamBridge'),
                          ('driver_steam_link_native_passthrough','controlListenAddress'),
                          ('driver_steam_link_native_passthrough','controlPort')) and
                      type(item['file_present'])is bool and type(item['section_present'])is bool and
                      type(item['mode'])is int and 0<=item['mode']<=0o777 and
                      type(item['after'])is list and 1<=len(item['after'])<=3,
                      'Unknown setup recovery settings scope preserved.')
            identity=(item['path'],item['section'],item['key'])
            e.require(identity not in seen_settings,'Duplicate setup recovery setting preserved.')
            seen_settings.add(identity)
            for entry in [item['before'],*item['after']]:
                e.require(type(entry)is dict and set(entry)=={'present','value'} and type(entry['present'])is bool,
                          'Invalid setup recovery setting.')
        self._retained(value)
        return value

    def _retained(self,value):
        e=self.e;retained=value.get('continuation',{}).get('retained',{'files':[],'settings':[]})
        e.require(type(retained)is dict and set(retained)=={'files','settings'} and
                  type(retained['files'])is list and len(retained['files'])<=128 and
                  type(retained['settings'])is list and len(retained['settings'])<=4,
                  'Unknown retained setup scope preserved.')
        operations={item['path']:item for item in value['operations']}
        preferences={(item['path'],item['section'],item['key']):item for item in value['settings']}
        files={};settings={}
        for item in retained['files']:
            e.require(type(item)is dict and set(item)=={'path','value'} and type(item['path'])is str and
                      item['path'] in operations and item['path'] not in files,
                      'Unknown retained setup file preserved.')
            old=operations[item['path']]
            e.require(item['value'] in [old['before'],*old['after']], 'Changed retained setup file preserved.')
            files[item['path']]=item['value']
        for item in retained['settings']:
            e.require(type(item)is dict and set(item)=={'path','section','key','value'} and
                      all(type(item[key])is str for key in ('path','section','key')),
                      'Unknown retained setup preference preserved.')
            key=(item['path'],item['section'],item['key']);old=preferences.get(key)
            e.require(old is not None and key not in settings and any(e.encoded(item['value'])==e.encoded(entry)
                      for entry in [old['before'],*old['after']]), 'Changed retained setup preference preserved.')
            settings[key]=item['value']
        return files,settings

    def _verify_prepared(self,value):
        files,settings=self._retained(value)
        for item in value['operations']:
            self.e.require(self.current(Path(item['path']))==files.get(item['path'],item['before']),
                           'Original setup files changed before recovery could finish.')
        for item in value['settings']:
            path=Path(item['path']);current=self.e.parse(self.e.read(path,2*1024*1024)) if path.exists() else {}
            section=current.get(item['section'],{});key=(item['path'],item['section'],item['key'])
            self.e.require(type(section)is dict and self.e.encoded({'present':item['key'] in section,'value':section.get(item['key'])})==
                           self.e.encoded(settings.get(key,item['before'])),
                           'Original setup preferences changed before recovery could finish.')

    def retain_current(self,target):
        """Transfer a pending Uninstall without reactivating any predecessor."""
        value=self.value()
        if value is None:return self.reconcile(target)
        files=[{'path':item['path'],'value':self.current(Path(item['path']))} for item in value['operations']]
        settings=[]
        for item in value['settings']:
            path=Path(item['path']);data=self.e.parse(self.e.read(path,2*1024*1024)) if path.exists() else {}
            section=data.get(item['section'],{})
            self.e.require(type(section)is dict,'Changed retained camera preference preserved.')
            settings.append({**{key:item[key] for key in ('path','section','key')},
                             'value':{'present':item['key'] in section,'value':section.get(item['key'])}})
        return self.reconcile(target,retained_files=files,retained_settings=settings)

    def reconcile(self,target,*,retained_files=(),retained_settings=()):
        """Keep original Undo authority while handing restored files to Apply.

        This is not completed recovery: no runtime or picture is claimed. A
        replacement checkpoint must retain these original predecessors before
        any new package writes. Retrying the same ready handoff is idempotent.
        """
        e=self.e;e.require(type(target)is str and re.fullmatch('[0-9a-f]{64}',target),
                          'A checked package is required for setup continuation.')
        old=self.value()
        if old is None:
            self.begin('repair',[],[])
            old=self.value()
        retained={'files':list(retained_files),'settings':list(retained_settings)}
        marker={'target':target,'id':secrets.token_hex(16),'phase':'ready','previous':'0'*64}
        if retained['files'] or retained['settings']:marker['retained']=retained
        self._verify_prepared({**old,'continuation':marker})
        prior=old.get('continuation')
        if prior and prior['phase']=='ready' and prior['target']==target and prior.get('retained',{'files':[],'settings':[]})==retained:
            return prior['id']
        raw=e.read(self.path,LIMIT,private=True)
        e.require(e.parse(raw)==old,'The setup checkpoint changed before continuation could be recorded.')
        updated={**old,'continuation':{**marker,'previous':e.sha(raw)}}
        e.put(self.path,e.encoded(updated),raw)
        self._sync_directory()
        self.value()
        return updated['continuation']['id']

    def begin(self,action,operations,settings,registered=None,*,target=None):
        e=self.e;old=self.value()
        value={'schema':SCHEMA,'action':action,'operations':operations,'settings':settings,'registered':registered}
        previous=None
        if old is not None:
            handoff=old.get('continuation')
            e.require(handoff and handoff['phase']=='ready' and target==handoff['target'],
                      'An unfinished setup recovery must be completed before another installation.')
            self._verify_prepared(old)
            retained_files,retained_settings=self._retained(old)
            previous=e.read(self.path,LIMIT,private=True)
            e.require(e.parse(previous)==old,'The setup checkpoint changed before continuation could start.')
            by_path={item['path']:item for item in operations}
            e.require(len(by_path)==len(operations),'Duplicate continuation file scope refused.')
            for item in old['operations']:
                replacement=by_path.get(item['path'])
                if replacement is not None:
                    observed=retained_files.get(item['path'],item['before'])
                    e.require(replacement['before']==observed,'Changed continuation predecessor preserved.')
                    after=list(replacement['after'])
                    if observed!=item['before'] and observed not in after:after.append(observed)
                    by_path[item['path']]={**replacement,'before':item['before'],'after':after}
                else:
                    by_path[item['path']]={**item,'after':[retained_files.get(item['path'],item['before'])]}
            by_key={(item['path'],item['section'],item['key']):item for item in settings}
            e.require(len(by_key)==len(settings),'Duplicate continuation setting refused.')
            for item in old['settings']:
                key=(item['path'],item['section'],item['key']);replacement=by_key.get(key)
                if replacement is not None:
                    observed=retained_settings.get(key,item['before'])
                    e.require(e.encoded(replacement['before'])==e.encoded(observed), 'Changed continuation preference preserved.')
                    after=list(replacement['after'])
                    if e.encoded(observed)!=e.encoded(item['before']) and all(e.encoded(observed)!=e.encoded(entry) for entry in after):after.append(observed)
                    # The initial file/section ownership survives the handoff.
                    by_key[key]={**replacement,'before':item['before'],'after':after,**{k:item[k] for k in ('file_present','section_present','mode')}}
                else:by_key[key]={**item,'after':[retained_settings.get(key,item['before'])]}
            if old['registered'] is not None:
                e.require(registered is None or registered==old['registered'] or action=='uninstall' and 'retained' in handoff,
                          'Changed continuation registration preserved.')
                value['registered']=old['registered']
            value.update(operations=list(by_path.values()),settings=list(by_key.values()),
                         continuation={k:v for k,v in {**handoff,'phase':'active','previous':e.sha(previous)}.items() if k!='retained'})
        raw=e.encoded(value);e.require(len(raw)<=LIMIT,'Setup recovery checkpoint exceeds its bound.')
        e.put(self.path,raw,previous)
        self._sync_directory()
        try:self.value()
        except BaseException:
            if previous is None:e.remove_owned(self.path,raw)
            else:e.put(self.path,previous,raw)
            self._sync_directory()
            raise
        return value

    def operation(self,path,after):
        self.e.require(self.allowed(Path(path)),'Setup recovery path is outside its fixed scope.')
        return {'path':str(path),'before':self.current(Path(path)),'after':after}

    def setting(self,path,section,key,after):
        e=self.e;path=Path(path);present=path.exists()
        e.require(self.allowed(path),'Setup settings recovery path is outside its fixed scope.')
        value=e.parse(e.read(path,2*1024*1024)) if present else {}
        e.require(type(value)is dict and (section not in value or type(value[section])is dict),'Invalid setup settings preserved.')
        keys=value.get(section,{})
        return {'path':str(path),'section':section,'key':key,'file_present':present,'section_present':section in value,
                'mode':stat.S_IMODE(path.stat().st_mode) if present else 0o600,
                'before':{'present':key in keys,'value':keys.get(key)},'after':after}

    def restore(self):
        e=self.e;value=self.value()
        if value is None:return {'restored':True,'registered':None}
        # Validate all file/settings ownership before the first reversal.
        for op in value['operations']:
            observed=self.current(Path(op['path']))
            e.require(observed==op['before'] or observed in op['after'],
                      'A changed file was preserved; setup recovery is not complete.')
        values={}
        for item in value['settings']:
            path=Path(item['path'])
            if path not in values:
                raw=e.read(path,2*1024*1024) if path.exists() else None
                values[path]=[raw,e.parse(raw) if raw is not None else {}]
            current=values[path][1];section=current.get(item['section'],{})
            e.require(type(section)is dict,'Changed settings were preserved; recovery is not complete.')
            observed={'present':item['key'] in section,'value':section.get(item['key'])}
            e.require(any(e.encoded(observed)==e.encoded(v) for v in [item['before'],*item['after']]),
                      'Changed settings were preserved; recovery is not complete.')
            if item['before']['present']:current.setdefault(item['section'],{})[item['key']]=item['before']['value']
            elif item['section'] in current:
                current[item['section']].pop(item['key'],None)
                if not current[item['section']] and not item['section_present']:current.pop(item['section'])
        for op in reversed(value['operations']):
            observed=self.current(Path(op['path']))
            if observed!=op['before']:self.replace(Path(op['path']),observed,op['before'])
        for path,(raw,current) in values.items():
            updated=e.encoded(current)
            specification=next(item for item in value['settings'] if item['path']==str(path))
            if not current and not specification['file_present']:
                if raw is not None:e.remove_owned(path,raw)
            else:
                if raw!=updated:e.put(path,updated,raw)
                import os
                if os.name!='nt':os.chmod(path,specification['mode'])
        # Keep the checkpoint until the original files AND running Frame UI
        # have passed the separate lifecycle/native-health checks.
        return {'restored':True,'registered':value['registered']}

    def verify_restored(self):
        value=self.value()
        if value is None:return
        for op in value['operations']:
            self.e.require(self.current(Path(op['path']))==op['before'],
                           'Original setup files changed before recovery could finish.')
        for item in value['settings']:
            path=Path(item['path']);current=self.e.parse(self.e.read(path,2*1024*1024)) if path.exists() else {}
            section=current.get(item['section'],{})
            self.e.require(type(section)is dict and self.e.encoded({'present':item['key'] in section,'value':section.get(item['key'])})==self.e.encoded(item['before']),
                           'Original setup preferences changed before recovery could finish.')

    def finish(self,*,restored=False):
        if restored:self.verify_restored()
        value=self.value()
        if value is not None:self.e.remove_owned(self.path,self.e.read(self.path,LIMIT,private=True))


def file_value(raw,mode=0o600):
    # Windows stat synthesizes 0666 for ordinary files even when our protected
    # DACL is private. POSIX permission bits are not Windows file identity.
    if os.name=='nt':mode=0o600
    return {'kind':'file','bytes':base64.b64encode(raw).decode(),'mode':mode}


def read_value(engine,path):
    import os
    path=Path(path)
    if not os.path.lexists(path):return None
    info=path.lstat()
    if stat.S_ISLNK(info.st_mode):return {'kind':'link','target':os.readlink(path)}
    return file_value(engine.read(path),stat.S_IMODE(info.st_mode))


def replace_value(engine,path,observed,value):
    import os
    path=Path(path)
    engine.require(read_value(engine,path)==observed,'A recovery file changed during restoration.')
    if observed and observed['kind']=='link':path.unlink();observed=None
    if value is None:
        if observed:engine.remove_owned(path,base64.b64decode(observed['bytes']))
    elif value['kind']=='file':
        engine.put(path,base64.b64decode(value['bytes']),base64.b64decode(observed['bytes']) if observed else None)
        if os.name!='nt':os.chmod(path,value['mode'])
    else:
        if observed:engine.remove_owned(path,base64.b64decode(observed['bytes']))
        path.parent.mkdir(parents=True,exist_ok=True,mode=0o700);os.symlink(value['target'],path)
