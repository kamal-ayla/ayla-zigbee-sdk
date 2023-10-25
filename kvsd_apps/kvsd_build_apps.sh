#!/bin/bash

help() {
    echo "Usage: $0 <cmake_c_flags_path> <cmake_toolchain_flags_path> <cmake_includes.cmake> <cmake_library_path>"
    exit 1
}

install_hls_sdk() {
    cmake_c_flags=$1
    cmake_toolchain_flags=$2
    cmake_includes=$3
    cmake_library_path=$4
    ayla_install_dir=$5
	#git clone --recursive https://github.com/awslabs/amazon-kinesis-video-streams-producer-sdk-cpp.git
	mkdir -p amazon-kinesis-video-streams-producer-sdk-cpp/build
	cd amazon-kinesis-video-streams-producer-sdk-cpp/build

	echo "======== CMake run cmd: ========="
	echo "cmake -DBUILD_OPENSSL_PLATFORM=linux-generic32 -DBUILD_LOG4CPLUS_HOST=arm-linux -DBUILD_GSTREAMER_PLUGIN=TRUE $cmake_toolchain_flags -DCMAKE_C_FLAGS='$cmake_c_flags' -DGST_APP_INCLUDE_DIRS='$cmake_includes' .."

	cmake -DBUILD_OPENSSL_PLATFORM=linux-generic32 \
	    -DBUILD_LOG4CPLUS_HOST=arm-linux \
	    -DBUILD_GSTREAMER_PLUGIN=TRUE \
	    $cmake_toolchain_flags \
	    -DCMAKE_C_FLAGS="$cmake_c_flags" \
	    -DCMAKE_CXX_FLAGS="$cmake_c_flags" \
        -DCMAKE_LIBRARY_PATH=$cmake_library_path \
	    -DGST_APP_INCLUDE_DIRS="$cmake_includes" \
	    ..
	if [ $? -ne 0 ]; then exit 1; fi
	make -j
	if [ $? -ne 0 ]; then exit 1; fi

    # Install
    cd ..
	cp -av build/libKinesisVideoProducer.so $ayla_install_dir/usr/lib
	cp -av build/libgstkvssink.so $ayla_install_dir/usr/lib/gstreamer-1.0
    cp -av open-source/local/lib/liblog4cplus*.so* $ayla_install_dir/usr/lib
}

