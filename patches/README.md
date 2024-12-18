# CCP Python Module Modifications

These patch files keep track of changes made to files sourced from CCPs fork of the cpython repo https://github.com/ccpgames/cpython. Please make sure to generate new patch files when you make changes to their corresponding source files.

When updating to a new Python version fresh copies of those files should be placed in the repo. Then the patches should be applied either manually by hand, or using whatever tools you like. Be mindful that not all changes required are necessarily going to be included in the patch. For example the socket module has been converted from C to C++, so any C++ incompatible changes will have to be fixed up.

Mapping of files sourced from Python:

| cpython                         | carbon-io                   |
|---------------------------------|-----------------------------|
| Modules/socketmodule.h          | include/socketmodule.h      |
| Modules/_ssl/clinic/cert.c.h    | src/_ssl/clinic/cert.c.h    |       
| Modules/_ssl/cert.c             | src/_ssl/cert.c             |
| Modules/_ssl/debughelpers.c     | src/_ssl/debughelpers.c     |
| Modules/_ssl/misc.c             | src/_ssl/misc.c             |
| Modules/clinic/_ssl.c.h         | src/clinic/_ssl.c.h         |
| Modules/clinic/selectmodule.c.h | src/clinic/selectmodule.c.h |      
| Modules/clinic/socketmodule.c.h | src/clinic/socketmodule.c.h |    
| Modules/_ssl.c                  | src/_ssl.c                  |   
| Modules/_ssl.h                  | src/_ssl.h                  |  
| Modules/_ssl_data.h             | src/_ssl_data.h             | 
| Modules/_ssl_data_31.h          | src/_ssl_data_31.h          |
| Modules/_ssl_data_111.h         | src/_ssl_data_111.h         |
| Modules/_ssl_data_300.h         | src/_ssl_data_300.h         |
| Modules/addrinfo.h              | src/addrinfo.h              |
| Modules/selectmodule.c          | src/selectmodule.c          |
| Modules/socketmodule.c          | src/socketmodule.cpp        |
| Lib/test/certdata               | tests/certdata              |
| Lib/test/ssl_servers.py         | tests/ssl_servers.py        |
| Lib/test/test_select.py         | tests/test_select.py        |
| Lib/test/test_socket.py         | tests/test_socket.py        |
| Lib/test/test_ssl.py            | tests/test_ssl.py           |

Base cpython branch: 3.12
Base cpython release: v3.12.3 (f6650f9ad73359051f3e558c2431a109bc016664)
