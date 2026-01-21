if(BUILD_TESTS)
    enable_testing()

    add_executable(cbuspp_test test.cpp)
    target_link_libraries(cbuspp_test PRIVATE cbuspp::cbuspp)
    
    add_executable(cbuspp_comprehensive_test test_comprehensive.cpp)
    target_link_libraries(cbuspp_comprehensive_test PRIVATE cbuspp::cbuspp)

    add_test(NAME BasicTest COMMAND cbuspp_test)
    add_test(NAME ComprehensiveTest COMMAND cbuspp_comprehensive_test)
endif()
