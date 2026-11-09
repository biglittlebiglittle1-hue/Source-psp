cmake_minimum_required(VERSION 3.5)
project(SourceLike C)

add_executable(sourcelike main.c)

target_link_libraries(sourcelike PRIVATE
    pspdebug pspdisplay pspge pspgu pspgum pspctrl psprtc m)

create_pbp_file(
    TARGET sourcelike
    TITLE "Source-Like Bhop"
    ICON_PATH NULL
    BACKGROUND_PATH NULL
    PREVIEW_PATH NULL
)
