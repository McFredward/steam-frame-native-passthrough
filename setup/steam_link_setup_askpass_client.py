#!/usr/bin/env python3
"""One OpenSSH askpass request to the installer's authenticated loopback broker.

Only the broker token and port are inherited in the environment. The answer
travels in memory over the local socket and the dedicated stdout response pipe;
it is never placed in arguments, environment variables, logs or temporary files.
"""
from __future__ import annotations

import os
import re
import socket
import struct
import sys
import time

MAGIC = b'SFNPASK1'
MAX_PROMPT = 4096
MAX_ANSWER = 1024
TIMEOUT_SECONDS = 300.0
CANCEL = 0xffffffff


def _remaining(deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError('Askpass request expired.')
    return remaining


def _receive(connection, count, deadline):
    answer = bytearray()
    while len(answer) < count:
        connection.settimeout(_remaining(deadline))
        block = connection.recv(count - len(answer))
        if not block:
            raise ValueError('Incomplete askpass response.')
        answer.extend(block)
    return answer


def request(prompt, environment=None):
    """Return a bounded validated UTF-8 answer, or None on refusal/cancellation."""
    environment = os.environ if environment is None else environment
    port = environment.get('SFNP_ASKPASS_PORT', '')
    token = environment.get('SFNP_ASKPASS_TOKEN', '')
    if (type(port) is not str or not re.fullmatch(r'[1-9][0-9]{0,4}', port)
            or not 0 < int(port) <= 65535 or type(token) is not str
            or not re.fullmatch(r'[0-9a-fA-F]{64}', token)
            or type(prompt) is not str or '\x00' in prompt):
        return None
    try:
        encoded = prompt.encode('utf-8', errors='strict')
        if len(encoded) > MAX_PROMPT:
            return None
        packet = MAGIC + token.encode('ascii') + struct.pack('!I', len(encoded)) + encoded
        deadline = time.monotonic() + TIMEOUT_SECONDS
        with socket.create_connection(('127.0.0.1', int(port)), timeout=_remaining(deadline)) as connection:
            connection.settimeout(_remaining(deadline))
            connection.sendall(packet)
            size = struct.unpack('!I', _receive(connection, 4, deadline))[0]
            if size == CANCEL or size > MAX_ANSWER:
                return None
            answer = _receive(connection, size, deadline)
            accepted = False
            try:
                if any(value in answer for value in (0, 10, 13)):
                    return None
                answer.decode('utf-8', errors='strict')
                accepted = True
                return answer
            except (UnicodeError, ValueError):
                return None
            finally:
                if not accepted:
                    answer[:] = b'\x00' * len(answer)
    except (OSError, UnicodeError, ValueError, OverflowError, TypeError):
        return None


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    answer = None
    try:
        if len(argv) != 1:
            return 1
        answer = request(argv[0])
        if answer is None:
            return 1
        sys.stdout.buffer.write(answer)
        sys.stdout.buffer.write(b'\n')
        sys.stdout.buffer.flush()
        return 0
    except (OSError, ValueError, TypeError, KeyboardInterrupt):
        return 1
    finally:
        if answer is not None:
            answer[:] = b'\x00' * len(answer)


if __name__ == '__main__':
    raise SystemExit(main())
