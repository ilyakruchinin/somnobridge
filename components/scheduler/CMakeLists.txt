idf_component_register(
    SRCS "sched.c" "therapy.c" "phase.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer ezshare catalog settings netw drift journal
)
