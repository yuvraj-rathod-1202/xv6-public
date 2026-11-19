#!/usr/bin/env python3
"""Automated test harness for the xv6 scheduler project.

    make test          # quick suite
    make test-full     # everything, including usertests and benchmarks
    python3 tools/xv6test.py --quick|--full [--only NAME]

What it does:
  1. Builds the host red-black-tree unit test and runs it, then runs the
     mutation test (each deliberately broken copy of rbtree.c must fail it).
  2. Builds a SCHED_DEBUG kernel (every run-queue operation re-verifies the
     scheduler invariants and every CFS pick is cross-checked; any violation
     panics) and boots it in QEMU on 1, 2 and 4 CPUs, running schedtest and
     the randomised schedstress on each.
  3. Builds the normal (release) kernel and runs schedtest again.
  --full additionally:
  4. Runs a long (4000-tick) schedstress on 4 CPUs, and xv6's own usertests
     under RR and under CFS (debug kernel, SMP).
  5. Runs the benchmarks on the release kernel with QEMU's -icount clock
     (deterministic timing): schedbench, schedoverhead, and scheduling traces
     of the same workload under RR, FIFO and CFS rendered as SVG Gantt charts.

Every QEMU boot gets a fresh copy of the disk image. A run fails on a kernel
panic, a missing success line, or a timeout. Console logs, benchmark output
and charts go to results/; results/summary.md lists every check.
Exit status is 0 only if every check passed.
"""
import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")
LOGS = os.path.join(RESULTS, "logs")
QEMU = shutil.which("qemu-system-i386") or "qemu-system-i386"

PANIC = re.compile(rb"panic: [^\n]*")


class BootError(Exception):
    pass


class Xv6:
    """One QEMU boot of xv6, driven through its serial console."""

    def __init__(self, image_dir, cpus, log_name, extra=()):
        self.tmp = tempfile.mkdtemp(prefix="xv6test-")
        fs = os.path.join(self.tmp, "fs.img")
        shutil.copy(os.path.join(image_dir, "fs.img"), fs)
        cmd = [QEMU, "-nographic", "-no-reboot",
               "-drive", f"file={fs},index=1,media=disk,format=raw",
               "-drive", f"file={os.path.join(image_dir, 'xv6.img')},"
                         "index=0,media=disk,format=raw",
               "-smp", f"{cpus},sockets={cpus}", "-m", "512", *extra]
        self.log_path = os.path.join(LOGS, log_name)
        self.log = open(self.log_path, "wb")
        self.buf = b""
        self.pos = 0          # start of output not yet consumed by expect()
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.expect(rb"init: starting sh\r?\n\$ ", 120)

    def _read(self, timeout):
        r, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not r:
            return
        data = os.read(self.proc.stdout.fileno(), 65536)
        if not data:
            raise BootError("QEMU exited")
        data = data.replace(b"\r", b"")
        self.buf += data
        self.log.write(data)
        self.log.flush()

    def expect(self, pattern, timeout):
        """Wait for pattern in new output; return the text before + match."""
        pat = re.compile(pattern)
        deadline = time.time() + timeout
        while True:
            m = pat.search(self.buf, self.pos)
            p = PANIC.search(self.buf, self.pos)
            if p and (not m or p.start() < m.start()):
                raise BootError("kernel " + p.group().decode(errors="replace"))
            if m:
                text = self.buf[self.pos:m.end()]
                self.pos = m.end()
                return text.decode(errors="replace")
            left = deadline - time.time()
            if left <= 0:
                raise BootError(f"timeout after {timeout}s waiting for "
                                f"{pattern.decode(errors='replace')!r}")
            self._read(min(left, 1.0))

    def run(self, command, success, timeout, failure=None):
        """Type a command; wait for its success line and the next prompt."""
        self.proc.stdin.write(command.encode() + b"\n")
        self.proc.stdin.flush()
        pats = [success] + ([failure] if failure else [])
        text = self.expect(b"|".join(b"(?:" + p + b")" for p in pats), timeout)
        text += self.expect(rb"\$ ", 60)
        ok = re.search(success, text.encode()) is not None
        return ok, text

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        self.log.close()
        shutil.rmtree(self.tmp, ignore_errors=True)


results = []   # (name, passed, seconds, detail)


def record(name, passed, seconds, detail=""):
    results.append((name, passed, seconds, detail))
    mark = "PASS" if passed else "FAIL"
    print(f"  [{mark}] {name} ({seconds:.0f}s){' - ' + detail if detail else ''}",
          flush=True)


def sh(cmd, log_name):
    with open(os.path.join(LOGS, log_name), "w") as log:
        return subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                              shell=isinstance(cmd, str)).returncode


def build(kind):
    """Build kernel + fs image; copy them to results/build-<kind>/."""
    out = os.path.join(RESULTS, f"build-{kind}")
    os.makedirs(out, exist_ok=True)
    flags = "SCHED_DEBUG=1" if kind == "debug" else ""
    t0 = time.time()
    sh("make clean", f"make-clean-{kind}.log")
    rc = sh(f"make {flags} kernel xv6.img fs.img", f"make-{kind}.log")
    record(f"build {kind} kernel", rc == 0, time.time() - t0,
           "" if rc == 0 else f"see results/logs/make-{kind}.log")
    if rc != 0:
        return None
    for f in ("xv6.img", "fs.img"):
        shutil.copy(os.path.join(ROOT, f), out)
    return out


