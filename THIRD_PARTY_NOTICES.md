# Third-party notices

## Redistributed

- **Sound Open Firmware binary and topology** (`fw/`) — Intel Corporation and contributors.
  Licensed under the SOF firmware release licence (BSD-3-Clause and others): see [fw/LICENCE.Intel](fw/LICENCE.Intel) and [fw/README.md](fw/README.md).

## Referenced

The driver code was written for this project; no source files from the projects below are included.
Their hardware sequences and the firmware IPC ABI were reimplemented, so the BSD-3-Clause notice below is
reproduced for any portion that may be considered derived from them.

- **Sound Open Firmware** (https://github.com/thesofproject/sof) — IPC3 message layouts and command
  numbers (`src/include/ipc/*.h`, BSD-3-Clause). The packed structures in `driver/jslaudio.cpp` mirror this
  binary ABI so that the firmware can understand them.
- **Linux kernel SOF driver** (`sound/soc/sof/intel/hda-dsp.c`, `hda-loader.c`, `hda-stream.c`;
  dual-licensed GPL-2.0-only OR BSD-3-Clause, used here under BSD-3-Clause) — DSP core power/reset, the
  code-loader firmware download and the PCM trigger ordering.
- **Linux kernel Intel Jasper Lake pinctrl driver** (`drivers/pinctrl/intel/pinctrl-jasperlake.c`) — used only as
  a source of hardware facts (pad numbering and GPIO base mapping); no code was taken from it.

### BSD-3-Clause notice (Sound Open Firmware IPC ABI, Linux SOF Intel HDA driver)

```
Copyright(c) 2018 Intel Corporation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## Build-time only (not redistributed)

- Windows Driver Kit / Windows SDK headers and libraries (Microsoft) are required to build but are not part
  of this repository.

## Trademarks

Intel, Acer, Chromebook, Realtek and Windows are trademarks of their respective owners. This project is not
affiliated with or endorsed by any of them.
