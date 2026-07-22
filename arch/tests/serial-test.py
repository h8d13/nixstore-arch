#!/usr/bin/env python3
# Full install flow through the SERIAL=on console of uefi-vm.sh, on a
# pty (mon:stdio needs a terminal): phase1 = fresh (ISO + blank target,
# nixgen-setup /dev/vda answered over serial), phase2 = boot (installed
# disk alone, store must come from the disk, no ISO hunt). Markers
# mirror install-test.sh; this additionally exercises uefi-vm.sh itself
# (OVMF probe, persistent NVRAM, SERIAL mux) where install-test drives
# bare qemu over a unix socket.
# usage: serial-test.py [phase1|phase2]   (no arg = both)
# transcript: build/serial-test.log
import os, pty, select, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(
	os.path.dirname(os.path.abspath(__file__))))
LOG = os.path.join(REPO, "build/serial-test.log")
NAME = "serial-test"

def run_phase(mode, steps, boot_timeout):
	log = open(LOG, "ab")
	master, slave = pty.openpty()
	env = dict(os.environ, SERIAL="on")
	p = subprocess.Popen(["arch/uefi-vm.sh", mode], cwd=REPO,
		stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True)
	os.close(slave)
	buf = b""

	def wait_for(pat, timeout):
		nonlocal buf
		deadline = time.time() + timeout
		while time.time() < deadline:
			if p.poll() is not None:
				return False
			r, _, _ = select.select([master], [], [], 1)
			if master in r:
				try:
					d = os.read(master, 65536)
				except OSError:
					return False
				log.write(d)
				log.flush()
				buf += d
				if pat.encode() in buf:
					buf = b""
					return True
		return False

	if not wait_for("NIXARCH BOOT OK", boot_timeout):
		print(f"FAIL [{mode}]: no boot marker in {boot_timeout}s"
			f" (see {LOG})")
		p.kill()
		sys.exit(1)
	print(f"[{mode}] shell up")
	for send_line, expect, timeout in steps:
		os.write(master, send_line.encode() + b"\r")
		if not wait_for(expect, timeout):
			print(f"FAIL [{mode}]: no '{expect}' after: {send_line}")
			p.kill()
			sys.exit(1)
		print(f"[{mode}] ok: {expect}")
	os.write(master, b"poweroff\r")
	p.wait(timeout=120)
	if p.returncode != 0:
		print(f"FAIL [{mode}]: qemu exited rc={p.returncode}")
		sys.exit(1)
	print(f"[{mode}] qemu exited clean")

def phase1():
	run_phase("fresh", [
		(f"nixgen-setup /dev/vda {NAME}",
			"type the device path to continue", 60),
		("/dev/vda", f"installed: {NAME} on /dev/vda", 540),
	], 180)

def phase2():
	# quoted marker fragments (STORE_ON_"DISK") keep the command echo
	# from matching its own expect string, same trick as install-test.sh
	run_phase("boot", [
		('grep -o "nixgen=[^ ]*" /proc/cmdline', f"-{NAME}", 30),
		('findmnt -no SOURCE /nixstore | grep -q /dev/ '
			'&& echo STORE_ON_"DISK"', "STORE_ON_DISK", 30),
		('journalctl -b -u nixgen-store.service --no-pager '
			'| grep -c "waiting for device labeled" '
			'| sed "s/^/HUNT=/"', "HUNT=0", 30),
		('journalctl -b -u nixgen-store.service --no-pager '
			'| grep -q "writable store on" '
			'&& echo FROM_"STOREDEV"', "FROM_STOREDEV", 30),
	], 180)

arg = sys.argv[1] if len(sys.argv) > 1 else ""
if arg not in ("", "phase1", "phase2"):
	sys.exit("usage: serial-test.py [phase1|phase2]")
if arg != "phase2":
	if os.path.exists(LOG):
		os.unlink(LOG)
	phase1()
if arg != "phase1":
	phase2()
	print("PASS: serial install flow, disk boots alone, store from disk")
