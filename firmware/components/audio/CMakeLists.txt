idf_component_register(SRCS "src/audio.c" "src/ima_adpcm.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_driver_i2s pinmap storage)

# The mixer/resampler is a per-sample hot loop; size optimisation (-Os) is
# noticeably slower there. Flash has ample headroom, so optimise this component
# for speed and leave the rest of the image size-optimised.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
