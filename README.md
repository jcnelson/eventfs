eventfs
=======

Eventfs is a specialized userspace filesystem where each directory serves as process-owned message queue.  Its semantics differ from POSIX in the following ways:

* Every non-empty directory has a `head` and `tail` symlink.
  * The `head` symlink always points to the *oldest* file in the directory.
  * The `tail` symlink always points to the *newest* file in the directory.
* Unlinking `head` unlinks the file pointed to by `head`.  Similarly, unlinking `tail` unlinks the file that `tail` points to.
  * In both cases, the symlink is atomically re-pointed to the next-oldest or next-newest file, respectively.
* Each directory shares fate with the process that created it.  If the creator process dies, the directory and its contents cease to exist.
* There are no nested directories.

Sample Use-Cases
----------------
* Give [libudev-compat](https://github.com/jcnelson/vdev) clients a way to receive device events while guaranteeing that they do not leave behind any residual state once they exit.
* Provide namespaceable event channels for sending messages between containers.

Example
-------

Here is a sample execution trace, where a Python script creates a directory called `demo` and goes to sleep.  Other processes create files in `demo`, and eventfs ensures that the directory's `head` and `tail` symlinks always point to the oldest and newest files.  Once the `demo` directory is empty, the `head` and `tail` symlinks vanish.  Once the Python script exits, the `demo` directory vanishes.

```
$ cat ./demo.py
#!/usr/bin/env python

import os
import time 
import sys

os.mkdir( sys.argv[1] )
time.sleep(10000)

$ mkdir events 
$ ./eventfs events
$ ls -l events
total 0
$ ./demo.py events/demo &
[1] 4014
$ ls -l events/demo
total 0
$ echo "message text" > events/demo/msg
$ ls -l events/demo 
total 0
lrwxrwxrwx 1 root root  3 Sep  4 12:11 head -> msg
-rw-r--r-- 1 jude jude 26 Sep  4 12:11 msg
lrwxrwxrwx 1 root root  3 Sep  4 12:11 tail -> msg
$ echo "message text 2" > events/demo/msg2
$ ls -l events/demo
total 0
lrwxrwxrwx 1 root root  3 Sep  4 12:11 head -> msg
-rw-r--r-- 1 jude jude 30 Sep  4 12:12 msg
-rw-r--r-- 1 jude jude 30 Sep  4 12:12 msg2
lrwxrwxrwx 1 root root  4 Sep  4 12:11 tail -> msg2
$ rm events/demo/head
$ ls -l events/demo 
total 0
lrwxrwxrwx 1 root root  4 Sep  4 12:14 head -> msg2
-rw-r--r-- 1 jude jude 30 Sep  4 12:12 msg2
lrwxrwxrwx 1 root root  4 Sep  4 12:11 tail -> msg2
$ rm events/demo/tail
$ ls -l events/demo
total 0
$ fg
./demo.py tmp/events
^CTraceback (most recent call last):
  File "./demo.py", line 8, in <module>
    time.sleep(10000)
KeyboardInterrupt
$ ls -l events
total 0
$ fusermount -u events
```

Dependencies
------------
* [fskit](https://github.com/jcnelson/fskit)
* [libpstat](https://github.com/jcnelson/libpstat)

Building
---------

To build:

        $ make

Installing
----------

To install:

        $ sudo make install

By default, eventfs will be installed to `/usr/local/bin`.  You may set `PREFIX` to control the installation directory, and `DESTDIR` to set an alternate installation root.

Running
-------

To run:

        $ ./eventfs /path/to/mountpoint

It takes FUSE arguments like -f for "foreground", etc.  See `fuse(8).`