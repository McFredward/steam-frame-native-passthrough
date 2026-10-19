#!/usr/bin/env python3
"""Actual installed shader/cold-reader boundary; private inputs are never bundled."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--libraries', type=Path, required=True)
    parser.add_argument('--coverage', type=Path, required=True)
    parser.add_argument('--retired-atlas', type=Path, required=True)
    parser.add_argument('--historical-count3', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root=args.source_root.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    fixture=Path(__file__).with_name('artifact_tests.cpp')
    inputs=[args.coverage.resolve(),args.retired_atlas.resolve(),args.historical_count3.resolve()]
    sources=['layer/stream_bridge_process.cpp','layer/stream_bridge_process.h','layer/stream_bridge_artifacts.h',
             'tests/stream-bridge-process/artifact_tests.cpp','tests/stream-bridge-process/lifecycle_tests.cpp']
    result={'scope':'Actual production readCoverageFragmentCold with installed coverage and ignored obsolete atlas files; no renderer reader stub or native Steam/Frame execution.',
            'hardware':False,'sources':{name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in sources},
            'inputs':{str(name):hashlib.sha256(name.read_bytes()).hexdigest() for name in inputs},'checks':{}}
    def run(name,command,negative=False):
        completed=subprocess.run(list(map(str,command)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=120)
        (out/(name+'.log')).write_text(completed.stdout)
        if negative:
            if completed.returncode!=1 or 'actual installed count1 coverage accepted by production cold loader' not in completed.stdout:
                raise RuntimeError(name+' missed intended causal assertion')
        elif completed.returncode:
            raise RuntimeError(name+' failed; see '+str(out/(name+'.log')))
        return {'exit':completed.returncode,'expected_negative':negative}
    try:
        with tempfile.TemporaryDirectory(prefix='sfnp-artifact-reader-') as directory:
            work=Path(directory);(work/'layer').mkdir()
            production=(root/'layer/stream_bridge_process.cpp').read_text()
            native=[root/('receiver/'+name+'.cpp') for name in ('stream_bridge_native_decode','stream_bridge_native_loader','stream_bridge_native_runtime','stream_bridge_native_images','stream_bridge_local')]
            libraries=[args.libraries/('libsfnp_stream_bridge'+name+'.a') for name in ('_control','')]
            def compile(name,source,sanitize=False):
                binary=work/name;flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitize else []
                command=['g++','-std=c++17','-Wall','-Wextra','-Wpedantic','-Werror','-pthread',*flags,
                         '-I'+str(root),'-I'+str(root/'layer'),'-DSFNP_PROCESS_SOURCE="'+str(source)+'"',fixture,
                         *native,'-Wl,--start-group',*libraries,'-Wl,--end-group','-ldl','-o',binary]
                run(name+'-build',command);return binary
            result['checks']['normal']=run('normal',[compile('normal',root/'layer/stream_bridge_process.cpp'),*inputs])
            result['checks']['sanitized']=run('sanitized',[compile('sanitized',root/'layer/stream_bridge_process.cpp',True),*inputs])
            anchor='fragment("coverage.spv",stream_bridge_artifacts::CoverageBytes,stream_bridge_artifacts::CoverageSha256,output)'
            if production.count(anchor)!=1:raise RuntimeError('actual reader mutation anchor changed')
            mutant=work/'layer/stream_bridge_process.cpp'
            mutant.write_text(production.replace(anchor,'fragment("coverage.spv",7140,"54760d17ec2933ca667923ca713f64d6810914f9b2b53503d9a6ebb68ee2bdc0",output)'))
            (work/'layer/stream_bridge_process.h').write_bytes((root/'layer/stream_bridge_process.h').read_bytes())
            for name in ('receiver','bridge'):(work/name).symlink_to(root/name,target_is_directory=True)
            result['checks']['stale-count3-runtime-pin']=run('stale-count3-runtime-pin',[compile('stale-count3-runtime-pin',mutant),*inputs],negative=True)
            result['result']='PASS'
    except BaseException as error:
        result.update(result='FAIL',error=str(error));raise
    finally:
        (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
        cleanup={'processes_reaped':True,'temporary_tree_removed':True}
        retention=Path('/home/claw/gloomhaven_vr/scripts')
        if retention.is_dir():
            sys.path.insert(0,str(retention))
            from town_run_retention import cleanup_completed_run
            cleanup['completion_api']=cleanup_completed_run('stream-bridge-artifact-reader',0 if result.get('result')=='PASS' else 1,out,root,process_reaped=True)
        (out/'cleanup.json').write_text(json.dumps(cleanup,indent=2,default=str)+'\n')
    print(json.dumps(result['checks']))


if __name__=='__main__':main()
