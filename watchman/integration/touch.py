# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Portable simple implementation of `touch`

# no unicode literals
from __future__ import absolute_import, division, print_function

import errno
import os
import sys


fname = sys.argv[1]

try:
    os.utime(fname, None)
except OSError as e:
    if e.errno == errno.ENOENT:
        with open(fname, "a"):
            os.utime(fname, None)
    else:
        raise
