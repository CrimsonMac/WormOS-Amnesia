# Security policy

## Supported versions

Only the latest release gets security fixes. Installed systems receive
them with `wh update` and `wh upgrade`.

## Reporting a vulnerability

Please report security problems privately, through GitHub's private
vulnerability reporting ("Report a vulnerability" on the Security tab of
the WormOS repository), not in a public issue. Include the WormOS version
(`grep VERSION_ID /etc/os-release`), what you did and what happened; a
proof of concept helps. You should get an answer within a week, and a fix
in a release as soon as one is ready. Credit is given in the release notes
unless you prefer otherwise.

## What counts

WormOS's security features have stated limits (see the
[user guide](docs/guide.md)). Problems
inside these boundaries are vulnerabilities:

- **Blackholes:** a program inside reaching the host's files, processes,
  network, terminal, clipboard or snapshots, gaining privileges outside,
  or surviving `bh close` or `worm panic`.
- **Firewall:** inbound traffic reaching a port that is not open, or the
  network coming up without the firewall.
- **Anonymous mode:** any packet leaving the machine other than through
  Tor (and root's DHCP renewals), including DNS.
- **worm panic:** a running blackhole it does not stop, or traffic getting
  out while the network is cut.
- **Packages:** wh installing anything whose signature or checksum does not
  match, or writing outside the system it installs into.
- **Privilege escalation:** a user gaining root through `bh`, `whc`, the
  setuid helpers (`/usr/lib/worm/power`, `snapshots`, `install`,
  `install-system`) or a misconfiguration.
- **Snapshots:** a normal user changing or deleting snapshots.

## Known limits (not vulnerabilities on their own)

- Root on the same machine can do anything, including rewriting
  snapshots; protection against a compromised root needs isolation
  (blackholes) or another machine.
- Blackholes share the kernel with the host: a kernel exploit escapes
  them. A virtual machine mode is planned.
- Wormholes (software from Debian and Kali) share the user's home folder
  and the network; they are not a security boundary.
- Disks are not encrypted yet.
- Anonymous mode hides where traffic comes from, not who you are.
