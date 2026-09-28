# CMake generated Testfile for 
# Source directory: D:/Project/MrProper
# Build directory: D:/Project/MrProper/build/Debug
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if("${CTEST_CONFIGURATION_TYPE}" MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[core_unit]=] "D:/Project/MrProper/build/Debug/Debug/mrproper_unit_tests.exe")
  set_tests_properties([=[core_unit]=] PROPERTIES  _BACKTRACE_TRIPLES "D:/Project/MrProper/CMakeLists.txt;26;add_test;D:/Project/MrProper/CMakeLists.txt;0;")
elseif("${CTEST_CONFIGURATION_TYPE}" MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[core_unit]=] "D:/Project/MrProper/build/Debug/Release/mrproper_unit_tests.exe")
  set_tests_properties([=[core_unit]=] PROPERTIES  _BACKTRACE_TRIPLES "D:/Project/MrProper/CMakeLists.txt;26;add_test;D:/Project/MrProper/CMakeLists.txt;0;")
elseif("${CTEST_CONFIGURATION_TYPE}" MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[core_unit]=] "D:/Project/MrProper/build/Debug/MinSizeRel/mrproper_unit_tests.exe")
  set_tests_properties([=[core_unit]=] PROPERTIES  _BACKTRACE_TRIPLES "D:/Project/MrProper/CMakeLists.txt;26;add_test;D:/Project/MrProper/CMakeLists.txt;0;")
elseif("${CTEST_CONFIGURATION_TYPE}" MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[core_unit]=] "D:/Project/MrProper/build/Debug/RelWithDebInfo/mrproper_unit_tests.exe")
  set_tests_properties([=[core_unit]=] PROPERTIES  _BACKTRACE_TRIPLES "D:/Project/MrProper/CMakeLists.txt;26;add_test;D:/Project/MrProper/CMakeLists.txt;0;")
else()
  add_test([=[core_unit]=] NOT_AVAILABLE)
endif()
