# Falcon Examples

# Simple download example
add_executable(simple_download simple_download.cpp)

# Link against Falcon library
target_link_libraries(simple_download PRIVATE Falcon::falcon)

# Set C++ standard
target_compile_features(simple_download PRIVATE cxx_std_17)

# Compiler options
if(MSVC)
    target_compile_options(simple_download PRIVATE /W4)
else()
    target_compile_options(simple_download PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Install examples
install(TARGETS simple_download
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT examples
)
