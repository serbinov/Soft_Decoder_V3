cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# G-RTOS-8: RTOS task budget contract.
#
# Safety and the motor loop must run every 10 ms; the DCC receiver, sound
# engine and audio mixer must keep their documented priorities/stacks so none
# of them starves the safety loop. Some of those components are outside this
# build file's ownership, so validate their task-creation sites here at
# configure time. A drift fails the build immediately.
# ---------------------------------------------------------------------------
function(soft_decoder_require_task budget_path budget_needle budget_description)
    file(READ "${budget_path}" budget_content)
    string(FIND "${budget_content}" "${budget_needle}" budget_pos)
    if(budget_pos EQUAL -1)
        message(FATAL_ERROR
            "Task budget drift: ${budget_description} missing in ${budget_path}: ${budget_needle}")
    endif()
endfunction()

set(SD_MAIN "${CMAKE_CURRENT_LIST_DIR}/main/app_main.c")
set(SD_DCC "${CMAKE_CURRENT_LIST_DIR}/components/dcc/src/dcc.c")
set(SD_MOTOR "${CMAKE_CURRENT_LIST_DIR}/components/motor/src/motor.c")
set(SD_SOUND "${CMAKE_CURRENT_LIST_DIR}/components/sound/src/sound.c")
set(SD_AUDIO "${CMAKE_CURRENT_LIST_DIR}/components/audio/src/audio.c")
set(SD_PROV "${CMAKE_CURRENT_LIST_DIR}/components/provision/src/provision.c")

soft_decoder_require_task("${SD_MAIN}" "#define SAFETY_TASK_STACK       3072U" "safety stack")
soft_decoder_require_task("${SD_MAIN}" "#define SAFETY_TASK_PRIO        6U" "safety priority")
soft_decoder_require_task("${SD_MAIN}" "#define SAFETY_PERIOD_MS        10U" "safety 10 ms period")
soft_decoder_require_task("${SD_MAIN}" "#define PERSISTENCE_TASK_STACK  4096U" "persistence stack")
soft_decoder_require_task("${SD_MAIN}" "#define PERSISTENCE_TASK_PRIO   3U" "persistence priority")
soft_decoder_require_task("${SD_MAIN}" "xTaskCreate(safety_task, \"safety\", SAFETY_TASK_STACK" "safety task create")
soft_decoder_require_task("${SD_MAIN}" "xTaskCreate(persistence_task, \"settings\", PERSISTENCE_TASK_STACK" "persistence task create")

soft_decoder_require_task("${SD_DCC}" "#define DCC_TASK_STACK       8192U" "DCC receiver stack")
soft_decoder_require_task("${SD_DCC}" "#define DCC_TASK_PRIO        10U" "DCC receiver priority")
soft_decoder_require_task("${SD_DCC}" "#define DCC_ACK_TASK_STACK   2048U" "DCC ACK stack")
soft_decoder_require_task("${SD_DCC}" "#define DCC_ACK_TASK_PRIO    5U" "DCC ACK priority")

soft_decoder_require_task("${SD_MOTOR}" "#define MOTOR_TICK_MS 10" "motor 10 ms period")
soft_decoder_require_task("${SD_MOTOR}" "xTaskCreate(motor_task, \"motor\", 3072, NULL, 7, NULL)" "motor task create")

soft_decoder_require_task("${SD_SOUND}" "#define SOUND_TICK_MS          20" "sound 20 ms period")
soft_decoder_require_task("${SD_SOUND}" "xTaskCreatePinnedToCore(sound_task, \"sound\", 4096, NULL, 6, NULL, 1)" "sound task create")

soft_decoder_require_task("${SD_AUDIO}" "#define MIX_RATE  22050" "audio mixer rate")
soft_decoder_require_task("${SD_AUDIO}" "#define MIX_BLOCK 256" "audio mixer block")
soft_decoder_require_task("${SD_AUDIO}" "xTaskCreatePinnedToCore(mixer_task, \"audio_mix\", 4096, NULL, 7, NULL, 1)" "audio mixer task create")
# The mixer cadence is MIX_BLOCK / MIX_RATE = 256 / 22050 ~= 11.6 ms; pinning
# both macros above keeps that period (and its priority/stack) under test.

soft_decoder_require_task("${SD_PROV}" "#define PROV_LISTENER_STACK  8192U" "provision listener stack")
soft_decoder_require_task("${SD_PROV}" "#define PROV_LISTENER_PRIO   4U" "provision listener priority")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(soft_decoder_v3)
