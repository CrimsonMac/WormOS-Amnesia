#!/usr/bin/env python3
"""Boot the WormOS image in QEMU and check the running system.

Logs in on the serial console and runs a set of checks. It then takes a
snapshot, changes the system, rewinds (a soft reboot that keeps the kernel
running) and checks the change is gone, then reboots and picks a snapshot
from the "Rewind to a snapshot" boot menu. Last, it exercises worm
unlock/lock and the package manager against out/repo, served over HTTP
from this machine: install, a trial removal that rolls back, a removal and
its undo. Firewall checks reach a web server in the VM from this machine
through a QEMU port forward. The full console output is written to
build/boot.log.

Usage: tests/boot-test.py [--timeout SECONDS]
"""

import argparse
import functools
import http.server
import os
import re
import select
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "boot.log")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")

# (description, shell command, regular expression expected in the output)
CHECKS = [
    ("kernel", "uname -r", r"-wormos"),
    ("os-release", ". /etc/os-release; echo $NAME", r"WormOS"),
    ("init is worminit", "readlink /proc/1/exe", r"/worminit$"),
    ("booted through initramfs", "dmesg | grep -c 'Freeing initrd memory'", r"^1$"),
    ("root on @ subvolume", "grep ' / ' /proc/mounts", r"subvol=/@\s"),
    ("root is read-only", "grep ' / ' /proc/mounts", r"\sro[,\s]"),
    ("root cannot be written", "touch /probe 2>&1", r"Read-only file system"),
    ("var on @var subvolume", "grep ' /var ' /proc/mounts", r"\srw,.*subvol=/@var"),
    ("root home is writable", "touch ~/probe && grep -c ' /root ' /proc/mounts", r"^1$"),
    ("snapshots not mounted", "grep -c snapshots /proc/mounts", r"^0$"),
    ("home on @home subvolume", "grep ' /home ' /proc/mounts", r"subvol=/@home"),
    ("network configured", "ip -4 addr show eth0", r"inet 10\.0\.2\.15"),
    ("syslog running", "pidof syslogd", r"^\d+"),
    ("system bus runs unprivileged", "stat -c %U /proc/$(pidof dbus-daemon)",
     r"^messagebus$"),
    ("system bus answers",
     "dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus "
     "org.freedesktop.DBus.GetId", r'string "[0-9a-f]{32}"'),
    ("license texts installed",
     "ls /usr/share/licenses/wormos/COPYING /usr/share/licenses/glibc /usr/share/licenses/linux "
     "> /dev/null && echo n=$(ls /usr/share/licenses | wc -l)", r"^n=(1[0-9][0-9]|[5-9][0-9])$"),
    ("settings schemas compiled by the hook",
     "gsettings list-schemas | grep -c '^org.gtk.gtk4.Settings'", r"^[1-9]$"),
    ("file type database compiled by the hook",
     "stat -c %s /usr/share/mime/mime.cache; gio mime text/plain 2>&1 | head -1", r"^[1-9]\d+\n"),
    ("snapshot summary written at boot", "cat /run/snap-status", r"^count = 0$"),
    ("machine ID kept in /var", "readlink /etc/machine-id; cat /etc/machine-id",
     # Daemons' messages on the console can come in between.
     r"^\.\./var/lib/dbus/machine-id$(.|\n)*^[0-9a-f]{32}$"),
    ("hostname", "hostname", r"^wormos$"),
    # The resolver keeps its trust anchor current in /var/lib/unbound.
    ("system files owned by root",
     "find / /var -xdev ! -user 0 ! -path '/var/lib/unbound*' | wc -l", r"^0$"),
    ("shadow is private", "stat -c '%a %u' /etc/shadow", r"^600 0$"),
]

# Kernel security features (kernel/wormos.config), settings and mounts.
KERNEL_CHECKS = [
    ("hardening on the command line", "cat /proc/cmdline", r"slab_nomerge debugfs=off .*iommu\.strict=1"),
    ("io_uring off", "cat /proc/sys/kernel/io_uring_disabled", r"^2$"),
    ("kernel pointers hidden", "cat /proc/sys/kernel/kptr_restrict", r"^2$"),
    ("protected links and files",
     "cat /proc/sys/fs/protected_symlinks /proc/sys/fs/protected_regular | tr '\\n' ' '",
     r"^1 2\s*$"),
    ("no ICMP redirects", "cat /proc/sys/net/ipv4/conf/all/accept_redirects", r"^0$"),
    ("IPv6 privacy addresses", "cat /proc/sys/net/ipv6/conf/eth0/use_tempaddr", r"^2$"),
    ("processes of others hidden", "grep ' /proc ' /proc/mounts", r"hidepid=invisible"),
    ("no programs from /dev, /run or /dev/shm",
     "grep -E ' /(dev|run|dev/shm) ' /proc/mounts | grep -c noexec", r"^3$"),
    ("random MAC address", "cat /sys/class/net/eth0/address", r"^[0-9a-f][26ae]:"),
    ("not QEMU's MAC address", "cat /sys/class/net/eth0/address", r"^(?!52:54:00:12:34:56)"),
    ("lockdown in integrity mode", "cat /sys/kernel/security/lockdown", r"\[integrity\]"),
    ("security modules active", "cat /sys/kernel/security/lsm", r"lockdown.*yama.*landlock"),
    ("landlock running", "dmesg | grep -c 'landlock: Up and running'", r"^1$"),
    ("seccomp filters available", "cat /proc/sys/kernel/seccomp/actions_avail", r"kill_process"),
    ("module signatures enforced", "cat /sys/module/module/parameters/sig_enforce", r"^Y$"),
    ("no raw kernel memory access", "ls /dev/mem /dev/port /proc/kcore 2>&1 | grep -c 'No such'", r"^3$"),
    ("sysrq off", "cat /proc/sys/kernel/sysrq", r"^0$"),
    ("nftables built in",
     "grep -cE '^CONFIG_(NF_TABLES_INET|NFT_CT|NFT_NAT|NFT_MASQ|NFT_REDIR|NFT_FIB_INET)=y' /boot/config-$(uname -r)",
     r"^6$"),
    ("no iptables", "grep -c '^CONFIG_IP_NF_IPTABLES=' /boot/config-$(uname -r)", r"^0$"),
    ("sandbox filesystems", "grep -cwE 'overlay|fuse' /proc/filesystems", r"^2$"),
    ("vm and tunnel devices", "ls /dev/vhost-net /dev/vhost-vsock /dev/vsock /dev/net/tun | wc -l", r"^4$"),
]

# DNS: a local resolver with encrypted upstreams (needs no internet here).
DNS_CHECKS = [
    ("local resolver running", "pidof unbound", r"^\d+"),
    ("resolver runs unprivileged", "ps -o user,comm | grep -w unbound", r"^unbound\s+unbound"),
    ("resolver configuration valid", "unbound-checkconf 2>&1", r"no errors"),
    ("programs use the local resolver", "cat /etc/resolv.conf", r"^nameserver 127\.0\.0\.1$"),
    ("network's DNS servers only recorded",
     "cat /run/worm/dhcp-dns; grep -c 10.0.2.3 /etc/resolv.conf", r"^nameserver 10\.0\.2\.3\n0$"),
    ("resolver answers", "nslookup localhost 127.0.0.1 2>&1", r"Address:?\s+127\.0\.0\.1"),
    ("trust anchor kept up to date", "ls -l /var/lib/unbound/root.key", r"\sunbound\s+unbound\s"),
]

