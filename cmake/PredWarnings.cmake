# Applies the project's warning policy to a target.
function(pred_set_warnings target)
    if(MSVC)
        # /W4 with a few noisy warnings disabled:
        #   4100 unreferenced formal parameter (interfaces with unused params)
        #   4201 nameless struct/union (glm)
        #   4324 structure padded due to alignment specifier
        target_compile_options(${target} PRIVATE /W4 /wd4100 /wd4201 /wd4324)
        if(PRED_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
        if(PRED_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
