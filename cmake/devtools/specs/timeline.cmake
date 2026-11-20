miacode_add_dev_tool(timeline_model_spec TEST
    SOURCES
        src/tools/timeline/TimelineModelSpec.cpp
    LIBS miacode_timeline Qt6::Core Qt6::Gui Qt6::Widgets
    INCLUDES src
)
# Source-contract assertions in TimelineModelSpec must work from both the
# standard `build/` CTest directory and the developer build tree.
target_compile_definitions(timeline_model_spec PRIVATE
    "MIACODE_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"")

if(MIACODE_BUILD_APP)
miacode_add_dev_tool(timeline_hand_order_skin_spec TEST
    SOURCES src/tools/timeline/TimelineHandOrderSkinSpec.cpp
    LIBS miacode_timeline_quick Qt6::OpenGL Qt6::Widgets
    INCLUDES src
)
target_compile_definitions(timeline_hand_order_skin_spec PRIVATE
    "MIACODE_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
endif()

miacode_add_dev_tool(timeline_marker_offset_spec TEST
    SOURCES
        src/tools/timeline/TimelineMarkerOffsetSpec.cpp
    LIBS Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(timeline_cadence_arbitration_policy_spec TEST
    SOURCES
        src/tools/timeline/TimelineCadenceArbitrationPolicySpec.cpp
    LIBS Qt6::Core
    INCLUDES src
)
