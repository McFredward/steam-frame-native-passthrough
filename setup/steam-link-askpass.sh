#!/bin/sh
# This child only returns one in-memory broker answer to OpenSSH's stdout pipe.
case "${SFNP_ASKPASS_PYTHON-}" in /*) ;; *) exit 1 ;; esac
case "${SFNP_ASKPASS_CLIENT-}" in /*) ;; *) exit 1 ;; esac
exec "$SFNP_ASKPASS_PYTHON" -I -B "$SFNP_ASKPASS_CLIENT" "$@" 2>/dev/null