# The firewall. A web server in the VM on port 8080 is fetched from this
# machine through a QEMU port forward between these lists.
FIREWALL_CHECKS = [
    ("firewall active at boot", "worm fw status", r"^Firewall: active"),
    ("no ports open by default", "worm fw status", r"^Open ports: none$"),
    ("inbound and forwarding denied",
     "nft list table inet worm | grep -c 'policy drop'", r"^2$"),
    ("loaded before the network", "ls /etc/worm/services | grep -E 'firewall|network' | tr '\\n' ' '",
     r"^05-firewall 30-network\s*$"),
    ("pings get no answer", "nft list chain inet worm input | grep -c echo-request", r"^0$"),
    ("network waits for the firewall", "grep -c 'nft list chain inet worm input' /usr/lib/worm/network",
     r"^1$"),
    ("dhcp names no client", "cat /proc/$(pidof udhcpc)/cmdline | tr '\\0' ' '", r"-V\s+-s"),
    ("web server started",
     "mkdir -p /tmp/www && echo hi > /tmp/www/index.html && httpd -p 8080 -h /tmp/www && echo ok",
     r"^ok$"),
]

FIREWALL_OPEN_CHECKS = [
    ("worm fw allow", "worm fw allow 8080", r"^Opened 8080/tcp\.$"),
]

FIREWALL_CLOSE_CHECKS = [
    ("port listed", "worm fw status", r"^\s+8080/tcp$"),
    ("port saved", "cat /etc/worm/firewall", r"^tcp 8080$"),
    ("root still read-only", "touch /probe 2>&1", r"Read-only file system"),
    ("allow is idempotent", "worm fw allow 8080/tcp", r"already open"),
    ("invalid port refused", "worm fw allow 70000 2>&1; echo rc=$?", r"invalid port(.|\n)*rc=1"),
    ("worm fw deny", "worm fw deny 8080", r"^Closed 8080/tcp\.$"),
]

FIREWALL_AFTER_CHECKS = [
    ("drops counted", "worm fw status", r"^Dropped since loaded: [1-9]"),
    ("bad port list loads closed",
     "mount -o remount,bind,rw / && echo 'tcp banana' >> /etc/worm/firewall && "
     "worm fw load 2>&1; echo rc=$?; worm fw status",
     r"invalid line(.|\n)*rc=1(.|\n)*Firewall: active(.|\n)*Open ports: none"),
    ("bad line removed",
     "sed -i '/banana/d' /etc/worm/firewall && mount -o remount,bind,ro / && worm fw load && echo ok",
     r"^ok$"),
    ("udp port kept for later", "worm fw allow 5353/udp", r"^Opened 5353/udp\.$"),
]

# Blackholes, mostly escape attempts from inside one. "bh open t1 -- CMD"
# runs one command in the blackhole t1; echo n=... gives a positive marker.
BLACKHOLE_CHECKS = [
    ("bh put", "echo sample > /root/sample && echo secret > /home/secret && bh put t1 /root/sample",
     r"^/root/sample -> t1:/inbox/sample$"),
    ("file in the inbox", "bh open t1 -- cat /inbox/sample", r"^sample$"),
    ("root inside", "bh open t1 -- id -u", r"^0$"),
    ("unprivileged outside", "bh open t1 -- cat /proc/self/uid_map", r"^\s*0\s+1073741825\s+1$"),
    ("own host name", "bh open t1 -- hostname", r"^t1$"),
    ("host home hidden", "bh open t1 -- sh -c 'echo n=$(ls -A /home | wc -l)'", r"^n=0$"),
    ("root's home hidden", "bh open t1 -- ls /root/probe 2>&1", r"No such file"),
    ("no disks", "bh open t1 -- sh -c 'echo n=$(ls /dev | grep -c vd)'", r"^n=0$"),
    ("cannot mount the snapshots",
     "bh open t1 -- mount -t btrfs -o subvol=@snapshots /dev/vda2 /tmp 2>&1",
     r"permission denied|not permitted"),
    ("only loopback", "bh open t1 -- sh -c 'echo n=$(ip -o link | wc -l)'", r"^n=1$"),
    ("no network", "bh open t1 -- busybox wget -q -T 5 -O- http://10.0.2.2:1/ 2>&1", r"unreachable"),
    ("host processes hidden",
     "bh open t1 -- sh -c 'echo n=$(ls /proc | grep -c \"^[0-9]\")'", r"^n=[1-6]$"),
    ("cannot signal host processes", "bh open t1 -- kill -0 $(pidof syslogd) 2>&1",
     r"No such process"),
    ("host files read-only",
     "bh open t1 -- sh -c 'echo x >> /etc/passwd; touch /usr/bin/x' 2>&1",
     r"Permission denied(.|\n)*(Permission denied|Read-only file system)"),
    ("writes stay inside", "bh open t1 -- sh -c 'echo x > /escape && echo ok'; ls /escape 2>&1",
     r"^ok$(.|\n)*No such file"),
    ("no message bus", "bh open t1 -- ls /run/dbus 2>&1", r"No such file"),
    ("no sysrq", "bh open t1 -- sh -c 'echo b > /proc/sysrq-trigger' 2>&1", r"Permission denied"),
    ("no kernel log", "bh open t1 -- dmesg 2>&1", r"not permitted"),
    ("sandboxed", "bh open t1 -- grep -E 'NoNewPrivs|Seccomp:' /proc/self/status",
     r"NoNewPrivs:\s+1(.|\n)*Seccomp:\s+2"),
    ("no nested namespaces", "bh open t1 -- unshare -U true 2>&1", r"not permitted"),
    ("own terminal", "bh open t1 -- tty", r"^/dev/pts/0\s*$"),
    ("only harmless capabilities", "bh open t1 -- grep -E 'Cap(Bnd|Eff)' /proc/self/status",
     r"CapEff:\s+00000000000004fb(.|\n)*CapBnd:\s+00000000000004fb|"
     r"CapBnd:\s+00000000000004fb(.|\n)*CapEff:\s+00000000000004fb"),
    ("cannot configure its network", "bh open t1 -- ip link set lo down 2>&1",
     r"not permitted"),
    ("no vsock",
     "bh open t1 -- python3 -c 'import socket; socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)' 2>&1",
     r"Address family not supported"),
    ("no packet sockets",
     "bh open t1 -- python3 -c 'import socket; socket.socket(socket.AF_PACKET, socket.SOCK_RAW)' 2>&1",
     r"Address family not supported"),
    ("no netfilter",
     "bh open t1 -- python3 -c 'import socket; socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 12)' 2>&1",
     r"Protocol not supported"),
    ("process limit", "bh open t1 -- grep 'Max processes' /proc/self/limits", r"\s4096\s+4096\s"),
    ("thousands of threads",
     "bh open t1 -- python3 -c 'import threading as t; e=t.Event(); "
     "[t.Thread(target=e.wait, daemon=True).start() for i in range(2000)]; print(\"n=\", t.active_count())'",
     r"^n= 2001$"),
    ("fork bomb stays inside",
     "bh open --pids 300 -- python3 -c 'import os, signal\nn=0\ntry:\n while n<1000:\n  if os.fork()==0:\n   signal.pause()\n  n+=1\n"
     "except OSError as e: print(\"n=%d errno=%d\" % (n, e.errno))'",
     r"^n=29\d errno=11$"),
    ("clipboard sequences dropped",
     "bh open t1 -- printf 'a\\033]52;c;ZXZpbA==\\007b\\n' | od -An -c | tr -s ' '",
     r"^\s*a b \\n\s*$"),
    ("notification and link sequences dropped",
     "bh open t1 -- printf 'a\\033]777;notify;x;y\\007b\\033]8;;http://x\\033\\\\c\\n' | od -An -c | tr -s ' '",
     r"^\s*a b c \\n\s*$"),
    ("other sequences kept", "bh open t1 -- printf '\\033]0;t\\007' | od -An -c | tr -s ' '",
     r"^\s*033 \] 0 ; t \\a\s*$"),
    ("no inherited descriptors",
     "exec 7</home; bh open t1 -- sh -c 'echo fds=$(ls /proc/self/fd | tr \"\\n\" \" \")'; exec 7<&-",
     r"^fds=0 1 2 3\s*$"),
    ("files kept between sessions",
     "bh open t1 -- sh -c 'echo kept > /root/note'; bh open t1 -- cat /root/note", r"^kept$"),
    ("running with limits",
     "bh open t1 -- sleep 60 & sleep 5; cd /sys/fs/cgroup/blackholes/0-t1 && "
     "cat pids.max cpu.weight && echo m=$(( $(cat memory.max) * 2 / 1024 / $(sed -n 's/MemTotal: *//p' /proc/meminfo | cut -d' ' -f1) )); cd; bh list",
     r"^4096\n50\nm=1$(.|\n)*^t1\s+running"),
    ("a second session joins it",
     "bh open t1 -- sh -c 'echo n=$(pgrep sleep | wc -l) $(hostname) $(cat /root/note)'",
     r"^n=1 t1 kept$"),
    ("a joined session gets its own terminal", "bh open t1 -- tty", r"^/dev/pts/\d+\s*$"),
    ("options only when it starts", "bh open --net t1 -- true 2>&1", r"already running"),
    ("joined sessions end with the first",
     "bh open t1 -- sleep 600 & sleep 2; kill %1; wait; bh list | grep -c 't1.*running'",
     r"^0$"),
    ("root's blackholes closed to users", "stat -c '%a %U' /var/lib/blackholes/0/t1",
     r"^710 root$"),
    ("bh close", "bh close t1", r"^Closed blackhole t1\.$"),
    ("everything removed",
     "sleep 1; echo n=$(ls /var/lib/blackholes/0 /sys/fs/cgroup/blackholes | grep -c t1)",
     r"^n=0$"),
    ("temporary blackhole removed",
     "bh open -- touch /x && echo n=$(ls /var/lib/blackholes/0 | wc -l)", r"^n=0$"),
    ("bh list", "bh list", r"^No blackholes"),
    ("add a normal user",
     "mount -o remount,bind,rw / && adduser -D -H tester && mount -o remount,bind,ro / && echo ok",
     r"^ok$"),
    ("user's blackhole", "su tester -c 'bh open u1 -- cat /proc/self/uid_map'",
     r"^\s*0\s+[1-9]\d*\s+1$"),
    ("user's blackhole is private", "ls -ld /var/lib/blackholes/$(id -u tester)",
     r"^drwx------\s+\d+\s+tester\s"),
    ("user's blackhole cannot reach root's",
     "bh open t9 -- true; su tester -c 'ls /var/lib/blackholes/0/t9' 2>&1; bh close t9",
     r"Permission denied"),
    ("user's blackhole has limits too",
     "su tester -c 'bh open u1 -- sleep 30' & sleep 4; "
     "cat /sys/fs/cgroup/blackholes/$(id -u tester)-u1/pids.max", r"^4096$"),
    ("limits helper is setuid root", "stat -c '%U %a' /usr/lib/bh/limits", r"^root 4755$"),
    ("limits helper refuses others' processes",
     "su tester -c '/usr/lib/bh/limits create x 1 100000000 10' 2>&1", r"not yours"),
    ("user's limits removed",
     "wait; sleep 1; ls /sys/fs/cgroup/blackholes | grep -c -- '-u1$'", r"^0$"),
    ("user closes it", "su tester -c 'bh close u1'", r"^Closed blackhole u1\.$"),
    ("power helper is setuid root", "stat -c '%U %a' /usr/lib/worm/power", r"^root 4755$"),
    ("power helper only for administrators",
     "su tester -c '/usr/lib/worm/power reboot'; echo rc=$?",
     r"only administrators(.|\n)*^rc=1$"),
]

