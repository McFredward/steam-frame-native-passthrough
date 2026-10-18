# Friendly setup presentation and discovery

`setup/steam_link_setup_ui.py` is an optional standard-library presentation layer.
The engine owns the installation/report contract and bypasses this layer for its
machine-readable `--check` flow. Load it from its verified bundle path using
`importlib` (register the module in `sys.modules` before execution for dataclasses).

```python
ui = Presenter(lang=args.lang, input_enabled=interactive)
ui.banner(inspection=True)
with ui.progress('pc_check'):
    inventory = local.inventory()
found = find_frame(validate_frame, cached_hosts=previous_successful_hosts,
                   presenter=ui)
if found:
    remote, inventory = found.value  # Reuse the authenticated result.
```

`validate_frame(host, interactive=False)` must perform the engine's real Frame
identity/inventory check. It returns its owned result only on success, otherwise
`None`. A name, private address, CPU architecture or open SSH port alone cannot
establish that identity. Batch calls must disable credential prompts and have a
bounded timeout. All callbacks run inside `Presenter.suspend()`. When the engine
performs any additional OpenSSH or input operation, use that context as well.
Animation depends on output TTY; prompt capability depends independently on
input TTY or the explicit `input_enabled` value. German is selected from the OS
locale, including Windows `German_Germany`; other languages fall back to English.

Discovery first checks at most four cached addresses/names and three common
Frame names. DNS resolution runs in a bounded child, so missing physical-network
inventory does not prevent a successful known-name lookup. It then uses a single
physical, active private LAN with a default gateway: bounded mDNS queries,
existing neighbor records, and finally TCP/22 checks within the local `/24`.
There are at most 254 scan targets, 16 concurrent socket checks and eight batch
inventory validations. Public, loopback, link-local and detected virtual/VPN
networks are excluded. Unknown SSH hosts receive no automatic password prompts.
One cached/named hint may receive an interactive validation; manual private IPv4
entry is the final fallback. Blank input continues with the PC alone.

These are discovery hints, not a guarantee that every router/hostname setup can
be found. Automatic passwords, SSH/service/firewall changes, dependency installs,
persistence and Steam restarts do not exist in this module. The bootstrap owns
automatic Python/OpenSSH preparation; never direct the user to install modules.

Focused validation: `python -m unittest discover -s tests -p test_setup_ui.py`.
The tests use injected sources/connectors/validators and do not scan a real LAN.
They include real bounded Python child processes, a redirected pipe, a POSIX PTY,
DNS packet parsing and fixed Windows/Linux command-adapter vectors. Windows
PowerShell runtime behavior and actual Frame discovery require separate evidence.
