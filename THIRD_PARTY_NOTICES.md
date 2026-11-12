# Third-party notices

Briks (a fork of Risk-P2P) is licensed under the GNU Affero General Public License v3.0 as described in the root `LICENSE` file. The components below are third-party dependencies used by the audio pipeline and retain their own licenses.

## RNNoise / Xiph.Org

- Component: RNNoise
- License: BSD 3-Clause
- Purpose in Briks: neural-network noise suppression core used by the RNNoise WebAssembly build.
- Copyright holders include Jean-Marc Valin, Amazon, Mozilla, Xiph.Org Foundation and Mark Borgerding.
- Full license text: `LICENSES/RNNoise-BSD-3-Clause.txt`
- Upstream: https://github.com/xiph/rnnoise

## @shiguredo/rnnoise-wasm 2022.2.0

- Component: WebAssembly build/glue for RNNoise
- License: Apache License 2.0
- Copyright 2021-2021, Takeru Ohta (Original Author)
- Copyright 2021-2021, Shiguredo Inc.
- Full license text: `LICENSES/Shiguredo-RNNoise-WASM-Apache-2.0.txt`
- Upstream: https://github.com/shiguredo/rnnoise-wasm

The generated RNNoise WebAssembly binary remains subject to the RNNoise/Xiph.Org license noted above. The `@sapphi-red/web-noise-suppressor` 0.3.5 build copies the RNNoise WebAssembly artifact from `@shiguredo/rnnoise-wasm` into its distributed package.

## @sapphi-red/web-noise-suppressor 0.3.5

- Component: Web Audio API / AudioWorklet wrapper used by Briks to run RNNoise off the renderer main thread.
- License: MIT
- Copyright (c) 2022 翠 / green
- Full license text: `LICENSES/Web-Noise-Suppressor-MIT.txt`
- Upstream: https://github.com/sapphi-red/web-noise-suppressor

These notices and the complete third-party license texts are distributed with packaged Briks desktop builds.

## livekit-client

- Component: LiveKit JavaScript client SDK (SFU transport)
- License: Apache License 2.0
- Upstream: https://github.com/livekit/client-sdk-js

## wasapi (Rust crate)

- Component: Windows WASAPI bindings used for per-process audio loopback
- License: MIT
- Upstream: https://github.com/HEnquist/wasapi-rs

## libvpx (VP8/VP9) — BSD-3-Clause
Briks 1.4.4+ liga estaticamente a libvpx 1.14.1 (Copyright (c) 2010, The WebM Project authors). Licença BSD de 3 cláusulas e concessão de patentes: https://github.com/webmproject/libvpx/blob/v1.14.1/LICENSE
