# Copyright © 2026 CCP ehf.

import _imp
import importlib.machinery
import os
import sys

class FlavoredExtensionFileLoader(importlib.machinery.ExtensionFileLoader):
    def __init__(self, name, path, flavor):
        super().__init__(name, path)
        self.flavor = flavor

    def create_module(self, spec):
        unflavored_name = spec.name
        spec.name = f"{spec.name}_{self.flavor}"
        module = super().create_module(spec)
        if spec.name in sys.modules:  # this is a simple guard in case that the import failed
            sys.modules[unflavored_name] = sys.modules[spec.name]

        return module


class FlavoredExtensionImporter:
    def __init__(self, flavor):
        self.flavor = flavor
        self.loader_class = FlavoredExtensionFileLoader
        self.extension_suffixes = [f"_{self.flavor}{suffix}" for suffix in _imp.extension_suffixes()]

    def find_spec(self, fullname, path=None, target=None):
        for suffix in self.extension_suffixes:
            for path in sys.path:
                origin = os.path.join(path, f"{fullname}{suffix}")
                if os.path.exists(origin):
                    break
            else:
                continue
            break
        else:
            return None  # no flavored version was found, return None

        return importlib.machinery.ModuleSpec(fullname, self.loader_class(fullname, origin, self.flavor), origin=origin)


flavor = os.environ.get('BUILDFLAVOR', '')
if flavor:
    sys.meta_path.insert(0, FlavoredExtensionImporter(flavor))


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
