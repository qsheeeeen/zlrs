# SPDX-License-Identifier: Apache-2.0
# Mandatory application protocol, shared by firmware and its Zephyr fixtures.
target_include_directories(app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
)
target_sources(app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/proto/crc.c"
  "${CMAKE_CURRENT_LIST_DIR}/proto/crypto.c"
  "${CMAKE_CURRENT_LIST_DIR}/proto/fhss.c"
  "${CMAKE_CURRENT_LIST_DIR}/proto/frame.c"
  "${CMAKE_CURRENT_LIST_DIR}/proto/mod.c"
  "${CMAKE_CURRENT_LIST_DIR}/link/link.c"
  "${CMAKE_CURRENT_LIST_DIR}/link/pairing.c"
  "${CMAKE_CURRENT_LIST_DIR}/link/session.c"
  "${CMAKE_CURRENT_LIST_DIR}/link/data.c"
  "${CMAKE_CURRENT_LIST_DIR}/link/packet.c"
  "${CMAKE_CURRENT_LIST_DIR}/region/region.c"
)
