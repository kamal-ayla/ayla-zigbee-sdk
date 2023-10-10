#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <gst/gst.h>
#include <sys/un.h>
#include <signal.h>
#include <pthread.h>
#include <stdbool.h>
#include "stream_comm.h"
#include <stdint.h>

#define GST_CHK_ELEM(elem) if(elem == NULL) { g_printerr("Error\n"); exit(-1); }
#define GST_CHK(bool) if(bool == FALSE) { g_printerr("Error\n"); exit(-1); }
#define COMM_BUFFER_SIZE        32


static GMainLoop* loop = NULL;
GstPad *queue1_blockpad, *queue2_blockpad;
gulong queue1_blockpad_probe_id, queue2_blockpad_probe_id;
GstElement *queue1, *queue2;
GstElement* pipeline;
GstElement *udpsink1, *udpsink2;
GstElement *fakesink1, *fakesink2;
GstElement *videoscale = NULL;
GstElement *videoflip = NULL;
GstElement *source;
GstElement *tee;
GstElement* capsfilter_video;
GstElement* capsfilter_rtp;
GstElement *videorate;
char* rtsp_location;

struct video_convert
{
    uint32_t kbitrate;
    uint32_t width;
    uint32_t height;
    uint32_t flip;
} video_convert_conf;


struct stream_data
{
    char name[128];
    uint16_t port;

    GstElement* udpsink;
    GstElement* fakesink;
    GstElement* queue;
    void (*block_queue)(void);
    void (*unblock_queue)(void);
};

struct comm_data
{
    char name[128];
    int fd_in;
    int fd_out;
    struct sockaddr_un addr;
    char path[1024];
    
    struct stream_data* hls_stream_data;
    struct stream_data* webrtc_stream_data;
};

int start_stream(struct stream_data* sdata);
int stop_stream(struct stream_data* sdata);
void restart_pipeline(void);
static void print_state_for_all_elements(GstElement *container);

struct stream_data hls_stream_data;
struct stream_data webrtc_stream_data;
struct comm_data comm_stream_data;

gboolean set_check_property(GstElement *object, const gchar *property_name, ...) {
    GParamSpec *property_spec;
    va_list args;

    if(object == NULL)
    {
        printf("Error: Invalid GObject: NULL\n");
        return FALSE;
    }

    // Check if the object is valid
    if (!GST_IS_ELEMENT(object)) {
        printf("Error: Invalid GObject passed: %s\n", GST_OBJECT_NAME(object));
        return FALSE;
    }

    // Check if the object has the property
    property_spec = g_object_class_find_property(G_OBJECT_GET_CLASS(object), property_name);
    if (!property_spec) {
        printf("Error: Property %s does not exist for object %s.\n",
                property_name, G_OBJECT_TYPE_NAME(object));
        return FALSE;
    }

    // Set the property using g_object_set_valist
    printf("g_object_set_valist: %s\n", property_name);
    va_start(args, property_name);
    g_object_set_valist(G_OBJECT(object), property_name, args);
    va_end(args);

    return TRUE;
}