# whc without network access: a tiny wormhole made of busybox and its
# libraries. make test-wormholes covers real Debian and Kali ones.
# Blackholes with a network (pasta) and virtual machine blackholes (KVM,
# nested in the test VM). {repo} is this machine's web server at 10.0.2.2,
# a private address: reachable only with --lan.
BH_NET_CHECKS = [
    ("network interface with --net", "bh open --net -- ip -4 -o addr show eth0",
     r"inet 100\.96\.0\.2/29"),
    ("names through the host's resolver",
     "bh open --net -- nslookup -type=a localhost 2>&1", r"127\.0\.0\.1"),
    ("local network kept out", "bh open --net -- busybox wget -q -T 5 -O- {repo}index 2>&1; echo rc=$?",
     r"rc=[1-9]"),
    ("local network with --lan",
     "bh open --net --lan -- busybox wget -q -T 10 -O- {repo}index | head -c 0; echo rc=$?", r"^rc=0$"),
    ("host kept out even with --lan",
     "bh open --net --lan -- busybox wget -q -T 5 -O- http://10.0.2.15:9/ 2>&1; echo rc=$?",
     r"rc=[1-9]"),
    ("network still cannot be reconfigured", "bh open --net -- ip route del default 2>&1",
     r"not permitted"),
    ("pasta gone afterwards", "sleep 1; pidof passt pasta passt.avx2; echo rc=$?", r"^rc=1$"),
    ("virtual machine boots its own kernel",
     "bh open --vm v1 -- sh -c 'grep -o bh.name=v1 /proc/cmdline; echo n=$(ls /proc | grep -c \"^[0-9]\")'",
     r"bh\.name=v1(.|\n)*n=\d+"),
    ("guest sees the blackhole's files",
     "bh put v1 /root/sample >/dev/null; bh open --vm v1 -- sh -c 'cat /inbox/sample; echo vm > /root/from-vm'; "
     "bh open v1 -- cat /root/from-vm", r"^sample\s*$(.|\n)*^vm$"),
    ("guest cannot write the system", "bh open --vm v1 -- touch /usr/bin/x 2>&1",
     r"Read-only|Permission denied"),
    ("exit code comes back", "bh open --vm v1 -- sh -c 'exit 7'; echo rc=$?", r"rc=7"),
    ("guest network with --net --lan",
     "bh open --vm --net --lan v1 -- busybox wget -q -T 20 -O- {repo}index | head -c 0; echo rc=$?",
     r"rc=0"),
    ("monitor jailed and limited",
     "bh open --vm v1 -- sleep 30 & for i in $(seq 20); do p=$(pidof lkvm) && break; sleep 1; done; "
     "grep -E '^(NoNewPrivs|Seccomp):' /proc/$p/status; "
     "cat /proc/$p/uid_map; cat /sys/fs/cgroup/blackholes/0-v1/memory.max; wait",
     r"NoNewPrivs:\s+1(.|\n)*Seccomp:\s+2(.|\n)*0\s+1073741\d+\s+1(.|\n)*^\d+$"),
    ("virtual machine closed", "bh close v1", r"^Closed blackhole v1\.$"),
]