def boot_and_run(name, image_dir, cpus, steps, extra=(), log=None):
    """steps: list of (label, command, success_regex, timeout, failure_regex)."""
    log = log or name.replace(" ", "_").replace("/", "_") + ".log"
    t0 = time.time()
    try:
        vm = Xv6(image_dir, cpus, log, extra)
    except BootError as e:
        record(f"{name}: boot", False, time.time() - t0, str(e))
        return {}
    outputs = {}
    try:
        for label, command, success, timeout, failure in steps:
            t1 = time.time()
            try:
                ok, text = vm.run(command, success, timeout, failure)
                outputs[label] = text
                record(f"{name}: {label}", ok, time.time() - t1,
                       "" if ok else f"see results/logs/{log}")
            except BootError as e:
                record(f"{name}: {label}", False, time.time() - t1,
                       f"{e} (results/logs/{log})")
                break
    finally:
        vm.close()
    return outputs


SCHEDTEST = ("schedtest", "schedtest", rb"schedtest: ALL TESTS PASSED", 900,
             rb"schedtest: \d+ CHECK")
STRESS = ("schedstress", "schedstress 500", rb"schedstress: PASS", 900,
          rb"schedstress: FAIL")


def usertests(policy):
    return [("set " + policy, "schedctl " + policy, rb"Scheduler set to", 30, None),
            ("usertests", "usertests", rb"ALL TESTS PASSED", 1800,
             rb"FAILED|failed")]


def run_benchmarks(release):
    icount = ("-icount", "shift=3,sleep=off")
    out = boot_and_run("bench 1cpu", release, 1, [
        ("schedbench", "schedbench", rb"=== nice[^$]*shares match", 1800, None),
        ("schedoverhead", "schedoverhead", rb"\n\s+240 \|[^\n]*\n", 1800, None),
    ], extra=icount)
    for label, fname in (("schedbench", "schedbench.txt"),
                         ("schedoverhead", "schedoverhead.txt")):
        if label in out:
            with open(os.path.join(RESULTS, fname), "w") as f:
                f.write(out[label])

    # Same workload under each scheduler, traced.
    steps = [(f"trace {p}", f"schedtrace -c schedbench cpu {p}",
              rb"TRACE-END", 600, None) for p in ("rr", "fifo", "cfs")]
    steps += [("trace ascii cfs", "schedtrace schedbench mix cfs",
               rb"avg run \(= effective slice\)\n", 600, None)]
    out = boot_and_run("trace 1cpu", release, 1, steps, extra=icount,
                       log="trace_1cpu.log")
    for p, title in (("rr", "Round Robin"), ("fifo", "FIFO"), ("cfs", "CFS")):
        text = out.get(f"trace {p}")
        if not text:
            continue
        raw = os.path.join(RESULTS, f"trace-{p}.txt")
        with open(raw, "w") as f:
            f.write(text)
        svg = os.path.join(RESULTS, f"gantt-{p}.svg")
        rc = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gantt.py"),
                             raw, "-o", svg, "--title",
                             f"{title}: one 100-tick job, then four 20-tick jobs (1 CPU)"],
                            capture_output=True).returncode
        record(f"gantt chart {p}", rc == 0, 0, os.path.relpath(svg, ROOT))
    if "trace ascii cfs" in out:
        with open(os.path.join(RESULTS, "trace-ascii-cfs.txt"), "w") as f:
            f.write(out["trace ascii cfs"])


def write_summary(started):
    passed = sum(1 for r in results if r[1])
    path = os.path.join(RESULTS, "summary.md")
    with open(path, "w") as f:
        f.write("# xv6 scheduler test results\n\n")
        f.write(f"{passed}/{len(results)} checks passed in "
                f"{(time.time() - started) / 60:.1f} minutes.\n\n")
        f.write("| Check | Result | Time (s) | Detail |\n|---|---|---|---|\n")
        for name, ok, secs, detail in results:
            f.write(f"| {name} | {'PASS' if ok else '**FAIL**'} | "
                    f"{secs:.0f} | {detail} |\n")
    return passed, path


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--quick", action="store_true", help="quick suite (default)")
    mode.add_argument("--full", action="store_true", help="full suite")
    args = ap.parse_args()

    started = time.time()
    shutil.rmtree(RESULTS, ignore_errors=True)
    os.makedirs(LOGS)

    print("== host unit tests", flush=True)
    t0 = time.time()
    rc = sh("make rbtree-test", "rbtree_test.log")
    record("rbtree unit tests, 1.8M verified operations (host)", rc == 0,
           time.time() - t0)
    t0 = time.time()
    rc = sh("make rbtree-mutation", "rbtree_mutation.log")
    record("rbtree mutation test: every broken tree is caught (host)", rc == 0,
           time.time() - t0)

    print("== SCHED_DEBUG kernel", flush=True)
    debug = build("debug")
    if debug:
        for cpus in (1, 2, 4):
            boot_and_run(f"debug {cpus}cpu", debug, cpus, [SCHEDTEST, STRESS])
        if args.full:
            boot_and_run("debug 4cpu long", debug, 4, [
                ("schedstress 4000 ticks", "schedstress 4000",
                 rb"schedstress: PASS", 1800, rb"schedstress: FAIL")])
            boot_and_run("usertests rr 2cpu", debug, 2, usertests("rr"))
            boot_and_run("usertests cfs 4cpu", debug, 4, usertests("cfs"))

    print("== release kernel", flush=True)
    release = build("release")
    if release:
        boot_and_run("release 2cpu", release, 2, [SCHEDTEST])
        if args.full:
            run_benchmarks(release)

    passed, path = write_summary(started)
    print(f"\n{passed}/{len(results)} checks passed; summary in "
          f"{os.path.relpath(path, ROOT)}")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
