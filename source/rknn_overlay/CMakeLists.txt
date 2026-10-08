cmake_minimum_required(VERSION 3.10)

project(rknn_yolov5_demo)

if (ENABLE_ASAN)
	message(STATUS "BUILD WITH ADDRESS SANITIZER")
	set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif ()

set(rknpu_yolov5_file rknpu2/yolov5.cc)
if (TARGET_SOC STREQUAL "rv1106" OR TARGET_SOC STREQUAL "rv1103")
    add_definitions(-DRV1106_1103)
    set(rknpu_yolov5_file rknpu2/yolov5_rv1106_1103.cc)
    #dma
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/allocator/dma)
elseif(TARGET_SOC STREQUAL "rk1808" OR TARGET_SOC STREQUAL "rv1109" OR TARGET_SOC STREQUAL "rv1126")
    add_definitions(-DRKNPU1)
    set(rknpu_yolov5_file rknpu1/yolov5.cc)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/ 3rdparty.out)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../utils/ utils.out)

set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

file(GLOB SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)


add_executable(${PROJECT_NAME}
    main.cc
    postprocess.cc
    ${rknpu_yolov5_file}
)

target_link_libraries(${PROJECT_NAME}
    imageutils
    fileutils
    imagedrawing    
    ${LIBRKNNRT}
    dl
)

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(${PROJECT_NAME}
    log
)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} Threads::Threads)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
)

install(TARGETS ${PROJECT_NAME} DESTINATION .)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/bus.jpg DESTINATION ./model)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/coco_80_labels_list.txt DESTINATION ./model)
file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../model/*.rknn")
install(FILES ${RKNN_FILES} DESTINATION model)

add_executable(rknn_yolov5_daemon
    yolo_daemon.cc
    postprocess.cc
    ${rknpu_yolov5_file}
)
target_link_libraries(rknn_yolov5_daemon
    imageutils fileutils imagedrawing ${LIBRKNNRT} dl
)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(rknn_yolov5_daemon Threads::Threads)
endif()
target_include_directories(rknn_yolov5_daemon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
)
install(TARGETS rknn_yolov5_daemon DESTINATION .)


find_package(PkgConfig QUIET)
pkg_check_modules(GST_APP QUIET gstreamer-1.0 gstreamer-app-1.0)
add_executable(imx415_yolo_roi_demo
    imx415-yolo-roi-demo.cc
    postprocess.cc
    ${rknpu_yolov5_file}
)
if(GST_APP_FOUND)
    target_compile_definitions(imx415_yolo_roi_demo PRIVATE HAVE_GSTREAMER=1)
    target_include_directories(imx415_yolo_roi_demo PRIVATE ${GST_APP_INCLUDE_DIRS})
    target_link_libraries(imx415_yolo_roi_demo ${GST_APP_LIBRARIES})
else()
    message(STATUS "GStreamer development package not found; using runtime dlopen display backend")
endif()
target_include_directories(imx415_yolo_roi_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
)
target_link_libraries(imx415_yolo_roi_demo
    imageutils fileutils imagedrawing ${LIBRKNNRT} dl Threads::Threads
)
install(TARGETS imx415_yolo_roi_demo DESTINATION .)

add_executable(imx415_dual_person_demo
    imx415-dual-person-demo.cc
    postprocess.cc
    ${rknpu_yolov5_file}
)
target_include_directories(imx415_dual_person_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
)
target_link_libraries(imx415_dual_person_demo
    imageutils fileutils imagedrawing ${LIBRKNNRT} dl Threads::Threads
)
install(TARGETS imx415_dual_person_demo DESTINATION .)
