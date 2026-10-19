#pragma once
// A real independent subprocess with a deliberately controllable API-result
// boundary. This fixture does not model or initialize native OpenVR.
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
namespace sfnp_camera_fixture {
inline void write(const std::filesystem::path& path,const char* mode="normal"){
    std::filesystem::create_directory(path/"tools");chmod((path/"tools").c_str(),0700);
    std::ofstream config(path/"camera-session.json");config<<mode;config.close();chmod((path/"camera-session.json").c_str(),0600);
    std::ofstream helper(path/"tools/stream_bridge_camera.py");helper<<R"PY(
import sys,pathlib,time,os
assert sys.argv[1]=='serve' and sys.argv[2]=='--root'
root=pathlib.Path(sys.argv[3]);mode=(root/'camera-session.json').read_text();previous=None;last=None
def mark(text):
 if not root.exists():return
 with (root/'events').open('a') as f:f.write(text+'\n')
def emit(text):
 try:print(text,flush=True)
 except BrokenPipeError:
  sys.stdout=open(os.devnull,'w');raise SystemExit(0)
try:
 mark('started');emit('R 0')
 for raw in sys.stdin:
  op,value=raw.split();seq=int(value);mark(op+' '+value)
  if op=='E':
   last=seq
   if mode=='gate':
    mark('enable-pending')
    while root.exists() and not (root/'enable-release').exists():time.sleep(.001)
   if mode=='hold':previous=seq
   elif mode=='bad':emit('X '+value)
   else:
    emit('A '+value)
    if mode=='health':
     while root.exists() and not (root/'health-loss').exists():time.sleep(.001)
     emit('N '+value);break
  elif op=='D':
   if mode=='resume' and last is not None:
    emit('A '+str(last));mark('restore-pending')
    while root.exists() and not (root/'restore-release').exists():time.sleep(.001)
   if previous is not None:
    emit('A '+str(previous));previous=None;mark('stale-sent')
    while root.exists() and not (root/'release').exists():time.sleep(.001)
   mark('restored');emit('D '+value)
finally:mark('eof-restored')
)PY";helper.close();chmod((path/"tools/stream_bridge_camera.py").c_str(),0600);
}
inline std::string events(const std::filesystem::path& path){std::ifstream in(path/"events");return {std::istreambuf_iterator<char>(in),{}};}
}
