C API Changes
=============

Carbon-IO adds some functions to the capsule exposed by the standard Python socket module's C API.

To access these functions include ``<socket.h>`` and import the Python capsule by calling ``PySocketModule_ImportModuleAndAPI()``. If the import is successful, the function will return a pointer to an instance of ``PySocketModule_APIObject``.

The API Object
--------------

.. doxygenstruct:: PySocketModule_APIObject
    :members:

Type Definitions
----------------

.. doxygentypedef:: OobDataCallback