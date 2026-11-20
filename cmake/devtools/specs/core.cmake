# ---- Spec executables (registered with CTest) ----
miacode_add_dev_tool(oplog_self_test TEST
    SOURCES
        src/tools/oplog/OperationLogSpec.cpp
    LIBS miacode_base Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(simai_parser_spec TEST
    SOURCES
        src/tools/simai_parser/SimaiParserSpec.cpp
    LIBS miacode_chart Qt6::Core Qt6::Gui Qt6::Widgets
    INCLUDES src
)

miacode_add_dev_tool(chart_batch_transform_spec TEST
    SOURCES
        src/tools/chart_transform/ChartBatchTransformSpec.cpp
    LIBS miacode_chart Qt6::Core Qt6::Gui Qt6::Widgets
    INCLUDES src
)

miacode_add_dev_tool(chart_selection_beat_summary_spec TEST
    SOURCES
        src/tools/chart_selection/ChartSelectionBeatSummarySpec.cpp
    LIBS miacode_chart Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(extension_manifest_spec TEST
    SOURCES
        src/tools/extensions/ExtensionManifestSpec.cpp
        src/extensions/ExtensionManifest.h
        src/extensions/ExtensionManifest.cpp
    LIBS Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(app_background_settings_spec TEST
    SOURCES
        src/tools/ui/AppBackgroundSettingsSpec.cpp
        src/app/ui/preferences/AppBackgroundSettings.h
        src/app/ui/preferences/AppBackgroundSettings.cpp
    LIBS Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(theme_variant_resolver_spec TEST
    SOURCES
        src/tools/ui/ThemeVariantResolverSpec.cpp
        src/app/ui/theme/ThemeVariantResolver.h
        src/app/ui/theme/ThemeVariantResolver.cpp
        src/app/services/PreferenceDocument.h
    LIBS Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(simai_document_spec TEST
    SOURCES
        src/tools/chart_document/SimaiDocumentSpec.cpp
    LIBS miacode_chart Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(cover_layout_model_spec TEST
    SOURCES
        src/tools/cover_export/CoverLayoutModelSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui Qt6::Widgets
    INCLUDES src
)

miacode_add_dev_tool(cover_pv_frame_source_spec TEST
    SOURCES
        src/tools/cover_export/CoverPvFrameSourceSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(cover_frame_playback_controller_spec TEST
    SOURCES
        src/tools/cover_export/CoverFramePlaybackControllerSpec.cpp
    LIBS miacode_export Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(cover_frame_scene_binder_spec TEST
    SOURCES
        src/tools/cover_export/CoverFrameSceneBinderSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(muri_spec TEST
    SOURCES
        src/tools/muri/MuriSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui Qt6::Widgets
    INCLUDES src
)

miacode_add_dev_tool(touch_pad_authoring_edit_spec TEST
    SOURCES
        src/tools/editor/TouchPadAuthoringEditSpec.cpp
    LIBS miacode_editor_core Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(simai_completion_catalog_spec TEST
    SOURCES
        src/tools/editor/SimaiCompletionCatalogSpec.cpp
    LIBS miacode_editor_core Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(simai_text_edit_policy_spec TEST
    SOURCES
        src/tools/editor/SimaiTextEditPolicySpec.cpp
    LIBS miacode_editor_core Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(video_export_runtime_policy_spec TEST
    SOURCES
        src/tools/video_export/VideoExportRuntimePolicySpec.cpp
    LIBS miacode_export Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(raw_video_pipe_frame_conservation_spec TEST
    SOURCES
        src/tools/video_export/RawVideoPipeFrameConservationSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(video_export_pending_frame_redraw_spec TEST
    SOURCES
        src/tools/video_export/VideoExportPendingFrameRedrawSpec.cpp
    LIBS Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(video_export_intro_mode_spec TEST
    SOURCES
        src/tools/video_export/VideoExportIntroModeSpec.cpp
    LIBS Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(font_library_isolation_spec TEST
    SOURCES src/tools/video_export/FontLibraryIsolationSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)
target_compile_definitions(font_library_isolation_spec PRIVATE
    "MIACODE_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"")

miacode_add_dev_tool(video_export_hud_font_snapshot_spec TEST
    SOURCES src/tools/video_export/VideoExportHudFontSnapshotSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)
target_compile_definitions(video_export_hud_font_snapshot_spec PRIVATE
    "MIACODE_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"")

miacode_add_dev_tool(video_export_intro_sound_spec TEST
    SOURCES
        src/tools/video_export/VideoExportIntroSoundSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(video_export_audio_render_plan_spec TEST
    SOURCES
        src/tools/video_export/VideoExportAudioRenderPlanSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(chart_zip_packager_spec TEST
    SOURCES
        src/tools/zip_export/ChartZipPackagerSpec.cpp
    LIBS miacode_media_tools Qt6::Core Qt6::Gui miniz
    INCLUDES src
)

# Net behavior specifications consume the same media_tools library as the product.
# The engine specification also compiles the existing batch workers.
miacode_add_dev_tool(net_client_spec TEST
    SOURCES
        src/tools/net/NetClientSpec.cpp
    LIBS miacode_media_tools Qt6::Core Qt6::Gui Qt6::Network miniz
    INCLUDES src
)

miacode_add_dev_tool(net_query_rules_spec TEST
    SOURCES src/tools/net/NetQueryRulesSpec.cpp
    LIBS miacode_media_tools Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(net_http_transport_spec TEST
    SOURCES src/tools/net/NetHttpTransportSpec.cpp
    LIBS miacode_media_tools Qt6::Core Qt6::Network
    INCLUDES src
)

miacode_add_dev_tool(net_provider_spec TEST
    SOURCES src/tools/net/NetProviderSpec.cpp
    LIBS miacode_media_tools Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(net_download_flow_spec TEST
    SOURCES
        src/tools/net/NetDownloadFlowSpec.cpp
        src/app/services/api/ApiCatalog.cpp
        src/app/services/api/ApiDispatcher.cpp
        src/app/services/net/NetService.cpp
        src/app/services/net/NetService.h
        src/app/services/net/NetAccountStore.cpp
        src/app/services/net/NetConfiguration.cpp
        src/app/services/PreferenceDocument.cpp
        src/app/services/jobs/JobRegistry.cpp
        src/app/services/jobs/JobRegistry.h
    LIBS miacode_media_tools miacode_base Qt6::Core Qt6::Network miniz $<$<PLATFORM_ID:Windows>:Crypt32>
    INCLUDES src
)
target_compile_definitions(net_download_flow_spec PRIVATE
    "MIACODE_TEST_OUTPUT_ROOT=\"${CMAKE_CURRENT_BINARY_DIR}\""
    "MIACODE_ZH_CN_QM_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/zh_CN.qm\"")
add_dependencies(net_download_flow_spec miacode_lrelease)

miacode_add_dev_tool(pv_compression_policy_spec TEST
    SOURCES
        src/tools/media/PvCompressionPolicySpec.cpp
    LIBS miacode_media_tools Qt6::Core
    INCLUDES src
)

# Drift guard for ID-based QTranslator catalogs (.ts parity + source literal ids).
miacode_add_dev_tool(ui_text_locale_spec TEST
    SOURCES
        src/tools/ui_text/UiTextLocaleSpec.cpp
        src/app/services/PreferenceDocument.h
        src/app/services/PreferenceDocument.cpp
    LIBS miacode_base Qt6::Core
    INCLUDES src
)
target_compile_definitions(ui_text_locale_spec PRIVATE
    "MIACODE_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\""
    "MIACODE_EN_US_QM_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/en_US.qm\"")
qt_add_resources(ui_text_locale_spec "ui_text_spec_i18n"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${MIACODE_TRANSLATION_QM_FILES})
add_dependencies(ui_text_locale_spec miacode_lrelease)

miacode_add_dev_tool(native_chrome_policy_spec TEST
    SOURCES
        src/tools/ui/NativeChromePolicySpec.cpp
        src/app/quick_shell/QuickShellPopupPosition.h
        src/app/ui/chrome/NativeWindowThemePolicy.h
    LIBS Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(ui_text_preferences_spec TEST
    SOURCES
        src/tools/ui_text/UiTextPreferencesSpec.cpp
        src/app/services/PreferenceDocument.h
        src/app/services/PreferenceDocument.cpp
    LIBS miacode_base Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(video_export_estimate_history_spec TEST
    SOURCES src/tools/video_export/VideoExportEstimateHistorySpec.cpp
    LIBS miacode_export Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(video_export_output_spec TEST
    SOURCES src/tools/video_export/VideoExportOutputSpec.cpp
    LIBS miacode_export Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(media_prepend_policy_spec TEST
    SOURCES src/tools/media/MediaPrependPolicySpec.cpp
    LIBS miacode_media_tools Qt6::Core
    INCLUDES src
)
miacode_add_dev_tool(batch_cleanup_core_spec TEST
    SOURCES
        src/tools/batch_cleaner/BatchCleanupCoreSpec.cpp
        src/batch_cleaner/BatchCleanupCore.cpp
    LIBS miacode_chart Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_edit_spec TEST
    SOURCES src/tools/chart_transform/HandOrderEditSpec.cpp
    LIBS miacode_chart Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(hand_order_screen_split_spec TEST
    SOURCES src/tools/chart_transform/HandOrderScreenSplitSpec.cpp
    LIBS miacode_chart Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(native_slide_split_spec TEST
    SOURCES src/tools/chart_transform/NativeSlideSplitSpec.cpp
    LIBS miacode_chart miacode_analysis miacode_scene Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(basic_hand_order_spec TEST
    SOURCES src/tools/analysis/BasicHandOrderSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_local_context_spec TEST
    SOURCES src/tools/analysis/HandOrderLocalContextSpec.cpp
    LIBS miacode_analysis miacode_chart Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(basic_hand_order_layout_spec TEST
    SOURCES src/tools/chart_transform/BasicHandOrderLayoutSpec.cpp
    LIBS miacode_chart Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(hand_order_touch_judge_spec TEST
    SOURCES src/tools/analysis/HandOrderTouchJudgeSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_full_replay_spec TEST
    SOURCES src/tools/analysis/HandOrderFullReplaySpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(basic_hand_order_replay_spec TEST
    SOURCES src/tools/analysis/BasicHandOrderReplaySpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(wifi_hand_order_render_spec TEST
    SOURCES src/tools/video_export/WifiHandOrderRenderSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)
target_compile_definitions(wifi_hand_order_render_spec PRIVATE
    "MIACODE_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\""
)

miacode_add_dev_tool(slide_hand_order_render_spec TEST
    SOURCES src/tools/video_export/SlideHandOrderRenderSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)
target_compile_definitions(slide_hand_order_render_spec PRIVATE
    "MIACODE_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\""
)

miacode_add_dev_tool(hand_order_player_model_spec TEST
    SOURCES src/tools/analysis/HandOrderPlayerModelSpec.cpp
    LIBS Qt6::Core
    INCLUDES src
)

miacode_add_dev_tool(simulated_play_render_spec TEST
    SOURCES src/tools/video_export/SimulatedPlayRenderSpec.cpp
    LIBS miacode_export Qt6::Core Qt6::Gui
    INCLUDES src
)
target_compile_definitions(simulated_play_render_spec PRIVATE
    "MIACODE_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\""
)

miacode_add_dev_tool(hand_order_player_integration_spec TEST
    SOURCES src/tools/analysis/HandOrderPlayerIntegrationSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_ergonomics_spec TEST
    SOURCES src/tools/analysis/HandOrderErgonomicsSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(lance_pattern_spec TEST
    SOURCES src/tools/analysis/LancePatternSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_hold_candidates_spec TEST
    SOURCES src/tools/analysis/HandOrderHoldCandidatesSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_slide_timing_spec TEST
    SOURCES src/tools/analysis/HandOrderSlideTimingSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(pattern_hand_order_optimization_spec TEST
    SOURCES src/tools/analysis/PatternHandOrderOptimizationSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_pattern_rules_spec TEST
    SOURCES src/tools/analysis/HandOrderPatternRulesSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(pattern_candidate_render_spec TEST
    SOURCES src/tools/video_export/PatternCandidateRenderSpec.cpp
    LIBS miacode_export miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)
target_compile_definitions(pattern_candidate_render_spec PRIVATE
    "MIACODE_SOURCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}\"")

miacode_add_dev_tool(hand_order_progress_spec TEST
    SOURCES src/tools/analysis/HandOrderProgressSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_context_guard_spec TEST
    SOURCES src/tools/analysis/HandOrderContextGuardSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_verification_tail_spec TEST
    SOURCES src/tools/analysis/HandOrderVerificationTailSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_gesture_variants_spec TEST
    SOURCES src/tools/analysis/HandOrderGestureVariantsSpec.cpp
    LIBS miacode_analysis Qt6::Core Qt6::Gui
    INCLUDES src
)
miacode_add_dev_tool(chart_note_selection_spec TEST
    SOURCES src/tools/chart_selection/ChartNoteSelectionSpec.cpp
    LIBS miacode_timeline Qt6::Core Qt6::Gui
    INCLUDES src
)
miacode_add_dev_tool(preview_note_hit_test_spec TEST
    SOURCES src/tools/preview/PreviewNoteHitTestSpec.cpp
    LIBS miacode_scene Qt6::Core Qt6::Gui
    INCLUDES src
)
miacode_add_dev_tool(chart_tools_spec TEST
    SOURCES src/tools/chart_tools/ChartToolsSpec.cpp
    LIBS miacode_chart Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(hand_order_note_selection_spec TEST
    SOURCES src/tools/chart_selection/HandOrderNoteSelectionSpec.cpp
        src/app/ui/editor/EditorController.cpp src/app/ui/editor/EditorController.h
        src/app/ui/editor/NoteSelectionEdit.h
    LIBS miacode_analysis miacode_editor_core miacode_timeline Qt6::Core Qt6::Gui
    INCLUDES src
)

miacode_add_dev_tool(timeline_note_selection_interaction_spec TEST
    SOURCES src/tools/timeline/TimelineNoteSelectionInteractionSpec.cpp
    LIBS miacode_timeline_quick Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test
    INCLUDES src)
set_tests_properties(timeline_note_selection_interaction_spec PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
