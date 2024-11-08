Component Interaction
=====================

Blue
----
The Carbon IO modules get patched into ``sys.modules`` when starting up PyOS in Blue. Blue also calls the ``dispatch()`` method each tick in order to unblock tasklets that have finished their blocking socket IO.

DB
__
Carbon DB takes advantage of the exposed UV loop from the ``_socket`` modules C API. This means that when blue dispatches socket IO events for Carbon IO it also dispatches events for Carbon DB.
