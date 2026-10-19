"""Camera faults must remain visible without overriding a user's picture verdict."""
import importlib.util
from pathlib import Path
import types
import sys
import tempfile
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module)
    return module

fixture=load('sfnp_camera_workflow_fixture',ROOT/'tests/test_setup_workflow.py')
bridge,flow=fixture.b,fixture.flow

def camera(status='warning'):
    return {'schema':'frame-camera-startup-observation-v1','status':status,
            'reason_codes':['firmware-response-missing'] if status=='warning' else [],
            'camera_pixels_verified':False,'automatic_restart':False,'automatic_rollback':False}

def health(status='warning'):
    return {'status':'ready','software_ready':True,'camera_health':camera(status)}


class Adapter(unittest.TestCase):
    def test_ui_ready_preserves_separate_camera_warning_and_never_verifies_pixels(self):
        source=types.SimpleNamespace(resume=mock.Mock(),camera_health=mock.Mock(return_value=camera()))
        module=types.SimpleNamespace(NativeEvidenceSource=lambda home:source,
            wait_ready=lambda *a,**kw:{'status':'ready','process_identities':{'compositor':1}})
        baseline={'baseline':{'cursor':'fixed'},'checkpoint':{'offset':0}}
        result=bridge.StartupHealth(module,Path('/unused')).check(baseline)
        self.assertTrue(result['software_ready']);self.assertEqual(result['camera_health'],camera())
        self.assertFalse(result['headset_picture_verified']);self.assertFalse(result['automatic_rollback'])
        source.camera_health.assert_called_once_with(baseline['baseline'])

    def test_optional_camera_observation_cannot_fail_ui_or_trigger_recovery(self):
        for failure in (PermissionError,RuntimeError):
            with self.subTest(failure=failure):
                source=types.SimpleNamespace(resume=lambda value:None,
                    camera_health=mock.Mock(side_effect=failure('private log content')))
                module=types.SimpleNamespace(NativeEvidenceSource=lambda home:source,
                    wait_ready=lambda *a,**kw:{'status':'ready'})
                result=bridge.StartupHealth(module,Path('/unused')).check({'baseline':{},'checkpoint':{}})
                self.assertTrue(result['software_ready']);self.assertEqual(result['camera_health']['status'],'unavailable')
                self.assertNotIn('private',str(result));self.assertFalse(result['camera_health']['camera_pixels_verified'])

    def test_older_observer_without_optional_method_remains_compatible(self):
        module=types.SimpleNamespace(NativeEvidenceSource=lambda home:types.SimpleNamespace(resume=lambda value:None),
            wait_ready=lambda *a,**kw:{'status':'ready'})
        result=bridge.StartupHealth(module,Path('/unused')).check({'baseline':{},'checkpoint':{}})
        self.assertTrue(result['software_ready']);self.assertNotIn('camera_health',result)


class Feedback(unittest.TestCase):
    def test_camera_warning_does_not_undo_a_picture_confirmed_by_user(self):
        ports=fixture.Ports(prepared=health());ui=fixture.UI()
        self.assertEqual(flow.Workflow(ports,ui).run('update'),0)
        self.assertIn('bridge_camera_warning',ui.messages)
        self.assertIn('finished_prepared',ports.events);self.assertNotIn('restore_files',ports.events)

    def test_no_offers_read_only_recheck_then_explicit_undo_with_camera_warning(self):
        ports=fixture.Ports(prepared=health());ui=fixture.UI(choices=['rollback','exit'],picture=False)
        self.assertEqual(flow.Workflow(ports,ui).run('update'),2)
        self.assertGreaterEqual(ui.messages.count('bridge_camera_warning'),2)
        self.assertEqual(ports.events.count('restart_prepared'),1)
        self.assertEqual(ports.events.count('restart_original'),1)
        self.assertGreaterEqual(ports.events.count('prepared_health'),2)
        self.assertIn('finished_original',ports.events)

    def test_no_errors_observed_or_unavailable_is_not_a_camera_pixel_verdict(self):
        for state in ('no-errors-observed','unavailable'):
            with self.subTest(state=state):
                ports=fixture.Ports(prepared=health(state));ui=fixture.UI()
                ui.confirm=mock.Mock(return_value=True)
                self.assertEqual(flow.Workflow(ports,ui).run('update'),0)
                ui.confirm.assert_called_once_with('bridge_picture')
                self.assertNotIn('bridge_camera_warning',ui.messages)

    def test_warning_is_available_in_both_product_languages(self):
        module=load('sfnp_camera_presenter',ROOT/'setup/steam_link_setup_ui.py')
        for language in ('en','de'):
            presenter=module.Presenter(lang=language)
            warning=presenter.text('bridge_camera_warning')
            self.assertTrue(len(warning)>100);self.assertNotEqual(warning,'bridge_camera_warning')


