#!/usr/bin/env python
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# no unicode literals
from __future__ import absolute_import, division, print_function

import sys


args = sys.argv[1:]

if not args:
    args = ["-"]

for file_name in args:
    if file_name == "-":
        sys.stdout.write(sys.stdin.read())
    else:
        with open(file_name, "rb") as f:
            sys.stdout.write(f.read())
