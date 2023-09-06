/** @file stream_comm.h
  * @author Maciej Andrzejewski
  * @date 17.08.23
  * @copyright Copyright © M-Works, Maciej Andrzejewski 2023
  * @brief A Documented file.
  * @details Details.
  */


#ifndef __STREAM_COMM_H__
#define __STREAM_COMM_H__

enum stream_comm_to_master_cmds
{
    SC_TM_CMD_NULL = 0,
    SC_TM_CMD_START_HLS,
    SC_TM_CMD_STOP_HLS,
    SC_TM_CMD_START_WEBRTC,
    SC_TM_CMD_STOP_WEBRTC,
    SC_TM_CMD_CNT
};

enum stream_comm_to_slave_cmds
{
    SC_TS_CMD_NULL = 0,
    SC_TS_CMD_HLS_STARTED,
    SC_TS_CMD_HLS_STOPPED,
    SC_TS_CMD_HLS_ERROR,
    SC_TS_CMD_WEBRTC_STARTED,
    SC_TS_CMD_WEBRTC_STOPPED,
    SC_TS_CMD_WEBRTC_ERROR,
    SC_TS_CMD_CNT
};

#endif /* __STREAM_COMM_H__ */


