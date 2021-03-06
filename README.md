asymmetricfs - An Asymmetric, Encrypting Filesystem for Userspace

(c) 2013-2014 - Chris Kennelly (chris@ckennelly.com)

[![Build Status](https://travis-ci.org/ckennelly/asymmetricfs.svg?branch=master)](https://travis-ci.org/ckennelly/asymmetricfs)

Overview
========

`asymmetricfs` exposes an encrypting file system in userspace with FUSE.  Encryption is performed with `gpg`, as to leverage existing public/private key infrastructure.

In write-only mode, the mounted filesystem permits appending to existing files and creating new files.  Other filesystem operations, such as (complete) truncation and deletion are permitted as well.

In read/write mode, stored files are decrypted on-the-fly with `gpg` as needed.  Arbitrary file modification is possible.

Building
========

`asymmetricfs` depends on boost and CMake at compile time.  Google Test is pulled-in and built via a git submodule.  As `asymmetricfs` uses the `vmsplice` syscall, a modern (3.8.0 or higher) version of Valgrind must be available.

At runtime, `gpg` must be available in the path.

Limitations
===========

For security-sensitive applications, the functionality of `asymmetricfs` may easily be insufficient.  This filesystem primarily allows weakly trusted computers to handle sensitive, transient data while reducing the unencrypted time-in-flight.  If a system is compromised before data is flushed to disk (and ultimately any remnant data cleared from system memory), an attacker will have access to the plaintext contents of the filesystem.

Metadata is not protected by `asymmetricfs`.

As with any use of encryption, keeping good backups is crucial.
