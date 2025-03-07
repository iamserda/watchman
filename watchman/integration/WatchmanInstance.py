# vim:ts=4:sw=4:et:
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# no unicode literals
from __future__ import absolute_import, division, print_function

import atexit
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid

import pywatchman
import TempDir


try:
    import pwd
except ImportError:
    # Windows
    pass

tls = threading.local()


def setSharedInstance(inst):
    global tls
    tls.instance = inst
    atexit.register(lambda: inst.stop())


def getSharedInstance():
    global tls
    if hasattr(tls, "instance"):
        return tls.instance
    # Ensure that the temporary dir is configured
    TempDir.get_temp_dir().get_dir()
    inst = Instance()
    inst.start()
    setSharedInstance(inst)
    return tls.instance


def hasSharedInstance():
    global tls
    return hasattr(tls, "instance")


class InitWithFilesMixin(object):
    def _init_state(self):
        self.base_dir = tempfile.mkdtemp(prefix="inst")
        # no separate user directory here -- that's only in InitWithDirMixin
        self.user_dir = None
        self.cfg_file = os.path.join(self.base_dir, "config.json")
        self.log_file_name = os.path.join(self.base_dir, "log")
        self.cli_log_file_name = os.path.join(self.base_dir, "cli-log")
        self.pid_file = os.path.join(self.base_dir, "pid")
        self.pipe_name = (
            "\\\\.\\pipe\\watchman-test-%s"
            % uuid.uuid5(uuid.NAMESPACE_URL, self.base_dir).hex
        )
        self.sock_file = os.path.join(self.base_dir, "sock")
        self.state_file = os.path.join(self.base_dir, "state")

    def get_state_args(self):
        return [
            "--unix-listener-path={0}".format(self.sock_file),
            "--named-pipe-path={0}".format(self.pipe_name),
            "--logfile={0}".format(self.log_file_name),
            "--statefile={0}".format(self.state_file),
            "--pidfile={0}".format(self.pid_file),
        ]


class InitWithDirMixin(object):
    """A mixin to allow setting up a state dir rather than a state file. This is
    only meant to test state dir creation and permissions -- most operations are
    unlikely to work.
    """

    def _init_state(self):
        self.base_dir = tempfile.mkdtemp(prefix="inst")
        self.cfg_file = os.path.join(self.base_dir, "config.json")
        # This needs to be separate from the log_file_name because the
        # log_file_name won't exist in the beginning, but the cli_log_file_name
        # will.
        self.cli_log_file_name = os.path.join(self.base_dir, "cli-log")
        # This doesn't work on Windows, but we don't expect to be hitting this
        # codepath on Windows anyway
        username = pwd.getpwuid(os.getuid())[0]
        self.user_dir = os.path.join(self.base_dir, "%s-state" % username)
        self.log_file_name = os.path.join(self.user_dir, "log")
        self.sock_file = os.path.join(self.user_dir, "sock")
        self.state_file = os.path.join(self.user_dir, "state")
        self.pipe_name = "INVALID"

    def get_state_args(self):
        return ["--test-state-dir={0}".format(self.base_dir)]


class _Instance(object):
    # Tracks a running watchman instance.  It is created with an
    # overridden global configuration file; you may pass that
    # in to the constructor

    def __init__(self, config=None, start_timeout=60.0, debug_watchman=False):
        self.start_timeout = start_timeout
        self._init_state()
        self.proc = None
        self.pid = None
        self.debug_watchman = debug_watchman
        with open(self.cfg_file, "w") as f:
            f.write(json.dumps(config or {}))

    def __del__(self):
        self.stop()

    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.stop()

    def getSockPath(self):
        return pywatchman.SockPath(
            unix_domain=self.getUnixSockPath(), named_pipe=self.getNamedPipePath()
        )

    def getUnixSockPath(self):
        return self.sock_file

    def getNamedPipePath(self):
        return self.pipe_name

    def getCLILogContents(self):
        with open(self.cli_log_file_name, "r") as f:
            return f.read()

    def getServerLogContents(self):
        with open(self.log_file_name, "r") as f:
            return f.read()

    def stop(self):
        if self.proc:
            self.proc.kill()
            self.proc.wait()
            self.proc = None

    def watchmanBinary(self):
        return os.environ.get("WATCHMAN_BINARY", "watchman")

    def commandViaCLI(self, cmd, prefix=None):
        """a very bare bones helper to test the site spawner functionality"""
        args = prefix or []
        args.extend([self.watchmanBinary(), "--log-level=2"])
        args.extend(self.get_state_args())
        args.extend(cmd)

        env = os.environ.copy()
        env["WATCHMAN_CONFIG_FILE"] = self.cfg_file
        del env["WATCHMAN_NO_SPAWN"]
        proc = subprocess.Popen(
            args, env=env, stdin=None, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        return proc.communicate()

    def start(self, extra_env=None):
        args = [self.watchmanBinary(), "--foreground", "--log-level=2"]
        args.extend(self.get_state_args())
        env = os.environ.copy()
        env["WATCHMAN_CONFIG_FILE"] = self.cfg_file
        if extra_env:
            env.update(extra_env)
        with open(self.cli_log_file_name, "w+") as cli_log_file:
            self.proc = subprocess.Popen(
                args, env=env, stdin=None, stdout=cli_log_file, stderr=cli_log_file
            )
        if self.debug_watchman:
            print("Watchman instance PID: " + str(self.proc.pid))
            if pywatchman.compat.PYTHON3:
                user_input = input
            else:
                user_input = raw_input  # noqa:F821
            user_input("Press Enter to continue...")

        # wait for it to come up
        deadline = time.time() + self.start_timeout
        while time.time() < deadline:
            try:
                client = pywatchman.client(sockpath=self.getSockPath())
                self.pid = client.query("get-pid")["pid"]
                break
            except pywatchman.SocketConnectError:
                t, val, tb = sys.exc_info()
                time.sleep(0.1)
            finally:
                client.close()

        if self.pid is None:
            # self.proc didn't come up: wait for it to die
            self.stop()
            pywatchman.compat.reraise(t, val, tb)

    def _waitForSuspend(self, suspended, timeout):
        if os.name == "nt":
            # There's no 'ps' equivalent we can use
            return True

        # Check the information in the 'ps' output
        deadline = time.time() + timeout
        state = "s" if sys.platform.startswith("sunos") else "state"
        while time.time() < deadline:
            out, err = subprocess.Popen(
                ["ps", "-o", state, "-p", str(self.pid)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            ).communicate()
            status = out.splitlines()[-1]
            is_suspended = "T" in status.decode("utf-8", "surrogateescape")
            if is_suspended == suspended:
                return True

            time.sleep(0.03)
        return False

    def suspend(self):
        if self.proc.poll() or self.pid <= 1:
            raise Exception("watchman process isn't running")
        if os.name == "nt":
            subprocess.check_call(["susres.exe", "suspend", str(self.pid)])
        else:
            os.kill(self.pid, signal.SIGSTOP)

        if not self._waitForSuspend(True, 5):
            raise Exception("watchman process didn't stop in 5 seconds")

    def resume(self):
        if self.proc.poll() or self.pid <= 1:
            raise Exception("watchman process isn't running")
        if os.name == "nt":
            subprocess.check_call(["susres.exe", "resume", str(self.pid)])
        else:
            os.kill(self.pid, signal.SIGCONT)

        if not self._waitForSuspend(False, 5):
            raise Exception("watchman process didn't resume in 5 seconds")


class Instance(_Instance, InitWithFilesMixin):
    pass


class InstanceWithStateDir(_Instance, InitWithDirMixin):
    pass
