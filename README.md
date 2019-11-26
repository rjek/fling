fling -- transfer data from stdin over network to destination quickly
-----------------------------------------------------------------------------

fling transfers data quickly over a trusted network. It does not
encrypt the data.

You need to run fling on both ends of the transfer. Run it first on
the receiver:

    fling -r 12756 > file.dat

And then on the sender:

    fling other.host.address 12756 < file.dat

Note that sender reads the data from its stdin, receiver writes it to
stdout.


To build
-----------------------------------------------------------------------------

Get source code from <https://github.com/rjek/fling> and build it on
Linux by running the `make` command. Copy the `fling` binary to the
target host, or build a fresh copy there.


Legalese
-----------------------------------------------------------------------------

Licence: MIT <https://opensource.org/licenses/MIT>

Copyright 2019  Rob Kendrick

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