WHC_CHECKS = [
    ("small wormhole",
     "W=/var/lib/wormholes/mini && mkdir -p $W/usr/bin $W/usr/lib $W/etc $W/tmp && "
     "cp /usr/bin/busybox $W/usr/bin/ && "
     "cp /usr/lib/ld-linux-x86-64.so.2 /usr/lib/libc.so.6 /usr/lib/libm.so.6 "
     "/usr/lib/libresolv.so.2 $W/usr/lib/ && "
     "ln -s usr/bin $W/bin && ln -s usr/lib $W/lib && ln -s usr/lib $W/lib64 && echo ok",
     r"^ok$"),
    ("whc as root", "touch /var/lib/wormholes/mini/inside && whc mini busybox ls /inside",
     r"^/inside$"),
    ("whc as a user keeps their ID",
     "su tester -c 'whc mini busybox cat /proc/self/uid_map'",
     r"^\s*([1-9]\d*)\s+\1\s+1$"),
    ("whc user cannot change the wormhole",
     "su tester -c 'whc mini busybox touch /usr/x' 2>&1", r"Permission denied|Read-only"),
    ("whc shares the host's /tmp",
     "echo shared > /tmp/whc-probe && su tester -c 'whc mini busybox cat /tmp/whc-probe'",
     r"^shared$"),
    ("wormhole removed", "rm -rf /var/lib/wormholes/mini && echo ok", r"^ok$"),
]

TIME_CHECKS = [
    ("UTC by default", "worm time", r"^Time zone: UTC$"),
    ("time zone set", "worm time zone Europe/Berlin && date +%Z", r"^CES?T$"),
    ("time zone is a link into the zone data", "readlink /etc/localtime",
     r"^/usr/share/zoneinfo/Europe/Berlin$"),
    ("unknown time zone refused", "worm time zone ../../etc/passwd 2>&1; echo rc=$?",
     r"unknown time zone(.|\n)*rc=1"),
    ("back to UTC", "worm time zone UTC && date +%Z && ls /etc/localtime 2>&1", r"^UTC\n.*No such file"),
    ("clock checked over HTTPS", "cat /etc/worm/services/35-time | grep -c timesync; pidof timesync || "
     "ps -o args | grep -c '[t]imesync'", r"^1\n[1-9]"),
]

PASSWD_CHECKS = [
    ("a user password", "mount -o remount,bind,rw / && echo tester:first1 | chpasswd -c sha512 "
     "> /dev/null && mount -o remount,bind,ro / && echo ok", r"^ok$"),
    ("wrong current password refused",
     "su tester -c 'printf \"nope\\nsecond2\\n\" | /usr/lib/worm/passwd' 2>&1; echo rc=$?",
     r"current password is wrong(.|\n)*rc=3"),
    ("user changes their password",
     "H=$(grep '^tester:' /etc/shadow); su tester -c 'printf \"first1\\nsecond2\\n\" | "
     "/usr/lib/worm/passwd' && [ \"$(grep '^tester:' /etc/shadow)\" != \"$H\" ] && echo changed",
     r"^changed$"),
    ("new password works", "su tester -c 'printf \"second2\\nthird3\\n\" | /usr/lib/worm/passwd' "
     "&& echo ok", r"^ok$"),
    ("root stays read-only", "touch /probe 2>&1", r"Read-only file system"),
    ("only one's own account", "su tester -c '/usr/bin/worm passwd-apply root < /dev/null' 2>&1",
     r"needs root"),
]

KIT_CHECKS = [
    ("tool kits listed", "worm kit list",
     r"^basic(.|\n)*^forensics(.|\n)*^pentest(.|\n)*^web(.|\n)*^windows"),
    ("basic kit marks core packages", "worm kit show basic",
     r"WormOS packages(.|\n)* coreutils (.|\n)* 7zip\*(.|\n)*Tools \(from Debian(.|\n)*bind9-dnsutils"),
    ("windows kit points at VM blackholes", "worm kit show windows", r"bh open --vm(.|\n)*wine"),
    ("kit contents", "worm kit show forensics", r"sleuthkit(.|\n)*Not installed"),
    ("unknown kit refused", "worm kit show nothing 2>&1; echo rc=$?", r"no kit(.|\n)*rc=1"),
    ("users cannot install kits", "su tester -c 'worm kit install web' 2>&1", r"needs root"),
    ("console greeting up to date", "grep -c \"$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release "
     "| tr -d '\\\"')\" /etc/issue", r"^1$"),
]

STATUS_CHECKS = [
    ("worm status", "worm status",
     r"System\s+read-only(.|\n)*Mode\s+normal(.|\n)*Packages\s+no change waiting(.|\n)*"
     r"Firewall\s+active, 1 open port(.|\n)*DNS\s+encrypted(.|\n)*Blackholes\s+0 running"
     r"(.|\n)*Kernel\s+lockdown integrity(.|\n)*io_uring off"),
]

# Anonymous mode (worm anon). Tor cannot connect here; what matters is
# that nothing leaves the machine directly. {repo} is the repository URL.
ANON_CHECKS = [
    ("tor installed", "tor --version | head -1", r"^Tor version 0\.4\.9"),
    ("worm anon on", "worm anon on", r"^Anonymous mode is on"),
    ("tor runs unprivileged", "sleep 3; ps -o user,comm | grep -w tor", r"^tor\s+tor"),
    ("connections sent to Tor", "nft list chain inet worm anon_nat",
     r"udp dport 53 redirect to :5353(.|\n)*l4proto (tcp|6) redirect to :9040"),
    ("only what was sent to Tor reaches it", "nft list chain inet worm output",
     r"ct status dnat ip daddr 127\.0\.0\.1 accept(.|\n)*meta skuid \d+ meta l4proto (tcp|6) accept"
     r"(.|\n)*meta skuid 0 udp sport 68 udp dport 67 accept"),
    ("everything else blocked", "ping -c 1 -W 3 10.0.2.2 2>&1", r"not permitted|100% packet loss"),
    ("direct request attempted", "busybox wget -q -T 10 -O- {repo}anon-leak-check 2>&1; echo done",
     r"done$"),
    ("connections reach Tor", "busybox wget -q -T 10 -O- http://198.51.100.1/ 2>&1; echo rc=$?",
     r"\A(?![\s\S]*refused)[\s\S]*rc="),
    ("status shows it", "worm status", r"DNS\s+through Tor(.|\n)*Anonymity\s+on"),
    ("survives a firewall reload", "worm fw load && nft list chain inet worm anon_nat | grep -c 9040",
     r"^1$"),
    ("bridges through Tor Browser's transports",
     "worm anon bridges snowflake | head -1; grep -c '^Bridge snowflake' /etc/tor/torrc.d/bridges.conf; "
     "nft list chain inet worm output | grep -c 'l4proto udp accept'; "
     "tor -f /etc/tor/torrc --verify-config | tail -1",
     r"snowflake bridges?\.\n[1-9]\n1\nConfiguration was valid"),
    ("bridges off", "worm anon bridges off | head -1; ls /etc/tor/torrc.d | wc -l; "
     "nft list chain inet worm output | grep -c 'l4proto udp accept'",
     r"^Tor connects directly again(.|\n)*^0\n0$"),
    ("worm anon off", "worm anon off", r"^Anonymous mode is off"),
    ("tor stopped", "sleep 2; pidof tor || echo none", r"^none$"),
    ("direct connections again", "busybox wget -q -T 10 -O- {repo}anon-off-check 2>&1; echo done",
     r"done$"),
]

# worm panic, at the end: its snapshot would change the IDs checked above.
# {repo} is the repository URL.
PANIC_CHECKS = [
    ("a blackhole running", "bh open p1 -- sleep 600 & sleep 5; bh list", r"^p1\s+running"),
    ("worm panic", "worm panic",
     r"Network cut(.|\n)*Stopped 1 blackhole(.|\n)*snapshot \d+ \(pinned"),
    ("blackhole stopped, files kept", "sleep 1; bh list", r"^p1\s+idle"),
    ("devices down", "ip -o link show eth0", r"^\d+: eth0: <(?![^>]*\bUP\b)"),
    ("nothing gets out", "busybox wget -q -T 5 -O- {repo}index 2>&1 | head -1", r"\S"),
    ("panic snapshot pinned", "snap list", r"^\s*\d+\*.*all\s+manual\s+Panic at"),
    ("firewall refuses changes", "worm fw allow 22 2>&1", r"network is cut"),
    ("status shows it", "worm status", r"Firewall\s+network cut"),
    ("users see that the network is cut", "su tester -c 'test -e /run/worm/network-cut && echo cut'",
     r"^cut$"),
    ("worm panic off", "worm panic off", r"network is back"),
    ("network works again", "sleep 5; wh --repo {repo} update",
     r"^Package index updated: \d+ packages$"),
]

