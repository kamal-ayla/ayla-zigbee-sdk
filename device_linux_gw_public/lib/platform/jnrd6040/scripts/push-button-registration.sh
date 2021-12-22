#!/bin/sh
#
# Copyright 2014-2018 Ayla Networks, Inc.  All rights reserved.
#
#

export REQUEST_URI="/push_button_reg.json"
export CONTENT_LENGTH="0"
export REQUEST_METHOD="POST"
export HTTP_ACCEPT="application/json"
/sbin/acgi -l &

