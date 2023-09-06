#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gst/gst.h>
#include <stdint.h>


static GMainLoop* loop = NULL;
GstPipeline* pipeline = NULL;
struct config
{
    char* stream_name;
    uint16_t port;
    uint32_t storage_size;
};

static void print_state_for_all_elements(GstElement *container)
{
    GstIterator *it;
    GValue item = G_VALUE_INIT;
    GstState state;
    const gchar *state_name;

    it = gst_bin_iterate_elements(GST_BIN(container));

    printf("HLS get all states:\n");
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

static gboolean master_bus_msg(GstBus* bus, GstMessage* msg, gpointer data)
{
    GstPipeline* pipeline = data;

    printf("Bus message '%d': %s\n", GST_MESSAGE_TYPE (msg), GST_MESSAGE_TYPE_NAME (msg));

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

            g_error_free(err);
            g_free(dbg);

            GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN(pipeline),
                                               GST_DEBUG_GRAPH_SHOW_ALL, "ipc.error");

            g_main_loop_quit(loop);

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

static void start_sink(struct config* config)
{
    char pipeline_str[4096];
    snprintf(pipeline_str, 4096,
             "udpsrc port=%u ! application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96 ! rtph264depay ! "
             "h264parse ! "
             "kvssink stream-name=%s storage-size=%u",
             config->port, config->stream_name, config->storage_size);

    GError* error = NULL;
    pipeline = gst_parse_launch(pipeline_str, &error);
    if (error)
    {
        g_printerr("Could not create pipeline: %s\n", error->message);
        g_error_free(error);
        return;
    }

    gst_bus_add_watch(GST_ELEMENT_BUS (pipeline), master_bus_msg, pipeline);

    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN(pipeline),
                                       GST_DEBUG_GRAPH_SHOW_ALL, "ipc.sink");
    /* The state of the slave pipeline will change together with the state
     * of the master, there is no need to call gst_element_set_state() here */

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
}

int
main(int argc, char** argv)
{
    if(argc != 4)
    {
        printf("Usage: %s <channel name> <storage size> <port>\n", argv[0]);
        return -1;
    }

    struct config config;
    config.stream_name = argv[1];
    config.storage_size = atoi(argv[2]);
    config.port = atoi(argv[3]);

    // get AWS env for kvs
    char* aws_access_key_id = getenv("AWS_ACCESS_KEY_ID");
    char* aws_secret_access_key = getenv("AWS_SECRET_ACCESS_KEY");
    char* aws_default_region = getenv("AWS_DEFAULT_REGION");
    char* aws_session_token = getenv("AWS_SESSION_TOKEN");

    printf("AWS_ACCESS_KEY_ID: %s\n", aws_access_key_id);
    printf("AWS_SECRET_ACCESS_KEY: %s\n", aws_secret_access_key);
    printf("AWS_DEFAULT_REGION: %s\n", aws_default_region);
    printf("AWS_SESSION_TOKEN: %s\n", aws_session_token);
    printf("Channel name: %s\n", config.stream_name);
    printf("Storage size: %u\n", config.storage_size);
    printf("Port: %u\n", config.port);

    gst_init(NULL, NULL);
    start_sink(&config);

    return 0;
}