# After booting the menu's safe mode entry.
SAFE_CHECKS = [
    ("safe mode", "worm status", r"Mode\s+SAFE MODE"),
    ("network not started", "pidof udhcpc || echo none", r"^none$"),
    ("network cut", "worm fw status", r"network is cut"),
    ("stays cut until a normal restart", "worm panic off 2>&1", r"restart normally"),
]

# The helper behind the Snapshots program (run last: it adds snapshots).
# Root counts as an administrator; the development image's root password
# is empty, so an empty line is the right password.
H = "/usr/lib/worm/snapshots"
LAST = f"{H} list | tail -n 1"
I = "/usr/lib/worm/install"
HELPER_CHECKS = [
    ("snapshot helper is setuid root", f"stat -c '%U %a' {H}", r"^root 4755$"),
    ("only administrators use it", f"su tester -c '{H} list'; echo rc=$?",
     r"only administrators(.|\n)*^rc=1$"),
    ("lists snapshots for programs", f"{LAST} | awk -F '\\t' '{{ print NF }}'", r"^7$"),
    ("takes a snapshot", f"{H} now system 'Helper test' >/dev/null && {LAST} | cut -f 3,7",
     r"^system\tHelper test$"),
    ("sets a comment", f"{H} comment $({LAST} | cut -f 1) Renamed >/dev/null && "
     f"{LAST} | cut -f 7", r"^Renamed$"),
    ("pins without a password", f"{H} pin $({LAST} | cut -f 1) >/dev/null && {LAST} | cut -f 5",
     r"^1$"),
    ("unpinning needs the password",
     f"echo wrong | {H} unpin $({LAST} | cut -f 1); echo rc=$?; {LAST} | cut -f 5",
     r"wrong password(.|\n)*^rc=3\n1$"),
    ("unpins with the password", f"echo | {H} unpin $({LAST} | cut -f 1) >/dev/null && "
     f"{LAST} | cut -f 5", r"^0$"),
    ("refuses control characters", f"{H} now system \"$(printf 'a\\tb')\"; echo rc=$?",
     r"control characters(.|\n)*^rc=2$"),
    ("refuses paths with ..", f"echo | {H} restore 1 /etc/../etc/shadow; echo rc=$?",
     r"without \.\.(.|\n)*^rc=2$"),
    ("deleting needs the password", f"echo nope | {H} delete $({LAST} | cut -f 1); "
     f"{LAST} | cut -f 7", r"wrong password(.|\n)*^Renamed$"),
    ("deletes with the password", f"echo | {H} delete $({LAST} | cut -f 1) && "
     f"{H} list | grep -c Renamed", r"^Deleted snapshot \d+\n0$"),
    ("install helper is setuid root", f"stat -c '%U %a' {I}", r"^root 4755$"),
    ("install helper only for administrators",
     f"su tester -c 'echo | {I} tor-browser-runtime'; echo rc=$?",
     r"only administrators(.|\n)*^rc=1$"),
    ("install helper refuses unlisted packages", f"echo | {I} tcpdump; echo rc=$?",
     r"cannot be installed from the desktop(.|\n)*^rc=2$"),
    ("install helper refuses odd names", f"echo | {I} ../tcpdump; echo rc=$?",
     r"not a package name(.|\n)*^rc=2$"),
    ("install helper needs the password", f"echo wrong | {I} tor-browser-runtime; echo rc=$?",
     r"wrong password(.|\n)*^rc=3$"),
    ("Tor Browser key shipped",
     "gpg --homedir /tmp --with-colons --show-keys "
     "/usr/share/wormos/tor-browser/tor-browser-developers.gpg 2>/dev/null | grep -m1 ^fpr",
     r"EF6E286DDA85EA2A4BA7DE684E2C6E8793298290"),
    ("Tor Browser launcher refuses a bad signature",
     "tor-browser --check /etc/os-release /etc/hostname; echo rc=$?",
     r"does not match(.|\n)*^rc=1$"),
    ("Tor Browser comes with the image", "cat /opt/tor-browser/version", r"^\d+\.\d+"),
    ("Tor Browser bundle usable by users",
     "su tester -c 'test -x /opt/tor-browser/Browser/firefox.real && echo yes'", r"^yes$"),
    ("Tor Browser finds all its libraries",
     "LD_LIBRARY_PATH=/opt/tor-browser/Browser ldd /opt/tor-browser/Browser/libxul.so "
     "| grep -c 'not found'", r"^0$"),
    ("Tor Browser does not run as root",
     "tor-browser </dev/null; echo rc=$?", r"does not run as root(.|\n)*^rc=1$"),
    ("Tor Browser needs a desktop",
     "su tester -c 'env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR=/nonexistent tor-browser'; echo rc=$?",
     r"no desktop session(.|\n)*^rc=1$"),
]

