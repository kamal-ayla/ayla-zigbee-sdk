from . import halconfig_types as types
from . import halconfig_dependency as dep

name = "RSSI"
displayname = "RSSI"
description = "RSSI management options for the radio."
compatibility = dep.Dependency(platform=(dep.Platform.SERIES1, dep.Platform.SERIES2), mcu_type=dep.McuType.RADIO)  # EFR32
category = " Radio"
options = {
    "SL_RAIL_UTIL_RSSI_OFFSET": [
        {
            "type": "int8_t",
            "description": "Radio RSSI Offset",
            "min": "-35",
            "max": "35",
            "defaultValue": "-8",
            "subcategory": "RSSI",
            "dependency": dep.Dependency(platform=dep.Platform.SERIES1),
            "longdescription": "Radio RSSI Offset (-35 to 35)",
        },
        {
            "type": "int8_t",
            "description": "Radio RSSI Offset",
            "min": "-35",
            "max": "35",
            "defaultValue": "-11",
            "subcategory": "RSSI",
            "dependency": dep.Dependency(sdid=[200]),
            "longdescription": "Radio RSSI Offset (-35 to 35)",
        },
        {
            "type": "int8_t",
            "description": "Radio RSSI Offset",
            "min": "-35",
            "max": "35",
            "defaultValue": "0",
            "subcategory": "RSSI",
            "dependency": dep.Dependency(sdid=[205,210,215]),
            "longdescription": "Radio RSSI Offset (-35 to 35)",
        },
    ],
}
