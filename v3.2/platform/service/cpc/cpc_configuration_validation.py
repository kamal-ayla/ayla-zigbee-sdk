def validate(project):
  config_obj = project.config('SL_CPC_DEBUG_SYSTEM_VIEW')
  if ((project.config('SL_CPC_DEBUG_SYSTEM_VIEW_LOG_CORE_EVENT') == '1' 
      or project.config('SL_CPC_DEBUG_SYSTEM_VIEW_LOG_ENDPOINT_EVENT') == '1')
      and project.is_selected('segger_systemview') == 0):
    project.error('Segger System View is required when SL_CPC_DEBUG_SYSTEM_VIEW is enabled', project.target_for_defines("SL_CPC_DEBUG_SYSTEM_VIEW"))