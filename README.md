fling -- transfer data from stdin over network to destination quickly
-----------------------------------------------------------------------------

fling transfers data quickly over a trusted network. It does not
encrypt the data. It tries to avoid copying data between kernel and
userspace where it can; you will see the most improvement over other
tools like netcat on systems with low memory bandwidth.  fling is extremely
Linux-specific, as almost all the optimisations used beyond what other
similar tools do are non-portable.

You need to run fling on both ends of the transfer. Run it first on
the receiver:

    fling -r 12756 > file.dat

And then on the sender:

    fling other.host.address 12756 < file.dat

Note that sender reads the data from its stdin, receiver writes it to
stdout.


Using ssh
-----------------------------------------------------------------------------

You can have fling run fling on the remote end over ssh. This avoids
having to start it manually, in a different terminal. You still need
the fling executable on the remote end. If it's not in the PATH, you
can set the `FLING_REMOTE_EXE` environment variable to specify where
it is.

    FLING_REMOTE_EXE="/usr/local/bin/fling" ./fling other.host:data < data

If your ssh command is not called ssh, you can set its name using the
`FLING_SSH` envionment variable.

The ssh support is currently experimental.

To build
-----------------------------------------------------------------------------

Get source code from <https://github.com/rjek/fling> and build it on
Linux by running the `make` command. Copy the `fling` binary to the
target host, or build a fresh copy there.


Wire protocol
-----------------------------------------------------------------------------

The wire protocol, i.e., what gets transferred over the TCP connection
between two fling processes, is really simple. It is just the bytes of
the input file to be transferred, no metadata, no checksum, and only
one file allowed. This is to keep things as simple and fast as
possible.


Legalese
-----------------------------------------------------------------------------

Licence: MIT <https://opensource.org/licenses/MIT>

Copyright 2019 Codethink Ltd.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
