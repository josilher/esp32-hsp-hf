idf_component_register(
        SRCS "sco_demo_util.c" "hfp_hf_demo.c" "main.c"
        INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"
        REQUIRES btstack btstack_config host_link)
