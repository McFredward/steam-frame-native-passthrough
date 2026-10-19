#!/usr/bin/env python3
"""Actual independent subprocess/IPC client tests; never initializes OpenVR."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--sanitize',action='store_true')
    parser.add_argument('--mutants',action='store_true')
    args=parser.parse_args();root=Path(__file__).resolve().parents[2];out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source=root/'receiver/stream_bridge_camera_client.cpp';original=source.read_text();results={}
    cases={'normal':original}
    if args.sanitize:cases['sanitize']=original
    if args.mutants:
        cases['early-api-admission']=original.replace('void Client::request(bool enabled) noexcept{impl_->set(enabled);}', 'void Client::request(bool enabled) noexcept{impl_->set(enabled);if(enabled)impl_->ready.store(true);}')
        cases['stale-ack-admitted']=original.replace('if(value!=request||value!=sent)return true;', 'if(false)return true;').replace("if(bytes[0]=='A'&&target())","if(bytes[0]=='A')")
        cases['resume-never-restores']=original.replace('offset-sleepOffset>5000000ULL','offset-sleepOffset>~std::uint64_t(0)')
        cases['stall-is-suspend']=original.replace('sample.bootNanoseconds>sample.monotonicNanoseconds?sample.bootNanoseconds-sample.monotonicNanoseconds:0','sample.bootNanoseconds')
    with tempfile.TemporaryDirectory(prefix='sfnp-camera-client-tests-') as directory:
        temp=Path(directory)
        for name,text in cases.items():
            modified=temp/(name+'.cpp');modified.write_text(text);binary=temp/name
            flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if name=='sanitize' else []
            command=['g++','-std=c++17','-Wall','-Wextra','-Wpedantic','-Werror','-I'+str(root),'-I'+str(root/'receiver'),'-pthread',*flags,str(root/'tests/stream-bridge-camera-client/client_tests.cpp'),str(modified),'-o',str(binary)]
            with (out/(name+'-build.log')).open('w') as log:subprocess.run(command,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=60)
            result=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=20);(out/(name+'.log')).write_text(result.stdout)
            expected={'early-api-admission':'enable request alone cannot admit rendering before API acknowledgment','stale-ack-admitted':'stale enable acknowledgment never revives explicitly disabled session','resume-never-restores':'real suspend offset revokes old ready acknowledgment immediately despite unchanged MR demand','stall-is-suspend':'pure scheduling stall advances both clocks and never renews settings'}.get(name)
            if expected:assert result.returncode and expected in result.stdout,(name,result.stdout)
            else:assert result.returncode==0,(name,result.stdout)
            results[name]={'exit_status':result.returncode,'expected_red':bool(expected)}
    (out/'results.json').write_text(json.dumps({'results':results,'client_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'scope':'Production C++ client and real Python subprocess/sockets; explicit fake API acknowledgments. No native settings or device actions. TemporaryDirectory cleans child executables after reaping.'},indent=2)+'\n');print(json.dumps(results))
if __name__=='__main__':main()