static gboolean
master_bus_msg(GstBus* bus, GstMessage* msg, gpointer data)
{
    GstPipeline* pipeline = data;

    printf("Master: Bus message '%d': %s\n", GST_MESSAGE_TYPE (msg), GST_MESSAGE_TYPE_NAME (msg));

    switch(GST_MESSAGE_TYPE (msg))
    {
        case GST_MESSAGE_ERROR:
        {
            GError* err;
            gchar* dbg;

            gst_message_parse_error(msg, &err, &dbg);
            g_printerr("ERROR: %s\n", err->message);
            if(dbg != NULL)
            {
                g_printerr("ERROR debug information: %s\n", dbg);
            }

            bool handled = false;
            if(err->code == GST_RESOURCE_ERROR_READ ||
                err->code == GST_RESOURCE_ERROR_WRITE)
            {
                // Broken pipe on socket
                // Parse for witch object
                if(strstr(dbg, "udpsink1") != NULL)
                {
                    printf("Broken pipe on udpsink1\n");
                }
                else if(strstr(dbg, "udpsink2") != NULL)
                {
                    printf("Broken pipe on udpsink2\n");
                }
                handled = true;
            }

            g_error_free(err);
            g_free(dbg);

            GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN(pipeline),
                                               GST_DEBUG_GRAPH_SHOW_ALL, "ipc.error");

            if(! handled)
            {
                g_main_loop_quit(loop);
            }
            break;
        }
        case GST_MESSAGE_WARNING:
        {
            GError* err;
            gchar* dbg;

            gst_message_parse_warning(msg, &err, &dbg);
            g_printerr("WARNING: %s\n", err->message);
            if(dbg != NULL)
            {
                g_printerr("WARNING debug information: %s\n", dbg);
            }
            g_error_free(err);
            g_free(dbg);

            GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN(pipeline),
                                               GST_DEBUG_GRAPH_SHOW_ALL, "ipc.warning");
            break;
        }
        case GST_MESSAGE_ASYNC_DONE:
            GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN(pipeline),
                                               GST_DEBUG_GRAPH_SHOW_ALL, "ipc.async-done");
            break;
        case GST_MESSAGE_BUFFERING:
        {
            gint percent;
            gst_message_parse_buffering(msg, &percent);
            g_print("Buffering (%3d%%)\r", percent);
            break;
        }
        case GST_MESSAGE_LATENCY:
        {
            gst_bin_recalculate_latency(GST_BIN(pipeline));
            break;
        }
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
//            gst_element_set_state(GST_ELEMENT (pipeline), GST_STATE_NULL);
//            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_STATE_CHANGED:
        {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

                g_print("Element %s changed state from %s to %s.\n",
                        GST_OBJECT_NAME(msg->src),
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state));
            }

            print_state_for_all_elements(GST_ELEMENT (pipeline));
            break;
        }
        default:
            break;
    }
    return TRUE;
}

static void print_state_for_all_elements(GstElement *container)
{
    GstIterator *it;
    GValue item = G_VALUE_INIT;
    GstState state;
    const gchar *state_name;

    it = gst_bin_iterate_elements(GST_BIN(container));

    printf("Master get all states:\n");
    gboolean done = FALSE;
    while (!done) {
        switch (gst_iterator_next(it, &item)) {
            case GST_ITERATOR_OK:
            {
                GstElement *element = GST_ELEMENT(g_value_get_object(&item));
                gst_element_get_state(element, &state, NULL, GST_CLOCK_TIME_NONE);
                state_name = gst_element_state_get_name(state);
                printf("\tElement %s state: %s\n", GST_ELEMENT_NAME(element), state_name);
                g_value_reset(&item);
            }
                break;
            case GST_ITERATOR_RESYNC:
                gst_iterator_resync(it);
                break;
            case GST_ITERATOR_ERROR:
            case GST_ITERATOR_DONE:
                done = TRUE;
                break;
        }
    }

    g_value_unset(&item);
    gst_iterator_free(it);
}

static GstPadProbeReturn
pad_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
    printf("pad_probe_cb\n");

    gulong* blocked_pad_id = user_data;
    *blocked_pad_id = GST_PAD_PROBE_INFO_ID (info);

    return GST_PAD_PROBE_OK;
}

static void block_queue1 ()
{
    printf("Block queue1\n");

    gst_pad_add_probe (queue1_blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                       pad_probe_cb, &queue1_blockpad_probe_id, NULL);
}

static void unblock_queue1 ()
{
    printf("Unblock queue1\n");

    gst_pad_remove_probe (queue1_blockpad, queue1_blockpad_probe_id);
}

static void block_queue2 ()
{
    printf("Block queue2\n");

    gst_pad_add_probe (queue2_blockpad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                       pad_probe_cb, &queue2_blockpad_probe_id, NULL);
}

static void unblock_queue2 ()
{
    printf("Unblock queue2\n");

    gst_pad_remove_probe (queue2_blockpad, queue2_blockpad_probe_id);
}

