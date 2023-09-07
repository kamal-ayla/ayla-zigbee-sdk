cmake_minimum_required(VERSION 3.10)
project(kvsd_stream_hls)

add_executable(${PROJECT_NAME} main.c)

target_link_libraries(${PROJECT_NAME} PRIVATE
        gobject-2.0
        gio-2.0
        glib-2.0
        gstreamer-1.0
        gstbase-1.0
        gstapp-1.0
        gstvideo-1.0
)
