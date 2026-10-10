# VGS Native Playback Bridge — Proprietary Licence Notice

Copyright © 2026 Víctor M. Feliz. All rights reserved.

The VGS native playback bridge (`vgsbridge`) and its incorporated VGS Decoder
are proprietary, closed-source components.

**Purpose.** `vgsbridge` is the separately supplied native library through which
host integrations play VGS captures, including integrations distributed under
the GNU GPL or other free-software licences, such as the Blender add-on. It is
called only through its public C interface (`include/vgsbridge.h`); those
integrations do not contain its source code, and it is not part of their
Corresponding Source. Whether a particular way of combining it with a given
host or plugin is permitted is governed by that host's and that plugin's own
licences, and, for the Blender add-on, by the additional permission in the
add-on's licence; this notice does not itself decide that question. Supplied native binaries and public
interface headers are made available free of charge, including for commercial
integration, under [the VGS Decoder licence](../../decoder/LICENSE.md).
Redistribution is permitted within end-user products such as the Blender and
Houdini plugins, subject to that licence; standalone SDK redistribution and
modification of the supplied native binaries require separate permission.

The bridge's proprietary implementation source code is not example source code
licensed by the decoder's example-code grant. Access to it in this repository
does not grant permission to use, modify, compile, publish or distribute it;
those acts require separate written authorisation, subject to mandatory
applicable law.

The GPL licence and additional permission for the Blender Python add-on do not
relicense this native component or grant access to its proprietary source code.
The VGS Decoder licence's warranty and liability provisions apply to the
supplied native binaries and public headers.

**Víctor M. Feliz**\
The4DScanner | ScanMeNow\
victor.feliz@the4dscanner.com