/* Callback to link the dynamic source pad */
static void on_pad_added(GstElement *src, GstPad *new_pad, GstElement *sink)
{
    GstPad *sink_pad = gst_element_get_static_pad(sink, "sink");
    if (!sink_pad)
    {
        g_warning("Failed to get sink pad!");
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
    if (ret != GST_PAD_LINK_OK)
    {
        g_warning("Failed to link pads: %d", ret);
    }

    gst_element_sync_state_with_parent(sink);

    gst_object_unref(sink_pad);
}

static void
start_source()
{
    pipeline = gst_pipeline_new(NULL); GST_CHK_ELEM(pipeline);
    gst_bus_add_watch(GST_ELEMENT_BUS (pipeline), master_bus_msg, pipeline);

    source = gst_element_factory_make("rtspsrc", "rtspsrc");
//    set_check_property(source, "is-live", 1, NULL);
    set_check_property(source, "latency", 0, NULL);
    set_check_property(source, "drop-on-latency", TRUE, NULL);
    set_check_property(source, "buffer-mode", 0, NULL);
    set_check_property(source, "ntp-sync", FALSE, NULL);
    set_check_property(source, "udp-buffer-size", 51200, NULL);
    set_check_property(source, "tcp-timeout", 1000, NULL);
    set_check_property(source, "short-header", TRUE, NULL);
    set_check_property(source, "location", rtsp_location, NULL);

    GstElement* decodebin = gst_element_factory_make("decodebin", "decodebin"); GST_CHK_ELEM(decodebin);
    GstElement* videoconvert = gst_element_factory_make("videoconvert", "videoconvert"); GST_CHK_ELEM(videoconvert);
    GstElement* x264enc = gst_element_factory_make("x264enc", "x264enc"); GST_CHK_ELEM(x264enc);
    set_check_property(x264enc, "tune", 4 /*"zerolatency"*/, NULL);
    GstElement* rtph264pay = gst_element_factory_make("rtph264pay", "rtph264pay"); GST_CHK_ELEM(rtph264pay);

    tee = gst_element_factory_make("tee", "tee"); GST_CHK_ELEM(tee);

    udpsink1 = gst_element_factory_make("udpsink", "udpsink1"); GST_CHK_ELEM(udpsink1);
    udpsink2 = gst_element_factory_make("udpsink", "udpsink2"); GST_CHK_ELEM(udpsink2);
    set_check_property(udpsink1, "host", "127.0.0.1", "port", hls_stream_data.port, NULL);
    set_check_property(udpsink2, "host", "127.0.0.1", "port", webrtc_stream_data.port, NULL);

    queue1 = gst_element_factory_make("queue", "queue1"); GST_CHK_ELEM(queue1);
    set_check_property(queue1, "flush-on-eos", 1, NULL);
    set_check_property(queue1, "leaky", 2, NULL);
    set_check_property(queue1, "max-size-buffers", 0, NULL);
    set_check_property(queue1, "max-size-bytes", 0, NULL);
    set_check_property(queue1, "max-size-time", (guint64)0, NULL);
    queue1_blockpad = gst_element_get_static_pad (queue1, "src");

    queue2 = gst_element_factory_make("queue", "queue2"); GST_CHK_ELEM(queue2);
    set_check_property(queue2, "flush-on-eos", 1, NULL);
    set_check_property(queue2, "leaky", 2, NULL);
    set_check_property(queue2, "max-size-buffers", 0, NULL);
    set_check_property(queue2, "max-size-bytes", 0, NULL);
    set_check_property(queue2, "max-size-time", (guint64)0, NULL);
    queue2_blockpad = gst_element_get_static_pad (queue2, "src");

    fakesink1 = gst_element_factory_make("fakesink", "fakesink1"); GST_CHK_ELEM(fakesink1);
    fakesink2 = gst_element_factory_make("fakesink", "fakesink2"); GST_CHK_ELEM(fakesink2);

    videoscale = gst_element_factory_make("videoscale", "videoscale"); GST_CHK_ELEM(videoscale);
    capsfilter_video = gst_element_factory_make("capsfilter", "capsfilter_video"); GST_CHK_ELEM(capsfilter_video);
    capsfilter_rtp = gst_element_factory_make("capsfilter", "capsfilter_rtp"); GST_CHK_ELEM(capsfilter_video);
    videoflip = gst_element_factory_make("videoflip", "videoflip"); GST_CHK_ELEM(videoflip);
    videorate = gst_element_factory_make("videorate", "videorate"); GST_CHK_ELEM(videorate);

    gst_bin_add_many(GST_BIN (pipeline), source, decodebin, videoconvert, videoscale, videoflip, videorate, capsfilter_video, capsfilter_rtp, x264enc, rtph264pay,
                     tee, queue1, queue2, fakesink1, fakesink2, NULL);

    // Support for kbitrate
    if(video_convert_conf.kbitrate != 0)
    {
        set_check_property(x264enc, "bitrate", video_convert_conf.kbitrate, NULL);
    }

    // Support for the flip
    if(video_convert_conf.flip != 0)
    {
        set_check_property(videoflip, "method", video_convert_conf.flip, NULL);
    }

    // Support for width, height
    if(video_convert_conf.width != 0 && video_convert_conf.height != 0)
    {
        GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                            "width", G_TYPE_INT, video_convert_conf.width,
                                            "height", G_TYPE_INT, video_convert_conf.height,
                                            NULL);
        g_object_set(capsfilter_video, "caps", caps, NULL);
        gst_caps_unref(caps);
    }
    else if(video_convert_conf.width != 0)
    {
        GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                            "width", G_TYPE_INT, video_convert_conf.width,
                                            NULL);
        g_object_set(capsfilter_video, "caps", caps, NULL);
        gst_caps_unref(caps);
    }
    else if(video_convert_conf.height != 0)
    {
        GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                            "height", G_TYPE_INT, video_convert_conf.height,
                                            NULL);
        g_object_set(capsfilter_video, "caps", caps, NULL);
        gst_caps_unref(caps);
    }

    // Filter out the audio channel from RTP
    {
        GstCaps* caps = gst_caps_new_simple("application/x-rtp",
                                            "media", G_TYPE_STRING, "video",
                                            NULL);
        g_object_set(capsfilter_rtp, "caps", caps, NULL);
        gst_caps_unref(caps);
    }

    // Don't link rtspsrc (src) yet since it uses dynamic pads.
    // Instead, connect a callback to its "pad-added" signal.
    g_signal_connect(source, "pad-added", G_CALLBACK(on_pad_added), capsfilter_rtp);
    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_pad_added), videoconvert);

    GST_CHK(gst_element_link_many(capsfilter_rtp, decodebin, NULL));
    GST_CHK(gst_element_link_many(videoconvert, videoscale, videorate, capsfilter_video, videoflip, x264enc, rtph264pay, tee, NULL));

    GST_CHK(gst_element_link_many(tee, queue1, fakesink1, NULL));
    GST_CHK(gst_element_link_many(tee, queue2, fakesink2, NULL));

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN(pipeline),
                                       GST_DEBUG_GRAPH_SHOW_ALL, "ipc.src");
}

