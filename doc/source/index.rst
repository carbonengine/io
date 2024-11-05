carbon-io
=========

This component enables tasklet-friendly asynchronous socket IO with an interface as close to the standard Python socket module as possible. Tasklet-blocking behavior is supported for TCP and UDP connections only. Other socket types will exhibit the standard blocking behavior. Some functionality to support sending / receiving of machoNet packets has been added.

This project provides modified versions of the C part of the Python `socket <https://docs.python.org/3.12/library/socket.html>`_ and `ssl <https://docs.python.org/3.12/library/ssl.html>`_ modules, as well as an unmodified version of the `select <https://docs.python.org/3.12/library/select.html>`_ module. The module names have the prefix ``carbon`` in order to distinguish them from their unmodified counterparts. In order to use them with the standard socket and ssl modules, they need to be injected into ``sys.modules`` with the carbon prefix removed.

Once the modules have been successfully patched in, they can be used the same way one would use the standard library modules, except that in order to unblock tasklets that have completed their IO, the ``dispatch()`` method should be called regularly.

API Changes
===========

.. toctree::
   :maxdepth: 1

   pythonApi
   cApi

Misc
====

.. toctree::
   :maxdepth: 1

   componentInteraction
   monkeypatching
