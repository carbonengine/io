import sys

# Patch carbon-io exports
import _carbonsocket
import _carbonssl
import carbonselect
sys.modules['_socket'] = _carbonsocket
sys.modules['_ssl'] = _carbonssl
sys.modules['select'] = carbonselect

if 'socket' in sys.modules:
    raise RuntimeError("Socket module should not be loaded before patching `carbon-io` exports")

import unittest
import socket
import scheduler


def main():
    import sys
    if sys.argv[0].endswith("__main__.py"):
        import os.path
        # We change sys.argv[0] to make help message more useful
        # use executable without path, unquoted
        # (it's just a hint anyway)
        # (if you have spaces in your executable you get what you deserve!)
        executable = os.path.basename(sys.executable)
        sys.argv[0] = executable + " -m unittest"
        del os

    class TaskletTestRunner(unittest.TextTestRunner):
        def __init__(self, *args, **kwargs):
            self.result = None
            super().__init__(*args, **kwargs)

        def run(self, test):
            scheduler.tasklet(self._run_impl)(test)
            while self.result is None:
                scheduler.run()
                socket.dispatch()

            return self.result

        def _run_impl(self, test):
            self.result = super().run(test)

    unittest.main(module=None, testRunner=TaskletTestRunner())


main()
