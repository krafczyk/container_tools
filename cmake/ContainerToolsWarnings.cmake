function(container_tools_enable_warnings target)
  target_compile_features(${target} PRIVATE c_std_11)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wshadow -Wformat=2 -Wmissing-prototypes
  )
endfunction()
