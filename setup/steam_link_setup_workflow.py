"""Confirmed setup automation with exact recovery and explicit retries.

Only the interactive caller may construct/run this after Apply confirmation.
The Frame UI stays running throughout file preparation. A short coordinated
restart happens after preparation or restoration, followed by native health.
An uninstall is not a substitute for rolling back the current operation.
"""


class Workflow:
    def __init__(self,ports,ui):
        self.ports=ports;self.ui=ui;self._cancellation_reported=False

    def check_cancelled(self):
        cancelled=getattr(self.ui,'cancelled',None)
        if cancelled and cancelled():raise KeyboardInterrupt()

    def retain_cancelled(self,error,*,recovery=False):
        # Explicit Ctrl+C stops the operation. Checkpoints and runtime state are
        # left for a later deliberate recovery; cancellation never authorizes it.
        if self._cancellation_reported:return
        self._cancellation_reported=True
        try:self.ports.failure(error,recovery=recovery)
        except (Exception,KeyboardInterrupt):pass
        try:self.ui.say('bridge_cancelled_retained')
        except (Exception,KeyboardInterrupt):pass

    def healthy(self,*,original=False):
        while True:
            self.check_cancelled()
            value=self.ports.original_health() if original else self.ports.prepared_health()
            self.check_cancelled()
            if type(value)is not dict:return value is True
            if value.get('software_ready') is True:return True
            if value.get('status')!='not-ready':return False
            # A slow start is not a crash. A bounded probe offers another
            # observation without restarting, stopping UI or a timed rollback.
            self.ui.say('bridge_health_waiting')
            self.check_cancelled()
            action=self.ui.choose_action(['retry','rollback'],recommended='retry')
            self.check_cancelled()
            if action!='retry':return False

    def picture_health(self,*,original=False):
        """Observe again after a rejected picture, without changing either device."""
        self.check_cancelled()
        self.ui.say('bridge_picture_rechecking')
        try:
            value=self.ports.original_health() if original else self.ports.prepared_health()
            ready=value is True or type(value)is dict and value.get('software_ready')is True
        except KeyboardInterrupt:raise
        except Exception as error:
            self.check_cancelled()
            self.ports.failure(error,recovery=original)
            ready=False
        self.check_cancelled()
        self.ui.say('bridge_picture_checks_ok' if ready else 'bridge_picture_checks_unconfirmed')

    def picture(self,*,original=False):
        """Keep a visible No separate from cancellation and explicit Undo."""
        while True:
            self.check_cancelled()
            answer=self.ui.confirm('bridge_original_picture' if original else 'bridge_picture')
            self.check_cancelled()
            if answer is True:return True
            if answer is not False:raise KeyboardInterrupt()
            self.picture_health(original=original)
            self.check_cancelled()
            actions=['verify','exit'] if original else ['verify','rollback','exit']
            action=self.ui.choose_action(actions,recommended='verify')
            self.check_cancelled()
            if action=='rollback' and not original:return False
            if action!='verify':raise KeyboardInterrupt()
            self.picture_health(original=original)

    def recover(self):
        try:
            # This entry is reached only by an explicit recovery choice or a
            # genuine failure. Do not clear a newer Ctrl+C racing that entry.
            self.check_cancelled()
            resume=getattr(self.ui,'allow_recovery',None)
            if resume:resume()
            self.check_cancelled()
            while True:
                try:
                    self.check_cancelled()
                    self.ports.restore_files()
                    self.check_cancelled()
                    self.ports.restart_original()
                    self.check_cancelled()
                    healthy=self.healthy(original=True)
                    if not healthy:raise self.ports.error('The original Frame startup could not be confirmed; recovery remains incomplete.')
                    if not self.picture(original=True):
                        raise self.ports.error('The original Frame picture could not be confirmed; recovery remains incomplete.')
                    self.check_cancelled()
                    self.ports.finish(restored=True)
                    self.check_cancelled()
                    self.ui.say('bridge_rolled_back')
                    return True
                except KeyboardInterrupt:raise
                except BaseException as error:
                    self.check_cancelled()
                    self.ports.failure(error,recovery=True)
                    self.check_cancelled()
                    self.ui.say('bridge_recovery_incomplete')
                    self.check_cancelled()
                    action=self.ui.choose_action(['retry','exit'],recommended='retry')
                    self.check_cancelled()
                    if action!='retry':return False
        except KeyboardInterrupt as error:
            self.retain_cancelled(error,recovery=True)
            raise

    def run(self,action):
        try:
            while True:
                try:
                    self.check_cancelled()
                    self.ports.begin()
                    self.check_cancelled()
                    self.ui.say('bridge_stopping')
                    self.check_cancelled()
                    self.ports.stop_pc()
                    self.check_cancelled()
                    self.ports.prepare(action)
                    self.check_cancelled()
                    self.ui.say('bridge_starting')
                    self.check_cancelled()
                    self.ports.restart_prepared()
                    self.check_cancelled()
                    if not self.healthy():
                        raise self.ports.error('The prepared Frame startup failed its health check.')
                    self.ui.say('bridge_health_ok')
                    self.check_cancelled()
                    picture=self.picture()
                    # A concurrent Ctrl+C overrides even a just-resolved Yes
                    # or explicit Undo selection; neither may proceed afterward.
                    self.check_cancelled()
                    if not picture:
                        raise self.ports.error('The user could not confirm a usable Frame picture.')
                    self.ports.finish(restored=False)
                    self.check_cancelled()
                    self.ui.say('bridge_pc_stopped')
                    self.ui.say('bridge_removed' if action=='uninstall' else 'bridge_installed')
                    return 0
                except KeyboardInterrupt:raise
                except BaseException as error:
                    self.check_cancelled()
                    self.ports.failure(error,recovery=False)
                    self.check_cancelled()
                    self.ui.say('bridge_partial')
                    self.check_cancelled()
                    # Only a genuine operation failure authorizes this existing
                    # safe recovery path; explicit cancellation was split above.
                    if not self.recover():return 2
                    self.check_cancelled()
                    answer=self.ui.choose_action(['retry','exit'],recommended='retry')
                    self.check_cancelled()
                    if answer!='retry':return 2
        except KeyboardInterrupt as error:
            self.retain_cancelled(error)
            return 130

