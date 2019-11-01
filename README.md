epoll-shim
==========

This is a small library that implements epoll on top of kqueue.
It has been successfully used to port libinput, libevdev, Wayland and more
software to FreeBSD: https://www.freshports.org/devel/libepoll-shim/

It may be useful for porting other software that uses epoll as well.

There are some tests inside `test/`. They should also compile under Linux and
can be used to verify proper epoll behavior.

However, this library contains some very ugly hacks and workarounds. For
example:
 - When using timerfd, signalfd or eventfd, `read`, `write` and `close` are
   redefined as macros to internal helper functions. This is needed as there
   is some internal context that has to be free'd properly. This means that
   you shouldn't create a timerfd/signalfd in one part of a program and close
   it in a different part where `sys/timerfd.h` isn't included. The context
   would leak. Luckily, software such as `libinput` behaves very nicely and
   puts all timerfd related code in a single source file.
 - There is exactly one static int reserved for fds that can be polled but are
   not supported by kqueue under FreeBSD. This includes graphics or sound
   devices under `/dev`. You can only have one of them throughout all epoll
   instances in your process!


Installation
------------

Run the following commands to build libepoll-shim:

    $ mkdir build
    $ cd build
    $ cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
    $ cmake --build .

To run the tests:

    $ ctest --output-on-failure

To install (as root):

    # cmake --build . --target install