install_kvsd_stream_webrtc() {
    cmake_c_flags=$1
    cmake_toolchain_flags=$2
    cmake_includes=$3
    cmake_library_path=$4
    ayla_install_dir=$5
	#git clone --recursive https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c.git
	mkdir -p amazon-kinesis-video-streams-webrtc-sdk-c/build
	cd amazon-kinesis-video-streams-webrtc-sdk-c/build
	#git checkout b820e2ea3d28c670da2fbc789face742cb949ef1
	#git apply $ayla_src_dir/kvs-webrtc-slave.diff
	cmake -DBUILD_OPENSSL=TRUE \
	        -DBUILD_OPENSSL_PLATFORM=linux-generic32 \
	        -DBUILD_LIBSRTP_HOST_PLATFORM=x86_64-unknown-linux-gnu \
	        -DBUILD_LIBSRTP_DESTINATION_PLATFORM=arm-buildroot-linux-gnueabi \
	        $cmake_toolchain_flags \
            -DCMAKE_C_FLAGS="$cmake_c_flags" \
            -DCMAKE_CXX_FLAGS="$cmake_c_flags" \
            -DCMAKE_LIBRARY_PATH=$cmake_library_path \
            -DGST_APP_INCLUDE_DIRS="$cmake_includes" \
            ..
	if [ $? -ne 0 ]; then exit 1; fi
	make -j
	if [ $? -ne 0 ]; then exit 1; fi

    # Install
    cd ..
	cp -av build/*.so $ayla_install_dir/usr/lib
    cp -av build/samples/kvsWebrtcClientMasterGstSample $ayla_install_dir/usr/bin/kvsd_stream_webrtc
    cp -av open-source/lib/libcrypto*.so* $ayla_install_dir/usr/lib
    cp -av open-source/lib/libssl*.so* $ayla_install_dir/usr/lib
    cp -av open-source/lib/engines* $ayla_install_dir/usr/lib
}

install_kvsd_stream_master() {
    cmake_c_flags=$1
    cmake_toolchain_flags=$2
    cmake_includes=$3
    cmake_library_path=$4
    ayla_install_dir=$5
    mkdir kvsd_stream_master/build
    cd kvsd_stream_master/build
    cmake \
        $cmake_toolchain_flags \
        -DCMAKE_C_FLAGS="$cmake_c_flags" \
        -DCMAKE_CXX_FLAGS="$cmake_c_flags" \
        -DCMAKE_LIBRARY_PATH=$cmake_library_path \
        ..
    if [ $? -ne 0 ]; then exit 1; fi
    make -j
    if [ $? -ne 0 ]; then exit 1; fi

    # Install
    cp -av kvsd_stream_master $ayla_install_dir/usr/bin
}

install_kvsd_stream_hls() {
    cmake_c_flags=$1
    cmake_toolchain_flags=$2
    cmake_includes=$3
    cmake_library_path=$4
    ayla_install_dir=$5
    mkdir kvsd_stream_hls/build
    cd kvsd_stream_hls/build
    cmake \
        $cmake_toolchain_flags \
        -DCMAKE_C_FLAGS="$cmake_c_flags" \
        -DCMAKE_CXX_FLAGS="$cmake_c_flags" \
        -DCMAKE_LIBRARY_PATH=$cmake_library_path \
        ..
    if [ $? -ne 0 ]; then exit 1; fi
    make -j
    if [ $? -ne 0 ]; then exit 1; fi

    # Install
    cp -av kvsd_stream_hls $ayla_install_dir/usr/bin
}

# Loop through all .so.* files
so_link_fix() {
    DIR=$1
    if [[ "$DIR" == "" ]]; then
        return
    fi

    CURR=$PWD
    cd $DIR
    for file in *.so.*; do
        # Extract the base name without version
        base_name=$(echo "$file" | sed 's/\(.*\)\..*/\1/')

        # Create a symbolic link
        ln -sf "$file" "$base_name"
    done
    cd $CURR
}


#####################  Main
if [ $# -ne 4 ]; then
    help
    exit 1
fi

cmake_c_flags=$(cat $1)
cmake_toolchain_flags=$(cat $2)
cmake_includes=$(cat $3)
cmake_library_path=$(cat $4)

echo "=== Cmake C flags: $cmake_c_flags"
echo "=== Cmake Toolchain flags: $cmake_toolchain_flags"


DIR=$PWD
ayla_install_dir=$PWD/rootfs
echo "=== Install rootfs path: '$ayla_install_dir'"

if [[ -d "$ayla_install_dir" ]]; then
    echo "Rootfs for KVSD apps already exists: '$ayla_install_dir'. Deleting."
    rm -rf $ayla_install_dir
fi
mkdir -p $ayla_install_dir
mkdir -p $ayla_install_dir/usr/lib
mkdir -p $ayla_install_dir/usr/lib/gstreamer-1.0
mkdir -p $ayla_install_dir/usr/bin

cd $DIR
install_hls_sdk "$cmake_c_flags" "$cmake_toolchain_flags" "$cmake_includes" "$cmake_library_path" "$ayla_install_dir"
 
cd $DIR
install_kvsd_stream_webrtc "$cmake_c_flags" "$cmake_toolchain_flags" "$cmake_includes" "$cmake_library_path" "$ayla_install_dir"
 
cd $DIR
install_kvsd_stream_master "$cmake_c_flags" "$cmake_toolchain_flags" "$cmake_includes" "$cmake_library_path" "$ayla_install_dir"
 
cd $DIR
install_kvsd_stream_hls "$cmake_c_flags" "$cmake_toolchain_flags" "$cmake_includes" "$cmake_library_path" "$ayla_install_dir"

so_link_fix $ayla_install_dir/usr/lib

exit 0
