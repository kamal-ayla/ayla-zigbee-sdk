#!/bin/bash

# ./kvs_streaming_my.sh "AC000W030477320-front_camera" 128

#/usr/bin/gst-launch-1.0 -v rtspsrc location=rtsp://<addr> ! rtph264depay ! h264parse ! kvssink stream-name=$1 storage-size=$2

echo "USER = $(whoami)"
echo "LOCATION = $1"
echo "STREAM_NAME = $2"
echo "STORAGE_SIZE = $3"
echo "WIDTH = $4"
echo "HEIGHT = $5"
echo "BITRATE = $6"
echo "FLIP = $7"
echo "GST_PLUGIN_PATH=$GST_PLUGIN_PATH"
echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo "AWS_ACCESS_KEY_ID=$AWS_ACCESS_KEY_ID"
echo "AWS_SECRET_ACCESS_KEY=$AWS_SECRET_ACCESS_KEY"
echo "AWS_DEFAULT_REGION=$AWS_DEFAULT_REGION"
echo "AWS_SESSION_TOKEN=$AWS_SESSION_TOKEN"

export GST_PLUGIN_PATH=$GST_PLUGIN_PATH
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH
export AWS_ACCESS_KEY_ID=$AWS_ACCESS_KEY_ID
export AWS_SECRET_ACCESS_KEY=$AWS_SECRET_ACCESS_KEY
export AWS_DEFAULT_REGION=$AWS_DEFAULT_REGION
export AWS_SESSION_TOKEN=$AWS_SESSION_TOKEN

/usr/bin/gst-launch-1.0 -v rtspsrc location=$1 ! rtph264depay ! h264parse ! decodebin ! videoconvert ! videoflip method=$7 ! videoscale ! videorate ! video/x-raw,width=$4,height=$5 ! x264enc bitrate=$6 ! video/x-h264, profile=baseline ! h264parse ! kvssink stream-name="$2" storage-size="$3"


