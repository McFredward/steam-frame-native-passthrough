"""Camera faults must remain visible without overriding a user's picture verdict."""
import importlib.util
from pathlib import Path
import types
import sys
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


if __name__=='__main__':unittest.main()
