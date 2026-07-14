# carbon-io

[English](./README.md) | [简体中文](./README.zh-CN.md)

[![license](https://img.shields.io/badge/License-PSF%202.0-blue)](LICENSE.txt)

## 概述

为配合 carbon-scheduler 使用而设计的 tasklet 阻塞式套接字(socket)。本模块的目的是提供对 tasklet 友好的异步 IO,同时使接口尽可能贴近标准 Python socket 模块。tasklet 阻塞行为仅支持 TCP 和 UDP 连接,其他类型的套接字将表现出标准的阻塞行为。此外还加入了一些用于收发 machoNet 数据包的功能。

本项目提供了 Python [socket](https://docs.python.org/3.12/library/socket.html) 和 [ssl](https://docs.python.org/3.12/library/ssl.html) 模块 C 语言部分的修改版本,以及未经修改的 [select](https://docs.python.org/3.12/library/select.html) 模块。这些模块带有 "_carbon" 前缀,以便与未修改的原版模块区分。若要将它们与标准 socket 和 ssl 模块配合使用,需要在去掉 carbon 前缀后将其注入 `sys.modules`。

## 🛠️ 构建

使用仓库根目录下提供的 `CMakeLists` 及提供的预设(presets)进行构建。

### 当前文档生成的环境要求

1. 文档可以在 Windows 或 macOS 上构建
2. 构建机器上需安装 Doxygen 1.12.0(或更高版本)

### 构建文档

文档构建的默认设置如下:
- TeamCity 构建代理:开启(ON)
- 本地开发构建:关闭(OFF)

如需覆盖默认的文档构建设置,请将 CMake 选项 `BUILD_DOCUMENTATION` 设为 `ON/OFF`。

构建 `INSTALL` 目标会构建全部文档,并将其放置在 `CMAKE_INSTALL_PREFIX` 指定的路径下。

文档的入口文件为 `documentation/index.html`。

文档源文件可以使用 .rst(reStructuredText)或 .md(Markdown)格式编写。

## 🤝 参与贡献

贡献遵循标准的 Git PR(Pull Request)流程。

提交 Pull Request 或以其他方式为本项目做出贡献,即表示您同意以 PSF 2.0 许可证授权您的贡献内容,并确认您拥有这样做的权利。

## 📄 许可证与法律声明

Carbon.io

版权所有 (c) 2001 Python Software Foundation;保留所有权利。

版权所有 (c) 2026 CCP Games

本软件是包含由 CCP Games 修改的 Python 的衍生作品。

本软件提供底层网络功能。

商标声明:CCP Games 是 CCP ehf. 的商标。

本项目基于 [PSF-2.0 许可证](LICENSE.txt)授权;有关署名信息以及 CCP Games 如何依据上述条款分发 CARBON.IO,请参见 [NOTICE.md](NOTICE.md)。[PSF-2.0 许可证](LICENSE.txt)中的任何条款均不授予任何关于 CCP Games 商标或游戏内容的权利。
