# carbon-io
[![license](https://img.shields.io/badge/License-PSF%202.0-blue)](LICENSE.txt)

## Overview

Tasklet-blocking sockets for use with carbon-scheduler. This module exists in order to allow for tasklet-friendly asynchronous IO with an interface as close to the standard Python socket module as possible. Tasklet-blocking behavior is supported for TCP and UDP connections only. Other socket types will exhibit the standard blocking behavior. Some functionality to support sending / receiving of machoNet packets has been added.

This project provides modified versions of the C part of the Python [socket](https://docs.python.org/3.12/library/socket.html) and [ssl](https://docs.python.org/3.12/library/ssl.html) modules, as well as an unmodified version of the [select](https://docs.python.org/3.12/library/select.html) module. The modules have the prefix "_carbon" in order to distinguish them from their unmodified counterparts. In order to use them with the standard socket and ssl modules, they need to be injected into `sys.modules` with the carbon prefix removed.

## 🛠️ Building

Build using provided `CMakeLists` in the repository root using the provided presets.

### Current requirements for documentation generation

1. Documentation can be built on either Windows or macOS
2. Doxygen 1.12.0 (or higher) installed on build machine

### Building the documentation

Documentation is built using the following defaults:
- TeamCity build agents: ON
- Local dev build: OFF

To override the default documentation build settings, turn the CMake option `BUILD_DOCUMENTATION` to `ON/OFF`.

Building the `INSTALL` target will build all documentation and place it in the path specified by `CMAKE_INSTALL_PREFIX`.

Entry point for the documentation is `documentation/index.html`.

Documentation can be built using either .rst (restructuredText) or .md (markdown) files as sources.

## 🤝 Contributing

Contribution follows the standard GIT PR model.

By submitting a pull request or otherwise contributing to this project, you agree to license your contribution under the PSF2.0 Licence, and you confirm that you have the right to do so.

## 📄 License and Legal Notices

Carbon.io

Copyright (c) 2001 Python Software Foundation; All Rights Reserved

Copyright (c) 2026 CCP Games

This software is a derivative work incorporating Python as modified by CCP Games.

This software provides low-level networking functionality.

Trademark Notice: CCP Games is a trademark of CCP ehf.

This project is licensed under the [PSF-2.0 LICENSE](LICENSE.txt); see [NOTICE.md](NOTICE.md) for the attribution and how CCP Games distributes CARBON.IO under those terms. Nothing in the [PSF-2.0 License](LICENSE.txt) grants any rights to CCP Games' trademarks or game content.