# The command-line tools that come with the image.
TOOL_CHECKS = [
    ("worm help lists the commands", "worm help | grep -c -e 'snap now' -e 'wh install' "
     "-e 'bh open' -e 'worm fw' -e tor-browser", r"^([5-9]|1[0-9])$"),
    ("worm help works for users", "su tester -c 'worm help snap' | head -1",
     r"^usage: snap"),
    ("bash", "bash -c 'echo ${BASH_VERSINFO[0]}'", r"^5$"),
    ("less", "less --version | head -1", r"^less \d+"),
    ("nano", "nano --version | head -1", r"GNU nano"),
    ("vim and vi", "vim --version | head -1; readlink /usr/bin/vi", r"VIM - Vi IMproved(.|\n)*^vim$"),
    ("htop", "htop --version", r"^htop 3"),
    ("tmux", "tmux -V", r"^tmux 3"),
    ("git", "cd /tmp && git init -q t && cd t && git -c user.name=a -c user.email=a@b "
     "commit -q --allow-empty -m x && git log --oneline | wc -l", r"^1$"),
    ("ssh", "ssh -V 2>&1", r"^OpenSSH_10"),
    ("rsync", "rsync --version | head -1", r"^rsync\s+version 3"),
    ("tar is bsdtar with xz, zstd and zip",
     "cd /tmp && echo hi > f && tar -cJf f.txz f && tar -a -cf f.zip f && "
     "tar --zstd -cf f.tzst f && rm f && unzip -o -q f.zip && cat f && tar --version | head -1",
     r"^hi\nbsdtar 3"),
    ("xz and bzip2", "echo ok | xz | xz -d; echo ok | bzip2 | bzcat", r"^ok\nok$"),
    ("file", "file -b /usr/bin/bash", r"ELF 64-bit LSB pie executable"),
    ("tree", "tree -d /etc/zsh | tail -1", r"directories"),
    ("ncdu", "ncdu --version", r"^ncdu 1"),
    ("jq", "echo '{\"a\":[1,2]}' | jq -c '.a | map(. * 2)'", r"^\[2,4\]$"),
    ("lsof", "lsof -p 1 | head -1", r"^COMMAND"),
    ("strace", "strace -f -e trace=openat -o /dev/null true && echo ok", r"^ok$"),
    ("wg", "wg --version", r"^wireguard-tools"),
    ("sqlite3", "sqlite3 -list :memory: 'select 6*7'", r"^42$"),
    ("python with its modules", "python3 -c 'import ssl, sqlite3, ctypes, lzma, bz2, zlib, "
     "readline, curses, json, venv; print(ssl.OPENSSL_VERSION.split()[0])'", r"^OpenSSL$"),
    ("python makes a venv with pip", "python3 -m venv /tmp/venv && /tmp/venv/bin/pip --version",
     r"^pip \d+"),
    ("nmap", "nmap --version | head -1", r"^Nmap version 7"),
    ("ncat", "ncat --version 2>&1", r"Ncat: Version 7"),
    ("tcpdump", "tcpdump --version 2>&1 | head -1", r"^tcpdump version"),
    ("binutils", "readelf -h /usr/bin/bash | grep -c Machine; strings /usr/bin/bash | grep -c GNU",
     r"^1\n[1-9]"),
    ("GNU grep, sed, awk, find and patch",
     "echo a12b | grep -oP '\\d+'; sed --version | head -1; awk --version | head -1; "
     "find --version | head -1; patch --version | head -1",
     r"^12\n.*GNU sed.*\nGNU Awk 5.*\nfind \(GNU findutils\).*\nGNU patch"),
    ("GNU grep took the busybox link over",
     "test -L /usr/bin/grep || echo real; grep -c ' usr/bin/grep$' /usr/lib/wh/db/busybox/files; "
     "grep -c '^usr/bin/grep$' /usr/share/busybox/links", r"^real\n0\n1$"),
    ("7-Zip", "cd /tmp && echo seven > s7 && 7z a -bd -bso0 s7.7z s7 && rm s7 && "
     "7z x -bd -bso0 s7.7z && cat s7", r"^seven$"),
    ("zip", "cd /tmp && echo z > z1 && zip -q z.zip z1 && unzip -l z.zip | grep -c z1", r"^1$"),
    ("wget", "wget --version | head -1", r"^GNU Wget 1\.25"),
    ("iproute2", "ip -br link show lo; ss -ltn | head -1", r"^lo\s+UNKNOWN(.|\n)*State"),
    ("lspci names devices", "lspci | grep -ci 'ethernet controller'", r"^[1-9]"),
    ("lsusb", "lsusb --version", r"usbutils"),
    ("mkfs.fat", "dd if=/dev/zero of=/tmp/fat.img bs=1M count=8 2>/dev/null && "
     "mkfs.fat /tmp/fat.img > /dev/null && fsck.fat -n /tmp/fat.img | tail -1", r"files"),
    ("whois, traceroute, ethtool", "whois --version | head -1; traceroute --version; "
     "ethtool --version", r"^Version 5\.6(.|\n)*traceroute.*2\.1(.|\n)*ethtool version 7"),
    ("the standard edition's package", "wh list | grep -c '^wormos-standard '", r"^1$"),
    ("btop, mc, fastfetch, gdb", "btop --version | head -1; mc --version | head -1; "
     "fastfetch --version; gdb --version | head -1",
     r"^btop version 1\.4(.|\n)*GNU Midnight Commander 4\.8(.|\n)*fastfetch 2\.(.|\n)*GNU gdb"),
    ("fastfetch shows the WormOS logo", "fastfetch --pipe false -s os | grep -c -e @ -e WormOS", r"^[2-9]$"),
    ("ext4 and exFAT", "cd /tmp && dd if=/dev/zero of=e.img bs=1M count=16 2>/dev/null && "
     "mkfs.ext4 -q -F e.img && e2fsck -fn e.img | tail -1 && mkfs.exfat e.img > /dev/null && "
     "fsck.exfat -n e.img | tail -1",
     r"e\.img: \d+/\d+ files(.|\n)*clean"),
    ("yara finds a pattern", "cd /tmp && printf 'rule w { strings: $a = \"wormhole\" condition: $a }' > w.yar "
     "&& echo 'a wormhole here' > w.txt && yara w.yar w.txt", r"^w w\.txt$"),
    ("ssdeep", "ssdeep -s /usr/bin/bash | tail -1", r"^\d+:\S+:\S+,\"/usr/bin/bash\"$"),
    ("lz4, pigz and pv", "echo ok | lz4 -q | lz4 -dq; echo ok | pigz | pv -q | unpigz", r"^ok\nok$"),
    ("disk and firmware tools", "smartctl --version | head -1; hdparm -V; dmidecode -s system-manufacturer",
     r"^smartctl 7\.5(.|\n)*hdparm v9\.65(.|\n)*QEMU"),
    ("busybox links taken over cleanly", "readlink /usr/bin/tar; test -L /usr/bin/less || "
     "echo real; grep -c -e ' usr/bin/less$' -e ' usr/bin/tar$' /usr/lib/wh/db/busybox/files",
     r"^bsdtar\nreal\n0$"),
]

# Snapshot, change the system, and look at the change.
SNAPSHOT_CHECKS = [
    ("snap now", "snap now -c 'test base'", r"^Snapshot 1 created \(system\)$"),
    ("store stays hidden", "grep -c snapshots /proc/mounts", r"^0$"),
    ("change the system",
     "mount -o remount,bind,rw / && echo x > /etc/probe && mount -o remount,bind,ro / && echo ok",
     r"^ok$"),
    ("snap diff", "snap diff 1", r"^\+ /etc/probe$"),
    ("snap list", "snap list", r"^\s+1\s.*system\s+manual\s+test base$"),
    ("snap verify", "snap verify", r"^snapshot 1 \(system\): ok$"),
    ("store stays hidden after use", "grep -c snapshots /proc/mounts", r"^0$"),
    ("summary readable by everyone",
     "stat -c %a /run/snap-status; grep -E '^(count|last|last_comment) ' /run/snap-status",
     r"^644\ncount = 1\nlast = 1\nlast_comment = test base$"),
]

# Inside "snap browse 1", a read-only shell in the snapshot.
BROWSE_CHECKS = [
    ("browse shows the snapshot", "pwd; ls /run/snap/view/etc/probe 2>&1", r"No such file"),
    ("browse is read-only", "touch /run/snap/view/x 2>&1", r"Read-only file system"),
    ("browse hides the store", "ls /run/snap/top 2>&1 | wc -l", r"^0$"),
]

# After "snap rewind" and the soft reboot it causes.
REWIND_CHECKS = [
    ("services restarted", "pidof syslogd && grep -c ' /var ' /proc/mounts", r"^1$"),
    ("change is gone", "ls /etc/probe 2>&1", r"No such file"),
    ("firewall reloaded", "worm fw status", r"^Firewall: active(.|\n)*^\s+5353/udp$"),
    ("previous state saved", "snap list", r"^\s+2\s.*auto\s+Before rewind to 1$"),
    ("saved state has the change", "snap diff 1 2", r"^\+ /etc/probe$"),
    ("root still read-only", "grep ' / ' /proc/mounts", r"\sro[,\s]"),
    ("change the system again",
     "mount -o remount,bind,rw / && echo y > /etc/probe2 && mount -o remount,bind,ro / && echo ok",
     r"^ok$"),
    ("restore one file", "snap restore 2 /etc/probe && cat /etc/probe",
     r"^Restored /etc/probe from snapshot 2 .*\n(.*\n)?x$"),
    ("home snapshot",
     "mkdir -p /home/u && echo keep > /home/u/a && snap now --home -c home",
     r"^Snapshot \d+ created \(home\)$"),
    ("change home", "echo junk > /home/u/b && rm /home/u/a && echo ok", r"^ok$"),
]

