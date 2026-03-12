# Copyright © 2026 CCP ehf.

import sys
import unittest


# mock scheduler module for test discovery; it just needs to be importable at that point.
# The alternative is to modify the PYTHONPATH
sys.modules['scheduler'] = sys


def print_suite(suite):
    if hasattr(suite, '_exception'):
        print(suite._exception)
        sys.exit(1)
    else:
        if not hasattr(suite, '__iter__'):
            print(suite.id())
        else:
            [print_suite(suite) for suite in suite]


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.discover('.')
    print_suite(suite)
