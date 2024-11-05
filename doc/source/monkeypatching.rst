Patching socket/ssl To Use the Carbon Modules
---------------------------------------------

Currently this gets done when starting up PyOS in Blue, but here is an explanation for what needs to happen in order to set up the Carbon IO modules.
The carbon modules are meant to replace their standard python counterparts. In order to make this work we need to inject them into ``sys.modules`` under the name of the modules they are replacing.

.. code-block:: python

    import _carbonsocket
    import _carbonssl
    import carbonselect
    sys.modules["_socket"] = _carbonsocket
    sys.modules["_ssl"] = _carbonssl
    sys.modules["select"] = carbonselect


Depending on whether they have already been imported, the socket and ssl modules may need to be reloaded as well.

.. code-block:: python

    import socket
    import ssl
    reload(socket)
    reload(ssl)

The reason for this monkey-patching is that in order to take advantage of PyCharm's integrated debugging tools, we need to be able to import the standard socket module when starting the debugger and establishing a connection. After the debugging session has been established, the tasklet blocking version can be patched in.