# After "snap rewind --home".
HOME_CHECKS = [
    ("home file is back", "cat /home/u/a", r"^keep$"),
    ("later home file is gone", "ls /home/u/b 2>&1", r"No such file"),
    ("system was not rewound", "cat /etc/probe2", r"^y$"),
]

# After rewinding from the boot menu to the newest system snapshot, 3,
# taken just before /etc/probe was restored.
MENU_CHECKS = [
    ("snapshot 3 state is back", "cat /etc/probe2", r"^y$"),
    ("later restore is undone", "ls /etc/probe 2>&1", r"No such file"),
    ("menu rewind saved previous state", "snap list", r"^\s+\d+\s.*system\s+auto\s+Before rewind to 3$"),
    ("firewall after reboot", "worm fw status", r"^Firewall: active(.|\n)*^\s+5353/udp$"),
]


# Package manager and worm checks. {repo} is the repository URL.
WORM_CHECKS = [
    ("installed packages recorded", "wh list", r"^wh\s+0\.1\.0-\d+"),
    ("wh update", "wh --repo {repo} update", r"^Package index updated: \d+ packages$"),
    # Only the core repository: Debian's and Kali's lists need the internet.
    ("wh search", "wh search --from core tcpdump", r"^tcpdump 4\.99\.7-1"),
    ("worm unlock", "worm unlock", r"^The system is unlocked"),
    ("wh refuses while unlocked", "wh --repo {repo} -y remove tcpdump 2>&1",
     r"the system is unlocked"),
    ("lock refused while a file is open",
     "exec 3>/etc/held; worm lock 2>&1", r"^Cannot lock yet(.|\n)*/etc/held"),
    ("worm lock", "exec 3>&-; worm lock",
     r"^The system is locked\.\n(.*\n)?Snapshot \d+ created"),
    ("manual change kept", "ls /etc/held && touch /etc/x 2>&1", r"Read-only file system"),
]

# tcpdump comes with the image: the package tests remove it first.
FIRST_REMOVE_CHECKS = [
    ("preinstalled tcpdump removed", "ls /usr/bin/tcpdump 2>&1", r"No such file"),
    ("remove in history", "wh history", r"\sremove\s+\d+\s+(tcpdump diffutils|diffutils tcpdump)$"),
    ("busybox takes back the links of a removed package",
     "readlink /usr/bin/diff; readlink /usr/bin/cmp; grep -c -e ' usr/bin/diff$' -e ' usr/bin/cmp$' "
     "/usr/lib/wh/db/busybox/files; diff /dev/null /dev/null && echo same",
     r"^busybox\nbusybox\n2\nsame$"),
]

AFTER_INSTALL_CHECKS = [
    ("tcpdump runs", "tcpdump --version 2>&1", r"^tcpdump version 4\.99\.7"),
    ("install in history", "wh history", r"\sinstall\s+\d+\s+tcpdump$"),
    ("install snapshot", "snap list", r"auto\s+wh install tcpdump$"),
    ("still read-only after install", "touch /usr/x 2>&1", r"Read-only file system"),
]

AFTER_TRIAL_CHECKS = [
    ("trial removed tcpdump", "ls /usr/bin/tcpdump 2>&1", r"No such file"),
    ("trial is reported", "snap list", r"A trial is running"),
]

AFTER_TRIAL_END_CHECKS = [
    ("trial rolled back", "tcpdump --version 2>&1", r"^tcpdump version"),
]

AFTER_REMOVE_CHECKS = [
    ("tcpdump removed", "ls /usr/bin/tcpdump 2>&1", r"No such file"),
    ("library kept", "wh list", r"^libpcap\s"),
]

