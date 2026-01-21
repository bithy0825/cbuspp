add_library(cbuspp INTERFACE)
add_library(cbuspp::cbuspp ALIAS cbuspp)

target_include_directories(cbuspp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include/cbuspp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(cbuspp INTERFACE cxx_std_23)

include(GNUInstallDirs)

install(TARGETS cbuspp
    EXPORT cbusppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/cbuspp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
