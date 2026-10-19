"""Execute the release-only C selector with private config and real exec/spawn."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReleaseLaunch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="sfnp-release-launch-")
        cls.root = Path(cls.temp.name)
        cls.private = cls.root / "private"
        cls.private.mkdir(mode=0o700)
        cls.target = cls.root / "vrlink"
        cls.foreign = cls.root / "other"
        cls.launcher = cls.root / "launch"
        cls.selector = cls.root / "selector.so"
        cls.payload = cls.root / "payload.so"
        cls.foreign_so = cls.root / "foreign.so"
        cls.audit = cls.root / "audit.so"
        cls.config = cls.private / "launch.conf"
        program = cls.root / "program.c"
        program.write_text(r'''
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static const char* value(const char* key){const char* v=getenv(key);return v?v:"";}
int main(int argc,char** argv){
 if(argc==3){char* args[]={argv[2],NULL};
  if(!strcmp(argv[1],"unlink"))unlink(getenv("SFNP_STREAM_BRIDGE_CONFIG"));
  if(!strcmp(argv[1],"spawn")){pid_t pid;int status;if(posix_spawn(&pid,argv[2],NULL,NULL,args,environ))return 3;
   if(waitpid(pid,&status,0)!=pid||!WIFEXITED(status)){return 4;}return WEXITSTATUS(status);}
  execv(argv[2],args);return 5;
 }
 printf("PAYLOAD=%d\nFOREIGN=%d\nPRELOAD=%s\nAUDIT=%s\nTRACE=%s\nRAW=%s\n",
  dlsym(RTLD_DEFAULT,"payload_marker")!=NULL,dlsym(RTLD_DEFAULT,"foreign_marker")!=NULL,
  value("LD_PRELOAD"),value("LD_AUDIT"),value("FRAME_MASK_TRACE"),value("GHVR_FRAME_COMPOSITOR_RAW_OUTPUT"));return 0;
}
''')
        cls.run_build(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(program), "-ldl", "-o", str(cls.launcher)])
        shutil.copy2(cls.launcher, cls.target)
        shutil.copy2(cls.launcher, cls.foreign)
        for name, function in [("payload", "payload_marker"), ("foreign", "foreign_marker")]:
            source = cls.root / (name + ".c")
            source.write_text("int " + function + "(void){return 1;}\n")
            cls.run_build(["cc", "-shared", "-fPIC", str(source), "-o", str(cls.root / (name + ".so"))])
        audit = cls.root / "audit.c"
        audit.write_text("#define _GNU_SOURCE 1\n#include <link.h>\nunsigned int la_version(unsigned int v){(void)v;return LAV_CURRENT;}\n")
        cls.run_build(["cc", "-shared", "-fPIC", str(audit), "-o", str(cls.audit)])
        cls.run_build(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-shared", "-fPIC",
                       "-DFRAME_LAUNCH_RELEASE_BRIDGE", '-DFRAME_STREAM_BRIDGE_TARGET_PATH="' + str(cls.target) + '"',
                       str(ROOT / "receiver/compositor_launch_selector.c"), str(ROOT / "receiver/stream_bridge_launch_config.c"),
                       "-ldl", "-o", str(cls.selector)])

    @classmethod
    def run_build(cls, arguments):
        result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        if self.config.is_symlink():
            self.config.unlink()
        self.private.chmod(0o700)
        self.config.write_text("SFNP_STREAM_LAUNCH/1\n" + str(self.target) + "\n" + str(self.selector) + "\n" + str(self.payload) + "\n")
        self.config.chmod(0o600)

    def launch(self, target=None, mode="exec", dirty=False, config=True, config_path=None):
        env = {k: v for k, v in os.environ.items() if k not in ("LD_PRELOAD", "LD_AUDIT", "SFNP_STREAM_BRIDGE_CONFIG")}
        env.update(LD_PRELOAD=str(self.foreign_so) + ":" + str(self.selector) + (":" + str(self.payload) if dirty else ""),
                   LD_AUDIT=str(self.audit), FRAME_MASK_TRACE="preserved", GHVR_FRAME_COMPOSITOR_RAW_OUTPUT="/foreign/value")
        if config:
            env["SFNP_STREAM_BRIDGE_CONFIG"] = str(config_path or self.config)
        result = subprocess.run([str(self.launcher), mode, str(target or self.target)], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10, check=True)
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def test_actual_target_exec_and_spawn(self):
        for mode in ("exec", "spawn"):
            with self.subTest(mode=mode):
                report = self.launch(mode=mode)
                self.assertEqual(report["PAYLOAD"], "1")
                self.assertEqual(report["FOREIGN"], "1")
                self.assertEqual(report["AUDIT"], str(self.audit))
                self.assertEqual(report["TRACE"], "preserved")
                self.assertEqual(report["RAW"], "/foreign/value")
                # The original delimiter next to an owned token is retained;
                # repeated loader delimiters are valid and part of the core contract.
                self.assertEqual(report["PRELOAD"], str(self.foreign_so) + "::" + str(self.selector) + ":" + str(self.payload))

    def test_foreign_children_never_receive_heavy_payload(self):
        for mode in ("exec", "spawn"):
            report = self.launch(self.foreign, mode, dirty=True)
            self.assertEqual(report["PAYLOAD"], "0")
            self.assertEqual(report["FOREIGN"], "1")
            self.assertNotIn(str(self.payload), report["PRELOAD"])
            self.assertEqual(report["AUDIT"], str(self.audit))

    def test_config_is_frozen_before_exec(self):
        report = self.launch(mode="unlink")
        self.assertFalse(self.config.exists())
        self.assertEqual(report["PAYLOAD"], "1")

    def test_missing_or_insecure_file_keeps_native_launch(self):
        self.assertEqual(self.launch(config=False)["PAYLOAD"], "0")
        self.config.chmod(0o644)
        self.assertEqual(self.launch()["PAYLOAD"], "0")
        self.config.chmod(0o600)
        self.private.chmod(0o755)
        self.assertEqual(self.launch()["PAYLOAD"], "0")

    def test_unknown_target_and_malformed_contents_refused(self):
        current = self.config.read_text()
        for value in (current.replace(str(self.target), str(self.foreign)), current[:-1], current + "extra\n",
                      current.replace("SFNP_STREAM_LAUNCH/1", "SFNP_STREAM_LAUNCH/2"), current + "\0", "x" * 4097):
            with self.subTest(length=len(value)):
                self.config.write_text(value)
                self.assertEqual(self.launch()["PAYLOAD"], "0")

    def test_symlink_and_hardlink_are_not_private_config(self):
        actual = self.private / "actual.conf"
        shutil.copy2(self.config, actual)
        self.config.unlink()
        self.config.symlink_to(actual.name)
        self.assertEqual(self.launch()["PAYLOAD"], "0")
        self.config.unlink()
        os.link(actual, self.config)
        self.assertEqual(self.launch()["PAYLOAD"], "0")

    def test_parent_symlink_and_unsafe_library_paths_refused(self):
        alias = self.root / "alias"
        alias.symlink_to(self.private, target_is_directory=True)
        self.assertEqual(self.launch(config_path=alias / "launch.conf")["PAYLOAD"], "0")
        current = self.config.read_text()
        for unsafe in ("/tmp/../bad.so", "/tmp/bad:bad.so", "/tmp/bad bad.so", str(self.selector)):
            self.config.write_text(current.replace(str(self.payload), unsafe))
            self.assertEqual(self.launch()["PAYLOAD"], "0")


if __name__ == "__main__":
    unittest.main()
