# Face ID of the editor (docs/editor/faceid.md): the camera, the detection and the recognition of faces on ONNX Runtime. MYENGINE_FACEID is ON by itself when
# tools\faceid\fetch_models.ps1 has put ONNX Runtime into external/onnxruntime; without it the module of the models is a stub and the editor starts as before.
# (the models themselves, assets/faceid/models, are looked for at run time: without them Face ID says so and the editor starts.) The shipping game has no Face ID.
set(_faceid_ort "${CMAKE_SOURCE_DIR}/external/onnxruntime")
set(_faceid_default OFF)
if(NOT MYENGINE_SHIPPING AND EXISTS "${_faceid_ort}/lib/onnxruntime.lib" AND EXISTS "${_faceid_ort}/include/onnxruntime_cxx_api.h")
  set(_faceid_default ON)
endif()
option(MYENGINE_FACEID "Build Face ID with ONNX Runtime (tools/faceid/fetch_models.ps1 downloads it into external/onnxruntime)" ${_faceid_default})
if(MYENGINE_FACEID AND (MYENGINE_SHIPPING OR NOT EXISTS "${_faceid_ort}/lib/onnxruntime.lib"))
  message(WARNING "MYENGINE_FACEID is ON, but ONNX Runtime is not in external/onnxruntime (tools/faceid/fetch_models.ps1) or this is the shipping build: Face ID is off")
  set(MYENGINE_FACEID OFF CACHE BOOL "" FORCE)
endif()
set(MYENGINE_ONNXRUNTIME_DIR "${_faceid_ort}")
