

The reason this exist is because many abstraction libraries and/or applications don't support and don't intend to support global keyboard input. Which for a non-zero amount of application is a necessary thing.

### How will this application work.
 This application will work as a server where it listen to global input then sends that out to a local
 domain socket for clients to feast on. I would like to note very loudly this is not intended for malicious
 purposes. 

 The intended use is that I can drop a client into any of my projects very quickly and get global
 input without a lot of setup. 
 
 > [!WARNING]
 > This application is currently not intended for actual use.
 > At the moment, the server does not implement authentication, passphrase verification, or client
 > authorization. It simply forwards captured input events to a predefined socket path.
 > Anyone with access to that socket can receive the forwarded input events.
 > Do not use this application in environments where sensitive input could be exposed.

This currently Makes use of x86 intrinsics to track performance, so currently is not compatible on arm based CPUs

### Currently supported platforms.
  Linux.

### future supported platforms.
 Windows.
 MacOS.

# Building

 The build script would look something like this.
 As I haven't gotten around to working on the other platform layers
 I haven't introduced CMake or a proper build system in as not to add unneeded complexity.

 ```
  gcc -o Test.out src/main.c 
```