int start_stream(struct stream_data* sdata)
{
    sdata->block_queue();
    g_usleep(100000);

//    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    gst_element_set_state(sdata->fakesink, GST_STATE_NULL);
    gst_element_set_state(sdata->udpsink, GST_STATE_NULL);
    gst_object_ref(sdata->fakesink);
    gst_bin_remove_many(GST_BIN (pipeline), sdata->fakesink, NULL);
    gst_bin_add_many(GST_BIN (pipeline), sdata->udpsink, NULL);
    gst_element_link_many(sdata->queue, sdata->udpsink, NULL);
    g_usleep(100000);
    gst_element_set_state(sdata->udpsink, GST_STATE_PAUSED);
//    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    gst_element_set_state(sdata->udpsink, GST_STATE_PLAYING);
    g_usleep(100000);
    sdata->unblock_queue();
    
    return 0;
}

int stop_stream(struct stream_data* sdata)
{
    sdata->block_queue();
    g_usleep(100000);

    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    g_usleep(100000);
    gst_element_set_state(sdata->fakesink, GST_STATE_NULL);
    gst_element_set_state(sdata->udpsink, GST_STATE_NULL);
    gst_object_ref(sdata->udpsink);
    gst_bin_remove_many(GST_BIN (pipeline), sdata->udpsink, NULL);
    gst_bin_add_many(GST_BIN (pipeline), sdata->fakesink, NULL);
    gst_element_link_many(sdata->queue, sdata->fakesink, NULL);
    g_usleep(100000);
    gst_element_set_state(sdata->fakesink, GST_STATE_PAUSED);
//    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    gst_element_set_state(sdata->fakesink, GST_STATE_PLAYING);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_usleep(100000);
    sdata->unblock_queue();

    return 0;
}

