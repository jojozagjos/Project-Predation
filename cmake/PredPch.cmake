# Precompiled headers: the large libraries and standard headers nearly every file includes, parsed once per target rather
# than once per file. A file still includes what it uses; this only makes that faster.
option(PRED_PCH "Precompiled headers (faster builds)" ON)

function(pred_precompile target)
    if(NOT PRED_PCH)
        return()
    endif()
    target_precompile_headers(${target} PRIVATE
        <algorithm>
        <array>
        <cmath>
        <cstdint>
        <functional>
        <memory>
        <string>
        <string_view>
        <unordered_map>
        <vector>
        <glm/glm.hpp>
        <glm/gtc/matrix_transform.hpp>
        <glm/gtc/quaternion.hpp>
        <spdlog/spdlog.h>
        <nlohmann/json.hpp>)
endfunction()
