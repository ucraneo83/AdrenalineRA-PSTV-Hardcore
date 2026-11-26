set(BTCNF_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/btcnf.py")

set(BTCNFS
  pspbtknf:pspbtcnf_inferno
  pspbtlnf:pspbtcnf_march33
  pspbtnnf:pspbtcnf_me
  pspbtjnf:pspbtcnf_normal
  pspbtmnf:pspbtcnf_np9660
  pspbtrnf:pspbtcnf_recovery
)

foreach(BTCNF IN LISTS BTCNFS)
  string(REPLACE ":" ";" kv "${BTCNF}")
  list(GET kv 0 BTCNF_NAME)
  list(GET kv 1 FOLDER)

  set(BTCNF_SOURCE "${CMAKE_CURRENT_LIST_DIR}/${FOLDER}/pspbtcnf.txt")
  list(APPEND BTCNF_BIN_FILES "${BTCNF_NAME}.bin")

  add_custom_command(
    OUTPUT ${BTCNF_NAME}.bin
    DEPENDS "${BTCNF_SOURCE}" "${BTCNF_SCRIPT}"
    COMMAND ${CMAKE_COMMAND} -E copy "${BTCNF_SOURCE}" "${BTCNF_NAME}.txt"
    COMMAND python3 "${BTCNF_SCRIPT}" build "${BTCNF_NAME}.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${BTCNF_NAME}.bin" "${FLASH0_DIR}/kd/${BTCNF_NAME}.bin"
    VERBATIM
  )
endforeach()

add_custom_target(build_btcnfs ALL
  DEPENDS ${BTCNF_BIN_FILES}
)