static void* comm_socket_thread(void* arg)
{
    struct comm_data* comm_data = (struct comm_data*)arg;
    
    ssize_t numRead;
    char buffer[COMM_BUFFER_SIZE];

    // Create socket
    comm_data->fd_in = socket(AF_UNIX, SOCK_STREAM, 0);
    if (comm_data->fd_in == -1) {
        perror("Server: socket error");
        exit(EXIT_FAILURE);
    }

    // Remove the socket if it already exists
    unlink(comm_data->path);

    memset(&comm_data->addr, 0, sizeof(struct sockaddr_un));
    comm_data->addr.sun_family = AF_UNIX;
    strncpy(comm_data->addr.sun_path, comm_data->path, sizeof(comm_data->addr.sun_path) - 1);

    // Bind the socket
    if (bind(comm_data->fd_in, (struct sockaddr *)&comm_data->addr, sizeof(struct sockaddr_un)) == -1) {
        perror("Server: bind error");
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(comm_data->fd_in, 5) == -1) {
        perror("Server: listen error");
        exit(EXIT_FAILURE);
    }

    int cmd;
    int ret;
    char to_slave_buff[COMM_BUFFER_SIZE];

    while(1)
    {
        fprintf(stdout, "Server: waiting for connection...\n");
        comm_data->fd_out = accept(comm_data->fd_in, NULL, NULL);
        if (comm_data->fd_out == -1) {
            perror("Server: accept error");
            exit(EXIT_FAILURE);
        }

        while(1)
        {
            // Read from the client
            numRead = read(comm_data->fd_out, buffer, COMM_BUFFER_SIZE);
            if(numRead == 0)
            {
                fprintf(stdout, "Server: client disconnected\n");
                close(comm_data->fd_out);
                break;
            }
            else if(numRead > 0)
            {
                buffer[numRead] = '\0';
                fprintf(stdout, "Server received %d bytes\n", numRead);
                cmd = buffer[0];
                if(cmd >= SC_TM_CMD_CNT)
                {
                    cmd -= 48;  // Convert from ASCII to int
                }

                switch(cmd)
                {
                    case SC_TM_CMD_START_HLS:
                    {
                        fprintf(stdout, "Server: received start HLS command\n");
                        ret = start_stream(comm_data->hls_stream_data);
                        if(ret != 0)
                        {
                            to_slave_buff[0] = SC_TS_CMD_HLS_ERROR;
                            send(comm_data->fd_out, to_slave_buff, 1, MSG_NOSIGNAL);
                        }
                        else
                        {
                            to_slave_buff[0] = SC_TS_CMD_HLS_STARTED;
                            send(comm_data->fd_out, to_slave_buff, 1, MSG_NOSIGNAL);
                        }
                        break;
                    }
                    case SC_TM_CMD_STOP_HLS:
                    {
                        fprintf(stdout, "Server: received stop HLS command\n");
                        ret = stop_stream(comm_data->hls_stream_data);
                        if(ret != 0)
                        {
                            to_slave_buff[0] = SC_TS_CMD_HLS_ERROR;
                            send(comm_data->fd_out, to_slave_buff, 1, MSG_NOSIGNAL);
                        }
                        else
                        {
                            to_slave_buff[0] = SC_TS_CMD_HLS_STOPPED;
                            send(comm_data->fd_out, to_slave_buff, 1, MSG_NOSIGNAL);
                        }
                        break;
                    }
                    case SC_TM_CMD_START_WEBRTC:
                    {
                        fprintf(stdout, "Server: received start WebRTC command\n");
                        ret = start_stream(comm_data->webrtc_stream_data);
                        if(ret != 0)
                        {
                            to_slave_buff[0] = SC_TS_CMD_WEBRTC_ERROR;
                            send(comm_data->fd_out, to_slave_buff, 1, MSG_NOSIGNAL);
                        }
                        else
                        {
                            to_slave_buff[0] = SC_TS_CMD_WEBRTC_STARTED;
                            send(comm_data->fd_out, to_slave_buff, 1, MSG_NOSIGNAL);
                        }
                        break;
                    }
                    case SC_TM_CMD_STOP_WEBRTC:
                    {
                        fprintf(stdout, "Server: received stop WebRTC command\n");
                        ret = stop_stream(comm_data->webrtc_stream_data);
                        if(ret != 0)
                        {
                            to_slave_buff[0] = SC_TS_CMD_WEBRTC_ERROR;
                            send(comm_data->fd_out, to_slave_buff, 1, MSG_NOSIGNAL);
                        }
                        else
                        {
                            to_slave_buff[0] = SC_TS_CMD_WEBRTC_STOPPED;
                            send(comm_data->fd_out, to_slave_buff, 1, MSG_NOSIGNAL);
                        }
                        break;
                    }
                    default:
                    {
                        fprintf(stdout, "Server: received unknown command: %d\n", cmd);
                        break;
                    }
                }
            }
            else
            {
                perror("Server: read error");
                break;
            }
        }
    }
}


