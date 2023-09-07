#!/bin/bash

help() {
    echo "Usage: $0 <build_dir> <kvsd_src_dir> <cmake_defines>"
    exit 1
}

install_hls_sdk() {
	cd $working_dir
	git clone --recursive https://github.com/awslabs/amazon-kinesis-video-streams-producer-sdk-cpp.git
	mkdir -p amazon-kinesis-video-streams-producer-sdk-cpp/build
	cd amazon-kinesis-video-streams-producer-sdk-cpp/build
	cmake -DBUILD_GSTREAMER_PLUGIN=TRUE ..
	if [ $? -ne 0 ]; then print_msg "Failed."; exit 1; fi
	make -j
	if [ $? -ne 0 ]; then print_msg "Failed."; exit 1; fi
# 	mkdir -p $ayla_install_dir/lib/kvsd
# 	cp -av ../open-source/local/lib/* $ayla_install_dir/lib/kvsd
# 	cp -av *.so $ayla_install_dir/lib/kvsd
}

install_kvsd_stream_webrtc() {
	cd $working_dir
	git clone --recursive https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c.git
	mkdir -p amazon-kinesis-video-streams-webrtc-sdk-c/build
	cd amazon-kinesis-video-streams-webrtc-sdk-c/
	git checkout b820e2ea3d28c670da2fbc789face742cb949ef1
	git apply $ayla_src_dir/kvs-webrtc-slave.diff
	cd build
	cmake ..
	if [ $? -ne 0 ]; then print_msg "Failed."; exit 1; fi
	make -j
	if [ $? -ne 0 ]; then print_msg "Failed."; exit 1; fi
# 	cp -fv samples/kvsWebrtcClientMasterGstSample $ayla_install_dir/bin/kvsd_stream_webrtc
}

install_kvsd_stream_master() {
    cd $working_dir
    cp -avf $ayla_src_dir/kvsd_stream_master $working_dir
    mkdir kvsd_stream_master/build
    cd kvsd_stream_master/build
    cmake ..
    if [ $? -ne 0 ]; then print_msg "Failed."; exit 1; fi
    make -j
    if [ $? -ne 0 ]; then print_msg "Failed."; exit 1; fi
#     cp -fv kvsd_stream_master $ayla_install_dir/bin
}

install_kvsd_stream_hls() {
    cmake_defines=$1
    cd $working_dir
    cp -avf $ayla_src_dir/kvsd_stream_hls $working_dir
    mkdir kvsd_stream_hls/build
    cd kvsd_stream_hls/build
    cmake "$cmake_defines" ..
    if [ $? -ne 0 ]; then print_msg "Failed."; exit 1; fi
    make -j
    if [ $? -ne 0 ]; then print_msg "Failed."; exit 1; fi
#     cp -fv kvsd_stream_hls $ayla_install_dir/bin
}

#####################  Main
if [[ $# != 3 ]]; then
    echo "Usage: $0 <build_dir> <kvsd_src_dir> <cmake_defines>"
    exit 1
fi

working_dir=$(realpath $1)
shift 1
ayla_src_dir=$(realpath $1)
shift
cmake_defines=$@

echo "=== Working dir:   $working_dir"
echo "=== Ayla src dir:  $ayla_src_dir"
echo "=== Cmake defines: $cmake_defines"

#install_hls_sdk
#install_kvsd_stream_master
install_kvsd_stream_hls $cmake_defines
#install_kvsd_stream_webrtc

exit 0
