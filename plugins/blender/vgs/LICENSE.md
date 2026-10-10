# VGS Blender Add-on — Licensing Notice

Copyright © 2026 Víctor M. Feliz.

The VGS Blender add-on's Python code is made available **free of charge** under
the **GNU General Public License, version 3 or, at your option, any later
version** (GPL-3.0-or-later). The complete GPLv3 text is supplied in
[LICENSE.txt](LICENSE.txt). You may use, copy, modify and redistribute the
GPL-covered code in accordance with that licence. The GPL also permits
redistributors to charge for copies or services.

The native playback library in `bin/` is a separate **proprietary, closed-source
component** supplied free of charge under [bin/LICENSE.md](bin/LICENSE.md), the
VGS Decoder licence. The GPL notice for the Python code does not relicense that
library or grant access to its source code. Its integration and redistribution
permissions and restrictions must be followed separately.

## Native decoder architecture and separation

The VGS native playback library is an independently developed, proprietary
decoding component. It implements VGS capture decoding through its own native
C interface and does not depend on Blender's Python API or internal rendering
functionality.

The GPL-licensed Python add-on loads the native library through `ctypes`,
obtains decoded Gaussian Splatting data through that interface, and transfers
those data into Blender's native data structures for rendering.

The native library does not incorporate Blender code or link against Blender
libraries. Its decoding functionality is independent of Blender and may be
used in other applications under the separate VGS Decoder licence.

The separation of these components does not modify the licensing obligations
applicable to Blender or any third-party software. The additional permission
below applies only to code for which the granting copyright holder has the
necessary rights.

## 1. Additional permission under GPLv3 section 7

For the add-on code in which Víctor M. Feliz holds the relevant copyright,
he grants additional permission under section 7 of GPLv3 to link or combine
that code, including modified versions, with the separately supplied
**VGS native playback bridge (`vgsblender.dll`, `vgsblender.so` or
`vgsblender.dylib`) and its incorporated VGS Decoder**, through its native
C interface, and to convey the resulting combination while that native
component remains under the VGS Decoder licence.

When exercising this permission, comply with the GPL for all GPL-covered parts
and with the VGS Decoder licence for the native component. Solely as a result of
the linking or combination authorised here, you are not required to include the
native component's proprietary source code in the add-on's Corresponding Source
or to license that component under the GPL. You must still provide the
Corresponding Source required by the GPL for the GPL-covered parts, including
your modifications and the relevant scripts needed to build or install them.

This permission grants no additional right to modify, publish or redistribute
the native component outside its own licence. It does not extend to other
proprietary libraries or waive obligations for code owned by Blender or other
third parties. It applies only to code whose rights holder grants it.

As permitted by GPLv3 section 7, you may remove this additional permission from
your copy. Contributors may extend it to their own contributions when they have
authority to do so. Preserve this notice when relying on the permission.

## 2. No warranty and limitation of liability

The add-on is supplied **AS IS**, without warranty, to the fullest extent
permitted by applicable law. GPLv3 sections **15, 16 and 17** govern the absence
of warranty and limitation of liability for its GPL-covered code. The separate
VGS Decoder licence governs warranty and liability for the native component.

Within those terms and the limits of applicable law, the rights holders and
contributors accept no liability for losses caused by use or inability to use
the software, including lost or corrupted data, lost profits, production delays
or incompatibility with Blender, hardware or other software. No support,
maintenance, updates or compatibility guarantee is promised unless separately
agreed in writing. Nothing restricts rights or liability that cannot lawfully
be restricted.

## 3. Other rights and contact

The licences do not transfer ownership of captures or other user materials.
Names, logos and trademarks remain subject to their respective owners' rights;
no endorsement by Blender or the Blender Foundation is implied.

This add-on's free availability does not grant permission to use VGS Encoder
or VGS Editor, which require their own written commercial licence agreements.

**Víctor M. Feliz**\
The4DScanner | ScanMeNow\
victor.feliz@the4dscanner.com