AFTER_UNDO_CHECKS = [
    ("undo brought tcpdump back", "tcpdump --version 2>&1", r"^tcpdump version"),
    ("undo in history", "wh history", r"\sundo\s+\d+\s+#\d+$"),
]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def fetch(port, timeout=10):
    """Fetch / from the VM's web server through the forwarded port."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
            s.sendall(b"GET / HTTP/1.0\r\n\r\n")
            return s.recv(64).startswith(b"HTTP/")
    except OSError:
        return False


# Paths the VM requested from the repository server, to detect leaks.
REQUESTS = []


def serve_repo():
    """Serve out/repo over HTTP; the VM reaches it at 10.0.2.2."""
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            REQUESTS.append(self.path)
            super().do_GET()

    handler = functools.partial(Quiet, directory=os.path.join(ROOT, "out", "repo"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return f"http://10.0.2.2:{server.server_address[1]}/"


class Console:
    def __init__(self, proc, log):
        self.proc = proc
        self.log = log
        self.buf = b""
        self.seq = 0

    def _read(self, timeout):
        ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not ready:
            return True
        chunk = os.read(self.proc.stdout.fileno(), 4096)
        if not chunk:
            return False
        self.buf += chunk
        self.log.write(chunk)
        self.log.flush()
        return True

    def expect(self, token, timeout):
        """Wait for token and return everything before it."""
        token = token.encode()
        deadline = time.time() + timeout
        while time.time() < deadline:
            if b"end Kernel panic" in self.buf:
                raise RuntimeError("kernel panic")
            if token in self.buf:
                before, _, self.buf = self.buf.partition(token)
                return ANSI.sub(b"", before).decode(errors="replace")
            if not self._read(1):
                break
        raise TimeoutError(f"timed out waiting for {token!r}")

    def send(self, text):
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()

    def run(self, cmd, timeout=60):
        # The marker is assembled by printf, so it never appears in the
        # echoed command line and cannot be matched too early.
        self.seq += 1
        tag = f"__END {self.seq}"
        self.send(f"{cmd}; printf '%s_%s__\\n' {tag}\n")
        out = self.expect(f"__END_{self.seq}__", timeout).replace("\r", "")
        _, _, out = out.rpartition(tag)          # drop the echoed command
        return out.partition("\n")[2].strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds to wait for the login prompt")
    args = parser.parse_args()

    # Disk writes are discarded, so the test always starts from the built
    # image and leaves it untouched.
    web_port = free_port()
    env = dict(os.environ, QEMU_HOSTFWD=f"tcp:127.0.0.1:{web_port}-:8080")
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    failures = 0
    passed = 0
    repo = serve_repo()
    worm_checks = [(n, c.format(repo=repo), p) for n, c, p in WORM_CHECKS]
    panic_checks = [(n, c.format(repo=repo), p) for n, c, p in PANIC_CHECKS]
    anon_checks = [(n, c.format(repo=repo), p) for n, c, p in ANON_CHECKS]
    bh_net_checks = [(n, c.format(repo=repo), p) for n, c, p in BH_NET_CHECKS]
    total = (len(CHECKS) + len(KERNEL_CHECKS) + len(DNS_CHECKS) + len(FIREWALL_CHECKS) +
             len(STATUS_CHECKS) + len(panic_checks) + len(SAFE_CHECKS) + 2 +
             len(HELPER_CHECKS) + len(TOOL_CHECKS) + 4 +
             len(anon_checks) + 2 +
             len(FIREWALL_OPEN_CHECKS) + len(FIREWALL_CLOSE_CHECKS) +
             len(FIREWALL_AFTER_CHECKS) + 3 + len(BLACKHOLE_CHECKS) + len(bh_net_checks) + len(WHC_CHECKS) + len(KIT_CHECKS) + len(TIME_CHECKS) + len(PASSWD_CHECKS) +
             len(SNAPSHOT_CHECKS) +
             len(BROWSE_CHECKS) + len(REWIND_CHECKS) + len(HOME_CHECKS) + len(MENU_CHECKS) +
             len(worm_checks) + len(FIRST_REMOVE_CHECKS) + 1 + len(AFTER_INSTALL_CHECKS) +
             len(AFTER_TRIAL_CHECKS) + len(AFTER_TRIAL_END_CHECKS) +
             len(AFTER_REMOVE_CHECKS) + len(AFTER_UNDO_CHECKS) + 9)
    start = time.time()

    def check(name, ok, out=""):
        nonlocal passed, failures
        passed += ok
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            print(f"      output: {out!r}")

    def run_checks(con, checks):
        for name, cmd, pattern in checks:
            out = con.run(cmd)
            check(name, re.search(pattern, out, re.MULTILINE) is not None, out)

    def restart_with(con, name, cmd, expect):
        """Run a command that restarts the system; check the boot output."""
        con.send(cmd + "\n")
        out = con.expect("login:", args.timeout)
        check(name, expect in out, out[-500:])
        con.send("root\n")
        con.expect("# ", 60)
        con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh

    def login(con, timeout):
        con.expect("login:", timeout)
        con.send("root\n")
        con.expect("# ", 60)
        con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh

    with open(LOG, "wb") as log:
        con = Console(proc, log)
        try:
            login(con, args.timeout)
            print(f"booted to login prompt in {time.time() - start:.0f}s")
            run_checks(con, CHECKS)
            run_checks(con, KERNEL_CHECKS)
            run_checks(con, DNS_CHECKS)
            run_checks(con, FIREWALL_CHECKS)
            check("closed port unreachable from outside", not fetch(web_port))
            run_checks(con, FIREWALL_OPEN_CHECKS)
            check("open port reachable from outside", fetch(web_port))
            run_checks(con, FIREWALL_CLOSE_CHECKS)
            check("port unreachable again after deny", not fetch(web_port))
            run_checks(con, FIREWALL_AFTER_CHECKS)
            run_checks(con, BLACKHOLE_CHECKS)
            run_checks(con, bh_net_checks)
            run_checks(con, WHC_CHECKS)
            run_checks(con, KIT_CHECKS)
            run_checks(con, PASSWD_CHECKS)
            run_checks(con, TIME_CHECKS)
            run_checks(con, STATUS_CHECKS)
            run_checks(con, anon_checks)
            check("nothing reached the network directly in anonymous mode",
                  "/anon-leak-check" not in REQUESTS, REQUESTS[-5:])
            check("direct requests arrive again after anon off",
                  "/anon-off-check" in REQUESTS, REQUESTS[-5:])
            run_checks(con, SNAPSHOT_CHECKS)
            con.send("snap browse 1\n")
            con.expect("Exit the shell to leave.", 60)
            run_checks(con, BROWSE_CHECKS)
            con.send("exit\n")
            time.sleep(1)

            boot_id = con.run("cat /proc/sys/kernel/random/boot_id")
            con.send("snap rewind -y\n")
            out = con.expect("login:", args.timeout)
            check("rewind applied by soft reboot",
                  "rewound system to snapshot 1" in out and "soft reboot" in out,
                  out[-500:])
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            out = con.run("cat /proc/sys/kernel/random/boot_id")
            check("kernel kept running", boot_id.splitlines()[-1] in out, out)
            run_checks(con, REWIND_CHECKS)

            con.send("snap rewind --home -y\n")
            out = con.expect("login:", args.timeout)
            check("home rewind applied", "rewound home to snapshot" in out, out[-500:])
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            run_checks(con, HOME_CHECKS)

            # Reboot and pick the newest snapshot from the boot menu: down to
            # the submenu (after safe mode), enter it, take its first entry.
            con.send("reboot\n")
            con.expect("Rewind to a snapshot", args.timeout)
            con.send("\x1b[B")
            time.sleep(0.5)
            con.send("\x1b[B")
            time.sleep(0.5)
            con.send("\r")
            con.expect("Snapshot 3", 60)
            time.sleep(0.5)
            con.send("\r")
            out = con.expect("login:", args.timeout)
            check("boot menu rewind applied", "rewound system to snapshot 3" in out,
                  out[-500:])
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            run_checks(con, MENU_CHECKS)

            run_checks(con, worm_checks)
            restart_with(con, "preinstalled package removed", "wh -y remove tcpdump diffutils",
                         "applied the package change")
            run_checks(con, FIRST_REMOVE_CHECKS)
            restart_with(con, "install applied", f"wh --repo {repo} -y install tcpdump",
                         "applied the package change")
            run_checks(con, AFTER_INSTALL_CHECKS)
            restart_with(con, "trial applied", "wh --try -y remove tcpdump",
                         "applied the package change (trial)")
            run_checks(con, AFTER_TRIAL_CHECKS)
            restart_with(con, "trial ended", "kill -HUP 1", "the trial was not kept")
            run_checks(con, AFTER_TRIAL_END_CHECKS)
            restart_with(con, "remove applied", "wh -y remove tcpdump",
                         "applied the package change")
            run_checks(con, AFTER_REMOVE_CHECKS)
            restart_with(con, "undo applied", "wh undo", "rewound system to snapshot")
            run_checks(con, AFTER_UNDO_CHECKS)

            run_checks(con, panic_checks)
            panic_id = re.search(r"^\s*(\d+)\*.*Panic at", con.run("snap list"), re.MULTILINE)

            # Safe mode, the second entry: a known-good snapshot, never the
            # one saved by the panic.
            con.send("reboot\n")
            out = con.expect("Rewind to a snapshot", args.timeout)
            con.send("\x1b[B")
            time.sleep(0.5)
            con.send("\r")
            out = con.expect("login:", args.timeout)
            rewound = re.search(r"rewound system to snapshot (\d+)", out)
            check("safe mode rewinds to a known-good snapshot",
                  rewound is not None and panic_id is not None and
                  rewound.group(1) != panic_id.group(1), out[-500:])
            check("safe mode leaves out the network",
                  "safe mode: not starting 30-network" in out, out[-500:])
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            run_checks(con, SAFE_CHECKS)
            run_checks(con, HELPER_CHECKS)
            run_checks(con, TOOL_CHECKS)

            # A shutdown hook (WormOS Amnesia wipes memory with one).
            con.run("mount -o remount,bind,rw / && mkdir -p /etc/worm/shutdown.d && "
                    "printf '#!/bin/sh\\necho shutdown-hook-ran > /dev/console\\n' "
                    "> /etc/worm/shutdown.d/50-test && chmod 755 /etc/worm/shutdown.d/50-test && "
                    "mount -o remount,bind,ro /")

            # The ACPI power button (QEMU's system_powerdown, as VM
            # managers send for "Shut Down") powers off cleanly. The QEMU
            # monitor shares the terminal: Ctrl-A c switches to it and back.
            out = con.run("ls -l /proc/$(pidof powerkey)/fd | grep -c /dev/input/event")
            check("power button watched", re.search(r"^[1-9]", out) is not None, out)
            con.send("\x01c")
            con.expect("(qemu)", 10)
            con.send("system_powerdown\n\x01c")
            out = con.expect("Power down", 120)
            check("power button powers off cleanly", "powering off" in out, out[-300:])
            check("shutdown hooks run before the end", "shutdown-hook-ran" in out, out[-600:])
            proc.wait(timeout=60)
            print("powered off cleanly")

            # The same image boots with BIOS firmware (SeaBIOS) too.
            proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot",
                                     "--bios"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, env=env)
            con = Console(proc, log)
            con.expect("login:", args.timeout)
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            out = con.run("test -d /sys/firmware/efi && echo efi || echo bios")
            check("boots with BIOS firmware", out == "bios", out)
            con.send("poweroff\n")
            con.expect("Power down", 120)
            proc.wait(timeout=60)
        except (TimeoutError, RuntimeError, subprocess.TimeoutExpired) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc.poll() is None:
                proc.kill()

    print(f"{passed}/{total} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
