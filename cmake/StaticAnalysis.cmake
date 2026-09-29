if(EXCHANGE_ENABLE_CLANG_TIDY)
    find_program(EXCHANGE_CLANG_TIDY NAMES clang-tidy-18 clang-tidy REQUIRED)
endif()

function(exchange_enable_static_analysis target)
    if(EXCHANGE_ENABLE_CLANG_TIDY)
        set_property(TARGET ${target} PROPERTY CXX_CLANG_TIDY "${EXCHANGE_CLANG_TIDY}")
    endif()
endfunction()
