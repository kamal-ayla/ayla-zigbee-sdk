// This file is generated by Simplicity Studio.  Please do not edit manually.
//
//

// Enclosing macro to prevent multiple inclusion
#ifndef __AF_GEN_EVENT__
#define __AF_GEN_EVENT__


// Code used to configure the cluster event mechanism
#define EMBER_AF_GENERATED_EVENT_CODE \
  EmberEventControl emberAfIdentifyClusterServerTickCallbackControl1; \
  extern EmberEventControl emberAfPluginFindAndBindInitiatorCheckTargetResponsesEventControl; \
  extern EmberEventControl emberAfPluginFormAndJoinCleanupEventControl; \
  extern EmberEventControl emberAfPluginIasZoneClientStateMachineEventControl; \
  extern EmberEventControl emberAfPluginIdentifyFeedbackProvideFeedbackEventControl; \
  extern EmberEventControl emberAfPluginInterpanFragmentReceiveEventControl; \
  extern EmberEventControl emberAfPluginInterpanFragmentTransmitEventControl; \
  extern EmberEventControl emberAfPluginNetworkCreatorSecurityOpenNetworkEventControl; \
  extern EmberEventControl emberAfPluginNetworkFindTickEventControl; \
  extern EmberEventControl emberAfPluginScanDispatchScanEventControl; \
  extern EmberEventControl emberAfPluginTrustCenterNwkKeyUpdateBroadcastMyEventControl; \
  extern void emberAfPluginFindAndBindInitiatorCheckTargetResponsesEventHandler(void); \
  extern void emberAfPluginFormAndJoinCleanupEventHandler(void); \
  extern void emberAfPluginIasZoneClientStateMachineEventHandler(void); \
  extern void emberAfPluginIdentifyFeedbackProvideFeedbackEventHandler(void); \
  extern void emberAfPluginInterpanFragmentReceiveEventHandler(void); \
  extern void emberAfPluginInterpanFragmentTransmitEventHandler(void); \
  extern void emberAfPluginNetworkCreatorSecurityOpenNetworkEventHandler(void); \
  extern void emberAfPluginNetworkFindTickEventHandler(void); \
  extern void emberAfPluginScanDispatchScanEventHandler(void); \
  extern void emberAfPluginTrustCenterNwkKeyUpdateBroadcastMyEventHandler(void); \
  static void clusterTickWrapper(EmberEventControl *control, EmberAfTickFunction callback, uint8_t endpoint) \
  { \
    emberAfPushEndpointNetworkIndex(endpoint); \
    emberEventControlSetInactive(*control); \
    (*callback)(endpoint); \
    emberAfPopNetworkIndex(); \
  } \
  void emberAfIdentifyClusterServerTickCallbackWrapperFunction1(void) { clusterTickWrapper(&emberAfIdentifyClusterServerTickCallbackControl1, emberAfIdentifyClusterServerTickCallback, 1); } \
  EmberEventControl emberAfPluginZllIdentifyServerTriggerEffectEndpointEventControls[1]; \
  extern void emberAfPluginZllIdentifyServerTriggerEffectEndpointEventHandler(uint8_t endpoint); \
  void emberAfPluginZllIdentifyServerTriggerEffectEndpointEventWrapper1(void) { clusterTickWrapper(&emberAfPluginZllIdentifyServerTriggerEffectEndpointEventControls[0], emberAfPluginZllIdentifyServerTriggerEffectEndpointEventHandler, 1); } \


// EmberEventData structs used to populate the EmberEventData table
#define EMBER_AF_GENERATED_EVENTS   \
  { &emberAfIdentifyClusterServerTickCallbackControl1, emberAfIdentifyClusterServerTickCallbackWrapperFunction1 }, \
  { &emberAfPluginFindAndBindInitiatorCheckTargetResponsesEventControl, emberAfPluginFindAndBindInitiatorCheckTargetResponsesEventHandler }, \
  { &emberAfPluginFormAndJoinCleanupEventControl, emberAfPluginFormAndJoinCleanupEventHandler }, \
  { &emberAfPluginIasZoneClientStateMachineEventControl, emberAfPluginIasZoneClientStateMachineEventHandler }, \
  { &emberAfPluginIdentifyFeedbackProvideFeedbackEventControl, emberAfPluginIdentifyFeedbackProvideFeedbackEventHandler }, \
  { &emberAfPluginInterpanFragmentReceiveEventControl, emberAfPluginInterpanFragmentReceiveEventHandler }, \
  { &emberAfPluginInterpanFragmentTransmitEventControl, emberAfPluginInterpanFragmentTransmitEventHandler }, \
  { &emberAfPluginNetworkCreatorSecurityOpenNetworkEventControl, emberAfPluginNetworkCreatorSecurityOpenNetworkEventHandler }, \
  { &emberAfPluginNetworkFindTickEventControl, emberAfPluginNetworkFindTickEventHandler }, \
  { &emberAfPluginScanDispatchScanEventControl, emberAfPluginScanDispatchScanEventHandler }, \
  { &emberAfPluginTrustCenterNwkKeyUpdateBroadcastMyEventControl, emberAfPluginTrustCenterNwkKeyUpdateBroadcastMyEventHandler }, \
  { &emberAfPluginZllIdentifyServerTriggerEffectEndpointEventControls[0], emberAfPluginZllIdentifyServerTriggerEffectEndpointEventWrapper1 }, \


#define EMBER_AF_GENERATED_EVENT_STRINGS   \
  "Identify Cluster Server EP 1",  \
  "Find and Bind Initiator Plugin CheckTargetResponses",  \
  "Form and Join Library Plugin Cleanup",  \
  "IAS Zone Client Plugin StateMachine",  \
  "Identify Feedback Plugin ProvideFeedback",  \
  "Interpan Plugin FragmentReceive",  \
  "Interpan Plugin FragmentTransmit",  \
  "Network Creator Security Plugin OpenNetwork",  \
  "Network Find Plugin Tick",  \
  "Scan Dispatch Plugin Scan",  \
  "Trust Center Network Key Update Broadcast Plugin My",  \
  "ZLL Identify Server Plugin TriggerEffect EP 1", \


// The length of the event context table used to track and retrieve cluster events
#define EMBER_AF_EVENT_CONTEXT_LENGTH 1

// EmberAfEventContext structs used to populate the EmberAfEventContext table
#define EMBER_AF_GENERATED_EVENT_CONTEXT { 0x1, 0x3, false, EMBER_AF_LONG_POLL, EMBER_AF_OK_TO_SLEEP, &emberAfIdentifyClusterServerTickCallbackControl1}


#endif // __AF_GEN_EVENT__