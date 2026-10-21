"""Confirmed setup automation with exact recovery and explicit retries.

Only the interactive caller may construct/run this after Apply confirmation.
The Frame UI stays running throughout file preparation. A short coordinated
restart happens after preparation or restoration, followed by native health.
An uninstall is not a substitute for rolling back the current operation.
"""


class Workflow:
    def __init__(self,ports,ui):
        self.ports=ports;self.ui=ui;self._cancellation_reported=False

    @property
    def frame_required(self):
        return getattr(self.ports,'frame_required',True) is not False

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
            self.camera_warning(value)
            if value.get('software_ready') is True:return True
            if value.get('status')!='not-ready':return False
            # A slow start is not a crash. A bounded probe offers another
            # observation without restarting, stopping UI or a timed rollback.
            self.ui.say('bridge_health_waiting')
            self.check_cancelled()
            action=self.ui.choose_action(['retry','rollback'],recommended='retry')
            self.check_cancelled()
            if action!='retry':return False

    def camera_warning(self,value):
        """Camera faults are a visible warning, never automatic recovery."""
        observation=value.get('camera_health') if type(value)is dict else None
        if type(observation)is dict and observation.get('status')=='warning':
            self.ui.say('bridge_camera_warning')

    def picture_health(self,*,original=False):
        """Observe again after a rejected picture, without changing either device."""
        self.check_cancelled()
        self.ui.say('bridge_picture_rechecking')
        try:
            value=self.ports.original_health() if original else self.ports.prepared_health()
            self.camera_warning(value)
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
            self.observe_picture(original=original,status='started')
            answer=self.ui.confirm('bridge_original_picture' if original else 'bridge_picture')
            self.check_cancelled()
            self.observe_picture(original=original,status='ok' if answer is True else 'failed' if answer is False else 'cancelled')
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

    def observe_picture(self,*,original,status):
        # Optional diagnostic metadata cannot turn Yes into automatic recovery.
        observe=getattr(self.ports,'picture_observation',None)
        if observe:
            try:observe(original=original,status=status)
            except Exception:pass

    def finish_confirmed(self,*,restored):
        """Finalize records after Yes; a cleanup error never authorizes Undo.

        The production finish ports reconcile current records before deleting
        exact owned data, and treat already removed records as complete. Retry
        only repeats that idempotent cleanup, never prepare/health/restart.
        """
        while True:
            self.check_cancelled()
            try:self.ports.finish(restored=restored)
            except KeyboardInterrupt:raise
            except Exception as error:
                self.check_cancelled()
                try:self.ports.failure(error,recovery=restored)
                except Exception:pass
                self.check_cancelled()
                self.ui.say('bridge_finalize_incomplete' if self.frame_required else 'bridge_pc_finalize_incomplete')
                self.check_cancelled()
                action=self.ui.choose_action(['retry','exit'],recommended='retry')
                self.check_cancelled()
                if action!='retry':return False
            else:
                self.check_cancelled()
                return True

    def announce_completed(self,*keys):
        """Presentation failures cannot undo or repeat a completed transaction."""
        for key in keys:
            self.check_cancelled()
            try:self.ui.say(key)
            except KeyboardInterrupt:raise
            except Exception as error:
                self.check_cancelled()
                try:self.ports.failure(error,recovery=False)
                except Exception:pass
        self.check_cancelled()

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
                    if not healthy:raise self.ports.error('The original installation could not be confirmed; recovery remains incomplete.')
                    if self.frame_required and not self.picture(original=True):
                        raise self.ports.error('The original Frame picture could not be confirmed; recovery remains incomplete.')
                    self.check_cancelled()
                    break
                except KeyboardInterrupt:raise
                except BaseException as error:
                    self.check_cancelled()
                    self.ports.failure(error,recovery=True)
                    self.check_cancelled()
                    self.ui.say('bridge_recovery_incomplete' if self.frame_required else 'bridge_pc_recovery_incomplete')
                    self.check_cancelled()
                    action=self.ui.choose_action(['retry','exit'],recommended='retry')
                    self.check_cancelled()
                    if action!='retry':return False
            # Verification is a boundary: cleanup cannot restart recovery.
            if not self.finish_confirmed(restored=True):return False
            self.announce_completed('bridge_rolled_back' if self.frame_required else 'bridge_pc_rolled_back')
            return True
        except KeyboardInterrupt as error:
            self.retain_cancelled(error,recovery=True)
            raise

    def run(self,action,*,reconcile=False):
        try:
            while True:
                try:
                    self.check_cancelled()
                    if reconcile:
                        self.ui.say('bridge_pending_apply' if self.frame_required else 'bridge_pc_pending_apply')
                        self.check_cancelled()
                        self.ports.reconcile_for_apply()
                        self.check_cancelled()
                    self.ports.begin()
                    self.check_cancelled()
                    self.ui.say('bridge_stopping' if self.frame_required else 'bridge_pc_only_stopping')
                    self.check_cancelled()
                    self.ports.stop_pc()
                    self.check_cancelled()
                    self.ports.prepare(action)
                    self.check_cancelled()
                    if self.frame_required:
                        self.ui.say('bridge_starting')
                        self.check_cancelled()
                        self.ports.restart_prepared()
                        self.check_cancelled()
                    if not self.healthy():
                        raise self.ports.error('The prepared installation failed its health check.')
                    self.ui.say('bridge_health_ok' if self.frame_required else 'bridge_pc_health_ok')
                    self.check_cancelled()
                    picture=self.picture() if self.frame_required else True
                    # A concurrent Ctrl+C overrides even a just-resolved Yes
                    # or explicit Undo selection; neither may proceed afterward.
                    self.check_cancelled()
                    if not picture:
                        raise self.ports.error('The user could not confirm a usable Frame picture.')
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
                else:
                    # Keep finalization outside the automatic failure recovery
                    # scope. Required file/startup/picture checks passed.
                    if not self.finish_confirmed(restored=False):return 2
                    self.announce_completed('bridge_pc_stopped',
                        'bridge_removed' if action=='uninstall' else 'bridge_installed' if self.frame_required else 'bridge_pc_only_installed')
                    return 0
        except KeyboardInterrupt as error:
            self.retain_cancelled(error)
            return 130

