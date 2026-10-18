"""Confirmed setup automation with exact recovery and explicit retries.

Only the interactive caller may construct/run this after Apply confirmation.
The Frame UI stays running throughout file preparation. A short coordinated
restart happens after preparation or restoration, followed by native health.
An uninstall is not a substitute for rolling back the current operation.
"""


class Workflow:
    def __init__(self,ports,ui):
        self.ports=ports;self.ui=ui

    def healthy(self,*,original=False):
        while True:
            value=self.ports.original_health() if original else self.ports.prepared_health()
            if type(value)is not dict:return value is True
            if value.get('software_ready') is True:return True
            if value.get('status')!='not-ready':return False
            # A slow start is not a crash. A bounded probe offers another
            # observation without restarting, stopping UI or a timed rollback.
            self.ui.say('bridge_health_waiting')
            if self.ui.choose_action(['retry','rollback'],recommended='retry')!='retry':return False

    def recover(self):
        # A cancellation can stop network I/O, but must not cancel the prompts
        # needed to restore the exact original, running Frame state.
        resume=getattr(self.ui,'allow_recovery',None)
        if resume:resume()
        while True:
            try:
                self.ports.restore_files()
                self.ports.restart_original()
                healthy=self.healthy(original=True)
                if not healthy:raise self.ports.error('The original Frame startup could not be confirmed; recovery remains incomplete.')
                self.ports.finish(restored=True)
                self.ui.say('bridge_rolled_back')
                return True
            except BaseException as error:
                self.ports.failure(error,recovery=True)
                self.ui.say('bridge_recovery_incomplete')
                if self.ui.choose_action(['retry','exit'],recommended='retry')!='retry':return False

    def run(self,action):
        while True:
            try:
                self.ports.begin()
                self.ui.say('bridge_stopping')
                self.ports.stop_pc()
                self.ports.prepare(action)
                self.ui.say('bridge_starting')
                self.ports.restart_prepared()
                if not self.healthy():
                    raise self.ports.error('The prepared Frame startup failed its health check.')
                self.ui.say('bridge_health_ok')
                if not self.ui.confirm('bridge_picture'):
                    raise self.ports.error('The user could not confirm a usable Frame picture.')
                self.ports.finish(restored=False)
                self.ui.say('bridge_pc_stopped')
                self.ui.say('bridge_removed' if action=='uninstall' else 'bridge_installed')
                return 0
            except BaseException as error:
                self.ports.failure(error,recovery=False)
                self.ui.say('bridge_partial')
                # A checkpoint may describe uncertain writes after an SSH
                # interruption. Restore/reconcile it before offering a new
                # installation attempt; never blindly replay that mutation.
                if not self.recover():return 2
                if isinstance(error,KeyboardInterrupt):return 130
                if self.ui.choose_action(['retry','exit'],recommended='retry')!='retry':return 130 if isinstance(error,KeyboardInterrupt) else 2

