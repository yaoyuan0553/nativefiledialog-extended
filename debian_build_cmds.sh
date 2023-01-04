docker run --rm \
    -v "$(pwd)":"$(pwd)" \
    -w "$(pwd)" \
    -e HOME -e USER -e USERID=$(id -u) -u $(id -u):$(id -g) \
    -it linuxez0553/i386-wine_build:debian-11 \
    /bin/bash

# static library
cmake -DCMAKE_BUILD_TYPE=Release -DNFD_PORTAL=ON -DNFD_TARGET_ARCHITECTURE=x86 -S . -B cmake-debian-build-release

cmake --build cmake-debian-build-release --target all -j 36

# shared library
cmake -DCMAKE_BUILD_TYPE=Release -DNFD_PORTAL=ON -DNFD_TARGET_ARCHITECTURE=x86 -DBUILD_SHARED_LIBS=ON -S . -B cmake-debian-build-release-shared

cmake --build cmake-debian-build-release-shared --target all -j 36
