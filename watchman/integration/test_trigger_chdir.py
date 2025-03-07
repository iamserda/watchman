# vim:ts=4:sw=4:et:
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# no unicode literals
from __future__ import absolute_import, division, print_function

import json
import os
import os.path
import sys
import time

import WatchmanTestCase


WATCHMAN_SRC_DIR = os.environ.get("WATCHMAN_SRC_DIR", os.getcwd())
THIS_DIR = os.path.join(WATCHMAN_SRC_DIR, "integration")


@WatchmanTestCase.expand_matrix
class TestTrigger(WatchmanTestCase.WatchmanTestCase):
    def fileContains(self, file_name, thing):
        if not os.path.exists(file_name):
            return False

        thing = thing + "\n"
        with open(file_name, "r") as f:
            return thing in f

    def fileHasValidJson(self, file_name):
        if not os.path.exists(file_name):
            return False

        try:
            with open(file_name, "r") as f:
                json.load(f)
            return True
        except Exception:
            return False

    def checkOSApplicability(self):
        if os.name == "nt":
            self.skipTest("no append on Windows")

    def test_triggerChdir(self):
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "sub"))
        self.watchmanCommand("watch", root)

        self.watchmanCommand(
            "trigger",
            root,
            {
                "name": "cap",
                "command": [sys.executable, os.path.join(THIS_DIR, "trig-cwd.py")],
                "stdout": ">%s" % os.path.join(root, "trig.log"),
                "expression": ["suffix", "txt"],
                "stdin": "/dev/null",
                "chdir": "sub",
            },
        )

        self.touchRelative(root, "A.txt")
        self.assertWaitFor(
            lambda: self.fileContains(
                os.path.join(root, "trig.log"), "PWD=" + os.path.join(root, "sub")
            )
        )
        self.assertWaitFor(
            lambda: self.fileContains(
                os.path.join(root, "trig.log"), "WATCHMAN_EMPTY_ENV_VAR="
            )
        )

    def test_triggerChdirRelativeRoot(self):
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "sub1"))
        os.mkdir(os.path.join(root, "sub1", "sub2"))
        self.watchmanCommand("watch", root)

        self.watchmanCommand(
            "trigger",
            root,
            {
                "name": "cap",
                "command": [sys.executable, os.path.join(THIS_DIR, "trig-cwd.py")],
                "stdout": ">%s" % os.path.join(root, "trig.log"),
                "expression": ["suffix", "txt"],
                "relative_root": "sub1",
                "stdin": "/dev/null",
                "chdir": "sub2",
            },
        )

        self.touchRelative(root, "sub1", "A.txt")
        self.assertWaitFor(
            lambda: self.fileContains(
                os.path.join(root, "trig.log"),
                "PWD=" + os.path.join(root, "sub1", "sub2"),
            )
        )

        self.assertWaitFor(
            lambda: self.fileContains(
                os.path.join(root, "trig.log"), "WATCHMAN_ROOT=" + root
            )
        )
        self.assertWaitFor(
            lambda: self.fileContains(
                os.path.join(root, "trig.log"),
                "WATCHMAN_RELATIVE_ROOT=" + os.path.join(root, "sub1"),
            )
        )

    def test_triggerMaxFiles(self):
        root = self.mkdtemp()

        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"settle": 200}))

        self.watchmanCommand("watch", root)

        self.watchmanCommand(
            "trigger",
            root,
            {
                "name": "cap",
                "command": [sys.executable, os.path.join(THIS_DIR, "trig-cwd.py")],
                "stdout": ">>%s" % os.path.join(root, "trig.log"),
                "expression": ["suffix", "txt"],
                "stdin": ["name"],
                "max_files_stdin": 2,
            },
        )

        self.touchRelative(root, "A.txt")
        self.assertWaitFor(
            lambda: self.fileContains(os.path.join(root, "trig.log"), "PWD=" + root)
        )

        self.assertTrue(
            not self.fileContains(
                os.path.join(root, "trig.log"), "WATCHMAN_FILES_OVERFLOW=true"
            ),
            msg="No overflow for a single file",
        )

        deadline = time.time() + 5
        overflown = False
        while time.time() < deadline:
            os.unlink(os.path.join(root, "trig.log"))

            self.touchRelative(root, "B.txt")
            self.touchRelative(root, "A.txt")
            self.touchRelative(root, "C.txt")
            self.touchRelative(root, "D.txt")

            self.assertWaitFor(
                lambda: self.fileContains(os.path.join(root, "trig.log"), "PWD=" + root)
            )

            if self.fileContains(
                os.path.join(root, "trig.log"), "WATCHMAN_FILES_OVERFLOW=true"
            ):
                overflown = True
                break

        self.assertTrue(overflown, "Observed WATCHMAN_FILES_OVERFLOW")

    def test_triggerNamePerLine(self):
        root = self.mkdtemp()

        self.watchmanCommand("watch", root)
        log_file = os.path.join(root, "trig.log")
        self.watchmanCommand(
            "trigger",
            root,
            {
                "name": "cat",
                "command": [sys.executable, os.path.join(THIS_DIR, "cat.py")],
                "stdout": ">%s" % log_file,
                "expression": ["suffix", "txt"],
                "stdin": "NAME_PER_LINE",
            },
        )

        self.touchRelative(root, "A.txt")
        self.assertWaitFor(lambda: self.fileContains(log_file, "A.txt"))

        self.touchRelative(root, "B.txt")
        self.touchRelative(root, "A.txt")
        self.assertWaitFor(
            lambda: self.fileContains(log_file, "A.txt")
            and self.fileContains(log_file, "B.txt")
        )
        with open(log_file, "r") as f:
            self.assertEqual(["A.txt\n", "B.txt\n"], sorted(f.readlines()))

    def test_triggerNamePerLineRelativeRoot(self):
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "subdir"))

        self.watchmanCommand("watch", root)
        log_file = os.path.join(root, "trig.log")
        self.watchmanCommand(
            "trigger",
            root,
            {
                "name": "cat",
                "command": [sys.executable, os.path.join(THIS_DIR, "cat.py")],
                "relative_root": "subdir",
                "stdout": ">%s" % log_file,
                "expression": ["suffix", "txt"],
                "stdin": "NAME_PER_LINE",
            },
        )

        self.touchRelative(root, "A.txt")
        self.touchRelative(root, "subdir", "B.txt")
        self.assertWaitFor(lambda: self.fileContains(log_file, "B.txt"))

    def test_triggerNamePerLineAppend(self):
        root = self.mkdtemp()

        self.watchmanCommand("watch", root)
        log_file = os.path.join(root, "trig.log")
        self.watchmanCommand(
            "trigger",
            root,
            {
                "name": "cat",
                "command": [sys.executable, os.path.join(THIS_DIR, "cat.py")],
                "stdout": ">>%s" % log_file,
                "expression": ["suffix", "txt"],
                "stdin": "NAME_PER_LINE",
            },
        )

        self.touchRelative(root, "A.txt")
        self.assertWaitFor(lambda: self.fileContains(log_file, "A.txt"))

        self.touchRelative(root, "B.txt")
        self.assertWaitFor(
            lambda: self.fileContains(log_file, "A.txt")
            and self.fileContains(log_file, "B.txt")
        )
        with open(log_file, "r") as f:
            self.assertEqual(["A.txt\n", "B.txt\n"], sorted(f.readlines()))

    def test_triggerJsonNameOnly(self):
        root = self.mkdtemp()

        self.watchmanCommand("watch", root)
        log_file = os.path.join(root, "trig.log")
        self.watchmanCommand(
            "trigger",
            root,
            {
                "name": "cat",
                "command": [sys.executable, os.path.join(THIS_DIR, "cat.py")],
                "stdout": ">%s" % log_file,
                "expression": ["suffix", "txt"],
                "stdin": ["name"],
            },
        )

        self.touchRelative(root, "A.txt")
        self.assertWaitFor(lambda: self.fileHasValidJson(log_file))

        with open(log_file, "r") as f:
            data = json.load(f)
        self.assertEqual(["A.txt"], data)

    def test_triggerJsonNameAndSize(self):
        root = self.mkdtemp()

        self.watchmanCommand("watch", root)
        log_file = os.path.join(root, "trig.log")
        self.watchmanCommand(
            "trigger",
            root,
            {
                "name": "cat",
                "command": [sys.executable, os.path.join(THIS_DIR, "cat.py")],
                "stdout": ">%s" % log_file,
                "expression": ["suffix", "txt"],
                "stdin": ["name", "size"],
            },
        )

        self.touchRelative(root, "A.txt")
        self.assertWaitFor(lambda: self.fileHasValidJson(log_file))

        with open(log_file, "r") as f:
            data = json.load(f)
        self.assertEqual("A.txt", data[0]["name"])
        self.assertEqual(0, data[0]["size"])
