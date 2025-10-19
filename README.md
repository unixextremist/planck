# tinygit

- a minimal git implementation in c
- all it does is clone repos

# use cases

- embedded systems
- freebsd style ports systems (ex macports haikuports xbps-src derive-ports)

# compiling

you need 

- libcurl
- libcurl-devel
- libssl
- the internet

``gcc -o tinygit tinygit.c -lcurl``


