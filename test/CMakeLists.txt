
set(TEST_LIST
        test_opendialog_win.c
        test_opendialog.c
        test_opendialog_cpp.cpp
        test_opendialogmultiple_win.c
        test_opendialogmultiple.c
        test_opendialogmultiple_cpp.cpp
        test_opendialogmultiple_enum.c
        test_pickfolder.c
        test_pickfolder_cpp.cpp
        test_savedialog.c
        test_savedialog_win.c
        test_async.c
        test_opendialog_async.c
        test_opendialogmultiple_async.c
        test_filemanagershowitem.c
        test_pickfolder_async.c)

foreach (TEST ${TEST_LIST})
  string(REPLACE "." "_" CLEAN_TEST_NAME ${TEST})
  add_executable(${CLEAN_TEST_NAME}
    ${TEST})
  target_link_libraries(${CLEAN_TEST_NAME}
    PUBLIC nfd)
endforeach()