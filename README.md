NV-RAM support for QEMU. 

To save RAM please use:

    dump-guest-memory file.name

Then, to use this image, add 

    qemu-system-i386 <..> -nvram-path file.name
