add_library(VkLayer_fossilize SHARED
	device.cpp
	device.hpp
	instance.cpp
	instance.hpp
	dispatch.cpp
	dispatch_helper.hpp
	dispatch_helper.cpp)

target_include_directories(VkLayer_fossilize PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(VkLayer_fossilize PRIVATE ${FOSSILIZE_CXX_FLAGS})
set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")

option(FOSSILIZE_LAYER_CAPTURE_SIGSEGV "Support capturing segfaults." ON)
if (FOSSILIZE_LAYER_CAPTURE_SIGSEGV)
	target_compile_definitions(VkLayer_fossilize PRIVATE FOSSILIZE_LAYER_CAPTURE_SIGSEGV)
endif()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/layer)

if (WIN32)
	add_custom_command(TARGET VkLayer_fossilize POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_fossilize_win32.json ${CMAKE_BINARY_DIR}/layer/VkLayer_fossilize.json)
	set_target_properties(VkLayer_fossilize PROPERTIES PREFIX "" LIBRARY_OUTPUT_NAME VkLayer_fossilize)
	if (CMAKE_COMPILER_IS_GNUCXX)
		if (${CMAKE_BUILD_TYPE} MATCHES "Release")
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -Wl,--no-undefined -static -s")
		else()
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -Wl,--no-undefined -static")
		endif()
	endif()
else()
	if (CMAKE_COMPILER_IS_GNUCXX OR (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang"))
		target_compile_options(VkLayer_fossilize PRIVATE -fvisibility=hidden)
	endif()

	if (NOT ANDROID)
		if (APPLE)
			add_custom_command(TARGET VkLayer_fossilize POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_fossilize_dylib.json ${CMAKE_BINARY_DIR}/layer/VkLayer_fossilize.json)
		else()
			add_custom_command(TARGET VkLayer_fossilize POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_fossilize.json ${CMAKE_BINARY_DIR}/layer/VkLayer_fossilize.json)
		endif()
	endif()
	if (${CMAKE_BUILD_TYPE} MATCHES "Release")
		if (APPLE)
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -s")
		else()
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -Wl,--no-undefined -s")
		endif()
	else()
		if (NOT APPLE)
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -Wl,--no-undefined")
		endif()
	endif()
endif()

set_target_properties(VkLayer_fossilize PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/layer")
set_target_properties(VkLayer_fossilize PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/layer")
set_target_properties(VkLayer_fossilize PROPERTIES RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/layer")
set_target_properties(VkLayer_fossilize PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/layer")
target_link_libraries(VkLayer_fossilize fossilize)

if (ANDROID)
	target_link_libraries(VkLayer_fossilize log)
endif()