static void
run(pid_t pid)
{
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
}

static void close_sockets()
{
    close(comm_stream_data.fd_in);
    close(comm_stream_data.fd_out);
    unlink(comm_stream_data.path);
}

void handle_sigint(int sig)
{
    close_sockets();
    exit(0);
}

void handle_sigpipe(int signal) {
    printf("SIGPIPE received. Other end closed the connection.\n");
}

int main(int argc, char** argv)
{
    if(argc < 5 || argc > 9)
    {
        printf("Usage: %s <rtsp_location> <comm_socket> <hls_port> <webrtc_port> (kbitrate) (width) (height) (flip)\n", argv[0]);
        printf("Eg.: %s rtsp://127.0.0.1:554 /tmp/comm.socket 5001 5002 500 640 480 1"
               "Optional parameters:\n"
               "\tkbitrate: bitrate in kbit/s\n"
                "\twidth: width of the video\n"
                "\theight: height of the video\n"
                "\tflip: flip the video (gstreamer 'videoflip' compatible format)\n"
               , argv[0]);
        return -1;
    }

    rtsp_location = argv[1];
    strcpy(comm_stream_data.path, argv[2]);
    hls_stream_data.port = atoi(argv[3]);
    webrtc_stream_data.port = atoi(argv[4]);

    memset(&video_convert_conf, 0, sizeof(struct video_convert));
    if(argc >= 6)
    {
        video_convert_conf.kbitrate = atoi(argv[5]);
    }
    if(argc >= 7)
    {
        video_convert_conf.width = atoi(argv[6]);
    }
    if(argc >= 8)
    {
        video_convert_conf.height = atoi(argv[7]);
    }
    if(argc >= 9)
    {
        video_convert_conf.flip = atoi(argv[8]);
    }

    printf("RTSP location: %s\n", rtsp_location);
    printf("Socket Comm: %s\n", comm_stream_data.path);
    printf("HLS port: %u\n", hls_stream_data.port);
    printf("WebRTC port: %u\n", webrtc_stream_data.port);

    // Handle Ctrl+C
    struct sigaction action;
    action.sa_handler = handle_sigint;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);

    // Handle SIGPIPE
    signal(SIGPIPE, handle_sigpipe);
    signal(SIGPIPE, SIG_IGN);

    setenv("GST_DEBUG_FILE", "gstsrc.log", 1);
    gst_init(&argc, &argv);
    start_source();

    // Start socket threads
    hls_stream_data.block_queue = block_queue1;
    hls_stream_data.unblock_queue = unblock_queue1;
    strcpy(hls_stream_data.name, "Pipeline 1");
    hls_stream_data.udpsink = udpsink1;
    hls_stream_data.fakesink = fakesink1;
    hls_stream_data.queue = queue1;

    webrtc_stream_data.block_queue = block_queue2;
    webrtc_stream_data.unblock_queue = unblock_queue2;
    strcpy(webrtc_stream_data.name, "Pipeline 2");
    webrtc_stream_data.udpsink = udpsink2;
    webrtc_stream_data.fakesink = fakesink2;
    webrtc_stream_data.queue = queue2;

    pthread_t comm_thread;
    comm_stream_data.hls_stream_data = &hls_stream_data;
    comm_stream_data.webrtc_stream_data = &webrtc_stream_data;
    strcpy(comm_stream_data.name, "Master stream");
    pthread_create(&comm_thread, NULL, comm_socket_thread, &comm_stream_data);

    run(0);

    close_sockets();

    return 0;
}