class DurableActivation(unittest.TestCase):
    def setUp(self):
        self.folder=tempfile.TemporaryDirectory(prefix='sfnp-health-activation-')
        self.addCleanup(self.folder.cleanup)
        self.frame_fixture=load('sfnp_camera_lifecycle_fixture',ROOT/'tests/test_setup_frame.py')
        self.system=self.frame_fixture.System()
        self.system.require_fresh_vr=True
        self.runtime=self.frame_fixture.frame.FrameLifecycle(fixture.s,Path(self.folder.name),runner=self.system,
            sleeper=self.system.sleep,monotonic=lambda:self.system.now,poll_attempts=4,poll_interval=.01,phase_timeout=1)
        self.lost=False;self.health_baselines=0

    def current_health(self):
        # The real startup gate requires this observed invocation to differ
        # from the post-restart invocation; it cannot use a new retry snapshot.
        return {'baseline':{'invocation':self.system.units['steamvr.service']['InvocationID']},'checkpoint':{}}

    def ports(self):
        ports=object.__new__(bridge.Automation)
        ports.baseline=None;ports.restore_baseline=None;ports.pc_recovery_error=None
        ports.phase=lambda stage:None;ports.runtime=mock.Mock()
        def call(command,**values):
            if command=='bridge-health-baseline':
                self.health_baselines+=1;return self.current_health()
            self.assertEqual(command,'bridge-runtime-restart')
            result=self.runtime.restart(restoring=values.get('restoring')is True,
                health_receipt=self.current_health())
            if self.lost:self.lost=False;raise OSError('reply lost after successful activation')
            return result
        ports.call=call;return ports

    def test_lost_reply_reuses_pre_restart_health_without_restarting_again(self):
        before=self.current_health();ports=self.ports();self.lost=True
        with self.assertRaises(OSError):ports.restart_original()
        actions=list(self.system.actions)
        ports.restart_original()
        self.assertEqual(ports.restore_baseline,before);self.assertEqual(self.health_baselines,1)
        self.assertEqual(self.system.actions,actions)
        self.assertNotEqual(ports.restore_baseline,self.current_health())

    def test_new_setup_process_uses_health_receipt_persisted_before_activation(self):
        before=self.current_health();ports=self.ports();ports.restart_original()
        actions=list(self.system.actions)
        # A new CLI/SSH object has no in-memory baseline; the owned journal does.
        self.runtime=self.frame_fixture.frame.FrameLifecycle(fixture.s,Path(self.folder.name),runner=self.system,
            sleeper=self.system.sleep,monotonic=lambda:self.system.now,poll_attempts=4,poll_interval=.01,phase_timeout=1)
        replacement=self.ports();replacement.restart_original()
        self.assertEqual(replacement.restore_baseline,before)
        self.assertEqual(self.system.actions,actions)

    def test_prepared_activation_receives_its_durable_health_authority(self):
        before=self.current_health();ports=self.ports();ports.restart_prepared()
        self.assertEqual(ports.baseline,before);self.assertNotEqual(ports.baseline,self.current_health())

    def test_restart_rpc_supplies_the_pinned_observer_source(self):
        bundle=types.SimpleNamespace(value={'schema':'fixture'},files={'frame/tools/compositor_startup_health.py':b'owned checker'})
        request=bridge.request(bundle,'bridge-runtime-restart',restoring=True)
        self.assertTrue(request['restoring']);self.assertEqual(request['health_source'],'b3duZWQgY2hlY2tlcg==')

    def test_actual_remote_dispatch_retains_health_and_rejects_changed_observer_before_actions(self):
        files=fixture.fixtures.Tests('runTest');files.setUp();self.addCleanup(files.doCleanups)
        before=self.current_health()
        observer=types.SimpleNamespace(baseline=self.current_health)
        helper=types.SimpleNamespace(FrameLifecycle=lambda *args:self.runtime)
        request=bridge.request(files.bundle,'bridge-runtime-restart',restoring=True)
        with mock.patch.object(bridge,'load_module',return_value=object()), \
             mock.patch.object(bridge,'StartupHealth',return_value=observer), \
             mock.patch.object(bridge,'load_helper',return_value=helper):
            result=bridge.remote_request(fixture.s,request,unit=files.unit)
            self.assertEqual(result['health_receipt'],before)
            actions=list(self.system.actions)
            repeated=bridge.remote_request(fixture.s,request,unit=files.unit)
            self.assertEqual(repeated['health_receipt'],before)
            self.assertEqual(self.system.actions,actions)
            changed={**request,'health_source':'Y2hhbmdlZA=='}
            with self.assertRaises(fixture.s.SetupError):
                bridge.remote_request(fixture.s,changed,unit=files.unit)
            self.assertEqual(self.system.actions,actions)


if __name__=='__main__':unittest.main()
