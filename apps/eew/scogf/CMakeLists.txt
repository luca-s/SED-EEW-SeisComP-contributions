SET(EEWOGF_TARGET scogf)
SET(EEWOGF_SOURCES
	main.cpp
	app.cpp
	association.cpp
	npy.cpp
	prediction.cpp
)

INCLUDE_DIRECTORIES(${SC3_PACKAGE_SOURCE_DIR}/../sed/libs)
INCLUDE_DIRECTORIES(${SC3_PACKAGE_BINARY_DIR}/../sed/libs)

SC_ADD_EXECUTABLE(EEWOGF ${EEWOGF_TARGET})
SC_LINK_LIBRARIES_INTERNAL(${EEWOGF_TARGET} client datamodel_vs)

IF(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../trunk/apps/templates/initd.py")
	SC_INSTALL_INIT(${EEWOGF_TARGET} ../../../../trunk/apps/templates/initd.py)
ELSE()
	SC_INSTALL_INIT(${EEWOGF_TARGET} ../../../../../base/common/apps/templates/initd.py)
ENDIF()

FILE(GLOB descs "${CMAKE_CURRENT_SOURCE_DIR}/descriptions/*.xml")
INSTALL(FILES ${descs} DESTINATION ${SC3_PACKAGE_APP_DESC_DIR})

IF(SC_GLOBAL_UNITTESTS)
	SUBDIRS(tests)
ENDIF()
