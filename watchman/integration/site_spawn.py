#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# This is a simple script that spawns a watchman process in the background
# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import subprocess
import sys


args = sys.argv[1:]
args.insert(0, os.environ.get("WATCHMAN_BINARY", "watchman"))
args.insert(1, "--foreground")
subprocess.Popen(args)
