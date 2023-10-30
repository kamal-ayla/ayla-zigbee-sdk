#!/bin/bash

rm build.log

cd amazon-kinesis-video-streams-producer-sdk-cpp
rm -rf build dependency open-source
cd ..

cd amazon-kinesis-video-streams-webrtc-sdk-c
rm -rf build dependency open-source
cd ..

cd kvsd_stream_hls
rm -rf build
cd ..

cd kvsd_stream_master
rm -rf build
cd ..

rm -rf rootfs

exit 0

