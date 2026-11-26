ClientLab is licensed under the MIT license; see LICENSE.txt.

The launcher distributes .NET, Avalonia, CefGlue, Chromium Embedded Framework,
CommunityToolkit.Mvvm, Iced, SkiaSharp, HarfBuzzSharp, Wasmtime and their transitive dependencies.
Release builds also carry the Emscripten sysroot headers (Emscripten and musl) used to build the runtime module.
Their respective licenses apply. Browser resource files and notices supplied by the
upstream packages are retained in the extracted application directory.

Upstream license and source locations:

- .NET: https://github.com/dotnet/runtime/blob/main/LICENSE.TXT
- Avalonia: https://github.com/AvaloniaUI/Avalonia/blob/master/licence.md
- CefGlue: https://github.com/OutSystems/CefGlue/blob/main/LICENSE
- Chromium Embedded Framework: https://github.com/chromiumembedded/cef/blob/master/LICENSE.txt
- Chromium third-party notices: chrome://credits in the bundled browser
- CommunityToolkit: https://github.com/CommunityToolkit/dotnet/blob/main/License.md
- Iced: https://github.com/icedland/iced/blob/master/LICENSE.txt
- SkiaSharp: https://github.com/mono/SkiaSharp/blob/main/LICENSE.md
- HarfBuzz: https://github.com/harfbuzz/harfbuzz/blob/main/COPYING
- Wasmtime: https://github.com/bytecodealliance/wasmtime-dotnet/blob/main/LICENSE
- Emscripten: https://github.com/emscripten-core/emscripten/blob/main/LICENSE
- musl: https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT
- YoWASP clang (downloaded on demand, not distributed): https://github.com/YoWASP/clang and LLVM's
  https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT

No NosTale executable, game asset, generated client code, account data or captured
Gameforge identity is distributed. Game and build-tool downloads occur locally when
the user requests them and remain subject to the upstream terms and licenses.

Packet capture signatures adapted from NosCore.DeveloperTools.Hook/Signatures.cs
(https://github.com/NosCoreIO/NosCore.DeveloperTools), using the documented login
null-check constraint from docs/finding-hooks.md.

MIT License

Copyright (c) 2026 NosCoreIO

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

