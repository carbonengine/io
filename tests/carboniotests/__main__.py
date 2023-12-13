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

    # Cannot import unittest before we patch the socket, because unittest imports socket.
    import unittest
    __unittest = True

    import stackless

    class TaskletTestRunner(unittest.TextTestRunner):
        def __init__(self, *args, **kwargs):
            self.result = None
            super().__init__(*args, **kwargs)

        def run(self, test):
            stackless.tasklet(self._run_impl)(test)
            while self.result is None:
                stackless.run()

            return self.result

        def _run_impl(self, test):
            self.result = super().run(test)

    unittest.main(module=None, testRunner=TaskletTestRunner())


main()
