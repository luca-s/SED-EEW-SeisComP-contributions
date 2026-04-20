SET(TESTS
	npy.cpp
	table.cpp
)

FOREACH(testSrc ${TESTS})
	GET_FILENAME_COMPONENT(testName ${testSrc} NAME_WE)
	SET(testName test_sedeew_ogf_${testName})
	ADD_EXECUTABLE(${testName} ${testSrc} ../association.cpp ../npy.cpp)
	SC_LINK_LIBRARIES_INTERNAL(${testName} unittest core)

	ADD_TEST(
		NAME ${testName}
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		COMMAND ${testName}
	)
ENDFOREACH()
